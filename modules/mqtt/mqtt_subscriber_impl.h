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

#include <memory>
#include <utility>

#include "./impl/subscriber_impl.h"
#include "./mqtt_factory.h"

namespace vlink {

// MqttSubscriberImpl
class MqttSubscriberImpl final : public SubscriberImpl {
 public:
  using Object = MqttSubscriber;
  static constexpr uint8_t kImplType = kSubscriber | kGetter;

  explicit MqttSubscriberImpl(const MqttConf& conf);

 private:
  void init() override;

  void deinit() override;

  const Conf* get_conf() const override;

  const AbstractNode* get_abstract_node() const override;

  bool listen(MsgCallback&& callback) override;

  void set_latency_and_lost_enabled(bool enable) override;

  bool is_latency_and_lost_enabled() const override;

  int64_t get_latency() const override;

  SampleLostInfo get_lost() const override;

  MqttConf conf_;
  std::shared_ptr<Object> object_;

  std::atomic_bool is_latency_and_lost_enabled_{false};

  VLINK_DISALLOW_COPY_AND_ASSIGN(MqttSubscriberImpl)
};

}  // namespace vlink
