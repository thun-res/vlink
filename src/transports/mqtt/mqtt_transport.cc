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

#include "./transports/mqtt_transport.h"

#include <memory>
#include <string>

#include "./base/helpers.h"
#include "./base/logger.h"
#include "./impl/transport_plugin_interface.h"
#include "./impl/url.h"
#include "./mqtt_client_impl.h"
#include "./mqtt_getter_impl.h"
#include "./mqtt_publisher_impl.h"
#include "./mqtt_server_impl.h"
#include "./mqtt_setter_impl.h"
#include "./mqtt_subscriber_impl.h"

namespace vlink {

VLINK_DEFINE_GLOBAL_PROPERTY(MqttTransport)

// MqttTransport
void MqttTransport::global_init() { MqttFactory::get(); }

bool MqttTransport::parse_protocol(struct Protocol* protocol) {
  if VUNLIKELY (get_impl_type() == kUnknownImplType) {
    VLOG_F("MqttTransport: Unknown node type, cannot parse protocol.");
    return false;
  }

  if VUNLIKELY (protocol->host.empty()) {
    return false;
  }

  static int default_domain_id = MqttFactory::get_default_domain_id();
  static int default_qos = MqttFactory::get_default_qos();

  address = protocol->host + (protocol->path.empty() ? "" : "/" + protocol->path);
  event = protocol->dictionary["event"];
  domain = Helpers::to_int(protocol->dictionary["domain"], default_domain_id);
  qos = Helpers::to_int(protocol->dictionary["qos"], default_qos);
  fragment = protocol->fragment;

  return true;
}

std::unique_ptr<ServerImpl> MqttTransport::create_server() const { return std::make_unique<MqttServerImpl>(*this); }

std::unique_ptr<ClientImpl> MqttTransport::create_client() const { return std::make_unique<MqttClientImpl>(*this); }

std::unique_ptr<PublisherImpl> MqttTransport::create_publisher() const {
  return std::make_unique<MqttPublisherImpl>(*this);
}

std::unique_ptr<SubscriberImpl> MqttTransport::create_subscriber() const {
  return std::make_unique<MqttSubscriberImpl>(*this);
}

std::unique_ptr<SetterImpl> MqttTransport::create_setter() const { return std::make_unique<MqttSetterImpl>(*this); }

std::unique_ptr<GetterImpl> MqttTransport::create_getter() const { return std::make_unique<MqttGetterImpl>(*this); }

bool MqttTransport::is_valid() const {
  if VUNLIKELY (domain < 0 || address.empty()) {
    return false;
  }

  if VUNLIKELY (qos < 0 || qos > 2) {
    return false;
  }

  return true;
}

std::ostream& operator<<(std::ostream& ostream, const MqttTransport& transport) noexcept {
  std::ios_base::fmtflags f = ostream.flags();

  ostream << "MqttTransport:"
          << "[type]" << +transport.get_impl_type() << "[address]" << transport.address << "[event]" << transport.event
          << "[domain]" << transport.domain << "[qos]" << transport.qos << "[fragment]" << transport.fragment;

  ostream.flags(f);

  return ostream;
}

class TransportPluginMqtt : public TransportPluginInterface {
 protected:
  TransportType get_transport_type() const override { return TransportType::kMqtt; }

  std::unique_ptr<Transport> create() const override { return std::make_unique<MqttTransport>(); }
};

#ifdef VLINK_LIBRARY
VLINK_PLUGIN_DECLARE(TransportPluginMqtt, 1, 0)
#endif

}  // namespace vlink
