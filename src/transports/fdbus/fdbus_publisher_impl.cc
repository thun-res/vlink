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

#include "./fdbus_publisher_impl.h"

#include "./base/helpers.h"

namespace vlink {

// FdbusPublisherImpl
FdbusPublisherImpl::FdbusPublisherImpl(const FdbusTransport& transport) : transport_(transport) {}

void FdbusPublisherImpl::init() {
  static auto& factory = FdbusFactory::get();

  transport_.hash_code = Helpers::get_hash_code(transport_.event);

  object_ = factory.get_object<Object>({kImplType, transport_.mode, transport_.address});

  object_->add_impl(this);

  object_->register_sub_connect_callback(this, [this](bool present) {
    subscribed_.store(present, std::memory_order_release);
    PublisherImpl::update_subscribers();
  });

  subscribed_.store(object_->monitor_subscribers(transport_), std::memory_order_release);
  PublisherImpl::update_subscribers();
}

void FdbusPublisherImpl::deinit() { object_->remove_impl(this); }

const Transport* FdbusPublisherImpl::get_transport() const { return &transport_; }

const AbstractNode* FdbusPublisherImpl::get_abstract_node() const { return object_.get(); }

bool FdbusPublisherImpl::has_subscribers() const { return subscribed_.load(std::memory_order_acquire); }

bool FdbusPublisherImpl::write(const Bytes& msg_data) {
  return object_->broadcast(static_cast<int32_t>(transport_.hash_code), msg_data.data(), msg_data.size(),
                            transport_.event.data(), FDB_QOS_RELIABLE);
}

}  // namespace vlink
