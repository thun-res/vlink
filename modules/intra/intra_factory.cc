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

  if VUNLIKELY (IntraConf::get_thread_count() != 1) {
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
    pipeline_ = &(factory.get_pipeline(pipeline));
  }
}

IntraNode::~IntraNode() = default;

std::any IntraNode::get_native_handle() const { return this; }

template <typename DataT>
void IntraNode::deliver_data(MessageLoop* target_loop, uint32_t channel, const DataT& data) {
  auto deliver = [this, target_loop, channel, &data](NodeImpl* impl, const auto& callback) {
    const auto* conf = impl->get_target_conf<IntraConf>();
    auto* loop = impl->get_message_loop();

    if (!loop) {
      loop = pipeline_;
    }

    if VUNLIKELY (conf->hash_code != channel || impl->has_suspend || loop != target_loop) {
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
bool IntraNode::publish_data(IntraType type, uint32_t channel, const DataT& data) {
  MessageLoop* first_loop = nullptr;
  std::vector<MessageLoop*> other_loops;
  bool delivered = false;

  auto route = [this, channel, &data, &first_loop, &other_loops, &delivered](NodeImpl* impl, const auto& callback) {
    const auto* conf = impl->get_target_conf<IntraConf>();

    if VUNLIKELY (conf->hash_code != channel || impl->has_suspend) {
      return;
    }

    auto* loop = impl->get_message_loop();

    if (!loop) {
      loop = pipeline_;
    }

    if (!loop) {
      callback(data);
      delivered = true;
    } else if (!first_loop) {
      first_loop = loop;
    } else if (loop != first_loop && std::find(other_loops.begin(), other_loops.end(), loop) == other_loops.end()) {
      other_loops.push_back(loop);
    }
  };

  if constexpr (std::is_same_v<DataT, Bytes>) {
    traverse_msg_callback(route);
  } else {
    traverse_intra_msg_callback(route);
  }

  if (!first_loop) {
    if (delivered || type != IntraType::kQueue) {
      return delivered;
    }

    first_loop = pipeline_;
  }

  auto weak_self = weak_from_this();

  if (other_loops.empty()) {
    return first_loop->post_task([weak_self, first_loop, channel, data]() {
      auto self = weak_self.lock();

      if VUNLIKELY (!self) {
        return;
      }

      self->deliver_data(first_loop, channel, data);
    });
  }

  if constexpr (std::is_same_v<DataT, Bytes>) {
    auto shared_data = MemoryResource::make_shared<Bytes>(data);
    auto post = [&weak_self, channel, &shared_data](MessageLoop* loop) {
      return loop->post_task([weak_self, loop, channel, shared_data]() {
        auto self = weak_self.lock();

        if VUNLIKELY (!self) {
          return;
        }

        self->deliver_data(loop, channel, *shared_data);
      });
    };

    bool result = post(first_loop);

    for (auto* loop : other_loops) {
      result = post(loop) && result;
    }

    return result;
  } else {
    auto post = [&weak_self, channel, &data](MessageLoop* loop) {
      return loop->post_task([weak_self, loop, channel, data]() {
        auto self = weak_self.lock();

        if VUNLIKELY (!self) {
          return;
        }

        self->deliver_data(loop, channel, data);
      });
    };

    bool result = post(first_loop);

    for (auto* loop : other_loops) {
      result = post(loop) && result;
    }

    return result;
  }
}

bool IntraNode::publish(IntraType type, uint32_t channel, const Bytes& msg_data) {
  return publish_data(type, channel, msg_data);
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
  auto weak_self = weak_from_this();

  if (callback) {
    auto response_callback = std::move(callback);

    callback = [weak_self, requester, inline_if_same_thread,
                response_callback = std::move(response_callback)](const Bytes& response) mutable {
      auto self = weak_self.lock();

      if VUNLIKELY (!self) {
        return;
      }

      MessageLoop* loop = nullptr;

      self->invoke_callback(requester, [&]() {
        loop = requester->get_message_loop();

        if (!loop || inline_if_same_thread) {
          response_callback(response);
        }
      });

      if (loop && !inline_if_same_thread) {
        loop->post_task([weak_self, requester, loop, response, response_callback = std::move(response_callback)]() {
          auto self = weak_self.lock();

          if VUNLIKELY (!self) {
            return;
          }

          if (self->is_contains_impl(requester) && requester->get_message_loop() == loop) {
            response_callback(response);
          }
        });
      }
    };
  }

  bool found = false;
  bool result = false;
  NodeImpl* server = nullptr;
  MessageLoop* target_loop = nullptr;

  traverse_req_resp_callback([&](NodeImpl* impl, const auto&) {
    const auto* conf = impl->get_target_conf<IntraConf>();

    if VUNLIKELY (conf->hash_code != channel || impl->has_suspend) {
      return;
    }

    if VUNLIKELY (found) {
      VLOG_F(*conf, "Two identical service requests.");
      return;
    }

    found = true;
    server = impl;
    target_loop = impl->get_message_loop();

    if (!target_loop) {
      target_loop = pipeline_;
    }
  });

  if (server) {
    if (!target_loop || (inline_if_same_thread && target_loop->is_in_same_thread())) {
      deliver_request(server, requester, target_loop, req_data, callback);
      result = true;
    } else {
      result = target_loop->post_task(
          [weak_self, server, requester, target_loop, req_data, callback = std::move(callback)]() {
            auto self = weak_self.lock();

            if VUNLIKELY (!self) {
              return;
            }

            self->deliver_request(server, requester, target_loop, req_data, callback);
          });
    }
  }

  if (!found && type == IntraType::kQueue) {
    return pipeline_->post_task([weak_self, requester, channel, req_data, callback = std::move(callback)]() mutable {
      if (auto self = weak_self.lock(); self && self->is_contains_impl(requester)) {
        self->call(requester, IntraType::kDirect, channel, req_data, std::move(callback), true);
      }
    });
  }

  return result;
}

}  // namespace vlink
