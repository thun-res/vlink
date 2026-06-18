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

#include "./shm2_setter_impl.h"

#include "./base/helpers.h"

namespace vlink {

// Shm2SetterImpl
Shm2SetterImpl::Shm2SetterImpl(const Shm2Conf& conf) : conf_(conf) {}

void Shm2SetterImpl::init() {
  static auto& factory = Shm2Factory::get();

  conf_.hash_code = Helpers::get_hash_code(conf_.event);

  object_ = factory.get_object<Object>(
      {kImplType, conf_.address, conf_.domain, conf_.depth, conf_.history, conf_.wait, conf_.size});

  object_->add_impl(this);
}

void Shm2SetterImpl::deinit() {
  detach();

  object_->remove_impl(this);
}

bool Shm2SetterImpl::is_support_loan() const { return true; }

Bytes Shm2SetterImpl::loan(int64_t size) { return object_->loan(static_cast<uint64_t>(conf_.hash_code), size); }

bool Shm2SetterImpl::return_loan(const Bytes& bytes) { return object_->release(bytes); }

const Conf* Shm2SetterImpl::get_conf() const { return &conf_; }

const AbstractNode* Shm2SetterImpl::get_abstract_node() const { return object_.get(); }

void Shm2SetterImpl::write(const Bytes& msg_data) {
  object_->publish(static_cast<uint64_t>(conf_.hash_code), msg_data);
}

void Shm2SetterImpl::sync(SyncCallback&& callback) {
  (void)callback;

  object_->enable_detect_timer();
}

}  // namespace vlink
