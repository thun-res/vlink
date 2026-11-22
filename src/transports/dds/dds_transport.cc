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

#include "./transports/dds_transport.h"

#include <map>
#include <memory>
#include <string>
#include <vector>

#include "./base/helpers.h"
#include "./base/logger.h"
#include "./dds_client_impl.h"
#include "./dds_getter_impl.h"
#include "./dds_publisher_impl.h"
#include "./dds_server_impl.h"
#include "./dds_setter_impl.h"
#include "./dds_subscriber_impl.h"
#include "./impl/transport_plugin_interface.h"
#include "./impl/url.h"
#include "./impl/url_parser.h"

namespace vlink {

VLINK_DEFINE_GLOBAL_PROPERTY(DdsTransport)

std::map<std::string, Function<void*()>> DdsTransport::type_support_map_;
std::map<std::string, Qos> DdsTransport::qos_map_;
std::shared_mutex DdsTransport::mtx_;

// DdsTransport
std::vector<std::tuple<std::string, std::string>> DdsTransport::get_discovered_topics(int32_t _domain) {
  return DdsFactory::get_discovered_topics(_domain);
}

bool DdsTransport::load_global_qos_file(const std::string& filepath) {
  return DdsFactory::load_global_qos_file(filepath);
}

void DdsTransport::register_qos(const std::string& name, const Qos& qos) {
  std::lock_guard lock(mtx_);

  if VUNLIKELY (qos_map_.find(name) != qos_map_.end() || name == "part" || name == "topic" || name == "pub" ||
                name == "sub" || name == "writer" || name == "reader" || name == "depth") {
    VLOG_F("DdsTransport: Invalid qos name: '", name, "' (reserved or already registered).");
  }

  register_qos_internal(name, qos);
}

void DdsTransport::register_qos_internal(const std::string& name, const Qos& qos) { qos_map_[name] = qos; }

Function<void*()> DdsTransport::find_type_support(const std::string& name) {
  std::shared_lock lock(mtx_);
  auto iter = type_support_map_.find(name);

  if (iter != type_support_map_.end()) {
    return iter->second;
  }

  return nullptr;
}

const Qos& DdsTransport::find_qos(const std::string& name) {
  std::shared_lock lock(mtx_);

  auto iter = qos_map_.find(name);

  if VUNLIKELY (iter == qos_map_.end()) {
    CLOG_E("DdsTransport: Invalid qos [%s].", name.c_str());
    static Qos invalid_qos;
    return invalid_qos;
  }

  return iter->second;
}

std::string DdsTransport::get_topic_for_url(const std::string& url) {
  UrlParser parser(url);

  if VUNLIKELY (parser.get_host().empty()) {
    return "";
  }

  return parser.get_host() + (parser.get_path().empty() ? "" : "/" + parser.get_path());
}

void DdsTransport::global_init() { DdsFactory::get(); }

bool DdsTransport::parse_protocol(struct Protocol* protocol) {
  if VUNLIKELY (get_impl_type() == kUnknownImplType) {
    VLOG_F("DdsTransport: Unknown node type, cannot parse protocol.");

    return false;
  }

  if VUNLIKELY (protocol->host.empty()) {
    return false;
  }

  static int default_domain_id = DdsFactory::get_default_domain_id();

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
      VLOG_W("DdsTransport: Unknown qos key: ", key, ".");
    }
  }

  return true;
}

std::unique_ptr<ServerImpl> DdsTransport::create_server() const { return std::make_unique<DdsServerImpl>(*this); }

std::unique_ptr<ClientImpl> DdsTransport::create_client() const { return std::make_unique<DdsClientImpl>(*this); }

std::unique_ptr<PublisherImpl> DdsTransport::create_publisher() const {
  return std::make_unique<DdsPublisherImpl>(*this);
}

std::unique_ptr<SubscriberImpl> DdsTransport::create_subscriber() const {
  return std::make_unique<DdsSubscriberImpl>(*this);
}

std::unique_ptr<SetterImpl> DdsTransport::create_setter() const { return std::make_unique<DdsSetterImpl>(*this); }

std::unique_ptr<GetterImpl> DdsTransport::create_getter() const { return std::make_unique<DdsGetterImpl>(*this); }

bool DdsTransport::is_valid() const {
  if VUNLIKELY (domain < 0 || topic.empty()) {
    return false;
  }

  if VUNLIKELY (!qos.empty() && !qos_ext.empty()) {
    return false;
  }

  return true;
}

std::ostream& operator<<(std::ostream& ostream, const Transport::PropertiesMap& qos) noexcept {
  std::ios_base::fmtflags f = ostream.flags();

  for (const auto& [key, value] : qos) {
    ostream << "(" << key << ")" << value;
  }

  ostream.flags(f);

  return ostream;
}

std::ostream& operator<<(std::ostream& ostream, const DdsTransport& transport) noexcept {
  std::ios_base::fmtflags f = ostream.flags();

  ostream << "DdsTransport:"
          << "[type]" << +transport.get_impl_type() << "[topic]" << transport.topic << "[domain]" << transport.domain
          << "[depth]" << transport.depth << "[qos]" << transport.qos << "[qos_ext]" << transport.qos_ext;

  ostream.flags(f);

  return ostream;
}

class TransportPluginDds : public TransportPluginInterface {
 protected:
  TransportType get_transport_type() const override { return TransportType::kDds; }

  std::unique_ptr<Transport> create() const override { return std::make_unique<DdsTransport>(); }
};

#ifdef VLINK_LIBRARY
VLINK_PLUGIN_DECLARE(TransportPluginDds, 1, 0)
#endif

}  // namespace vlink
