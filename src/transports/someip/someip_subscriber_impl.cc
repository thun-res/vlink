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

#include "./someip_subscriber_impl.h"

#include <utility>

namespace vlink {

// SomeipSubscriberImpl
SomeipSubscriberImpl::SomeipSubscriberImpl(const SomeipTransport& transport) : transport_(transport) {}

void SomeipSubscriberImpl::init() {
  static auto& factory = SomeipFactory::get();

  object_ = factory.get_object<Object>({kImplType, transport_.service, transport_.instance});

  object_->add_impl(this);

  if (init_impl_type != kGetter) {
    start_subscription();
  }
}

void SomeipSubscriberImpl::deinit() {
  object_->remove_impl(this);

  if (init_impl_type == kGetter && !is_listened) {
    return;
  }

  auto app = object_->app();

  if VUNLIKELY (!app || !object_->is_initialized()) {
    return;
  }

  for (auto g : transport_.groups) {
    bool in_use = false;

    object_->traverse_server_connect_callback([&](NodeImpl* impl, const auto&) {
      if (impl->get_target_transport<SomeipTransport>()->groups.count(g) != 0) {
        in_use = true;
      }
    });

    if (!in_use) {
      app->unsubscribe(transport_.service, transport_.instance, g);
    }
  }

  app->release_event(transport_.service, transport_.instance, transport_.event);
}

bool SomeipSubscriberImpl::suspend() {
  has_suspend = true;

  return true;
}

bool SomeipSubscriberImpl::resume() {
  has_suspend = false;

  return true;
}

bool SomeipSubscriberImpl::is_suspend() const { return has_suspend; }

const Transport* SomeipSubscriberImpl::get_transport() const { return &transport_; }

const AbstractNode* SomeipSubscriberImpl::get_abstract_node() const { return object_.get(); }

bool SomeipSubscriberImpl::listen(MsgCallback&& callback) {
  auto app = object_->app();

  if VUNLIKELY (!app || !object_->is_initialized()) {
    return false;
  }

  object_->register_msg_callback(this, std::move(callback));

  if (init_impl_type == kGetter) {
    start_subscription();
  }

  return true;
}

void SomeipSubscriberImpl::start_subscription() {
  auto app = object_->app();

  if VUNLIKELY (!app || !object_->is_initialized()) {
    return;
  }

  app->request_event(transport_.service, transport_.instance, transport_.event, transport_.groups,
                     transport_.field ? someip::event_type_e::ET_FIELD : someip::event_type_e::ET_EVENT);

  object_->invoke_callback(this, [this, &app]() {
    has_subscribed_ = false;

    if (object_->is_connected()) {
      for (auto g : transport_.groups) {
        app->subscribe(transport_.service, transport_.instance, g);
      }

      has_subscribed_ = true;
    }

    object_->register_server_connect_callback(this, [this](bool connected) {
      if (!has_subscribed_ && connected) {
        auto current_app = object_->app();

        if VLIKELY (current_app && object_->is_initialized()) {
          for (auto g : transport_.groups) {
            current_app->subscribe(transport_.service, transport_.instance, g);
          }
        }
      }

      has_subscribed_ = connected;
    });
  });

  object_->start();
}

}  // namespace vlink
