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

#include "./modules/ddsc_conf.h"

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
#include "./impl/conf_plugin_interface.h"
#include "./impl/url.h"

namespace vlink {

VLINK_DEFINE_GLOBAL_PROPERTY(DdscConf)

std::map<std::string, Qos> DdscConf::qos_map_;
std::shared_mutex DdscConf::mtx_;

// DdscConf
void DdscConf::register_qos(const std::string& name, const Qos& qos) {
  std::lock_guard lock(mtx_);

  if VUNLIKELY (qos_map_.find(name) != qos_map_.end() || name == "part" || name == "topic" || name == "pub" ||
                name == "sub" || name == "writer" || name == "reader" || name == "depth") {
    VLOG_F("DdscConf: Invalid qos name: '", name, "' (reserved or already registered).");
  }

  register_qos_internal(name, qos);
}

void DdscConf::register_qos_internal(const std::string& name, const Qos& qos) { qos_map_[name] = qos; }

const Qos& DdscConf::find_qos(const std::string& name) {
  std::shared_lock lock(mtx_);

  auto iter = qos_map_.find(name);

  if VUNLIKELY (iter == qos_map_.end()) {
    CLOG_E("DdscConf: Invalid qos [%s].", name.c_str());
    static Qos invalid_qos;
    return invalid_qos;
  }

  return iter->second;
}

void DdscConf::global_init() { DdscFactory::get(); }

bool DdscConf::parse_protocol(struct Protocol* protocol) {
  if VUNLIKELY (get_impl_type() == kUnknownImplType) {
    VLOG_F("DdscConf: Unknown node type, cannot parse protocol.");
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

bool DdscConf::is_valid() const {
  if VUNLIKELY (domain < 0 || topic.empty()) {
    return false;
  }

  return true;
}

std::unique_ptr<ServerImpl> DdscConf::create_server() const { return std::make_unique<DdscServerImpl>(*this); }

std::unique_ptr<ClientImpl> DdscConf::create_client() const { return std::make_unique<DdscClientImpl>(*this); }

std::unique_ptr<PublisherImpl> DdscConf::create_publisher() const { return std::make_unique<DdscPublisherImpl>(*this); }

std::unique_ptr<SubscriberImpl> DdscConf::create_subscriber() const {
  return std::make_unique<DdscSubscriberImpl>(*this);
}

std::unique_ptr<SetterImpl> DdscConf::create_setter() const { return std::make_unique<DdscSetterImpl>(*this); }

std::unique_ptr<GetterImpl> DdscConf::create_getter() const { return std::make_unique<DdscGetterImpl>(*this); }

std::ostream& operator<<(std::ostream& ostream, const DdscConf& conf) noexcept {
  std::ios_base::fmtflags f = ostream.flags();

  ostream << "DdscConf:"
          << "[type]" << +conf.get_impl_type() << "[topic]" << conf.topic << "[domain]" << conf.domain << "[depth]"
          << conf.depth << "[qos]" << conf.qos;

  ostream.flags(f);

  return ostream;
}

class ConfPluginDdsc : public ConfPluginInterface {
 protected:
  TransportType get_transport_type() const override { return TransportType::kDdsc; }

  std::unique_ptr<Conf> create() const override { return std::make_unique<DdscConf>(); }
};

#ifdef VLINK_LIBRARY
VLINK_PLUGIN_DECLARE(ConfPluginDdsc, 1, 0)
#endif

}  // namespace vlink
