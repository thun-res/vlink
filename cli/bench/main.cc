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

#include <vlink/base/helpers.h>
#include <vlink/base/logger.h>
#include <vlink/base/terminal_stream.h>
#include <vlink/base/utils.h>
#include <vlink/extension/qos_profile.h>
#include <vlink/version.h>
#ifdef VLINK_SUPPORT_SHM
#include <vlink/modules/fdbus_conf.h>
#include <vlink/modules/shm_conf.h>
#endif

#include <algorithm>
#include <argparse/argparse.hpp>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <iostream>
#include <limits>
#include <sstream>
#include <string>
#include <type_traits>
#include <vector>

#include "./bench.h"
#include "./report.h"

struct OutputOptions final {
  bool html{true};
  bool terminal{true};
  bool csv{false};
  bool json{false};
  bool pager{true};
  bool silent{false};
};

void print_output_summary(const vlink::bench::report::Summary& summary, const vlink::bench::Bench::Result& result,
                          const OutputOptions& output_options, const std::string& output_prefix) {
  const auto terminal_size = vlink::Utils::get_terminal_size();
  const bool colorize = terminal_size.first > 0 && terminal_size.second > 0;
  const size_t planned_case_count = result.planned_case_count == 0 ? summary.case_count : result.planned_case_count;

  auto print_line = [colorize](const std::string& line, const char* color = nullptr) {
    if (colorize && color != nullptr) {
      std::cout << color << line << "\033[0m" << std::endl;
    } else {
      std::cout << line << std::endl;
    }
  };

  print_line(" vlink-bench outputs ", colorize ? "\033[48;5;24;37;1m" : nullptr);
  print_line(" planned : " + std::to_string(planned_case_count) + " | skipped : " +
             std::to_string(result.skipped_case_count) + " | samples : " + std::to_string(summary.sample_count));
  print_line(" cases   : " + std::to_string(summary.case_count) + " | passing : " +
             std::to_string(summary.passing_case_count) + " | warning : " + std::to_string(summary.warning_case_count) +
             " | failing : " + std::to_string(summary.failing_case_count));

  if (output_options.json) {
    print_line(" json    : " + output_prefix + ".json", colorize ? "\033[38;5;72m" : nullptr);
  }

  if (output_options.csv) {
    print_line(" csv     : " + output_prefix + ".csv", colorize ? "\033[38;5;72m" : nullptr);
  }

  if (output_options.html) {
    print_line(" html    : " + output_prefix + ".html", colorize ? "\033[38;5;72m" : nullptr);
  }

  if (output_options.terminal) {
    print_line(" terminal: shown above", colorize ? "\033[38;5;81m" : nullptr);
  }
}

std::string join_command_line(int argc, char** argv) {
  std::string line;

  for (int index = 0; index < argc; ++index) {
    if (index != 0) {
      line.push_back(' ');
    }

    const std::string arg = argv[index] == nullptr ? std::string() : std::string(argv[index]);

    if (arg.find_first_of(" \t\"") == std::string::npos) {
      line.append(arg);
      continue;
    }

    line.push_back('"');

    for (char c : arg) {
      if (c == '"') {
        line.append("\\\"");
      } else {
        line.push_back(c);
      }
    }

    line.push_back('"');
  }

  return line;
}

std::string make_default_output_prefix() {
  std::time_t now = std::time(nullptr);
  std::tm tm_now{};
#ifdef _WIN32
  ::localtime_s(&tm_now, &now);
#else
  ::localtime_r(&now, &tm_now);
#endif

  char buffer[64] = {0};
  std::strftime(buffer, sizeof(buffer), "vlink-bench-%Y%m%d-%H%M%S", &tm_now);

  std::error_code ec;
  std::filesystem::path abs = std::filesystem::absolute(buffer, ec);

  if (ec) {
    return std::string(buffer);
  }

  return abs.string();
}

std::vector<std::string> expand_tokens(const std::vector<std::string>& tokens) {
  std::vector<std::string> result;

  for (const auto& token : tokens) {
    auto split = vlink::Helpers::split_any(token);

    for (auto& item : split) {
      if (!item.empty()) {
        result.emplace_back(std::move(item));
      }
    }
  }

  return result;
}

std::vector<std::string> trim_tokens(const std::vector<std::string>& tokens) {
  std::vector<std::string> result;

  for (const auto& token : tokens) {
    auto item = vlink::Helpers::trim_string(token);

    if (!item.empty()) {
      result.emplace_back(std::move(item));
    }
  }

  return result;
}

void append_unique(std::vector<std::string>& values, const std::string& value) {
  if (std::find(values.begin(), values.end(), value) == values.end()) {
    values.emplace_back(value);
  }
}

std::string scheme_of(const std::string& url) {
  const auto pos = url.find("://");

  if (pos == std::string::npos) {
    return {};
  }

  return url.substr(0, pos);
}

std::string transport_unavailable_reason(const std::string& url) {
  const auto scheme = scheme_of(url);

  if (scheme == "intra") {
#ifndef VLINK_SUPPORT_INTRA
    return "intra:// is not compiled in this build";
#else
    return {};
#endif
  }

  if (scheme == "shm") {
#ifndef VLINK_SUPPORT_SHM
    return "shm:// is not compiled in this build";
#else

    if (!vlink::ShmConf::auto_init_roudi(true)) {
      return "iox-roudi not running";
    }

    return {};
#endif
  }

  if (scheme == "shm2") {
#ifndef VLINK_SUPPORT_SHM2
    return "shm2:// is not compiled in this build";
#else
    return {};
#endif
  }

  if (scheme == "dds") {
#ifndef VLINK_SUPPORT_DDS
    return "dds:// is not compiled in this build";
#else
    return {};
#endif
  }

  if (scheme == "ddsc") {
#ifndef VLINK_SUPPORT_DDSC
    return "ddsc:// is not compiled in this build";
#else
    return {};
#endif
  }

  if (scheme == "ddsr") {
#ifndef VLINK_SUPPORT_DDSR
    return "ddsr:// is not compiled in this build";
#else
    return {};
#endif
  }

  if (scheme == "zenoh") {
#ifndef VLINK_SUPPORT_ZENOH
    return "zenoh:// is not compiled in this build";
#else
    return {};
#endif
  }

  if (scheme == "someip") {
#ifndef VLINK_SUPPORT_SOMEIP
    return "someip:// is not compiled in this build";
#else
    return {};
#endif
  }

  if (scheme == "mqtt") {
#ifndef VLINK_SUPPORT_MQTT
    return "mqtt:// is not compiled in this build";
#else

    if (vlink::Utils::get_env("VLINK_MQTT_BROKER").empty()) {
      return "VLINK_MQTT_BROKER not set";
    }

    return {};
#endif
  }

  if (scheme == "fdbus") {
#ifndef VLINK_SUPPORT_FDBUS
    return "fdbus:// is not compiled in this build";
#else

    if (!vlink::FdbusConf::has_name_server()) {
      return "name_server not running";
    }

    return {};
#endif
  }

  return {};
}

