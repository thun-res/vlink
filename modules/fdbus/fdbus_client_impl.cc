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

#include "./fdbus_client_impl.h"

#include <limits>
#include <utility>

#include "./base/elapsed_timer.h"
#include "./base/helpers.h"

namespace vlink {

// FdbusClientImpl
FdbusClientImpl::FdbusClientImpl(const FdbusConf& conf) : conf_(conf) {}

void FdbusClientImpl::init() {
  static auto& factory = FdbusFactory::get();

  conf_.hash_code = Helpers::get_hash_code(conf_.event);
  callback_state_ = std::make_shared<CallbackState>();

  object_ = factory.get_object<Object>({kImplType, conf_.transport, conf_.address});

  object_->add_impl(this);

  object_->start_timer();

  object_->register_server_connect_callback(this, [this](bool) { ClientImpl::update_connected(); });

  ClientImpl::update_connected();
}

void FdbusClientImpl::deinit() {
  {
    std::lock_guard lock(callback_state_->mtx);
    callback_state_->active = false;
  }

  object_->remove_impl(this);
}

void FdbusClientImpl::interrupt() {
  ClientImpl::interrupt();

  ack_manager_.clear();
}

const Conf* FdbusClientImpl::get_conf() const { return &conf_; }

const AbstractNode* FdbusClientImpl::get_abstract_node() const { return object_.get(); }

bool FdbusClientImpl::is_connected() const { return object_->getSessionCount() > 0; }

bool FdbusClientImpl::call(const Bytes& req_data, MsgCallback&& callback, std::chrono::milliseconds timeout) {
  if VUNLIKELY (!callback) {
    return object_->call(conf_.hash_code, req_data);
  }

  if (timeout.count() != 0) {
    ack_manager_.reset_interrupted();

    if VUNLIKELY (object_->worker()->isSelf()) {
      VLOG_W("Blocking call is not allowed on the fdbus worker thread.");
      return false;
    }

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
    auto callback_state = callback_state_;

    auto ack_function = [this, callback_state = std::move(callback_state), ack_request,
                         callback = std::move(callback)](const Bytes& resp_data) mutable {
      std::lock_guard lock(callback_state->mtx);

      if VUNLIKELY (!callback_state->active) {
        return;
      }

      ack_manager_.notify(ack_request, [&callback, &resp_data]() { callback(resp_data); });
    };

    int32_t remaining_timeout = -1;
    int32_t object_timeout = 0;

    if (timeout.count() > 0) {
      auto remaining = timeout.count() - elapsed;

      if VUNLIKELY (remaining > std::numeric_limits<int32_t>::max()) {
        remaining_timeout = std::numeric_limits<int32_t>::max();
      } else {
        remaining_timeout = static_cast<int32_t>(remaining);
      }

      object_timeout = remaining_timeout;
    }

    return ack_manager_.process(ack_request, remaining_timeout,
                                [this, &req_data, ack_function = std::move(ack_function), object_timeout]() mutable {
                                  return object_->call(conf_.hash_code, req_data, std::move(ack_function),
                                                       object_timeout);
                                });
  }

  auto callback_state = callback_state_;

  auto response_callback = [callback_state = std::move(callback_state),
                            callback = std::move(callback)](const Bytes& resp_data) mutable {
    std::lock_guard lock(callback_state->mtx);

    if VUNLIKELY (!callback_state->active) {
      return;
    }

    callback(resp_data);
  };

  return object_->call(conf_.hash_code, req_data, std::move(response_callback));
}

}  // namespace vlink
