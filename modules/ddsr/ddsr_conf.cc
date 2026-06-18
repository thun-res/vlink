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

#include "./modules/ddsr_conf.h"

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
#include "./impl/conf_plugin_interface.h"
#include "./impl/url.h"

namespace vlink {

VLINK_DEFINE_GLOBAL_PROPERTY(DdsrConf)

std::map<std::string, Qos> DdsrConf::qos_map_;
std::shared_mutex DdsrConf::mtx_;

// DdsrConf
void DdsrConf::register_qos(const std::string& name, const Qos& qos) {
  std::lock_guard lock(mtx_);

  if VUNLIKELY (qos_map_.find(name) != qos_map_.end() || name == "part" || name == "topic" || name == "pub" ||
                name == "sub" || name == "writer" || name == "reader" || name == "depth") {
    VLOG_F("DdsrConf: Invalid qos name: '", name, "' (reserved or already registered).");
  }

  register_qos_internal(name, qos);
}

void DdsrConf::register_qos_internal(const std::string& name, const Qos& qos) { qos_map_[name] = qos; }

const Qos& DdsrConf::find_qos(const std::string& name) {
  std::shared_lock lock(mtx_);

  auto iter = qos_map_.find(name);

  if VUNLIKELY (iter == qos_map_.end()) {
    CLOG_E("DdsrConf: Invalid qos [%s].", name.c_str());
    static Qos invalid_qos;
    return invalid_qos;
  }

  return iter->second;
}

void DdsrConf::global_init() { DdsrFactory::get(); }

bool DdsrConf::parse_protocol(struct Protocol* protocol) {
  if VUNLIKELY (get_impl_type() == kUnknownImplType) {
    VLOG_F("DdsrConf: Unknown node type, cannot parse protocol.");

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
      VLOG_W("DdsrConf: Unknown qos key: ", key, ".");
    }
  }

  return true;
}

bool DdsrConf::is_valid() const {
  if VUNLIKELY (domain < 0 || topic.empty()) {
    return false;
  }

  if VUNLIKELY (!qos.empty() && !qos_ext.empty()) {
    return false;
  }

  return true;
}

std::unique_ptr<ServerImpl> DdsrConf::create_server() const { return std::make_unique<DdsrServerImpl>(*this); }

std::unique_ptr<ClientImpl> DdsrConf::create_client() const { return std::make_unique<DdsrClientImpl>(*this); }

std::unique_ptr<PublisherImpl> DdsrConf::create_publisher() const { return std::make_unique<DdsrPublisherImpl>(*this); }

std::unique_ptr<SubscriberImpl> DdsrConf::create_subscriber() const {
  return std::make_unique<DdsrSubscriberImpl>(*this);
}

std::unique_ptr<SetterImpl> DdsrConf::create_setter() const { return std::make_unique<DdsrSetterImpl>(*this); }

std::unique_ptr<GetterImpl> DdsrConf::create_getter() const { return std::make_unique<DdsrGetterImpl>(*this); }

std::ostream& operator<<(std::ostream& ostream, const DdsrConf& conf) noexcept {
  std::ios_base::fmtflags f = ostream.flags();

  ostream << "DdsrConf:"
          << "[type]" << +conf.get_impl_type() << "[topic]" << conf.topic << "[domain]" << conf.domain << "[depth]"
          << conf.depth << "[qos]" << conf.qos;

  ostream.flags(f);

  return ostream;
}

class ConfPluginDdsr : public ConfPluginInterface {
 protected:
  TransportType get_transport_type() const override { return TransportType::kDdsr; }

  std::unique_ptr<Conf> create() const override { return std::make_unique<DdsrConf>(); }
};

#ifdef VLINK_LIBRARY
VLINK_PLUGIN_DECLARE(ConfPluginDdsr, 1, 0)
#endif

}  // namespace vlink
