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

#include "./intra_server_impl.h"

#include <utility>

#include "./base/helpers.h"

namespace vlink {

// IntraServerImpl
IntraServerImpl::IntraServerImpl(const IntraConf& conf) : conf_(conf) {
  if (conf.type == "direct") {
    type_ = IntraType::kDirect;
  }
}

void IntraServerImpl::init() {
  static auto& factory = IntraFactory::get();

  conf_.hash_code = Helpers::get_hash_code(conf_.event);

  object_ = factory.get_object<Object>({kImplType, conf_.address, conf_.pipeline, type_});

  if (object_->req_resp_map_is_empty()) {
    object_->add_impl(this);

    object_->traverse_server_connect_callback([](NodeImpl*, const auto& callback) { callback(true); });
  } else {
    object_->add_impl(this);
  }
}

void IntraServerImpl::deinit() {
  object_->remove_impl(this);

  if (object_->req_resp_map_is_empty()) {
    object_->traverse_server_connect_callback([](NodeImpl*, const auto& callback) { callback(false); });
  }
}

bool IntraServerImpl::suspend() {
  has_suspend = true;

  return true;
}

bool IntraServerImpl::resume() {
  has_suspend = false;

  return true;
}

bool IntraServerImpl::is_suspend() const { return has_suspend; }

const Conf* IntraServerImpl::get_conf() const { return &conf_; }

const AbstractNode* IntraServerImpl::get_abstract_node() const { return object_.get(); }

bool IntraServerImpl::attach(class MessageLoop*) {
  VLOG_W("Function [attach] is not supported.");
  return false;
}

bool IntraServerImpl::detach() {
  VLOG_W("Function [detach] is not supported.");
  return false;
}

bool IntraServerImpl::has_clients() const { return !object_->server_connect_map_is_empty(); }

bool IntraServerImpl::listen(ReqRespCallback&& callback) {
  object_->register_req_resp_callback(this, std::move(callback));

  return true;
}

}  // namespace vlink
