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

#include "./zenoh_setter_impl.h"

#include "./base/helpers.h"

namespace vlink {

// ZenohSetterImpl
ZenohSetterImpl::ZenohSetterImpl(const ZenohConf& conf) : conf_(conf) {}

void ZenohSetterImpl::init() {
  static auto& factory = ZenohFactory::get();

  conf_.hash_code = Helpers::get_hash_code(conf_.event);

  auto properties = get_all_properties();
  conf_.append_properties(properties);

  object_ = factory.get_object<Object>(
      {kImplType, conf_.address, conf_.domain, conf_.depth, conf_.qos, conf_.fragment, properties});

  object_->add_impl(this);

  object_->check_matching();
}

void ZenohSetterImpl::deinit() {
  detach();

  object_->remove_impl(this);
}

const Conf* ZenohSetterImpl::get_conf() const { return &conf_; }

const AbstractNode* ZenohSetterImpl::get_abstract_node() const { return object_.get(); }

bool ZenohSetterImpl::is_support_loan() const { return object_->is_support_loan(); }

Bytes ZenohSetterImpl::loan(int64_t size) { return object_->loan(static_cast<uint64_t>(conf_.hash_code), size); }

bool ZenohSetterImpl::return_loan(const Bytes& bytes) { return object_->release(bytes); }

void ZenohSetterImpl::write(const Bytes& msg_data) {
  if VUNLIKELY (!object_->has_subscribers()) {
    return;
  }

  object_->publish(static_cast<uint64_t>(conf_.hash_code), msg_data);
}

void ZenohSetterImpl::sync(SyncCallback&& callback) {
  object_->register_sub_connect_callback(this, [callback = std::move(callback)](bool connected) mutable {
    if (connected) {
      callback();
    }
  });
}

}  // namespace vlink
