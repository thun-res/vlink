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

#include "./intra_publisher_impl.h"

#include "./base/helpers.h"

namespace vlink {

// IntraPublisherImpl
IntraPublisherImpl::IntraPublisherImpl(const IntraConf& conf) : conf_(conf) {
  if (conf.type == "direct") {
    type_ = IntraType::kDirect;
  }
}

void IntraPublisherImpl::init() {
  static auto& factory = IntraFactory::get();

  conf_.hash_code = Helpers::get_hash_code(conf_.event);

  object_ = factory.get_object<Object>({kImplType, conf_.address, conf_.pipeline, type_});

  object_->add_impl(this);

  object_->register_sub_connect_callback(this, [this](bool) { PublisherImpl::update_subscribers(); });

  PublisherImpl::update_subscribers();
}

void IntraPublisherImpl::deinit() { object_->remove_impl(this); }

const Conf* IntraPublisherImpl::get_conf() const { return &conf_; }

const AbstractNode* IntraPublisherImpl::get_abstract_node() const { return object_.get(); }

bool IntraPublisherImpl::attach(class MessageLoop*) {
  VLOG_W("Function [attach] is not supported.");
  return false;
}

bool IntraPublisherImpl::detach() {
  VLOG_W("Function [detach] is not supported.");
  return false;
}

bool IntraPublisherImpl::has_subscribers() const {
  return !object_->msg_map_is_empty() || !object_->intra_msg_map_is_empty();
}

bool IntraPublisherImpl::write(const Bytes& msg_data) { return object_->publish(type_, conf_.hash_code, msg_data); }

bool IntraPublisherImpl::write(const IntraData& intra_data) {
  return object_->publish(type_, conf_.hash_code, intra_data);
}

}  // namespace vlink
