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

#include "./extension/someip_serializer.h"

namespace vlink {

// SomeipSetterImpl
SomeipSetterImpl::SomeipSetterImpl(const SomeipTransport& transport) : transport_(transport) {}

void SomeipSetterImpl::init() {
  static auto& factory = SomeipFactory::get();

  object_ = factory.get_object<Object>({kImplType, transport_.service, transport_.instance});

  object_->add_impl(this);

  auto app = object_->app();

  if VUNLIKELY (!app || !object_->is_initialized()) {
    return;
  }

  app->offer_event(transport_.service, transport_.instance, transport_.event, transport_.groups,
                   transport_.field ? someip::event_type_e::ET_FIELD : someip::event_type_e::ET_EVENT);

  object_->start();
}

void SomeipSetterImpl::deinit() {
  auto app = object_->app();

  if (app && object_->is_initialized()) {
    app->stop_offer_event(transport_.service, transport_.instance, transport_.event);
  }

  object_->remove_impl(this);
}

const Transport* SomeipSetterImpl::get_transport() const { return &transport_; }

const AbstractNode* SomeipSetterImpl::get_abstract_node() const { return object_.get(); }

void SomeipSetterImpl::write(const Bytes& msg_data) {
  if VUNLIKELY (msg_data.size() > SomeipSerializer::kMaxPayloadSize) {
    VLOG_E("SomeipSetterImpl: Payload exceeds the protocol limit.");
    return;
  }

  auto app = object_->app();

  if VUNLIKELY (!app || !object_->is_initialized()) {
    return;
  }

  auto payload = msg_data.empty()
                     ? someip::runtime::get()->create_payload()
                     : someip::runtime::get()->create_payload(msg_data.data(), static_cast<uint32_t>(msg_data.size()));

  app->notify(transport_.service, transport_.instance, transport_.event, payload);
}

void SomeipSetterImpl::sync(SyncCallback&& callback) { callback(); }

}  // namespace vlink
