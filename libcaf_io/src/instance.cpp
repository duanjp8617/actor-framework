/******************************************************************************
 *                       ____    _    _____                                   *
 *                      / ___|  / \  |  ___|    C++                           *
 *                     | |     / _ \ | |_       Actor                         *
 *                     | |___ / ___ \|  _|      Framework                     *
 *                      \____/_/   \_|_|                                      *
 *                                                                            *
 * Copyright (C) 2011 - 2015                                                  *
 * Dominik Charousset <dominik.charousset (at) haw-hamburg.de>                *
 *                                                                            *
 * Distributed under the terms and conditions of the BSD 3-Clause License or  *
 * (at your option) under the terms and conditions of the Boost Software      *
 * License 1.0. See accompanying files LICENSE and LICENSE_ALTERNATIVE.       *
 *                                                                            *
 * If you did not receive a copy of the license files, see                    *
 * http://opensource.org/licenses/BSD-3-Clause and                            *
 * http://www.boost.org/LICENSE_1_0.txt.                                      *
 ******************************************************************************/

#include "caf/io/basp/instance.hpp"

#include "caf/binary_serializer.hpp"
#include "caf/binary_deserializer.hpp"

#include "caf/io/basp/version.hpp"
#include "caf/io/basp/error_code.hpp"

