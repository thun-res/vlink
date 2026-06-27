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

#include "./impl/conf.h"

#include <memory>

#include "./base/logger.h"
#include "./impl/client_impl.h"
#include "./impl/getter_impl.h"
#include "./impl/publisher_impl.h"
#include "./impl/server_impl.h"
#include "./impl/setter_impl.h"
#include "./impl/subscriber_impl.h"

namespace vlink {

// Conf
Conf::~Conf() = default;

bool Conf::parse(ImplType impl_type) const {
  if VUNLIKELY (impl_type == kUnknownImplType) {
    VLOG_F("Conf::parse() called with unknown node type (kUnknownImplType).");
    return false;
  }

  impl_type_ = impl_type;
  return true;
}

bool Conf::is_valid() const { return false; }

ImplType Conf::get_impl_type() const { return impl_type_; }

TransportType Conf::get_transport_type() const { return TransportType::kUnknown; }

Conf::Conf() = default;

bool Conf::parse_protocol(struct Protocol* protocol) {
  (void)protocol;
  return false;
}

std::unique_ptr<ServerImpl> Conf::create_server() const { return nullptr; }

std::unique_ptr<ClientImpl> Conf::create_client() const { return nullptr; }

std::unique_ptr<PublisherImpl> Conf::create_publisher() const { return nullptr; }

std::unique_ptr<SubscriberImpl> Conf::create_subscriber() const { return nullptr; }

std::unique_ptr<SetterImpl> Conf::create_setter() const { return nullptr; }

std::unique_ptr<GetterImpl> Conf::create_getter() const { return nullptr; }

}  // namespace vlink