void append_if_runnable(std::vector<std::string>& values, const std::string& value) {
  if (!transport_unavailable_reason(value).empty()) {
    return;
  }

  append_unique(values, value);
}

std::vector<std::string> get_builtin_urls(bool include_intra) {
  std::vector<std::string> urls;

  if (include_intra) {
#ifdef VLINK_SUPPORT_INTRA
    append_if_runnable(urls, "intra://bench/perf_intra");
#endif
  }

#ifdef VLINK_SUPPORT_SHM
  append_if_runnable(urls, "shm://bench/perf_shm?depth=10");
#endif
#ifdef VLINK_SUPPORT_SHM2
  append_if_runnable(urls, "shm2://bench/perf_shm2?depth=10");
#endif
#ifdef VLINK_SUPPORT_DDS
  append_if_runnable(urls, "dds://bench/perf_dds?qos=better");
#endif
#ifdef VLINK_SUPPORT_DDSC
  append_if_runnable(urls, "ddsc://bench/perf_ddsc?qos=better");
#endif
#ifdef VLINK_SUPPORT_DDSR
  append_if_runnable(urls, "ddsr://bench/perf_ddsr?qos=better");
#endif
#ifdef VLINK_SUPPORT_ZENOH
  append_if_runnable(urls, "zenoh://bench/perf_zenoh?qos=better");
#endif
#ifdef VLINK_SUPPORT_SOMEIP
  append_if_runnable(urls, "someip://0x5566/0x5486?groups=0x8&event=0x9");
#endif
#ifdef VLINK_SUPPORT_MQTT
  append_if_runnable(urls, "mqtt://bench/perf_mqtt");
#endif
#ifdef VLINK_SUPPORT_FDBUS
  append_if_runnable(urls, "fdbus://bench/perf_fdbus");
#endif

  return urls;
}

std::vector<std::string> get_showcase_urls() { return get_builtin_urls(false); }

std::vector<std::string> get_default_urls() { return get_builtin_urls(true); }

bool filter_unavailable_urls(std::vector<std::string>& urls, std::string& error) {
  std::vector<std::string> kept;

  for (const auto& url : urls) {
    const auto reason = transport_unavailable_reason(url);

    if (!reason.empty()) {
      if (reason.find("not compiled in") != std::string::npos) {
        error = "url '";
        error += url;
        error += "' rejected: ";
        error += reason;
        return false;
      }

      std::cerr << "vlink-bench: skipping " << url << " (" << reason << ")" << std::endl;
      continue;
    }

    kept.emplace_back(url);
  }

  urls = std::move(kept);
  return true;
}

template <typename T, typename FnT>
bool parse_list(std::string_view name, const std::vector<std::string>& tokens, std::vector<T>& values, FnT&& parser,
                std::string& error) {
  values.clear();

  for (const auto& token : expand_tokens(tokens)) {
    T value{};

    if (!parser(token, value)) {
      error = std::string(name) + " has invalid value: " + token;
      return false;
    }

    values.emplace_back(std::move(value));
  }

  return true;
}

template <typename T>
bool parse_integral(const std::string& token, T& value) {
  try {
    size_t pos = 0;

    if constexpr (std::is_same_v<T, size_t>) {
      if (!token.empty() && token.front() == '-') {
        return false;
      }

      const auto parsed = std::stoull(token, &pos);

      if (parsed > static_cast<uint64_t>(std::numeric_limits<size_t>::max())) {
        return false;
      }

      value = static_cast<size_t>(parsed);
    } else {
      const auto parsed = std::stoll(token, &pos);

      if (parsed < static_cast<int64_t>(std::numeric_limits<T>::min()) ||
          parsed > static_cast<int64_t>(std::numeric_limits<T>::max())) {
        return false;
      }

      value = static_cast<T>(parsed);
    }

    if (pos != token.size()) {
      return false;
    }
  } catch (std::exception&) {
    return false;
  }

  return true;
}

bool is_valid_preset(const std::string& preset) {
  return preset.empty() || preset == "showcase" || preset == "quick" || preset == "full";
}

template <typename T>
bool validate_non_negative_list(const std::vector<T>& values, std::string_view name, std::string& error) {
  for (const auto& value : values) {
    if (value < 0) {
      error = std::string(name) + " must be >= 0";
      return false;
    }
  }

  return true;
}

template <typename T>
bool validate_positive_list(const std::vector<T>& values, std::string_view name, std::string& error) {
  for (const auto& value : values) {
    if (value <= 0) {
      error = std::string(name) + " must be > 0";
      return false;
    }
  }

  return true;
}

template <typename T>
void deduplicate_values(std::vector<T>& values) {
  std::vector<T> unique_values;
  unique_values.reserve(values.size());

  for (const auto& value : values) {
    if (std::find(unique_values.begin(), unique_values.end(), value) == unique_values.end()) {
      unique_values.emplace_back(value);
    }
  }

  values.swap(unique_values);
}

