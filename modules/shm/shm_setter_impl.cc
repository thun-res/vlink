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

#include "./shm_setter_impl.h"

#include "./base/helpers.h"

namespace vlink {

// ShmSetterImpl
ShmSetterImpl::ShmSetterImpl(const ShmConf& conf) : conf_(conf) {}

void ShmSetterImpl::init() {
  static auto& factory = ShmFactory::get();

  conf_.hash_code = Helpers::get_hash_code(conf_.event);

  object_ =
      factory.get_object<Object>({kImplType, conf_.address, conf_.domain, conf_.depth, conf_.history, conf_.wait});

  object_->add_impl(this);
}

void ShmSetterImpl::deinit() {
  detach();

  object_->remove_impl(this);
}

bool ShmSetterImpl::is_support_loan() const { return true; }

Bytes ShmSetterImpl::loan(int64_t size) { return object_->loan(static_cast<uint64_t>(conf_.hash_code), size); }

bool ShmSetterImpl::return_loan(const Bytes& bytes) { return object_->release(bytes); }

const Conf* ShmSetterImpl::get_conf() const { return &conf_; }

const AbstractNode* ShmSetterImpl::get_abstract_node() const { return object_.get(); }

void ShmSetterImpl::write(const Bytes& msg_data) { object_->publish(static_cast<uint64_t>(conf_.hash_code), msg_data); }

void ShmSetterImpl::sync(SyncCallback&& callback) {
  // object_->register_sub_connect_callback(this, [callback = std::move(callback)](bool connected) {
  //   if (connected) {
  //     callback();
  //   }
  // });

  (void)callback;

  object_->enable_detect_timer();
}

}  // namespace vlink
