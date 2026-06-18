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

#include "./shm2_server_impl.h"

#include <utility>

#include "./base/helpers.h"

namespace vlink {

// Shm2ServerImpl
Shm2ServerImpl::Shm2ServerImpl(const Shm2Conf& conf) : conf_(conf) {}

void Shm2ServerImpl::init() {
  static auto& factory = Shm2Factory::get();

  conf_.hash_code = Helpers::get_hash_code(conf_.event);

  object_ = factory.get_object<Object>(
      {kImplType, conf_.address, conf_.domain, conf_.depth, conf_.history, conf_.wait, conf_.size});

  object_->add_impl(this);
}

void Shm2ServerImpl::deinit() {
  detach();

  object_->remove_impl(this);
}

bool Shm2ServerImpl::suspend() { return object_->suspend(); }

bool Shm2ServerImpl::resume() { return object_->resume(); }

bool Shm2ServerImpl::is_suspend() const { return object_->is_suspend(); }

bool Shm2ServerImpl::is_support_loan() const { return true; }

Bytes Shm2ServerImpl::loan(int64_t size) {
  if VUNLIKELY (is_resp_type && !is_sync_type) {
    return Bytes();
  }

  return object_->loan(static_cast<uint64_t>(conf_.hash_code), size);
}

bool Shm2ServerImpl::return_loan(const Bytes& bytes) { return object_->release(bytes); }

const Conf* Shm2ServerImpl::get_conf() const { return &conf_; }

const AbstractNode* Shm2ServerImpl::get_abstract_node() const { return object_.get(); }

bool Shm2ServerImpl::has_clients() const { return object_->has_clients(); }

bool Shm2ServerImpl::listen(ReqRespCallback&& callback) {
  object_->register_req_resp_callback(this, std::move(callback));
  object_->start();

  return true;
}

bool Shm2ServerImpl::reply(uint64_t req_id, const Bytes& resp_data, bool is_sync) {
  (void)req_id;

  if VUNLIKELY (!is_sync) {
    VLOG_W("Function [reply] is not supported.");
    return false;
  }

  object_->reply(static_cast<uint64_t>(conf_.hash_code), resp_data);

  return true;
}

}  // namespace vlink
