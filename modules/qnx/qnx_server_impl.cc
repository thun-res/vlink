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

#include "./qnx_server_impl.h"

#include <utility>

#include "./base/helpers.h"

namespace vlink {

// QnxServerImpl
QnxServerImpl::QnxServerImpl(const QnxConf& conf) : conf_(conf) {}

void QnxServerImpl::init() {
  static auto& factory = QnxFactory::get();

  conf_.hash_code = Helpers::get_hash_code(conf_.event);

  object_ = factory.get_object<Object>({kImplType, conf_.address});

  object_->add_impl(this);
}

void QnxServerImpl::deinit() { object_->remove_impl(this); }

bool QnxServerImpl::suspend() {
  has_suspend = true;

  return true;
}

bool QnxServerImpl::resume() {
  has_suspend = false;

  return true;
}

bool QnxServerImpl::is_suspend() const { return has_suspend; }

const Conf* QnxServerImpl::get_conf() const { return &conf_; }

const AbstractNode* QnxServerImpl::get_abstract_node() const { return object_.get(); }

bool QnxServerImpl::attach(class MessageLoop*) {
  VLOG_W("Function [attach] is not supported.");
  return false;
}

bool QnxServerImpl::detach() {
  VLOG_W("Function [detach] is not supported.");
  return false;
}

bool QnxServerImpl::has_clients() const { return object_->get_session_count() > 0; }

bool QnxServerImpl::listen(ReqRespCallback&& callback) {
  object_->register_req_resp_callback(this, std::move(callback));

  return true;
}

}  // namespace vlink
