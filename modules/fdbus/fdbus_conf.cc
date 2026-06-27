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

#include "./modules/fdbus_conf.h"

#include <memory>

#include "./base/logger.h"
#include "./base/utils.h"
#include "./fdbus_client_impl.h"
#include "./fdbus_getter_impl.h"
#include "./fdbus_publisher_impl.h"
#include "./fdbus_server_impl.h"
#include "./fdbus_setter_impl.h"
#include "./fdbus_subscriber_impl.h"
#include "./impl/conf_plugin_interface.h"
#include "./impl/url.h"

namespace vlink {

VLINK_DEFINE_GLOBAL_PROPERTY(FdbusConf)

// FdbusConf
bool FdbusConf::has_name_server() {
#ifdef _WIN32
  return Utils::is_process_running("name_server.exe");
#else
  return Utils::is_process_running("name_server");
#endif
}

void FdbusConf::global_init() { FdbusFactory::get(); }

bool FdbusConf::parse_protocol(struct Protocol* protocol) {
  if VUNLIKELY (get_impl_type() == kUnknownImplType) {
    VLOG_F("FdbusConf: Unknown node type, cannot parse protocol.");

    return false;
  }

  if VUNLIKELY (protocol->host.empty()) {
    return false;
  }

  address = protocol->host + (protocol->path.empty() ? "" : "/" + protocol->path);
  event = protocol->dictionary["event"];

  if (!protocol->fragment.empty()) {
    transport = protocol->fragment;
  }

  return true;
}

bool FdbusConf::is_valid() const {
  if VUNLIKELY (address.empty()) {
    return false;
  }

  if VUNLIKELY (transport != "svc" && transport != "ipc") {
    return false;
  }

  return true;
}

std::unique_ptr<ServerImpl> FdbusConf::create_server() const { return std::make_unique<FdbusServerImpl>(*this); }

std::unique_ptr<ClientImpl> FdbusConf::create_client() const { return std::make_unique<FdbusClientImpl>(*this); }

std::unique_ptr<PublisherImpl> FdbusConf::create_publisher() const {
  return std::make_unique<FdbusPublisherImpl>(*this);
}

std::unique_ptr<SubscriberImpl> FdbusConf::create_subscriber() const {
  return std::make_unique<FdbusSubscriberImpl>(*this);
}

std::unique_ptr<SetterImpl> FdbusConf::create_setter() const { return std::make_unique<FdbusSetterImpl>(*this); }

std::unique_ptr<GetterImpl> FdbusConf::create_getter() const { return std::make_unique<FdbusGetterImpl>(*this); }

std::ostream& operator<<(std::ostream& ostream, const FdbusConf& conf) noexcept {
  std::ios_base::fmtflags f = ostream.flags();

  ostream << "FdbusConf:"
          << "[type]" << +conf.get_impl_type() << "[address]" << conf.address << "[event]" << conf.event
          << "[transport]" << conf.transport;

  ostream.flags(f);

  return ostream;
}

class ConfPluginFdbus : public ConfPluginInterface {
 protected:
  TransportType get_transport_type() const override { return TransportType::kFdbus; }

  std::unique_ptr<Conf> create() const override { return std::make_unique<FdbusConf>(); }
};

#ifdef VLINK_LIBRARY
VLINK_PLUGIN_DECLARE(ConfPluginFdbus, 1, 0)
#endif

}  // namespace vlink