namespace caf {
namespace io {
namespace basp {

instance::callee::callee(actor_system& sys, proxy_registry::backend& backend)
    : namespace_(sys, backend) {
  // nop
}

instance::callee::~callee() {
  // nop
}

instance::instance(abstract_broker* parent, callee& lstnr)
    : tbl_(parent),
      this_node_(parent->system().node()),
      callee_(lstnr) {
  CAF_ASSERT(this_node_ != invalid_node_id);
}

connection_state instance::handle(execution_unit* ctx,
                                  new_data_msg& dm, header& hdr,
                                  bool is_payload) {
  CAF_LOG_TRACE("");
  // function object providing cleanup code on errors
  auto err = [&]() -> connection_state {
    auto cb = make_callback([&](const node_id& nid){
      callee_.purge_state(nid);
    });
    tbl_.erase_direct(dm.handle, cb);
    return close_connection;
  };
  std::vector<char>* payload = nullptr;
  if (is_payload) {
    payload = &dm.buf;
    if (payload->size() != hdr.payload_len) {
      CAF_LOG_WARNING("received invalid payload");
      return err();
    }
  } else {
    binary_deserializer bd{ctx, dm.buf.data(), dm.buf.size()};
    bd >> hdr;
    CAF_LOG_DEBUG(CAF_ARG(hdr));
    if (! valid(hdr)) {
      CAF_LOG_WARNING("received invalid header:" << CAF_ARG(hdr.operation));
      return err();
    }
    if (hdr.payload_len > 0)
      return await_payload;
  }
  // needs forwarding?
  if (! is_handshake(hdr)
      && ! is_heartbeat(hdr)
      && hdr.dest_node != this_node_) {
    auto path = lookup(hdr.dest_node);
    if (path) {
      binary_serializer bs{ctx, std::back_inserter(path->wr_buf)};
      bs << hdr;
      if (payload)
        bs.apply_raw(payload->size(), payload->data());
      tbl_.flush(*path);
      notify<hook::message_forwarded>(hdr, payload);
    } else {
      CAF_LOG_INFO("cannot forward message, no route to destination");
      if (hdr.source_node != this_node_) {
        auto reverse_path = lookup(hdr.source_node);
        if (! reverse_path) {
          CAF_LOG_WARNING("cannot send error message: no route to source");
        } else {
          write_dispatch_error(ctx,
                               reverse_path->wr_buf,
                               this_node_,
                               hdr.source_node,
                               error_code::no_route_to_destination,
                               hdr,
                               payload);
        }
      } else {
        CAF_LOG_WARNING("lost packet with probably spoofed source");
      }
      notify<hook::message_forwarding_failed>(hdr, payload);
    }
    return await_header;
  }
  // function object for checking payload validity
  auto payload_valid = [&]() -> bool {
    return payload != nullptr && payload->size() == hdr.payload_len;
  };
  // handle message to ourselves
  switch (hdr.operation) {
    case message_type::server_handshake: {
      actor_id aid = invalid_actor_id;
      std::set<std::string> sigs;
      if (payload_valid()) {
        binary_deserializer bd{ctx, payload->data(), payload->size()};
        bd >> aid >> sigs;
      }
      // close self connection after handshake is done
      if (hdr.source_node == this_node_) {
        CAF_LOG_INFO("close connection to self immediately");
        callee_.finalize_handshake(hdr.source_node, aid, sigs);
        return err();
      }
      // close this connection if we already have a direct connection
      if (tbl_.lookup_direct(hdr.source_node) != invalid_connection_handle) {
        CAF_LOG_INFO("close connection since we already have a "
                     "direct connection: " << CAF_ARG(hdr.source_node));
        callee_.finalize_handshake(hdr.source_node, aid, sigs);
        return err();
      }
      // add direct route to this node and remove any indirect entry
      CAF_LOG_INFO("new direct connection:" << CAF_ARG(hdr.source_node));
      tbl_.add_direct(dm.handle, hdr.source_node);
      auto was_indirect = tbl_.erase_indirect(hdr.source_node);
      // write handshake as client in response
      auto path = tbl_.lookup(hdr.source_node);
      if (!path) {
        CAF_LOG_ERROR("no route to host after server handshake");
        return err();
      }
      write_client_handshake(ctx, path->wr_buf, hdr.source_node);
      callee_.learned_new_node_directly(hdr.source_node, was_indirect);
      callee_.finalize_handshake(hdr.source_node, aid, sigs);
      flush(*path);
      break;
    }
    case message_type::client_handshake: {
      if (tbl_.lookup_direct(hdr.source_node) != invalid_connection_handle) {
        CAF_LOG_INFO("received second client handshake:"
                     << CAF_ARG(hdr.source_node));
        break;
      }
      // add direct route to this node and remove any indirect entry
      CAF_LOG_INFO("new direct connection:" << CAF_ARG(hdr.source_node));
      tbl_.add_direct(dm.handle, hdr.source_node);
      auto was_indirect = tbl_.erase_indirect(hdr.source_node);
      callee_.learned_new_node_directly(hdr.source_node, was_indirect);
      break;
    }
    case message_type::dispatch_message: {
      if (! payload_valid())
        return err();
      // in case the sender of this message was received via a third node,
      // we assume that that node to offers a route to the original source
      auto last_hop = tbl_.lookup_direct(dm.handle);
      if (hdr.source_node != invalid_node_id
          && hdr.source_node != this_node_
          && last_hop != hdr.source_node
          && tbl_.lookup_direct(hdr.source_node) == invalid_connection_handle
          && tbl_.add_indirect(last_hop, hdr.source_node))
        callee_.learned_new_node_indirectly(hdr.source_node);
      binary_deserializer bd{ctx, payload->data(), payload->size()};
      std::vector<actor_addr> forwarding_stack;
      message msg;
      bd >> forwarding_stack >> msg;
      callee_.deliver(hdr.source_node, hdr.source_actor,
                      hdr.dest_node, hdr.dest_actor,
                      message_id::from_integer_value(hdr.operation_data),
                      forwarding_stack, msg);
      break;
    }
    case message_type::announce_proxy_instance:
      callee_.proxy_announced(hdr.source_node, hdr.dest_actor);
      break;
    case message_type::kill_proxy_instance:
      callee_.kill_proxy(hdr.source_node, hdr.source_actor,
                         static_cast<exit_reason>(hdr.operation_data));
      break;
    case message_type::heartbeat: {
      CAF_LOG_TRACE("received heartbeat: " << CAF_ARG(hdr.source_node));
      callee_.handle_heartbeat(hdr.source_node);
      break;
    }
    default:
      CAF_LOG_ERROR("invalid operation");
      return err();
  }
  return await_header;
}

void instance::handle_heartbeat(execution_unit* ctx) {
  for (auto& kvp: tbl_.direct_by_hdl_) {
    CAF_LOG_TRACE(CAF_ARG(kvp.first) << CAF_ARG(kvp.second));
    write_heartbeat(ctx, tbl_.parent_->wr_buf(kvp.first), kvp.second);
    tbl_.parent_->flush(kvp.first);
  }
}

void instance::handle_node_shutdown(const node_id& affected_node) {
  CAF_LOG_TRACE(CAF_ARG(affected_node));
  if (affected_node == invalid_node_id)
    return;
  CAF_LOG_INFO("lost direct connection:" << CAF_ARG(affected_node));
  auto cb = make_callback([&](const node_id& nid){
    callee_.purge_state(nid);
  });
  tbl_.erase(affected_node, cb);
}

maybe<routing_table::route> instance::lookup(const node_id& target) {
  return tbl_.lookup(target);
}

void instance::flush(const routing_table::route& path) {
  tbl_.flush(path);
}

void instance::write(execution_unit* ctx, const routing_table::route& r,
                     header& hdr, payload_writer* writer) {
  CAF_ASSERT(hdr.payload_len == 0 || writer != nullptr);
  write(ctx, r.wr_buf, hdr, writer);
  tbl_.flush(r);
}

void instance::add_published_actor(uint16_t port,
                                   actor_addr published_actor,
                                   std::set<std::string> published_interface) {
  using std::swap;
  auto& entry = published_actors_[port];
  swap(entry.first, published_actor);
  swap(entry.second, published_interface);
  notify<hook::actor_published>(entry.first, entry.second, port);
}

size_t instance::remove_published_actor(uint16_t port,
                                        removed_published_actor* cb) {
  auto i = published_actors_.find(port);
  if (i == published_actors_.end())
    return 0;
  if (cb)
    (*cb)(i->second.first, i->first);
  published_actors_.erase(i);
  return 1;
}

size_t instance::remove_published_actor(const actor_addr& whom, uint16_t port,
                                        removed_published_actor* cb) {
  size_t result = 0;
  if (port != 0) {
    auto i = published_actors_.find(port);
    if (i != published_actors_.end() && i->second.first == whom) {
      if (cb)
        (*cb)(whom, port);
      published_actors_.erase(i);
      result = 1;
    }
  } else {
    auto i = published_actors_.begin();
    while (i != published_actors_.end()) {
      if (i->second.first == whom) {
        if (cb)
          (*cb)(whom, i->first);
        i = published_actors_.erase(i);
        ++result;
      } else {
        ++i;
      }
    }
  }
  return result;
}

bool instance::dispatch(execution_unit* ctx, const actor_addr& sender,
                        const std::vector<actor_addr>& forwarding_stack,
                        const actor_addr& receiver, message_id mid,
                        const message& msg) {
  CAF_LOG_TRACE("");
  CAF_ASSERT(system().node() != receiver.node());
  auto path = lookup(receiver->node());
  if (! path) {
    notify<hook::message_sending_failed>(sender, receiver, mid, msg);
    return false;
  }
  auto writer = make_callback([&](serializer& sink) {
    sink << forwarding_stack << msg;
  });
  header hdr{message_type::dispatch_message, 0, mid.integer_value(),
             sender ? sender->node() : this_node(), receiver->node(),
             sender ? sender->id() : invalid_actor_id, receiver->id()};
  write(ctx, path->wr_buf, hdr, &writer);
  flush(*path);
  notify<hook::message_sent>(sender, path->next_hop, receiver, mid, msg);
  return true;
}

void instance::write(execution_unit* ctx,
                     buffer_type& buf,
                     message_type operation,
                     uint32_t* payload_len,
                     uint64_t operation_data,
                     const node_id& source_node,
                     const node_id& dest_node,
                     actor_id source_actor,
                     actor_id dest_actor,
                     payload_writer* pw) {
  if (! pw) {
    uint32_t zero = 0;
    binary_serializer bs{ctx, std::back_inserter(buf)};
    bs << source_node
       << dest_node
       << source_actor
       << dest_actor
       << zero
       << operation
       << operation_data;
  } else {
    // reserve space in the buffer to write the payload later on
    auto wr_pos = static_cast<ptrdiff_t>(buf.size());
    char placeholder[basp::header_size];
    buf.insert(buf.end(), std::begin(placeholder), std::end(placeholder));
    auto pl_pos = buf.size();
    { // lifetime scope of first serializer (write payload)
      binary_serializer bs{ctx, std::back_inserter(buf)};
      (*pw)(bs);
    }
    // write broker message to the reserved space
    binary_serializer bs2{ctx, buf.begin() + wr_pos};
    auto plen = static_cast<uint32_t>(buf.size() - pl_pos);
    bs2 << source_node
        << dest_node
        << source_actor
        << dest_actor
        << plen
        << operation
        << operation_data;
    if (payload_len)
      *payload_len = plen;
  }
}

void instance::write(execution_unit* ctx, buffer_type& buf,
                     header& hdr, payload_writer* pw) {
  write(ctx, buf, hdr.operation, &hdr.payload_len, hdr.operation_data,
        hdr.source_node, hdr.dest_node, hdr.source_actor, hdr.dest_actor, pw);
}

void instance::write_server_handshake(execution_unit* ctx,
                                      buffer_type& out_buf,
                                      maybe<uint16_t> port) {
  using namespace detail;
  published_actor* pa = nullptr;
  if (port) {
    auto i = published_actors_.find(*port);
    if (i != published_actors_.end())
      pa = &i->second;
  }
  auto writer = make_callback([&](serializer& sink) {
    if (pa) {
      auto i = pa->first.id();
      sink << i << pa->second;
    }
  });
  header hdr{message_type::server_handshake, 0, version,
             this_node_, invalid_node_id,
             pa ? pa->first.id() : invalid_actor_id, invalid_actor_id};
  write(ctx, out_buf, hdr, &writer);
}

void instance::write_client_handshake(execution_unit* ctx,
                                      buffer_type& buf,
                                      const node_id& remote_side) {
  write(ctx, buf, message_type::client_handshake, nullptr, 0,
        this_node_, remote_side, invalid_actor_id, invalid_actor_id);
}

void instance::write_dispatch_error(execution_unit* ctx,
                                    buffer_type& buf,
                                    const node_id& source_node,
                                    const node_id& dest_node,
                                    error_code ec,
                                    const header& original_hdr,
                                    buffer_type* payload) {
  auto writer = make_callback([&](serializer& sink) {
    sink << original_hdr;
    if (payload)
      sink.apply_raw(payload->size(), payload->data());
  });
  header hdr{message_type::kill_proxy_instance, 0,
             static_cast<uint64_t>(ec),
             source_node, dest_node,
             invalid_actor_id, invalid_actor_id};
  write(ctx, buf, hdr, &writer);
}

void instance::write_kill_proxy_instance(execution_unit* ctx,
                                         buffer_type& buf,
                                         const node_id& dest_node,
                                         actor_id aid,
                                         exit_reason rsn) {
  header hdr{message_type::kill_proxy_instance, 0, static_cast<uint32_t>(rsn),
             this_node_, dest_node, aid, invalid_actor_id};
  write(ctx, buf, hdr);
}

void instance::write_heartbeat(execution_unit* ctx,
                               buffer_type& buf,
                               const node_id& remote_side) {
  write(ctx, buf, message_type::heartbeat, nullptr, 0,
        this_node_, remote_side, invalid_actor_id, invalid_actor_id);
}

} // namespace basp
} // namespace io
} // namespace caf