void apply_preset(const std::string& preset, vlink::bench::Bench::RunOptions& options, bool warmup_used,
                  bool duration_used, bool drain_used, bool serialization_duration_used) {
  if (preset == "full") {
    if (options.suites.empty()) {
      options.suites = {vlink::bench::Bench::kThroughputSuite, vlink::bench::Bench::kLatencySuite,
                        vlink::bench::Bench::kTopologySuite, vlink::bench::Bench::kSerializationSuite};
    }

    if (options.modes.empty()) {
      options.modes = {vlink::bench::Bench::kLocalDirectMode, vlink::bench::Bench::kLocalLoopMode,
                       vlink::bench::Bench::kProcessMode};
    }

    if (options.topologies.empty()) {
      options.topologies = {vlink::bench::Bench::kOneToOneTopology, vlink::bench::Bench::kOneToManyTopology,
                            vlink::bench::Bench::kManyToOneTopology, vlink::bench::Bench::kManyToManyTopology};
    }

    if (options.rate_patterns.empty()) {
      options.rate_patterns = {vlink::bench::Bench::kMaxRatePattern, vlink::bench::Bench::kFixedRatePattern,
                               vlink::bench::Bench::kBurstRatePattern};
    }

    if (options.payloads.empty()) {
      options.payloads = {vlink::bench::Bench::kBytesPayload, vlink::bench::Bench::kRawDataPayload};
    }

    if (options.urls.empty()) {
      options.urls = get_default_urls();
    }

    if (options.qos_profiles.empty()) {
      options.qos_profiles = {std::string("sensor"), std::string("best")};
    }

    if (options.payload_sizes.empty()) {
      options.payload_sizes = {16, 64, 256, 1024, 4096, 16384, 65536, 262144, 1048576, 4194304};
    }

    if (options.latency_sizes.empty()) {
      options.latency_sizes = {16, 64, 256, 1024, 4096, 16384, 65536};
    }

    if (options.topology_sizes.empty()) {
      options.topology_sizes = {64, 256, 1024, 4096, 16384, 65536, 262144};
    }

    if (options.latency_rates.empty()) {
      options.latency_rates = {10, 50, 100, 500, 1000, 5000, 10000, 50000};
    }

    if (options.fanout_subscribers.empty()) {
      options.fanout_subscribers = {1, 2, 4, 8, 16};
    }

    if (options.publisher_counts.empty()) {
      options.publisher_counts = {1, 2, 4, 8, 16};
    }

    if (options.burst_messages.empty()) {
      options.burst_messages = {8, 32, 64, 128, 512};
    }

    if (!warmup_used && options.warmup_ms == 0) {
      options.warmup_ms = 1000;
    }

    if (!duration_used && options.duration_ms == 0) {
      options.duration_ms = 2500;
    }

    if (!serialization_duration_used && options.serialization_duration_ms == 0) {
      options.serialization_duration_ms = 1200;
    }
  } else {
    if (options.suites.empty()) {
      options.suites = {vlink::bench::Bench::kThroughputSuite, vlink::bench::Bench::kLatencySuite};
    }

    if (options.modes.empty()) {
      options.modes = {vlink::bench::Bench::kProcessMode};
    }

    if (options.topologies.empty()) {
      options.topologies = {vlink::bench::Bench::kOneToOneTopology};
    }

    if (options.rate_patterns.empty()) {
      options.rate_patterns = {vlink::bench::Bench::kMaxRatePattern};
    }

    if (options.payloads.empty()) {
      options.payloads = {vlink::bench::Bench::kBytesPayload};
    }

    if (options.urls.empty()) {
      options.urls = get_showcase_urls();
    }

    if (options.payload_sizes.empty()) {
      options.payload_sizes = {16384, 262144, 1048576};
    }

    if (options.latency_sizes.empty()) {
      options.latency_sizes = {128, 4096, 65536, 524288, 2097152};
    }

    if (options.topology_sizes.empty()) {
      options.topology_sizes = {4096};
    }

    if (options.latency_rates.empty()) {
      options.latency_rates = {1000};
    }

    if (options.fanout_subscribers.empty()) {
      options.fanout_subscribers = {4, 16};
    }

    if (options.publisher_counts.empty()) {
      options.publisher_counts = {4, 16};
    }

    if (options.burst_messages.empty()) {
      options.burst_messages = {32};
    }

    if (!warmup_used && options.warmup_ms == 0) {
      options.warmup_ms = 1000;
    }

    if (!duration_used && options.duration_ms == 0) {
      options.duration_ms = 2000;
    }

    if (!serialization_duration_used && options.serialization_duration_ms == 0) {
      options.serialization_duration_ms = 800;
    }
  }

  if (!drain_used && options.drain_ms == 0) {
    options.drain_ms = 300;
  }

  if (options.repeat_count <= 0) {
    options.repeat_count = (preset == "full") ? 3 : 1;
  }
}

