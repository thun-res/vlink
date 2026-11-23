/*
 * Copyright (C) 2026 by Thun Lu. All rights reserved.
 * Author: Thun Lu <thun.lu@zohomail.cn>
 * Repo:   https://github.com/thun-res/vlink
 *  _    __   __      _           __
 * | |  / /  / /     (_) ____    / /__
 * | | / /  / /     / / / __ \  / //_/
 * | |/ /  / /___  / / / / / / / ,<
 * |___/  /_____/ /_/ /_/ /_/ /_/|_|
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "./intra_factory.h"

#include <algorithm>
#include <type_traits>
#include <utility>

#include "./base/memory_resource.h"
#include "./base/utils.h"

namespace vlink {

constexpr size_t kMaxTaskSize = 10000U;

// IntraPipeline
size_t IntraPipeline::get_max_task_count() const { return kMaxTaskSize; }

// IntraFactory
IntraPipeline& IntraFactory::get_pipeline(int32_t pipeline) {
  std::lock_guard lock(pipeline_mtx_);

  auto [iter, inserted] = pipeline_map_.try_emplace(pipeline);

  if (inserted) {
    iter->second.set_name("INTRA-PIPELINE-" + std::to_string(pipeline));
    iter->second.async_run();
  }

  return iter->second;
}

IntraFactory::IntraFactory() {
  Bytes::init_memory_pool();

  if VUNLIKELY (IntraTransport::get_thread_count() != 1) {
    VLOG_W("IntraFactory: Intra does not support setting thread count.");
  }
}

IntraFactory::~IntraFactory() {
#ifdef _WIN32
  if (Utils::is_terminating()) {
    return;
  }
#endif

  for (auto& [num, pipeline] : pipeline_map_) {
    pipeline.quit(true);
    pipeline.wait_for_quit();
  }

  pipeline_map_.clear();
}

// IntraNode
IntraNode::IntraNode(const IntraID& id) {
  const auto& [impl_type, address, pipeline, type, channel] = id;

  static auto& factory = IntraFactory::get();

  if (type == IntraType::kQueue) {
    pipeline_ = &factory.get_pipeline(pipeline);
  }
}

IntraNode::~IntraNode() = default;

std::any IntraNode::get_native_handle() const { return this; }

template <typename DataT>
void IntraNode::deliver_data(MessageLoop* target_loop, uint32_t channel, const DataT& data) {
  (void)channel;

  auto deliver = [this, target_loop, &data](NodeImpl* impl, const auto& callback) {
    auto* loop = impl->get_message_loop();

    if (!loop) {
      loop = pipeline_;
    }

    if VUNLIKELY (impl->has_suspend.load(std::memory_order_relaxed) || loop != target_loop) {
      return;
    }

    callback(data);
  };

  if constexpr (std::is_same_v<DataT, Bytes>) {
    traverse_msg_callback(deliver);
  } else {
    traverse_intra_msg_callback(deliver);
  }
}

template <typename DataT>
bool IntraNode::publish_data(IntraType type, uint32_t channel, DataT&& data) {
  struct LoopRoute final {
    NodeImpl::LoopGatePtr gate;
    MessageLoop* loop{nullptr};
  };

  LoopRoute first_route{};
  std::vector<LoopRoute> other_routes;
  bool delivered = false;

  auto route = [this, &data, &first_route, &other_routes, &delivered](NodeImpl* impl, const auto& callback) {
    if VUNLIKELY (impl->has_suspend.load(std::memory_order_relaxed)) {
      return;
    }

    auto* attached_loop = impl->get_message_loop();
    auto* loop = attached_loop ? attached_loop : pipeline_;

    if (!loop) {
      callback(data);
      delivered = true;
    } else if (!first_route.loop) {
      first_route = {attached_loop ? impl->get_loop_gate() : nullptr, loop};
    } else if (loop != first_route.loop &&
               std::find_if(other_routes.begin(), other_routes.end(),
                            [loop](const LoopRoute& item) { return item.loop == loop; }) == other_routes.end()) {
      other_routes.push_back({attached_loop ? impl->get_loop_gate() : nullptr, loop});
    }
  };

  if constexpr (std::is_same_v<std::decay_t<DataT>, Bytes>) {
    traverse_msg_callback(route);
  } else {
    traverse_intra_msg_callback(route);
  }

  if (!first_route.loop) {
    if (delivered || type != IntraType::kQueue) {
      return delivered;
    }

    first_route = {nullptr, pipeline_};
  }

  if constexpr (std::is_same_v<DataT, Bytes>) {
    if VUNLIKELY (!data.is_owner()) {
      Bytes snapshot(data);

      if VUNLIKELY (snapshot.real_size() != data.real_size()) {
        return false;
      }

      data = std::move(snapshot);
    }
  }

  auto post_route = [this](const LoopRoute& target, MoveFunction<void()>&& task) {
    if (!target.gate) {
      return target.loop->post_task(std::move(task));
    }

    const int post_status = NodeImpl::post_to_loop_gate_status(target.gate, target.loop, std::move(task));

    if VLIKELY (post_status > 0) {
      return true;
    }

    if (post_status < 0) {
      return false;
    }

    std::vector<NodeImpl::LoopGatePtr> fallback_gates;

    auto collect = [&fallback_gates, &target](NodeImpl* impl, const auto&) {
      if (impl->get_message_loop() == target.loop) {
        auto gate = impl->get_loop_gate();

        if (gate != target.gate) {
          fallback_gates.emplace_back(std::move(gate));
        }
      }
    };

    if constexpr (std::is_same_v<std::decay_t<DataT>, Bytes>) {
      traverse_msg_callback(collect);
    } else {
      traverse_intra_msg_callback(collect);
    }

    for (const auto& gate : fallback_gates) {
      const int fallback_status = NodeImpl::post_to_loop_gate_status(gate, target.loop, std::move(task));

      if (fallback_status > 0) {
        return true;
      }

      if (fallback_status < 0) {
        return false;
      }
    }

    return false;
  };

  auto weak_self = weak_from_this();

  if VLIKELY (other_routes.empty()) {
    return post_route(first_route, [weak_self = std::move(weak_self), loop = first_route.loop, channel,
                                    data = std::forward<DataT>(data)]() {
      auto self = weak_self.lock();

      if VUNLIKELY (!self) {
        return;
      }

      self->deliver_data(loop, channel, data);
    });
  }

  if constexpr (std::is_same_v<std::decay_t<DataT>, Bytes>) {
    auto shared_data = MemoryResource::make_shared<Bytes>(std::forward<DataT>(data));
    auto post = [&weak_self, channel, &shared_data, &post_route](const LoopRoute& target) {
      return post_route(target, [weak_self, loop = target.loop, channel, shared_data]() {
        auto self = weak_self.lock();

        if VUNLIKELY (!self) {
          return;
        }

        self->deliver_data(loop, channel, *shared_data);
      });
    };

    bool result = post(first_route);

    for (const auto& target : other_routes) {
      result = post(target) && result;
    }

    return result;
  } else {
    auto post = [&weak_self, channel, &data, &post_route](const LoopRoute& target) {
      return post_route(target, [weak_self, loop = target.loop, channel, data = data]() {
        auto self = weak_self.lock();

        if VUNLIKELY (!self) {
          return;
        }

        self->deliver_data(loop, channel, data);
      });
    };

    bool result = post(first_route);

    for (const auto& target : other_routes) {
      result = post(target) && result;
    }

    return result;
  }
}

bool IntraNode::publish(IntraType type, uint32_t channel, const Bytes& msg_data) {
  return publish_data(type, channel, msg_data);
}

bool IntraNode::publish(IntraType type, uint32_t channel, Bytes&& msg_data) {
  return publish_data(type, channel, std::move(msg_data));
}

bool IntraNode::publish(IntraType type, uint32_t channel, const IntraData& intra_data) {
  return publish_data(type, channel, intra_data);
}

void IntraNode::deliver_request(NodeImpl* server, NodeImpl* requester, MessageLoop* target_loop, const Bytes& request,
                                const NodeImpl::MsgCallback& callback) {
  Bytes response;
  bool replied = false;

  invoke_req_resp_callback(server, [&, this](NodeImpl* impl, const auto& handler) {
    auto* loop = impl->get_message_loop();

    if (!loop) {
      loop = pipeline_;
    }

    if VUNLIKELY (!is_contains_impl(requester) || impl->has_suspend || loop != target_loop) {
      return;
    }

    if (callback) {
      handler(0, request, &response);
      replied = true;
    } else {
      handler(0, request, nullptr);
    }
  });

  if (replied) {
    callback(response);
  }
}

bool IntraNode::call(NodeImpl* requester, IntraType type, uint32_t channel, const Bytes& req_data,
                     NodeImpl::MsgCallback&& callback, bool inline_if_same_thread) {
  if (callback) {
    auto response_callback = std::move(callback);

    callback = [weak_self = weak_from_this(), requester, inline_if_same_thread,
                response_callback = std::move(response_callback)](const Bytes& response) mutable {
      auto self = weak_self.lock();

      if VUNLIKELY (!self) {
        return;
      }

      MessageLoop* loop = nullptr;
      NodeImpl::LoopGatePtr loop_gate;

      self->invoke_callback(requester, [&]() {
        loop = requester->get_message_loop();

        if (!loop || inline_if_same_thread) {
          response_callback(response);
          return;
        }

        loop_gate = requester->get_loop_gate();
      });

      if (loop && !inline_if_same_thread) {
        NodeImpl::post_to_loop_gate(loop_gate, loop,
                                    [weak_self = std::move(weak_self), requester, loop, response = response,
                                     response_callback = std::move(response_callback)]() {
                                      auto self = weak_self.lock();

                                      if VUNLIKELY (!self) {
                                        return;
                                      }

                                      bool attached = false;
                                      self->invoke_callback(
                                          requester, [&]() { attached = requester->get_message_loop() == loop; });

                                      if VLIKELY (attached) {
                                        response_callback(response);
                                      }
                                    });
      }
    };
  }

  bool found = false;
  bool result = false;
  NodeImpl* server = nullptr;
  NodeImpl::LoopGatePtr server_loop_gate;
  MessageLoop* attached_loop = nullptr;
  MessageLoop* target_loop = nullptr;

  traverse_req_resp_callback([&](NodeImpl* impl, const auto&) {
    if VUNLIKELY (impl->has_suspend.load(std::memory_order_relaxed)) {
      return;
    }

    if VUNLIKELY (found) {
      const auto* transport = impl->get_target_transport<IntraTransport>();
      VLOG_F(*transport, "Two identical service requests.");
      return;
    }

    found = true;
    server = impl;
    attached_loop = impl->get_message_loop();

    if (attached_loop) {
      server_loop_gate = impl->get_loop_gate();
      target_loop = attached_loop;
    } else {
      target_loop = pipeline_;
    }
  });

  if (server) {
    const bool in_same_thread = inline_if_same_thread && target_loop != nullptr &&
                                (attached_loop ? NodeImpl::is_loop_gate_in_same_thread(server_loop_gate, attached_loop)
                                               : target_loop->is_in_same_thread());

    if (!target_loop || in_same_thread) {
      deliver_request(server, requester, target_loop, req_data, callback);
      result = true;
    } else {
      auto task = [weak_self = weak_from_this(), server, requester, target_loop, req_data = req_data,
                   callback = std::move(callback)]() {
        auto self = weak_self.lock();

        if VUNLIKELY (!self) {
          return;
        }

        self->deliver_request(server, requester, target_loop, req_data, callback);
      };

      result = attached_loop ? NodeImpl::post_to_loop_gate(server_loop_gate, attached_loop, std::move(task))
                             : target_loop->post_task(std::move(task));
    }
  }

  if (!found && type == IntraType::kQueue) {
    return pipeline_->post_task([weak_self = weak_from_this(), requester, channel, req_data = req_data,
                                 callback = std::move(callback)]() mutable {
      auto self = weak_self.lock();

      if (self && self->is_contains_impl(requester)) {
        self->call(requester, IntraType::kDirect, channel, req_data, std::move(callback), true);
      }
    });
  }

  return result;
}

}  // namespace vlink
