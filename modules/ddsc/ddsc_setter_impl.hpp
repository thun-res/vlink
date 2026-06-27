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
#include <utility>

#include "./ddsc_factory.hpp"
#include "./ddsc_status_listener.hpp"
#include "./impl/setter_impl.h"

namespace vlink {

// DdscSetterImpl
class DdscSetterImpl final : public SetterImpl, public AbstractNode {
 public:
  // WriterListener
  class WriterListener : public DdscWriterListener {
   public:
    explicit WriterListener(NodeImpl* impl);
  };

  explicit DdscSetterImpl(const DdscConf& conf);

 private:
  void init() override;

  void deinit() override;

  const Conf* get_conf() const override;

  const AbstractNode* get_abstract_node() const override;

  Status::BasePtr get_status(Status::Type type) const override;

  std::any get_native_handle() const override;

  void write(const Bytes& msg_data) override;

  void sync(SyncCallback&& callback) override;

  alignas(64) std::atomic<uint64_t> seq_{0};

  DdscConf conf_;
  std::shared_ptr<ddsc::DomainParticipant> participant_;
  std::shared_ptr<ddsc::Topic> topic_;
  std::shared_ptr<ddsc::Publisher> publisher_;
  std::optional<WriterListener> listener_;
  std::shared_ptr<ddsc::DataWriter> writer_;

  VLINK_DISALLOW_COPY_AND_ASSIGN(DdscSetterImpl)
};

}  // namespace vlink