bool apply_report_token(const std::string& token, bool allow_json, OutputOptions& options, std::string& error) {
  std::string lower = token;
  std::transform(lower.begin(), lower.end(), lower.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

  if (lower == "html") {
    options.html = true;
    return true;
  }

  if (lower == "terminal" || lower == "term" || lower == "tty") {
    options.terminal = true;
    return true;
  }

  if (lower == "both") {
    options.html = true;
    options.terminal = true;
    return true;
  }

  if (lower == "csv") {
    options.csv = true;
    return true;
  }

  if (lower == "json") {
    if (!allow_json) {
      error = "json emit is not supported by plot";
      return false;
    }

    options.json = true;
    return true;
  }

  error = "invalid report target";
  return false;
}

bool build_run_options(const argparse::ArgumentParser& command, vlink::bench::Bench::RunOptions& options,
                       std::string& output_prefix, std::string& error) {
  const auto preset = command.get<std::string>("--preset");

  if (!is_valid_preset(preset)) {
    error = "invalid preset";
    return false;
  }

  options.executable_path = vlink::Utils::get_app_path();
  options.warmup_ms = command.get<int>("--warmup");
  options.duration_ms = command.get<int>("--duration");
  options.drain_ms = command.get<int>("--drain");
  options.serialization_duration_ms = command.get<int>("--serialization-duration");
  options.repeat_count = command.get<int>("--repeat");
  options.verbose = command.get<bool>("--verbose");
  options.payloads_explicit = !command.get<std::vector<std::string>>("--payload").empty();
  options.payload_sizes_explicit = !command.get<std::vector<std::string>>("--size").empty();
  options.latency_sizes_explicit = !command.get<std::vector<std::string>>("--latency-size").empty();
  options.topology_sizes_explicit = !command.get<std::vector<std::string>>("--topology-size").empty();
  options.latency_rates_explicit = !command.get<std::vector<std::string>>("--rate").empty();
  options.burst_messages_explicit = !command.get<std::vector<std::string>>("--burst").empty();
  const bool warmup_used = command.is_used("--warmup");
  const bool duration_used = command.is_used("--duration");
  const bool drain_used = command.is_used("--drain");
  const bool serialization_duration_used = command.is_used("--serialization-duration");

  if (!parse_list(
          "--suite", command.get<std::vector<std::string>>("--suite"), options.suites,
          [](const std::string& token, vlink::bench::Bench::Suite& value) {
            return vlink::bench::Bench::parse_suite(token, value);
          },
          error) ||
      !parse_list(
          "--mode", command.get<std::vector<std::string>>("--mode"), options.modes,
          [](const std::string& token, vlink::bench::Bench::Mode& value) {
            return vlink::bench::Bench::parse_mode(token, value);
          },
          error) ||
      !parse_list(
          "--topology", command.get<std::vector<std::string>>("--topology"), options.topologies,
          [](const std::string& token, vlink::bench::Bench::Topology& value) {
            return vlink::bench::Bench::parse_topology(token, value);
          },
          error) ||
      !parse_list(
          "--pattern", command.get<std::vector<std::string>>("--pattern"), options.rate_patterns,
          [](const std::string& token, vlink::bench::Bench::RatePattern& value) {
            return vlink::bench::Bench::parse_rate_pattern(token, value);
          },
          error) ||
      !parse_list(
          "--payload", command.get<std::vector<std::string>>("--payload"), options.payloads,
          [](const std::string& token, vlink::bench::Bench::PayloadKind& value) {
            return vlink::bench::Bench::parse_payload(token, value);
          },
          error) ||
      !parse_list(
          "--size", command.get<std::vector<std::string>>("--size"), options.payload_sizes,
          [](const std::string& token, size_t& value) { return parse_integral(token, value); }, error) ||
      !parse_list(
          "--latency-size", command.get<std::vector<std::string>>("--latency-size"), options.latency_sizes,
          [](const std::string& token, size_t& value) { return parse_integral(token, value); }, error) ||
      !parse_list(
          "--topology-size", command.get<std::vector<std::string>>("--topology-size"), options.topology_sizes,
          [](const std::string& token, size_t& value) { return parse_integral(token, value); }, error) ||
      !parse_list(
          "--rate", command.get<std::vector<std::string>>("--rate"), options.latency_rates,
          [](const std::string& token, int& value) { return parse_integral(token, value); }, error) ||
      !parse_list(
          "--fanout", command.get<std::vector<std::string>>("--fanout"), options.fanout_subscribers,
          [](const std::string& token, int& value) { return parse_integral(token, value); }, error) ||
      !parse_list(
          "--publishers", command.get<std::vector<std::string>>("--publishers"), options.publisher_counts,
          [](const std::string& token, int& value) { return parse_integral(token, value); }, error) ||
      !parse_list(
          "--burst", command.get<std::vector<std::string>>("--burst"), options.burst_messages,
          [](const std::string& token, int& value) { return parse_integral(token, value); }, error) ||
      !parse_list(
          "--backpressure-sleep", command.get<std::vector<std::string>>("--backpressure-sleep"),
          options.backpressure_sleep_us,
          [](const std::string& token, int& value) { return parse_integral(token, value); }, error)) {
    return false;
  }

  options.urls = expand_tokens(command.get<std::vector<std::string>>("--url"));
  options.qos_profiles = expand_tokens(command.get<std::vector<std::string>>("--qos"));
  options.properties = trim_tokens(command.get<std::vector<std::string>>("--property"));
  options.pub_properties = trim_tokens(command.get<std::vector<std::string>>("--pub-property"));
  options.sub_properties = trim_tokens(command.get<std::vector<std::string>>("--sub-property"));

  for (const auto& qos : options.qos_profiles) {
    if (qos.empty() || qos == "default") {
      continue;
    }

    const auto& qos_map = vlink::QosProfile::get_available_qos_map();

    if (qos_map.find(qos) == qos_map.end()) {
      error = "unknown qos profile: " + qos;
      return false;
    }
  }

  for (const auto* properties : {&options.properties, &options.pub_properties, &options.sub_properties}) {
    for (const auto& entry : *properties) {
      const auto pos = entry.find('=');

      if (pos == std::string::npos || pos == 0 || pos + 1 >= entry.size()) {
        error = "invalid property: " + entry;
        return false;
      }
    }
  }

  apply_preset(preset, options, warmup_used, duration_used, drain_used, serialization_duration_used);

  deduplicate_values(options.suites);
  deduplicate_values(options.modes);
  deduplicate_values(options.topologies);
  deduplicate_values(options.rate_patterns);
  deduplicate_values(options.payloads);
  deduplicate_values(options.urls);

  if (!filter_unavailable_urls(options.urls, error)) {
    return false;
  }

  deduplicate_values(options.qos_profiles);
  deduplicate_values(options.payload_sizes);
  deduplicate_values(options.latency_sizes);
  deduplicate_values(options.topology_sizes);
  deduplicate_values(options.latency_rates);
  deduplicate_values(options.fanout_subscribers);
  deduplicate_values(options.publisher_counts);
  deduplicate_values(options.burst_messages);
  deduplicate_values(options.backpressure_sleep_us);

  if (options.repeat_count <= 0) {
    error = "repeat must be > 0";
    return false;
  }

  if (options.warmup_ms < 0 || options.duration_ms < 0 || options.drain_ms < 0 ||
      options.serialization_duration_ms < 0) {
    error = "duration arguments must be >= 0";
    return false;
  }

  if (!validate_non_negative_list(options.latency_rates, "rate", error) ||
      !validate_positive_list(options.fanout_subscribers, "fanout", error) ||
      !validate_positive_list(options.publisher_counts, "publishers", error) ||
      !validate_positive_list(options.burst_messages, "burst", error) ||
      !validate_positive_list(options.backpressure_sleep_us, "backpressure-sleep", error)) {
    return false;
  }

  if (std::find_if(options.rate_patterns.begin(), options.rate_patterns.end(),
                   [](vlink::bench::Bench::RatePattern pattern) {
                     return pattern != vlink::bench::Bench::kMaxRatePattern;
                   }) != options.rate_patterns.end() &&
      !validate_positive_list(options.latency_rates, "rate", error)) {
    return false;
  }

  output_prefix = command.get<std::string>("--output");

  if (output_prefix.empty()) {
    output_prefix = make_default_output_prefix();
  } else {
    std::error_code ec;
    std::filesystem::path abs = std::filesystem::absolute(output_prefix, ec);
    if (!ec) {
      output_prefix = abs.string();
    }
  }

  return true;
}

bool build_output_options(const argparse::ArgumentParser& command, bool allow_json, OutputOptions& options,
                          std::string& error) {
  options = OutputOptions{};

  const bool report_used = command.is_used("--report");

  if (report_used) {
    options.html = false;
    options.terminal = false;
    options.csv = false;
    options.json = false;

    for (const auto& token : expand_tokens(command.get<std::vector<std::string>>("--report"))) {
      if (!apply_report_token(token, allow_json, options, error)) {
        error += ": " + token;
        return false;
      }
    }
  }

  if (!options.html && !options.terminal && !options.csv && !options.json) {
    error = "report must contain at least one output";
    return false;
  }

  options.silent = command.get<bool>("--silent");
  options.pager = !options.silent && !command.get<bool>("--no-pager");
  return true;
}

bool build_worker_options(const argparse::ArgumentParser& command, bool allow_latency_flag,
                          vlink::bench::Bench::WorkerOptions& options, std::string& error) {
  if (!vlink::bench::Bench::parse_payload(command.get<std::string>("--payload"), options.payload)) {
    error = "invalid payload kind";
    return false;
  }

  options.url = command.get<std::string>("url");
  options.qos_profile = command.get<std::string>("--qos");
  options.properties = trim_tokens(command.get<std::vector<std::string>>("--property"));
  options.pub_properties = trim_tokens(command.get<std::vector<std::string>>("--pub-property"));
  options.sub_properties = trim_tokens(command.get<std::vector<std::string>>("--sub-property"));
  options.rate_pattern = vlink::bench::Bench::kMaxRatePattern;

  if (!vlink::bench::Bench::parse_rate_pattern(command.get<std::string>("--pattern"), options.rate_pattern)) {
    error = "invalid rate pattern";
    return false;
  }

  options.payload_size = static_cast<size_t>(command.get<int>("--size"));
  options.rate_hz = command.get<int>("--rate");
  options.burst_messages = command.get<int>("--burst");
  options.warmup_ms = command.get<int>("--warmup");
  options.duration_ms = command.get<int>("--duration");
  options.drain_ms = command.get<int>("--drain");
  options.publisher_index = command.get<int>("--publisher-id");
  options.enable_latency = allow_latency_flag ? command.get<bool>("--latency") : false;
  options.subscriber_sleep_us = allow_latency_flag ? command.get<int>("--subscriber-sleep-us") : 0;
  options.wait_start = command.get<bool>("--wait-start");
  options.result_file = command.get<std::string>("--output");
  options.verbose = command.get<bool>("--verbose");

  if (command.get<int>("--size") < 0) {
    error = "size must be >= 0";
    return false;
  }

  if (options.rate_hz < 0) {
    error = "rate must be >= 0";
    return false;
  }

  const bool internal_idle_fixed = options.wait_start &&
                                   options.rate_pattern == vlink::bench::Bench::kFixedRatePattern &&
                                   options.rate_hz == 0 && options.burst_messages > 0;
  const bool internal_idle_burst = options.wait_start &&
                                   options.rate_pattern == vlink::bench::Bench::kBurstRatePattern &&
                                   options.rate_hz > 0 && options.burst_messages == 0;

  if (options.rate_pattern != vlink::bench::Bench::kMaxRatePattern && options.rate_hz == 0 && !internal_idle_fixed) {
    error = "rate must be > 0 for fixed/burst pattern";
    return false;
  }

  if (options.burst_messages <= 0 && !internal_idle_burst) {
    error = "burst must be > 0";
    return false;
  }

  if (options.warmup_ms < 0 || options.duration_ms < 0 || options.drain_ms < 0) {
    error = "duration arguments must be >= 0";
    return false;
  }

  if (options.publisher_index < 0) {
    error = "publisher-id must be >= 0";
    return false;
  }

  if (options.subscriber_sleep_us < 0) {
    error = "subscriber-sleep-us must be >= 0";
    return false;
  }

  if (!options.qos_profile.empty() && options.qos_profile != "default") {
    const auto& qos_map = vlink::QosProfile::get_available_qos_map();

    if (qos_map.find(options.qos_profile) == qos_map.end()) {
      error = "unknown qos profile: " + options.qos_profile;
      return false;
    }
  }

  for (const auto* properties : {&options.properties, &options.pub_properties, &options.sub_properties}) {
    for (const auto& entry : *properties) {
      const auto pos = entry.find('=');

      if (pos == std::string::npos || pos == 0 || pos + 1 >= entry.size()) {
        error = "invalid property: " + entry;
        return false;
      }
    }
  }

  return true;
}

bool save_outputs(const vlink::bench::Bench::Result& result, const std::string& output_prefix,
                  const OutputOptions& output_options, std::string& error) {
  if (output_options.json && !vlink::bench::Bench::save_json(result, output_prefix + ".json", error)) {
    return false;
  }

  if (output_options.csv && !vlink::bench::report::save_csv(result, output_prefix + ".csv", error)) {
    return false;
  }

  if (output_options.html && !vlink::bench::report::save_html(result, output_prefix + ".html", error)) {
    return false;
  }

  if (output_options.terminal && !output_options.silent) {
    vlink::bench::report::TerminalOptions terminal_options;
    terminal_options.interactive = output_options.pager && (vlink::Utils::get_terminal_size().first > 0);

    if (!vlink::bench::report::print_terminal(result, terminal_options, error)) {
      return false;
    }
  }

  return true;
}

int main(int argc, char** argv) {
  std::ios::sync_with_stdio(false);

  std::setvbuf(stdout, nullptr, _IONBF, 0);
  std::setvbuf(stderr, nullptr, _IONBF, 0);

  vlink::Utils::set_console_utf8_output();
  VLINK_TERM_OUT.init();
  vlink::Utils::unset_env("VLINK_BAG_PATH");

  vlink::Logger::set_console_level(vlink::Logger::kOff);
  vlink::Logger::set_file_level(vlink::Logger::kOff);
  vlink::Logger::init("vlink-bench");

  vlink::bench::Bench::reset_stop();
  vlink::Utils::register_terminate_signal([](int) { vlink::bench::Bench::request_stop(); });

  const std::string command_line = join_command_line(argc, argv);

  argparse::ArgumentParser program("vlink-bench", VLINK_VERSION, argparse::default_arguments::all);
  program.add_description("Benchmark VLink pub/sub transport, serialization, latency, throughput and fanout.");

  argparse::ArgumentParser run_command("run");
  run_command.add_description("Run benchmark scenarios and emit selected html/json/csv/terminal outputs.");
  run_command.add_argument("-p", "--preset")
      .help("Scenario preset: showcase (default) / quick / full")
      .default_value(std::string());
  run_command.add_argument("-u", "--url")
      .help("Benchmark URL list, comma or space separated, or repeated")
      .default_value(std::vector<std::string>())
      .nargs(argparse::nargs_pattern::any);
  run_command.add_argument("-s", "--suite")
      .help("Suite list: throughput latency topology fanout serialization backpressure")
      .default_value(std::vector<std::string>())
      .nargs(argparse::nargs_pattern::any);
  run_command.add_argument("-m", "--mode")
      .help("Execution mode list: local-direct local-loop process")
      .default_value(std::vector<std::string>())
      .nargs(argparse::nargs_pattern::any);
  run_command.add_argument("-t", "--topology")
      .help(
          "Topology list: 1:1 1:n n:1 n:n; topology suite focuses on 1:n n:1 n:n, while 1:1 is covered by "
          "throughput/latency")
      .default_value(std::vector<std::string>())
      .nargs(argparse::nargs_pattern::any);
  run_command.add_argument("--pattern")
      .help("Rate pattern list: max fixed burst")
      .default_value(std::vector<std::string>())
      .nargs(argparse::nargs_pattern::any);
  run_command.add_argument("-k", "--payload")
      .help(
          "Payload list: bytes string rawdata; default preset routes string mainly to serialization while quick "
          "transport cases focus on bytes")
      .default_value(std::vector<std::string>())
      .nargs(argparse::nargs_pattern::any);
  run_command.add_argument("-q", "--qos")
      .help("QoS profile list, for example: default sensor event best")
      .default_value(std::vector<std::string>())
      .nargs(argparse::nargs_pattern::any);
  run_command.add_argument("--property")
      .help("Node property list in key=value format")
      .default_value(std::vector<std::string>())
      .nargs(argparse::nargs_pattern::any);
  run_command.add_argument("--pub-property")
      .help("Publisher-side property list in key=value format")
      .default_value(std::vector<std::string>())
      .nargs(argparse::nargs_pattern::any);
  run_command.add_argument("--sub-property")
      .help("Subscriber-side property list in key=value format")
      .default_value(std::vector<std::string>())
      .nargs(argparse::nargs_pattern::any);
  run_command.add_argument("--size")
      .help("Payload size list in bytes")
      .default_value(std::vector<std::string>())
      .nargs(argparse::nargs_pattern::any);
  run_command.add_argument("--latency-size")
      .help("Latency suite payload size list in bytes")
      .default_value(std::vector<std::string>())
      .nargs(argparse::nargs_pattern::any);
  run_command.add_argument("--topology-size")
      .help("Topology suite payload size list in bytes")
      .default_value(std::vector<std::string>())
      .nargs(argparse::nargs_pattern::any);
  run_command.add_argument("-r", "--rate")
      .help(
          "Total scenario cadence list in Hz for fixed-rate pressure cases and burst schedules; multi-publisher "
          "scenarios split it")
      .default_value(std::vector<std::string>())
      .nargs(argparse::nargs_pattern::any);
  run_command.add_argument("-f", "--fanout")
      .help("Fanout subscriber count list")
      .default_value(std::vector<std::string>())
      .nargs(argparse::nargs_pattern::any);
  run_command.add_argument("--publishers")
      .help("Publisher count list for multi-publisher scenarios")
      .default_value(std::vector<std::string>())
      .nargs(argparse::nargs_pattern::any);
  run_command.add_argument("--burst")
      .help("Total scenario burst message count list; multi-publisher scenarios split it")
      .default_value(std::vector<std::string>())
      .nargs(argparse::nargs_pattern::any);
  run_command.add_argument("--backpressure-sleep")
      .help("Backpressure subscriber sleep list in microseconds; default 100,1000,10000")
      .default_value(std::vector<std::string>())
      .nargs(argparse::nargs_pattern::any);
  run_command.add_argument("--warmup")
      .help("Warmup duration in milliseconds; when omitted the preset supplies a default, and explicit 0 is allowed")
      .scan<'i', int>()
      .default_value(0);
  run_command.add_argument("--duration")
      .help("Run duration in milliseconds; when omitted the preset supplies a default, and explicit 0 is allowed")
      .scan<'i', int>()
      .default_value(0);
  run_command.add_argument("--drain")
      .help("Drain duration in milliseconds; when omitted the preset supplies a default, and explicit 0 is allowed")
      .scan<'i', int>()
      .default_value(0);
  run_command.add_argument("--serialization-duration")
      .help(
          "Serialization suite duration in milliseconds; when omitted the preset supplies a default, and explicit 0 is "
          "allowed")
      .scan<'i', int>()
      .default_value(0);
  run_command.add_argument("--repeat").help("Repeat count per scenario").scan<'i', int>().default_value(1);
  run_command.add_argument("-o", "--output").help("Output file prefix, without extension").default_value(std::string());
  run_command.add_argument("--report")
      .help("Output targets: html terminal csv json both, comma or space separated, or repeated, default html,terminal")
      .default_value(std::vector<std::string>())
      .nargs(argparse::nargs_pattern::any);
  run_command.add_argument("--no-pager")
      .help("Disable terminal pager when terminal emit is enabled")
      .default_value(false)
      .implicit_value(true);
  run_command.add_argument("--silent")
      .help("Silent mode: skip the terminal table and summary banner, then exit when the run finishes")
      .default_value(false)
      .implicit_value(true);
  run_command.add_argument("--verbose").help("Verbose progress output").default_value(false).implicit_value(true);

  argparse::ArgumentParser plot_command("plot");
  plot_command.add_description("Render selected html/json/csv/terminal outputs from an existing bench json.");
  plot_command.add_argument("input").help("Input bench json file path");
  plot_command.add_argument("-o", "--output")
      .help("Output file prefix, without extension")
      .default_value(std::string());
  plot_command.add_argument("--report")
      .help("Output targets: html terminal csv json both, comma or space separated, or repeated, default html,terminal")
      .default_value(std::vector<std::string>())
      .nargs(argparse::nargs_pattern::any);
  plot_command.add_argument("--no-pager")
      .help("Disable terminal pager when terminal emit is enabled")
      .default_value(false)
      .implicit_value(true);
  plot_command.add_argument("--silent")
      .help("Silent mode: skip the terminal table and summary banner, then exit when rendering finishes")
      .default_value(false)
      .implicit_value(true);

  argparse::ArgumentParser pub_command("pub");
  pub_command.add_description("Internal worker: publisher side for process benchmark.");
  pub_command.add_argument("url").help("Benchmark URL");
  pub_command.add_argument("-k", "--payload")
      .help("Payload kind: bytes string rawdata")
      .default_value(std::string("bytes"));
  pub_command.add_argument("-q", "--qos")
      .help("QoS profile name, for example: default sensor event best")
      .default_value(std::string("default"));
  pub_command.add_argument("--pattern").help("Rate pattern: max fixed burst").default_value(std::string("max"));
  pub_command.add_argument("--property")
      .help("Node property list in key=value format")
      .default_value(std::vector<std::string>())
      .nargs(argparse::nargs_pattern::any);
  pub_command.add_argument("--pub-property")
      .help("Publisher-side property list in key=value format")
      .default_value(std::vector<std::string>())
      .nargs(argparse::nargs_pattern::any);
  pub_command.add_argument("--sub-property")
      .help("Subscriber-side property list in key=value format")
      .default_value(std::vector<std::string>())
      .nargs(argparse::nargs_pattern::any);
  pub_command.add_argument("--size").help("Payload size in bytes").scan<'i', int>().default_value(1024);
  pub_command.add_argument("-r", "--rate")
      .help("Publish rate in Hz, 0 means unlimited")
      .scan<'i', int>()
      .default_value(0);
  pub_command.add_argument("--burst")
      .help("Messages sent in each burst by this worker")
      .scan<'i', int>()
      .default_value(1);
  pub_command.add_argument("--warmup").help("Warmup duration in milliseconds").scan<'i', int>().default_value(500);
  pub_command.add_argument("--duration").help("Run duration in milliseconds").scan<'i', int>().default_value(1500);
  pub_command.add_argument("--drain").help("Drain duration in milliseconds").scan<'i', int>().default_value(300);
  pub_command.add_argument("--publisher-id").help("Publisher worker index").scan<'i', int>().default_value(0);
  pub_command.add_argument("--wait-start")
      .help("Internal worker: wait for parent GO signal on stdin before timing")
      .default_value(false)
      .implicit_value(true);
  pub_command.add_argument("-o", "--output")
      .help("Worker result json file path")
      .default_value(std::string("vlink-bench-pub.json"));
  pub_command.add_argument("--verbose").help("Verbose worker output").default_value(false).implicit_value(true);

  argparse::ArgumentParser sub_command("sub");
  sub_command.add_description("Internal worker: subscriber side for process benchmark.");
  sub_command.add_argument("url").help("Benchmark URL");
  sub_command.add_argument("-k", "--payload")
      .help("Payload kind: bytes string rawdata")
      .default_value(std::string("bytes"));
  sub_command.add_argument("-q", "--qos")
      .help("QoS profile name, for example: default sensor event best")
      .default_value(std::string("default"));
  sub_command.add_argument("--pattern").help("Rate pattern: max fixed burst").default_value(std::string("max"));
  sub_command.add_argument("--property")
      .help("Node property list in key=value format")
      .default_value(std::vector<std::string>())
      .nargs(argparse::nargs_pattern::any);
  sub_command.add_argument("--pub-property")
      .help("Publisher-side property list in key=value format")
      .default_value(std::vector<std::string>())
      .nargs(argparse::nargs_pattern::any);
  sub_command.add_argument("--sub-property")
      .help("Subscriber-side property list in key=value format")
      .default_value(std::vector<std::string>())
      .nargs(argparse::nargs_pattern::any);
  sub_command.add_argument("--size").help("Payload size in bytes").scan<'i', int>().default_value(1024);
  sub_command.add_argument("-r", "--rate")
      .help("Publisher cadence in Hz, forwarded from the parent for paired worker metadata")
      .scan<'i', int>()
      .default_value(0);
  sub_command.add_argument("--burst")
      .help("Burst message count from the paired publisher worker, forwarded for paired worker metadata")
      .scan<'i', int>()
      .default_value(1);
  sub_command.add_argument("--warmup").help("Warmup duration in milliseconds").scan<'i', int>().default_value(500);
  sub_command.add_argument("--duration").help("Run duration in milliseconds").scan<'i', int>().default_value(1500);
  sub_command.add_argument("--drain").help("Drain duration in milliseconds").scan<'i', int>().default_value(300);
  sub_command.add_argument("--latency")
      .help("Enable latency sampling on subscriber worker")
      .default_value(false)
      .implicit_value(true);
  sub_command.add_argument("--publisher-id").help("Publisher worker index").scan<'i', int>().default_value(0);
  sub_command.add_argument("--subscriber-sleep-us")
      .help("Microseconds to sleep per received message (simulates slow consumer / backpressure)")
      .scan<'i', int>()
      .default_value(0);
  sub_command.add_argument("--wait-start")
      .help("Internal worker: wait for parent GO signal on stdin before timing")
      .default_value(false)
      .implicit_value(true);
  sub_command.add_argument("-o", "--output")
      .help("Worker result json file path")
      .default_value(std::string("vlink-bench-sub.json"));
  sub_command.add_argument("--verbose").help("Verbose worker output").default_value(false).implicit_value(true);

  program.add_subparser(run_command);
  program.add_subparser(plot_command);
  program.add_subparser(pub_command);
  program.add_subparser(sub_command);

  try {
    program.parse_args(argc, argv);
  } catch (const std::exception& e) {
    std::cerr << e.what() << std::endl;

    if (program.is_subcommand_used("run")) {
      std::cerr << run_command;
    } else if (program.is_subcommand_used("plot")) {
      std::cerr << plot_command;
    } else if (program.is_subcommand_used("pub")) {
      std::cerr << pub_command;
    } else if (program.is_subcommand_used("sub")) {
      std::cerr << sub_command;
    } else {
      std::cerr << program;
    }

    return 1;
  }

  if (program.is_subcommand_used("run") || program.is_subcommand_used("plot")) {
    if VUNLIKELY (!vlink::Utils::check_singleton("vlink-bench")) {
      std::cerr << "vlink-bench is already running." << std::endl;
      return 1;
    }
  }

  if (program.is_subcommand_used("run")) {
    vlink::bench::Bench::RunOptions options;
    vlink::bench::Bench::Result result;
    OutputOptions output_options;
    std::string output_prefix;
    std::string error;
    const auto terminal_size = vlink::Utils::get_terminal_size();
    const bool has_terminal = terminal_size.first > 0 && terminal_size.second > 0;
    bool keyboard_started = false;

    if (has_terminal) {
      vlink::Utils::start_detect_keyboard(
          [](const std::string& key) {
            if (key == "q" || key == "esc") {
              vlink::bench::Bench::request_stop();
            }
          },
          30);
      keyboard_started = true;
    }

    if VUNLIKELY (!build_run_options(run_command, options, output_prefix, error) ||
                  !build_output_options(run_command, true, output_options, error) ||
                  !vlink::bench::Bench::run(options, result, error)) {
      if (keyboard_started) {
        vlink::Utils::stop_detect_keyboard();
      }

      std::cerr << error << std::endl;
      return 1;
    }

    if (keyboard_started) {
      vlink::Utils::stop_detect_keyboard();
    }

    result.command_line = command_line;

    if VUNLIKELY (!save_outputs(result, output_prefix, output_options, error)) {
      std::cerr << error << std::endl;
      return 1;
    }

    const auto summary = vlink::bench::report::summarize(result);

    if (!output_options.silent) {
      print_output_summary(summary, result, output_options, output_prefix);
    }

    return summary.failing_case_count == 0 ? 0 : 2;
  }

  if (program.is_subcommand_used("plot")) {
    vlink::bench::Bench::Result result;
    OutputOptions output_options;
    std::string error;
    auto output_prefix = plot_command.get<std::string>("--output");

    if (output_prefix.empty()) {
      const auto& input_path = plot_command.get<std::string>("input");
      std::error_code ec;
      std::filesystem::path abs = std::filesystem::absolute(input_path, ec);
      output_prefix = (ec ? std::filesystem::path(input_path) : abs).replace_extension("").string();
    } else {
      std::error_code ec;
      std::filesystem::path abs = std::filesystem::absolute(output_prefix, ec);
      if (!ec) {
        output_prefix = abs.string();
      }
    }

    if (!build_output_options(plot_command, true, output_options, error) ||
        !vlink::bench::Bench::load_json(plot_command.get<std::string>("input"), result, error) ||
        !save_outputs(result, output_prefix, output_options, error)) {
      std::cerr << error << std::endl;
      return 1;
    }

    const auto summary = vlink::bench::report::summarize(result);

    if (!output_options.silent) {
      print_output_summary(summary, result, output_options, output_prefix);
    }

    return summary.failing_case_count == 0 ? 0 : 2;
  }

  if (program.is_subcommand_used("pub")) {
    vlink::bench::Bench::WorkerOptions options;
    std::string error;

    if VUNLIKELY (!build_worker_options(pub_command, false, options, error)) {
      std::cerr << error << std::endl;
      return 1;
    }

    return vlink::bench::Bench::run_pub_worker(options);
  }

  if (program.is_subcommand_used("sub")) {
    vlink::bench::Bench::WorkerOptions options;
    std::string error;

    if VUNLIKELY (!build_worker_options(sub_command, true, options, error)) {
      std::cerr << error << std::endl;
      return 1;
    }

    return vlink::bench::Bench::run_sub_worker(options);
  }

  std::ostringstream help_stream;
  help_stream << program;
  const std::string help_text = help_stream.str();
  VLINK_TERM_OUT.write_raw(help_text.data(), help_text.size());
  VLINK_TERM_OUT.flush();
  return 0;
}
