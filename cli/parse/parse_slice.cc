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

#include "./parse_slice.h"

#include "./parse_context.h"
#include "./parse_expr.h"
#include "./parse_extract.h"
#include "./parse_path.h"
#include "./parse_plan.h"
#include "./parse_proto_cache.h"
#include "./parse_schema.h"
#include "./parse_types.h"

#ifdef VLINK_HAS_PROTOBUF_COMPILER

#include <google/protobuf/compiler/importer.h>
#include <google/protobuf/descriptor.h>
#include <google/protobuf/dynamic_message.h>
#include <google/protobuf/message.h>

#endif

#include <vlink/base/helpers.h>
#include <vlink/extension/bag_plugin_interface.h>
#include <vlink/extension/bag_reader.h>
#include <vlink/extension/bag_writer.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <memory>
#include <nlohmann/json.hpp>
#include <sstream>
#include <string>
#include <system_error>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#ifdef VLINK_HAS_PROTOBUF_COMPILER

static constexpr int64_t kMaxSliceCount = 100000;
static constexpr int64_t kWriterTimestampMarginUs = 100000;
static constexpr int64_t kMaxPlaybackTimeMs = (std::numeric_limits<int64_t>::max() - kWriterTimestampMarginUs) / 1000;
static constexpr int64_t kMaxPluginTimestampUs = kMaxPlaybackTimeMs * 1000;
static constexpr int64_t kMaxVcapStartTimestampMs = std::numeric_limits<int64_t>::max() / 1000000;
static constexpr int64_t kMaxVcapOutputTimestampMs =
    (std::numeric_limits<int64_t>::max() - kWriterTimestampMarginUs * 1000) / 1000000;

struct QualityStats final {
  int64_t last_timestamp_us{-1};
  int64_t message_count{0};
  int64_t gap_count{0};
  int64_t gap_sample_count{0};
  int64_t max_gap_us{0};
  int64_t min_gap_us{std::numeric_limits<int64_t>::max()};
  int64_t total_gap_us{0};
};

struct SliceStats final {
  int index{0};
  std::string file_name;
  int64_t begin_time_ms{0};
  int64_t end_time_ms{0};
  int64_t begin_time_us{0};
  int64_t end_time_us{0};
  int64_t message_count{0};
  std::unordered_set<std::string> urls;
};

struct ResolvedTypes {
  std::string ser;
  vlink::SchemaType schema_type{vlink::SchemaType::kUnknown};
  vlink::SchemaType resolved{vlink::SchemaType::kUnknown};
};

class BoundedReadBagPlugin final : public vlink::BagPluginInterface {
 public:
  BoundedReadBagPlugin(std::shared_ptr<vlink::BagPluginInterface> plugin, int64_t begin_us, int64_t end_us)
      : plugin_(std::move(plugin)), begin_us_(begin_us), end_us_(end_us) {
    plugin_->bind_direction(vlink::BagPluginInterface::kRead);
    plugin_->register_callback([this](const vlink::Frame& frame) { do_callback(frame); });
  }

  ~BoundedReadBagPlugin() override { plugin_->register_callback({}); }

  bool convert_url_meta(std::string& url, std::string& ser_type, vlink::SchemaType& schema_type) override {
    return plugin_->convert_url_meta(url, ser_type, schema_type);
  }

  void on_read(const vlink::Frame& frame) override {
    if (frame.timestamp >= begin_us_ && frame.timestamp < end_us_) {
      plugin_->on_read(frame);
    }
  }

  void on_write(const vlink::Frame& frame) override { plugin_->on_write(frame); }

  void on_reset() override { plugin_->on_reset(); }

  void flush() override { plugin_->flush(); }

 private:
  std::shared_ptr<vlink::BagPluginInterface> plugin_;
  int64_t begin_us_{0};
  int64_t end_us_{0};
};

static std::shared_ptr<vlink::BagPluginInterface> bind_bounded_read_plugin(
    const std::shared_ptr<vlink::BagReader>& player, int64_t begin_ms, int64_t end_ms) {
  auto plugin = vlink::parse::ParseContext::get().bag_plugin_interface;

  if (!player || !plugin) {
    return nullptr;
  }

  auto bounded = std::make_shared<BoundedReadBagPlugin>(std::move(plugin), begin_ms * 1000, end_ms * 1000);
  player->bind_bag_interface(bounded);
  return bounded;
}

static bool checked_subtract(int64_t lhs, int64_t rhs, int64_t& result) {
  if ((rhs > 0 && lhs < std::numeric_limits<int64_t>::min() + rhs) ||
      (rhs < 0 && lhs > std::numeric_limits<int64_t>::max() + rhs)) {
    return false;
  }

  result = lhs - rhs;
  return true;
}

static bool plugin_timestamp_supported(int64_t timestamp_us) {
  return timestamp_us >= 0 && timestamp_us < kMaxPluginTimestampUs;
}

static bool read_json_int64(const nlohmann::json& object, const char* key, int64_t& result) {
  if (!object.contains(key)) {
    return false;
  }

  const auto& value = object[key];

  if (!value.is_number_integer()) {
    return false;
  }

  if (value.is_number_unsigned()) {
    const auto unsigned_value = value.get<uint64_t>();

    if (unsigned_value > static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) {
      return false;
    }

    result = static_cast<int64_t>(unsigned_value);
  } else {
    result = value.get<int64_t>();
  }

  return true;
}

static bool resolve_time_range(const vlink::parse::SliceOptions& opt, const vlink::BagReader::Info& info,
                               int64_t& effective_begin, int64_t& effective_end) {
  if VUNLIKELY (!info.has_completed) {
    std::cerr << "Slice/scan requires a completed bag; repair or finalize the input first." << std::endl;
    return false;
  }

  if VUNLIKELY (info.message_count < 0 || info.start_timestamp < 0 || info.total_duration < 0 ||
                info.blank_duration < 0 || info.total_duration < info.blank_duration) {
    std::cerr << "Invalid bag time metadata." << std::endl;
    return false;
  }

  if VUNLIKELY (info.message_count == 0) {
    std::cerr << "Bag contains no messages." << std::endl;
    return false;
  }

  int64_t bag_end = info.total_duration;

  if (bag_end < std::numeric_limits<int64_t>::max()) {
    ++bag_end;
  }

  effective_begin = opt.begin_time_set ? opt.begin_time : info.blank_duration;
  effective_end = opt.end_time_set ? std::min(opt.end_time, bag_end) : bag_end;

  if VUNLIKELY (bag_end > kMaxPlaybackTimeMs || effective_begin < 0 || effective_end < 0 ||
                effective_begin > kMaxPlaybackTimeMs || effective_end > kMaxPlaybackTimeMs) {
    std::cerr << "Playback time range is too large." << std::endl;
    return false;
  }

  if VUNLIKELY (effective_begin >= effective_end) {
    std::cerr << "Invalid time range." << std::endl;
    return false;
  }

  if (info.storage_type == "vcap") {
    if VUNLIKELY (info.start_timestamp > kMaxVcapStartTimestampMs ||
                  effective_begin > kMaxVcapStartTimestampMs - info.start_timestamp) {
      std::cerr << "VCAP playback start time is too large." << std::endl;
      return false;
    }
  }

  return true;
}

static int64_t dropout_threshold_to_us(double seconds) {
  const double threshold_us = seconds * 1000000.0;

  if (threshold_us >= 0x1p63) {
    return std::numeric_limits<int64_t>::max();
  }

  return static_cast<int64_t>(threshold_us);
}

static ResolvedTypes resolve_url_types(const std::string& url, const std::string& meta_ser,
                                       vlink::SchemaType meta_schema_type,
                                       const std::unordered_map<std::string, UrlSchemaOverride>& overrides) {
  ResolvedTypes types{meta_ser, meta_schema_type, vlink::SchemaType::kUnknown};
  auto iter = overrides.find(url);

  if (iter != overrides.end()) {
    types.ser = iter->second.ser_type;
    types.schema_type = iter->second.schema_type == vlink::SchemaType::kUnknown
                            ? vlink::SchemaData::resolve_type(meta_schema_type, types.ser)
                            : iter->second.schema_type;
  }

  types.resolved = vlink::SchemaData::resolve_type(types.schema_type, types.ser);
  return types;
}

static bool meta_action_selected(const vlink::BagReader::Info::UrlMeta& meta, const std::vector<int>& actions) {
  if (meta.action_type != vlink::ActionType::kUnknownAction) {
    return vlink::parse::action_selected(actions, meta.action_type);
  }

  for (auto action : actions) {
    if (action == static_cast<int>(vlink::ActionType::kUnknownAction) ||
        (meta.url_type == "Method" && action >= static_cast<int>(vlink::ActionType::kClientRequest) &&
         action <= static_cast<int>(vlink::ActionType::kServerResponse)) ||
        (meta.url_type == "Event" && action >= static_cast<int>(vlink::ActionType::kPublish) &&
         action <= static_cast<int>(vlink::ActionType::kSubscribe)) ||
        (meta.url_type == "Field" && action >= static_cast<int>(vlink::ActionType::kSet) &&
         action <= static_cast<int>(vlink::ActionType::kGet))) {
      return true;
    }
  }

  return false;
}

