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

#include "./modules/shm_conf.h"

#include <memory>
#include <string>
#include <thread>

#include "./base/helpers.h"
#include "./base/logger.h"
#include "./impl/conf_plugin_interface.h"
#include "./impl/url.h"
#include "./shm_client_impl.h"
#include "./shm_getter_impl.h"
#include "./shm_publisher_impl.h"
#include "./shm_server_impl.h"
#include "./shm_setter_impl.h"
#include "./shm_subscriber_impl.h"

namespace vlink {

VLINK_DEFINE_GLOBAL_PROPERTY(ShmConf)

static constexpr size_t kMaxStringSize = 80U;

// ShmConf
bool ShmConf::has_roudi_inited() { return ShmFactory::has_roudi_inited(); }

bool ShmConf::has_runtime_inited() { return ShmFactory::has_runtime_inited(); }

bool ShmConf::has_roudi_running() { return ShmFactory::has_roudi_running(); }

bool ShmConf::auto_init_roudi(bool same_process_from_roudi) {
  return ShmFactory::auto_init_roudi(same_process_from_roudi);
}

void ShmConf::init_runtime(const std::string& name, bool same_process_from_roudi) {
  if VUNLIKELY (name.size() > kMaxStringSize) {
    VLOG_F("ShmConf: Input string length is too long.");

    return;
  }

  Bytes::init_memory_pool();

  ShmFactory::init_runtime(name, same_process_from_roudi);
}

void ShmConf::deinit_runtime() { ShmFactory::deinit_runtime(); }

void ShmConf::init_roudi(const std::string& config_path, int memory_strategy, bool monitoring_enable) {
  Bytes::init_memory_pool();

  ShmFactory::init_roudi(config_path, memory_strategy, monitoring_enable);
}

void ShmConf::global_init() {
  Bytes::init_memory_pool();

  ShmFactory::get();
}

bool ShmConf::parse_protocol(struct Protocol* protocol) {
  if VUNLIKELY (get_impl_type() == kUnknownImplType) {
    VLOG_F("ShmConf: Unknown node type, cannot parse protocol.");

    return false;
  }

  if VUNLIKELY (protocol->host.empty()) {
    return false;
  }

  address = protocol->host + (protocol->path.empty() ? "" : "/" + protocol->path);
  event = protocol->dictionary["event"];
  domain = Helpers::to_int(protocol->dictionary["domain"], 0);
  depth = Helpers::to_int(protocol->dictionary["depth"], 0);

  const auto& history_str = protocol->dictionary["history"];

  if (history_str.empty()) {
    if (get_impl_type() == kSetter || get_impl_type() == kGetter) {
      history = 1;
    } else {
      history = 0;
    }
  } else {
    history = Helpers::to_int(history_str, 0);
  }

  wait = Helpers::to_int(protocol->dictionary["wait"], 0);

  if (wait > 0 && (get_impl_type() == kClient || get_impl_type() == kServer || get_impl_type() == kSetter ||
                   get_impl_type() == kGetter)) {
    VLOG_W("ShmConf: Wait mode must be Event(Pub/Sub).");

    return false;
  }

  return true;
}

bool ShmConf::is_valid() const {
  if VUNLIKELY (address.empty()) {
    return false;
  }

  if VUNLIKELY (address.size() > kMaxStringSize || event.size() > kMaxStringSize) {
    return false;
  }

  if VUNLIKELY (domain < 0 || depth < 0 || history < 0) {
    return false;
  }

  return true;
}

std::unique_ptr<ServerImpl> ShmConf::create_server() const { return std::make_unique<ShmServerImpl>(*this); }

std::unique_ptr<ClientImpl> ShmConf::create_client() const { return std::make_unique<ShmClientImpl>(*this); }

std::unique_ptr<PublisherImpl> ShmConf::create_publisher() const { return std::make_unique<ShmPublisherImpl>(*this); }

std::unique_ptr<SubscriberImpl> ShmConf::create_subscriber() const {
  return std::make_unique<ShmSubscriberImpl>(*this);
}

std::unique_ptr<SetterImpl> ShmConf::create_setter() const { return std::make_unique<ShmSetterImpl>(*this); }

std::unique_ptr<GetterImpl> ShmConf::create_getter() const { return std::make_unique<ShmGetterImpl>(*this); }

std::ostream& operator<<(std::ostream& ostream, const ShmConf& conf) noexcept {
  std::ios_base::fmtflags f = ostream.flags();

  ostream << "ShmConf:"
          << "[type]" << +conf.get_impl_type() << "[address]" << conf.address << "[event]" << conf.event << "[domain]"
          << conf.domain << "[depth]" << conf.depth << "[history]" << conf.history << "[wait]" << conf.wait;

  ostream.flags(f);

  return ostream;
}

class ConfPluginShm : public ConfPluginInterface {
 protected:
  TransportType get_transport_type() const override { return TransportType::kShm; }

  std::unique_ptr<Conf> create() const override { return std::make_unique<ShmConf>(); }
};

#ifdef VLINK_LIBRARY
VLINK_PLUGIN_DECLARE(ConfPluginShm, 1, 0)
#endif

}  // namespace vlink
