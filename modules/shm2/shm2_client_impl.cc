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

#include "./shm2_client_impl.h"

#include <utility>

#include "./base/elapsed_timer.h"
#include "./base/helpers.h"

namespace vlink {

// Shm2ClientImpl
Shm2ClientImpl::Shm2ClientImpl(const Shm2Conf& conf) : conf_(conf) {}

void Shm2ClientImpl::init() {
  static auto& factory = Shm2Factory::get();

  conf_.hash_code = Helpers::get_hash_code(conf_.event);

  object_ = factory.get_object<Object>(
      {kImplType, conf_.address, conf_.domain, conf_.depth, conf_.history, conf_.wait, conf_.size});

  object_->add_impl(this);

  object_->register_server_connect_callback(this, [this](bool) {
    auto* message_loop = get_message_loop();

    if (message_loop) {
      message_loop->post_task([this]() { ClientImpl::update_connected(); });
    } else {
      ClientImpl::update_connected();
    }
  });

  ClientImpl::update_connected();
}

void Shm2ClientImpl::deinit() {
  object_->remove_impl(this);

  detach();
}

void Shm2ClientImpl::interrupt() {
  ClientImpl::interrupt();

  ack_manager_.clear();
}

bool Shm2ClientImpl::is_support_loan() const { return true; }

Bytes Shm2ClientImpl::loan(int64_t size) { return object_->loan(static_cast<uint64_t>(conf_.hash_code), size); }

bool Shm2ClientImpl::return_loan(const Bytes& bytes) { return object_->release(bytes); }

const Conf* Shm2ClientImpl::get_conf() const { return &conf_; }

const AbstractNode* Shm2ClientImpl::get_abstract_node() const { return object_.get(); }

bool Shm2ClientImpl::is_connected() const { return object_->is_connected(); }

void Shm2ClientImpl::detect_connected(ConnectCallback&& callback) {
  object_->enable_detect_timer();

  ClientImpl::detect_connected(std::move(callback));
}

bool Shm2ClientImpl::wait_for_connected(std::chrono::milliseconds timeout) {
  object_->enable_detect_timer();

  return ClientImpl::wait_for_connected(timeout);
}

bool Shm2ClientImpl::call(const Bytes& req_data, MsgCallback&& callback, std::chrono::milliseconds timeout) {
  if VUNLIKELY (!callback) {
    return object_->call(static_cast<uint64_t>(conf_.hash_code), req_data);
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

    uint64_t response_seq = 0;
    bool has_response_seq = false;

    bool ret = ack_manager_.process(
        ack_request, timeout.count() - elapsed,
        [this, &req_data, &response_seq, &has_response_seq, ack_function = std::move(ack_function)]() mutable {
          has_response_seq =
              object_->call(static_cast<uint64_t>(conf_.hash_code), req_data, std::move(ack_function), &response_seq);
          return has_response_seq;
        });

    if VUNLIKELY (!ret && has_response_seq) {
      object_->remove_response_callback(response_seq);
    }

    return ret;
  }

  return object_->call(static_cast<uint64_t>(conf_.hash_code), req_data, std::move(callback));
}

}  // namespace vlink
