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

#include "./someip_server_impl.h"

#include <utility>

namespace vlink {

// SomeipServerImpl
SomeipServerImpl::SomeipServerImpl(const SomeipConf& conf) : conf_(conf) {}

void SomeipServerImpl::init() {
  static auto& factory = SomeipFactory::get();

  object_ = factory.get_object<Object>({kImplType, conf_.service, conf_.instance});

  object_->add_impl(this);

  object_->start();
}

void SomeipServerImpl::deinit() { object_->remove_impl(this); }

bool SomeipServerImpl::suspend() {
  has_suspend = true;

  return true;
}

bool SomeipServerImpl::resume() {
  has_suspend = false;

  return true;
}

bool SomeipServerImpl::is_suspend() const { return has_suspend; }

const Conf* SomeipServerImpl::get_conf() const { return &conf_; }

const AbstractNode* SomeipServerImpl::get_abstract_node() const { return object_.get(); }

bool SomeipServerImpl::attach(class MessageLoop*) {
  VLOG_W("Function [attach] is not supported.");
  return false;
}

bool SomeipServerImpl::detach() {
  VLOG_W("Function [detach] is not supported.");
  return false;
}

bool SomeipServerImpl::has_clients() const {
  VLOG_W("Function [has_clients] is not supported.");

  return false;
}

bool SomeipServerImpl::listen(ReqRespCallback&& callback) {
  object_->register_req_resp_callback(this, std::move(callback));

  return true;
}

}  // namespace vlink
