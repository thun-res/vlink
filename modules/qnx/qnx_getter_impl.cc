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

#include "./qnx_getter_impl.h"

#include <utility>

#include "./base/helpers.h"

namespace vlink {

// QnxGetterImpl
QnxGetterImpl::QnxGetterImpl(const QnxConf& conf) : conf_(conf) {}

void QnxGetterImpl::init() {
  static auto& factory = QnxFactory::get();

  conf_.hash_code = Helpers::get_hash_code(conf_.event);

  object_ = factory.get_object<Object>({kImplType, conf_.address});

  object_->add_impl(this);

  object_->start_timer();
}

void QnxGetterImpl::deinit() { object_->remove_impl(this); }

bool QnxGetterImpl::suspend() {
  has_suspend = true;

  return true;
}

bool QnxGetterImpl::resume() {
  has_suspend = false;

  return true;
}

bool QnxGetterImpl::is_suspend() const { return has_suspend; }

const Conf* QnxGetterImpl::get_conf() const { return &conf_; }

const AbstractNode* QnxGetterImpl::get_abstract_node() const { return object_.get(); }

bool QnxGetterImpl::attach(class MessageLoop*) {
  VLOG_W("Function [attach] is not supported.");
  return false;
}

bool QnxGetterImpl::detach() {
  VLOG_W("Function [detach] is not supported.");
  return false;
}

bool QnxGetterImpl::listen(MsgCallback&& callback) {
  object_->register_msg_callback(this, std::move(callback));

  if (object_->is_connected()) {
    object_->subscribe(conf_.hash_code);

    has_subscribed_ = true;
  }

  object_->register_server_connect_callback(this, [this](bool connected) {
    if (!has_subscribed_ && connected) {
      object_->subscribe(conf_.hash_code);
    }

    has_subscribed_ = connected;
  });

  return true;
}

}  // namespace vlink
