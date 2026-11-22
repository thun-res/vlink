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

#include <memory>

#include "./extension/someip_serializer.h"

#ifdef VSOMEIP_DEPRECATED_UID_GID
#define VSOMEIP_SUB_HANDLE_ARG const vsomeip_sec_client_t*, const std::string&
#else
#define VSOMEIP_SUB_HANDLE_ARG someip::uid_t, someip::gid_t
#endif

namespace vlink {

// SomeipPublisherImpl
SomeipPublisherImpl::SomeipPublisherImpl(const SomeipTransport& transport) : transport_(transport) {}

void SomeipPublisherImpl::init() {
  static auto& factory = SomeipFactory::get();

  object_ = factory.get_object<Object>({kImplType, transport_.service, transport_.instance});

  object_->add_impl(this);
  object_->register_sub_connect_callback(this, [this](bool) { PublisherImpl::update_subscribers(); });

  auto app = object_->app();

  if VUNLIKELY (!app || !object_->is_initialized()) {
    return;
  }

  app->offer_event(transport_.service, transport_.instance, transport_.event, transport_.groups,
                   transport_.field ? someip::event_type_e::ET_FIELD : someip::event_type_e::ET_EVENT);

  for (auto g : transport_.groups) {
    app->register_subscription_handler(
        transport_.service, transport_.instance, g,
        [object = object_.get(), g](someip::client_t client_id, VSOMEIP_SUB_HANDLE_ARG, bool is_reg) {
          {
            std::lock_guard lock(object->get_client_mtx());
            auto& clients = object->get_clients()[g];

            if (is_reg) {
              clients.emplace(client_id);
            } else {
              clients.erase(client_id);
            }
          }

          object->traverse_sub_connect_callback([is_reg](NodeImpl*, const auto& callback) { callback(is_reg); });

          return is_reg;
        });
  }

  object_->start();

  PublisherImpl::update_subscribers();
}

void SomeipPublisherImpl::deinit() {
  object_->remove_impl(this);

  auto app = object_->app();
  const bool app_ready = app && object_->is_initialized();

  for (auto g : transport_.groups) {
    bool in_use = false;

    object_->traverse_sub_connect_callback([&](NodeImpl* impl, const auto&) {
      if (impl->get_target_transport<SomeipTransport>()->groups.count(g) != 0) {
        in_use = true;
      }
    });

    if (!in_use) {
      if (app_ready) {
        app->unregister_subscription_handler(transport_.service, transport_.instance, g);
      }

      std::lock_guard lock(object_->get_client_mtx());
      object_->get_clients().erase(g);
    }
  }

  if (app_ready) {
    app->stop_offer_event(transport_.service, transport_.instance, transport_.event);
  }
}

const Transport* SomeipPublisherImpl::get_transport() const { return &transport_; }

const AbstractNode* SomeipPublisherImpl::get_abstract_node() const { return object_.get(); }

bool SomeipPublisherImpl::has_subscribers() const {
  std::lock_guard lock(object_->get_client_mtx());

  const auto& clients = object_->get_clients();

  for (auto g : transport_.groups) {
    auto iter = clients.find(g);

    if (iter != clients.end() && !iter->second.empty()) {
      return true;
    }
  }

  return false;
}

bool SomeipPublisherImpl::write(const Bytes& msg_data) {
  if VUNLIKELY (msg_data.size() > SomeipSerializer::kMaxPayloadSize) {
    VLOG_E("SomeipPublisherImpl: Payload exceeds the protocol limit.");
    return false;
  }

  auto app = object_->app();

  if VUNLIKELY (!app || !object_->is_initialized()) {
    return false;
  }

  auto payload = msg_data.empty()
                     ? someip::runtime::get()->create_payload()
                     : someip::runtime::get()->create_payload(msg_data.data(), static_cast<uint32_t>(msg_data.size()));

  app->notify(transport_.service, transport_.instance, transport_.event, payload);

  return true;
}

}  // namespace vlink
