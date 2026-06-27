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

#include "./modules/zenoh_conf.h"

#include <memory>
#include <string>
#include <thread>

#include "./base/helpers.h"
#include "./base/logger.h"
#include "./impl/conf_plugin_interface.h"
#include "./impl/url.h"
#include "./zenoh_client_impl.h"
#include "./zenoh_getter_impl.h"
#include "./zenoh_publisher_impl.h"
#include "./zenoh_server_impl.h"
#include "./zenoh_setter_impl.h"
#include "./zenoh_subscriber_impl.h"

namespace vlink {

VLINK_DEFINE_GLOBAL_PROPERTY(ZenohConf)

std::map<std::string, Qos> ZenohConf::qos_map_;
std::shared_mutex ZenohConf::mtx_;

// ZenohConf
void ZenohConf::register_qos(const std::string& name, const Qos& qos) {
  std::lock_guard lock(mtx_);

  if VUNLIKELY (qos_map_.find(name) != qos_map_.end() || name == "part" || name == "topic" || name == "pub" ||
                name == "sub" || name == "writer" || name == "reader") {
    VLOG_F("ZenohConf: Invalid qos name: '", name, "' (reserved or already registered).");
  }

  register_qos_internal(name, qos);
}

void ZenohConf::register_qos_internal(const std::string& name, const Qos& qos) { qos_map_[name] = qos; }

const Qos& ZenohConf::find_qos(const std::string& name) {
  std::shared_lock lock(mtx_);

  auto iter = qos_map_.find(name);

  if VUNLIKELY (iter == qos_map_.end()) {
    CLOG_E("ZenohConf: Invalid qos [%s].", name.c_str());
    static Qos invalid_qos;
    return invalid_qos;
  }

  return iter->second;
}

void ZenohConf::global_init() { ZenohFactory::get(); }

bool ZenohConf::parse_protocol(struct Protocol* protocol) {
  if VUNLIKELY (get_impl_type() == kUnknownImplType) {
    VLOG_F("ZenohConf: Unknown node type, cannot parse protocol.");
    return false;
  }

  if VUNLIKELY (protocol->host.empty()) {
    return false;
  }

  static int default_domain_id = ZenohFactory::get_default_domain_id();

  address = protocol->host + (protocol->path.empty() ? "" : "/" + protocol->path);
  event = protocol->dictionary["event"];
  domain = Helpers::to_int(protocol->dictionary["domain"], default_domain_id);
  depth = Helpers::to_int(protocol->dictionary["depth"], 0);
  qos = protocol->dictionary["qos"];
  fragment = protocol->fragment;
  shm = protocol->dictionary["shm"];
  shm_mode = protocol->dictionary["shm_mode"];
  shm_size = protocol->dictionary["shm_size"];
  shm_threshold = protocol->dictionary["shm_threshold"];
  shm_loan_threshold = protocol->dictionary["shm_loan_threshold"];
  shm_blocking = protocol->dictionary["shm_blocking"];

  return true;
}

bool ZenohConf::is_valid() const {
  if VUNLIKELY (domain < 0 || address.empty()) {
    return false;
  }

  return true;
}

std::unique_ptr<ServerImpl> ZenohConf::create_server() const { return std::make_unique<ZenohServerImpl>(*this); }

std::unique_ptr<ClientImpl> ZenohConf::create_client() const { return std::make_unique<ZenohClientImpl>(*this); }

std::unique_ptr<PublisherImpl> ZenohConf::create_publisher() const {
  return std::make_unique<ZenohPublisherImpl>(*this);
}

std::unique_ptr<SubscriberImpl> ZenohConf::create_subscriber() const {
  return std::make_unique<ZenohSubscriberImpl>(*this);
}

std::unique_ptr<SetterImpl> ZenohConf::create_setter() const { return std::make_unique<ZenohSetterImpl>(*this); }

std::unique_ptr<GetterImpl> ZenohConf::create_getter() const { return std::make_unique<ZenohGetterImpl>(*this); }

std::ostream& operator<<(std::ostream& ostream, const ZenohConf& conf) noexcept {
  std::ios_base::fmtflags f = ostream.flags();

  ostream << "ZenohConf:"
          << "[type]" << +conf.get_impl_type() << "[address]" << conf.address << "[event]" << conf.event << "[domain]"
          << conf.domain << "[depth]" << conf.depth << "[qos]" << conf.qos << "[fragment]" << conf.fragment;

  if (!conf.shm.empty()) {
    ostream << "[shm]" << conf.shm;
  }

  if (!conf.shm_mode.empty()) {
    ostream << "[shm_mode]" << conf.shm_mode;
  }

  if (!conf.shm_size.empty()) {
    ostream << "[shm_size]" << conf.shm_size;
  }

  if (!conf.shm_threshold.empty()) {
    ostream << "[shm_threshold]" << conf.shm_threshold;
  }

  if (!conf.shm_loan_threshold.empty()) {
    ostream << "[shm_loan_threshold]" << conf.shm_loan_threshold;
  }

  if (!conf.shm_blocking.empty()) {
    ostream << "[shm_blocking]" << conf.shm_blocking;
  }

  ostream.flags(f);

  return ostream;
}

class ConfPluginZenoh : public ConfPluginInterface {
 protected:
  TransportType get_transport_type() const override { return TransportType::kZenoh; }

  std::unique_ptr<Conf> create() const override { return std::make_unique<ZenohConf>(); }
};

#ifdef VLINK_LIBRARY
VLINK_PLUGIN_DECLARE(ConfPluginZenoh, 1, 0)
#endif

}  // namespace vlink
