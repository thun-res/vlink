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

#include "./check_common.h"

struct EnvInfo final {
  std::string env_key;
  std::string env_value;
  std::string description;
  bool has_set{false};
};

int check_env(bool available_case, const std::string& prefix) {
  std::vector<EnvInfo> env_list = {
      {"VLINK_PROTO_DIR", "", "Search directory for .proto schemas.", false},
      {"VLINK_FBS_DIR", "", "Search directory for .fbs schemas.", false},
      {"VLINK_SCHEMA_PLUGIN", "", "Plugin that loads Protobuf and FlatBuffers schemas.", false},
      {"VLINK_TMP_DIR", "", "Temporary file directory.", false},
      {"VLINK_LOCK_DIR", "", "Singleton lock file directory.", false},
      {"VLINK_MEMORY_LEVEL", "", "Memory pool level (0 disables pooling, 1..9 tune capacity, default 3).", false},
      {"VLINK_MEMORY_PREALLOC", "", "Set to 1 to preallocate all tiers when the memory pool is created.", false},
      {"VLINK_MEMORY_BATCH_SIZE", "", "Blocks per free-list transfer (default 16).", false},
      {"VLINK_MEMORY_LAZY_SCALE", "", "Set to 1 to scale memory pool growth by tier capacity.", false},
      {"VLINK_PLUGIN_DIR", "", "Search directory for dynamic plugins.", false},
      {"VLINK_TRANSPORT_PLUGINS", "", "Use auto for on-demand loading, none to disable, or a preload list.", false},
      {"VLINK_URL_PLUGINS", "", "Fallback when VLINK_TRANSPORT_PLUGINS is unset or empty.", false},
      {"VLINK_URL_REMAP", "", "JSON file with URL rewrite rules.", false},

      {"VLINK_LOG_LEVEL", "", "Global log level (TRACE=0, DEBUG=1, INFO=2, WARN=3, ERROR=4, FATAL=5, OFF=6).", false},
      {"VLINK_LOG_CONSOLE_LEVEL", "", "Log level for console output.", false},
      {"VLINK_LOG_FILE_LEVEL", "", "Log level for file output and custom logger plugins.", false},
      {"VLINK_LOG_CONSOLE_UNORDER", "", "Set to 1 to allow unordered console output.", false},
      {"VLINK_LOG_CONSOLE_FMT", "", "Console format (0=minimal, 1=extended).", false},
      {"VLINK_LOG_DIR", "", "Log root directory. Each application has its own subdirectory.", false},
      {"VLINK_LOG_PID_ENABLE", "", "Set to 1 to keep a separate log directory for each process ID.", false},
      {"VLINK_LOG_UTC_ENABLE", "", "Set to 1 for UTC log timestamps.", false},
      {"VLINK_LOG_MAX_SIZE", "", "Maximum log file size in bytes (default 10 MiB).", false},
      {"VLINK_LOG_MAX_COUNT", "", "Timestamped files to keep (1..10000) or fixed-name backups (0..200000).", false},
      {"VLINK_LOG_FLUSH_DELAY", "", "Flush interval in ms (default 500, 0 flushes every record).", false},
      {"VLINK_LOG_PLUGIN", "", "Custom logger plugin base name, without a library prefix or suffix.", false},
      {"VLINK_LOG_STORE_STRATEGY", "", "Rotated log names are timestamped (default) or fixed (1).", false},
      {"VLINK_LOG_OPEN_APPEND", "", "Set to 1 to append to the latest log at startup.", false},
      {"VLINK_LOG_BLOCK_SYNC", "", "Set to 1 to block on a full queue. Errors and fatal records always block.", false},
      {"VLINK_LOG_WRITE_DEPTH", "", "Async log queue capacity in records (default 8192).", false},

      {"VLINK_BAG_PATH", "", "Record published messages and field updates to a .vdb or .vcap path.", false},
      {"VLINK_BAG_TAG", "", "Recording session tag (default Empty).", false},

      {"VLINK_DISCOVERY_DISABLE", "", "Set to 1 to disable cross-process discovery reporting.", false},
      {"VLINK_DISCOVERY_NATIVE", "", "Set to 1 for loopback-only discovery multicast.", false},
      {"VLINK_DISCOVERY_NATIVE_IP", "", "Fallback for VLINK_DDS_NATIVE_IP (default 127.0.0.1).", false},
      {"VLINK_DISCOVERY_IP", "", "Local IPv4 list for discovery multicast (comma or space separated).", false},
      {"VLINK_DISCOVERY_DOMAIN", "", "Discovery domain (0..255, default 0). Peers must match.", false},
      {"VLINK_PROFILER_ENABLE", "", "CPU profiler switch (0=off, 1=on). Default set at build time.", false},
      {"VLINK_QOS_CONFIG", "", "Path to the VLink QoS profile file.", false},

#ifdef VLINK_SUPPORT_INTRA
      {"VLINK_INTRA_BIND", "", "Use this transport for intra:// URLs.", false},
#endif

#if defined(VLINK_SUPPORT_DDS) || defined(VLINK_SUPPORT_DDSC) || defined(VLINK_SUPPORT_DDSR)
      {"VLINK_DDS_BIND", "", "Backend for dds:// URLs (dds/ddsf, ddsc, or ddsr).", false},
      {"VLINK_DDS_DEBUG", "", "Set to 1 for verbose DDS backend logs (default 0).", false},
      {"VLINK_DDS_EVENT_QOS", "", "Default QoS profile name for DDS Publisher/Subscriber nodes.", false},
      {"VLINK_DDS_METHOD_QOS", "", "Default QoS profile name for DDS Client/Server nodes.", false},
      {"VLINK_DDS_FIELD_QOS", "", "Default QoS profile name for DDS Setter/Getter nodes.", false},
      {"VLINK_DDS_DOMAIN", "", "DDS domain id for this process (valid range 0-232).", false},
      {"VLINK_DDS_IP", "", "Unicast IPv4 list advertised by DDS discovery (comma or space separated).", false},
      {"VLINK_DDS_NATIVE_IP", "", "Native-mode DDS IP. Falls back to VLINK_DISCOVERY_NATIVE_IP.", false},
      {"VLINK_DDS_IP_FILTER", "", "Set to 1 to keep only local addresses from VLINK_DDS_IP.", false},
      {"VLINK_DDS_MULTICAST_IP", "", "Discovery multicast IP list (ddsc only checks whether it is set).", false},
      {"VLINK_DDS_PEER", "", "Initial DDS discovery peers (comma or space separated).", false},
      {"VLINK_DDS_BUF", "", "DDS socket send/recv buffer size hint in bytes.", false},
      {"VLINK_DDS_MTU", "", "Maximum DDS payload size in bytes before fragmentation.", false},
      {"VLINK_DDS_UDP", "", "DDS UDP transport (1=on, default 1).", false},
      {"VLINK_DDS_TCP", "", "DDS TCP transport (1=on, default 0).", false},
      {"VLINK_DDS_SHM", "", "DDS shared-memory transport (1=on, default 0).", false},
      {"VLINK_DDS_NOBLOCK", "", "Enables non-blocking DDS sends (default 0).", false},
      {"VLINK_DDS_LESS_MEMORY", "", "Set to 1 to reduce DDS memory use at the cost of throughput.", false},
#endif

#ifdef VLINK_SUPPORT_DDS
      {"VLINK_FASTDDS_QOS_FILE", "", "Path to a Fast-DDS XML QoS profile file.", false},
#endif

#ifdef VLINK_SUPPORT_DDSC
      {"VLINK_CYCLONEDDS_URI", "", "Cyclone DDS config URI (file://..., <CycloneDDS>... inline XML).", false},
#endif

#ifdef VLINK_SUPPORT_SHM
      {"VLINK_SHM_DEBUG", "", "When set to 1 enables verbose logs in the SHM / iceoryx factory.", false},
      {"VLINK_SHM_DEPTH", "", "Queue depth used when creating iceoryx publishers / subscribers.", false},
#endif

#ifdef VLINK_SUPPORT_SHM2
      {"VLINK_SHM2_DEBUG", "", "When set to 1 enables verbose logs in the SHM2 / iceoryx2 factory.", false},
      {"VLINK_SHM2_DEPTH", "", "Queue depth used when creating iceoryx2 publishers / subscribers.", false},
      {"VLINK_SHM2_CONFIG", "", "Path to the iceoryx2 TOML configuration file.", false},
      {"VLINK_SHM2_NOTIFY_EVERY", "", "Notify once per N published messages (default 1).", false},
#endif

#ifdef VLINK_SUPPORT_ZENOH
      {"VLINK_ZENOH_CONFIG", "", "Path to the Zenoh JSON5 session configuration file.", false},
      {"VLINK_ZENOH_DEBUG", "", "Set to 1 for zenoh-c debug logs (default 0).", false},
      {"VLINK_ZENOH_DOMAIN", "", "Zenoh domain id (numeric, used to scope key expressions).", false},
      {"VLINK_ZENOH_MODE", "", "Zenoh session mode: peer / client / router (default peer).", false},
      {"VLINK_ZENOH_IP", "", "Zenoh peer IP list expanded to connect endpoints (comma or space separated).", false},
      {"VLINK_ZENOH_PEER", "", "Initial peer endpoint list (comma or space separated, e.g. tcp/host:7447).", false},
      {"VLINK_ZENOH_LISTEN", "", "Zenoh listen endpoints (comma or space separated).", false},
      {"VLINK_ZENOH_MULTICAST", "", "Zenoh multicast scout address (default 239.255.0.100).", false},
      {"VLINK_ZENOH_MULTICAST_IF", "", "Network interface used for Zenoh multicast scouting.", false},
      {"VLINK_ZENOH_MULTICAST_TTL", "", "TTL for Zenoh multicast scouting packets.", false},
      {"VLINK_ZENOH_GOSSIP", "", "When set to 1 enables Zenoh gossip discovery (default 0).", false},
      {"VLINK_ZENOH_RX_BUF", "", "Zenoh receive buffer size hint in bytes.", false},
      {"VLINK_ZENOH_MAX_MSG", "", "Maximum Zenoh message size in bytes before fragmentation.", false},
      {"VLINK_ZENOH_TX_QUEUE_DATA", "", "Zenoh data send queue depth.", false},
      {"VLINK_ZENOH_TX_QUEUE_RT", "", "Zenoh real-time send queue depth.", false},
      {"VLINK_ZENOH_LOWLATENCY", "", "When set to 1 enables Zenoh low-latency tuning (default 0).", false},
      {"VLINK_ZENOH_QOS", "", "When set to 1 enables Zenoh QoS network priorities (default 1).", false},
      {"VLINK_ZENOH_COMPRESSION", "", "When set to 1 enables Zenoh payload compression (default 0).", false},
      {"VLINK_ZENOH_TIMESTAMPS", "", "When set to 1 attaches HLC timestamps to Zenoh samples (default 0).", false},
      {"VLINK_ZENOH_BATCH_ENABLED", "", "Zenoh batch publishing (true by default).", false},
      {"VLINK_ZENOH_BATCH_TIME_LIMIT_MS", "", "Zenoh batch coalescing window in milliseconds (default 1).", false},
      {"VLINK_ZENOH_ALLOWED_LOCALITY", "", "Allowed origin (local, remote, or any). Requires the unstable API.", false},
      {"VLINK_ZENOH_EVENT_QOS", "", "Default QoS profile name for Zenoh Publisher/Subscriber nodes.", false},
      {"VLINK_ZENOH_METHOD_QOS", "", "Default QoS profile name for Zenoh Client/Server nodes.", false},
      {"VLINK_ZENOH_FIELD_QOS", "", "Default QoS profile name for Zenoh Setter/Getter nodes.", false},
      {"VLINK_ZENOH_SHM", "", "Set to 1 for Zenoh SHM. Requires zenoh-c with SHM and unstable APIs.", false},
      {"VLINK_ZENOH_SHM_MODE", "", "Zenoh SHM provider init mode: 'init' (default, eager) / 'lazy'.", false},
      {"VLINK_ZENOH_SHM_SIZE", "", "Zenoh shared-memory pool size (B/K/M/G).", false},
      {"VLINK_ZENOH_SHM_THRESHOLD", "", "Minimum message size in bytes for automatic Zenoh SHM use.", false},
      {"VLINK_ZENOH_SHM_LOAN_THRESHOLD", "", "Minimum SHM loan size (default 8192 bytes, accepts B/K/M/G).", false},
      {"VLINK_ZENOH_SHM_BLOCKING", "", "Set to 1 to wait for memory when the Zenoh SHM loan pool is full.", false},
#endif

#ifdef VLINK_SUPPORT_MQTT
      {"VLINK_MQTT_BROKER", "", "MQTT broker endpoint (default tcp://localhost:1883).", false},
      {"VLINK_MQTT_CLIENT_ID", "", "MQTT client ID prefix (default vlink_mqtt, PID/UUID appended).", false},
      {"VLINK_MQTT_DOMAIN", "", "Domain id mixed into MQTT topic prefixes for tenant isolation.", false},
      {"VLINK_MQTT_KEEPALIVE", "", "MQTT keepalive interval in seconds (default 60).", false},
      {"VLINK_MQTT_QOS", "", "MQTT default QoS level for published messages (0 / 1 / 2).", false},
#endif

#ifdef VLINK_SUPPORT_SOMEIP
      {"VLINK_SOMEIP_CFG", "", "Path to the vSomeIP JSON configuration file.", false},
#endif

      {"VLINK_SSL_VERIFY", "", "Enable TLS certificate verification.", false},
      {"VLINK_SSL_CA", "", "Path to the TLS CA bundle.", false},
      {"VLINK_SSL_CERT", "", "Path to the TLS client certificate.", false},
      {"VLINK_SSL_KEY", "", "Path to the TLS private key.", false},
      {"VLINK_SSL_KEY_PASS", "", "Password used to decrypt the TLS private key.", false},
      {"VLINK_SSL_CIPHERS", "", "Allowed TLS cipher list.", false},
      {"VLINK_SSL_SNI", "", "Server Name Indication override for TLS.", false},
  };

  int set_count = 0;
  int matched_count = 0;
  int shown_count = 0;

  for (auto& info : env_list) {
    if (!prefix.empty() && !vlink::Helpers::has_startwith(info.env_key, prefix)) {
      continue;
    }

    ++matched_count;
    std::string env_value = vlink::Utils::get_env(info.env_key);

    if (!env_value.empty()) {
      info.has_set = true;
      ++set_count;
    }

    info.env_value = std::move(env_value);

    if (info.has_set) {
      std::cout << kColorPass;
      std::cout << "[" << info.env_key << "]: ";
      std::cout << (info.env_key == "VLINK_SSL_KEY_PASS" ? "<redacted>" : info.env_value.c_str());
      std::cout << kColorReset << std::endl;

      std::cout << info.description << std::endl << std::endl;
      ++shown_count;
      continue;
    }

    if (available_case) {
      continue;
    }

    std::cout << kColorFail;
    std::cout << "[" << info.env_key << "]";
    std::cout << kColorReset << std::endl;

    std::cout << info.description << std::endl << std::endl;
    ++shown_count;
  }

  std::cout << kColorInfo << "Summary: " << set_count << "/" << matched_count << " VLink environment variables set";

  if (!prefix.empty()) {
    std::cout << " (shown " << shown_count << " matching prefix \"" << prefix << "\")";
  }

  std::cout << kColorReset << std::endl;

  return 0;
}
