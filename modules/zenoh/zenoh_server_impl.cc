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
ZenohServerImpl::ZenohServerImpl(const ZenohConf& conf) : conf_(conf) {}

void ZenohServerImpl::init() {
  static auto& factory = ZenohFactory::get();

  conf_.hash_code = Helpers::get_hash_code(conf_.event);

  auto properties = get_all_properties();
  conf_.append_properties(properties);

  object_ = factory.get_object<Object>(
      {kImplType, conf_.address, conf_.domain, conf_.depth, conf_.qos, conf_.fragment, properties});

  object_->add_impl(this);
}

void ZenohServerImpl::deinit() {
  detach();

  object_->remove_impl(this);
}

const Conf* ZenohServerImpl::get_conf() const { return &conf_; }

const AbstractNode* ZenohServerImpl::get_abstract_node() const { return object_.get(); }

bool ZenohServerImpl::has_clients() const {
  VLOG_W("Function [has_clients] is not supported.");
  return false;
}

bool ZenohServerImpl::is_support_loan() const { return object_->is_support_loan(); }

Bytes ZenohServerImpl::loan(int64_t size) { return object_->loan(static_cast<uint64_t>(conf_.hash_code), size); }

bool ZenohServerImpl::return_loan(const Bytes& bytes) { return object_->release(bytes); }

bool ZenohServerImpl::listen(ReqRespCallback&& callback) {
  object_->register_req_resp_callback(this, std::move(callback));

  return true;
}

bool ZenohServerImpl::reply(uint64_t req_id, const Bytes& resp_data, bool is_sync) {
  (void)is_sync;

  return object_->reply(static_cast<uint64_t>(conf_.hash_code), req_id, resp_data);
}

}  // namespace vlink
