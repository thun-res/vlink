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

#include "./mqtt_publisher_impl.h"

#include <utility>

#include "./base/helpers.h"

namespace vlink {

// MqttPublisherImpl
MqttPublisherImpl::MqttPublisherImpl(const MqttConf& conf) : conf_(conf) {}

void MqttPublisherImpl::init() {
  static auto& factory = MqttFactory::get();

  conf_.hash_code = Helpers::get_hash_code(conf_.event);

  object_ = factory.get_object<Object>(
      {kImplType, conf_.address, conf_.domain, conf_.qos, conf_.fragment, get_all_properties()});

  object_->add_impl(this);
  object_->enable_connection_notifications();

  object_->register_sub_connect_callback(this, [this](bool) { PublisherImpl::update_subscribers(); });

  PublisherImpl::update_subscribers();
}

void MqttPublisherImpl::deinit() {
  detach();

  object_->remove_impl(this);
}

const Conf* MqttPublisherImpl::get_conf() const { return &conf_; }

const AbstractNode* MqttPublisherImpl::get_abstract_node() const { return object_.get(); }

bool MqttPublisherImpl::has_subscribers() const { return object_->has_subscribers(); }

bool MqttPublisherImpl::write(const Bytes& msg_data) {
  return object_->publish(static_cast<uint64_t>(conf_.hash_code), msg_data);
}

}  // namespace vlink
