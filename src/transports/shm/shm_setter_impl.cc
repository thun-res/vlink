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
ShmSetterImpl::ShmSetterImpl(const ShmTransport& transport) : transport_(transport) {}

void ShmSetterImpl::init() {
  static auto& factory = ShmFactory::get();

  transport_.hash_code = Helpers::get_hash_code(transport_.event);

  if (transport_.history == 0) {
    transport_.history = 1;
  }

  object_ = factory.get_object<Object>({kImplType, transport_.address, transport_.domain, transport_.depth,
                                        transport_.history, transport_.wait, transport_.event, nullptr});

  object_->add_impl(this);
}

void ShmSetterImpl::deinit() {
  if (object_) {
    object_->remove_impl(this);
  }

  detach();
}

bool ShmSetterImpl::is_support_loan() const { return true; }

Bytes ShmSetterImpl::loan(int64_t size) { return object_->loan(static_cast<uint64_t>(transport_.hash_code), size); }

bool ShmSetterImpl::return_loan(const Bytes& bytes) { return object_->release(bytes); }

const Transport* ShmSetterImpl::get_transport() const { return &transport_; }

const AbstractNode* ShmSetterImpl::get_abstract_node() const { return object_.get(); }

void ShmSetterImpl::write(const Bytes& msg_data) {
  object_->publish(static_cast<uint64_t>(transport_.hash_code), msg_data);
}

void ShmSetterImpl::sync(SyncCallback&& callback) { callback(); }

}  // namespace vlink
