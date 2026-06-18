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

#include "./someip_setter_impl.h"

namespace vlink {

// SomeipSetterImpl
SomeipSetterImpl::SomeipSetterImpl(const SomeipConf& conf) : conf_(conf) {}

void SomeipSetterImpl::init() {
  static auto& factory = SomeipFactory::get();

  object_ = factory.get_object<Object>({kImplType, conf_.service, conf_.instance});

  object_->add_impl(this);

  object_->app()->offer_event(conf_.service, conf_.instance, conf_.event, conf_.groups,
                              conf_.field ? someip::event_type_e::ET_FIELD : someip::event_type_e::ET_EVENT);

  object_->start();
}

void SomeipSetterImpl::deinit() {
  object_->app()->stop_offer_event(conf_.service, conf_.instance, conf_.event);

  object_->remove_impl(this);
}

const Conf* SomeipSetterImpl::get_conf() const { return &conf_; }

const AbstractNode* SomeipSetterImpl::get_abstract_node() const { return object_.get(); }

bool SomeipSetterImpl::attach(class MessageLoop*) {
  VLOG_W("Function [attach] is not supported.");
  return false;
}

bool SomeipSetterImpl::detach() {
  VLOG_W("Function [detach] is not supported.");
  return false;
}

void SomeipSetterImpl::write(const Bytes& msg_data) {
  auto payload = someip::runtime::get()->create_payload(msg_data.data(), msg_data.size());

  object_->app()->notify(conf_.service, conf_.instance, conf_.event, payload);
}

void SomeipSetterImpl::sync(SyncCallback&& callback) { (void)callback; }

}  // namespace vlink
