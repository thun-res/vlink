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

#include "./modules/ddst_conf.h"

#include <map>
#include <memory>
#include <string>
#include <vector>

#include "./base/helpers.h"
#include "./base/logger.h"
#include "./ddst_client_impl.h"
#include "./ddst_getter_impl.h"
#include "./ddst_publisher_impl.h"
#include "./ddst_server_impl.h"
#include "./ddst_setter_impl.h"
#include "./ddst_subscriber_impl.h"
#include "./impl/conf_plugin_interface.h"
#include "./impl/url.h"

namespace vlink {

VLINK_DEFINE_GLOBAL_PROPERTY(DdstConf)

std::map<std::string, Function<void*()>> DdstConf::type_support_map_;
std::map<std::string, Qos> DdstConf::qos_map_;
std::shared_mutex DdstConf::mtx_;

// DdstConf
std::vector<std::tuple<std::string, std::string>> DdstConf::get_discovered_topics(int32_t _domain) {
  return DdstFactory::get_discovered_topics(_domain);
}

bool DdstConf::load_global_qos_file(const std::string& filepath) { return DdstFactory::load_global_qos_file(filepath); }

void DdstConf::register_qos(const std::string& name, const Qos& qos) {
  std::lock_guard lock(mtx_);

  if VUNLIKELY (qos_map_.find(name) != qos_map_.end() || name == "part" || name == "topic" || name == "pub" ||
                name == "sub" || name == "writer" || name == "reader" || name == "depth") {
    VLOG_F("DdstConf: Invalid qos name: '", name, "' (reserved or already registered).");
  }

  register_qos_internal(name, qos);
}

void DdstConf::register_qos_internal(const std::string& name, const Qos& qos) { qos_map_[name] = qos; }

const Qos& DdstConf::find_qos(const std::string& name) {
  std::shared_lock lock(mtx_);

  auto iter = qos_map_.find(name);

  if VUNLIKELY (iter == qos_map_.end()) {
    CLOG_E("DdstConf: Invalid qos [%s].", name.c_str());
    static Qos invalid_qos;
    return invalid_qos;
  }

  return iter->second;
}

void DdstConf::global_init() { DdstFactory::get(); }

bool DdstConf::parse_protocol(struct Protocol* protocol) {
  if VUNLIKELY (get_impl_type() == kUnknownImplType) {
    VLOG_F("DdstConf: Unknown node type, cannot parse protocol.");

    return false;
  }

  if VUNLIKELY (protocol->host.empty()) {
    return false;
  }

  static int default_domain_id = DdstFactory::get_default_domain_id();

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
      VLOG_W("DdstConf: Unknown qos key: ", key, ".");
    }
  }

  return true;
}

bool DdstConf::is_valid() const {
  if VUNLIKELY (domain < 0 || topic.empty()) {
    return false;
  }

  if VUNLIKELY (!qos.empty() && !qos_ext.empty()) {
    return false;
  }

  return true;
}

std::unique_ptr<ServerImpl> DdstConf::create_server() const { return std::make_unique<DdstServerImpl>(*this); }

std::unique_ptr<ClientImpl> DdstConf::create_client() const { return std::make_unique<DdstClientImpl>(*this); }

std::unique_ptr<PublisherImpl> DdstConf::create_publisher() const { return std::make_unique<DdstPublisherImpl>(*this); }

std::unique_ptr<SubscriberImpl> DdstConf::create_subscriber() const {
  return std::make_unique<DdstSubscriberImpl>(*this);
}

std::unique_ptr<SetterImpl> DdstConf::create_setter() const { return std::make_unique<DdstSetterImpl>(*this); }

std::unique_ptr<GetterImpl> DdstConf::create_getter() const { return std::make_unique<DdstGetterImpl>(*this); }

std::ostream& operator<<(std::ostream& ostream, const Conf::PropertiesMap& qos) noexcept {
  std::ios_base::fmtflags f = ostream.flags();

  for (const auto& [key, value] : qos) {
    ostream << "(" << key << ")" << value;
  }

  ostream.flags(f);

  return ostream;
}

std::ostream& operator<<(std::ostream& ostream, const DdstConf& conf) noexcept {
  std::ios_base::fmtflags f = ostream.flags();

  ostream << "DdstConf:"
          << "[type]" << +conf.get_impl_type() << "[topic]" << conf.topic << "[domain]" << conf.domain << "[depth]"
          << conf.depth << "[qos]" << conf.qos << "[qos_ext]" << conf.qos_ext;

  ostream.flags(f);

  return ostream;
}

class ConfPluginDdst : public ConfPluginInterface {
 protected:
  TransportType get_transport_type() const override { return TransportType::kDdst; }

  std::unique_ptr<Conf> create() const override { return std::make_unique<DdstConf>(); }
};

#ifdef VLINK_LIBRARY
VLINK_PLUGIN_DECLARE(ConfPluginDdst, 1, 0)
#endif

}  // namespace vlink
