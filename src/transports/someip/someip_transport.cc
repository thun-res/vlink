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

#include "./transports/someip_transport.h"

#include <memory>
#include <sstream>
#include <string>

#include "./base/helpers.h"
#include "./base/logger.h"
#include "./impl/transport_plugin_interface.h"
#include "./impl/url.h"
#include "./someip_client_impl.h"
#include "./someip_getter_impl.h"
#include "./someip_publisher_impl.h"
#include "./someip_server_impl.h"
#include "./someip_setter_impl.h"
#include "./someip_subscriber_impl.h"

namespace vlink {

VLINK_DEFINE_GLOBAL_PROPERTY(SomeipTransport)

// SomeipTransport
bool SomeipTransport::load_global_config_file(const std::string& filepath) {
  return SomeipFactory::load_global_config_file(filepath);
}

void SomeipTransport::global_init() { SomeipFactory::get(); }

bool SomeipTransport::parse_protocol(struct Protocol* protocol) {
  if VUNLIKELY (get_impl_type() == kUnknownImplType) {
    VLOG_F("SomeipTransport: Unknown node type, cannot parse protocol.");

    return false;
  }

  if VUNLIKELY (protocol->host.empty() || protocol->path.empty() || protocol->dictionary.empty()) {
    return false;
  }

  service = Helpers::to_int(protocol->host, 0);
  instance = Helpers::to_int(protocol->path, 0);
  method = Helpers::to_int(protocol->dictionary["method"], 0);

  auto groups_str = protocol->dictionary["groups"];

  if (!groups_str.empty()) {
    std::stringstream ss(groups_str);
    std::string substr;

    while (std::getline(ss, substr, '|')) {
      if VLIKELY (!substr.empty()) {
        groups.emplace(Helpers::to_int(substr, 0));
      }
    }

    event = Helpers::to_int(protocol->dictionary["event"], 0);
    field = Helpers::to_int(protocol->dictionary["field"], 0) != 0;
  }

  return true;
}

std::unique_ptr<ServerImpl> SomeipTransport::create_server() const { return std::make_unique<SomeipServerImpl>(*this); }

std::unique_ptr<ClientImpl> SomeipTransport::create_client() const { return std::make_unique<SomeipClientImpl>(*this); }

std::unique_ptr<PublisherImpl> SomeipTransport::create_publisher() const {
  return std::make_unique<SomeipPublisherImpl>(*this);
}

std::unique_ptr<SubscriberImpl> SomeipTransport::create_subscriber() const {
  return std::make_unique<SomeipSubscriberImpl>(*this);
}

std::unique_ptr<SetterImpl> SomeipTransport::create_setter() const { return std::make_unique<SomeipSetterImpl>(*this); }

std::unique_ptr<GetterImpl> SomeipTransport::create_getter() const { return std::make_unique<SomeipGetterImpl>(*this); }

bool SomeipTransport::is_valid() const {
  if VUNLIKELY (service == 0 || instance == 0) {
    return false;
  }

  if VUNLIKELY ((groups.empty() || event == 0) && (get_impl_type() == kPublisher || get_impl_type() == kSubscriber ||
                                                   get_impl_type() == kSetter || get_impl_type() == kGetter)) {
    return false;
  }

  if VUNLIKELY (!field && (get_impl_type() == kSetter || get_impl_type() == kGetter)) {
    return false;
  }

  return true;
}

std::ostream& operator<<(std::ostream& ostream, const SomeipTransport::Groups& groups) noexcept {
  std::ios_base::fmtflags f = ostream.flags();

  int i = 0;

  for (auto group : groups) {
    if VUNLIKELY (i != 0) {
      ostream << "|";
    }

    ostream << "0x" << VLINK_LOG_HEXSS(4) << group;
    ++i;
  }

  ostream.flags(f);

  return ostream;
}

std::ostream& operator<<(std::ostream& ostream, const SomeipTransport& transport) noexcept {
  std::ios_base::fmtflags f = ostream.flags();

  ostream << "SomeipTransport:"
          << "[type]" << +transport.get_impl_type() << std::hex << "[service]0x" << transport.service << "[instance]0x"
          << transport.instance << "[method]0x" << transport.method << "[groups]" << transport.groups << "[event]0x"
          << transport.event << std::dec << "[field]" << transport.field;

  ostream.flags(f);

  return ostream;
}

class TransportPluginSomeip : public TransportPluginInterface {
 protected:
  TransportType get_transport_type() const override { return TransportType::kSomeip; }

  std::unique_ptr<Transport> create() const override { return std::make_unique<SomeipTransport>(); }
};

#ifdef VLINK_LIBRARY
VLINK_PLUGIN_DECLARE(TransportPluginSomeip, 1, 0)
#endif

}  // namespace vlink