static bool validate_field_extraction_topics(const vlink::BagReader::Info& info,
                                             const vlink::parse::UrlSelection& selection,
                                             const std::vector<int>& actions,
                                             const std::unordered_map<std::string, UrlSchemaOverride>& overrides,
                                             vlink::parse::ProtoMessageCache& proto_cache, std::string_view mode) {
  auto& ctx = vlink::parse::ParseContext::get();

  if (ctx.field_specs.empty()) {
    return true;
  }

  for (const auto& meta : info.url_metas) {
    if ((!selection.all && selection.urls.count(meta.url) == 0) || !meta_action_selected(meta, actions)) {
      continue;
    }

    if (meta.url_type == "Method") {
      std::cerr << mode << " field extraction does not support Method URL: " << meta.url << std::endl;
      return false;
    }

    auto types = resolve_url_types(meta.url, meta.ser_type, meta.schema_type, overrides);

    if (types.resolved == vlink::SchemaType::kFlatbuffers) {
      std::cerr << mode << " field extraction does not support FlatBuffers topic: " << meta.url
                << ". Narrow --urls/--url_filter to supported topics or export this topic without -c/--filter/--event."
                << std::endl;
      return false;
    }

    if (types.resolved == vlink::SchemaType::kProtobuf && proto_cache.get(types.ser) == nullptr) {
      std::cerr << mode << " protobuf field extraction requires a schema for '" << types.ser << "' (URL: " << meta.url
                << "). Use -d/--proto_dir, --schema_config, or VLINK_SCHEMA_PLUGIN." << std::endl;
      return false;
    }

    if VUNLIKELY (types.resolved == vlink::SchemaType::kZeroCopy &&
                  vlink::zerocopy::MessageParser::detect_type(types.ser) == vlink::zerocopy::MessageParser::kUnknown) {
      std::cerr << mode << " field extraction does not support ZeroCopy type '" << types.ser << "' (URL: " << meta.url
                << ")." << std::endl;
      return false;
    }

    if (types.resolved != vlink::SchemaType::kZeroCopy && types.resolved != vlink::SchemaType::kProtobuf) {
      std::cerr << mode << " field extraction supports only ZeroCopy and Protobuf topics; unsupported URL: " << meta.url
                << std::endl;
      return false;
    }
  }

  return true;
}

static bool ensure_output_dir(const std::filesystem::path& dir, const std::string& display_path) {
  std::error_code fs_ec;

  if (!std::filesystem::exists(dir, fs_ec) && !fs_ec) {
    std::filesystem::create_directories(dir, fs_ec);

    if (fs_ec) {
      std::cerr << "Failed to create output directory: " << display_path << " (" << fs_ec.message() << ")" << std::endl;
      return false;
    }
  }

  if (!std::filesystem::is_directory(dir, fs_ec) || fs_ec) {
    std::cerr << "Output path is not a directory: " << display_path << std::endl;
    return false;
  }

  return true;
}

static std::string infer_output_suffix(const vlink::parse::SliceOptions& opt) {
  auto suffix = vlink::parse::sanitize_suffix(opt.suffix);

  if (!suffix.empty()) {
    return suffix;
  }

  auto src_ext =
      vlink::parse::to_lower_copy(vlink::parse::path_to_utf8(vlink::parse::utf8_to_path(opt.bag_file).extension()));

  if (src_ext == ".vdbx") {
    suffix = ".vdb";
  } else if (src_ext == ".vcapx") {
    suffix = ".vcap";
  } else {
    suffix = src_ext;
  }

  return vlink::parse::sanitize_suffix(suffix);
}

bool collect_protected_input_paths(const vlink::parse::SliceOptions& opt,
                                   std::vector<std::filesystem::path>& protected_paths) {
  const auto index_path = vlink::parse::utf8_to_path(opt.bag_file);

  if VUNLIKELY (index_path.empty()) {
    std::cerr << "Invalid input bag path: " << opt.bag_file << std::endl;
    return false;
  }

  protected_paths = {index_path};

  for (const auto* input_path : {&opt.schema_config_path, &opt.segments_file}) {
    if (input_path->empty()) {
      continue;
    }

    auto filesys_input_path = vlink::parse::utf8_to_path(*input_path);

    if VUNLIKELY (filesys_input_path.empty()) {
      std::cerr << "Invalid input path: " << *input_path << std::endl;
      return false;
    }

    protected_paths.emplace_back(std::move(filesys_input_path));
  }

  auto suffix = vlink::parse::to_lower_copy(vlink::Helpers::path_to_string(index_path.extension()));

  auto protect_sqlite_sidecars = [&protected_paths](const std::filesystem::path& database_path) {
    std::error_code ec;
    const auto canonical_path = std::filesystem::weakly_canonical(database_path, ec);

    if VUNLIKELY (ec) {
      std::cerr << "Failed to resolve input bag path: " << ec.message() << std::endl;
      return false;
    }

    for (const auto* sidecar_suffix : {"-wal", "-shm"}) {
      auto sidecar = canonical_path;
      sidecar += sidecar_suffix;

      if (std::filesystem::exists(sidecar, ec)) {
        protected_paths.emplace_back(std::move(sidecar));
      }

      if VUNLIKELY (ec) {
        std::cerr << "Failed to inspect input bag sidecar: " << ec.message() << std::endl;
        return false;
      }
    }

    return true;
  };

  if (suffix != ".vdbx" && suffix != ".vcapx") {
    return suffix != ".vdb" || protect_sqlite_sidecars(index_path);
  }

  try {
    std::ifstream index_file(index_path);

    if VUNLIKELY (!index_file.is_open()) {
      std::cerr << "Failed to open bag index: " << opt.bag_file << std::endl;
      return false;
    }

    nlohmann::json root;
    index_file >> root;

    const auto& files = root.at("VLinkFiles");

    if VUNLIKELY (!files.is_array() || files.empty()) {
      std::cerr << "Bag index contains no source files: " << opt.bag_file << std::endl;
      return false;
    }

    protected_paths.reserve(protected_paths.size() + files.size());

    for (const auto& file : files) {
      if VUNLIKELY (!file.is_string()) {
        std::cerr << "Bag index contains an invalid source path: " << opt.bag_file << std::endl;
        return false;
      }

      const auto source_string = file.get<std::string>();

      auto source_path = vlink::parse::utf8_to_path(source_string);

      if VUNLIKELY (source_path.empty()) {
        std::cerr << "Bag index contains an invalid source path: " << opt.bag_file << std::endl;
        return false;
      }

      if (source_path.is_relative()) {
        source_path = index_path.parent_path() / source_path;
      }

      if VUNLIKELY (suffix == ".vdbx" && !protect_sqlite_sidecars(source_path)) {
        return false;
      }

      protected_paths.emplace_back(std::move(source_path));
    }
  } catch (const nlohmann::json::exception& e) {
    std::cerr << "Failed to parse bag index " << opt.bag_file << ": " << e.what() << std::endl;
    return false;
  }

  return true;
}

static bool load_segments_from_file(const std::string& path, int64_t bag_start_ts,
                                    std::vector<vlink::parse::SegmentDef>& segments) {
  std::ifstream seg_file(vlink::parse::utf8_to_path(path));

  if VUNLIKELY (!seg_file.is_open()) {
    std::cerr << "Failed to open segments file: " << path << std::endl;
    return false;
  }

  try {
    auto seg_json = nlohmann::json::parse(seg_file);
    const auto& seg_arr = seg_json.contains("segments") ? seg_json["segments"] : seg_json;

    if VUNLIKELY (!seg_arr.is_array()) {
      std::cerr << "Segments file must be an array or contain a 'segments' array: " << path << std::endl;
      return false;
    }

    if VUNLIKELY (seg_arr.size() > static_cast<size_t>(kMaxSliceCount)) {
      std::cerr << "Too many segments (" << seg_arr.size() << "), maximum is " << kMaxSliceCount << std::endl;
      return false;
    }

    for (const auto& entry : seg_arr) {
      if VUNLIKELY (!entry.is_object()) {
        std::cerr << "Invalid segment entry: every segment must be an object in " << path << std::endl;
        return false;
      }

      vlink::parse::SegmentDef seg;
      seg.name = entry.value("name", "");

      if (entry.contains("epoch_begin_ms")) {
        int64_t epoch_begin_ms = 0;
        int64_t epoch_end_ms = 0;

        if VUNLIKELY (!read_json_int64(entry, "epoch_begin_ms", epoch_begin_ms) ||
                      !read_json_int64(entry, "epoch_end_ms", epoch_end_ms)) {
          std::cerr << "Invalid segment entry: epoch_begin_ms and epoch_end_ms must both be integer milliseconds."
                    << std::endl;
          return false;
        }

        if VUNLIKELY (!checked_subtract(epoch_begin_ms, bag_start_ts, seg.begin_ms) ||
                      !checked_subtract(epoch_end_ms, bag_start_ts, seg.end_ms)) {
          std::cerr << "Segment epoch time is outside the supported range." << std::endl;
          return false;
        }
      } else {
        if VUNLIKELY (!read_json_int64(entry, "begin_ms", seg.begin_ms) ||
                      !read_json_int64(entry, "end_ms", seg.end_ms)) {
          std::cerr << "Invalid segment entry: begin_ms and end_ms must both be integer milliseconds." << std::endl;
          return false;
        }
      }

      if (seg.name.empty()) {
        seg.name = "seg_" + std::to_string(segments.size());
      }

      segments.emplace_back(std::move(seg));
    }
  } catch (const nlohmann::json::exception& e) {
    std::cerr << "Failed to parse segments file: " << e.what() << std::endl;
    return false;
  }

  if VUNLIKELY (segments.empty()) {
    std::cerr << "No valid segments found in " << path << std::endl;
    return false;
  }

  return true;
}

