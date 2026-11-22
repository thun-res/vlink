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

#include "./intra_setter_impl.h"

#include "./base/helpers.h"

namespace vlink {

// IntraSetterImpl
IntraSetterImpl::IntraSetterImpl(const IntraTransport& transport) : transport_(transport) {
  if (transport.type == "direct") {
    type_ = IntraType::kDirect;
  }
}

void IntraSetterImpl::init() {
  static auto& factory = IntraFactory::get();

  transport_.hash_code = Helpers::get_hash_code(transport_.event);

  object_ =
      factory.get_object<Object>({kImplType, transport_.address, transport_.pipeline, type_, transport_.hash_code});

  object_->add_impl(this);
}

void IntraSetterImpl::deinit() { object_->remove_impl(this); }

const Transport* IntraSetterImpl::get_transport() const { return &transport_; }

const AbstractNode* IntraSetterImpl::get_abstract_node() const { return object_.get(); }

void IntraSetterImpl::write(const Bytes& value_data) { object_->publish(type_, transport_.hash_code, value_data); }

void IntraSetterImpl::sync(SyncCallback&& callback) {
  object_->register_sub_connect_callback(this, [callback = std::move(callback)](bool connected) mutable {
    if (connected) {
      callback();
    }
  });
}

}  // namespace vlink
