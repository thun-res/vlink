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

#include "./mapping.h"

#include <vlink/base/logger.h>

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <limits>

#include "./webviz_app_utils.h"
#include "./webviz_loader_utils.h"

namespace vlink {
namespace webviz {

FieldExpression::FieldExpression(const std::string& expression) {
#ifdef VLINK_ENABLE_EXPRTK
  std::string compiled;
  std::vector<std::string> names;

  for (size_t i = 0; i < expression.size();) {
    const auto ch = static_cast<unsigned char>(expression[i]);

    if (std::isdigit(ch) ||
        (ch == '.' && i + 1 < expression.size() && std::isdigit(static_cast<unsigned char>(expression[i + 1])))) {
      char* end = nullptr;
      std::strtod(expression.c_str() + i, &end);
      const auto length = static_cast<size_t>(end - expression.c_str()) - i;
      compiled.append(expression, i, length);
      i += length;
      continue;
    }

    if (!std::isalpha(ch) && ch != '_') {
      compiled += expression[i++];
      continue;
    }

    const auto start = i++;

    while (i < expression.size()) {
      const auto next = static_cast<unsigned char>(expression[i]);

      if (!std::isalnum(next) && next != '_' && next != '.' && next != '[' && next != ']') {
        break;
      }

      ++i;
    }

    const auto name = expression.substr(start, i - start);
    if (name == "e") {
      compiled += "exp(1)";
      continue;
    }
    const auto next = expression.find_first_not_of(" \t", i);
    static const std::vector<std::string> kKeywords = {"pi",  "epsilon", "inf",  "true", "false", "and", "or",
                                                       "not", "xor",     "nand", "nor",  "xnor",  "mod"};

    if ((next != std::string::npos && expression[next] == '(') ||
        std::find(kKeywords.begin(), kKeywords.end(), name) != kKeywords.end()) {
      compiled += name;
      continue;
    }

    auto iter = std::find(names.begin(), names.end(), name);
    size_t index = static_cast<size_t>(iter - names.begin());

    if (iter == names.end()) {
      FieldPath path;

      if (!parse_field_path(name, path)) {
        MLOG_E("Invalid expression field: {}", name);
        return;
      }

      const bool size = path.back().name == "_size" && !path.back().indexed;
      if (size) {
        path.pop_back();
      }
      paths_.push_back({std::move(path), size});
      names.push_back(name);
    }

    compiled += "vlink_field_" + std::to_string(index);
  }

  values_.resize(paths_.size());
  symbols_.add_constants();

  for (size_t i = 0; i < values_.size(); ++i) {
    if (!symbols_.add_variable("vlink_field_" + std::to_string(i), values_[i])) {
      MLOG_E("Cannot bind expression field: {}", names[i]);
      return;
    }
  }

  expression_.register_symbol_table(symbols_);
  valid_ = expression_.compile(compiled);

  if (!valid_) {
    MLOG_E("Invalid mapping expression: {}", expression);
  }
#else
  (void)expression;
#endif
}

double FieldExpression::evaluate(const FieldReader& fields) const {
#ifdef VLINK_ENABLE_EXPRTK
  if VUNLIKELY (!valid_) {
    return 0.0;
  }

  const std::lock_guard lock(mutex_);

  for (size_t i = 0; i < paths_.size(); ++i) {
    const auto& path = paths_[i].path;
    FieldValue value;
    if (paths_[i].size) {
      value = uint64_t{fields.source(path).size()};
    } else if (path.size() == 1 && path[0].name == "_index") {
      value = uint64_t{fields.index()};
    } else if (path.size() == 1 && path[0].name == "_value") {
      value = fields.source().value(true);
    } else {
      value = fields.source(path).value(true);
    }

    if (const auto* integer = std::get_if<uint64_t>(&value)) {
      if VUNLIKELY (*integer > 9007199254740992ULL) {
        VLOG_W_EVERY_MS(1000, "Expression input exceeds exact double integer precision");
      }
    } else if (const auto* integer = std::get_if<int64_t>(&value)) {
      if VUNLIKELY (*integer > 9007199254740992LL || *integer < -9007199254740992LL) {
        VLOG_W_EVERY_MS(1000, "Expression input exceeds exact double integer precision");
      }
    }

    values_[i] = field_number(value);
  }

  return expression_.value();
#else
  (void)fields;
  return 0.0;
#endif
}

static int64_t time_scale(std::string_view unit) {
  if (unit == "s") {
    return 1000000000;
  }
  if (unit == "ms") {
    return 1000000;
  }
  return unit == "us" ? 1000 : 1;
}

MappingSet::MappingSet(const std::vector<std::string>& files, std::string_view target_key) {
  for (const auto& file : files) {
    std::ifstream stream(file);

    if (!stream) {
      valid_ = false;
      MLOG_E("Cannot open mapping: {}", file);
      continue;
    }

    auto root = nlohmann::json::parse(stream, nullptr, false);

    if (root.is_object()) {
      root = nlohmann::json::array({std::move(root)});
    }

    if (!root.is_array()) {
      valid_ = false;
      MLOG_E("Mapping must contain an object or array: {}", file);
      continue;
    }

    for (const auto& entry : root) {
      try {
        MessageMapping mapping;
        mapping.ser = entry.at("ser").get<std::string>();
        mapping.target = entry.value(std::string(target_key), std::string{});
        mapping.converter = entry.value("converter", std::string{});
        mapping.entity_path = entry.value("entity_path", std::string{});
        mapping.is_static = entry.value("static", false);
        mapping.encoding = entry.value("encoding", target_key == "schema" ? "flatbuffer" : "protobuf");
        mapping.schema_encoding =
            entry.value("schema_encoding",
                        target_key == "schema" && mapping.converter != "passthrough" ? "flatbuffer" : mapping.encoding);
        for (auto* encoding : {&mapping.encoding, &mapping.schema_encoding}) {
          if (*encoding == "flatbuffers" || *encoding == "fbs" || *encoding == "bfbs") {
            *encoding = "flatbuffer";
          }
        }

        if (mapping.converter == "passthrough" && mapping.target.empty()) {
          mapping.target = mapping.ser;
        }

        std::string unit = entry.value("timestamp_unit", "us");

        if (mapping.ser.empty() || (mapping.target.empty() && mapping.converter.empty()) ||
            !parse_field_path(entry.value("timestamp_field", ""), mapping.timestamp) ||
            !is_valid_timestamp_unit(unit)) {
          MLOG_E("Invalid mapping identity or timestamp: {}", file);
          valid_ = false;
          continue;
        }

        mapping.timestamp_scale = time_scale(unit);

        if (!parse_url_selector(entry, file, "mapping", mapping.urls)) {
          valid_ = false;
          continue;
        }

        bool valid = true;

        if (entry.contains("field_mappings")) {
          if (!entry["field_mappings"].is_array()) {
            valid_ = false;
            MLOG_E("field_mappings must be an array: {}", file);
            continue;
          }

          for (const auto& field : entry["field_mappings"]) {
            MappedField mapped;
            mapped.target = field.at("target").get<std::string>();
            mapped.has_default = field.contains("default_value");
            if (field.contains("time_unit")) {
              const auto time_unit = field.at("time_unit").get<std::string>();
              if (!is_valid_timestamp_unit(time_unit)) {
                valid = false;
                break;
              }
              mapped.time_scale = time_scale(time_unit);
            }

            if (mapped.has_default) {
              mapped.default_value = field["default_value"];
            }

            const auto expression = field.value("expression", "");

            if (!expression.empty()) {
              mapped.expression = std::make_unique<FieldExpression>(expression);
            }

            valid = !mapped.target.empty() && parse_field_path(field.value("source", ""), mapped.source) &&
                    (!mapped.expression || mapped.expression->valid()) &&
                    (!mapped.source.empty() || mapped.expression || mapped.has_default) &&
                    std::none_of(mapping.fields.begin(), mapping.fields.end(),
                                 [&](const MappedField& existing) { return existing.target == mapped.target; });

            if (!valid) {
              MLOG_E("Invalid mapping field: {}", file);
              break;
            }

            mapping.fields.push_back(std::move(mapped));
          }
        }

        if (mapping.converter == "send_time" && mapping.timestamp.empty()) {
          valid = false;
        }

        if (mapping.converter == "passthrough" &&
            (mapping.encoding != mapping.schema_encoding || !mapping.fields.empty() ||
             (mapping.encoding != "protobuf" && mapping.encoding != "flatbuffer"))) {
          valid = false;
        }

        if (mapping.encoding != "protobuf" && mapping.encoding != "flatbuffer" && mapping.encoding != "zerocopy" &&
            mapping.encoding != "json") {
          valid = false;
        }

        if (!valid) {
          valid_ = false;
          MLOG_E("Invalid mapping contract: {}", file);
        }

        if (valid) {
          mappings_[mapping.ser].push_back(std::move(mapping));
        }
      } catch (const nlohmann::json::exception& error) {
        valid_ = false;
        MLOG_E("Invalid mapping {}: {}", file, error.what());
      }
    }
  }
}

void MappingSet::validate(bool (*validator)(const MessageMapping&)) {
  for (const auto& entry : mappings_) {
    for (const auto& mapping : entry.second) {
      if (!validator(mapping)) {
        MLOG_E("Invalid mapping target: ser={} target={}", mapping.ser, mapping.target);
        valid_ = false;
      }
    }
  }
}

std::vector<const MessageMapping*> MappingSet::select(std::string_view url, const std::string& ser,
                                                      bool* ambiguous) const {
  if (ambiguous) {
    *ambiguous = false;
  }
  const auto found = mappings_.find(ser);
  if (found == mappings_.end()) {
    return {};
  }
  struct Selection final {
    const MessageMapping* mapping;
    int score;
    bool tied;
  };
  std::vector<Selection> selected;
  for (const auto& candidate : found->second) {
    const auto score = score_url_selector(url, candidate.urls);
    if (score < 0) {
      continue;
    }
    auto slot = std::find_if(selected.begin(), selected.end(), [&](const Selection& item) {
      return item.mapping->target == candidate.target && item.mapping->converter == candidate.converter &&
             item.mapping->entity_path == candidate.entity_path;
    });
    if (slot == selected.end()) {
      selected.push_back({&candidate, score, false});
    } else if (score > slot->score) {
      *slot = {&candidate, score, false};
    } else if (score == slot->score) {
      slot->tied = true;
    }
  }
  std::vector<const MessageMapping*> result;
  for (const auto& item : selected) {
    if (item.tied) {
      if (ambiguous) {
        *ambiguous = true;
      }
      MLOG_W("Ambiguous mapping: url={} ser={} target={}", url, ser, item.mapping->target);
      return {};
    }
    result.push_back(item.mapping);
  }
  return result;
}

bool MappingSet::has_converter(std::string_view converter) const {
  for (const auto& entry : mappings_) {
    for (const auto& mapping : entry.second) {
      if (mapping.converter == converter) {
        return true;
      }
    }
  }

  return false;
}

std::string native_ser(std::string_view converter) {
  if (converter == "camera_frame") {
    return "CameraFrame";
  }
  if (converter == "point_cloud") {
    return "PointCloud";
  }
  if (converter == "occupancy_grid") {
    return "OccupancyGrid";
  }
  if (converter == "object_array") {
    return "ObjectArray";
  }
  if (converter == "audio_frame") {
    return "AudioFrame";
  }
  if (converter == "tensor") {
    return "Tensor";
  }
  if (converter == "raw_data") {
    return "RawData";
  }
  return {};
}

FieldReader::FieldReader(const MessageView& source, const MessageMapping* mapping)
    : source_(source), root_(source), mapping_(mapping) {}

MessageView FieldReader::source(const FieldPath& path) const {
  if (path.empty() || path.front().name != "_root") {
    return source_.find(path);
  }
  auto result = root_;
  if (path.front().indexed) {
    result = result.at(path.front().index);
  }
  for (size_t i = 1; i < path.size(); ++i) {
    if (!path[i].name.empty()) {
      result = result.member(path[i].name);
    }
    if (path[i].indexed) {
      result = result.at(path[i].index);
    }
  }
  return result;
}

static bool target_prefix(std::string_view pattern, std::string_view target, size_t& offset) {
  offset = 0;
  for (size_t i = 0; i < target.size();) {
    if (pattern.substr(offset, 2) == "[]" && target[i] == '[') {
      const auto end = target.find(']', i + 1);
      if (end == std::string_view::npos) {
        return false;
      }
      i = end + 1;
      offset += 2;
    } else if (offset < pattern.size() && pattern[offset] == target[i]) {
      ++offset;
      ++i;
    } else {
      return false;
    }
  }
  return true;
}

const MappedField* FieldReader::field(std::string_view target) const {
  const MappedField* result = nullptr;
  size_t specificity = 0;
  if (mapping_) {
    for (const auto& field : mapping_->fields) {
      if (field.target == target) {
        return &field;
      }
      size_t offset = 0;
      if (target_prefix(field.target, target, offset) && offset == field.target.size() &&
          (!result || field.target.size() > specificity)) {
        result = &field;
        specificity = field.target.size();
      }
    }
  }
  return result;
}

FieldValue FieldReader::value(std::string_view target) const {
  const auto* mapped = field(target);

  if (mapped && mapped->expression) {
    return mapped->expression->evaluate(*this);
  }
  return view(target).value();
}

bool FieldReader::has_descendant(std::string_view target) const {
  if (!mapping_) {
    return false;
  }
  if (target.empty()) {
    return !mapping_->fields.empty();
  }
  for (const auto& field : mapping_->fields) {
    size_t offset = 0;
    if (target_prefix(field.target, target, offset) && offset < field.target.size() &&
        (field.target[offset] == '.' || field.target[offset] == '[')) {
      return true;
    }
  }
  return false;
}

std::vector<size_t> FieldReader::indices(std::string_view target) const {
  std::vector<size_t> result;
  if (mapping_) {
    for (const auto& field : mapping_->fields) {
      size_t offset = 0;
      if (!target_prefix(field.target, target, offset) || offset >= field.target.size() ||
          field.target[offset] != '[') {
        continue;
      }
      const auto* begin = field.target.data() + offset + 1;
      const auto* end = field.target.data() + field.target.size();
      size_t index = 0;
      const auto parsed = std::from_chars(begin, end, index);
      if (parsed.ec == std::errc() && parsed.ptr != end && *parsed.ptr == ']') {
        result.push_back(index);
      }
    }
  }
  std::sort(result.begin(), result.end());
  result.erase(std::unique(result.begin(), result.end()), result.end());
  return result;
}

double FieldReader::number(std::string_view target, double fallback) const {
  return field_number(value(target), fallback);
}

uint64_t FieldReader::integer(std::string_view target, uint64_t fallback) const {
  return field_unsigned(value(target), fallback);
}

std::string FieldReader::text(std::string_view target, std::string_view fallback) const {
  auto result = value(target);
  if (auto* text = std::get_if<std::string>(&result)) {
    return std::move(*text);
  }
  return std::holds_alternative<std::monostate>(result) ? std::string(fallback) : field_text(result);
}

MessageView FieldReader::view(std::string_view target, std::string_view default_source) const {
  if (const auto* mapped = field(target)) {
    if (mapped->expression) {
      return {};
    }
    if (!mapped->source.empty()) {
      auto source = this->source(mapped->source);

      if (source.valid()) {
        return source;
      }
    }

    return mapped->has_default ? MessageView(mapped->default_value) : MessageView{};
  }

  if (!default_source.empty()) {
    return source_.find(default_source);
  }

  return mapping_ && !mapping_->fields.empty() ? MessageView{} : source_.find(target);
}

Bytes FieldReader::bytes(std::string_view target) const { return view(target).bytes(); }

FieldReader FieldReader::child(const MessageView& source) const {
  auto result = *this;
  result.source_ = source;
  return result;
}

FieldReader FieldReader::child(const MessageView& source, size_t index) const {
  auto result = child(source);
  result.index_ = index;
  return result;
}

int64_t FieldReader::timestamp() const {
  if (!mapping_ || mapping_->timestamp.empty()) {
    return -1;
  }

  const auto view = source(mapping_->timestamp);
  const auto value = view.value(view.is_flatbuffer());

  if (!std::holds_alternative<int64_t>(value) && !std::holds_alternative<uint64_t>(value) &&
      !std::holds_alternative<bool>(value) && !std::holds_alternative<double>(value)) {
    return -1;
  }

  if (std::holds_alternative<double>(value)) {
    const auto nanos = field_number(value) * static_cast<double>(mapping_->timestamp_scale);

    if (!std::isfinite(field_number(value)) || nanos < 0) {
      return -1;
    }

    return nanos >= 0x1p63 ? std::numeric_limits<int64_t>::max() : static_cast<int64_t>(nanos);
  }

  if (const auto* number = std::get_if<int64_t>(&value)) {
    if (*number < 0) {
      return -1;
    }
  }

  const auto integer = field_unsigned(value);

  if (integer > static_cast<uint64_t>(std::numeric_limits<int64_t>::max()) / mapping_->timestamp_scale) {
    return std::numeric_limits<int64_t>::max();
  }

  return static_cast<int64_t>(integer * mapping_->timestamp_scale);
}

}  // namespace webviz
}  // namespace vlink
