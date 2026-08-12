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

#include "./someip_client_impl.h"

#include <limits>
#include <memory>
#include <utility>

#include "./base/elapsed_timer.h"

namespace vlink {

// SomeipClientImpl
SomeipClientImpl::SomeipClientImpl(const SomeipConf& conf) : conf_(conf) {}

void SomeipClientImpl::init() {
  static auto& factory = SomeipFactory::get();

  auto properties = factory.resolve_properties(conf_, get_all_properties());

  callback_state_ = std::make_shared<CallbackState>();

  object_ = factory.get_object<Object>({kImplType, conf_.service, conf_.instance, std::move(properties)});

  object_->add_impl(this);

  object_->register_server_connect_callback(this, [this](bool) { ClientImpl::update_connected(); });

  object_->start();

  ClientImpl::update_connected();
}

void SomeipClientImpl::deinit() {
  {
    std::lock_guard lock(callback_state_->mtx);
    callback_state_->active = false;
  }

  object_->remove_impl(this);
}

void SomeipClientImpl::interrupt() {
  ClientImpl::interrupt();

  ack_manager_.clear();
}

const Conf* SomeipClientImpl::get_conf() const { return &conf_; }

const AbstractNode* SomeipClientImpl::get_abstract_node() const { return object_.get(); }

bool SomeipClientImpl::attach(class MessageLoop*) {
  VLOG_W("Function [attach] is not supported.");
  return false;
}

bool SomeipClientImpl::detach() {
  VLOG_W("Function [detach] is not supported.");
  return false;
}

bool SomeipClientImpl::is_connected() const { return object_->is_connected(); }

bool SomeipClientImpl::call(const Bytes& req_data, MsgCallback&& callback, std::chrono::milliseconds timeout) {
  if VUNLIKELY (!callback) {
    return object_->call(conf_.method, req_data);
  }

  if (timeout.count() != 0) {
    ack_manager_.reset_interrupted();

    if VUNLIKELY (object_->is_receive_thread()) {
      VLOG_W("Blocking call is not allowed on the OpenSOMEIP receive thread.");
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

    int remaining_timeout = -1;

    if (timeout.count() > 0) {
      const auto remaining = timeout.count() - elapsed;
      remaining_timeout =
          remaining > std::numeric_limits<int>::max() ? std::numeric_limits<int>::max() : static_cast<int>(remaining);
    }

    auto ack_request = ack_manager_.create_request();
    auto callback_state = callback_state_;
    uint64_t seq = 0;
    bool has_seq = false;

    auto ack_function = [this, callback_state = std::move(callback_state), ack_request,
                         callback = std::move(callback)](const Bytes& resp_data) mutable {
      std::lock_guard lock(callback_state->mtx);

      if VUNLIKELY (!callback_state->active) {
        return;
      }

      ack_manager_.notify(ack_request, [&callback, &resp_data]() { callback(resp_data); });
    };

    bool ret =
        ack_manager_.process(ack_request, remaining_timeout,
                             [this, &req_data, &seq, &has_seq, ack_function = std::move(ack_function)]() mutable {
                               has_seq = object_->call(conf_.method, req_data, std::move(ack_function), &seq);
                               return has_seq;
                             });

    if VUNLIKELY (!ret && has_seq) {
      object_->remove_response_callback(seq);
    }

    return ret;
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

  return object_->call(conf_.method, req_data, std::move(response_callback));
}

}  // namespace vlink
