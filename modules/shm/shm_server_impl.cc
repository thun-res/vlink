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

#include "./shm_server_impl.h"

#include <utility>

#include "./base/helpers.h"

namespace vlink {

// ShmServerImpl
ShmServerImpl::ShmServerImpl(const ShmConf& conf) : conf_(conf) {}

void ShmServerImpl::init() {
  static auto& factory = ShmFactory::get();

  conf_.hash_code = Helpers::get_hash_code(conf_.event);

  object_ =
      factory.get_object<Object>({kImplType, conf_.address, conf_.domain, conf_.depth, conf_.history, conf_.wait});

  object_->add_impl(this);
}

void ShmServerImpl::deinit() {
  detach();

  object_->remove_impl(this);
}

bool ShmServerImpl::suspend() { return object_->suspend(); }

bool ShmServerImpl::resume() { return object_->resume(); }

bool ShmServerImpl::is_suspend() const { return object_->is_suspend(); }

bool ShmServerImpl::is_support_loan() const { return true; }

Bytes ShmServerImpl::loan(int64_t size) {
  if VUNLIKELY (is_resp_type && !is_sync_type) {
    return Bytes();
  }

  return object_->loan(static_cast<uint64_t>(conf_.hash_code), size);
}

bool ShmServerImpl::return_loan(const Bytes& bytes) { return object_->release(bytes); }

const Conf* ShmServerImpl::get_conf() const { return &conf_; }

const AbstractNode* ShmServerImpl::get_abstract_node() const { return object_.get(); }

bool ShmServerImpl::has_clients() const { return object_->has_clients(); }

bool ShmServerImpl::listen(ReqRespCallback&& callback) {
  object_->register_req_resp_callback(this, std::move(callback));
  object_->start();

  return true;
}

bool ShmServerImpl::reply(uint64_t req_id, const Bytes& resp_data, bool is_sync) {
  (void)req_id;

  if VUNLIKELY (!is_sync) {
    VLOG_W("Function [reply] is not supported.");
    return false;
  }

  object_->reply(static_cast<uint64_t>(conf_.hash_code), resp_data);

  return true;
}

}  // namespace vlink
