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
      {"VLINK_PROTO_DIR", "", "Directory scanned for .proto schemas by the Protobuf registry.", false},
      {"VLINK_FBS_DIR", "", "Directory scanned for .fbs schemas by the FlatBuffers registry.", false},
      {"VLINK_SCHEMA_PLUGIN", "", "Name of the dynamic plugin that loads Protobuf/FlatBuffers schemas.", false},
      {"VLINK_TMP_DIR", "", "Override directory used for temporary files.", false},
      {"VLINK_LOCK_DIR", "", "Override directory used for singleton lock files.", false},
      {"VLINK_MEMORY_LEVEL", "",
       "MemoryPool tier level (0..9, default 3). 0 = bypass; 1..9 select built-in pyramid (higher = more "
       "resident memory, fewer upstream allocs). Honoured only when Bytes::init_memory_pool() is called.",
       false},
      {"VLINK_MEMORY_PREALLOC", "",
       "Set to 1 to fill every tier to its full blocks_per_chunk quota when the global MemoryPool is built "
       "(best-effort).",
       false},
      {"VLINK_MEMORY_BATCH_SIZE", "",
       "Positive free-list shard transfer batch size used by the default MemoryPool configuration (default 16).",
       false},
      {"VLINK_PLUGIN_DIR", "", "Directory searched by vlink::Plugin when loading dynamic modules.", false},
      {"VLINK_URL_PLUGINS", "",
       "Set before the first URL initialization: auto enables first-use loading of recognized unlinked shared "
       "transports; empty or none disables plugin loading; any other non-empty value is a comma- or "
       "space-separated explicit preload list (case-insensitive auto/none).",
       false},
      {"VLINK_URL_REMAP", "", "Path to a JSON file describing URL rewrite rules applied at node creation.", false},

      {"VLINK_LOG_LEVEL", "",
       "Global log level: TRACE=0, DEBUG=1, INFO=2, WARN=3, ERROR=4, FATAL=5, OFF=6 (disable all output).", false},
      {"VLINK_LOG_CONSOLE_LEVEL", "", "Override of the log level used for console sink only.", false},
      {"VLINK_LOG_FILE_LEVEL", "", "Override of the log level used for the file sink and custom logger plugin.", false},
      {"VLINK_LOG_CONSOLE_UNORDER", "",
       "When set to 1 disables ordered console output (faster, may interleave across threads).", false},
      {"VLINK_LOG_CONSOLE_FMT", "",
       "When set to 1 enables VLink's extended console formatting (timestamp / thread / level color); empty/0 = "
       "minimal format. Boolean toggle, not a template string.",
       false},
      {"VLINK_LOG_DIR", "", "Directory where rotating log files are written.", false},
      {"VLINK_LOG_ENABLE_UTC", "", "When set to 1 prints timestamps in UTC instead of local time.", false},
      {"VLINK_LOG_MAX_SIZE", "", "Maximum size in bytes per log file before rotation (default 10485760 = 10 MiB).",
       false},
      {"VLINK_LOG_MAX_COUNT", "",
       "Timestamp retention target (1..10000), or fixed-name backup count (0..200000; active file excluded).", false},
      {"VLINK_LOG_FLUSH_DELAY", "",
       "LoggerBackend flush interval in milliseconds (default 500). ERROR also triggers a flush; 0 flushes every "
       "record.",
       false},
      {"VLINK_LOG_PLUGIN", "",
       "Custom logger plugin base name (no 'lib' prefix or '.so' suffix); implements LoggerPluginInterface and is "
       "resolved through Plugin::default_search_path().",
       false},
      {"VLINK_LOG_STORE_STRATEGY", "",
       "When set to 1 LoggerBackend uses fixed-name size rotation; default empty uses timestamped size rotation.",
       false},
      {"VLINK_LOG_OPEN_APPEND", "",
       "When set to 1 continues the active/latest file at startup; default 0 starts a new file or rotates the active "
       "file.",
       false},
      {"VLINK_LOG_BLOCK_SYNC", "",
       "When set to 1 blocks producer threads if the async log queue is full; default 0 allows drops "
       "(LoggerBackend drops an older dispatcher record). Error/Fatal records always block and remain protected.",
       false},
      {"VLINK_LOG_WRITE_DEPTH", "", "LoggerBackend MessageLoop dispatcher queue depth in records (default 8192).",
       false},

      {"VLINK_BAG_PATH", "",
       "Activates the process-global BagWriter (BagWriter::global_get()) at the given .vdb/.vcap path. All "
       "Publisher/Setter messages are auto-recorded transparently. CLI tools (vlink-bag/trigger/parse/eproto/efbs/"
       "list/monitor/bench) explicitly unset this on startup to avoid recursive recording.",
       false},
      {"VLINK_BAG_TAG", "", "User tag stored in bag metadata to label the recording session (default 'Empty').", false},

      {"VLINK_DISCOVER_DISABLE", "",
       "When set to 1 disables the runtime-owned discovery reporter (no cross-process visibility).", false},
      {"VLINK_DISCOVER_NATIVE", "",
       "When set to 1 restricts discovery multicast to the loopback interface (same-host only).", false},
      {"VLINK_PROFILER_ENABLE", "",
       "Toggles the built-in CpuProfiler (1=enable, 0=disable). Default depends on the compile-time macro "
       "VLINK_PROFILER_DEFAULT_STATE (0 in upstream).",
       false},
      {"VLINK_QOS_CONFIG", "", "Path to the VLink QoS profile file consumed by the extension layer.", false},

