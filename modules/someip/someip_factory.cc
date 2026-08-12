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

#include "./someip_factory.h"

#include <e2e/e2e_profiles/standard_profile.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <fstream>
#include <limits>
#include <nlohmann/json.hpp>
#include <string>
#include <utility>
#include <vector>

#include "./base/utils.h"
#include "./impl/server_impl.h"

namespace vlink {

static constexpr uint16_t kControlMethod = 0xFFFE;
static constexpr uint8_t kControlSubscribe = 1;
static constexpr uint8_t kControlUnsubscribe = 2;
static constexpr uint8_t kControlProbe = 3;
static constexpr uint8_t kControlDisconnect = 4;
static constexpr std::array<uint8_t, 4> kControlMagic{{'V', 'L', 'N', 'K'}};
static constexpr size_t kControlHeaderSize = kControlMagic.size() + 1;
static constexpr uint16_t kDefaultBasePort = 30491;
static constexpr uint16_t kDefaultPortSpan = 30000;
static constexpr size_t kMaxUdpPayload = 65507;

static someip::platform::ByteBuffer encode_control(uint8_t operation, const SomeipConf::Groups& groups) {
  someip::platform::ByteBuffer payload;

  payload.reserve(kControlHeaderSize + groups.size() * 2);
  payload.insert(payload.end(), kControlMagic.begin(), kControlMagic.end());
  payload.push_back(operation);

  for (const auto group : groups) {
    payload.push_back(static_cast<uint8_t>(group >> 8U));
    payload.push_back(static_cast<uint8_t>(group & 0xFFU));
  }

  return payload;
}

static bool is_control_message(const someip::Message& message) {
  const auto& payload = message.get_payload();
  return message.get_method_id() == kControlMethod && payload.size() >= kControlHeaderSize &&
         std::equal(kControlMagic.begin(), kControlMagic.end(), payload.begin());
}

// SomeipFactory
SomeipFactory::SomeipFactory() {
  Bytes::init_memory_pool();

  const auto config = Utils::get_env("VLINK_SOMEIP_CFG");

  if (!config.empty() && !load_config_file(config)) {
    VLOG_E("SomeipFactory: Failed to load VLINK_SOMEIP_CFG='", config, "'.");
  }
}

SomeipFactory::~SomeipFactory() = default;

bool SomeipFactory::load_global_config_file(const std::string& filepath) { return get().load_config_file(filepath); }

bool SomeipFactory::load_config_file(const std::string& filepath) {
  std::ifstream stream(filepath);

  if VUNLIKELY (!stream.is_open()) {
    VLOG_E("SomeipFactory: Cannot open config file '", filepath, "'.");
    return false;
  }

  try {
    nlohmann::json root;
    stream >> root;

    Conf::PropertiesMap parsed_global;
    std::map<std::pair<uint16_t, uint16_t>, ServiceConfig> parsed_services;

    const auto scalar = [](const nlohmann::json& value) {
      if (value.is_string()) {
        return value.get<std::string>();
      }

      if (value.is_boolean()) {
        return value.get<bool>() ? std::string("true") : std::string("false");
      }

      if (value.is_number_unsigned()) {
        return std::to_string(value.get<uint64_t>());
      }

      if (value.is_number_integer()) {
        return std::to_string(value.get<int64_t>());
      }

      return std::string();
    };

    const auto id = [&scalar](const nlohmann::json& value) {
      const auto text = scalar(value);
      const char* begin = text.data();
      const char* end = begin + text.size();
      int base = 10;

      if (text.size() > 2 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) {
        begin += 2;
        base = 16;
      }

      uint64_t parsed = 0;
      const auto result = std::from_chars(begin, end, parsed, base);

      if (result.ec != std::errc() || result.ptr != end || parsed > std::numeric_limits<uint16_t>::max()) {
        return uint16_t{0};
      }

      return static_cast<uint16_t>(parsed);
    };

    if (root.contains("opensomeip") && root["opensomeip"].is_object()) {
      const auto& opensomeip = root["opensomeip"];

      if (opensomeip.contains("transport")) {
        parsed_global["someip.transport"] = scalar(opensomeip["transport"]);
      }

      if (opensomeip.contains("local_ip")) {
        parsed_global["someip.local_ip"] = scalar(opensomeip["local_ip"]);
      }

      if (opensomeip.contains("local_port")) {
        parsed_global["someip.local_port"] = scalar(opensomeip["local_port"]);
      }

      if (opensomeip.contains("remote_ip")) {
        parsed_global["someip.remote_ip"] = scalar(opensomeip["remote_ip"]);
      }

      if (opensomeip.contains("remote_port")) {
        parsed_global["someip.remote_port"] = scalar(opensomeip["remote_port"]);
      }

      if (opensomeip.contains("client_id")) {
        parsed_global["someip.client_id"] = scalar(opensomeip["client_id"]);
      }

      if (opensomeip.contains("interface_version")) {
        parsed_global["someip.interface_version"] = scalar(opensomeip["interface_version"]);
      }

      if (opensomeip.contains("sd") && opensomeip["sd"].is_object()) {
        const auto& sd = opensomeip["sd"];

        if (sd.contains("enabled")) {
          parsed_global["someip.sd.enabled"] = scalar(sd["enabled"]);
        }

        if (sd.contains("multicast_ip")) {
          parsed_global["someip.sd.multicast_ip"] = scalar(sd["multicast_ip"]);
        }
      }

      if (opensomeip.contains("e2e") && opensomeip["e2e"].is_object() && opensomeip["e2e"].contains("enabled")) {
        parsed_global["someip.e2e.enabled"] = scalar(opensomeip["e2e"]["enabled"]);
      }
    }

    if (root.contains("unicast")) {
      parsed_global["someip.local_ip"] = scalar(root["unicast"]);
    }

    if (root.contains("service-discovery") && root["service-discovery"].is_object()) {
      const auto& sd = root["service-discovery"];

      if (sd.contains("enable")) {
        parsed_global["someip.sd.enabled"] = scalar(sd["enable"]);
      }

      if (sd.contains("multicast")) {
        parsed_global["someip.sd.multicast_ip"] = scalar(sd["multicast"]);
      }
    }

    if (root.contains("services") && root["services"].is_array()) {
      for (const auto& service : root["services"]) {
        if (!service.is_object() || !service.contains("service") || !service.contains("instance")) {
          continue;
        }

        ServiceConfig config;

        if (service.contains("unreliable")) {
          config.transport = "udp";
          config.unreliable_port = id(service["unreliable"]);
        }

        if (service.contains("reliable")) {
          if (config.transport.empty()) {
            config.transport = "tcp";
          }

          config.reliable_port = id(service["reliable"]);
        }

        if (!config.transport.empty() && (config.unreliable_port != 0 || config.reliable_port != 0)) {
          parsed_services[{id(service["service"]), id(service["instance"])}] = std::move(config);
        }
      }
    }

    std::lock_guard lock(config_mtx_);
    config_properties_ = std::move(parsed_global);
    service_configs_ = std::move(parsed_services);
  } catch (const nlohmann::json::exception& error) {
    VLOG_E("SomeipFactory: Invalid JSON config '", filepath, "': ", error.what());
    return false;
  }

  return true;
}

Conf::PropertiesMap SomeipFactory::resolve_properties(const SomeipConf& conf,
                                                      const Conf::PropertiesMap& node_properties) const {
  static constexpr std::array<std::pair<const char*, const char*>, 9> kEnvironmentProperties{{
      {"VLINK_SOMEIP_TRANSPORT", "someip.transport"},
      {"VLINK_SOMEIP_LOCAL_IP", "someip.local_ip"},
      {"VLINK_SOMEIP_LOCAL_PORT", "someip.local_port"},
      {"VLINK_SOMEIP_REMOTE_IP", "someip.remote_ip"},
      {"VLINK_SOMEIP_REMOTE_PORT", "someip.remote_port"},
      {"VLINK_SOMEIP_CLIENT_ID", "someip.client_id"},
      {"VLINK_SOMEIP_INTERFACE_VERSION", "someip.interface_version"},
      {"VLINK_SOMEIP_SD", "someip.sd.enabled"},
      {"VLINK_SOMEIP_E2E", "someip.e2e.enabled"},
  }};

  Conf::PropertiesMap properties;
  {
    std::lock_guard lock(config_mtx_);
    properties = config_properties_;
    const auto service_iter = service_configs_.find({conf.service, conf.instance});

    if (service_iter != service_configs_.end() && !service_iter->second.transport.empty()) {
      properties["someip.transport"] = service_iter->second.transport;
    }
  }

  for (const auto& [environment, property] : kEnvironmentProperties) {
    const auto value = Utils::get_env(environment);

    if (!value.empty()) {
      properties[property] = value;
    }
  }

  for (const auto& entry : SomeipConf::get_global_all_properties()) {
    properties[entry.first] = entry.second;
  }

  for (const auto& entry : node_properties) {
    properties[entry.first] = entry.second;
  }

  const auto impl_type = conf.get_impl_type();
  const bool server = impl_type == kServer || impl_type == kPublisher || impl_type == kSetter;
  const auto settings = make_settings(conf.service, conf.instance, server, properties);

  return {
      {"someip.transport", settings.use_tcp ? "tcp" : "udp"},
      {"someip.local_ip", settings.local_ip},
      {"someip.local_port", std::to_string(settings.local_port)},
      {"someip.remote_ip", settings.remote_ip},
      {"someip.remote_port", std::to_string(settings.remote_port)},
      {"someip.client_id", std::to_string(settings.client_id)},
      {"someip.interface_version", std::to_string(settings.interface_version)},
      {"someip.sd.enabled", settings.sd_enabled ? "true" : "false"},
      {"someip.sd.multicast_ip",
       std::string(settings.sd.multicast_address.data(), settings.sd.multicast_address.size())},
      {"someip.e2e.enabled", settings.e2e_enabled ? "true" : "false"},
  };
}

SomeipSettings SomeipFactory::make_settings(uint16_t service, uint16_t instance, bool server,
                                            const Conf::PropertiesMap& properties) const {
  SomeipSettings settings;
  const auto number = [&properties](const char* key, uint64_t fallback, uint64_t maximum) {
    const auto iter = properties.find(key);

    if (iter == properties.end() || iter->second.empty()) {
      return fallback;
    }

    const auto& value = iter->second;
    const char* begin = value.data();
    const char* end = begin + value.size();
    int base = 10;

    if (value.size() > 2 && value[0] == '0' && (value[1] == 'x' || value[1] == 'X')) {
      begin += 2;
      base = 16;
    }

    uint64_t parsed = 0;
    const auto result = std::from_chars(begin, end, parsed, base);

    if (result.ec != std::errc() || result.ptr != end) {
      return fallback;
    }

    return std::min(parsed, maximum);
  };

  if (const auto iter = properties.find("someip.transport"); iter != properties.end()) {
    settings.use_tcp = iter->second == "tcp" || iter->second == "reliable";
  }

  if (const auto iter = properties.find("someip.local_ip"); iter != properties.end()) {
    settings.local_ip = iter->second;
  }

  if (const auto iter = properties.find("someip.remote_ip"); iter != properties.end()) {
    settings.remote_ip = iter->second;
  }

  const auto available = static_cast<uint32_t>(std::numeric_limits<uint16_t>::max()) - kDefaultBasePort + 1U;
  const auto span = std::min<uint32_t>(available, kDefaultPortSpan);
  const auto offset = (static_cast<uint32_t>(service) * 31U + instance) % span;
  auto service_port = static_cast<uint16_t>(kDefaultBasePort + offset);

  {
    std::lock_guard lock(config_mtx_);
    auto service_iter = service_configs_.find({service, instance});

    if (service_iter != service_configs_.end()) {
      const auto configured_port =
          settings.use_tcp ? service_iter->second.reliable_port : service_iter->second.unreliable_port;

      if (configured_port != 0) {
        service_port = configured_port;
      }
    }
  }

  settings.local_port = static_cast<uint16_t>(
      number("someip.local_port", server ? service_port : 0, std::numeric_limits<uint16_t>::max()));
  settings.remote_port =
      static_cast<uint16_t>(number("someip.remote_port", service_port, std::numeric_limits<uint16_t>::max()));

  if (server && settings.local_port == 0) {
    settings.local_port = service_port;
  }

  if (settings.remote_port == 0) {
    settings.remote_port = service_port;
  }

  const auto process_client_id = static_cast<uint16_t>(Utils::get_pid()) | 1U;
  settings.client_id =
      static_cast<uint16_t>(number("someip.client_id", process_client_id, std::numeric_limits<uint16_t>::max()));

  if (settings.client_id == 0) {
    settings.client_id = process_client_id;
  }

  settings.interface_version = static_cast<uint8_t>(number("someip.interface_version", 0, 0xFF));

  if (const auto iter = properties.find("someip.sd.enabled"); iter != properties.end()) {
    settings.sd_enabled =
        iter->second == "1" || iter->second == "true" || iter->second == "yes" || iter->second == "on";
  }

  if (const auto iter = properties.find("someip.sd.multicast_ip"); iter != properties.end()) {
    settings.sd.multicast_address = iter->second;
  }

  settings.sd.unicast_address = settings.local_ip;

  const someip::transport::Endpoint sd_endpoint(settings.sd.multicast_address, settings.sd.multicast_port,
                                                someip::transport::TransportProtocol::UDP);

  if (settings.sd_enabled && (!sd_endpoint.is_ipv4() || !sd_endpoint.is_multicast())) {
    VLOG_E("SomeipFactory: OpenSOMEIP SD requires a valid IPv4 multicast endpoint.");
    settings.sd_enabled = false;
  }

  if (const auto iter = properties.find("someip.e2e.enabled"); iter != properties.end()) {
    settings.e2e_enabled =
        iter->second == "1" || iter->second == "true" || iter->second == "yes" || iter->second == "on";
  }

  settings.e2e.data_id = service;

  if (settings.e2e_enabled) {
    static std::once_flag e2e_profile_flag;
    std::call_once(e2e_profile_flag, someip::e2e::initialize_basic_profile);
  }

  const size_t header_size =
      someip::Message::get_header_size() + (settings.e2e_enabled ? someip::e2e::E2EHeader::get_header_size() : 0);
  const size_t message_limit = settings.use_tcp ? settings.tcp.max_receive_buffer : kMaxUdpPayload;
  settings.max_payload_size = message_limit > header_size ? message_limit - header_size : 0;
  settings.tcp.magic_cookie_enabled = false;

  return settings;
}

// SomeipServer
SomeipServer::SomeipServer(const SomeipID& id) {
  service_id_ = std::get<1>(id);
  instance_id_ = std::get<2>(id);
  settings_ = SomeipFactory::get().make_settings(service_id_, instance_id_, true, std::get<3>(id));

  const auto protocol =
      settings_.use_tcp ? someip::transport::TransportProtocol::TCP : someip::transport::TransportProtocol::UDP;
  const someip::transport::Endpoint local_endpoint(settings_.local_ip, settings_.local_port, protocol);

  if VUNLIKELY (!local_endpoint.is_ipv4()) {
    VLOG_E("SomeipServer: OpenSOMEIP transport requires an IPv4 local endpoint.");
    return;
  }

  if (settings_.use_tcp) {
    transport_ = std::make_unique<someip::transport::TcpTransport>(settings_.tcp);
  } else {
    transport_ = std::make_unique<someip::transport::UdpTransport>(local_endpoint, settings_.udp);
  }

  transport_->set_listener(this);
}

SomeipServer::~SomeipServer() {
  if (transport_) {
    transport_->set_listener(nullptr);
  }

  if (sd_server_) {
    std::thread([service = service_id_, instance = instance_id_, server = std::move(sd_server_)]() mutable {
      static_cast<void>(server->stop_offer_service(service, instance));
      server->shutdown();
    }).detach();
  }

  if (transport_) {
    const auto current_thread = Utils::get_native_thread_id();

    if (receive_thread_id_.load(std::memory_order_acquire) == current_thread ||
        connection_thread_id_.load(std::memory_order_acquire) == current_thread) {
      std::thread([transport = std::move(transport_)]() mutable { static_cast<void>(transport->stop()); }).detach();
    } else {
      static_cast<void>(transport_->stop());
    }
  }
}

std::any SomeipServer::get_native_handle() const { return transport_.get(); }

void SomeipServer::start() {
  bool expected = false;

  if (!has_started_.compare_exchange_strong(expected, true, std::memory_order_acq_rel, std::memory_order_relaxed)) {
    return;
  }

  if (settings_.use_tcp && transport_) {
    const someip::transport::Endpoint local_endpoint(settings_.local_ip, settings_.local_port,
                                                     someip::transport::TransportProtocol::TCP);
    auto* transport = static_cast<someip::transport::TcpTransport*>(transport_.get());

    if (transport->initialize(local_endpoint) != someip::Result::SUCCESS ||
        transport->enable_server_mode(static_cast<int>(settings_.tcp.max_connections)) != someip::Result::SUCCESS) {
      has_started_.store(false, std::memory_order_release);
      VLOG_E("SomeipServer: Failed to initialize TCP endpoint ", local_endpoint.to_string(), ".");
      return;
    }
  }

  if VUNLIKELY (!transport_ || transport_->start() != someip::Result::SUCCESS) {
    has_started_.store(false, std::memory_order_release);
    VLOG_E("SomeipServer: Failed to start service ", service_id_, ".");
    return;
  }

  if (settings_.sd_enabled && settings_.use_tcp) {
    VLOG_W("SomeipServer: OpenSOMEIP SdServer advertises UDP endpoints only; disabling SD for TCP service ",
           service_id_, ".");
  } else if (settings_.sd_enabled) {
    sd_server_ = std::make_unique<someip::sd::SdServer>(settings_.sd);
    const auto weak = weak_from_this();

    sd_server_->set_eventgroup_subscription_callback([weak](uint16_t service, uint16_t instance, uint16_t group,
                                                            const auto& address, uint16_t port, bool subscribed) {
      const auto self = weak.lock();

      if VUNLIKELY (!self || service != self->service_id_ || instance != self->instance_id_) {
        return;
      }

      const someip::transport::Endpoint endpoint(address, port, someip::transport::TransportProtocol::UDP);

      if VUNLIKELY (!endpoint.is_ipv4()) {
        return;
      }

      self->update_subscriber(endpoint, SomeipConf::Groups{group}, subscribed);
    });

    if (!sd_server_->initialize()) {
      VLOG_E("SomeipServer: Failed to initialize service discovery.");
      sd_server_.reset();
    } else {
      someip::sd::ServiceInstance instance(service_id_, instance_id_, settings_.interface_version, 0);
      instance.ttl_seconds = static_cast<uint32_t>(settings_.sd.ttl.count() / 1000);

      const auto local_endpoint = transport_->get_local_endpoint();
      const std::string address(local_endpoint.get_address().data(), local_endpoint.get_address().size());
      const auto endpoint = address + ":" + std::to_string(local_endpoint.get_port());

      if (!sd_server_->offer_service(instance, endpoint)) {
        VLOG_E("SomeipServer: Failed to offer service through OpenSOMEIP SD.");
      }
    }
  }
}

void SomeipServer::offer_event(uint16_t event, const SomeipConf::Groups& groups, bool field) {
  std::lock_guard lock(mtx_);

  auto& info = events_[event];

  for (const auto group : groups) {
    ++info.group_ref_counts[group];
  }

  if (field) {
    ++info.field_references;
  }

  ++info.references;
}

void SomeipServer::stop_offer_event(uint16_t event, const SomeipConf::Groups& groups, bool field) {
  std::lock_guard lock(mtx_);
  const auto iter = events_.find(event);

  if (iter == events_.end()) {
    return;
  }

  auto& info = iter->second;

  for (const auto group : groups) {
    const auto group_iter = info.group_ref_counts.find(group);

    if (group_iter != info.group_ref_counts.end() && --group_iter->second == 0) {
      info.group_ref_counts.erase(group_iter);
    }
  }

  if (field && info.field_references != 0) {
    --info.field_references;

    if (info.field_references == 0) {
      info.cached_payload.clear();
      info.has_cached_payload = false;
    }
  }

  if (info.references <= 1) {
    events_.erase(iter);
  } else {
    --info.references;
  }
}

bool SomeipServer::publish(uint16_t event, const Bytes& msg_data, bool field) {
  if VUNLIKELY (msg_data.size() > settings_.max_payload_size) {
    VLOG_E("SomeipServer: Payload exceeds the configured OpenSOMEIP transport limit.");
    return false;
  }

  uint16_t session = next_session_id_.fetch_add(1, std::memory_order_relaxed);

  if VUNLIKELY (session == 0) {
    session = next_session_id_.fetch_add(1, std::memory_order_relaxed);
  }

  someip::Message message({service_id_, event}, {0, session}, someip::MessageType::NOTIFICATION,
                          someip::ReturnCode::E_OK);
  message.set_interface_version(settings_.interface_version);
  message.set_payload(msg_data.data(), msg_data.size());

  std::vector<someip::transport::Endpoint> targets;
  {
    std::lock_guard lock(mtx_);
    auto event_iter = events_.find(event);

    if (event_iter == events_.end()) {
      return false;
    }

    if (field) {
      event_iter->second.cached_payload = message.get_payload();
      event_iter->second.has_cached_payload = true;
    }

    for (const auto& entry : event_iter->second.group_ref_counts) {
      const auto subscribers = subscribers_.find(entry.first);

      if (subscribers != subscribers_.end()) {
        targets.insert(targets.end(), subscribers->second.begin(), subscribers->second.end());
      }
    }
  }

  std::sort(targets.begin(), targets.end());
  targets.erase(std::unique(targets.begin(), targets.end()), targets.end());

  bool success = true;

  for (const auto& endpoint : targets) {
    success = send_message(message, endpoint) && success;
  }

  return success;
}

bool SomeipServer::has_subscribers(const SomeipConf::Groups& groups) const {
  std::lock_guard lock(mtx_);

  for (const auto group : groups) {
    const auto iter = subscribers_.find(group);

    if (iter != subscribers_.end() && !iter->second.empty()) {
      return true;
    }
  }

  return false;
}

bool SomeipServer::send_message(someip::Message& message, const someip::transport::Endpoint& endpoint) {
  std::lock_guard lock(send_mtx_);

  if VUNLIKELY (settings_.e2e_enabled && e2e_.protect(message, settings_.e2e) != someip::Result::SUCCESS) {
    VLOG_E("SomeipServer: OpenSOMEIP E2E protection failed.");
    return false;
  }

  const bool success = transport_ && transport_->send_message(message, endpoint) == someip::Result::SUCCESS;

  if VUNLIKELY (settings_.e2e_enabled) {
    message.clear_e2e_header();
  }

  return success;
}

void SomeipServer::handle_control(const someip::Message& message, const someip::transport::Endpoint& sender) {
  const auto& payload = message.get_payload();
  const auto operation = payload[kControlMagic.size()];

  SomeipConf::Groups groups;

  for (size_t offset = kControlHeaderSize; offset + 1 < payload.size(); offset += 2) {
    groups.emplace(static_cast<uint16_t>((static_cast<uint16_t>(payload[offset]) << 8U) | payload[offset + 1]));
  }

  if (operation == kControlSubscribe || operation == kControlUnsubscribe) {
    update_subscriber(sender, groups, operation == kControlSubscribe);
  } else if (operation == kControlDisconnect) {
    on_connection_lost(sender);
  } else if (operation != kControlProbe) {
    return;
  }

  if (message.get_message_type() == someip::MessageType::REQUEST) {
    someip::Message response(message.get_message_id(), message.get_request_id(), someip::MessageType::RESPONSE,
                             someip::ReturnCode::E_OK);
    response.set_interface_version(settings_.interface_version);
    response.set_payload(encode_control(operation, {}));
    static_cast<void>(send_message(response, sender));
  }
}

void SomeipServer::update_subscriber(const someip::transport::Endpoint& endpoint, const SomeipConf::Groups& groups,
                                     bool subscribed) {
  std::vector<std::pair<uint16_t, someip::platform::ByteBuffer>> cached_fields;
  SomeipConf::Groups new_groups;
  bool changed = false;

  {
    std::lock_guard lock(mtx_);

    for (const auto group : groups) {
      if (subscribed) {
        auto& endpoints = subscribers_[group];
        const bool inserted = endpoints.emplace(endpoint).second;

        if (inserted) {
          new_groups.emplace(group);
          changed = true;
        }
      } else {
        const auto iter = subscribers_.find(group);

        if (iter != subscribers_.end()) {
          changed = iter->second.erase(endpoint) != 0 || changed;

          if (iter->second.empty()) {
            subscribers_.erase(iter);
          }
        }
      }
    }

    if (!new_groups.empty()) {
      for (const auto& [event, info] : events_) {
        const bool shares_group =
            std::any_of(info.group_ref_counts.begin(), info.group_ref_counts.end(),
                        [&new_groups](const auto& entry) { return new_groups.count(entry.first) != 0; });

        if (info.field_references != 0 && info.has_cached_payload && shares_group) {
          cached_fields.emplace_back(event, info.cached_payload);
        }
      }
    }
  }

  for (const auto& [event, payload] : cached_fields) {
    uint16_t session = next_session_id_.fetch_add(1, std::memory_order_relaxed);

    if VUNLIKELY (session == 0) {
      session = next_session_id_.fetch_add(1, std::memory_order_relaxed);
    }

    someip::Message notification({service_id_, event}, {0, session}, someip::MessageType::NOTIFICATION,
                                 someip::ReturnCode::E_OK);
    notification.set_interface_version(settings_.interface_version);
    notification.set_payload(payload);
    static_cast<void>(send_message(notification, endpoint));
  }

  if (changed) {
    notify_subscriber_change();
  }
}

void SomeipServer::notify_subscriber_change() {
  traverse_sub_connect_callback([this](NodeImpl* impl, const auto& callback) {
    const auto* conf = impl->get_target_conf<SomeipConf>();
    callback(has_subscribers(conf->groups));
  });
}

void SomeipServer::on_message_received(someip::MessagePtr message, const someip::transport::Endpoint& sender) {
  auto keep_alive = weak_from_this().lock();

  if VUNLIKELY (!keep_alive) {
    return;
  }

  receive_thread_id_.store(Utils::get_native_thread_id(), std::memory_order_release);

  if VUNLIKELY (!message) {
    return;
  }

  const auto* received = message.get();

  if VUNLIKELY (received->get_service_id() != service_id_ ||
                received->get_interface_version() != settings_.interface_version) {
    return;
  }

  if VUNLIKELY (settings_.e2e_enabled) {
    const auto& payload = message->get_payload();
    someip::e2e::E2EHeader header;

    if VUNLIKELY (payload.size() < someip::e2e::E2EHeader::get_header_size() || !header.deserialize(payload)) {
      VLOG_E("SomeipServer: Rejected a message that failed OpenSOMEIP E2E validation.");
      return;
    }

    someip::platform::ByteBuffer actual_payload(payload.begin() + someip::e2e::E2EHeader::get_header_size(),
                                                payload.end());
    message->set_e2e_header(header);
    message->set_payload(std::move(actual_payload));

    if VUNLIKELY (e2e_.validate(*message, settings_.e2e) != someip::Result::SUCCESS) {
      VLOG_E("SomeipServer: Rejected a message that failed OpenSOMEIP E2E validation.");
      return;
    }
  }

  if (received->is_request() && is_control_message(*received)) {
    handle_control(*received, sender);
    return;
  }

  if VUNLIKELY (!received->is_request()) {
    return;
  }

  const auto& payload = received->get_payload();
  const Bytes request = Bytes::shallow_copy(payload.data(), payload.size());

  traverse_req_resp_callback([this, received, &request, &sender](NodeImpl* impl, const auto& callback) {
    const auto* conf = impl->get_target_conf<SomeipConf>();

    if VUNLIKELY (conf->method != received->get_method_id() || impl->has_suspend) {
      ignore_called();
      return;
    }

    if VUNLIKELY (has_called()) {
      VLOG_F(*conf, "Two identical service requests.");
      return;
    }

    if (static_cast<ServerImpl*>(impl)->is_resp_type && received->get_message_type() == someip::MessageType::REQUEST) {
      Bytes response_data;
      callback(0, request, &response_data);

      if VUNLIKELY (response_data.size() > settings_.max_payload_size) {
        VLOG_E(*conf, "SOME/IP response payload exceeds the configured OpenSOMEIP limit.");
        return;
      }

      someip::Message response(received->get_message_id(), received->get_request_id(), someip::MessageType::RESPONSE,
                               someip::ReturnCode::E_OK);
      response.set_interface_version(settings_.interface_version);
      response.set_payload(response_data.data(), response_data.size());
      static_cast<void>(send_message(response, sender));
    } else {
      callback(0, request, nullptr);
    }
  });
}

void SomeipServer::on_connection_lost(const someip::transport::Endpoint& endpoint) {
  auto keep_alive = weak_from_this().lock();

  if VUNLIKELY (!keep_alive) {
    return;
  }

  connection_thread_id_.store(Utils::get_native_thread_id(), std::memory_order_release);

  bool changed = false;

  {
    std::lock_guard lock(mtx_);

    for (auto iter = subscribers_.begin(); iter != subscribers_.end();) {
      changed = iter->second.erase(endpoint) != 0 || changed;

      if (iter->second.empty()) {
        iter = subscribers_.erase(iter);
      } else {
        ++iter;
      }
    }
  }

  if (changed) {
    notify_subscriber_change();
  }
}

void SomeipServer::on_connection_established(const someip::transport::Endpoint&) {
  auto keep_alive = weak_from_this().lock();

  if VUNLIKELY (!keep_alive) {
    return;
  }

  connection_thread_id_.store(Utils::get_native_thread_id(), std::memory_order_release);
}

void SomeipServer::on_error(someip::Result error) {
  auto keep_alive = weak_from_this().lock();

  if VUNLIKELY (!keep_alive) {
    return;
  }

  connection_thread_id_.store(Utils::get_native_thread_id(), std::memory_order_release);
  VLOG_E("SomeipServer: OpenSOMEIP transport error: ", someip::to_string(error));
}

// SomeipClient
SomeipClient::SomeipClient(const SomeipID& id) {
  service_id_ = std::get<1>(id);
  instance_id_ = std::get<2>(id);
  settings_ = SomeipFactory::get().make_settings(service_id_, instance_id_, false, std::get<3>(id));

  const auto protocol =
      settings_.use_tcp ? someip::transport::TransportProtocol::TCP : someip::transport::TransportProtocol::UDP;
  const someip::transport::Endpoint local_endpoint(settings_.local_ip, settings_.local_port, protocol);
  remote_endpoint_ = someip::transport::Endpoint(settings_.remote_ip, settings_.remote_port, protocol);

  if VUNLIKELY (!local_endpoint.is_ipv4() || !remote_endpoint_.is_ipv4()) {
    VLOG_E("SomeipClient: OpenSOMEIP transport requires IPv4 local and remote endpoints.");
    return;
  }

  if (settings_.use_tcp) {
    transport_ = std::make_unique<someip::transport::TcpTransport>(settings_.tcp);
  } else {
    transport_ = std::make_unique<someip::transport::UdpTransport>(local_endpoint, settings_.udp);
  }

  transport_->set_listener(this);
}

SomeipClient::~SomeipClient() {
  monitor_running_.store(false, std::memory_order_release);
  monitor_cv_.notify_all();

  if (monitor_thread_.joinable()) {
    if (monitor_thread_.get_id() == std::this_thread::get_id()) {
      monitor_thread_.detach();
    } else {
      monitor_thread_.join();
    }
  }

  if (transport_) {
    transport_->set_listener(nullptr);
  }

  SomeipConf::Groups groups;

  {
    std::lock_guard lock(mtx_);

    for (const auto& entry : group_ref_counts_) {
      groups.emplace(entry.first);
    }
  }

  if (!groups.empty()) {
    static_cast<void>(send_control(kControlUnsubscribe, groups));
  }

  if (has_started_.load(std::memory_order_acquire)) {
    static_cast<void>(send_control(kControlDisconnect, {}));
  }

  if (sd_client_) {
    std::thread([service = service_id_, client = std::move(sd_client_)]() mutable {
      client->unsubscribe_service(service);
      client->shutdown();
    }).detach();
  }

  if (transport_) {
    const auto current_thread = Utils::get_native_thread_id();

    if (receive_thread_id_.load(std::memory_order_acquire) == current_thread ||
        connection_thread_id_.load(std::memory_order_acquire) == current_thread) {
      std::thread([transport = std::move(transport_)]() mutable {
        static_cast<void>(transport->disconnect());
        static_cast<void>(transport->stop());
      }).detach();
    } else {
      static_cast<void>(transport_->disconnect());
      static_cast<void>(transport_->stop());
    }
  }
}

std::any SomeipClient::get_native_handle() const { return transport_.get(); }

void SomeipClient::start() {
  bool expected = false;

  if (!has_started_.compare_exchange_strong(expected, true, std::memory_order_acq_rel, std::memory_order_relaxed)) {
    return;
  }

  if (settings_.use_tcp && transport_) {
    const someip::transport::Endpoint local_endpoint(settings_.local_ip, settings_.local_port,
                                                     someip::transport::TransportProtocol::TCP);
    auto* transport = static_cast<someip::transport::TcpTransport*>(transport_.get());

    if (transport->initialize(local_endpoint) != someip::Result::SUCCESS) {
      has_started_.store(false, std::memory_order_release);
      VLOG_E("SomeipClient: Failed to initialize TCP endpoint ", local_endpoint.to_string(), ".");
      return;
    }
  }

  if VUNLIKELY (!transport_ || transport_->start() != someip::Result::SUCCESS) {
    has_started_.store(false, std::memory_order_release);
    VLOG_E("SomeipClient: Failed to start OpenSOMEIP transport.");
    return;
  }

  if (settings_.use_tcp) {
    if (transport_->connect(remote_endpoint_) != someip::Result::SUCCESS) {
      VLOG_W("SomeipClient: TCP service is not available at ", remote_endpoint_.to_string(), ".");
    }
  } else {
    static_cast<void>(send_control(kControlProbe, {}));
  }

  if (settings_.sd_enabled && !settings_.use_tcp) {
    sd_client_ = std::make_unique<someip::sd::SdClient>(settings_.sd);

    if (!sd_client_->initialize()) {
      VLOG_E("SomeipClient: Failed to initialize OpenSOMEIP service discovery.");
      sd_client_.reset();
    } else {
      const auto weak = weak_from_this();

      sd_client_->subscribe_service(
          service_id_,
          [weak](const someip::sd::ServiceInstance& instance) {
            if (const auto self = weak.lock()) {
              self->update_remote_endpoint(instance);

              SomeipConf::Groups groups;

              {
                std::lock_guard lock(self->mtx_);

                for (const auto& entry : self->group_ref_counts_) {
                  groups.emplace(entry.first);
                }
              }

              const auto endpoint = self->transport_->get_local_endpoint();

              for (const auto group : groups) {
                static_cast<void>(self->sd_client_->subscribe_eventgroup(self->service_id_, self->instance_id_, group,
                                                                         self->settings_.interface_version,
                                                                         endpoint.get_address(), endpoint.get_port()));
              }
            }
          },
          [weak](const someip::sd::ServiceInstance& instance) {
            if (const auto self = weak.lock(); self && instance.instance_id == self->instance_id_) {
              self->update_connected(false);
            }
          });

      sd_client_->find_service(service_id_, [weak](const someip::platform::Vector<someip::sd::ServiceInstance>& list) {
        if (const auto self = weak.lock()) {
          for (const auto& instance : list) {
            self->update_remote_endpoint(instance);
          }
        }
      });
    }
  }

  SomeipConf::Groups groups;

  {
    std::lock_guard lock(mtx_);

    for (const auto& entry : group_ref_counts_) {
      groups.emplace(entry.first);
    }
  }

  if (!groups.empty()) {
    static_cast<void>(send_control(kControlSubscribe, groups));
  }

  if (!settings_.use_tcp && settings_.probe_interval.count() > 0) {
    monitor_running_.store(true, std::memory_order_release);
    auto self = shared_from_this();
    monitor_thread_ = std::thread([self = std::move(self)] { self->monitor_connection(); });
  }
}

bool SomeipClient::subscribe(const SomeipConf::Groups& groups) {
  SomeipConf::Groups new_groups;

  {
    std::lock_guard lock(mtx_);

    for (const auto group : groups) {
      auto& count = group_ref_counts_[group];

      if (count++ == 0) {
        new_groups.emplace(group);
      }
    }
  }

  if (new_groups.empty() || !has_started_.load(std::memory_order_acquire)) {
    return true;
  }

  bool success = send_control(kControlSubscribe, new_groups);

  if (sd_client_) {
    const auto endpoint = transport_->get_local_endpoint();

    for (const auto group : new_groups) {
      success = sd_client_->subscribe_eventgroup(service_id_, instance_id_, group, settings_.interface_version,
                                                 endpoint.get_address(), endpoint.get_port()) &&
                success;
    }
  }

  return success;
}

void SomeipClient::unsubscribe(const SomeipConf::Groups& groups) {
  SomeipConf::Groups removed_groups;

  {
    std::lock_guard lock(mtx_);

    for (const auto group : groups) {
      const auto iter = group_ref_counts_.find(group);

      if (iter != group_ref_counts_.end() && --iter->second == 0) {
        removed_groups.emplace(group);
        group_ref_counts_.erase(iter);
      }
    }
  }

  if (!removed_groups.empty() && has_started_.load(std::memory_order_acquire)) {
    static_cast<void>(send_control(kControlUnsubscribe, removed_groups));

    if (sd_client_) {
      for (const auto group : removed_groups) {
        static_cast<void>(sd_client_->unsubscribe_eventgroup(service_id_, instance_id_, group));
      }
    }
  }
}

bool SomeipClient::call(uint16_t method, const Bytes& req_data, NodeImpl::MsgCallback&& callback, uint64_t* seq_out) {
  if VUNLIKELY (req_data.size() > settings_.max_payload_size) {
    VLOG_E("SomeipClient: Request payload exceeds the configured OpenSOMEIP transport limit.");
    return false;
  }

  uint16_t session = next_session_id_.fetch_add(1, std::memory_order_relaxed);

  if VUNLIKELY (session == 0) {
    session = next_session_id_.fetch_add(1, std::memory_order_relaxed);
  }

  const someip::RequestId request_id(settings_.client_id, session);
  const uint64_t sequence = request_id.to_uint32();
  someip::Message request({service_id_, method}, request_id,
                          callback ? someip::MessageType::REQUEST : someip::MessageType::REQUEST_NO_RETURN,
                          someip::ReturnCode::E_OK);
  request.set_interface_version(settings_.interface_version);
  request.set_payload(req_data.data(), req_data.size());

  if VLIKELY (callback) {
    std::lock_guard lock(mtx_);

    if VUNLIKELY (!resp_callbacks_.try_emplace(sequence, method, std::move(callback)).second) {
      return false;
    }
  }

  if VUNLIKELY (!send_message(request)) {
    std::lock_guard lock(mtx_);
    resp_callbacks_.erase(sequence);

    return false;
  }

  if (seq_out) {
    *seq_out = sequence;
  }

  return true;
}

void SomeipClient::remove_response_callback(uint64_t seq) {
  std::lock_guard lock(mtx_);
  resp_callbacks_.erase(seq);
}

bool SomeipClient::is_connected() const { return connected_.load(std::memory_order_acquire); }

bool SomeipClient::is_receive_thread() const {
  return receive_thread_id_.load(std::memory_order_acquire) == Utils::get_native_thread_id();
}

bool SomeipClient::send_message(someip::Message& message) {
  std::lock_guard send_lock(send_mtx_);

  if VUNLIKELY (settings_.e2e_enabled && e2e_.protect(message, settings_.e2e) != someip::Result::SUCCESS) {
    VLOG_E("SomeipClient: OpenSOMEIP E2E protection failed.");
    return false;
  }

  const bool success = transport_ && transport_->send_message(message, remote_endpoint_) == someip::Result::SUCCESS;

  if VUNLIKELY (settings_.e2e_enabled) {
    message.clear_e2e_header();
  }

  return success;
}

bool SomeipClient::send_control(uint8_t operation, const SomeipConf::Groups& groups) {
  uint16_t session = next_session_id_.fetch_add(1, std::memory_order_relaxed);

  if VUNLIKELY (session == 0) {
    session = next_session_id_.fetch_add(1, std::memory_order_relaxed);
  }

  someip::Message message({service_id_, kControlMethod}, {settings_.client_id, session}, someip::MessageType::REQUEST,
                          someip::ReturnCode::E_OK);
  message.set_interface_version(settings_.interface_version);
  message.set_payload(encode_control(operation, groups));

  return send_message(message);
}

void SomeipClient::update_remote_endpoint(const someip::sd::ServiceInstance& instance) {
  if (instance.service_id != service_id_ || instance.instance_id != instance_id_ || instance.ip_address.empty() ||
      instance.port == 0) {
    return;
  }

  bool endpoint_changed = false;

  {
    std::lock_guard lock(send_mtx_);
    const auto protocol =
        settings_.use_tcp ? someip::transport::TransportProtocol::TCP : someip::transport::TransportProtocol::UDP;
    someip::transport::Endpoint endpoint(instance.ip_address, instance.port, protocol);

    if VUNLIKELY (!endpoint.is_ipv4()) {
      return;
    }

    endpoint_changed = remote_endpoint_ != endpoint;
    remote_endpoint_ = std::move(endpoint);
  }

  const bool was_connected = connected_.load(std::memory_order_acquire);
  update_connected(true);

  if (!endpoint_changed || !was_connected) {
    return;
  }

  SomeipConf::Groups groups;

  {
    std::lock_guard lock(mtx_);

    for (const auto& entry : group_ref_counts_) {
      groups.emplace(entry.first);
    }
  }

  if (!groups.empty()) {
    static_cast<void>(send_control(kControlSubscribe, groups));
  }
}

void SomeipClient::update_connected(bool connected) {
  if (connected) {
    std::lock_guard lock(mtx_);
    last_message_time_ = std::chrono::steady_clock::now();
  }

  if (connected_.exchange(connected, std::memory_order_acq_rel) == connected) {
    return;
  }

  if (connected) {
    SomeipConf::Groups groups;

    {
      std::lock_guard lock(mtx_);

      for (const auto& entry : group_ref_counts_) {
        groups.emplace(entry.first);
      }
    }

    if (!groups.empty()) {
      static_cast<void>(send_control(kControlSubscribe, groups));
    }
  }

  traverse_server_connect_callback([connected](NodeImpl*, const auto& callback) { callback(connected); });
}

void SomeipClient::monitor_connection() {
  while (monitor_running_.load(std::memory_order_acquire) && has_impl()) {
    if (connected_.load(std::memory_order_acquire)) {
      std::chrono::steady_clock::time_point last_message;

      {
        std::lock_guard lock(mtx_);
        last_message = last_message_time_;
      }

      if (settings_.connection_timeout.count() > 0 &&
          std::chrono::steady_clock::now() - last_message > settings_.connection_timeout) {
        update_connected(false);
      }
    }

    static_cast<void>(send_control(kControlProbe, {}));

    std::unique_lock lock(monitor_mtx_);
    monitor_cv_.wait_for(lock, settings_.probe_interval,
                         [this] { return !monitor_running_.load(std::memory_order_acquire); });
  }
}

void SomeipClient::on_message_received(someip::MessagePtr message, const someip::transport::Endpoint& sender) {
  auto keep_alive = weak_from_this().lock();

  if VUNLIKELY (!keep_alive) {
    return;
  }

  receive_thread_id_.store(Utils::get_native_thread_id(), std::memory_order_release);

  if VUNLIKELY (!message) {
    return;
  }

  const auto* received = message.get();

  if VUNLIKELY (received->get_service_id() != service_id_ ||
                received->get_interface_version() != settings_.interface_version) {
    return;
  }

  {
    std::lock_guard lock(send_mtx_);

    if VUNLIKELY (sender != remote_endpoint_) {
      return;
    }
  }

  if VUNLIKELY (settings_.e2e_enabled) {
    const auto& payload = message->get_payload();
    someip::e2e::E2EHeader header;

    if VUNLIKELY (payload.size() < someip::e2e::E2EHeader::get_header_size() || !header.deserialize(payload)) {
      VLOG_E("SomeipClient: Rejected a message that failed OpenSOMEIP E2E validation.");
      return;
    }

    someip::platform::ByteBuffer actual_payload(payload.begin() + someip::e2e::E2EHeader::get_header_size(),
                                                payload.end());
    message->set_e2e_header(header);
    message->set_payload(std::move(actual_payload));

    if VUNLIKELY (e2e_.validate(*message, settings_.e2e) != someip::Result::SUCCESS) {
      VLOG_E("SomeipClient: Rejected a message that failed OpenSOMEIP E2E validation.");
      return;
    }
  }

  update_connected(true);

  if (received->get_message_type() == someip::MessageType::NOTIFICATION) {
    const auto& payload = received->get_payload();
    const Bytes data = Bytes::shallow_copy(payload.data(), payload.size());

    traverse_msg_callback([received, &data](NodeImpl* impl, const auto& callback) {
      const auto* conf = impl->get_target_conf<SomeipConf>();

      if VLIKELY (conf->event == received->get_method_id() && !impl->has_suspend) {
        callback(data);
      }
    });

    return;
  }

  if VUNLIKELY (!received->is_response()) {
    return;
  }

  if (is_control_message(*received)) {
    return;
  }

  NodeImpl::MsgCallback callback;

  {
    std::lock_guard lock(mtx_);
    const auto sequence = received->get_request_id().to_uint32();
    const auto iter = resp_callbacks_.find(sequence);

    if VUNLIKELY (iter == resp_callbacks_.end()) {
      return;
    }

    if VUNLIKELY (iter->second.first != received->get_method_id()) {
      return;
    }

    callback = std::move(iter->second.second);
    resp_callbacks_.erase(iter);
  }

  const auto& payload = received->get_payload();
  callback(Bytes::shallow_copy(payload.data(), payload.size()));
}

void SomeipClient::on_connection_lost(const someip::transport::Endpoint&) {
  auto keep_alive = weak_from_this().lock();

  if VUNLIKELY (!keep_alive) {
    return;
  }

  connection_thread_id_.store(Utils::get_native_thread_id(), std::memory_order_release);
  update_connected(false);
}

void SomeipClient::on_connection_established(const someip::transport::Endpoint&) {
  auto keep_alive = weak_from_this().lock();

  if VUNLIKELY (!keep_alive) {
    return;
  }

  connection_thread_id_.store(Utils::get_native_thread_id(), std::memory_order_release);
  update_connected(true);
}

void SomeipClient::on_error(someip::Result error) {
  auto keep_alive = weak_from_this().lock();

  if VUNLIKELY (!keep_alive) {
    return;
  }

  connection_thread_id_.store(Utils::get_native_thread_id(), std::memory_order_release);
  VLOG_E("SomeipClient: OpenSOMEIP transport error: ", someip::to_string(error));
}

}  // namespace vlink
