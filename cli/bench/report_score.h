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

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <string_view>

namespace vlink::bench::report {

struct ScoreConfig final {
  struct SuiteSortWeights final {
    double speed;
    double capacity;
  };

  SuiteSortWeights latency_suite_sort{0.96, 0.04};
  SuiteSortWeights throughput_suite_sort{0.04, 0.96};
  SuiteSortWeights backpressure_suite_sort{0.40, 0.60};
  SuiteSortWeights default_suite_sort{0.50, 0.50};

  double latency_total_weight = 0.36;
  double throughput_total_weight = 0.24;
  double efficiency_total_weight = 0.12;
  double delivery_total_weight = 0.12;
  double send_block_total_weight = 0.12;
  double coverage_total_weight = 0.12;
  double topology_total_weight = 0.12;

  double loss_speed_start_pct = 5.0;
  double loss_speed_inflection_pct = 25.0;
  double loss_speed_max_penalty = 0.60;
  double loss_speed_steepness = 0.20;

  double drop_speed_start_pct = 5.0;
  double drop_speed_inflection_pct = 25.0;
  double drop_speed_max_penalty = 0.50;
  double drop_speed_steepness = 0.20;

  double capacity_loss_start_pct = 15.0;
  double capacity_loss_inflection_pct = 35.0;
  double capacity_loss_max_penalty = 0.30;
  double capacity_loss_steepness = 0.18;

  double speed_gate_threshold = 25.0;
  double speed_gate_headroom_factor = 0.75;
  double capacity_gate_threshold = 18.0;
  double capacity_gate_headroom_factor = 0.80;

  double confidence_high = 1.00;
  double confidence_medium = 0.99;
  double confidence_noisy = 0.95;
  double confidence_solo = 0.99;
  double confidence_unknown = 0.95;
  double confidence_endpoint_medium_loss_pct = 5.0;
  double confidence_endpoint_noisy_loss_pct = 15.0;

  double latency_payload_weight_slope = 0.27;
  double latency_payload_weight_min = 1.00;
  double latency_payload_weight_max = 3.00;
};

inline const ScoreConfig& score_config() {
  static const ScoreConfig kCfg;
  return kCfg;
}

inline double clamp_score(double value) { return std::clamp(value, 0.0, 100.0); }

inline double clamp_total_score(double value) { return std::clamp(value, 0.0, 120.0); }

inline double compute_absolute_latency_score(double latency_us, size_t payload_size) {
  if (latency_us <= 0.0 || !std::isfinite(latency_us)) {
    return 0.0;
  }

  const double payload_kib = static_cast<double>(payload_size) / 1024.0;
  const double excellent_us = std::clamp(80.0 + payload_kib * 0.40, 80.0, 1500.0);
  const double poor_us = excellent_us * 8.0;

  if (latency_us <= excellent_us) {
    return 100.0;
  }

  if (latency_us >= poor_us) {
    return 0.0;
  }

  const double ratio = std::log(latency_us / excellent_us) / std::log(poor_us / excellent_us);
  return clamp_score((1.0 - ratio) * 100.0);
}

inline double latency_payload_weight(size_t payload_size) {
  const auto& c = score_config();
  const double payload_kib = static_cast<double>(payload_size) / 1024.0;
  return std::clamp(1.0 + c.latency_payload_weight_slope * std::log1p(std::max(payload_kib, 0.0)),
                    c.latency_payload_weight_min, c.latency_payload_weight_max);
}

inline double compute_absolute_send_block_score(double send_block_us, size_t payload_size) {
  if (send_block_us <= 0.0 || !std::isfinite(send_block_us)) {
    return 100.0;
  }

  const double payload_kib = static_cast<double>(payload_size) / 1024.0;
  const double excellent_us = std::clamp(5.0 + payload_kib * 0.05, 5.0, 200.0);
  const double poor_us = excellent_us * 30.0;

  if (send_block_us <= excellent_us) {
    return 100.0;
  }

  if (send_block_us >= poor_us) {
    return 0.0;
  }

  const double ratio = std::log(send_block_us / excellent_us) / std::log(poor_us / excellent_us);
  return clamp_score((1.0 - ratio) * 100.0);
}

inline double compute_absolute_throughput_score(double recv_mb_per_sec) {
  if (recv_mb_per_sec <= 0.0 || !std::isfinite(recv_mb_per_sec)) {
    return 0.0;
  }

  return clamp_score(std::log1p(recv_mb_per_sec) * 100.0 / std::log1p(1024.0));
}

inline double blend_absolute_relative_score(double absolute_score, double relative_score,
                                            double absolute_weight = 0.90) {
  const double w_abs = std::clamp(absolute_weight, 0.0, 1.0);
  return clamp_score(absolute_score * w_abs + relative_score * (1.0 - w_abs));
}

inline double logistic_loss_penalty(double pct, double start_pct, double inflection_pct, double max_penalty,
                                    double steepness) {
  if (pct <= start_pct || max_penalty <= 0.0) {
    return 0.0;
  }

  const double offset = std::max(0.001, inflection_pct - start_pct);
  const double s = 1.0 / (1.0 + std::exp(-steepness * (pct - start_pct - offset)));
  const double s0 = 1.0 / (1.0 + std::exp(steepness * offset));
  const double denom = std::max(1e-9, 1.0 - s0);
  return std::clamp(max_penalty * (s - s0) / denom, 0.0, max_penalty);
}

inline double per_case_speed_loss_factor(double loss_pct, double drop_pct) {
  const auto& c = score_config();
  const double drop_pen = logistic_loss_penalty(drop_pct, c.drop_speed_start_pct, c.drop_speed_inflection_pct,
                                                c.drop_speed_max_penalty, c.drop_speed_steepness);
  const double loss_pen = logistic_loss_penalty(loss_pct, c.loss_speed_start_pct, c.loss_speed_inflection_pct,
                                                c.loss_speed_max_penalty, c.loss_speed_steepness);
  return (1.0 - drop_pen) * (1.0 - loss_pen);
}

inline double per_case_capacity_loss_factor(double loss_pct, double drop_pct) {
  const auto& c = score_config();
  const double pen =
      logistic_loss_penalty(std::max(loss_pct, drop_pct), c.capacity_loss_start_pct, c.capacity_loss_inflection_pct,
                            c.capacity_loss_max_penalty, c.capacity_loss_steepness);
  return 1.0 - pen;
}

inline double apply_speed_gate(double avg_score, double min_score) {
  const auto& c = score_config();

  if (min_score >= c.speed_gate_threshold || min_score >= 100.5) {
    return avg_score;
  }

  const double cap = min_score + c.speed_gate_headroom_factor * (100.0 - min_score);
  return std::min(avg_score, cap);
}

inline double apply_capacity_gate(double avg_score, double min_score) {
  const auto& c = score_config();

  if (min_score >= c.capacity_gate_threshold || min_score >= 100.5) {
    return avg_score;
  }

  const double cap = min_score + c.capacity_gate_headroom_factor * (100.0 - min_score);
  return std::min(avg_score, cap);
}

inline double confidence_multiplier(std::string_view label) {
  const auto& c = score_config();

  if (label == "high") {
    return c.confidence_high;
  }

  if (label == "medium") {
    return c.confidence_medium;
  }

  if (label == "noisy") {
    return c.confidence_noisy;
  }

  if (label == "solo") {
    return c.confidence_solo;
  }

  return c.confidence_unknown;
}

}  // namespace vlink::bench::report
