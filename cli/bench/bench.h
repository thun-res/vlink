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

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace vlink::bench {

class Bench final {
 public:
  enum Suite : uint8_t {
    kThroughputSuite = 0,
    kLatencySuite = 1,
    kTopologySuite = 2,
    kSerializationSuite = 3,
    kBackpressureSuite = 4,
  };

  enum Mode : uint8_t {
    kLocalDirectMode = 0,
    kLocalLoopMode = 1,
    kProcessMode = 2,
  };

  enum Topology : uint8_t {
    kOneToOneTopology = 0,
    kOneToManyTopology = 1,
    kManyToOneTopology = 2,
    kManyToManyTopology = 3,
  };

  enum RatePattern : uint8_t {
    kMaxRatePattern = 0,
    kFixedRatePattern = 1,
    kBurstRatePattern = 2,
  };

  enum PayloadKind : uint8_t {
    kBytesPayload = 0,
    kStringPayload = 1,
    kRawDataPayload = 2,
  };

  struct Scenario final {
    Suite suite{kThroughputSuite};
    Mode mode{kLocalLoopMode};
    Topology topology{kOneToOneTopology};
    RatePattern rate_pattern{kMaxRatePattern};
    PayloadKind payload{kBytesPayload};
    std::string url;
    std::string qos_profile;
    std::vector<std::string> properties;
    std::vector<std::string> pub_properties;
    std::vector<std::string> sub_properties;
    size_t payload_size{0};
    int rate_hz{0};
    int burst_messages{1};
    int subscribers{1};
    int publishers{1};
    int warmup_ms{1000};
    int duration_ms{3000};
    int drain_ms{300};
    int repeat_index{0};
    int subscriber_sleep_us{0};
  };

  struct ScenarioResult final {
    Scenario scenario;
    std::string transport;
    size_t wire_size{0};
    bool success{false};
    std::string error;

    uint64_t sent{0};
    uint64_t received{0};
    uint64_t expected{0};
    uint64_t lost{0};

    double discovery_ms{0.0};
    double first_message_ms{0.0};

    double send_msgs_per_sec{0.0};
    double recv_msgs_per_sec{0.0};
    double send_mb_per_sec{0.0};
    double recv_mb_per_sec{0.0};

    double avg_latency_us{0.0};
    double p50_latency_us{0.0};
    double p90_latency_us{0.0};
    double p95_latency_us{0.0};
    double p99_latency_us{0.0};
    double p999_latency_us{0.0};
    double p9999_latency_us{0.0};
    double max_latency_us{0.0};
    double latency_stddev_us{0.0};
    uint64_t latency_samples_dropped{0};

    double avg_send_block_us{0.0};
    double p50_send_block_us{0.0};
    double p95_send_block_us{0.0};
    double p99_send_block_us{0.0};
    double max_send_block_us{0.0};
    uint64_t send_block_samples{0};

    double serialize_msgs_per_sec{0.0};
    double deserialize_msgs_per_sec{0.0};
    double serialize_mb_per_sec{0.0};
    double deserialize_mb_per_sec{0.0};

    double pub_cpu_ms{0.0};
    double sub_cpu_ms{0.0};
    double cpu_usage{0.0};
    double memory_usage{0.0};
  };

  struct Result final {
    std::string version;
    std::string created_at;
    std::string host_name;
    std::string platform;
    std::string command_line;
    size_t planned_case_count{0};
    size_t skipped_case_count{0};
    std::vector<std::string> skip_messages;
    std::vector<ScenarioResult> scenarios;
  };

  struct RunOptions final {
    std::vector<Suite> suites;
    std::vector<Mode> modes;
    std::vector<Topology> topologies;
    std::vector<RatePattern> rate_patterns;
    std::vector<PayloadKind> payloads;
    std::vector<std::string> urls;
    std::vector<std::string> qos_profiles;
    std::vector<std::string> properties;
    std::vector<std::string> pub_properties;
    std::vector<std::string> sub_properties;
    std::vector<size_t> payload_sizes;
    std::vector<size_t> latency_sizes;
    std::vector<size_t> topology_sizes;
    std::vector<int> latency_rates;
    std::vector<int> fanout_subscribers;
    std::vector<int> publisher_counts;
    std::vector<int> burst_messages;
    std::vector<int> backpressure_sleep_us;
    std::string executable_path;
    int warmup_ms{1000};
    int duration_ms{3000};
    int drain_ms{300};
    int serialization_duration_ms{1000};
    int repeat_count{1};
    bool payloads_explicit{false};
    bool payload_sizes_explicit{false};
    bool latency_sizes_explicit{false};
    bool topology_sizes_explicit{false};
    bool latency_rates_explicit{false};
    bool burst_messages_explicit{false};
    bool verbose{false};
  };

  struct WorkerOptions final {
    PayloadKind payload{kBytesPayload};
    std::string url;
    std::string qos_profile;
    std::vector<std::string> properties;
    std::vector<std::string> pub_properties;
    std::vector<std::string> sub_properties;
    RatePattern rate_pattern{kMaxRatePattern};
    size_t payload_size{0};
    int rate_hz{0};
    int burst_messages{1};
    int warmup_ms{1000};
    int duration_ms{3000};
    int drain_ms{300};
    int publisher_index{0};
    int subscriber_sleep_us{0};
    bool enable_latency{false};
    bool wait_start{false};
    std::string result_file;
    bool verbose{false};
  };

  static const char* suite_to_string(Suite suite) noexcept;

  static const char* mode_to_string(Mode mode) noexcept;

  static const char* topology_to_string(Topology topology) noexcept;

  static const char* rate_pattern_to_string(RatePattern pattern) noexcept;

  static const char* payload_to_string(PayloadKind payload) noexcept;

  static bool parse_suite(const std::string& value, Suite& suite) noexcept;

  static bool parse_mode(const std::string& value, Mode& mode) noexcept;

  static bool parse_topology(const std::string& value, Topology& topology) noexcept;

  static bool parse_rate_pattern(const std::string& value, RatePattern& pattern) noexcept;

  static bool parse_payload(const std::string& value, PayloadKind& payload) noexcept;

  static void request_stop() noexcept;

  static void reset_stop() noexcept;

  static bool stop_requested() noexcept;

  static bool run(const RunOptions& options, Result& result, std::string& error);

  static bool save_json(const Result& result, const std::string& file_path, std::string& error);

  static bool load_json(const std::string& file_path, Result& result, std::string& error);

  static int run_pub_worker(const WorkerOptions& options);

  static int run_sub_worker(const WorkerOptions& options);
};

}  // namespace vlink::bench
