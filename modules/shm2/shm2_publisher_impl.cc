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

#include "./shm2_publisher_impl.h"

#include <utility>

#include "./base/helpers.h"

namespace vlink {

// Shm2PublisherImpl
Shm2PublisherImpl::Shm2PublisherImpl(const Shm2Conf& conf) : conf_(conf) {}

void Shm2PublisherImpl::init() {
  static auto& factory = Shm2Factory::get();

  conf_.hash_code = Helpers::get_hash_code(conf_.event);

  object_ = factory.get_object<Object>(
      {kImplType, conf_.address, conf_.domain, conf_.depth, conf_.history, conf_.wait, conf_.size});

  object_->add_impl(this);

  object_->register_sub_connect_callback(this, [this](bool) {
    auto* message_loop = get_message_loop();

    if (message_loop) {
      message_loop->post_task([this]() { PublisherImpl::update_subscribers(); });
    } else {
      PublisherImpl::update_subscribers();
    }
  });

  PublisherImpl::update_subscribers();
}

void Shm2PublisherImpl::deinit() {
  object_->remove_impl(this);

  detach();
}

bool Shm2PublisherImpl::is_support_loan() const { return true; }

Bytes Shm2PublisherImpl::loan(int64_t size) { return object_->loan(static_cast<uint64_t>(conf_.hash_code), size); }

bool Shm2PublisherImpl::return_loan(const Bytes& bytes) { return object_->release(bytes); }

const Conf* Shm2PublisherImpl::get_conf() const { return &conf_; }

const AbstractNode* Shm2PublisherImpl::get_abstract_node() const { return object_.get(); }

bool Shm2PublisherImpl::has_subscribers() const { return object_->has_subscribers(); }

void Shm2PublisherImpl::detect_subscribers(ConnectCallback&& callback) {
  object_->enable_detect_timer();

  PublisherImpl::detect_subscribers(std::move(callback));
}

bool Shm2PublisherImpl::wait_for_subscribers(std::chrono::milliseconds timeout) {
  object_->enable_detect_timer();

  return PublisherImpl::wait_for_subscribers(timeout);
}

bool Shm2PublisherImpl::write(const Bytes& msg_data) {
  return object_->publish(static_cast<uint64_t>(conf_.hash_code), msg_data);
}

}  // namespace vlink
