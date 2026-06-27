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

#include "./fdbus_getter_impl.h"

#include <utility>

#include "./base/helpers.h"

namespace vlink {

// FdbusGetterImpl
FdbusGetterImpl::FdbusGetterImpl(const FdbusConf& conf) : conf_(conf) {}

void FdbusGetterImpl::init() {
  static auto& factory = FdbusFactory::get();

  conf_.hash_code = Helpers::get_hash_code(conf_.event);

  object_ = factory.get_object<Object>({kImplType, conf_.transport, conf_.address});

  object_->add_impl(this);

  object_->start_timer();
}

void FdbusGetterImpl::deinit() { object_->remove_impl(this); }

bool FdbusGetterImpl::suspend() {
  has_suspend = true;

  return true;
}

bool FdbusGetterImpl::resume() {
  has_suspend = false;

  return true;
}

bool FdbusGetterImpl::is_suspend() const { return has_suspend; }

const Conf* FdbusGetterImpl::get_conf() const { return &conf_; }

const AbstractNode* FdbusGetterImpl::get_abstract_node() const { return object_.get(); }

bool FdbusGetterImpl::attach(class MessageLoop*) {
  VLOG_W("Function [attach] is not supported.");
  return false;
}

bool FdbusGetterImpl::detach() {
  VLOG_W("Function [detach] is not supported.");
  return false;
}

bool FdbusGetterImpl::listen(MsgCallback&& callback) {
  object_->register_msg_callback(this, std::move(callback));

  if (object_->getSessionCount() > 0) {
    subscribe();

    has_subscribed_ = true;
  }

  object_->register_server_connect_callback(this, [this](bool connected) {
    if (!has_subscribed_ && connected) {
      subscribe();
    }

    has_subscribed_ = connected;
  });

  return true;
}

void FdbusGetterImpl::subscribe() {
  fdbus::CFdbMsgSubscribeList subscribe_list;

  subscribe_list.addNotifyItem(static_cast<int32_t>(conf_.hash_code), conf_.event.data());

  object_->subscribe(subscribe_list, FDB_QOS_RELIABLE);
}

}  // namespace vlink
