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

#include "./transports/intra_transport.h"

#include <memory>

#include "./base/helpers.h"
#include "./base/logger.h"
#include "./impl/transport_plugin_interface.h"
#include "./impl/url.h"
#include "./intra_client_impl.h"
#include "./intra_getter_impl.h"
#include "./intra_publisher_impl.h"
#include "./intra_server_impl.h"
#include "./intra_setter_impl.h"
#include "./intra_subscriber_impl.h"

namespace vlink {

VLINK_DEFINE_GLOBAL_PROPERTY(IntraTransport)

// IntraTransport
void IntraTransport::global_init() { IntraFactory::get(); }

bool IntraTransport::parse_protocol(struct Protocol* protocol) {
  if VUNLIKELY (get_impl_type() == kUnknownImplType) {
    VLOG_F("IntraTransport: Unknown node type, cannot parse protocol.");

    return false;
  }

  if VUNLIKELY (protocol->host.empty()) {
    return false;
  }

  address = protocol->host + (protocol->path.empty() ? "" : "/" + protocol->path);
  event = protocol->dictionary["event"];
  pipeline = Helpers::to_int(protocol->dictionary["pipeline"], 0);

  if (!protocol->fragment.empty()) {
    type = protocol->fragment;
  }

  return true;
}

std::unique_ptr<ServerImpl> IntraTransport::create_server() const { return std::make_unique<IntraServerImpl>(*this); }

std::unique_ptr<ClientImpl> IntraTransport::create_client() const { return std::make_unique<IntraClientImpl>(*this); }

std::unique_ptr<PublisherImpl> IntraTransport::create_publisher() const {
  return std::make_unique<IntraPublisherImpl>(*this);
}

std::unique_ptr<SubscriberImpl> IntraTransport::create_subscriber() const {
  return std::make_unique<IntraSubscriberImpl>(*this);
}

std::unique_ptr<SetterImpl> IntraTransport::create_setter() const { return std::make_unique<IntraSetterImpl>(*this); }

std::unique_ptr<GetterImpl> IntraTransport::create_getter() const { return std::make_unique<IntraGetterImpl>(*this); }

bool IntraTransport::is_valid() const {
  if VUNLIKELY (address.empty()) {
    return false;
  }

  if VUNLIKELY (type != "queue" && type != "direct") {
    return false;
  }

  return true;
}

std::ostream& operator<<(std::ostream& ostream, const IntraTransport& transport) noexcept {
  std::ios_base::fmtflags f = ostream.flags();

  ostream << "IntraTransport:"
          << "[type]" << +transport.get_impl_type() << "[address]" << transport.address << "[event]" << transport.event
          << "[pipeline]" << transport.pipeline << "[type]" << transport.type;

  ostream.flags(f);

  return ostream;
}

class TransportPluginIntra : public TransportPluginInterface {
 protected:
  TransportType get_transport_type() const override { return TransportType::kIntra; }

  std::unique_ptr<Transport> create() const override { return std::make_unique<IntraTransport>(); }
};

#ifdef VLINK_LIBRARY
VLINK_PLUGIN_DECLARE(TransportPluginIntra, 1, 0)
#endif

}  // namespace vlink
