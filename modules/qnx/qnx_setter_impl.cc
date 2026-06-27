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

#include "./qnx_setter_impl.h"

#include "./base/helpers.h"

namespace vlink {

// QnxSetterImpl
QnxSetterImpl::QnxSetterImpl(const QnxConf& conf) : conf_(conf) {}

void QnxSetterImpl::init() {
  static auto& factory = QnxFactory::get();

  conf_.hash_code = Helpers::get_hash_code(conf_.event);

  object_ = factory.get_object<Object>({kImplType, conf_.address});

  object_->add_impl(this);
}

void QnxSetterImpl::deinit() { object_->remove_impl(this); }

const Conf* QnxSetterImpl::get_conf() const { return &conf_; }

const AbstractNode* QnxSetterImpl::get_abstract_node() const { return object_.get(); }

bool QnxSetterImpl::attach(class MessageLoop*) {
  VLOG_W("Function [attach] is not supported.");
  return false;
}

bool QnxSetterImpl::detach() {
  VLOG_W("Function [detach] is not supported.");
  return false;
}

void QnxSetterImpl::write(const Bytes& msg_data) { object_->publish(conf_.hash_code, msg_data); }

void QnxSetterImpl::sync(SyncCallback&& callback) {
  object_->register_sub_connect_callback(this, [callback = std::move(callback)](bool connected) mutable {
    if (connected) {
      callback();
    }
  });
}

}  // namespace vlink
