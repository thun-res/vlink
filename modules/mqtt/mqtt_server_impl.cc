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

#include "./mqtt_server_impl.h"

#include <utility>

#include "./base/helpers.h"

namespace vlink {

// MqttServerImpl
MqttServerImpl::MqttServerImpl(const MqttConf& conf) : conf_(conf) {}

void MqttServerImpl::init() {
  static auto& factory = MqttFactory::get();

  conf_.hash_code = Helpers::get_hash_code(conf_.event);

  object_ = factory.get_object<Object>(
      {kImplType, conf_.address, conf_.domain, conf_.qos, conf_.fragment, get_all_properties()});

  object_->add_impl(this);

  object_->start_listening();
}

void MqttServerImpl::deinit() {
  detach();

  object_->remove_impl(this);

  if (!object_->has_impl()) {
    object_->stop_listening();
  }
}

const Conf* MqttServerImpl::get_conf() const { return &conf_; }

const AbstractNode* MqttServerImpl::get_abstract_node() const { return object_.get(); }

bool MqttServerImpl::has_clients() const {
  VLOG_W("Function [has_clients] is not supported.");
  return false;
}

bool MqttServerImpl::listen(ReqRespCallback&& callback) {
  object_->register_req_resp_callback(this, std::move(callback));

  return true;
}

bool MqttServerImpl::reply(uint64_t req_id, const Bytes& resp_data, bool is_sync) {
  (void)is_sync;

  return object_->reply(static_cast<uint64_t>(conf_.hash_code), req_id, resp_data);
}

}  // namespace vlink