#ifdef VLINK_SUPPORT_INTRA
      {"VLINK_INTRA_BIND", "", "Rebinds intra:// URLs to another transport scheme (e.g. dds, shm) for this process.",
       false},
#endif

#if defined(VLINK_SUPPORT_DDS) || defined(VLINK_SUPPORT_DDSC) || defined(VLINK_SUPPORT_DDSR)
      {"VLINK_DDS_BIND", "",
       "Rebinds dds:// URLs to a specific DDS backend at runtime: dds (Fast-DDS) / ddsf (alias of dds) / ddsc "
       "(CycloneDDS) / ddsr (RTI). Empty = no rebind.",
       false},
      {"VLINK_DDS_DEBUG", "", "When set to 1 raises Fast-DDS / CycloneDDS / RTI internal log verbosity; default 0.",
       false},
      {"VLINK_DDS_EVENT_QOS", "", "Default QoS profile name for DDS Publisher/Subscriber nodes.", false},
      {"VLINK_DDS_METHOD_QOS", "", "Default QoS profile name for DDS Client/Server nodes.", false},
      {"VLINK_DDS_FIELD_QOS", "", "Default QoS profile name for DDS Setter/Getter nodes.", false},
      {"VLINK_DDS_DOMAIN", "", "DDS domain id for this process (valid range 0-232).", false},
      {"VLINK_DDS_IP", "", "Unicast IPv4 list advertised by DDS discovery (comma or space separated).", false},
      {"VLINK_DDS_NATIVE_IP", "",
       "DDS IP applied by native-mode CLI, Proxy, Viewer, and WebViz; defaults to 127.0.0.1 when unset.", false},
      {"VLINK_DDS_IP_FILTER", "", "When set to 1 filters VLINK_DDS_IP down to addresses currently present on the host.",
       false},
      {"VLINK_DDS_MULTICAST_IP", "",
       "DDS discovery multicast address list (comma or space separated; ddsc only uses whether it is set).", false},
      {"VLINK_DDS_PEER", "", "Initial peer list for DDS participant discovery (comma or space separated).", false},
      {"VLINK_DDS_BUF", "", "DDS socket send/recv buffer size hint in bytes.", false},
      {"VLINK_DDS_MTU", "", "Maximum DDS payload size in bytes before fragmentation.", false},
      {"VLINK_DDS_UDP", "", "Toggles UDP transport for DDS (default 1 = enabled; set 0 to disable).", false},
      {"VLINK_DDS_TCP", "", "Toggles TCP transport for DDS (default 0; set 1 to enable).", false},
      {"VLINK_DDS_SHM", "", "Toggles same-host shared-memory transport inside DDS (default 0; set 1 to enable).",
       false},
      {"VLINK_DDS_LESS_MEMORY", "",
       "When set to 1 trims DDS participant memory footprint at the cost of throughput; default 0.", false},
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
      {"VLINK_SHM2_NOTIFY_EVERY", "",
       "Notify-every-N coalescing for shm2:// publishers (default 1; raise to amortize wakeups).", false},
      {"VLINK_SHM2_LOAN_MIN", "",
       "Minimum non-loaned Bytes payload size that uses loan/write/send instead of send_slice_copy (default 65536).",
       false},