static void build_window_segments(int64_t effective_begin, int64_t effective_end, int64_t window_ms,
                                  std::vector<vlink::parse::SegmentDef>& segments) {
  const int64_t span = effective_end - effective_begin;
  const int64_t count = span / window_ms + (span % window_ms != 0 ? 1 : 0);
  int pad_width = 1;

  for (int64_t n = count - 1; n >= 10; n /= 10) {
    ++pad_width;
  }

  if (pad_width < 3) {
    pad_width = 3;
  }

  segments.reserve(static_cast<size_t>(count));

  for (int64_t i = 0; i < count; ++i) {
    vlink::parse::SegmentDef seg;
    std::ostringstream oss;
    oss << "slice_" << std::setfill('0') << std::setw(pad_width) << i;
    seg.name = oss.str();
    seg.begin_ms = effective_begin + i * window_ms;
    seg.end_ms = i + 1 == count ? effective_end : seg.begin_ms + window_ms;
    segments.emplace_back(std::move(seg));
  }
}

#ifdef VLINK_ENABLE_EXPRTK

static void merge_overlapping_event_segments(std::vector<vlink::parse::SegmentDef>& segments) {
  std::sort(segments.begin(), segments.end(), [](const vlink::parse::SegmentDef& a, const vlink::parse::SegmentDef& b) {
    return a.begin_ms < b.begin_ms;
  });

  std::vector<vlink::parse::SegmentDef> merged;

  for (const auto& seg : segments) {
    if (!merged.empty() && seg.begin_ms <= merged.back().end_ms) {
      merged.back().end_ms = std::max(merged.back().end_ms, seg.end_ms);
    } else {
      merged.emplace_back(seg);
    }
  }

  for (size_t i = 0; i < merged.size(); ++i) {
    merged[i].name = "event_" + std::to_string(i);
  }

  segments = std::move(merged);
}

#endif

static void update_quality_stats(QualityStats& qs, int64_t timestamp_us, int64_t dropout_threshold_us) {
  ++qs.message_count;

  if (qs.last_timestamp_us >= 0) {
    int64_t gap = timestamp_us - qs.last_timestamp_us;
    ++qs.gap_sample_count;
    qs.total_gap_us += gap;
    qs.min_gap_us = std::min(qs.min_gap_us, gap);
    qs.max_gap_us = std::max(qs.max_gap_us, gap);

    if (gap > dropout_threshold_us) {
      ++qs.gap_count;
    }
  }

  qs.last_timestamp_us = timestamp_us;
}

static nlohmann::ordered_json build_scan_header(const vlink::parse::SliceOptions& opt, size_t event_count) {
  nlohmann::ordered_json scan_json;
  scan_json["schema_version"] = 1;
  scan_json["time_base"] = "relative_ms";
  scan_json["source"] = opt.bag_file;
  scan_json["event_expr"] = opt.event_expr;
  scan_json["pre_seconds"] = opt.event_pre;
  scan_json["post_seconds"] = opt.event_post;
  scan_json["event_count"] = event_count;
  return scan_json;
}

static nlohmann::ordered_json build_quality_object(const std::unordered_map<std::string, QualityStats>& quality_map,
                                                   double dropout_threshold_ms) {
  nlohmann::ordered_json quality_json = nlohmann::ordered_json::object();
  std::vector<std::string> sorted_urls;
  sorted_urls.reserve(quality_map.size());

  for (const auto& [url, qs] : quality_map) {
    (void)qs;
    sorted_urls.emplace_back(url);
  }

  std::sort(sorted_urls.begin(), sorted_urls.end());

  for (const auto& url : sorted_urls) {
    const auto& qs = quality_map.at(url);
    nlohmann::ordered_json qj;
    qj["message_count"] = qs.message_count;
    qj["gap_count"] = qs.gap_count;
    qj["max_gap_ms"] = qs.max_gap_us / 1000.0;
    qj["min_gap_ms"] = qs.gap_sample_count > 0 ? qs.min_gap_us / 1000.0 : 0.0;
    qj["avg_hz"] = (qs.gap_sample_count > 0 && qs.total_gap_us > 0)
                       ? (static_cast<double>(qs.gap_sample_count) * 1000000.0 / static_cast<double>(qs.total_gap_us))
                       : 0.0;
    qj["dropout_threshold_ms"] = dropout_threshold_ms;
    quality_json[url] = qj;
  }

  return quality_json;
}

static bool write_scan_json(const std::filesystem::path& out_dir, const std::string& scan_output_name,
                            const nlohmann::ordered_json& scan_json) {
  auto& ctx = vlink::parse::ParseContext::get();
  auto scan_path = out_dir / scan_output_name;
  auto scan_path_display = vlink::parse::path_to_utf8(scan_path);
  std::ofstream scan_file(scan_path);

  if (!scan_file.is_open()) {
    std::cerr << "Failed to write scan result: " << scan_path_display << std::endl;
    return false;
  }

  scan_file << scan_json.dump(2);
  scan_file.close();

  if (!scan_file.good()) {
    std::cerr << "Failed to write scan result: " << scan_path_display << std::endl;
    return false;
  }

  if (!ctx.quiet_flag) {
    std::cout << "Saved " << scan_path_display << std::endl;
  }

  return true;
}

#ifdef VLINK_ENABLE_EXPRTK

static bool extract_event_values(
    const std::string& url, const std::string& ser, vlink::SchemaType resolved_schema_type, const vlink::Bytes& data,
    int64_t timestamp_ms, vlink::parse::ProtoMessageCache& proto_cache,
    const std::vector<std::vector<std::string>>& event_field_paths,
    std::unordered_map<std::string, std::unordered_map<std::string, vlink::parse::EventVarState>>& cross_topic_state,
    vlink::parse::ExprContext& event_ctx, std::vector<bool>& vars_ready) {
  auto& ctx = vlink::parse::ParseContext::get();
  bool any_numeric_found = false;

  auto assign = [&event_ctx, &vars_ready, &any_numeric_found, &cross_topic_state, &url, &timestamp_ms](
                    size_t fi, const VariantType& val) {
    double dv = 0.0;

    if (!variant_to_double(val, dv)) {
      return;
    }

    event_ctx.set_value(fi, dv);
    vars_ready[fi] = true;
    any_numeric_found = true;
    cross_topic_state[url][event_ctx.var_names()[fi]] = vlink::parse::EventVarState{dv, timestamp_ms};
  };

  if (resolved_schema_type == vlink::SchemaType::kZeroCopy) {
    vlink::zerocopy::MessageParser parser;

    if (!parser.parse(ser, data)) {
      return false;
    }

    for (size_t fi = 0; fi < ctx.field_specs.size(); ++fi) {
      VariantType val;

      if (extract_zerocopy_value(parser, ctx.field_specs[fi], val)) {
        assign(fi, val);
      }
    }
  } else if (resolved_schema_type == vlink::SchemaType::kProtobuf && proto_cache.ready()) {
    auto* msg = proto_cache.get(ser);

    if (msg == nullptr || data.size() > static_cast<size_t>(std::numeric_limits<int>::max()) ||
        !msg->ParseFromArray(data.data(), static_cast<int>(data.size()))) {
      return false;
    }

    for (size_t fi = 0; fi < ctx.field_specs.size(); ++fi) {
      VariantType val;

      if (extract_proto_value(*msg, event_field_paths[fi], 0, val)) {
        assign(fi, val);
      }
    }
  }

  return any_numeric_found;
}

static void fill_event_cross_topic_state(
    int64_t timestamp_ms, int64_t event_state_max_age_ms,
    const std::unordered_map<std::string, std::unordered_map<std::string, vlink::parse::EventVarState>>&
        cross_topic_state,
    const std::vector<std::string>& var_names, vlink::parse::ExprContext& event_ctx, std::vector<bool>& vars_ready) {
  for (size_t fi = 0; fi < var_names.size(); ++fi) {
    if (vars_ready[fi]) {
      continue;
    }

    bool filled = false;
    int64_t newest_timestamp_ms = std::numeric_limits<int64_t>::min();
    double newest_value = 0.0;
    std::string_view newest_topic_url;

    for (const auto& [topic_url, topic_state] : cross_topic_state) {
      auto iter = topic_state.find(var_names[fi]);

      if (iter == topic_state.end()) {
        continue;
      }

      int64_t age_ms = timestamp_ms - iter->second.timestamp_ms;

      if (age_ms < 0) {
        continue;
      }

      if (event_state_max_age_ms > 0 && age_ms > event_state_max_age_ms) {
        continue;
      }

      if (!filled || iter->second.timestamp_ms > newest_timestamp_ms ||
          (iter->second.timestamp_ms == newest_timestamp_ms && topic_url < newest_topic_url)) {
        newest_timestamp_ms = iter->second.timestamp_ms;
        newest_value = iter->second.value;
        newest_topic_url = topic_url;
        filled = true;
      }
    }

    if (filled) {
      event_ctx.set_value(fi, newest_value);
      vars_ready[fi] = true;
    }
  }
}

