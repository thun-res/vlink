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

#include "./mqtt_client_impl.h"

#include <limits>
#include <utility>

#include "./base/elapsed_timer.h"
#include "./base/helpers.h"

namespace vlink {

// MqttClientImpl
MqttClientImpl::MqttClientImpl(const MqttConf& conf) : conf_(conf) {}

void MqttClientImpl::init() {
  static auto& factory = MqttFactory::get();

  conf_.hash_code = Helpers::get_hash_code(conf_.event);

  object_ = factory.get_object<Object>(
      {kImplType, conf_.address, conf_.domain, conf_.qos, conf_.fragment, get_all_properties()});

  object_->add_impl(this);

  object_->start_listening();
  object_->enable_connection_notifications();

  std::weak_ptr<Object> weak_object = object_;

  object_->register_server_connect_callback(this, [this, weak_object](bool) {
    auto* message_loop = get_message_loop();

    if (message_loop) {
      message_loop->post_task([this, weak_object]() {
        auto object = weak_object.lock();

        if VLIKELY (object && object->is_contains_impl(this)) {
          ClientImpl::update_connected();
        }
      });
    } else {
      ClientImpl::update_connected();
    }
  });

  ClientImpl::update_connected();
}

void MqttClientImpl::deinit() {
  detach();

  object_->cancel_calls(this);
  ack_manager_.clear();
  object_->remove_impl(this);

  if (!object_->has_impl()) {
    object_->stop_listening();
  }
}

void MqttClientImpl::interrupt() {
  ClientImpl::interrupt();

  object_->cancel_calls(this);
  ack_manager_.clear();
}

const Conf* MqttClientImpl::get_conf() const { return &conf_; }

const AbstractNode* MqttClientImpl::get_abstract_node() const { return object_.get(); }

bool MqttClientImpl::is_connected() const { return object_->is_connected(); }

bool MqttClientImpl::call(const Bytes& req_data, MsgCallback&& callback, std::chrono::milliseconds timeout) {
  if VUNLIKELY (!callback) {
    return object_->call(this, static_cast<uint64_t>(conf_.hash_code), req_data);
  }

  if (timeout.count() != 0) {
    ack_manager_.reset_interrupted();

    ElapsedTimer timer;
    timer.start();

    if (!wait_for_connected(timeout)) {
      return false;
    }

    auto elapsed = timer.get();

    if (timeout.count() > 0 && elapsed >= timeout.count()) {
      return false;
    }

    auto ack_request = ack_manager_.create_request();

    auto ack_function = [this, ack_request, callback = std::move(callback)](const Bytes& resp_data) mutable {
      ack_manager_.notify(ack_request, [&callback, &resp_data]() { callback(resp_data); });
    };

    auto remaining = timeout.count() - elapsed;
    int object_timeout = 0;

    if (remaining > std::numeric_limits<int>::max()) {
      object_timeout = std::numeric_limits<int>::max();
    } else if (remaining < std::numeric_limits<int>::min()) {
      object_timeout = std::numeric_limits<int>::min();
    } else {
      object_timeout = static_cast<int>(remaining);
    }

    return ack_manager_.process(ack_request, object_timeout,
                                [this, &req_data, ack_function = std::move(ack_function), object_timeout]() mutable {
                                  return object_->call(this, static_cast<uint64_t>(conf_.hash_code), req_data,
                                                       std::move(ack_function), object_timeout);
                                });
  }

  return object_->call(this, static_cast<uint64_t>(conf_.hash_code), req_data, std::move(callback), timeout.count());
}

}  // namespace vlink
