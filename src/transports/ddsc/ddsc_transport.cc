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

#include "./transports/ddsc_transport.h"

#include <map>
#include <memory>
#include <string>

#include "./base/helpers.h"
#include "./base/logger.h"
#include "./ddsc_client_impl.hpp"
#include "./ddsc_getter_impl.hpp"
#include "./ddsc_publisher_impl.hpp"
#include "./ddsc_server_impl.hpp"
#include "./ddsc_setter_impl.hpp"
#include "./ddsc_subscriber_impl.hpp"
#include "./impl/transport_plugin_interface.h"
#include "./impl/url.h"

namespace vlink {

VLINK_DEFINE_GLOBAL_PROPERTY(DdscTransport)

std::map<std::string, Qos> DdscTransport::qos_map_;
std::shared_mutex DdscTransport::mtx_;

// DdscTransport
void DdscTransport::register_qos(const std::string& name, const Qos& qos) {
  std::lock_guard lock(mtx_);

  if VUNLIKELY (qos_map_.find(name) != qos_map_.end() || name == "part" || name == "topic" || name == "pub" ||
                name == "sub" || name == "writer" || name == "reader" || name == "depth") {
    VLOG_F("DdscTransport: Invalid qos name: '", name, "' (reserved or already registered).");
  }

  register_qos_internal(name, qos);
}

void DdscTransport::register_qos_internal(const std::string& name, const Qos& qos) { qos_map_[name] = qos; }

const Qos& DdscTransport::find_qos(const std::string& name) {
  std::shared_lock lock(mtx_);

  auto iter = qos_map_.find(name);

  if VUNLIKELY (iter == qos_map_.end()) {
    CLOG_E("DdscTransport: Invalid qos [%s].", name.c_str());
    static Qos invalid_qos;
    return invalid_qos;
  }

  return iter->second;
}

void DdscTransport::global_init() { DdscFactory::get(); }

bool DdscTransport::parse_protocol(struct Protocol* protocol) {
  if VUNLIKELY (get_impl_type() == kUnknownImplType) {
    VLOG_F("DdscTransport: Unknown node type, cannot parse protocol.");
    return false;
  }

  if VUNLIKELY (protocol->host.empty()) {
    return false;
  }

  static int default_domain_id = DdscFactory::get_default_domain_id();

  topic = protocol->host + (protocol->path.empty() ? "" : "/" + protocol->path);
  domain = Helpers::to_int(protocol->dictionary["domain"], default_domain_id);
  depth = Helpers::to_int(protocol->dictionary["depth"]);
  qos = protocol->dictionary["qos"];

  return true;
}

std::unique_ptr<ServerImpl> DdscTransport::create_server() const { return std::make_unique<DdscServerImpl>(*this); }

std::unique_ptr<ClientImpl> DdscTransport::create_client() const { return std::make_unique<DdscClientImpl>(*this); }

std::unique_ptr<PublisherImpl> DdscTransport::create_publisher() const {
  return std::make_unique<DdscPublisherImpl>(*this);
}

std::unique_ptr<SubscriberImpl> DdscTransport::create_subscriber() const {
  return std::make_unique<DdscSubscriberImpl>(*this);
}

std::unique_ptr<SetterImpl> DdscTransport::create_setter() const { return std::make_unique<DdscSetterImpl>(*this); }

std::unique_ptr<GetterImpl> DdscTransport::create_getter() const { return std::make_unique<DdscGetterImpl>(*this); }

bool DdscTransport::is_valid() const {
  if VUNLIKELY (domain < 0 || topic.empty()) {
    return false;
  }

  return true;
}

std::ostream& operator<<(std::ostream& ostream, const DdscTransport& transport) noexcept {
  std::ios_base::fmtflags f = ostream.flags();

  ostream << "DdscTransport:"
          << "[type]" << +transport.get_impl_type() << "[topic]" << transport.topic << "[domain]" << transport.domain
          << "[depth]" << transport.depth << "[qos]" << transport.qos;

  ostream.flags(f);

  return ostream;
}

class TransportPluginDdsc : public TransportPluginInterface {
 protected:
  TransportType get_transport_type() const override { return TransportType::kDdsc; }

  std::unique_ptr<Transport> create() const override { return std::make_unique<DdscTransport>(); }
};

#ifdef VLINK_LIBRARY
VLINK_PLUGIN_DECLARE(TransportPluginDdsc, 1, 0)
#endif

}  // namespace vlink