#endif

#ifdef VLINK_SUPPORT_ZENOH
      {"VLINK_ZENOH_CONFIG", "", "Path to the Zenoh JSON5 session configuration file.", false},
      {"VLINK_ZENOH_DEBUG", "", "When set to 1 enables Zenoh runtime debug logging in zenoh-c builds (default 0).",
       false},
      {"VLINK_ZENOH_DOMAIN", "", "Zenoh domain id (numeric, used to scope key expressions).", false},
      {"VLINK_ZENOH_MODE", "", "Zenoh session mode: peer / client / router (default peer).", false},
      {"VLINK_ZENOH_IP", "", "Zenoh peer IP list expanded to connect endpoints (comma or space separated).", false},
      {"VLINK_ZENOH_PEER", "", "Initial peer endpoint list (comma or space separated, e.g. tcp/host:7447).", false},
      {"VLINK_ZENOH_LISTEN", "", "Listen endpoint list for incoming Zenoh connections (comma or space separated).",
       false},
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
      {"VLINK_ZENOH_BATCH_ENABLED", "",
       "When set to 'true' enables batch publishing on the Zenoh session (default true).", false},
      {"VLINK_ZENOH_BATCH_TIME_LIMIT_MS", "", "Zenoh batch coalescing window in milliseconds (default 1).", false},
      {"VLINK_ZENOH_ALLOWED_LOCALITY", "",
       "Zenoh allowed origin: 'local' (session-local only) / 'remote' (remote only) / other (any). "
       "Requires Z_FEATURE_UNSTABLE_API. Default 'any'.",
       false},
      {"VLINK_ZENOH_EVENT_QOS", "", "Default QoS profile name for Zenoh Publisher/Subscriber nodes.", false},
      {"VLINK_ZENOH_METHOD_QOS", "", "Default QoS profile name for Zenoh Client/Server nodes.", false},
      {"VLINK_ZENOH_FIELD_QOS", "", "Default QoS profile name for Zenoh Setter/Getter nodes.", false},
      {"VLINK_ZENOH_SHM", "",
       "When set to 1 enables Zenoh shared-memory transport (default 0; "
       "requires zenoh-c built with Z_FEATURE_SHARED_MEMORY + Z_FEATURE_UNSTABLE_API).",
       false},
      {"VLINK_ZENOH_SHM_MODE", "", "Zenoh SHM provider init mode: 'init' (default, eager) / 'lazy'.", false},
      {"VLINK_ZENOH_SHM_SIZE", "",
       "Zenoh SHM transport pool size; accepts B/K/M/G suffix (transport_optimization/pool_size).", false},
      {"VLINK_ZENOH_SHM_THRESHOLD", "",
       "Zenoh auto-SHM promotion threshold in bytes (transport_optimization/message_size_threshold).", false},
      {"VLINK_ZENOH_SHM_LOAN_THRESHOLD", "",
       "Minimum size for which Node::loan() returns a Zenoh SHM buffer; smaller sizes fall back to "
       "Bytes::create heap (default 8192, accepts B/K/M/G).",
       false},
      {"VLINK_ZENOH_SHM_BLOCKING", "",
       "When set to 1 Node::loan() blocks waiting for SHM GC + defrag on pool exhaustion (default 0 = non-blocking).",
       false},
#endif

#ifdef VLINK_SUPPORT_MQTT
      {"VLINK_MQTT_BROKER", "", "MQTT broker endpoint (default tcp://localhost:1883).", false},
      {"VLINK_MQTT_CLIENT_ID", "", "MQTT client id prefix (default 'vlink_mqtt'; PID/UUID suffix appended).", false},
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
  int shown_count = 0;

  for (auto& info : env_list) {
    if (!prefix.empty() && !vlink::Helpers::has_startwith(info.env_key, prefix)) {
      continue;
    }

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

  std::cout << kColorInfo << "Summary: " << set_count << "/" << env_list.size() << " VLink environment variables set";

  if (!prefix.empty()) {
    std::cout << " (shown " << shown_count << " matching prefix \"" << prefix << "\")";
  }

  std::cout << kColorReset << std::endl;

  return 0;
}