#endif

static int run_quality_only_scan(const vlink::parse::SliceOptions& opt, const std::filesystem::path& out_dir,
                                 const std::string& scan_output_name) {
  auto& ctx = vlink::parse::ParseContext::get();
  std::shared_ptr<vlink::BagReader> player;

  try {
    player = vlink::BagReader::create(opt.bag_file, true);
  } catch (vlink::Exception::RuntimeError& e) {
    std::cerr << e.what() << std::endl;
    return -1;
  }

  if VUNLIKELY (!player) {
    std::cerr << "Unsupported bag file suffix: " << opt.bag_file << std::endl;
    return -1;
  }

  int64_t effective_begin = 0;
  int64_t effective_end = 0;

  if VUNLIKELY (!resolve_time_range(opt, player->get_info(), effective_begin, effective_end)) {
    return -1;
  }

  auto bounded_plugin = bind_bounded_read_plugin(player, effective_begin, effective_end);

  vlink::parse::UrlSelection selection;

  if VUNLIKELY (!vlink::parse::build_url_selection(player->get_info(), opt.urls, opt.url_filter, opt.black_mode,
                                                   opt.target_url, selection)) {
    return -1;
  }

  std::unordered_map<std::string, QualityStats> quality_map;
  const int64_t dropout_threshold_us = dropout_threshold_to_us(opt.dropout_threshold);
  bool has_last_timestamp = false;
  int64_t last_timestamp = 0;
  bool order_error = false;
  bool plugin_timestamp_error = false;

  auto record_quality = [&ctx, &selection, &opt, &quality_map, &dropout_threshold_us, &has_last_timestamp,
                         &last_timestamp, &order_error, &effective_begin, &effective_end,
                         &plugin_timestamp_error](const vlink::Frame& frame) {
    const int64_t timestamp = frame.timestamp;
    const std::string& url = frame.url;
    const vlink::ActionType action_type = frame.action_type;

    if VUNLIKELY (ctx.has_quit || order_error) {
      return;
    }

    const bool selected =
        (selection.all || selection.urls.count(url) != 0) && vlink::parse::action_selected(opt.actions, action_type);

    if (ctx.bag_plugin_interface && !selected) {
      return;
    }

    if (has_last_timestamp && timestamp < last_timestamp) {
      order_error = true;
      return;
    }

    has_last_timestamp = true;
    last_timestamp = timestamp;

    if (!ctx.bag_plugin_interface && (timestamp < effective_begin * 1000 || timestamp >= effective_end * 1000)) {
      return;
    }

    if (!selected) {
      return;
    }

    if (ctx.bag_plugin_interface && !plugin_timestamp_supported(timestamp)) {
      plugin_timestamp_error = true;
      return;
    }

    update_quality_stats(quality_map[url], timestamp, dropout_threshold_us);
  };

  player->register_output_callback(std::move(record_quality));

  vlink::BagReader::Config play_config;
  play_config.begin_time = ctx.bag_plugin_interface ? effective_begin : 0;
  play_config.end_time = ctx.bag_plugin_interface ? effective_end : 0;
  play_config.times = 1;
  play_config.rate = 1.0;
  play_config.force_delay = 0;
  play_config.auto_quit = true;

  if (ctx.bag_plugin_interface && !selection.all) {
    play_config.filter_urls = selection.urls;
  }

  if (!ctx.quiet_flag) {
    std::cout << "Scanning for quality stats..." << std::endl;
  }

  player->play(play_config);

  try {
    player->run();
  } catch (const std::exception& e) {
    std::cerr << "Scan playback error: " << e.what() << std::endl;
    player.reset();
    return -1;
  }

  player.reset();

  if (plugin_timestamp_error) {
    std::cerr << "Plugin output timestamp is outside the supported range." << std::endl;
    return -1;
  }

  if (order_error) {
    std::cerr << "Scan requires non-decreasing input timestamps." << std::endl;
    return -1;
  }

  auto scan_json = build_scan_header(opt, 0);
  scan_json["events"] = nlohmann::ordered_json::array();
  scan_json["quality"] = build_quality_object(quality_map, opt.dropout_threshold * 1000.0);
  scan_json["segments"] = nlohmann::ordered_json::array();

  return write_scan_json(out_dir, scan_output_name, scan_json) ? 0 : -1;
}

