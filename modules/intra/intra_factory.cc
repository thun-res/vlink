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

#include <utility>

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
  for (auto& [num, pipeline] : pipeline_map_) {
    pipeline.quit(true);
    pipeline.wait_for_quit();
  }

  pipeline_map_.clear();
}

// IntraNode
IntraNode::IntraNode(const IntraID& id) {
  const auto& [impl_type, address, pipeline, type] = id;

  static auto& factory = IntraFactory::get();

  if (type == IntraType::kQueue) {
    pipeline_ = &(factory.get_pipeline(pipeline));
  }
}

IntraNode::~IntraNode() = default;

std::any IntraNode::get_native_handle() const { return this; }

bool IntraNode::publish(IntraType type, uint32_t channel, const Bytes& msg_data) {
  if (type == IntraType::kQueue && pipeline_) {
    std::weak_ptr<IntraNode> weak_self = shared_from_this();

    pipeline_->post_task([weak_self, channel, msg_data]() {
      auto self = weak_self.lock();

      if VUNLIKELY (!self) {
        return;
      }

      self->traverse_msg_callback([channel, &msg_data](NodeImpl* impl, const auto& callback) {
        const auto* conf_ptr = impl->get_target_conf<IntraConf>();

        if (conf_ptr->hash_code != channel || impl->has_suspend) {
          return;
        }

        callback(msg_data);
      });
    });

    return true;
  } else {
    bool ok = false;

    traverse_msg_callback([channel, &msg_data, &ok](NodeImpl* impl, const auto& callback) {
      const auto* conf_ptr = impl->get_target_conf<IntraConf>();

      if (conf_ptr->hash_code != channel || impl->has_suspend) {
        return;
      }

      callback(msg_data);

      ok = true;
    });

    return ok;
  }
}

bool IntraNode::publish(IntraType type, uint32_t channel, const IntraData& intra_data) {
  if (type == IntraType::kQueue && pipeline_) {
    std::weak_ptr<IntraNode> weak_self = shared_from_this();

    pipeline_->post_task([weak_self, channel, intra_data]() {
      auto self = weak_self.lock();

      if VUNLIKELY (!self) {
        return;
      }

      self->traverse_intra_msg_callback([channel, &intra_data](NodeImpl* impl, const auto& callback) {
        const auto* conf_ptr = impl->get_target_conf<IntraConf>();

        if (conf_ptr->hash_code != channel || impl->has_suspend) {
          return;
        }

        callback(intra_data);
      });
    });

    return true;
  } else {
    bool ok = false;

    traverse_intra_msg_callback([channel, &intra_data, &ok](NodeImpl* impl, const auto& callback) {
      const auto* conf_ptr = impl->get_target_conf<IntraConf>();

      if (conf_ptr->hash_code != channel || impl->has_suspend) {
        return;
      }

      callback(intra_data);

      ok = true;
    });

    return ok;
  }
}

bool IntraNode::call(IntraType type, uint32_t channel, const Bytes& req_data, NodeImpl::MsgCallback&& callback) {
  if (type == IntraType::kQueue && pipeline_) {
    std::weak_ptr<IntraNode> weak_self = shared_from_this();

    pipeline_->post_task([weak_self, channel, req_data, callback = std::move(callback)]() {
      auto self = weak_self.lock();

      if VUNLIKELY (!self) {
        return;
      }

      self->traverse_req_resp_callback([&self, channel, &req_data, &callback](NodeImpl* impl, const auto& callback2) {
        const auto* conf_ptr = impl->get_target_conf<IntraConf>();

        if (conf_ptr->hash_code != channel || impl->has_suspend) {
          self->ignore_called();
          return;
        }

        if VUNLIKELY (self->has_called()) {
          VLOG_F(*conf_ptr, "Two identical service requests.");
          return;
        }

        if (callback) {
          Bytes bytes;
          callback2(0, req_data, &bytes);
          callback(bytes);
        } else {
          callback2(0, req_data, nullptr);
        }
      });
    });

    return true;
  } else {
    bool ok = false;

    traverse_req_resp_callback([this, channel, &req_data, &callback, &ok](NodeImpl* impl, const auto& callback2) {
      const auto* conf_ptr = impl->get_target_conf<IntraConf>();

      if (conf_ptr->hash_code != channel || impl->has_suspend) {
        ignore_called();
        return;
      }

      if VUNLIKELY (has_called()) {
        VLOG_F(*conf_ptr, "Two identical service requests.");
        return;
      }

      if (callback) {
        Bytes bytes;
        callback2(0, req_data, &bytes);
        callback(bytes);
      } else {
        callback2(0, req_data, nullptr);
      }

      ok = true;
    });
    return ok;
  }
}

}  // namespace vlink
