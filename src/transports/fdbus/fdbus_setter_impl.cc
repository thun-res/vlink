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

#include "./fdbus_setter_impl.h"

#include <atomic>
#include <memory>

#include "./base/helpers.h"

namespace vlink {

// FdbusSetterImpl
FdbusSetterImpl::FdbusSetterImpl(const FdbusTransport& transport) : transport_(transport) {}

void FdbusSetterImpl::init() {
  static auto& factory = FdbusFactory::get();

  transport_.hash_code = Helpers::get_hash_code(transport_.event);

  object_ = factory.get_object<Object>({kImplType, transport_.mode, transport_.address});

  object_->add_impl(this);
}

void FdbusSetterImpl::deinit() { object_->remove_impl(this); }

const Transport* FdbusSetterImpl::get_transport() const { return &transport_; }

const AbstractNode* FdbusSetterImpl::get_abstract_node() const { return object_.get(); }

void FdbusSetterImpl::write(const Bytes& value_data) {
  if VUNLIKELY (!object_->has_clients()) {
    return;
  }

  object_->broadcast(static_cast<int32_t>(transport_.hash_code), value_data.data(), value_data.size(),
                     transport_.event.data(), FDB_QOS_RELIABLE);
}

void FdbusSetterImpl::sync(SyncCallback&& callback) {
  auto notified = std::make_shared<std::atomic_bool>(false);

  object_->register_sub_connect_callback(this, [callback, notified](bool connected) {
    if (connected) {
      notified->store(true, std::memory_order_release);
      callback();
    }
  });

  if (object_->has_subscribers(transport_) && !notified->load(std::memory_order_acquire)) {
    callback();
  }
}

}  // namespace vlink
