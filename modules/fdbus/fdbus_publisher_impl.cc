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
FdbusPublisherImpl::FdbusPublisherImpl(const FdbusConf& conf) : conf_(conf) {}

void FdbusPublisherImpl::init() {
  static auto& factory = FdbusFactory::get();

  conf_.hash_code = Helpers::get_hash_code(conf_.event);

  object_ = factory.get_object<Object>({kImplType, conf_.transport, conf_.address});

  object_->add_impl(this);

  object_->register_sub_connect_callback(this, [this](bool) { PublisherImpl::update_subscribers(); });

  PublisherImpl::update_subscribers();
}

void FdbusPublisherImpl::deinit() { object_->remove_impl(this); }

const Conf* FdbusPublisherImpl::get_conf() const { return &conf_; }

const AbstractNode* FdbusPublisherImpl::get_abstract_node() const { return object_.get(); }

bool FdbusPublisherImpl::attach(class MessageLoop*) {
  VLOG_W("Function [attach] is not supported.");
  return false;
}

bool FdbusPublisherImpl::detach() {
  VLOG_W("Function [detach] is not supported.");
  return false;
}

bool FdbusPublisherImpl::has_subscribers() const { return object_->getSessionCount() > 0; }

bool FdbusPublisherImpl::write(const Bytes& msg_data) {
  return object_->broadcast(static_cast<int32_t>(conf_.hash_code), msg_data.data(), msg_data.size(), conf_.event.data(),
                            FDB_QOS_RELIABLE);
}

}  // namespace vlink
