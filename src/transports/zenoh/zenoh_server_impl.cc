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

#include "./zenoh_server_impl.h"

#include <utility>

#include "./base/helpers.h"

namespace vlink {

// ZenohServerImpl
ZenohServerImpl::ZenohServerImpl(const ZenohTransport& transport) : transport_(transport) {}

void ZenohServerImpl::init() {
  static auto& factory = ZenohFactory::get();

  transport_.hash_code = ZenohFactory::get_channel(transport_.event);

  auto properties = ZenohFactory::resolve_properties(transport_, get_all_properties());

  object_ = factory.get_object<Object>({kImplType, transport_.address, transport_.event, transport_.domain,
                                        transport_.depth, transport_.qos, transport_.fragment, properties});

  object_->add_impl(this);
}

void ZenohServerImpl::deinit() {
  detach();

  if (object_) {
    object_->remove_impl(this);
  }
}

bool ZenohServerImpl::suspend() { return object_->suspend(); }

bool ZenohServerImpl::resume() { return object_->resume(); }

bool ZenohServerImpl::is_suspend() const { return object_->is_suspend(); }

const Transport* ZenohServerImpl::get_transport() const { return &transport_; }

const AbstractNode* ZenohServerImpl::get_abstract_node() const { return object_.get(); }

bool ZenohServerImpl::has_clients() const {
  VLOG_W("Function [has_clients] is not supported.");
  return false;
}

bool ZenohServerImpl::is_support_loan() const { return object_->is_support_loan(); }

Bytes ZenohServerImpl::loan(int64_t size) { return object_->loan(static_cast<uint64_t>(transport_.hash_code), size); }

bool ZenohServerImpl::return_loan(const Bytes& bytes) { return object_->release(bytes); }

bool ZenohServerImpl::listen(ReqRespCallback&& callback) {
  object_->register_req_resp_callback(this, std::move(callback));

  return true;
}

bool ZenohServerImpl::reply(uint64_t req_id, const Bytes& resp_data, bool is_sync) {
  (void)is_sync;

  return object_->reply(static_cast<uint64_t>(transport_.hash_code), req_id, resp_data);
}

}  // namespace vlink
