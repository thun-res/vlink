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

#include "./report_internal.h"

#include <algorithm>
#include <limits>
#include <map>
#include <set>
#include <string>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <vector>

#include "./report_helpers.h"
#include "./report_score.h"

namespace vlink::bench::report {

void MetricSummary::add(double value) {
  if (count == 0) {
    min = value;
    max = value;
  } else {
    min = std::min(min, value);
    max = std::max(max, value);
  }

  ++count;
  sum += value;
}

double MetricSummary::average() const noexcept {
  if (count == 0) {
    return 0.0;
  }

  return sum / static_cast<double>(count);
}

bool AggregatedCase::has_success() const noexcept { return success_count > 0; }

bool AggregatedCase::all_success() const noexcept {
  return success_count > 0 && failure_count == 0 && latency_samples_dropped == 0;
}

template <typename ValueT>
static void append_key_part(std::string& out, const ValueT& value) {
  auto text = std::to_string(value);
  out += std::to_string(text.size());
  out.push_back(':');
  out += text;
  out.push_back('|');
}

static void append_key_part(std::string& out, const std::string& value) {
  out += std::to_string(value.size());
  out.push_back(':');
  out += value;
  out.push_back('|');
}

static void append_key_part(std::string& out, const std::vector<std::string>& values) {
  append_key_part(out, values.size());

  for (const auto& value : values) {
    append_key_part(out, value);
  }
}

std::string build_endpoint_key(const AggregatedCase& item) {
  std::string key;
  append_key_part(key, static_cast<int>(item.deployment_layer));
  append_key_part(key, static_cast<int>(item.scenario.mode));
  append_key_part(key, item.transport);
  append_key_part(key, item.scenario.url);
  append_key_part(key, item.scenario.qos_profile);
  append_key_part(key, item.scenario.properties);
  append_key_part(key, item.scenario.pub_properties);
  append_key_part(key, item.scenario.sub_properties);
  return key;
}

void add_metric_sample(AggregatedCase& item, const Bench::ScenarioResult& result) {
  if (result.scenario.suite != Bench::kSerializationSuite) {
    item.sent.add(static_cast<double>(result.sent));
    item.received.add(static_cast<double>(result.received));
    item.expected.add(static_cast<double>(result.expected));
    item.lost.add(static_cast<double>(result.lost));

    if (result.expected > 0) {
      const auto pct = static_cast<double>(result.lost) * 100.0 / static_cast<double>(result.expected);

      if (item.worst_run_loss_pct < 0.0 || pct > item.worst_run_loss_pct) {
        item.worst_run_loss_pct = pct;
      }
    }

    if (result.latency_samples_dropped != 0) {
      const auto delivered = static_cast<double>(result.received);
      const auto dropped = static_cast<double>(result.latency_samples_dropped);
      const auto denominator = delivered > 0.0 ? delivered : dropped;

      if (denominator > 0.0) {
        const double pct = dropped * 100.0 / denominator;

        if (item.worst_run_latency_drop_pct < 0.0 || pct > item.worst_run_latency_drop_pct) {
          item.worst_run_latency_drop_pct = pct;
        }
      }
    }

    item.discovery_ms.add(result.discovery_ms);

    if (result.first_message_ms > 0.0) {
      item.first_message_ms.add(result.first_message_ms);
    }

    item.send_msgs_per_sec.add(result.send_msgs_per_sec);
    item.recv_msgs_per_sec.add(result.recv_msgs_per_sec);
    item.send_mb_per_sec.add(result.send_mb_per_sec);
    item.recv_mb_per_sec.add(result.recv_mb_per_sec);

    if (result.scenario.suite == Bench::kLatencySuite &&
        (result.received != 0 || result.latency_samples_dropped != 0)) {
      item.avg_latency_us.add(result.avg_latency_us);
      item.p50_latency_us.add(result.p50_latency_us);
      item.p90_latency_us.add(result.p90_latency_us);
      item.p95_latency_us.add(result.p95_latency_us);
      item.p99_latency_us.add(result.p99_latency_us);
      item.p999_latency_us.add(result.p999_latency_us);

      if (result.p9999_latency_us > 0.0) {
        item.p9999_latency_us.add(result.p9999_latency_us);
      }

      item.max_latency_us.add(result.max_latency_us);
      item.latency_stddev_us.add(result.latency_stddev_us);
      item.latency_samples_dropped += result.latency_samples_dropped;
    }

    if (result.send_block_samples > 0) {
      item.avg_send_block_us.add(result.avg_send_block_us);
      item.p50_send_block_us.add(result.p50_send_block_us);
      item.p95_send_block_us.add(result.p95_send_block_us);
      item.p99_send_block_us.add(result.p99_send_block_us);
      item.max_send_block_us.add(result.max_send_block_us);
      item.send_block_samples.add(static_cast<double>(result.send_block_samples));
    }
  } else {
    if (result.serialize_msgs_per_sec > 0.0) {
      item.serialize_msgs_per_sec.add(result.serialize_msgs_per_sec);
    }

    if (result.deserialize_msgs_per_sec > 0.0) {
      item.deserialize_msgs_per_sec.add(result.deserialize_msgs_per_sec);
    }

    if (result.serialize_mb_per_sec > 0.0) {
      item.serialize_mb_per_sec.add(result.serialize_mb_per_sec);
    }

    if (result.deserialize_mb_per_sec > 0.0) {
      item.deserialize_mb_per_sec.add(result.deserialize_mb_per_sec);
    }
  }

  if (result.pub_cpu_ms > 0.0) {
    item.pub_cpu_ms.add(result.pub_cpu_ms);
  }

  if (result.sub_cpu_ms > 0.0) {
    item.sub_cpu_ms.add(result.sub_cpu_ms);
  }

  if (result.cpu_usage > 0.0) {
    item.cpu_usage.add(result.cpu_usage);
  }

  if (result.memory_usage > 0.0) {
    item.memory_usage.add(result.memory_usage);
  }
}

std::string build_case_key(const Bench::ScenarioResult& result) {
  std::string key;
  key.reserve(320);

  append_key_part(key, static_cast<int>(result.scenario.suite));
  append_key_part(key, static_cast<int>(result.scenario.mode));
  append_key_part(key, static_cast<int>(result.scenario.topology));
  append_key_part(key, static_cast<int>(result.scenario.rate_pattern));
  append_key_part(key, static_cast<int>(result.scenario.payload));
  append_key_part(key, result.transport.empty() ? get_transport_from_url(result.scenario.url) : result.transport);
  append_key_part(key, result.scenario.url);
  append_key_part(key, result.scenario.qos_profile);
  append_key_part(key, result.scenario.properties);
  append_key_part(key, result.scenario.pub_properties);
  append_key_part(key, result.scenario.sub_properties);
  append_key_part(key, result.scenario.payload_size);
  append_key_part(key, result.scenario.rate_hz);
  append_key_part(key, result.scenario.burst_messages);
  append_key_part(key, result.scenario.subscriber_sleep_us);
  append_key_part(key, result.scenario.subscribers);
  append_key_part(key, result.scenario.publishers);
  append_key_part(key, result.scenario.warmup_ms);
  append_key_part(key, result.scenario.duration_ms);
  append_key_part(key, result.scenario.drain_ms);
  return key;
}

std::vector<AggregatedCase> aggregate_cases(const Bench::Result& result) {
  std::map<std::string, AggregatedCase> aggregated;

  std::unordered_map<std::string, int> url_order;
  for (const auto& sample : result.scenarios) {
    url_order.emplace(sample.scenario.url, static_cast<int>(url_order.size()));
  }

  for (const auto& sample : result.scenarios) {
    auto key = build_case_key(sample);
    auto [iter, inserted] = aggregated.try_emplace(key);
    auto& item = iter->second;

    if (inserted) {
      item.scenario = sample.scenario;
      item.scenario.repeat_index = 0;
      item.transport = sample.transport.empty() ? get_transport_from_url(sample.scenario.url) : sample.transport;
      item.wire_size = sample.wire_size;
    } else if (item.transport.empty()) {
      item.transport = sample.transport.empty() ? get_transport_from_url(sample.scenario.url) : sample.transport;
    }

    if (item.wire_size == 0 && sample.wire_size != 0) {
      item.wire_size = sample.wire_size;
    }

    ++item.sample_count;

    if (sample.success) {
      ++item.success_count;
      add_metric_sample(item, sample);
    } else {
      ++item.failure_count;
    }

    append_unique(item.errors, strip_ansi_escape_codes(sample.error));
  }

  std::vector<AggregatedCase> items;
  items.reserve(aggregated.size());

  for (auto& [key, item] : aggregated) {
    (void)key;
    auto it = url_order.find(item.scenario.url);
    item.url_order_index = it != url_order.end() ? it->second : -1;
    items.emplace_back(std::move(item));
  }

  auto collapse_worst_high = [](MetricSummary& s) {
    if (s.count <= 1) {
      return;
    }

    s.sum = s.max;
    s.count = 1;
  };
  auto collapse_worst_low = [](MetricSummary& s) {
    if (s.count <= 1) {
      return;
    }

    s.sum = s.min;
    s.count = 1;
  };

  for (auto& item : items) {
    if (item.success_count > 1) {
      collapse_worst_low(item.sent);
      collapse_worst_low(item.received);
      collapse_worst_high(item.expected);
      collapse_worst_high(item.lost);
      collapse_worst_high(item.discovery_ms);
      collapse_worst_high(item.first_message_ms);
      collapse_worst_low(item.send_msgs_per_sec);
      collapse_worst_low(item.recv_msgs_per_sec);
      collapse_worst_low(item.send_mb_per_sec);
      collapse_worst_low(item.recv_mb_per_sec);
      collapse_worst_high(item.avg_latency_us);
      collapse_worst_high(item.p50_latency_us);
      collapse_worst_high(item.p90_latency_us);
      collapse_worst_high(item.p95_latency_us);
      collapse_worst_high(item.p99_latency_us);
      collapse_worst_high(item.p999_latency_us);
      collapse_worst_high(item.p9999_latency_us);
      collapse_worst_high(item.max_latency_us);
      collapse_worst_high(item.latency_stddev_us);
      collapse_worst_high(item.avg_send_block_us);
      collapse_worst_high(item.p50_send_block_us);
      collapse_worst_high(item.p95_send_block_us);
      collapse_worst_high(item.p99_send_block_us);
      collapse_worst_high(item.max_send_block_us);
      collapse_worst_high(item.send_block_samples);
      collapse_worst_low(item.serialize_msgs_per_sec);
      collapse_worst_low(item.deserialize_msgs_per_sec);
      collapse_worst_low(item.serialize_mb_per_sec);
      collapse_worst_low(item.deserialize_mb_per_sec);
      collapse_worst_high(item.pub_cpu_ms);
      collapse_worst_high(item.sub_cpu_ms);
      collapse_worst_high(item.cpu_usage);
      collapse_worst_high(item.memory_usage);
    }
  }

  for (auto& item : items) {
    switch (item.scenario.mode) {
      case Bench::kLocalDirectMode:
      case Bench::kLocalLoopMode:
        item.deployment_layer = DeploymentLayer::kInProcess;
        break;
      case Bench::kProcessMode:
        item.deployment_layer = DeploymentLayer::kCrossProcess;
        break;
      default:
        item.deployment_layer = DeploymentLayer::kUnknown;
        break;
    }

    item.is_intra_rewritten = (item.transport == "intra" && item.scenario.mode == Bench::kLocalDirectMode);
    item.endpoint_key = build_endpoint_key(item);
  }

  {
    struct ScoreGroupKey final {
      int suite;
      int mode;
      int payload;
      size_t payload_size;
      int rate_hz;
      int deployment_layer;
      int topology;
      int publishers;
      int subscribers;
      int rate_pattern;
      int burst_messages;
      int subscriber_sleep_us;
      int warmup_ms;
      int duration_ms;
      int drain_ms;

      bool operator<(const ScoreGroupKey& o) const {
        return std::tie(suite, mode, payload, payload_size, rate_hz, deployment_layer, topology, publishers,
                        subscribers, rate_pattern, burst_messages, subscriber_sleep_us, warmup_ms, duration_ms,
                        drain_ms) < std::tie(o.suite, o.mode, o.payload, o.payload_size, o.rate_hz, o.deployment_layer,
                                             o.topology, o.publishers, o.subscribers, o.rate_pattern, o.burst_messages,
                                             o.subscriber_sleep_us, o.warmup_ms, o.duration_ms, o.drain_ms);
      }
    };

    auto make_key = [](const AggregatedCase& it) {
      return ScoreGroupKey{static_cast<int>(it.scenario.suite),
                           static_cast<int>(it.scenario.mode),
                           static_cast<int>(it.scenario.payload),
                           it.scenario.payload_size,
                           it.scenario.rate_hz,
                           static_cast<int>(it.deployment_layer),
                           static_cast<int>(it.scenario.topology),
                           it.scenario.publishers,
                           it.scenario.subscribers,
                           static_cast<int>(it.scenario.rate_pattern),
                           it.scenario.burst_messages,
                           it.scenario.subscriber_sleep_us,
                           it.scenario.warmup_ms,
                           it.scenario.duration_ms,
                           it.scenario.drain_ms};
    };

    struct GroupStats final {
      double min_latency = std::numeric_limits<double>::infinity();
      double max_throughput_mb = 0.0;
      double max_efficiency = 0.0;
      std::set<std::string> endpoints;
    };

    std::map<ScoreGroupKey, GroupStats> group_stats;
    std::map<ScoreGroupKey, GroupStats> clean_group_stats;

    auto add_to_stats = [](GroupStats& stats, const AggregatedCase& item) {
      stats.endpoints.insert(item.endpoint_key);

      const double lat = score_latency_us(item);

      if (lat > 0.0 && lat < stats.min_latency) {
        stats.min_latency = lat;
      }

      const double thr = item.recv_mb_per_sec.average();

      if (thr > stats.max_throughput_mb) {
        stats.max_throughput_mb = thr;
      }

      const double cpu_ms = item.pub_cpu_ms.average() + item.sub_cpu_ms.average();
      const double eff = cpu_ms > 0.001 ? (item.received.average() / cpu_ms) : 0.0;

      if (eff > stats.max_efficiency) {
        stats.max_efficiency = eff;
      }
    };

    for (const auto& item : items) {
      if (!has_runtime_success(item)) {
        continue;
      }

      const auto key = make_key(item);
      add_to_stats(group_stats[key], item);

      if (compute_loss_ratio_percent(item) <= 5.0 && compute_latency_drop_ratio_percent(item) <= 5.0) {
        add_to_stats(clean_group_stats[key], item);
      }
    }

    for (auto& item : items) {
      if (!has_runtime_success(item) || item.deployment_layer == DeploymentLayer::kUnknown) {
        item.speed_score = -1.0;
        item.capacity_score = -1.0;
        item.efficiency_score = -1.0;
        item.sort_score = -1.0;
        item.confidence_label = "unknown";
        continue;
      }

      const auto key = make_key(item);
      const auto& fallback_stats = group_stats[key];
      const auto clean_stats_iter = clean_group_stats.find(key);
      const GroupStats& stats =
          (clean_stats_iter != clean_group_stats.end() && clean_stats_iter->second.endpoints.size() >= 2)
              ? clean_stats_iter->second
              : fallback_stats;
      const bool is_solo = stats.endpoints.size() < 2;
      const double lat = score_latency_us(item);

      if (is_solo) {
        if (lat > 0.0) {
          if (item.scenario.suite == Bench::kLatencySuite) {
            item.speed_score = score_latency_quality(item);
          } else {
            item.speed_score = compute_absolute_latency_score(lat, item.scenario.payload_size);
          }
        } else {
          item.speed_score = -1.0;
        }

        const double thr = item.recv_mb_per_sec.average();
        item.capacity_score = thr > 0.0 ? compute_absolute_throughput_score(thr) : -1.0;

        const double cpu_ms = item.pub_cpu_ms.average() + item.sub_cpu_ms.average();
        const double recv = item.received.average();

        if (cpu_ms > 0.001 && recv > 0.0) {
          const double eff = recv / cpu_ms;
          item.efficiency_score = std::clamp(std::log1p(eff) * 100.0 / std::log1p(1000.0), 0.0, 100.0);
        } else {
          item.efficiency_score = -1.0;
        }
      } else {
        if (lat > 0.0 && stats.min_latency < std::numeric_limits<double>::infinity()) {
          const double relative_score =
              std::min(100.0, 100.0 * std::pow(std::clamp(stats.min_latency / lat, 0.0, 1.0), 1.10));
          const double absolute_score = item.scenario.suite == Bench::kLatencySuite
                                            ? score_latency_quality(item)
                                            : compute_absolute_latency_score(lat, item.scenario.payload_size);
          item.speed_score = blend_absolute_relative_score(absolute_score, relative_score);
        } else {
          item.speed_score = -1.0;
        }

        const double thr = item.recv_mb_per_sec.average();

        if (stats.max_throughput_mb > 0.0) {
          const double relative_score =
              std::min(100.0, 100.0 * std::pow(std::clamp(thr / stats.max_throughput_mb, 0.0, 1.0), 1.10));
          item.capacity_score = blend_absolute_relative_score(compute_absolute_throughput_score(thr), relative_score);
        } else {
          item.capacity_score = -1.0;
        }

        const double cpu_ms = item.pub_cpu_ms.average() + item.sub_cpu_ms.average();
        const double eff = cpu_ms > 0.001 ? (item.received.average() / cpu_ms) : 0.0;

        if (stats.max_efficiency > 0.0) {
          item.efficiency_score =
              std::min(100.0, 100.0 * std::pow(std::clamp(eff / stats.max_efficiency, 0.0, 1.0), 1.10));
        } else {
          item.efficiency_score = -1.0;
        }
      }

      {
        const double loss_pct = compute_loss_ratio_percent(item);
        const double drop_pct = compute_latency_drop_ratio_percent(item);

        if (item.speed_score > 0.0) {
          item.speed_score *= per_case_speed_loss_factor(loss_pct, drop_pct);
        }

        if (item.capacity_score > 0.0) {
          item.capacity_score *= per_case_capacity_loss_factor(loss_pct, drop_pct);
        }
      }

      {
        const double s = item.speed_score > 0 ? item.speed_score : 0.0;
        const double c = item.capacity_score > 0 ? item.capacity_score : 0.0;
        const auto& cfg = score_config();
        const auto* w = &cfg.default_suite_sort;

        if (item.scenario.suite == Bench::kLatencySuite) {
          w = &cfg.latency_suite_sort;
        } else if (item.scenario.suite == Bench::kThroughputSuite || item.scenario.suite == Bench::kTopologySuite) {
          w = &cfg.throughput_suite_sort;
        } else if (item.scenario.suite == Bench::kBackpressureSuite) {
          w = &cfg.backpressure_suite_sort;
        }

        item.sort_score = w->speed * s + w->capacity * c;
      }

      if (!has_runtime_success(item)) {
        item.confidence_label = "unknown";
      } else {
        const double loss_percent = compute_loss_ratio_percent(item);
        const double drop_percent = compute_latency_drop_ratio_percent(item);
        const auto& cfg = score_config();

        if (loss_percent >= cfg.confidence_endpoint_noisy_loss_pct ||
            drop_percent >= cfg.confidence_endpoint_noisy_loss_pct) {
          item.confidence_label = "noisy";
        } else if (loss_percent >= cfg.confidence_endpoint_medium_loss_pct ||
                   drop_percent >= cfg.confidence_endpoint_medium_loss_pct) {
          item.confidence_label = "medium";
        } else if (is_solo) {
          item.confidence_label = "solo";
        } else {
          item.confidence_label = "high";
        }
      }
    }
  }

  std::sort(items.begin(), items.end(), [](const AggregatedCase& lhs, const AggregatedCase& rhs) {
    return std::tuple{static_cast<int>(lhs.scenario.suite),
                      static_cast<int>(lhs.scenario.mode),
                      static_cast<int>(lhs.scenario.topology),
                      static_cast<int>(lhs.scenario.rate_pattern),
                      static_cast<int>(lhs.scenario.payload),
                      lhs.url_order_index,
                      lhs.transport,
                      lhs.scenario.url,
                      lhs.scenario.qos_profile,
                      lhs.scenario.properties,
                      lhs.scenario.pub_properties,
                      lhs.scenario.sub_properties,
                      lhs.scenario.publishers,
                      lhs.scenario.subscribers,
                      lhs.scenario.payload_size,
                      lhs.scenario.rate_hz,
                      lhs.scenario.burst_messages,
                      lhs.scenario.subscriber_sleep_us,
                      lhs.scenario.warmup_ms,
                      lhs.scenario.duration_ms,
                      lhs.scenario.drain_ms} < std::tuple{static_cast<int>(rhs.scenario.suite),
                                                          static_cast<int>(rhs.scenario.mode),
                                                          static_cast<int>(rhs.scenario.topology),
                                                          static_cast<int>(rhs.scenario.rate_pattern),
                                                          static_cast<int>(rhs.scenario.payload),
                                                          rhs.url_order_index,
                                                          rhs.transport,
                                                          rhs.scenario.url,
                                                          rhs.scenario.qos_profile,
                                                          rhs.scenario.properties,
                                                          rhs.scenario.pub_properties,
                                                          rhs.scenario.sub_properties,
                                                          rhs.scenario.publishers,
                                                          rhs.scenario.subscribers,
                                                          rhs.scenario.payload_size,
                                                          rhs.scenario.rate_hz,
                                                          rhs.scenario.burst_messages,
                                                          rhs.scenario.subscriber_sleep_us,
                                                          rhs.scenario.warmup_ms,
                                                          rhs.scenario.duration_ms,
                                                          rhs.scenario.drain_ms};
  });

  return items;
}

}  // namespace vlink::bench::report
