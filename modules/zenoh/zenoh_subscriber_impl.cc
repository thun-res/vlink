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

#include "./zenoh_subscriber_impl.h"

#include <utility>

#include "./base/helpers.h"

namespace vlink {

// ZenohSubscriberImpl
ZenohSubscriberImpl::ZenohSubscriberImpl(const ZenohConf& conf) : conf_(conf) { z_internal_null(&getter_token_); }

void ZenohSubscriberImpl::init() {
  static auto& factory = ZenohFactory::get();

  conf_.hash_code = ZenohFactory::get_channel(conf_.event);

  auto properties = ZenohFactory::resolve_properties(conf_, get_all_properties());

  object_ = factory.get_object<Object>(
      {impl_type, conf_.address, conf_.event, conf_.domain, conf_.depth, conf_.qos, conf_.fragment, properties});

  object_->add_impl(this);

  object_->set_latency_and_lost_enabled(is_latency_and_lost_enabled_.load(std::memory_order_acquire));
}

void ZenohSubscriberImpl::deinit() {
  if (object_) {
    object_->undeclare_getter(&getter_token_);
  }

  detach();

  if (object_) {
    object_->remove_impl(this);
  }
}

bool ZenohSubscriberImpl::suspend() { return object_->suspend(); }

bool ZenohSubscriberImpl::resume() { return object_->resume(); }

bool ZenohSubscriberImpl::is_suspend() const { return object_->is_suspend(); }

const Conf* ZenohSubscriberImpl::get_conf() const { return &conf_; }

const AbstractNode* ZenohSubscriberImpl::get_abstract_node() const { return object_.get(); }

bool ZenohSubscriberImpl::listen(MsgCallback&& callback) {
  object_->register_msg_callback(this, std::move(callback));
  object_->subscribe();

  if (impl_type == kGetter) {
    return object_->declare_getter(&getter_token_);
  }

  return true;
}

void ZenohSubscriberImpl::set_latency_and_lost_enabled(bool enable) {
  is_latency_and_lost_enabled_.store(enable, std::memory_order_release);

  if (object_) {
    object_->set_latency_and_lost_enabled(enable);
  }
}

bool ZenohSubscriberImpl::is_latency_and_lost_enabled() const {
  if (object_) {
    return object_->is_latency_and_lost_enabled();
  }

  return is_latency_and_lost_enabled_.load(std::memory_order_acquire);
}

int64_t ZenohSubscriberImpl::get_latency() const {
  if (object_) {
    return object_->get_latency();
  }

  return 0;
}

SampleLostInfo ZenohSubscriberImpl::get_lost() const {
  if (object_ && object_->is_latency_and_lost_enabled()) {
    return SampleLostInfo{object_->get_calculate_sample().get_total(), object_->get_calculate_sample().get_lost()};
  }

  return SampleLostInfo();
}

}  // namespace vlink
