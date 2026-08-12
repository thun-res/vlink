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

#include "./someip_getter_impl.h"

#include <utility>

namespace vlink {

// SomeipGetterImpl
SomeipGetterImpl::SomeipGetterImpl(const SomeipConf& conf) : conf_(conf) {}

void SomeipGetterImpl::init() {
  static auto& factory = SomeipFactory::get();

  auto properties = factory.resolve_properties(conf_, get_all_properties());

  object_ = factory.get_object<Object>({kImplType, conf_.service, conf_.instance, std::move(properties)});

  object_->add_impl(this);
}

void SomeipGetterImpl::deinit() {
  object_->unsubscribe(conf_.groups);
  object_->remove_impl(this);
}

bool SomeipGetterImpl::suspend() {
  has_suspend = true;

  return true;
}

bool SomeipGetterImpl::resume() {
  has_suspend = false;

  return true;
}

bool SomeipGetterImpl::is_suspend() const { return has_suspend; }

const Conf* SomeipGetterImpl::get_conf() const { return &conf_; }

const AbstractNode* SomeipGetterImpl::get_abstract_node() const { return object_.get(); }

bool SomeipGetterImpl::attach(class MessageLoop*) {
  VLOG_W("Function [attach] is not supported.");
  return false;
}

bool SomeipGetterImpl::detach() {
  VLOG_W("Function [detach] is not supported.");
  return false;
}

bool SomeipGetterImpl::listen(MsgCallback&& callback) {
  object_->register_msg_callback(this, std::move(callback));
  object_->subscribe(conf_.groups);
  object_->start();

  return true;
}

}  // namespace vlink
