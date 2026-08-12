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

#include "./someip_publisher_impl.h"

namespace vlink {

// SomeipPublisherImpl
SomeipPublisherImpl::SomeipPublisherImpl(const SomeipConf& conf) : conf_(conf) {}

void SomeipPublisherImpl::init() {
  static auto& factory = SomeipFactory::get();

  auto properties = factory.resolve_properties(conf_, get_all_properties());

  object_ = factory.get_object<Object>({kImplType, conf_.service, conf_.instance, std::move(properties)});

  object_->add_impl(this);
  object_->offer_event(conf_.event, conf_.groups, conf_.field);
  object_->register_sub_connect_callback(this, [this](bool) { PublisherImpl::update_subscribers(); });
  object_->start();

  PublisherImpl::update_subscribers();
}

void SomeipPublisherImpl::deinit() {
  object_->stop_offer_event(conf_.event, conf_.groups, conf_.field);
  object_->remove_impl(this);
}

const Conf* SomeipPublisherImpl::get_conf() const { return &conf_; }

const AbstractNode* SomeipPublisherImpl::get_abstract_node() const { return object_.get(); }

bool SomeipPublisherImpl::attach(class MessageLoop*) {
  VLOG_W("Function [attach] is not supported.");
  return false;
}

bool SomeipPublisherImpl::detach() {
  VLOG_W("Function [detach] is not supported.");
  return false;
}

bool SomeipPublisherImpl::has_subscribers() const { return object_->has_subscribers(conf_.groups); }

bool SomeipPublisherImpl::write(const Bytes& msg_data) { return object_->publish(conf_.event, msg_data, conf_.field); }

}  // namespace vlink
