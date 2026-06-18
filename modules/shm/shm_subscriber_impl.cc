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

#include "./shm_subscriber_impl.h"

#include <utility>

#include "./base/helpers.h"

namespace vlink {

// ShmSubscriberImpl
ShmSubscriberImpl::ShmSubscriberImpl(const ShmConf& conf) : conf_(conf) {}

void ShmSubscriberImpl::init() {
  static auto& factory = ShmFactory::get();

  conf_.hash_code = Helpers::get_hash_code(conf_.event);

  object_ =
      factory.get_object<Object>({kImplType, conf_.address, conf_.domain, conf_.depth, conf_.history, conf_.wait});

  object_->add_impl(this);

  object_->set_latency_and_lost_enabled(is_latency_and_lost_enabled_);
}

void ShmSubscriberImpl::deinit() {
  detach();

  object_->remove_impl(this);
}

bool ShmSubscriberImpl::suspend() { return object_->suspend(); }

bool ShmSubscriberImpl::resume() { return object_->resume(); }

bool ShmSubscriberImpl::is_suspend() const { return object_->is_suspend(); }

bool ShmSubscriberImpl::is_support_loan() const { return true; }

bool ShmSubscriberImpl::return_loan(const Bytes& bytes) { return object_->release(bytes); }

const Conf* ShmSubscriberImpl::get_conf() const { return &conf_; }

const AbstractNode* ShmSubscriberImpl::get_abstract_node() const { return object_.get(); }

bool ShmSubscriberImpl::listen(MsgCallback&& callback) {
  object_->register_msg_callback(this, std::move(callback));
  object_->subscribe();

  return true;
}

void ShmSubscriberImpl::set_latency_and_lost_enabled(bool enable) {
  is_latency_and_lost_enabled_.store(enable, std::memory_order_release);

  if (object_) {
    object_->set_latency_and_lost_enabled(enable);
  }
}

bool ShmSubscriberImpl::is_latency_and_lost_enabled() const {
  if (object_) {
    return object_->is_latency_and_lost_enabled();
  }

  return is_latency_and_lost_enabled_.load(std::memory_order_acquire);
}

SampleLostInfo ShmSubscriberImpl::get_lost() const {
  if (object_ && object_->is_latency_and_lost_enabled()) {
    return SampleLostInfo{object_->get_calculate_sample().get_total(), object_->get_calculate_sample().get_lost()};
  }

  return SampleLostInfo();
}

}  // namespace vlink
