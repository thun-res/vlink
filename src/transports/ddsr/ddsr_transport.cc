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

#include "./transports/ddsr_transport.h"

#include <map>
#include <memory>
#include <string>

#include "./base/helpers.h"
#include "./base/logger.h"
#include "./ddsr_client_impl.hpp"
#include "./ddsr_getter_impl.hpp"
#include "./ddsr_publisher_impl.hpp"
#include "./ddsr_server_impl.hpp"
#include "./ddsr_setter_impl.hpp"
#include "./ddsr_subscriber_impl.hpp"
#include "./impl/transport_plugin_interface.h"
#include "./impl/url.h"

namespace vlink {

VLINK_DEFINE_GLOBAL_PROPERTY(DdsrTransport)

std::map<std::string, Qos> DdsrTransport::qos_map_;
std::shared_mutex DdsrTransport::mtx_;

// DdsrTransport
void DdsrTransport::register_qos(const std::string& name, const Qos& qos) {
  std::lock_guard lock(mtx_);

  if VUNLIKELY (qos_map_.find(name) != qos_map_.end() || name == "part" || name == "topic" || name == "pub" ||
                name == "sub" || name == "writer" || name == "reader" || name == "depth") {
    VLOG_F("DdsrTransport: Invalid qos name: '", name, "' (reserved or already registered).");
  }

  register_qos_internal(name, qos);
}

void DdsrTransport::register_qos_internal(const std::string& name, const Qos& qos) { qos_map_[name] = qos; }

const Qos& DdsrTransport::find_qos(const std::string& name) {
  std::shared_lock lock(mtx_);

  auto iter = qos_map_.find(name);

  if VUNLIKELY (iter == qos_map_.end()) {
    CLOG_E("DdsrTransport: Invalid qos [%s].", name.c_str());
    static Qos invalid_qos;
    return invalid_qos;
  }

  return iter->second;
}

void DdsrTransport::global_init() { DdsrFactory::get(); }

bool DdsrTransport::parse_protocol(struct Protocol* protocol) {
  if VUNLIKELY (get_impl_type() == kUnknownImplType) {
    VLOG_F("DdsrTransport: Unknown node type, cannot parse protocol.");

    return false;
  }

  if VUNLIKELY (protocol->host.empty()) {
    return false;
  }

  static int default_domain_id = DdsrFactory::get_default_domain_id();

  topic = protocol->host + (protocol->path.empty() ? "" : "/" + protocol->path);
  domain = Helpers::to_int(protocol->dictionary["domain"], default_domain_id);
  depth = Helpers::to_int(protocol->dictionary["depth"]);
  qos = protocol->dictionary["qos"];
  qos_ext = protocol->dictionary;
  qos_ext.erase("domain");
  qos_ext.erase("depth");
  qos_ext.erase("qos");

  for (const auto& [key, value] : qos_ext) {
    if VUNLIKELY (key != "part" && key != "topic" && key != "pub" && key != "sub" && key != "writer" &&
                  key != "reader") {
      VLOG_W("DdsrTransport: Unknown qos key: ", key, ".");
    }
  }

  return true;
}

std::unique_ptr<ServerImpl> DdsrTransport::create_server() const { return std::make_unique<DdsrServerImpl>(*this); }

std::unique_ptr<ClientImpl> DdsrTransport::create_client() const { return std::make_unique<DdsrClientImpl>(*this); }

std::unique_ptr<PublisherImpl> DdsrTransport::create_publisher() const {
  return std::make_unique<DdsrPublisherImpl>(*this);
}

std::unique_ptr<SubscriberImpl> DdsrTransport::create_subscriber() const {
  return std::make_unique<DdsrSubscriberImpl>(*this);
}

std::unique_ptr<SetterImpl> DdsrTransport::create_setter() const { return std::make_unique<DdsrSetterImpl>(*this); }

std::unique_ptr<GetterImpl> DdsrTransport::create_getter() const { return std::make_unique<DdsrGetterImpl>(*this); }

bool DdsrTransport::is_valid() const {
  if VUNLIKELY (domain < 0 || topic.empty()) {
    return false;
  }

  if VUNLIKELY (!qos.empty() && !qos_ext.empty()) {
    return false;
  }

  return true;
}

std::ostream& operator<<(std::ostream& ostream, const DdsrTransport& transport) noexcept {
  std::ios_base::fmtflags f = ostream.flags();

  ostream << "DdsrTransport:"
          << "[type]" << +transport.get_impl_type() << "[topic]" << transport.topic << "[domain]" << transport.domain
          << "[depth]" << transport.depth << "[qos]" << transport.qos;

  ostream.flags(f);

  return ostream;
}

class TransportPluginDdsr : public TransportPluginInterface {
 protected:
  TransportType get_transport_type() const override { return TransportType::kDdsr; }

  std::unique_ptr<Transport> create() const override { return std::make_unique<DdsrTransport>(); }
};

#ifdef VLINK_LIBRARY
VLINK_PLUGIN_DECLARE(TransportPluginDdsr, 1, 0)
#endif

}  // namespace vlink
