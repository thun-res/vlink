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

#include "./mqtt_subscriber_impl.h"

#include <utility>

#include "./base/helpers.h"

namespace vlink {

// MqttSubscriberImpl
MqttSubscriberImpl::MqttSubscriberImpl(const MqttConf& conf) : conf_(conf) {}

void MqttSubscriberImpl::init() {
  static auto& factory = MqttFactory::get();

  conf_.hash_code = Helpers::get_hash_code(conf_.event);

  object_ = factory.get_object<Object>(
      {kImplType, conf_.address, conf_.domain, conf_.qos, conf_.fragment, get_all_properties()});

  object_->add_impl(this);

  object_->set_latency_and_lost_enabled(is_latency_and_lost_enabled_);
}

void MqttSubscriberImpl::deinit() {
  detach();

  object_->remove_impl(this);

  if (!object_->has_impl()) {
    object_->unsubscribe();
  }
}

const Conf* MqttSubscriberImpl::get_conf() const { return &conf_; }

const AbstractNode* MqttSubscriberImpl::get_abstract_node() const { return object_.get(); }

bool MqttSubscriberImpl::listen(MsgCallback&& callback) {
  object_->register_msg_callback(this, std::move(callback));
  object_->subscribe();

  return true;
}

void MqttSubscriberImpl::set_latency_and_lost_enabled(bool enable) {
  is_latency_and_lost_enabled_.store(enable, std::memory_order_release);

  if (object_) {
    object_->set_latency_and_lost_enabled(enable);
  }
}

bool MqttSubscriberImpl::is_latency_and_lost_enabled() const {
  if (object_) {
    return object_->is_latency_and_lost_enabled();
  }

  return is_latency_and_lost_enabled_.load(std::memory_order_acquire);
}

int64_t MqttSubscriberImpl::get_latency() const {
  if (object_) {
    return object_->get_latency();
  }

  return 0;
}

SampleLostInfo MqttSubscriberImpl::get_lost() const {
  if (object_ && object_->is_latency_and_lost_enabled()) {
    return SampleLostInfo{object_->get_calculate_sample().get_total(), object_->get_calculate_sample().get_lost()};
  }

  return SampleLostInfo();
}

}  // namespace vlink
