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

#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <vector>

#include "./base/message_loop.h"
#include "./impl/abstract_factory.h"
#include "./modules/intra_conf.h"

namespace vlink {

enum class IntraType : uint8_t {
  kQueue = 0,
  kDirect = 1,
};

using IntraID = std::tuple<uint8_t, std::string, int32_t, IntraType>;

// IntraPipeline
class IntraPipeline final : public MessageLoop {
 public:
  using MessageLoop::MessageLoop;

 protected:
  size_t get_max_task_count() const override;
};

// IntraFactory
class IntraFactory final : public AbstractFactory<IntraID> {
 public:
  IntraPipeline& get_pipeline(int32_t pipeline);

 private:
  IntraFactory();

  ~IntraFactory() override;

  std::mutex pipeline_mtx_;
  std::unordered_map<int32_t, IntraPipeline> pipeline_map_;

  VLINK_SINGLETON_DECLARE(IntraFactory)
};

// IntraNode
class IntraNode final : public AbstractObject<IntraID>, public std::enable_shared_from_this<IntraNode> {
 public:
  explicit IntraNode(const IntraID&);

  ~IntraNode() override;

  std::any get_native_handle() const override;

  bool publish(IntraType type, uint32_t channel, const Bytes& msg_data);

  bool publish(IntraType type, uint32_t channel, const IntraData& intra_data);

  bool call(IntraType type, uint32_t channel, const Bytes& req_data, NodeImpl::MsgCallback&& callback = nullptr);

 private:
  IntraPipeline* pipeline_{nullptr};
};

}  // namespace vlink
