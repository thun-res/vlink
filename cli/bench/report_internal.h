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
#include <limits>
#include <map>
#include <string>
#include <vector>

#include "./bench.h"

namespace vlink::bench::report {

enum class DeploymentLayer : uint8_t {
  kInProcess = 0,
  kCrossProcess = 1,
  kUnknown = 2,
};

struct MetricSummary final {
  size_t count{0};
  double sum{0.0};
  double min{0.0};
  double max{0.0};

  void add(double value);

  double average() const noexcept;
};

struct AggregatedCase final {
  Bench::Scenario scenario;
  std::string transport;
  size_t wire_size{0};
  size_t sample_count{0};
  size_t success_count{0};
  size_t failure_count{0};
  uint64_t latency_samples_dropped{0};
  std::vector<std::string> errors;

  DeploymentLayer deployment_layer{DeploymentLayer::kCrossProcess};
  bool is_intra_rewritten{false};
  std::string endpoint_key;

  int url_order_index{-1};

  double speed_score{-1.0};
  double capacity_score{-1.0};
  double efficiency_score{-1.0};
  double sort_score{-1.0};
  double worst_run_loss_pct{-1.0};
  double worst_run_latency_drop_pct{-1.0};
  std::string confidence_label;

  MetricSummary sent;
  MetricSummary received;
  MetricSummary expected;
  MetricSummary lost;

  MetricSummary discovery_ms;
  MetricSummary first_message_ms;

  MetricSummary send_msgs_per_sec;
  MetricSummary recv_msgs_per_sec;
  MetricSummary send_mb_per_sec;
  MetricSummary recv_mb_per_sec;

  MetricSummary avg_latency_us;
  MetricSummary p50_latency_us;
  MetricSummary p90_latency_us;
  MetricSummary p95_latency_us;
  MetricSummary p99_latency_us;
  MetricSummary p999_latency_us;
  MetricSummary p9999_latency_us;
  MetricSummary max_latency_us;
  MetricSummary latency_stddev_us;

  MetricSummary avg_send_block_us;
  MetricSummary p50_send_block_us;
  MetricSummary p95_send_block_us;
  MetricSummary p99_send_block_us;
  MetricSummary max_send_block_us;
  MetricSummary send_block_samples;

  MetricSummary serialize_msgs_per_sec;
  MetricSummary deserialize_msgs_per_sec;
  MetricSummary serialize_mb_per_sec;
  MetricSummary deserialize_mb_per_sec;

  MetricSummary pub_cpu_ms;
  MetricSummary sub_cpu_ms;
  MetricSummary cpu_usage;
  MetricSummary memory_usage;

  bool has_success() const noexcept;

  bool all_success() const noexcept;
};

struct ChartSeries final {
  std::string id;
  std::string color;
  std::string dash;
  std::string label;
  std::string detail;
  int url_order_index{std::numeric_limits<int>::max()};
  std::map<double, MetricSummary> points;
};

struct ChartPanel final {
  std::string title;
  std::string body;
};

enum class SeriesRateLabelMode : uint8_t {
  kOmit = 0,
  kIncludePattern = 1,
  kIncludeRate = 2,
};

void add_metric_sample(AggregatedCase& item, const Bench::ScenarioResult& result);
std::string build_case_key(const Bench::ScenarioResult& result);
std::string build_endpoint_key(const AggregatedCase& item);
std::vector<AggregatedCase> aggregate_cases(const Bench::Result& result);

}  // namespace vlink::bench::report