// NOLINTNEXTLINE(google-readability-function-size)
int start_slice(const vlink::parse::SliceOptions& opt) {
  auto& ctx = vlink::parse::ParseContext::get();

  if VUNLIKELY (!vlink::parse::validate_common_slice_options(opt)) {
    return -1;
  }

  std::vector<std::filesystem::path> protected_input_paths;
  auto filesys_out_dir = vlink::parse::utf8_to_path(opt.out_dir);

  std::string scan_output_name;

  if (opt.scan_only) {
    if VUNLIKELY (!ensure_output_dir(filesys_out_dir, opt.out_dir)) {
      return -1;
    }

    scan_output_name = vlink::parse::sanitize_file_component(opt.scan_output_name, "events.json");

    if (scan_output_name != opt.scan_output_name && !ctx.quiet_flag) {
      std::cerr << "Warning: unsafe scan output file name sanitized to " << scan_output_name << std::endl;
    }

    if VUNLIKELY (opt.force && !collect_protected_input_paths(opt, protected_input_paths)) {
      return -1;
    }

    if VUNLIKELY (!vlink::parse::preflight_output_files(filesys_out_dir, {scan_output_name}, opt.force,
                                                        protected_input_paths)) {
      return -1;
    }
  }

  SchemaConfig schema_config;

  if (!opt.schema_config_path.empty()) {
    if VUNLIKELY (!load_schema_config(opt.schema_config_path, schema_config)) {
      return -1;
    }
  }

  auto url_ser_override = build_url_schema_overrides(schema_config);

  if (opt.quality_only) {
    return run_quality_only_scan(opt, filesys_out_dir, scan_output_name);
  }

  std::shared_ptr<vlink::BagReader> player;

  try {
    player = vlink::BagReader::create(opt.bag_file, true);
  } catch (vlink::Exception::RuntimeError& e) {
    std::cerr << e.what() << std::endl;
    return -1;
  }

  if VUNLIKELY (!player) {
    std::cerr << "Unsupported bag file suffix: " << opt.bag_file << std::endl;
    return -1;
  }

  int64_t effective_begin = 0;
  int64_t effective_end = 0;

  if VUNLIKELY (!resolve_time_range(opt, player->get_info(), effective_begin, effective_end)) {
    return -1;
  }

  const int64_t playback_begin = effective_begin;
  const int64_t playback_end = effective_end;
  auto bounded_plugin = bind_bounded_read_plugin(player, playback_begin, playback_end);

  vlink::parse::UrlSelection slice_selection;

  if VUNLIKELY (!vlink::parse::build_url_selection(player->get_info(), opt.urls, opt.url_filter, opt.black_mode,
                                                   opt.target_url, slice_selection)) {
    return -1;
  }

  auto window_ms = vlink::parse::seconds_to_milliseconds(opt.window_seconds);

  if (!opt.event_expr.empty() && opt.event_pre <= 0 && opt.event_post <= 0) {
    std::cerr << "--pre and --post cannot both be zero for event slicing/scanning." << std::endl;
    return -1;
  }

  auto output_suffix = infer_output_suffix(opt);

  if VUNLIKELY (output_suffix.empty()) {
    std::cerr << "Can not infer output suffix from source file; use --suffix." << std::endl;
    return -1;
  }

  std::vector<vlink::parse::SegmentDef> segments;

  if (!opt.segments_file.empty()) {
    if VUNLIKELY (!load_segments_from_file(opt.segments_file, player->get_info().start_timestamp, segments)) {
      return -1;
    }
  } else if (!opt.event_expr.empty()) {
    if VUNLIKELY (ctx.field_specs.empty() && !opt.quality_check) {
      std::cerr << "--event requires -c to specify fields for expression variables." << std::endl;
      return -1;
    }

    if (!ctx.quiet_flag) {
      std::cout << "Scanning for events..." << std::endl;
    }

    auto event_pre_ms = vlink::parse::seconds_to_milliseconds(opt.event_pre);
    auto event_post_ms = vlink::parse::seconds_to_milliseconds(opt.event_post);

    auto scan_proto_runtime = load_proto_runtime(collect_proto_dirs(schema_config, opt.proto_dir));
    vlink::parse::ProtoMessageCache scan_proto_cache(scan_proto_runtime);

#ifdef VLINK_ENABLE_EXPRTK

    vlink::parse::ExprContext event_ctx;
    std::unordered_map<std::string, QualityStats> quality_map;
    bool quality_check = opt.quality_check;

    if VUNLIKELY (!event_ctx.compile_single(ctx.field_specs, opt.event_expr)) {
      std::cerr << "Failed to compile event expression: " << opt.event_expr << std::endl;
      return -1;
    }

    std::unordered_map<std::string, std::unordered_map<std::string, vlink::parse::EventVarState>> cross_topic_state;
    const int64_t dropout_threshold_us = dropout_threshold_to_us(opt.dropout_threshold);
    auto event_state_max_age_ms = vlink::parse::seconds_to_milliseconds(opt.event_state_max_age);
    auto event_min_interval_ms = vlink::parse::seconds_to_milliseconds(opt.event_min_interval);

    std::vector<int64_t> event_timestamps_ms;
    bool event_active = false;
    int64_t last_event_timestamp_ms = 0;
    bool has_last_event_timestamp = false;
    bool has_plugin_output_timestamp = false;
    int64_t plugin_output_begin_us = 0;
    int64_t plugin_output_end_us = 0;
    bool has_last_timestamp = false;
    int64_t last_timestamp = 0;
    bool order_error = false;
    bool plugin_timestamp_error = false;

    auto scan_player = std::move(player);

    if VUNLIKELY (!validate_field_extraction_topics(scan_player->get_info(), slice_selection, opt.actions,
                                                    url_ser_override, scan_proto_cache, "Scan/event")) {
      return -1;
    }

    const bool scan_with_plugin = ctx.bag_plugin_interface != nullptr;

    auto process_event_frame = [&ctx, &slice_selection, &opt, &quality_check, &quality_map, &dropout_threshold_us,
                                &url_ser_override, &cross_topic_state, &scan_proto_cache, &event_ctx,
                                &event_state_max_age_ms, &event_active, &last_event_timestamp_ms,
                                &has_last_event_timestamp, &event_min_interval_ms, &event_timestamps_ms,
                                &effective_begin, &effective_end, &scan_with_plugin, &has_plugin_output_timestamp,
                                &plugin_output_begin_us, &plugin_output_end_us, &has_last_timestamp, &last_timestamp,
                                &order_error, &plugin_timestamp_error](const vlink::Frame& frame) {
      const int64_t timestamp = frame.timestamp;
      const std::string& url = frame.url;
      const vlink::ActionType action_type = frame.action_type;
      const vlink::Bytes& data = frame.data;

      if VUNLIKELY (ctx.has_quit || order_error) {
        return;
      }

      const bool selected = (slice_selection.all || slice_selection.urls.count(url) != 0) &&
                            vlink::parse::action_selected(opt.actions, action_type);

      if (scan_with_plugin && !selected) {
        return;
      }

      if (has_last_timestamp && timestamp < last_timestamp) {
        order_error = true;
        return;
      }

      has_last_timestamp = true;
      last_timestamp = timestamp;

      if (!scan_with_plugin && (timestamp < effective_begin * 1000 || timestamp >= effective_end * 1000)) {
        return;
      }

      if (!selected) {
        return;
      }

      if (scan_with_plugin) {
        if (!plugin_timestamp_supported(timestamp)) {
          plugin_timestamp_error = true;
          return;
        }

        if (!has_plugin_output_timestamp) {
          plugin_output_begin_us = timestamp;
          plugin_output_end_us = timestamp;
          has_plugin_output_timestamp = true;
        } else {
          plugin_output_begin_us = std::min(plugin_output_begin_us, timestamp);
          plugin_output_end_us = std::max(plugin_output_end_us, timestamp);
        }
      }

      if (quality_check) {
        update_quality_stats(quality_map[url], timestamp, dropout_threshold_us);
      }

      auto types = resolve_url_types(url, frame.ser_type, frame.schema_type, url_ser_override);
      int64_t timestamp_ms = timestamp / 1000;
      std::vector<bool> vars_ready(ctx.field_specs.size(), false);

      bool any_numeric_found =
          extract_event_values(url, types.ser, types.resolved, data, timestamp_ms, scan_proto_cache, ctx.field_paths,
                               cross_topic_state, event_ctx, vars_ready);

      if (!any_numeric_found) {
        return;
      }

      fill_event_cross_topic_state(timestamp_ms, event_state_max_age_ms, cross_topic_state, event_ctx.var_names(),
                                   event_ctx, vars_ready);

      auto condition_function = [](bool ready) { return !ready; };

      if (std::any_of(vars_ready.begin(), vars_ready.end(), condition_function)) {
        return;
      }

      double result = event_ctx.evaluate_single();
      bool active = (result != 0.0);

      if (active && !event_active &&
          (scan_with_plugin || (timestamp_ms >= effective_begin && timestamp_ms < effective_end)) &&
          (!has_last_event_timestamp || timestamp_ms - last_event_timestamp_ms >= event_min_interval_ms)) {
        event_timestamps_ms.emplace_back(timestamp_ms);
        last_event_timestamp_ms = timestamp_ms;
        has_last_event_timestamp = true;
      }

      event_active = active;
    };

    scan_player->register_output_callback(std::move(process_event_frame));

    vlink::BagReader::Config scan_config;
    scan_config.begin_time = scan_with_plugin ? playback_begin : 0;
    scan_config.end_time = scan_with_plugin ? playback_end : 0;
    scan_config.times = 1;
    scan_config.rate = 1.0;
    scan_config.force_delay = 0;
    scan_config.auto_quit = true;

    if (scan_with_plugin && !slice_selection.all) {
      scan_config.filter_urls = slice_selection.urls;
    }

    scan_player->play(scan_config);

    try {
      scan_player->run();
    } catch (const std::exception& e) {
      std::cerr << "Scan playback error: " << e.what() << std::endl;
      scan_player.reset();
      return -1;
    }

    scan_player.reset();

    if (plugin_timestamp_error) {
      std::cerr << "Plugin output timestamp is outside the supported range." << std::endl;
      return -1;
    }

    if (order_error) {
      std::cerr << "Scan requires non-decreasing input timestamps." << std::endl;
      return -1;
    }

    if (scan_with_plugin && has_plugin_output_timestamp) {
      if VUNLIKELY (plugin_output_begin_us < 0 || plugin_output_end_us < 0) {
        std::cerr << "Plugin output timestamp is outside the supported range." << std::endl;
        return -1;
      }

      effective_begin = plugin_output_begin_us / 1000;
      effective_end = plugin_output_end_us / 1000 + 1;
    }

    if (event_timestamps_ms.empty()) {
      if (!ctx.quiet_flag) {
        std::cout << "No events found." << std::endl;
      }

      if (opt.scan_only) {
        auto scan_json = build_scan_header(opt, 0);
        scan_json["events"] = nlohmann::ordered_json::array();

        if (quality_check) {
          scan_json["quality"] = build_quality_object(quality_map, opt.dropout_threshold * 1000.0);
        }

        scan_json["segments"] = nlohmann::ordered_json::array();

        if (!write_scan_json(filesys_out_dir, scan_output_name, scan_json)) {
          player.reset();
          return -1;
        }
      }

      player.reset();
      return 0;
    }

    for (auto ts_ms : event_timestamps_ms) {
      vlink::parse::SegmentDef seg;
      seg.begin_ms = event_pre_ms >= ts_ms - effective_begin ? effective_begin : ts_ms - event_pre_ms;
      seg.end_ms = event_post_ms >= effective_end - ts_ms ? effective_end : ts_ms + event_post_ms;
      seg.name = "event_" + std::to_string(segments.size());
      segments.emplace_back(std::move(seg));
    }

    merge_overlapping_event_segments(segments);

    if (!ctx.quiet_flag) {
      std::cout << "Found " << event_timestamps_ms.size() << " event(s), merged into " << segments.size()
                << " segment(s)." << std::endl;
    }

    if (opt.scan_only) {
      auto scan_json = build_scan_header(opt, event_timestamps_ms.size());

      nlohmann::ordered_json events_arr = nlohmann::ordered_json::array();

      for (auto ts_ms : event_timestamps_ms) {
        nlohmann::ordered_json ev;
        ev["timestamp_ms"] = ts_ms;
        ev["timestamp_s"] = ts_ms / 1000.0;
        events_arr.emplace_back(ev);
      }

      scan_json["events"] = events_arr;

      if (quality_check) {
        scan_json["quality"] = build_quality_object(quality_map, opt.dropout_threshold * 1000.0);
      }

      nlohmann::ordered_json seg_arr = nlohmann::ordered_json::array();

      for (const auto& seg : segments) {
        nlohmann::ordered_json sj;
        sj["name"] = seg.name;
        sj["begin_ms"] = seg.begin_ms;
        sj["end_ms"] = seg.end_ms;
        sj["duration_ms"] = seg.end_ms - seg.begin_ms;
        seg_arr.emplace_back(sj);
      }

      scan_json["segments"] = seg_arr;

      if (!write_scan_json(filesys_out_dir, scan_output_name, scan_json)) {
        player.reset();
        return -1;
      }

      player.reset();
      return 0;
    }

    player.reset();

    try {
      player = vlink::BagReader::create(opt.bag_file, true);
    } catch (vlink::Exception::RuntimeError& e) {
      std::cerr << e.what() << std::endl;
      return -1;
    }

    if VUNLIKELY (!player) {
      std::cerr << "Unsupported bag file suffix: " << opt.bag_file << std::endl;
      return -1;
    }

    if (bounded_plugin) {
      player->bind_bag_interface(bounded_plugin);
    }

#else
    (void)event_pre_ms;
    (void)event_post_ms;
    std::cerr << "Event expression requires exprtk library." << std::endl;
    return -1;
#endif
  } else if (window_ms > 0) {
    if (ctx.bag_plugin_interface) {
      bool has_plugin_output_timestamp = false;
      int64_t plugin_output_begin_us = 0;
      int64_t plugin_output_end_us = 0;
      int64_t last_plugin_output_us = 0;
      bool plugin_order_error = false;
      bool plugin_timestamp_error = false;

      player->register_output_callback([&](const vlink::Frame& frame) {
        if ((!slice_selection.all && slice_selection.urls.count(frame.url) == 0) ||
            !vlink::parse::action_selected(opt.actions, frame.action_type)) {
          return;
        }

        if (!plugin_timestamp_supported(frame.timestamp)) {
          plugin_timestamp_error = true;
          return;
        }

        if (has_plugin_output_timestamp && frame.timestamp < last_plugin_output_us) {
          plugin_order_error = true;
          return;
        }

        if (!has_plugin_output_timestamp) {
          plugin_output_begin_us = frame.timestamp;
          plugin_output_end_us = frame.timestamp;
          has_plugin_output_timestamp = true;
        } else {
          plugin_output_begin_us = std::min(plugin_output_begin_us, frame.timestamp);
          plugin_output_end_us = std::max(plugin_output_end_us, frame.timestamp);
        }

        last_plugin_output_us = frame.timestamp;
      });

      vlink::BagReader::Config scan_config;
      scan_config.begin_time = playback_begin;
      scan_config.end_time = playback_end;
      scan_config.times = 1;
      scan_config.rate = 1.0;
      scan_config.force_delay = 0;
      scan_config.auto_quit = true;

      if (!slice_selection.all) {
        scan_config.filter_urls = slice_selection.urls;
      }

      player->play(scan_config);

      try {
        player->run();
      } catch (const std::exception& e) {
        std::cerr << "Plugin range scan error: " << e.what() << std::endl;
        player.reset();
        return -1;
      }

      player.reset();

      if (plugin_timestamp_error) {
        std::cerr << "Plugin output timestamp is outside the supported range." << std::endl;
        return -1;
      }

      if (plugin_order_error) {
        std::cerr << "Bag plugin emitted non-monotonic timestamps; slice requires non-decreasing plugin output."
                  << std::endl;
        return -1;
      }

      if (has_plugin_output_timestamp) {
        if VUNLIKELY (plugin_output_begin_us < 0 || plugin_output_end_us < 0) {
          std::cerr << "Plugin output timestamp is outside the supported range." << std::endl;
          return -1;
        }

        effective_begin = plugin_output_begin_us / 1000;
        effective_end = plugin_output_end_us / 1000 + 1;
      }

      try {
        player = vlink::BagReader::create(opt.bag_file, true);
      } catch (vlink::Exception::RuntimeError& e) {
        std::cerr << e.what() << std::endl;
        return -1;
      }

      if VUNLIKELY (!player) {
        std::cerr << "Unsupported bag file suffix: " << opt.bag_file << std::endl;
        return -1;
      }

      if (bounded_plugin) {
        player->bind_bag_interface(bounded_plugin);
      }
    }

    const int64_t span = effective_end - effective_begin;
    const int64_t count = span / window_ms + (span % window_ms != 0 ? 1 : 0);

    if VUNLIKELY (count > kMaxSliceCount) {
      std::cerr << "Too many segments (" << count << "), maximum is " << kMaxSliceCount
                << ". Use a larger --window value." << std::endl;
      return -1;
    }

    build_window_segments(effective_begin, effective_end, window_ms, segments);
  } else {
    std::cerr << "Slice mode requires --window, --segments, or --event." << std::endl;
    return -1;
  }

  if VUNLIKELY (!vlink::parse::normalize_segment_plan(
                    segments, effective_begin, effective_end,
                    opt.segments_file.empty() ? std::string{"generated segments"} : opt.segments_file)) {
    return -1;
  }

  int64_t output_start_timestamp = player->get_info().start_timestamp;

  if (output_start_timestamp == 0) {
    output_start_timestamp = std::chrono::time_point_cast<std::chrono::milliseconds>(std::chrono::system_clock::now())
                                 .time_since_epoch()
                                 .count();

    if VUNLIKELY (output_start_timestamp <= 0) {
      std::cerr << "Failed to establish a valid slice output start timestamp." << std::endl;
      return -1;
    }
  }

  const int64_t max_segment_begin = segments.back().begin_ms;
  const int64_t max_segment_end = segments.back().end_ms;
  const bool vcap_output = output_suffix == ".vcap";

  if VUNLIKELY (output_start_timestamp > std::numeric_limits<int64_t>::max() - max_segment_begin) {
    std::cerr << "Slice start timestamp is outside the supported range." << std::endl;
    return -1;
  }

  if VUNLIKELY (vcap_output && (output_start_timestamp > kMaxVcapOutputTimestampMs ||
                                max_segment_end > kMaxVcapOutputTimestampMs - output_start_timestamp)) {
    std::cerr << "VCAP output timestamp is outside the supported range." << std::endl;
    return -1;
  }

  auto schema_list = player->detect_schema();

  {
    auto external_schemas = import_schemas_from_config(schema_config, opt.proto_dir, opt.fbs_dir);
    std::unordered_map<std::string, size_t> existing_schema_keys;

    for (size_t i = 0; i < schema_list.size(); ++i) {
      existing_schema_keys.emplace(schema_data_key(schema_list[i]), i);
    }

    for (auto& s : external_schemas) {
      auto key = schema_data_key(s);
      auto iter = existing_schema_keys.find(key);

      if (iter == existing_schema_keys.end()) {
        existing_schema_keys.emplace(std::move(key), schema_list.size());
        schema_list.emplace_back(std::move(s));
      } else if (schema_list[iter->second].data.empty() && !s.data.empty()) {
        schema_list[iter->second] = std::move(s);
      }
    }
  }

  const bool has_filter = !opt.filter_expr.empty();
  const bool has_fields = !ctx.field_specs.empty();
  const bool needs_slice_field_extraction = has_fields && (has_filter || opt.export_csv);
  auto slice_proto_runtime = needs_slice_field_extraction
                                 ? load_proto_runtime(collect_proto_dirs(schema_config, opt.proto_dir))
                                 : ProtoRuntime{};
  vlink::parse::ProtoMessageCache slice_proto_cache(slice_proto_runtime);

  if (needs_slice_field_extraction && (slice_proto_runtime.pool != nullptr || slice_proto_runtime.plugin) &&
      !ctx.quiet_flag) {
    std::cout << "Loaded protobuf schemas" << std::endl;
  }

#ifdef VLINK_ENABLE_EXPRTK

  vlink::parse::ExprContext filter_ctx;

  if (has_filter && has_fields) {
    if (!filter_ctx.compile_single(ctx.field_specs, opt.filter_expr)) {
      std::cerr << "Failed to compile filter expression: " << opt.filter_expr << std::endl;
      return -1;
    }

    if (!ctx.quiet_flag) {
      std::cout << "Filter expression: " << opt.filter_expr << std::endl;
    }
  } else if (has_filter && !has_fields) {
    std::cerr << "Filter expression (--filter) requires -c to specify fields." << std::endl;
    return -1;
  }

#else

  if (has_filter) {
    std::cerr << "Filter expression requires exprtk library." << std::endl;
    return -1;
  }

#endif

  auto slice_count = static_cast<int64_t>(segments.size());

  if VUNLIKELY (slice_count <= 0) {
    std::cerr << "No segments to produce." << std::endl;
    return -1;
  }

  if VUNLIKELY (slice_count > kMaxSliceCount) {
    std::cerr << "Too many segments (" << slice_count << "), maximum is " << kMaxSliceCount << std::endl;
    return -1;
  }

  std::vector<SliceStats> slice_stats_list;
  slice_stats_list.resize(static_cast<size_t>(slice_count));

  for (int64_t i = 0; i < slice_count; ++i) {
    auto& stats = slice_stats_list[static_cast<size_t>(i)];
    stats.index = static_cast<int>(i);
    stats.begin_time_ms = segments[static_cast<size_t>(i)].begin_ms;
    stats.end_time_ms = segments[static_cast<size_t>(i)].end_ms;
    stats.begin_time_us = stats.begin_time_ms * 1000;
    stats.end_time_us = stats.end_time_ms * 1000;
    stats.file_name = segments[static_cast<size_t>(i)].name + output_suffix;
  }

  std::string manifest_name = vlink::parse::sanitize_file_component(opt.manifest_name, "manifest.json");

  if (!opt.no_manifest && manifest_name != opt.manifest_name && !ctx.quiet_flag) {
    std::cerr << "Warning: unsafe manifest file name sanitized to " << manifest_name << std::endl;
  }

  std::vector<std::string> planned_output_files;
  planned_output_files.reserve(static_cast<size_t>(slice_count) * 2 + 1);

  for (const auto& stats : slice_stats_list) {
    planned_output_files.emplace_back(stats.file_name);

    if (opt.export_csv && has_fields) {
      planned_output_files.emplace_back(vlink::parse::csv_name_for_slice_file(stats.file_name));
    }
  }

  if (!opt.no_manifest) {
    planned_output_files.emplace_back(manifest_name);
  }

  if VUNLIKELY (needs_slice_field_extraction &&
                !validate_field_extraction_topics(player->get_info(), slice_selection, opt.actions, url_ser_override,
                                                  slice_proto_cache, "Slice")) {
    return -1;
  }

  if (!ctx.quiet_flag) {
    std::cout << "Processing " << opt.bag_file << " into " << slice_count << " segment(s)..." << std::endl;

    for (const auto& seg : segments) {
      std::cout << "  " << seg.name << ": " << vlink::Helpers::double_to_string(seg.begin_ms / 1000.0, 2) << "s - "
                << vlink::Helpers::double_to_string(seg.end_ms / 1000.0, 2) << "s" << std::endl;
    }
  }

  if (opt.dry_run) {
    std::cout << "\n[DRY RUN] Would produce " << slice_count << " segment(s):" << std::endl;

    int64_t total_duration_ms = 0;

    for (const auto& stats : slice_stats_list) {
      int64_t dur = stats.end_time_ms - stats.begin_time_ms;
      total_duration_ms += dur;
      std::cout << "  " << stats.file_name << "  [" << vlink::Helpers::double_to_string(stats.begin_time_ms / 1000.0, 2)
                << "s - " << vlink::Helpers::double_to_string(stats.end_time_ms / 1000.0, 2) << "s]  ("
                << vlink::Helpers::double_to_string(dur / 1000.0, 2) << "s)" << std::endl;
    }

    std::cout << "\nTotal duration: " << vlink::Helpers::double_to_string(total_duration_ms / 1000.0, 2) << "s"
              << std::endl;
    std::cout << "Output directory: " << opt.out_dir << std::endl;
    std::cout << "URL filter: " << (slice_selection.all ? "all" : "filtered") << std::endl;

    if (opt.export_csv && has_fields) {
      std::cout << "CSV export: yes (fields: " << ctx.field_specs.size() << ")" << std::endl;
    }

    if (has_filter) {
      std::cout << "Content filter: " << opt.filter_expr << std::endl;
    }

    if (opt.sample_step > 1) {
      std::cout << "Sample step: every " << opt.sample_step << "th message" << std::endl;
    }

    player.reset();
    return 0;
  }

  if VUNLIKELY (!ensure_output_dir(filesys_out_dir, opt.out_dir)) {
    return -1;
  }

  if VUNLIKELY (opt.force && !collect_protected_input_paths(opt, protected_input_paths)) {
    return -1;
  }

  if VUNLIKELY (!vlink::parse::preflight_output_files(filesys_out_dir, planned_output_files, opt.force,
                                                      protected_input_paths)) {
    return -1;
  }

  int current_slice_index = 0;
  std::shared_ptr<vlink::BagWriter> current_writer;
  std::ofstream current_csv_file;
  std::filesystem::path current_csv_path;
  bool slice_error = false;

  auto close_current_csv = [&ctx, &current_csv_file, &slice_error, &current_csv_path]() {
    if (!current_csv_file.is_open()) {
      return;
    }

    current_csv_file.close();

    if (!current_csv_file.good()) {
      std::cerr << "Failed to write CSV: " << vlink::parse::path_to_utf8(current_csv_path) << std::endl;
      slice_error = true;
    } else if (!ctx.quiet_flag) {
      std::cout << "CSV: " << vlink::parse::path_to_utf8(current_csv_path) << std::endl;
    }
  };

  auto close_current_writer = [&current_writer]() {
    if (!current_writer) {
      return true;
    }

    current_writer->close();
    const bool success = !current_writer->fail();
    current_writer.reset();
    return success;
  };

  auto create_writer_for_slice = [&ctx, &slice_stats_list, &opt, &player, &current_writer, &schema_list,
                                  &close_current_writer, &has_fields, &current_csv_path, &current_csv_file,
                                  &filesys_out_dir, output_start_timestamp](int idx) -> bool {
    auto& stats = slice_stats_list[static_cast<size_t>(idx)];
    auto filesys_slice_path = filesys_out_dir / stats.file_name;
    auto slice_path = vlink::parse::path_to_utf8(filesys_slice_path);

    if VUNLIKELY (slice_path.empty()) {
      std::cerr << "Invalid slice output path." << std::endl;
      return false;
    }

    if (!opt.force) {
      std::error_code exists_ec;
      bool exists = std::filesystem::exists(filesys_slice_path, exists_ec);

      if (exists_ec && exists_ec != std::errc::no_such_file_or_directory) {
        std::cerr << "Failed to inspect output path: " << slice_path << " (" << exists_ec.message() << ")" << std::endl;
        return false;
      }

      if (exists) {
        std::cerr << "File already exists: " << slice_path << " (use --force to overwrite)" << std::endl;
        return false;
      }
    }

    vlink::BagWriter::Config config;
    config.compress = opt.compress ? vlink::BagWriter::kCompressAuto : vlink::BagWriter::kCompressNone;
    config.compress_level = opt.compress_level;
    config.tag_name = opt.tag_name.empty() ? player->get_info().tag_name : opt.tag_name;
    config.wal_mode = opt.wal_mode;
    config.cache_size = vlink::parse::cache_size_to_bytes(opt.cache_size);
    config.begin_time = stats.begin_time_ms;
    config.start_timestamp = output_start_timestamp + stats.begin_time_ms;
    config.sync_mode = true;
    config.optimize_on_exit = true;

    if (!opt.ignore_compress.empty()) {
      config.ignore_compress_urls.insert(opt.ignore_compress.begin(), opt.ignore_compress.end());
    }

    try {
      current_writer = vlink::BagWriter::create(slice_path, config);
    } catch (vlink::Exception::RuntimeError&) {
      std::cerr << "Failed to create output: " << slice_path << std::endl;
      return false;
    }

    if VUNLIKELY (!current_writer) {
      std::cerr << "Unsupported output suffix for slice file: " << slice_path << std::endl;
      return false;
    }

    for (const auto& schema_data : schema_list) {
      if VUNLIKELY (!current_writer->push_schema(schema_data)) {
        std::cerr << "Failed to write schema: " << schema_data.name << " into " << slice_path << std::endl;
        close_current_writer();
        return false;
      }
    }

    for (const auto& url_meta : player->get_info().url_metas) {
      if (url_meta.url_type != "Method") {
        current_writer->set_url_loss(url_meta.url, url_meta.loss);
      }
    }

    if (opt.export_csv && has_fields) {
      current_csv_path = filesys_out_dir / vlink::parse::csv_name_for_slice_file(stats.file_name);
      current_csv_file.open(current_csv_path);

      if VUNLIKELY (!current_csv_file.is_open()) {
        close_current_writer();
        std::cerr << "Failed to write CSV: " << vlink::parse::path_to_utf8(current_csv_path) << std::endl;
        return false;
      }

      write_csv_cell(current_csv_file, "source_time_s");
      current_csv_file << ",";
      write_csv_cell(current_csv_file, "slice_time_s");
      current_csv_file << ",";
      write_csv_cell(current_csv_file, "url");

      for (const auto& spec : ctx.field_specs) {
        current_csv_file << ",";
        write_csv_cell(current_csv_file, spec);
      }

      current_csv_file << "\n";
    }

    return true;
  };

  if (!create_writer_for_slice(0)) {
    return -1;
  }

  std::unordered_map<std::string, int> sample_counters;
  bool has_last_timestamp = false;
  int64_t last_timestamp = 0;

  player->register_output_callback([&](const vlink::Frame& frame) {
    const int64_t timestamp = frame.timestamp;
    const std::string& url = frame.url;
    const vlink::ActionType action_type = frame.action_type;
    const vlink::Bytes& data = frame.data;

    if VUNLIKELY (ctx.has_quit || !current_writer || slice_error) {
      return;
    }

    const bool selected = (slice_selection.all || slice_selection.urls.count(url) != 0) &&
                          vlink::parse::action_selected(opt.actions, action_type);

    if (ctx.bag_plugin_interface && !selected) {
      return;
    }

    if (has_last_timestamp && timestamp < last_timestamp) {
      std::cerr << "Slice requires non-decreasing input timestamps; output may be partial." << std::endl;
      slice_error = true;
      player->stop();
      return;
    }

    has_last_timestamp = true;
    last_timestamp = timestamp;

    if (!ctx.bag_plugin_interface && (timestamp < playback_begin * 1000 || timestamp >= playback_end * 1000)) {
      return;
    }

    if (!selected) {
      return;
    }

    if (ctx.bag_plugin_interface) {
      if (!plugin_timestamp_supported(timestamp)) {
        std::cerr << "Plugin output timestamp is outside the supported range." << std::endl;
        slice_error = true;
        player->stop();
        return;
      }
    }

    if (opt.sample_step > 1) {
      auto& counter = sample_counters[url];
      ++counter;

      if (counter == opt.sample_step) {
        counter = 0;
      }

      if (counter != 1) {
        return;
      }
    }

    while (current_slice_index < static_cast<int>(slice_stats_list.size()) - 1 &&
           timestamp >= slice_stats_list[static_cast<size_t>(current_slice_index)].end_time_us) {
      close_current_csv();

      if VUNLIKELY (slice_error) {
        player->stop();
        return;
      }

      if VUNLIKELY (!close_current_writer()) {
        slice_error = true;
        player->stop();
        return;
      }

      ++current_slice_index;

      if VUNLIKELY (!create_writer_for_slice(current_slice_index)) {
        slice_error = true;
        player->stop();
        return;
      }
    }

    auto& stats = slice_stats_list[static_cast<size_t>(current_slice_index)];

    if (timestamp < stats.begin_time_us || timestamp >= stats.end_time_us) {
      return;
    }

    auto types = resolve_url_types(url, frame.ser_type, frame.schema_type, url_ser_override);
    bool is_zerocopy = types.resolved == vlink::SchemaType::kZeroCopy;

    std::vector<VariantType> extracted_values;
    google::protobuf::Message* proto_message = nullptr;
    bool fields_applicable = false;

    if (needs_slice_field_extraction) {
      if (types.resolved == vlink::SchemaType::kProtobuf) {
        auto* msg = slice_proto_cache.get(types.ser);

        if (msg != nullptr && data.size() <= static_cast<size_t>(std::numeric_limits<int>::max()) &&
            msg->ParseFromArray(data.data(), static_cast<int>(data.size()))) {
          proto_message = msg;
        }
      }

      if (has_fields) {
        extracted_values.reserve(ctx.field_specs.size());
        bool any_found = false;
        bool all_found = true;
        vlink::zerocopy::MessageParser zerocopy_parser;
        const bool zerocopy_parsed = is_zerocopy && zerocopy_parser.parse(types.ser, data);

        for (size_t fi = 0; fi < ctx.field_specs.size(); ++fi) {
          VariantType val;
          bool found = false;

          if (is_zerocopy) {
            found = zerocopy_parsed && extract_zerocopy_value(zerocopy_parser, ctx.field_specs[fi], val);
          } else if (proto_message != nullptr) {
            found = extract_proto_value(*proto_message, ctx.field_paths[fi], 0, val);
          }

          if (found) {
            any_found = true;
          } else {
            all_found = false;
          }

          extracted_values.emplace_back(found ? std::move(val) : VariantType{std::string{"N/A"}});
        }

        fields_applicable = has_filter ? all_found : any_found;
      }
    }

#ifdef VLINK_ENABLE_EXPRTK

    if (has_filter) {
      if (!filter_ctx.ready() || !fields_applicable) {
        return;
      }

      bool valid = true;

      for (size_t fi = 0; fi < extracted_values.size() && fi < filter_ctx.variable_count(); ++fi) {
        double dv = 0.0;

        if (!variant_to_double(extracted_values[fi], dv)) {
          valid = false;
          break;
        }

        filter_ctx.set_value(fi, dv);
      }

      if (!valid) {
        return;
      }

      double filter_result = filter_ctx.evaluate_single();

      if (filter_result == 0.0) {
        return;
      }
    }

#endif

    int64_t relative_timestamp = timestamp - stats.begin_time_us;

    vlink::Frame push_frame;
    push_frame.timestamp = relative_timestamp;
    push_frame.url = url;
    push_frame.ser_type = types.ser;
    push_frame.schema_type = types.schema_type;
    push_frame.action_type = action_type;
    push_frame.data = vlink::Bytes::shallow_copy(data.data(), data.size());

    if VUNLIKELY (current_writer->push(push_frame) < 0) {
      std::cerr << "Failed to write message into " << stats.file_name << ": " << url << std::endl;
      slice_error = true;
      player->stop();
      return;
    }

    ++stats.message_count;
    stats.urls.emplace(url);

    if (opt.export_csv && fields_applicable && current_csv_file.is_open()) {
      write_csv_cell(current_csv_file, seconds_string_from_us(timestamp));
      current_csv_file << ",";
      write_csv_cell(current_csv_file, seconds_string_from_us(relative_timestamp));
      current_csv_file << ",";
      write_csv_cell(current_csv_file, url);

      for (const auto& value : extracted_values) {
        current_csv_file << ",";
        write_csv_cell(current_csv_file, variant_to_string(value));
      }

      current_csv_file << "\n";
    }

    if (!ctx.quiet_flag && ctx.detail_flag) {
      std::cout << "\033[2K\r";
      std::cout << "[" << current_slice_index << "] ";
      write_seconds_from_us(std::cout, timestamp);
      std::cout << "s " << url << std::endl;
    }
  });

  vlink::BagReader::Config play_config;
  play_config.begin_time = ctx.bag_plugin_interface ? playback_begin : 0;
  play_config.end_time = ctx.bag_plugin_interface ? playback_end : 0;
  play_config.times = 1;
  play_config.rate = 1.0;
  play_config.skip_blank = false;
  play_config.force_delay = 0;
  play_config.auto_pause = false;
  play_config.auto_quit = true;

  if (ctx.bag_plugin_interface && !slice_selection.all) {
    play_config.filter_urls = slice_selection.urls;
  }

  player->play(play_config);

  try {
    player->run();
  } catch (const std::exception& e) {
    std::cerr << "Playback error: " << e.what() << std::endl;
    slice_error = true;
  }

  while (!slice_error && current_slice_index < static_cast<int>(slice_stats_list.size()) - 1) {
    close_current_csv();

    if VUNLIKELY (slice_error) {
      break;
    }

    if VUNLIKELY (!close_current_writer()) {
      slice_error = true;
      break;
    }

    ++current_slice_index;

    if VUNLIKELY (!create_writer_for_slice(current_slice_index)) {
      slice_error = true;
    }
  }

  close_current_csv();

  if VUNLIKELY (!close_current_writer()) {
    slice_error = true;
  }

  ctx.has_quit = true;

  vlink::BagReader::Info source_info;

  if (player) {
    source_info = player->get_info();
  }

  player.reset();

  if (!opt.no_manifest && !slice_error) {
    nlohmann::ordered_json manifest;
    manifest["schema_version"] = 1;
    manifest["time_base"] = "relative_ms";
    manifest["source"] = opt.bag_file;
    manifest["total_duration_ms"] = effective_end - effective_begin;
    manifest["start_timestamp"] = source_info.start_timestamp;
    manifest["date_time"] = source_info.date_time;
    manifest["timezone"] = source_info.timezone;
    manifest["process_name"] = source_info.process_name;
    manifest["tag_name"] = source_info.tag_name;
    manifest["storage_type"] = source_info.storage_type;
    manifest["window_ms"] = window_ms;
    manifest["slice_count"] = slice_count;

    nlohmann::ordered_json slices_json = nlohmann::ordered_json::array();

    for (const auto& stats : slice_stats_list) {
      nlohmann::ordered_json slice_json;
      slice_json["index"] = stats.index;
      slice_json["file"] = stats.file_name;
      slice_json["begin_time_ms"] = stats.begin_time_ms;
      slice_json["end_time_ms"] = stats.end_time_ms;
      slice_json["message_count"] = stats.message_count;

      std::vector<std::string> sorted_urls(stats.urls.begin(), stats.urls.end());
      std::sort(sorted_urls.begin(), sorted_urls.end());
      nlohmann::ordered_json urls_json = nlohmann::ordered_json::array();

      for (const auto& u : sorted_urls) {
        urls_json.emplace_back(u);
      }

      slice_json["urls"] = urls_json;

      auto slice_file_path = filesys_out_dir / stats.file_name;

      {
        std::error_code size_ec;
        auto fsize = std::filesystem::file_size(slice_file_path, size_ec);
        slice_json["file_size"] = size_ec ? 0 : static_cast<int64_t>(fsize);
      }

      slices_json.emplace_back(slice_json);
    }

    manifest["slices"] = slices_json;

    auto filesys_manifest_path = filesys_out_dir / manifest_name;
    auto manifest_path = vlink::parse::path_to_utf8(filesys_manifest_path);
    std::ofstream manifest_file(filesys_manifest_path);

    if (manifest_file.is_open()) {
      manifest_file << manifest.dump(2);
      manifest_file.close();

      if (!manifest_file.good()) {
        std::cerr << "Failed to write manifest: " << manifest_path << std::endl;
        slice_error = true;
      } else if (!ctx.quiet_flag) {
        std::cout << "Manifest saved to " << manifest_path << std::endl;
      }
    } else {
      std::cerr << "Failed to write manifest: " << manifest_path << std::endl;
      slice_error = true;
    }
  }

  if (!ctx.quiet_flag) {
    int64_t total_messages = 0;

    for (const auto& stats : slice_stats_list) {
      total_messages += stats.message_count;
    }

    std::cout << "\033[2K\r" << (slice_error ? "Break. " : "Done. ") << slice_count << " slice(s), " << total_messages
              << " message(s) total." << std::endl;
  }

  return slice_error ? -1 : 0;
}

#endif
