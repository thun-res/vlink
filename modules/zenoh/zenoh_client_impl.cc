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

#include "./zenoh_client_impl.h"

#include <limits>
#include <utility>

#include "./base/elapsed_timer.h"
#include "./base/helpers.h"

namespace vlink {

// ZenohClientImpl
ZenohClientImpl::ZenohClientImpl(const ZenohConf& conf) : conf_(conf) {}

void ZenohClientImpl::init() {
  static auto& factory = ZenohFactory::get();

  conf_.hash_code = Helpers::get_hash_code(conf_.event);

  auto properties = get_all_properties();
  conf_.append_properties(properties);

  object_ = factory.get_object<Object>(
      {kImplType, conf_.address, conf_.domain, conf_.depth, conf_.qos, conf_.fragment, properties});

  object_->add_impl(this);

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

void ZenohClientImpl::deinit() {
  detach();

  object_->cancel_calls(this);
  ack_manager_.clear();
  object_->remove_impl(this);
}

void ZenohClientImpl::interrupt() {
  ClientImpl::interrupt();

  if (object_) {
    object_->cancel_calls(this);
  }

  ack_manager_.clear();
}

const Conf* ZenohClientImpl::get_conf() const { return &conf_; }

const AbstractNode* ZenohClientImpl::get_abstract_node() const { return object_.get(); }

bool ZenohClientImpl::is_connected() const { return object_->is_connected(); }

bool ZenohClientImpl::is_support_loan() const { return object_->is_support_loan(); }

Bytes ZenohClientImpl::loan(int64_t size) { return object_->loan(static_cast<uint64_t>(conf_.hash_code), size); }

bool ZenohClientImpl::return_loan(const Bytes& bytes) { return object_->release(bytes); }

bool ZenohClientImpl::call(const Bytes& req_data, MsgCallback&& callback, std::chrono::milliseconds timeout) {
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

    int remaining_timeout = -1;

    if (timeout.count() > 0) {
      auto remaining = timeout.count() - elapsed;

      if (remaining > std::numeric_limits<int>::max()) {
        remaining_timeout = std::numeric_limits<int>::max();
      } else if (remaining < std::numeric_limits<int>::min()) {
        remaining_timeout = std::numeric_limits<int>::min();
      } else {
        remaining_timeout = static_cast<int>(remaining);
      }
    }

    auto ack_request = ack_manager_.create_request();

    auto ack_function = [this, ack_request, callback = std::move(callback)](const Bytes& resp_data) mutable {
      ack_manager_.notify(ack_request, [&callback, &resp_data]() { callback(resp_data); });
    };

    return ack_manager_.process(ack_request, remaining_timeout,
                                [this, &req_data, ack_function = std::move(ack_function), remaining_timeout]() mutable {
                                  return object_->call(this, static_cast<uint64_t>(conf_.hash_code), req_data,
                                                       std::move(ack_function), remaining_timeout);
                                });
  }

  return object_->call(this, static_cast<uint64_t>(conf_.hash_code), req_data, std::move(callback), timeout.count());
}

}  // namespace vlink
