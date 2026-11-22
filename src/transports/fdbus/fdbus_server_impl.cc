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

#include "./fdbus_server_impl.h"

#include <utility>

#include "./base/helpers.h"

namespace vlink {

// FdbusServerImpl
FdbusServerImpl::FdbusServerImpl(const FdbusTransport& transport) : transport_(transport) {}

void FdbusServerImpl::init() {
  static auto& factory = FdbusFactory::get();

  transport_.hash_code = Helpers::get_hash_code(transport_.event);

  object_ = factory.get_object<Object>({kImplType, transport_.mode, transport_.address});

  object_->add_impl(this);
}

void FdbusServerImpl::deinit() { object_->remove_impl(this); }

bool FdbusServerImpl::suspend() {
  has_suspend = true;

  return true;
}

bool FdbusServerImpl::resume() {
  has_suspend = false;

  return true;
}

bool FdbusServerImpl::is_suspend() const { return has_suspend; }

const Transport* FdbusServerImpl::get_transport() const { return &transport_; }

const AbstractNode* FdbusServerImpl::get_abstract_node() const { return object_.get(); }

bool FdbusServerImpl::has_clients() const { return object_->has_clients(); }

bool FdbusServerImpl::listen(ReqRespCallback&& callback) {
  object_->register_req_resp_callback(this, std::move(callback));

  return true;
}

}  // namespace vlink
