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

#include "./transports/zenoh_transport.h"

#include <memory>
#include <string>
#include <thread>

#include "./base/helpers.h"
#include "./base/logger.h"
#include "./impl/transport_plugin_interface.h"
#include "./impl/url.h"
#include "./zenoh_client_impl.h"
#include "./zenoh_getter_impl.h"
#include "./zenoh_publisher_impl.h"
#include "./zenoh_server_impl.h"
#include "./zenoh_setter_impl.h"
#include "./zenoh_subscriber_impl.h"

namespace vlink {

VLINK_DEFINE_GLOBAL_PROPERTY(ZenohTransport)

std::map<std::string, Qos> ZenohTransport::qos_map_;
std::shared_mutex ZenohTransport::mtx_;

// ZenohTransport
void ZenohTransport::register_qos(const std::string& name, const Qos& qos) {
  std::lock_guard lock(mtx_);

  if VUNLIKELY (qos_map_.find(name) != qos_map_.end() || name == "part" || name == "topic" || name == "pub" ||
                name == "sub" || name == "writer" || name == "reader") {
    VLOG_F("ZenohTransport: Invalid qos name: '", name, "' (reserved or already registered).");
  }

  register_qos_internal(name, qos);
}

void ZenohTransport::register_qos_internal(const std::string& name, const Qos& qos) { qos_map_[name] = qos; }

const Qos& ZenohTransport::find_qos(const std::string& name) {
  std::shared_lock lock(mtx_);

  auto iter = qos_map_.find(name);

  if VUNLIKELY (iter == qos_map_.end()) {
    CLOG_E("ZenohTransport: Invalid qos [%s].", name.c_str());
    static Qos invalid_qos;
    return invalid_qos;
  }

  return iter->second;
}

void ZenohTransport::global_init() { ZenohFactory::get(); }

bool ZenohTransport::parse_protocol(struct Protocol* protocol) {
  if VUNLIKELY (get_impl_type() == kUnknownImplType) {
    VLOG_F("ZenohTransport: Unknown node type, cannot parse protocol.");
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

std::unique_ptr<ServerImpl> ZenohTransport::create_server() const { return std::make_unique<ZenohServerImpl>(*this); }

std::unique_ptr<ClientImpl> ZenohTransport::create_client() const { return std::make_unique<ZenohClientImpl>(*this); }

std::unique_ptr<PublisherImpl> ZenohTransport::create_publisher() const {
  return std::make_unique<ZenohPublisherImpl>(*this);
}

std::unique_ptr<SubscriberImpl> ZenohTransport::create_subscriber() const {
  return std::make_unique<ZenohSubscriberImpl>(*this);
}

std::unique_ptr<SetterImpl> ZenohTransport::create_setter() const { return std::make_unique<ZenohSetterImpl>(*this); }

std::unique_ptr<GetterImpl> ZenohTransport::create_getter() const { return std::make_unique<ZenohGetterImpl>(*this); }

bool ZenohTransport::is_valid() const {
  if VUNLIKELY (domain < 0 || depth < 0 || address.empty()) {
    return false;
  }

  return true;
}

std::ostream& operator<<(std::ostream& ostream, const ZenohTransport& transport) noexcept {
  std::ios_base::fmtflags f = ostream.flags();

  ostream << "ZenohTransport:"
          << "[type]" << +transport.get_impl_type() << "[address]" << transport.address << "[event]" << transport.event
          << "[domain]" << transport.domain << "[depth]" << transport.depth << "[qos]" << transport.qos << "[fragment]"
          << transport.fragment;

  if (!transport.shm.empty()) {
    ostream << "[shm]" << transport.shm;
  }

  if (!transport.shm_mode.empty()) {
    ostream << "[shm_mode]" << transport.shm_mode;
  }

  if (!transport.shm_size.empty()) {
    ostream << "[shm_size]" << transport.shm_size;
  }

  if (!transport.shm_threshold.empty()) {
    ostream << "[shm_threshold]" << transport.shm_threshold;
  }

  if (!transport.shm_loan_threshold.empty()) {
    ostream << "[shm_loan_threshold]" << transport.shm_loan_threshold;
  }

  if (!transport.shm_blocking.empty()) {
    ostream << "[shm_blocking]" << transport.shm_blocking;
  }

  ostream.flags(f);

  return ostream;
}

class TransportPluginZenoh : public TransportPluginInterface {
 protected:
  TransportType get_transport_type() const override { return TransportType::kZenoh; }

  std::unique_ptr<Transport> create() const override { return std::make_unique<ZenohTransport>(); }
};

#ifdef VLINK_LIBRARY
VLINK_PLUGIN_DECLARE(TransportPluginZenoh, 1, 0)
#endif

}  // namespace vlink
