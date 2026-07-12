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

#include <vlink/base/elapsed_timer.h>
#include <vlink/base/helpers.h>
#include <vlink/base/utils.h>
#include <vlink/extension/discovery_viewer.h>
#include <vlink/extension/schema_plugin_manager.h>
#include <vlink/extension/terminal_stream.h>
#include <vlink/version.h>
#include <vlink/vlink.h>
#include <vlink/zerocopy/audio_frame.h>
#include <vlink/zerocopy/camera_frame.h>
#include <vlink/zerocopy/message_parser.h>
#include <vlink/zerocopy/occupancy_grid.h>
#include <vlink/zerocopy/point_cloud.h>
#include <vlink/zerocopy/tensor.h>

#if __has_include(<google/protobuf/compiler/importer.h>) && __has_include(<google/protobuf/text_format.h>)

#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wcpp"
#endif

#include <google/protobuf/compiler/importer.h>
#include <google/protobuf/descriptor.h>
#include <google/protobuf/dynamic_message.h>
#include <google/protobuf/io/zero_copy_stream_impl.h>
#include <google/protobuf/message.h>
#include <google/protobuf/reflection.h>
#include <google/protobuf/stubs/common.h>
#include <google/protobuf/text_format.h>

#if __has_include(<google/protobuf/util/json_util.h>)
#include <google/protobuf/util/json_util.h>
#define VLINK_HAS_PROTOBUF_JSON_UTIL
#endif
#if GOOGLE_PROTOBUF_VERSION >= 3004000
#define VLINK_HAS_PROTOBUF_COMPILER
#endif

#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic pop
#endif

#endif

#include <argparse/argparse.hpp>
#include <nlohmann/json.hpp>
//
#include <algorithm>
#include <cstdio>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#if __has_include(<unistd.h>)
#include <unistd.h>
#endif

#ifdef _WIN32
#include <Windows.h>
#undef min
#undef max
#undef GetMessage
[[maybe_unused]] static constexpr int kFlushMinSleep{0};
[[maybe_unused]] static constexpr int kFlushMinLine{5};
#elif defined(__APPLE__)
[[maybe_unused]] static constexpr int kFlushMinSleep{10};
[[maybe_unused]] static constexpr int kFlushMinLine{1};
#else
[[maybe_unused]] static constexpr int kFlushMinSleep{10};
[[maybe_unused]] static constexpr int kFlushMinLine{5};
#endif

using RawPub = vlink::Publisher<vlink::Bytes>;
using RawSub = vlink::Subscriber<vlink::Bytes>;
using RawGetter = vlink::Getter<vlink::Bytes>;

[[maybe_unused]] static constexpr size_t kMaxTaskSize{20000U};
[[maybe_unused]] static constexpr int kCounterCache{2};
[[maybe_unused]] static constexpr int kCounterWeight{2};
[[maybe_unused]] static constexpr int kCollectInterval{1000};
[[maybe_unused]] static constexpr int kTerminalInterval{50};
[[maybe_unused]] static constexpr int kMaxElapsedTime{200};

[[maybe_unused]] static std::atomic_bool has_quit{false};
[[maybe_unused]] std::atomic_bool has_intra_bind{false};
[[maybe_unused]] std::atomic_bool is_paused{false};
[[maybe_unused]] std::atomic_bool is_changed{false};
[[maybe_unused]] std::atomic_bool has_printed{false};
[[maybe_unused]] std::atomic_bool force_update{false};
[[maybe_unused]] std::atomic_bool is_proto_type{false};
[[maybe_unused]] std::atomic_bool is_out_of_range{false};
[[maybe_unused]] std::atomic_bool black_mode{false};
[[maybe_unused]] std::atomic<size_t> max_str_count{0};
[[maybe_unused]] std::atomic_bool ignore_array{false};
[[maybe_unused]] std::atomic_bool ignore_string{false};
[[maybe_unused]] std::atomic_bool ignore_default{false};
[[maybe_unused]] std::atomic_bool use_long_repeated{false};
[[maybe_unused]] std::atomic_bool print_time_string{false};
[[maybe_unused]] std::atomic_bool print_hex_string{false};
[[maybe_unused]] std::atomic_bool print_enum_string{false};
[[maybe_unused]] std::atomic<int> current_page{0};
[[maybe_unused]] std::atomic<int> total_page{0};
[[maybe_unused]] std::atomic<int> max_rows{0};
[[maybe_unused]] std::atomic<int> max_columns{0};

[[maybe_unused]] std::vector<std::string> filter_list;
// NOLINTNEXTLINE(bugprone-throwing-static-initialization)
[[maybe_unused]] std::pair<int, int> terminal_size{0, 0};

[[maybe_unused]] static std::pair<int, int> get_terminal_size() {
  auto size = vlink::Utils::get_terminal_size();

  if (max_columns > 0) {
    size.first = max_columns;
  }

  if (max_rows > 0) {
    size.second = max_rows;
  }

  if (size.first < 0) {
    size.first = 1000;
  }

  if (size.second < 0) {
    size.second = 25;
  }

  return size;
}

[[maybe_unused]] static bool is_text_ser_type(std::string_view ser_type) {
  std::string lower_ser{ser_type};
  std::transform(lower_ser.begin(), lower_ser.end(), lower_ser.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return lower_ser == "string" || lower_ser == "std::string" || lower_ser == "json" ||
         lower_ser == "application/json" || lower_ser == "text/json" || lower_ser == "text";
}

[[maybe_unused]] static bool load_text_for_file(const std::string& filename, std::string& content) {
  std::ifstream input(filename, std::ios::binary);

  if VUNLIKELY (!input.is_open()) {
    return false;
  }

  content.assign(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
  return !input.bad();
}

[[maybe_unused]] static std::filesystem::path utf8_to_path(const std::string& utf8) noexcept {
  try {
#ifdef _WIN32
    return std::filesystem::path(vlink::Helpers::string_to_wstring(utf8));
#else
    return std::filesystem::path(utf8);
#endif
  } catch (const std::filesystem::filesystem_error&) {
    return {};
  } catch (const std::exception&) {
    return {};
  }
}

[[maybe_unused]] static std::string get_home_config_path(const std::string& filename) {
  std::string home = vlink::Utils::get_env("HOME");

  if (home.empty()) {
    home = vlink::Utils::get_env("USERPROFILE");
  }

  if VUNLIKELY (home.empty()) {
    return {};
  }

  auto home_path = utf8_to_path(home);

  if VUNLIKELY (home_path.empty()) {
    return {};
  }

  std::string result;

  try {
    auto joined = home_path / filename;
    result = vlink::Helpers::path_to_string(joined);
  } catch (const std::filesystem::filesystem_error&) {
    return {};
  } catch (const std::exception&) {
    return {};
  }

#ifdef _WIN32
  std::replace(result.begin(), result.end(), '\\', '/');
#endif
  return result;
}

[[maybe_unused]] static std::string read_home_config(const std::string& filename) {
  auto config_path_utf8 = get_home_config_path(filename);

  if VUNLIKELY (config_path_utf8.empty()) {
    return {};
  }

  auto fs_path = utf8_to_path(config_path_utf8);

  if VUNLIKELY (fs_path.empty()) {
    return {};
  }

  std::error_code ec;

  if (!std::filesystem::is_regular_file(fs_path, ec) || ec) {
    return {};
  }

  std::ifstream input(fs_path, std::ios::binary);

  if VUNLIKELY (!input.is_open()) {
    return {};
  }

  std::string content;
  content.assign(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());

  auto back_pos = content.find_last_not_of(" \t\r\n");

  if (back_pos != std::string::npos) {
    content.erase(back_pos + 1);
  } else {
    content.clear();
  }

  auto front_pos = content.find_first_not_of(" \t\r\n");

  if (front_pos != std::string::npos) {
    content.erase(0, front_pos);
  }

  return content;
}

[[maybe_unused]] static bool write_home_config(const std::string& filename, const std::string& value) {
  auto config_path_utf8 = get_home_config_path(filename);

  if VUNLIKELY (config_path_utf8.empty()) {
    return false;
  }

  auto fs_path = utf8_to_path(config_path_utf8);

  if VUNLIKELY (fs_path.empty()) {
    return false;
  }

  std::ofstream output(fs_path, std::ios::binary | std::ios::trunc);

  if VUNLIKELY (!output.is_open()) {
    return false;
  }

  output << value;
  output.flush();

  return output.good();
}

[[maybe_unused]] static bool format_json_text(const std::string& content, std::string& out) {
  auto json = nlohmann::ordered_json::parse(content, nullptr, false);

  if (json.is_discarded()) {
    return false;
  }

  out = json.dump(2);

  return true;
}

#ifdef VLINK_HAS_PROTOBUF_COMPILER

class CustomFieldValuePrinter final : public google::protobuf::TextFormat::FastFieldValuePrinter {
  void PrintBytes(const std::string& val, google::protobuf::TextFormat::BaseTextGenerator* generator) const override {
    (void)val;
    generator->PrintString("{...}");
  }

  void PrintString(const std::string& val, google::protobuf::TextFormat::BaseTextGenerator* generator) const override {
    FastFieldValuePrinter::PrintString(val, generator);
  }

  void PrintFieldName(const google::protobuf::Message& message, int field_index, int field_count,
                      const google::protobuf::Reflection* reflection, const google::protobuf::FieldDescriptor* field,
                      google::protobuf::TextFormat::BaseTextGenerator* generator) const override {
    FastFieldValuePrinter::PrintFieldName(message, field_index, field_count, reflection, field, generator);

    if (field->is_repeated() && field_index >= 0) {
      generator->PrintString("[" + std::to_string(field_index) + "]");
    }
  }

  void PrintFieldName(const google::protobuf::Message& message, const google::protobuf::Reflection* reflection,
                      const google::protobuf::FieldDescriptor* field,
                      google::protobuf::TextFormat::BaseTextGenerator* generator) const override {
    current_field_ = const_cast<google::protobuf::FieldDescriptor*>(field);

    FastFieldValuePrinter::PrintFieldName(message, reflection, field, generator);
  }

  void PrintMessageStart(const google::protobuf::Message& message, int field_index, int field_count,
                         bool single_line_mode,
                         google::protobuf::TextFormat::BaseTextGenerator* generator) const override {
    FastFieldValuePrinter::PrintMessageStart(message, field_index, field_count, single_line_mode, generator);
  }
#if GOOGLE_PROTOBUF_VERSION >= 3012000
  bool PrintMessageContent(const google::protobuf::Message& message, int field_index, int field_count,
                           bool single_line_mode,
                           google::protobuf::TextFormat::BaseTextGenerator* generator) const override {
    return FastFieldValuePrinter::PrintMessageContent(message, field_index, field_count, single_line_mode, generator);
  }
#endif

  void PrintMessageEnd(const google::protobuf::Message& message, int field_index, int field_count,
                       bool single_line_mode,
                       google::protobuf::TextFormat::BaseTextGenerator* generator) const override {
    FastFieldValuePrinter::PrintMessageEnd(message, field_index, field_count, single_line_mode, generator);
  }

  void PrintEnum(int32_t val, const std::string& name,
                 google::protobuf::TextFormat::BaseTextGenerator* generator) const override {
    if (print_enum_string) {
      FastFieldValuePrinter::PrintEnum(val, name, generator);
    } else {
      generator->PrintString(std::to_string(val));
    }
  }

  void PrintInt32(int32_t val, google::protobuf::TextFormat::BaseTextGenerator* generator) const override {
    if (print_time_string && current_field_) {
      if (print_hex_string) {
        generator->PrintString(vlink::Helpers::format_hex_number(static_cast<int64_t>(val)));
      } else {
        FastFieldValuePrinter::PrintInt32(val, generator);
      }
    } else {
      if (print_hex_string) {
        generator->PrintString(vlink::Helpers::format_hex_number(static_cast<int64_t>(val)));
      } else {
        FastFieldValuePrinter::PrintInt32(val, generator);
      }
    }
  }

  void PrintUInt32(uint32_t val, google::protobuf::TextFormat::BaseTextGenerator* generator) const override {
    if (print_hex_string) {
      generator->PrintString(vlink::Helpers::format_hex_number(static_cast<int64_t>(val)));
    } else {
      FastFieldValuePrinter::PrintUInt32(val, generator);
    }
  }

  void PrintInt64(int64_t val, google::protobuf::TextFormat::BaseTextGenerator* generator) const override {
    if (print_time_string && current_field_) {
      if (current_field_->name().find("time") != std::string::npos) {
        generator->PrintString(vlink::Helpers::format_date(val));
      } else {
        if (print_hex_string) {
          generator->PrintString(vlink::Helpers::format_hex_number(val));
        } else {
          FastFieldValuePrinter::PrintInt64(val, generator);
        }
      }
    } else {
      if (print_hex_string) {
        generator->PrintString(vlink::Helpers::format_hex_number(val));
      } else {
        FastFieldValuePrinter::PrintInt64(val, generator);
      }
    }
  }

  void PrintUInt64(uint64_t val, google::protobuf::TextFormat::BaseTextGenerator* generator) const override {
    if (print_time_string && current_field_) {
      if (current_field_->name().find("time") != std::string::npos) {
        generator->PrintString(vlink::Helpers::format_date(val));
      } else {
        if (print_hex_string) {
          generator->PrintString(vlink::Helpers::format_hex_number(val));
        } else {
          FastFieldValuePrinter::PrintUInt64(val, generator);
        }
      }
    } else {
      if (print_hex_string) {
        generator->PrintString(vlink::Helpers::format_hex_number(val));
      } else {
        FastFieldValuePrinter::PrintUInt64(val, generator);
      }
    }
  }

  void PrintFloat(float val, google::protobuf::TextFormat::BaseTextGenerator* generator) const override {
    FastFieldValuePrinter::PrintFloat(val, generator);
  }

  void PrintDouble(double val, google::protobuf::TextFormat::BaseTextGenerator* generator) const override {
    FastFieldValuePrinter::PrintDouble(val, generator);
  }

  void PrintBool(bool val, google::protobuf::TextFormat::BaseTextGenerator* generator) const override {
    FastFieldValuePrinter::PrintBool(val, generator);
  }

 private:
  mutable google::protobuf::FieldDescriptor* current_field_{nullptr};
};

#if GOOGLE_PROTOBUF_VERSION >= 3006000

[[maybe_unused]] static bool is_no_presence_default_scalar(const google::protobuf::Message& message,
                                                           const google::protobuf::FieldDescriptor* field) {
  if VUNLIKELY (field == nullptr || field->is_repeated() ||
                field->cpp_type() == google::protobuf::FieldDescriptor::CPPTYPE_MESSAGE) {
    return false;
  }

#if GOOGLE_PROTOBUF_VERSION >= 3012000
  if (field->has_presence()) {
    return false;
  }
#else
  if (field->is_extension() || field->containing_oneof() != nullptr ||
      field->file()->syntax() != google::protobuf::FileDescriptor::SYNTAX_PROTO3) {
    return false;
  }
#endif

  if (field->type() == google::protobuf::FieldDescriptor::TYPE_BYTES ||
      (ignore_string && field->type() == google::protobuf::FieldDescriptor::TYPE_STRING)) {
    return false;
  }

  const auto* reflection = message.GetReflection();

  if (reflection->HasField(message, field)) {
    return false;
  }

  switch (field->cpp_type()) {
    case google::protobuf::FieldDescriptor::CPPTYPE_INT32:
      return reflection->GetInt32(message, field) == field->default_value_int32();
    case google::protobuf::FieldDescriptor::CPPTYPE_INT64:
      return reflection->GetInt64(message, field) == field->default_value_int64();
    case google::protobuf::FieldDescriptor::CPPTYPE_UINT32:
      return reflection->GetUInt32(message, field) == field->default_value_uint32();
    case google::protobuf::FieldDescriptor::CPPTYPE_UINT64:
      return reflection->GetUInt64(message, field) == field->default_value_uint64();
    case google::protobuf::FieldDescriptor::CPPTYPE_DOUBLE:
      return reflection->GetDouble(message, field) == field->default_value_double();
    case google::protobuf::FieldDescriptor::CPPTYPE_FLOAT:
      return reflection->GetFloat(message, field) == field->default_value_float();
    case google::protobuf::FieldDescriptor::CPPTYPE_BOOL:
      return reflection->GetBool(message, field) == field->default_value_bool();
    case google::protobuf::FieldDescriptor::CPPTYPE_STRING:
#if GOOGLE_PROTOBUF_VERSION >= 6030000
      return reflection->GetString(message, field) == std::string(field->default_value_string());
#else
      return reflection->GetString(message, field) == field->default_value_string();
#endif
    case google::protobuf::FieldDescriptor::CPPTYPE_ENUM:
      return reflection->GetEnum(message, field) == field->default_value_enum();
    case google::protobuf::FieldDescriptor::CPPTYPE_MESSAGE:
      return false;
  }

  return false;
}

[[maybe_unused]] static bool field_name_passes_filter(const google::protobuf::FieldDescriptor* field) {
  if (filter_list.empty()) {
    return true;
  }

  bool skip = black_mode ? false : true;

#if GOOGLE_PROTOBUF_VERSION >= 6030000
  std::string left_str = std::string(field->name());
#else
  std::string left_str = field->name();
#endif

  std::transform(left_str.begin(), left_str.end(), left_str.begin(), [](char& c) { return std::tolower(c); });

  for (const auto& f : filter_list) {
    if (f.empty()) {
      continue;
    }

    std::string right_str = f;
    std::transform(right_str.begin(), right_str.end(), right_str.begin(), [](char& c) { return std::tolower(c); });

    if (left_str.find(right_str) != std::string::npos) {
      skip = black_mode ? true : false;
      break;
    }
  }

  return !skip;
}

static void print_message_with_defaults(const google::protobuf::TextFormat::Printer* printer,
                                        const google::protobuf::TextFormat::FastFieldValuePrinter* name_printer,
                                        const google::protobuf::Message& message, bool single_line_mode,
                                        bool root_level, google::protobuf::TextFormat::BaseTextGenerator* generator);

[[maybe_unused]] static bool map_entry_key_less(const google::protobuf::FieldDescriptor* key_field,
                                                const google::protobuf::Message* lhs,
                                                const google::protobuf::Message* rhs) {
  const auto* lhs_reflection = lhs->GetReflection();
  const auto* rhs_reflection = rhs->GetReflection();

  switch (key_field->cpp_type()) {
    case google::protobuf::FieldDescriptor::CPPTYPE_BOOL:
      return !lhs_reflection->GetBool(*lhs, key_field) && rhs_reflection->GetBool(*rhs, key_field);
    case google::protobuf::FieldDescriptor::CPPTYPE_INT32:
      return lhs_reflection->GetInt32(*lhs, key_field) < rhs_reflection->GetInt32(*rhs, key_field);
    case google::protobuf::FieldDescriptor::CPPTYPE_INT64:
      return lhs_reflection->GetInt64(*lhs, key_field) < rhs_reflection->GetInt64(*rhs, key_field);
    case google::protobuf::FieldDescriptor::CPPTYPE_UINT32:
      return lhs_reflection->GetUInt32(*lhs, key_field) < rhs_reflection->GetUInt32(*rhs, key_field);
    case google::protobuf::FieldDescriptor::CPPTYPE_UINT64:
      return lhs_reflection->GetUInt64(*lhs, key_field) < rhs_reflection->GetUInt64(*rhs, key_field);
    case google::protobuf::FieldDescriptor::CPPTYPE_STRING:
      return lhs_reflection->GetString(*lhs, key_field) < rhs_reflection->GetString(*rhs, key_field);
    default:
      return false;
  }
}

[[maybe_unused]] static void collect_sorted_map_entries(const google::protobuf::Reflection* reflection,
                                                        const google::protobuf::Message& message,
                                                        const google::protobuf::FieldDescriptor* field,
                                                        std::vector<const google::protobuf::Message*>* entries) {
  const int size = reflection->FieldSize(message, field);

  entries->reserve(static_cast<size_t>(size));

  for (int j = 0; j < size; ++j) {
    entries->push_back(&reflection->GetRepeatedMessage(message, field, j));
  }

  const google::protobuf::FieldDescriptor* key_field = field->message_type()->field(0);

  std::stable_sort(entries->begin(), entries->end(),
                   [key_field](const google::protobuf::Message* lhs, const google::protobuf::Message* rhs) {
                     return map_entry_key_less(key_field, lhs, rhs);
                   });
}

[[maybe_unused]] static void print_one_field(const google::protobuf::TextFormat::Printer* printer,
                                             const google::protobuf::TextFormat::FastFieldValuePrinter* name_printer,
                                             const google::protobuf::Message& message,
                                             const google::protobuf::Reflection* reflection,
                                             const google::protobuf::FieldDescriptor* field, bool single_line_mode,
                                             google::protobuf::TextFormat::BaseTextGenerator* generator) {
  if (!use_long_repeated && field->is_repeated() &&
      field->cpp_type() != google::protobuf::FieldDescriptor::CPPTYPE_STRING &&
      field->cpp_type() != google::protobuf::FieldDescriptor::CPPTYPE_MESSAGE) {
    const int size = reflection->FieldSize(message, field);

    name_printer->PrintFieldName(message, -1, size, reflection, field, generator);
    generator->PrintLiteral(": [");

    for (int i = 0; i < size; ++i) {
      if (i > 0) {
        generator->PrintLiteral(", ");
      }

      std::string value;
      printer->PrintFieldValueToString(message, field, i, &value);
      generator->PrintString(value);
    }

    if (single_line_mode) {
      generator->PrintLiteral("] ");
    } else {
      generator->PrintLiteral("]\n");
    }

    return;
  }

  int count = 0;

  if (field->is_repeated()) {
    count = reflection->FieldSize(message, field);
  } else if (reflection->HasField(message, field) || field->containing_type()->options().map_entry()) {
    count = 1;
  }

  const bool is_map = field->is_map();
  std::vector<const google::protobuf::Message*> sorted_map_entries;

  if (is_map) {
    collect_sorted_map_entries(reflection, message, field, &sorted_map_entries);
  }

  for (int j = 0; j < count; ++j) {
    const int field_index = field->is_repeated() ? j : -1;

    name_printer->PrintFieldName(message, field_index, count, reflection, field, generator);

    if (field->cpp_type() == google::protobuf::FieldDescriptor::CPPTYPE_MESSAGE) {
      const google::protobuf::Message* sub_message = nullptr;

      if (!field->is_repeated()) {
        sub_message = &reflection->GetMessage(message, field);
      } else if (is_map) {
        sub_message = sorted_map_entries[j];
      } else {
        sub_message = &reflection->GetRepeatedMessage(message, field, j);
      }

      name_printer->PrintMessageStart(*sub_message, field_index, count, single_line_mode, generator);
      generator->Indent();
      print_message_with_defaults(printer, name_printer, *sub_message, single_line_mode, false, generator);
      generator->Outdent();
      name_printer->PrintMessageEnd(*sub_message, field_index, count, single_line_mode, generator);
    } else {
      std::string value;
      printer->PrintFieldValueToString(message, field, field_index, &value);

      generator->PrintLiteral(": ");
      generator->PrintString(value);

      if (single_line_mode) {
        generator->PrintLiteral(" ");
      } else {
        generator->PrintLiteral("\n");
      }
    }
  }
}

[[maybe_unused]] static void print_message_with_defaults(
    const google::protobuf::TextFormat::Printer* printer,
    const google::protobuf::TextFormat::FastFieldValuePrinter* name_printer, const google::protobuf::Message& message,
    bool single_line_mode, bool root_level, google::protobuf::TextFormat::BaseTextGenerator* generator) {
  const auto* descriptor = message.GetDescriptor();
  const auto* reflection = message.GetReflection();

  if VUNLIKELY (descriptor == nullptr || reflection == nullptr) {
    return;
  }

  if (descriptor->options().map_entry()) {
    print_one_field(printer, name_printer, message, reflection, descriptor->field(0), single_line_mode, generator);
    print_one_field(printer, name_printer, message, reflection, descriptor->field(1), single_line_mode, generator);

    return;
  }

  std::vector<const google::protobuf::FieldDescriptor*> fields;

  reflection->ListFields(message, &fields);

  if (!ignore_default && !single_line_mode) {
    for (int i = 0; i < descriptor->field_count(); ++i) {
      const auto* field = descriptor->field(i);

      if (!is_no_presence_default_scalar(message, field)) {
        continue;
      }

      if (root_level && !field_name_passes_filter(field)) {
        continue;
      }

      fields.push_back(field);
    }

    std::stable_sort(fields.begin(), fields.end(),
                     [](const google::protobuf::FieldDescriptor* lhs, const google::protobuf::FieldDescriptor* rhs) {
                       return lhs->number() < rhs->number();
                     });
  }

  for (const auto* field : fields) {
    if (is_no_presence_default_scalar(message, field)) {
      name_printer->PrintFieldName(message, -1, 1, reflection, field, generator);
      generator->PrintLiteral(": ");

      std::string value;
      printer->PrintFieldValueToString(message, field, -1, &value);
      generator->PrintString(value);
      generator->PrintLiteral("\n");
    } else {
      print_one_field(printer, name_printer, message, reflection, field, single_line_mode, generator);
    }
  }
}

class DefaultScalarMessagePrinter final : public google::protobuf::TextFormat::MessagePrinter {
 public:
  DefaultScalarMessagePrinter(const google::protobuf::TextFormat::Printer* printer,
                              const google::protobuf::TextFormat::FastFieldValuePrinter* name_printer)
      : printer_(printer), name_printer_(name_printer) {}

  void Print(const google::protobuf::Message& message, bool single_line_mode,
             google::protobuf::TextFormat::BaseTextGenerator* generator) const override {
    print_message_with_defaults(printer_, name_printer_, message, single_line_mode, true, generator);
  }

 private:
  const google::protobuf::TextFormat::Printer* printer_{nullptr};
  const google::protobuf::TextFormat::FastFieldValuePrinter* name_printer_{nullptr};
};

#endif  // GOOGLE_PROTOBUF_VERSION >= 3006000

[[maybe_unused]] static bool load_proto_for_file(const std::string& filename, ::google::protobuf::Message* message) {
  if (message == nullptr) {
    return false;
  }

  std::ifstream file(filename);

  if (!file) {
    return false;
  }

  google::protobuf::io::IstreamInputStream input(&file);
  return google::protobuf::TextFormat::Parse(&input, message);
}

[[maybe_unused]] static bool load_proto_for_string(const std::string& content, ::google::protobuf::Message* message) {
  if (message == nullptr) {
    return false;
  }

  return google::protobuf::TextFormat::ParseFromString(content, message);
}

[[maybe_unused]] static bool convert_proto_to_txt(std::string& content, ::google::protobuf::Message* message) {
  if (message == nullptr) {
    return false;
  }

  google::protobuf::TextFormat::Printer printer;
  auto* value_printer = new CustomFieldValuePrinter;

  printer.SetDefaultFieldValuePrinter(value_printer);
  printer.SetHideUnknownFields(true);
  printer.SetUseShortRepeatedPrimitives(!use_long_repeated);

#if GOOGLE_PROTOBUF_VERSION >= 3006000
  printer.RegisterMessagePrinter(message->GetDescriptor(), new DefaultScalarMessagePrinter(&printer, value_printer));
#endif

  return printer.PrintToString(*message, &content);
}

#ifdef VLINK_HAS_PROTOBUF_JSON_UTIL
[[maybe_unused]] static bool load_proto_for_json_string(const std::string& content,
                                                        ::google::protobuf::Message* message,
                                                        std::string* error = nullptr) {
  if (message == nullptr) {
    return false;
  }

  message->Clear();

  google::protobuf::util::JsonParseOptions options;
  auto status = google::protobuf::util::JsonStringToMessage(content, message, options);

  if (!status.ok()) {
    if (error != nullptr) {
      *error = std::string(status.ToString());
    }

    return false;
  }

  return true;
}

[[maybe_unused]] static bool load_proto_for_json_file(const std::string& filename, ::google::protobuf::Message* message,
                                                      std::string* error = nullptr) {
  std::string content;

  if (!load_text_for_file(filename, content)) {
    if (error != nullptr) {
      *error = "Cannot open JSON file.";
    }

    return false;
  }

  return load_proto_for_json_string(content, message, error);
}

[[maybe_unused]] static bool convert_proto_to_json(std::string& content, const ::google::protobuf::Message* message,
                                                   std::string* error = nullptr) {
  if (message == nullptr) {
    return false;
  }

  google::protobuf::util::JsonPrintOptions options;
  options.add_whitespace = true;
  options.always_print_enums_as_ints = !print_enum_string;
  options.preserve_proto_field_names = true;

  std::string json_output;
  auto status = google::protobuf::util::MessageToJsonString(*message, &json_output, options);

  if (!status.ok()) {
    if (error != nullptr) {
      *error = std::string(status.ToString());
    }

    return false;
  }

  content = std::move(json_output);
  return true;
}
#endif

[[maybe_unused]] static void set_proto_value_to_default(google::protobuf::Message* message) {
  const google::protobuf::Descriptor* descriptor = message->GetDescriptor();
  const google::protobuf::Reflection* reflection = message->GetReflection();

  for (int i = 0; i < descriptor->field_count(); ++i) {
    const google::protobuf::FieldDescriptor* field = descriptor->field(i);

    if (field->is_map()) {
      continue;
    }

    if (field->is_repeated()) {
      int count = reflection->FieldSize(*message, field);

      for (int j = 0; j < count; ++j) {
        if (field->cpp_type() == google::protobuf::FieldDescriptor::CPPTYPE_MESSAGE) {
          set_proto_value_to_default(reflection->MutableRepeatedMessage(message, field, j));
        }
      }

      continue;
    }

    if (field->containing_oneof()) {
      const auto* oneof = field->containing_oneof();
      if (reflection->HasOneof(*message, oneof)) {
        continue;
      }
    } else {
      if (reflection->HasField(*message, field) &&
          field->cpp_type() != google::protobuf::FieldDescriptor::CPPTYPE_MESSAGE) {
        continue;
      }
    }

    switch (field->cpp_type()) {
      case google::protobuf::FieldDescriptor::CPPTYPE_INT32:
        reflection->SetInt32(message, field, field->default_value_int32());
        break;
      case google::protobuf::FieldDescriptor::CPPTYPE_INT64:
        reflection->SetInt64(message, field, field->default_value_int64());
        break;
      case google::protobuf::FieldDescriptor::CPPTYPE_UINT32:
        reflection->SetUInt32(message, field, field->default_value_uint32());
        break;
      case google::protobuf::FieldDescriptor::CPPTYPE_UINT64:
        reflection->SetUInt64(message, field, field->default_value_uint64());
        break;
      case google::protobuf::FieldDescriptor::CPPTYPE_DOUBLE:
        reflection->SetDouble(message, field, field->default_value_double());
        break;
      case google::protobuf::FieldDescriptor::CPPTYPE_FLOAT:
        reflection->SetFloat(message, field, field->default_value_float());
        break;
      case google::protobuf::FieldDescriptor::CPPTYPE_BOOL:
        reflection->SetBool(message, field, field->default_value_bool());
        break;
      case google::protobuf::FieldDescriptor::CPPTYPE_STRING:
#if GOOGLE_PROTOBUF_VERSION >= 6030000
        reflection->SetString(message, field, std::string(field->default_value_string()));
#else
        reflection->SetString(message, field, field->default_value_string());
#endif
        break;
      case google::protobuf::FieldDescriptor::CPPTYPE_ENUM:
        reflection->SetEnum(message, field, field->default_value_enum());
        break;
      case google::protobuf::FieldDescriptor::CPPTYPE_MESSAGE:
        set_proto_value_to_default(reflection->MutableMessage(message, field));
        break;
    }
  }
}

[[maybe_unused]] static void import_protos(google::protobuf::compiler::Importer* importer,
                                           const std::filesystem::path& root_dir, const std::filesystem::path& sub_dir,
                                           bool& has_import, int depth = 0) {
  if VUNLIKELY (depth >= 100) {
    return;
  }

  std::vector<std::filesystem::directory_entry> file_list;

  try {
    for (const auto& entry : std::filesystem::directory_iterator(sub_dir)) {
      file_list.emplace_back(entry);
    }
  } catch (std::filesystem::filesystem_error&) {
    return;
  }

  if VUNLIKELY (file_list.empty() || file_list.size() > 1000) {
    return;
  }

  for (const auto& file : file_list) {
    if (file.is_regular_file() && file.path().extension() == ".proto") {
      try {
#ifdef _WIN32
        auto relative_path = vlink::Helpers::path_to_string(std::filesystem::relative(file.path(), root_dir));
        std::replace(relative_path.begin(), relative_path.end(), '\\', '/');
#else
        auto relative_path = std::filesystem::relative(file.path(), root_dir).string();
#endif
        const auto* ptr = importer->Import(relative_path);

        if (ptr) {
          has_import = true;
        }
      } catch (std::filesystem::filesystem_error&) {
        continue;
      }
    } else if (file.is_directory()) {
      import_protos(importer, root_dir, file.path(), has_import, depth + 1);
    }
  }
}

int start_eproto_pub(const std::string& url, const std::string& proto_dir, const std::string& prototxt_file,
                     const std::string& prototxt_content, const std::string& ser, vlink::SchemaType schema_type,
                     bool use_blob_encoding, bool native_mode, int times, int interval, bool use_json_format) {
  GOOGLE_PROTOBUF_VERIFY_VERSION;

  if VUNLIKELY (!has_intra_bind && vlink::Url::is_intra_type(url)) {
    std::cerr << "Cannot pub intra url." << std::endl;
    has_quit = true;
    return -1;
  }

  std::shared_ptr<vlink::DiscoveryViewer> discovery_viewer;
  std::shared_ptr<google::protobuf::compiler::DiskSourceTree> source_tree;
  std::shared_ptr<google::protobuf::compiler::Importer> importer;
  std::shared_ptr<google::protobuf::DynamicMessageFactory> factory;
  std::shared_ptr<RawPub> raw_pub;

  if (interval < 0) {
    interval = 0;
  }

  if VUNLIKELY (url.empty()) {
    std::cerr << "Url is empty." << std::endl;
    has_quit = true;
    return -1;
  }

  try {
    raw_pub = std::make_shared<RawPub>(url, vlink::InitType::kWithoutInit);
  } catch (vlink::Exception::RuntimeError& e) {
    std::cerr << e.what() << std::endl;
    has_quit = true;
    return -1;
  }

  std::string target_ser = ser;

  if (target_ser.empty()) {
    try {
      vlink::DiscoveryViewer::FilterType filter_type = vlink::DiscoveryViewer::kFilterAvailable;

      if (native_mode) {
        filter_type = vlink::DiscoveryViewer::kFilterNative;
      }

      discovery_viewer = std::make_shared<vlink::DiscoveryViewer>(filter_type);
    } catch (vlink::Exception::RuntimeError& e) {
      std::cerr << e.what() << std::endl;
      has_quit = true;
      return -1;
    }

    discovery_viewer->async_run();
  }

  auto quit_function = [&discovery_viewer, &raw_pub](int) {
    if VUNLIKELY (has_quit) {
      return;
    }

    has_quit = true;

    raw_pub->deinit();

    if (discovery_viewer) {
      discovery_viewer->quit(true);
    }
  };

  vlink::Utils::register_terminate_signal(quit_function);

  auto target_schema_type = schema_type;

  if (target_ser.empty()) {
    VLINK_TERM_OUT << "Information Collecting, Please Wait...";
    VLINK_TERM_OUT.flush();

    discovery_viewer->wait_for_quit(kCollectInterval);

    VLINK_TERM_OUT << "\033[2K\r";
    VLINK_TERM_OUT.flush();

    target_ser = discovery_viewer->get_ser_type(url);

    if VUNLIKELY (target_ser.empty()) {
      std::cerr << "Cannot find ser for discovery." << std::endl;
      has_quit = true;
      return -1;
    }
  }

  if (target_schema_type == vlink::SchemaType::kUnknown && discovery_viewer) {
    target_schema_type = vlink::SchemaData::resolve_type(discovery_viewer->get_schema_type(url), target_ser);
  }

  if (target_schema_type == vlink::SchemaType::kUnknown) {
    target_schema_type = vlink::SchemaData::infer_ser_type(target_ser);
  }

  if (target_schema_type == vlink::SchemaType::kUnknown) {
    target_schema_type = vlink::SchemaType::kProtobuf;
  }

  const auto inferred_schema_type = vlink::SchemaData::infer_ser_type(target_ser);

  if VUNLIKELY (target_schema_type != vlink::SchemaType::kRaw && inferred_schema_type != vlink::SchemaType::kUnknown &&
                inferred_schema_type != target_schema_type) {
    std::cerr << "ser_type and encoding do not match." << std::endl;
    has_quit = true;
    return -1;
  }

  if VUNLIKELY (target_schema_type == vlink::SchemaType::kFlatbuffers) {
    std::cerr << "eproto pub does not support flatbuffers schema_type." << std::endl;
    has_quit = true;
    return -1;
  }

  if VUNLIKELY (discovery_viewer && discovery_viewer->is_ready_to_quit()) {
    has_quit = true;

    raw_pub.reset();
    factory.reset();
    importer.reset();
    source_tree.reset();
    discovery_viewer.reset();

    return 0;
  }

  const bool is_blob_type = target_schema_type == vlink::SchemaType::kRaw && use_blob_encoding;
  bool is_text_type = !is_blob_type && (target_schema_type == vlink::SchemaType::kRaw || is_text_ser_type(target_ser));
  const bool is_zerocopy_type = target_schema_type == vlink::SchemaType::kZeroCopy;

  if VUNLIKELY (is_zerocopy_type) {
    std::cerr << "eproto pub only supports protobuf or raw text/json payloads." << std::endl;
    has_quit = true;
    return -1;
  }

#ifndef VLINK_HAS_PROTOBUF_JSON_UTIL

  if VUNLIKELY (use_json_format && target_schema_type == vlink::SchemaType::kProtobuf) {
    std::cerr << "Current protobuf does not support JSON conversion." << std::endl;
    has_quit = true;
    return -1;
  }
#endif

  if (!prototxt_file.empty()) {
    try {
#ifdef _WIN32
      auto filesys_prototxt_file = std::filesystem::path(vlink::Helpers::string_to_wstring(prototxt_file));
#else
      auto filesys_prototxt_file = std::filesystem::path(prototxt_file);
#endif

      if VUNLIKELY (!std::filesystem::exists(filesys_prototxt_file)) {
        std::cerr << "Proto txt file does not exist." << std::endl;
        has_quit = true;
        return -1;
      }

      if VUNLIKELY (!std::filesystem::is_regular_file(filesys_prototxt_file)) {
        std::cerr << "Proto txt file is not a file." << std::endl;
        has_quit = true;
        return -1;
      }
    } catch (std::filesystem::filesystem_error& e) {
      std::cerr << e.what() << std::endl;
      has_quit = true;
      return -1;
    }
  }

  factory = std::make_shared<google::protobuf::DynamicMessageFactory>();
  source_tree = std::make_shared<google::protobuf::compiler::DiskSourceTree>();
  importer = std::make_shared<google::protobuf::compiler::Importer>(source_tree.get(), nullptr);
  google::protobuf::Message* root_msg = nullptr;
  vlink::Bytes raw_data;

  if (is_blob_type) {
    if (prototxt_file.empty()) {
      bool ok = false;
      raw_data = vlink::Bytes::from_user_input(prototxt_content, &ok);

      if VUNLIKELY (!ok) {
        std::cerr << "Blob content must be hex bytes." << std::endl;
        has_quit = true;
        return -1;
      }
    } else {
      std::string blob_payload;

      if VUNLIKELY (!load_text_for_file(prototxt_file, blob_payload)) {
        std::cerr << "load_text_for_file failed." << std::endl;
        has_quit = true;
        return -1;
      }

      raw_data = vlink::Bytes::from_string(blob_payload);
    }
  } else if (is_text_type) {
    std::string text_payload;

    if (prototxt_file.empty()) {
      text_payload = prototxt_content;
    } else if VUNLIKELY (!load_text_for_file(prototxt_file, text_payload)) {
      std::cerr << "load_text_for_file failed." << std::endl;
      has_quit = true;
      return -1;
    }

    raw_data = vlink::Bytes::from_string(text_payload);
  } else {
    google::protobuf::Descriptor* descriptor = nullptr;

    auto schema_interface = vlink::SchemaPluginManager::get().get_interface();

    if (schema_interface) {
      descriptor = static_cast<google::protobuf::Descriptor*>(schema_interface->search_protobuf_descriptor(target_ser));
    } else {
      if VUNLIKELY (proto_dir.empty()) {
        std::cerr << "Must set proto dir [-d], set env 'VLINK_PROTO_DIR', run 'vlink-eproto import <dir>', or load "
                     "VLINK_SCHEMA_PLUGIN."
                  << std::endl;
        has_quit = true;
        return 1;
      }

      bool has_import = false;

      try {
#ifdef _WIN32
        auto proto_path = std::filesystem::path(vlink::Helpers::string_to_wstring(proto_dir));
#else
        auto proto_path = std::filesystem::path(proto_dir);
#endif

        if VUNLIKELY (!std::filesystem::exists(proto_path)) {
          std::cerr << "Proto dir does not exist." << std::endl;
          has_quit = true;
          return -1;
        }

        if VUNLIKELY (!std::filesystem::is_directory(proto_path)) {
          std::cerr << "Proto dir is not a directory." << std::endl;
          has_quit = true;
          return -1;
        }

#ifdef _WIN32
        source_tree->MapPath("", vlink::Helpers::path_to_string(proto_path));
#else
        source_tree->MapPath("", proto_path.string());
#endif

        import_protos(importer.get(), proto_path, proto_path, has_import);
      } catch (std::filesystem::filesystem_error& e) {
        std::cerr << e.what() << std::endl;
        has_quit = true;
        return -1;
      }

      if VUNLIKELY (!has_import) {
        std::cerr << "Import proto dir failed." << std::endl;
        has_quit = true;
        return -1;
      }

      auto* des_pool = const_cast<google::protobuf::DescriptorPool*>(importer->pool());

      if VUNLIKELY (!des_pool) {
        std::cerr << "Cannot find proto." << std::endl;
        has_quit = true;
        return -1;
      }

      descriptor = const_cast<google::protobuf::Descriptor*>(des_pool->FindMessageTypeByName(target_ser));
    }

    if VUNLIKELY (!descriptor) {
      std::cerr << "Cannot find ser." << std::endl;
      has_quit = true;
      return -1;
    }

    root_msg = factory->GetPrototype(descriptor)->New();

    if VUNLIKELY (!root_msg) {
      std::cerr << "Create root msg failed." << std::endl;
      has_quit = true;
      return -1;
    }

    if (use_json_format) {
#ifdef VLINK_HAS_PROTOBUF_JSON_UTIL
      std::string json_error;

      if (prototxt_file.empty()) {
        if VUNLIKELY (!load_proto_for_json_string(prototxt_content, root_msg, &json_error)) {
          std::cerr << "load_proto_for_json_string failed.";
          if (!json_error.empty()) {
            std::cerr << " " << json_error;
          }
          std::cerr << std::endl;
          has_quit = true;
          return -1;
        }
      } else {
        if VUNLIKELY (!load_proto_for_json_file(prototxt_file, root_msg, &json_error)) {
          std::cerr << "load_proto_for_json_file failed.";
          if (!json_error.empty()) {
            std::cerr << " " << json_error;
          }
          std::cerr << std::endl;
          has_quit = true;
          return -1;
        }
      }
#endif
    } else {
      if (prototxt_file.empty()) {
        if VUNLIKELY (!load_proto_for_string(prototxt_content, root_msg)) {
          std::cerr << "load_proto_for_string failed." << std::endl;
          has_quit = true;
          return -1;
        }
      } else {
        if VUNLIKELY (!load_proto_for_file(prototxt_file, root_msg)) {
          std::cerr << "load_proto_for_file failed." << std::endl;
          has_quit = true;
          return -1;
        }
      }
    }

    raw_data = vlink::Bytes::create(root_msg->ByteSizeLong());

    bool ret = root_msg->SerializePartialToArray(raw_data.data(), raw_data.size());

    if VUNLIKELY (!ret) {
      raw_data.clear();
    }
  }

  if (native_mode) {
    raw_pub->set_property("dds.ip", "127.0.0.1");
  }

  raw_pub->set_ser_type(target_ser, target_schema_type);
  raw_pub->init();

  vlink::Utils::start_detect_keyboard([&quit_function](const std::string& key) {
    if (key == "q" || key == "esc") {
      quit_function(0);
    } else if (key == " ") {
      if (is_paused) {
        is_paused = false;
      } else {
        is_paused = true;
      }
    }
  });

  if (raw_pub->has_inited()) {
    raw_pub->wait_for_subscribers(std::chrono::milliseconds(500));
  }

  vlink::ElapsedTimer elapsed;
  elapsed.start();

  int dx = 0;

  for (int i = 0; i < times || times <= 0; ++i) {
    if (!raw_pub->has_inited()) {
      break;
    }

    raw_pub->publish(raw_data);

    dx = static_cast<int>((static_cast<int64_t>(i) + 1) * interval - elapsed.get());

    if (dx < 0) {
      dx = 0;
    }

    if (discovery_viewer) {
      if (discovery_viewer->wait_for_quit(dx)) {
        break;
      }
    } else {
      if VUNLIKELY (has_quit) {
        break;
      }

      if (dx > 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(dx));
      }

      if VUNLIKELY (has_quit) {
        break;
      }
    }
  }

  if (discovery_viewer) {
    discovery_viewer->quit(true);
    discovery_viewer->wait_for_quit();
  }

  has_quit = true;

  vlink::Utils::stop_detect_keyboard();
  // VLINK_TERM_OUT << std::endl;
  VLINK_TERM_OUT.flush();

  delete root_msg;
  root_msg = nullptr;

  raw_pub.reset();
  factory.reset();
  importer.reset();
  source_tree.reset();
  discovery_viewer.reset();

  return 0;
}

class ParserLoop : public vlink::MessageLoop {
 public:
  ParserLoop() {
    set_name("ParserLoop");
    set_strategy(vlink::MessageLoop::kPopStrategy);
  }

 protected:
  size_t get_max_task_count() const override { return kMaxTaskSize; }

  uint32_t get_max_elapsed_time() const override { return kMaxElapsedTime; }
};

// NOLINTNEXTLINE(google-readability-function-size)
int start_eproto_sub(const std::string& url, const std::string& proto_dir, const std::string& ser,
                     vlink::SchemaType schema_type, bool use_blob_encoding, bool native_mode, const std::string& filter,
                     bool use_getter, bool use_json_format) {
  GOOGLE_PROTOBUF_VERIFY_VERSION;

  if VUNLIKELY (!has_intra_bind && vlink::Url::is_intra_type(url)) {
    std::cerr << "Cannot sub intra url." << std::endl;
    has_quit = true;
    return -1;
  }

  std::shared_ptr<vlink::DiscoveryViewer> discovery_viewer;
  std::shared_ptr<ParserLoop> parser_loop;
  std::shared_ptr<google::protobuf::compiler::DiskSourceTree> source_tree;
  std::shared_ptr<google::protobuf::compiler::Importer> importer;
  std::shared_ptr<google::protobuf::DynamicMessageFactory> factory;
  std::optional<std::shared_ptr<RawSub>> raw_sub;
  std::optional<std::shared_ptr<RawGetter>> raw_getter;

  if VUNLIKELY (url.empty()) {
    std::cerr << "Url is empty." << std::endl;
    has_quit = true;
    return -1;
  }

  bool has_explicit_ser = !ser.empty();
  std::string target_ser = ser;

  if (target_ser.empty()) {
    try {
      vlink::DiscoveryViewer::FilterType filter_type = vlink::DiscoveryViewer::kFilterAvailable;

      if (native_mode) {
        filter_type = vlink::DiscoveryViewer::kFilterNative;
      }

      discovery_viewer = std::make_shared<vlink::DiscoveryViewer>(filter_type);
    } catch (vlink::Exception::RuntimeError& e) {
      std::cerr << e.what() << std::endl;
      has_quit = true;
      return -1;
    }

    discovery_viewer->async_run();
  }

  parser_loop = std::make_shared<ParserLoop>();
  parser_loop->async_run();

  auto quit_function = [&discovery_viewer, &parser_loop](int) {
    if VUNLIKELY (has_quit) {
      return;
    }

    has_quit = true;

    if (discovery_viewer) {
      discovery_viewer->quit(true);
    }

    parser_loop->quit(true);
  };

  vlink::Utils::register_terminate_signal(quit_function);
  uint32_t target_type = 0;
  auto target_schema_type = schema_type;

  if (target_ser.empty()) {
    VLINK_TERM_OUT << "Information Collecting, Please Wait...";
    VLINK_TERM_OUT.flush();

    discovery_viewer->wait_for_quit(kCollectInterval);

    VLINK_TERM_OUT << "\033[2K\r";
    VLINK_TERM_OUT.flush();

    target_ser = discovery_viewer->get_ser_type(url);

    for (const auto& info : discovery_viewer->get_info_list()) {
      if (info.url == url) {
        target_type = info.type;
        break;
      }
    }

    if VUNLIKELY (target_ser.empty()) {
      std::cerr << "Cannot find ser for discovery." << std::endl;
      has_quit = true;
      return -1;
    }
  }

  if (target_schema_type == vlink::SchemaType::kUnknown && discovery_viewer) {
    target_schema_type = vlink::SchemaData::resolve_type(discovery_viewer->get_schema_type(url), target_ser);
  }

  if (target_schema_type == vlink::SchemaType::kUnknown) {
    target_schema_type = vlink::SchemaData::infer_ser_type(target_ser);
  }

  if (target_schema_type == vlink::SchemaType::kUnknown) {
    target_schema_type = vlink::SchemaType::kProtobuf;
  }

  const auto inferred_schema_type = vlink::SchemaData::infer_ser_type(target_ser);

  if VUNLIKELY (target_schema_type != vlink::SchemaType::kRaw && inferred_schema_type != vlink::SchemaType::kUnknown &&
                inferred_schema_type != target_schema_type) {
    std::cerr << "ser_type and encoding do not match." << std::endl;
    has_quit = true;
    return -1;
  }

  if VUNLIKELY (target_schema_type == vlink::SchemaType::kFlatbuffers) {
    std::cerr << "eproto sub does not support flatbuffers schema_type." << std::endl;
    has_quit = true;
    return -1;
  }

  if VUNLIKELY (discovery_viewer && discovery_viewer->is_ready_to_quit()) {
    has_quit = true;

    raw_sub.reset();
    raw_getter.reset();
    factory.reset();
    importer.reset();
    source_tree.reset();
    discovery_viewer.reset();
    parser_loop.reset();

    return 0;
  }

  filter_list = vlink::Helpers::split_any(filter);

  google::protobuf::Message* root_msg = nullptr;
  const bool is_blob_type = target_schema_type == vlink::SchemaType::kRaw && use_blob_encoding;
  const bool is_text_type =
      !is_blob_type && (target_schema_type == vlink::SchemaType::kRaw || is_text_ser_type(target_ser));
  const bool is_zerocopy_type = target_schema_type == vlink::SchemaType::kZeroCopy;

#ifndef VLINK_HAS_PROTOBUF_JSON_UTIL

  if VUNLIKELY (use_json_format && target_schema_type == vlink::SchemaType::kProtobuf) {
    std::cerr << "Current protobuf does not support JSON conversion." << std::endl;
    has_quit = true;
    return -1;
  }
#endif

  if VUNLIKELY (use_json_format && is_zerocopy_type) {
    std::cerr << "JSON mode is not supported for zerocopy message types." << std::endl;
    has_quit = true;
    return -1;
  }

  if (target_schema_type == vlink::SchemaType::kZeroCopy) {
    is_proto_type = false;
  } else if (target_schema_type == vlink::SchemaType::kRaw) {
    is_proto_type = false;
  } else if (target_schema_type == vlink::SchemaType::kProtobuf) {
    is_proto_type = true;

    factory = std::make_shared<google::protobuf::DynamicMessageFactory>();
    source_tree = std::make_shared<google::protobuf::compiler::DiskSourceTree>();
    importer = std::make_shared<google::protobuf::compiler::Importer>(source_tree.get(), nullptr);
    google::protobuf::Descriptor* descriptor = nullptr;

    auto schema_interface = vlink::SchemaPluginManager::get().get_interface();

    if (schema_interface) {
      descriptor = static_cast<google::protobuf::Descriptor*>(schema_interface->search_protobuf_descriptor(target_ser));
    } else {
      if VUNLIKELY (proto_dir.empty()) {
        std::cerr << "Must set proto dir [-d], set env 'VLINK_PROTO_DIR', run 'vlink-eproto import <dir>', or load "
                     "VLINK_SCHEMA_PLUGIN."
                  << std::endl;
        has_quit = true;
        return 1;
      }

      bool has_import = false;

      try {
#ifdef _WIN32
        auto proto_path = std::filesystem::path(vlink::Helpers::string_to_wstring(proto_dir));
#else
        auto proto_path = std::filesystem::path(proto_dir);
#endif

        if VUNLIKELY (!std::filesystem::exists(proto_path)) {
          std::cerr << "Proto dir does not exist." << std::endl;
          has_quit = true;
          return -1;
        }

        if VUNLIKELY (!std::filesystem::is_directory(proto_path)) {
          std::cerr << "Proto dir is not a directory." << std::endl;
          has_quit = true;
          return -1;
        }

#ifdef _WIN32
        source_tree->MapPath("", vlink::Helpers::path_to_string(proto_path));
#else
        source_tree->MapPath("", proto_path.string());
#endif

        import_protos(importer.get(), proto_path, proto_path, has_import);
      } catch (std::filesystem::filesystem_error& e) {
        std::cerr << e.what() << std::endl;
        has_quit = true;
        return -1;
      }

      if VUNLIKELY (!has_import) {
        std::cerr << "Import proto dir failed." << std::endl;
        has_quit = true;
        return -1;
      }

      auto* des_pool = const_cast<google::protobuf::DescriptorPool*>(importer->pool());

      if VUNLIKELY (!des_pool) {
        std::cerr << "Cannot find proto." << std::endl;
        has_quit = true;
        return -1;
      }

      descriptor = const_cast<google::protobuf::Descriptor*>(des_pool->FindMessageTypeByName(target_ser));
    }

    if VUNLIKELY (!descriptor) {
      std::cerr << "Cannot find ser." << std::endl;
      has_quit = true;
      return -1;
    }

    root_msg = factory->GetPrototype(descriptor)->New();

    if VUNLIKELY (!root_msg) {
      std::cerr << "Create root msg failed." << std::endl;
      has_quit = true;
      return -1;
    }
  } else {
    std::cerr << "Unsupported schema_type for eproto sub: " << static_cast<int>(target_schema_type) << std::endl;
    has_quit = true;
    return -1;
  }

  std::atomic<int> parse_ret = 0;

  vlink::Bytes current_bytes;
  std::mutex bytes_mtx;
  std::atomic_bool has_new_data{false};
  std::atomic<int64_t> frame_seq{0};

  std::vector<std::string> print_list;
  std::vector<int> line_list;
  std::string print_str;
  std::string current_str;
  int current_line = 0;
  bool is_url_title_with_dot = false;
  bool has_rendered_url_title = false;
  bool rendered_url_title_with_dot = false;
  double current_frame_rate = 0.0;
  double rendered_frame_rate = -1.0;
  std::deque<int64_t> frame_seq_buffer;
  std::atomic<uint64_t> last_frame_timestamp_ms{0};
  int current_rate_color = 33;
  int rendered_rate_color = -1;

  auto redraw_url_title = [&url, &current_frame_rate, &current_rate_color](bool show_data_dot, bool restore_cursor) {
    const char* rate_color = "\033[33m";

    switch (current_rate_color) {
      case 31:
        rate_color = "\033[31m";
        break;
      case 32:
        rate_color = "\033[32m";
        break;
      default:
        break;
    }

    if (restore_cursor) {
      VLINK_TERM_OUT << "\033[s";
    }

    VLINK_TERM_OUT << "\033[2;1H";
    VLINK_TERM_OUT << "\033[K";

    VLINK_TERM_OUT << "\033[34;1;4m" << url << "\033[0m ";

    if (show_data_dot) {
      VLINK_TERM_OUT << rate_color << "\u25CF " << vlink::Helpers::double_to_string(current_frame_rate) << "Hz\033[0m";
    } else {
      VLINK_TERM_OUT << rate_color << "  " << vlink::Helpers::double_to_string(current_frame_rate) << "Hz\033[0m";
    }

    if (restore_cursor) {
      VLINK_TERM_OUT << "\033[u";
    } else {
      VLINK_TERM_OUT << std::endl;
    }
  };

  auto mark_url_title_rendered = [&has_rendered_url_title, &rendered_url_title_with_dot, &rendered_frame_rate,
                                  &rendered_rate_color, &current_frame_rate, &current_rate_color](bool show_data_dot) {
    has_rendered_url_title = true;
    rendered_url_title_with_dot = show_data_dot;
    rendered_frame_rate = current_frame_rate;
    rendered_rate_color = current_rate_color;
  };

  auto should_redraw_url_title = [&has_rendered_url_title, &rendered_url_title_with_dot, &rendered_frame_rate,
                                  &rendered_rate_color, &current_frame_rate, &current_rate_color](bool show_data_dot) {
    return !has_rendered_url_title || rendered_url_title_with_dot != show_data_dot ||
           rendered_rate_color != current_rate_color || rendered_frame_rate != current_frame_rate;
  };

  auto update_terminal_function = [&url, &target_ser, &current_bytes, &bytes_mtx, &print_str, &print_list, &line_list,
                                   &current_str, &current_line, &has_new_data, &is_url_title_with_dot,
                                   &redraw_url_title, &mark_url_title_rendered, &should_redraw_url_title,
                                   &discovery_viewer, &parser_loop, &quit_function, &parse_ret, is_text_type,
                                   is_blob_type, use_json_format, root_msg]() {
    auto target_terminal_size = get_terminal_size();
    bool show_data_dot = has_new_data.exchange(false);

    if (terminal_size != target_terminal_size) {
      terminal_size = target_terminal_size;
      is_changed = true;
    }

    if VUNLIKELY (terminal_size.first <= 0 || terminal_size.second <= 0) {
      return;
    }

    if VUNLIKELY (!has_printed) {
      if VLIKELY (!force_update) {
        is_changed = false;
        return;
      }

      VLINK_TERM_OUT << "\033[H\033[J";

      if (is_paused) {
        VLINK_TERM_OUT << "\033[33m"
                       << "Message Parsed by vlink-eproto (Wait For Message, Paused):"
                       << "\033[0m" << std::endl;
      } else {
        VLINK_TERM_OUT << "Message Parsed by vlink-eproto (Wait For Message)... " << std::endl;
      }

      VLINK_TERM_OUT << "\033[34;1;4m" << url << "\033[0m" << std::endl;
      VLINK_TERM_OUT.flush();

      is_changed = false;
      force_update = false;
      return;
    }

    if VLIKELY (!is_changed) {
      if (!is_paused && should_redraw_url_title(show_data_dot)) {
        redraw_url_title(show_data_dot, true);
        mark_url_title_rendered(show_data_dot);
        VLINK_TERM_OUT.flush();
      }

      is_url_title_with_dot = false;
      force_update = false;
      return;
    }

    if (is_proto_type) {
      {
        if VUNLIKELY (!root_msg) {
          return;
        }

        std::scoped_lock lock(bytes_mtx);

        // if (current_bytes.empty()) {
        //   force_update = false;
        //   return;
        // }

        if VUNLIKELY (!root_msg->ParseFromArray(current_bytes.data(), current_bytes.size())) {
          std::cerr << "Failed to parse Protobuf message." << std::endl;
          quit_function(0);

          parse_ret = 1;

          force_update = false;
          return;
        }

        if (!ignore_default) {
          set_proto_value_to_default(root_msg);
        }
      }

      if VUNLIKELY ((discovery_viewer && discovery_viewer->is_ready_to_quit()) || parser_loop->is_ready_to_quit()) {
        force_update = false;
        return;
      }

      {
        const google::protobuf::Descriptor* descriptor = root_msg->GetDescriptor();
        const google::protobuf::Reflection* reflection = root_msg->GetReflection();

        if VUNLIKELY (!descriptor || !reflection) {
          force_update = false;
          return;
        }

        for (int i = 0; i < descriptor->field_count(); ++i) {
          const google::protobuf::FieldDescriptor* field = descriptor->field(i);

          if (!filter_list.empty()) {
            bool skip = black_mode ? false : true;

#if GOOGLE_PROTOBUF_VERSION >= 6030000
            std::string left_str = std::string(field->name());
#else
            std::string left_str = field->name();
#endif
            std::transform(left_str.begin(), left_str.end(), left_str.begin(), [](char& c) { return std::tolower(c); });
            for (const auto& f : filter_list) {
              if (f.empty()) {
                continue;
              }

              std::string right_str = f;
              std::transform(right_str.begin(), right_str.end(), right_str.begin(),
                             [](char& c) { return std::tolower(c); });

              if (left_str.find(right_str) != std::string::npos) {
                skip = black_mode ? true : false;
                break;
              }
            }

            if (skip) {
              reflection->ClearField(root_msg, field);
            }
          }

          if (field->type() == google::protobuf::FieldDescriptor::TYPE_BYTES) {
            reflection->ClearField(root_msg, field);
          }

          if (ignore_string && field->type() == google::protobuf::FieldDescriptor::TYPE_STRING) {
            reflection->ClearField(root_msg, field);
          }

          if (ignore_array && field->is_repeated()) {
            reflection->ClearField(root_msg, field);
          }
        }
      }
    } else {
      {
        std::unique_lock lock(bytes_mtx);

        const auto target_zerocopy_type = vlink::zerocopy::MessageParser::detect_type(target_ser);
        vlink::zerocopy::MessageParser message_parser;
        bool zerocopy_parse_succeeded = false;

        if (!is_blob_type && !is_text_type && !current_bytes.empty() &&
            target_zerocopy_type != vlink::zerocopy::MessageParser::Type::kUnknown) {
          zerocopy_parse_succeeded = message_parser.parse(target_zerocopy_type, current_bytes);
        }

        if (is_blob_type) {
          print_str.clear();

          int max_line_chars = target_terminal_size.first - 2;

          if (max_line_chars < 2) {
            max_line_chars = 2;
          }

          int per_line = (max_line_chars + 1) / 3;

          if (per_line > 50) {
            per_line = 50;
          }

          if (per_line >= 10) {
            per_line = (per_line / 10) * 10;
          }

          if (per_line <= 0) {
            per_line = 10;
          }

          const size_t per_rows = current_bytes.empty() ? 0
                                                        : (current_bytes.size() + static_cast<size_t>(per_line) - 1) /
                                                              static_cast<size_t>(per_line);

          print_str += std::string("per_line: ") + std::to_string(per_line) + "\n";
          print_str += std::string("per_rows: ") + std::to_string(per_rows) + "\n";
          print_str += std::string("data_size: ") + std::to_string(current_bytes.size()) + "\n";
          print_str += std::string("data_blob:\n");

          const auto hex_str = vlink::Bytes::convert_to_hex_str(current_bytes.data(), current_bytes.size());
          size_t pos = 0;

          for (size_t i = 0; i < per_rows; ++i) {
            const size_t remain_bytes = current_bytes.size() - i * static_cast<size_t>(per_line);
            const size_t line_bytes = std::min<size_t>(static_cast<size_t>(per_line), remain_bytes);
            const size_t line_chars = line_bytes == 0 ? 0 : line_bytes * 3 - 1;

            print_str.append(hex_str, pos, line_chars);
            print_str += "\n";

            pos += line_chars;

            if (pos < hex_str.size() && hex_str[pos] == ' ') {
              ++pos;
            }
          }
        } else if (is_text_type) {
          std::string text_payload = current_bytes.to_string();

          if (!format_json_text(text_payload, print_str)) {
            print_str = std::move(text_payload);
          }
        } else if VUNLIKELY (current_bytes.empty()) {
          force_update = false;
          return;
        } else if (target_zerocopy_type != vlink::zerocopy::MessageParser::Type::kUnknown) {
          if VUNLIKELY (!zerocopy_parse_succeeded) {
            std::cerr << "Failed to parse " << vlink::zerocopy::MessageParser::type_name(target_zerocopy_type)
                      << " message." << std::endl;
            quit_function(0);

            parse_ret = 1;

            force_update = false;
            return;
          }

          vlink::zerocopy::MessageFormatOptions format_options;
          format_options.hex = print_hex_string;
          format_options.date = print_time_string;
          format_options.enum_name = print_enum_string;
          format_options.expand_arrays = !ignore_array;

          bool format_truncated = false;
          print_str = vlink::zerocopy::format_message(message_parser, format_options, &format_truncated);

          if (format_truncated) {
            is_out_of_range = true;
          }

        } else {
          std::cerr << "Unsupported type." << std::endl;
          quit_function(0);

          parse_ret = 1;

          force_update = false;
          return;
        }
      }
    }

    if VUNLIKELY ((discovery_viewer && discovery_viewer->is_ready_to_quit()) || parser_loop->is_ready_to_quit()) {
      force_update = false;
      return;
    }

    if (is_paused && !force_update) {
      VLINK_TERM_OUT << "\033[H";
      VLINK_TERM_OUT.flush();

      total_page = print_list.size();

      if (current_page > total_page - 1) {
        current_page = total_page - 1;
      }

      if (current_page < 0) {
        current_page = 0;
      }

      if (!print_list.empty() && !line_list.empty()) {
        current_str = print_list.at(current_page);
        current_line = line_list.at(current_page);
      }

      VLINK_TERM_OUT << "\033[K";

      VLINK_TERM_OUT << "\033[33m"
                     << "Message Parsed by vlink-eproto (Paused):"
                     << "\033[0m" << std::endl;

      redraw_url_title(show_data_dot, false);
      is_url_title_with_dot = show_data_dot;
      mark_url_title_rendered(show_data_dot);

      if (!current_str.empty()) {
        VLINK_TERM_OUT << "\033[32m";

        auto print_split_view_list = vlink::Helpers::split_view(current_str, '\n');

        for (size_t i = 0; i < print_split_view_list.size(); ++i) {
          VLINK_TERM_OUT << "\033[K";
          VLINK_TERM_OUT << print_split_view_list[i];

          if (i < print_split_view_list.size() - 1) {
            VLINK_TERM_OUT << "\n";

            if (i > 0 && i % kFlushMinLine == 0) {
              VLINK_TERM_OUT.flush();
              if constexpr (kFlushMinSleep > 0) {
                std::this_thread::sleep_for(std::chrono::microseconds(kFlushMinSleep));
              }
            }
          }
        }

        VLINK_TERM_OUT << "\033[0m";
        VLINK_TERM_OUT << "\033[K";
        VLINK_TERM_OUT << std::endl;
      }

      VLINK_TERM_OUT.flush();

    } else {
      if (is_proto_type) {
        if (use_json_format) {
#ifdef VLINK_HAS_PROTOBUF_JSON_UTIL
          std::string json_error;

          if VUNLIKELY (!convert_proto_to_json(print_str, root_msg, &json_error)) {
            std::cerr << "Failed to convert Protobuf message to JSON.";
            if (!json_error.empty()) {
              std::cerr << " " << json_error;
            }
            std::cerr << std::endl;
            quit_function(0);
            parse_ret = 1;
            force_update = false;
            return;
          }
#endif
        } else if VUNLIKELY (!convert_proto_to_txt(print_str, root_msg)) {
          std::cerr << "Failed to convert Protobuf message to text." << std::endl;
          quit_function(0);
          parse_ret = 1;
          force_update = false;
          return;
        }
      }

      if VUNLIKELY ((discovery_viewer && discovery_viewer->is_ready_to_quit()) || parser_loop->is_ready_to_quit()) {
        force_update = false;
        return;
      }

      if VUNLIKELY (print_str.size() > max_str_count) {
        VLINK_TERM_OUT << "\033[H\033[J";
        VLINK_TERM_OUT.flush();
        std::cerr << "The Message is too large to display." << std::endl;
        std::cerr.flush();

        force_update = false;
        return;
      }

      auto split_str_list = vlink::Helpers::split(print_str, '\n');

      if VUNLIKELY ((discovery_viewer && discovery_viewer->is_ready_to_quit()) || parser_loop->is_ready_to_quit()) {
        force_update = false;
        return;
      }

      std::string page_str;
      int line_count = 0;

      print_list.clear();
      line_list.clear();

      print_list.reserve(split_str_list.size() + 5);
      line_list.reserve(split_str_list.size() + 5);

      for (auto& str : split_str_list) {
        if (static_cast<int64_t>(str.size()) > terminal_size.first) {
          str = str.substr(0, terminal_size.first);
        }

        page_str += (str + "\n");
        ++line_count;

        if (line_count >= terminal_size.second - 3) {
          if (!page_str.empty()) {
            page_str.pop_back();
          }
          print_list.emplace_back(page_str);
          line_list.emplace_back(line_count);
          page_str.clear();
          line_count = 0;
        }
      }

      if (!page_str.empty()) {
        page_str.pop_back();
      }

      if (!page_str.empty()) {
        print_list.emplace_back(page_str);
        line_list.emplace_back(line_count);
      }

      total_page = std::min(print_list.size(), static_cast<size_t>(5000));

      if (current_page > total_page - 1) {
        current_page = total_page - 1;
      }

      if (current_page < 0) {
        current_page = 0;
      }

      if (!print_list.empty() && !line_list.empty()) {
        current_str = print_list.at(current_page);
        current_line = line_list.at(current_page);
      }

      VLINK_TERM_OUT << "\033[H\033[K";

      if (is_paused) {
        VLINK_TERM_OUT << "\033[33m"
                       << "Message Parsed by vlink-eproto (Paused):"
                       << "\033[0m" << std::endl;
      } else {
        VLINK_TERM_OUT << "Message Parsed by vlink-eproto:" << std::endl;
      }

      redraw_url_title(show_data_dot, false);
      is_url_title_with_dot = show_data_dot;
      mark_url_title_rendered(show_data_dot);
      VLINK_TERM_OUT.flush();

      if (!current_str.empty()) {
        if (!current_str.empty()) {
          VLINK_TERM_OUT << "\033[32m";

          auto print_split_view_list = vlink::Helpers::split_view(current_str, '\n');

          for (size_t i = 0; i < print_split_view_list.size(); ++i) {
            VLINK_TERM_OUT << "\033[K";
            VLINK_TERM_OUT << print_split_view_list[i];

            if (i < print_split_view_list.size() - 1) {
              VLINK_TERM_OUT << "\n";

              if (i > 0 && i % kFlushMinLine == 0) {
                VLINK_TERM_OUT.flush();
                if constexpr (kFlushMinSleep > 0) {
                  std::this_thread::sleep_for(std::chrono::microseconds(kFlushMinSleep));
                }
              }
            }
          }

          VLINK_TERM_OUT << "\033[0m";
          VLINK_TERM_OUT << "\033[K";
          VLINK_TERM_OUT << std::endl;
        }
      }
    }

    if (current_line < terminal_size.second - 3) {
      for (int i = 0; i < terminal_size.second - 3 - current_line; ++i) {
        VLINK_TERM_OUT << "\033[K";
        VLINK_TERM_OUT << std::endl;
      }
    }

    std::string last_line_str;

    last_line_str = std::string("\033[44;37;1m") + std::string("<") +
                    (total_page == 0 ? std::string("0") : std::to_string(current_page + 1)) + std::string("/") +
                    ((total_page >= 5000 || is_out_of_range) ? std::string("5000+") : std::to_string(total_page)) +
                    std::string(">") + std::string("\033[0m [ ");

    if (print_enum_string) {
      last_line_str += "\033[4mE\033[0m ";
    } else {
      last_line_str += "\033[0mE\033[0m ";
    }

    if (ignore_array) {
      last_line_str += "\033[4mR\033[0m ";
    } else {
      last_line_str += "\033[0mR\033[0m ";
    }

    if (ignore_string) {
      last_line_str += "\033[4mT\033[0m ";
    } else {
      last_line_str += "\033[0mT\033[0m ";
    }

    if (print_time_string) {
      last_line_str += "\033[4mY\033[0m ";
    } else {
      last_line_str += "\033[0mY\033[0m ";
    }

    if (print_hex_string) {
      last_line_str += "\033[4mU\033[0m ";
    } else {
      last_line_str += "\033[0mU\033[0m ";
    }

    if (ignore_default) {
      last_line_str += "\033[4mO\033[0m ";
    } else {
      last_line_str += "\033[0mO\033[0m ";
    }

    if (use_long_repeated) {
      last_line_str += "\033[4mP\033[0m ";
    } else {
      last_line_str += "\033[0mP\033[0m ";
    }

    last_line_str += std::string("] ");

    VLINK_TERM_OUT << "\033[K";

    if VLIKELY (last_line_str.size() <= static_cast<size_t>(terminal_size.first + 69)) {
      VLINK_TERM_OUT << last_line_str;
    } else {
      VLINK_TERM_OUT << last_line_str.substr(0, terminal_size.first + 69);
    }

    VLINK_TERM_OUT.flush();

    is_changed = false;
    is_out_of_range = false;
    force_update = false;
  };

  auto listen_bytes_function = [&parser_loop, &update_terminal_function, &current_bytes, &bytes_mtx, &has_new_data,
                                &frame_seq, &last_frame_timestamp_ms](const vlink::Bytes& bytes) {
    if VUNLIKELY (has_quit) {
      return;
    }

    std::lock_guard lock(bytes_mtx);

    if VLIKELY (!is_paused) {
      current_bytes = bytes;
      has_new_data = true;
      ++frame_seq;
      last_frame_timestamp_ms.store(vlink::ElapsedTimer::get_cpu_timestamp(vlink::ElapsedTimer::kMilli));
      is_changed = true;
      if (!has_printed) {
        has_printed = true;
        parser_loop->post_task(update_terminal_function);
      }
    }
  };

  bool should_use_getter = use_getter || (!has_explicit_ser && ((target_type & vlink::kSetter) != 0));

  try {
    if (should_use_getter) {
      raw_getter.emplace(std::make_shared<RawGetter>(url, vlink::InitType::kWithoutInit));

      if (native_mode) {
        (*raw_getter)->set_property("dds.ip", "127.0.0.1");
      }

      (*raw_getter)->set_ser_type(target_ser, target_schema_type);
      (*raw_getter)->init();
      (*raw_getter)->listen(listen_bytes_function);
    } else {
      raw_sub.emplace(std::make_shared<RawSub>(url, vlink::InitType::kWithoutInit));

      if (native_mode) {
        (*raw_sub)->set_property("dds.ip", "127.0.0.1");
      }

      (*raw_sub)->set_ser_type(target_ser, target_schema_type);
      (*raw_sub)->init();
      (*raw_sub)->listen(listen_bytes_function);
    }
  } catch (vlink::Exception::RuntimeError& e) {
    std::cerr << e.what() << std::endl;
    has_quit = true;
    return -1;
  }

  auto reset_frame_rate_state = [&has_new_data, &frame_seq, &frame_seq_buffer, &current_frame_rate, &current_rate_color,
                                 &last_frame_timestamp_ms]() {
    has_new_data = false;
    frame_seq.store(0);
    frame_seq_buffer.clear();
    current_frame_rate = 0.0;
    current_rate_color = 33;

    if (last_frame_timestamp_ms.load() == 0) {
      last_frame_timestamp_ms.store(vlink::ElapsedTimer::get_cpu_timestamp(vlink::ElapsedTimer::kMilli));
    }
  };

  vlink::Utils::start_detect_keyboard(
      [&parser_loop, &update_terminal_function, &quit_function, &reset_frame_rate_state](const std::string& key) {
        // VLINK_TERM_OUT << "key:" << key << std::endl;

        if (key == "q" || key == "esc") {
          quit_function(0);
        } else if (key == " ") {
          if (is_paused) {
            is_paused = false;
            is_changed = true;
            force_update = true;
            parser_loop->post_task(update_terminal_function);
          } else {
            is_paused = true;
            parser_loop->post_task([&reset_frame_rate_state, &update_terminal_function]() {
              reset_frame_rate_state();
              is_changed = true;
              force_update = true;
              update_terminal_function();
            });
          }
        } else if (key == "left") {
          if (current_page >= 1) {
            --current_page;
            is_changed = true;
            parser_loop->post_task(update_terminal_function);
          }
        } else if (key == "right") {
          if (current_page < total_page - 1) {
            ++current_page;
            is_changed = true;
            parser_loop->post_task(update_terminal_function);
          }
        } else if (key == "up") {
          if (current_page >= 10) {
            current_page -= 10;
            is_changed = true;
            parser_loop->post_task(update_terminal_function);
          } else if (current_page >= 1) {
            current_page = 0;
            is_changed = true;
            parser_loop->post_task(update_terminal_function);
          }
        } else if (key == "down") {
          if (current_page < total_page - 10) {
            current_page += 10;
            is_changed = true;
            parser_loop->post_task(update_terminal_function);
          } else if (current_page < total_page - 1) {
            current_page = total_page - 1;
            is_changed = true;
            parser_loop->post_task(update_terminal_function);
          }
        } else if (key == "pgup") {
          if (current_page >= 100) {
            current_page -= 100;
            is_changed = true;
            parser_loop->post_task(update_terminal_function);
          } else if (current_page >= 1) {
            current_page = 0;
            is_changed = true;
            parser_loop->post_task(update_terminal_function);
          }
        } else if (key == "pgdown") {
          if (current_page < total_page - 100) {
            current_page += 100;
            is_changed = true;
            parser_loop->post_task(update_terminal_function);
          } else if (current_page < total_page - 1) {
            current_page = total_page - 1;
            is_changed = true;
            parser_loop->post_task(update_terminal_function);
          }
        } else if (key == "home") {
          if (current_page != 0) {
            current_page = 0;
            is_changed = true;
            parser_loop->post_task(update_terminal_function);
          }
        } else if (key == "end") {
          if (current_page != total_page - 1) {
            current_page = total_page - 1;
            is_changed = true;
            parser_loop->post_task(update_terminal_function);
          }
        } else if (key == "e") {
          print_enum_string = !print_enum_string;
          is_changed = true;
          force_update = true;
          parser_loop->post_task(update_terminal_function);
        } else if (key == "r") {
          ignore_array = !ignore_array;
          is_changed = true;
          force_update = true;
          parser_loop->post_task(update_terminal_function);
        } else if (key == "t") {
          ignore_string = !ignore_string;
          is_changed = true;
          force_update = true;
          parser_loop->post_task(update_terminal_function);
        } else if (key == "y") {
          print_time_string = !print_time_string;
          is_changed = true;
          force_update = true;
          parser_loop->post_task(update_terminal_function);
        } else if (key == "u") {
          print_hex_string = !print_hex_string;
          is_changed = true;
          force_update = true;
          parser_loop->post_task(update_terminal_function);
        } else if (key == "o") {
          ignore_default = !ignore_default;
          is_changed = true;
          force_update = true;
          parser_loop->post_task(update_terminal_function);
        } else if (key == "p") {
          use_long_repeated = !use_long_repeated;
          is_changed = true;
          force_update = true;
          parser_loop->post_task(update_terminal_function);
        }
      });

  vlink::Timer terminal_timer;
  terminal_timer.set_interval(kTerminalInterval);
  terminal_timer.set_loop_count(vlink::Timer::kInfinite);
  terminal_timer.attach(parser_loop.get());
  terminal_timer.set_callback(update_terminal_function);
  terminal_timer.start();

  vlink::Timer stats_timer;
  stats_timer.set_interval(kCollectInterval);
  stats_timer.set_loop_count(vlink::Timer::kInfinite);
  stats_timer.attach(parser_loop.get());
  stats_timer.set_callback(
      [&frame_seq, &frame_seq_buffer, &current_frame_rate, &current_rate_color, &last_frame_timestamp_ms]() {
        double freq = 0;
        int weight = 1;
        int total_weight = 0;

        if VUNLIKELY (!has_printed) {
          return;
        }

        frame_seq_buffer.emplace_back(frame_seq.exchange(0));
        uint64_t now_ms = vlink::ElapsedTimer::get_cpu_timestamp(vlink::ElapsedTimer::kMilli);
        uint64_t last_frame_ms = last_frame_timestamp_ms.load();

        while (frame_seq_buffer.size() > kCounterCache) {
          frame_seq_buffer.pop_front();
        }

        for (auto seq : frame_seq_buffer) {
          freq += seq * weight;
          total_weight += weight;
          weight *= kCounterWeight;
        }

        if (total_weight > 0) {
          current_frame_rate = freq / total_weight;
        } else {
          current_frame_rate = 0.0;
        }

        if (frame_seq_buffer.back() > 0 && frame_seq_buffer.size() >= kCounterCache) {
          current_rate_color = 32;
        } else if (last_frame_ms > 0 && (now_ms - last_frame_ms) > kCollectInterval * kCounterCache) {
          current_frame_rate = 0.0;
          frame_seq_buffer.clear();
          current_rate_color = 31;
        } else {
          current_rate_color = 33;
        }
      });
  stats_timer.start();

  vlink::Timer::call_once(parser_loop.get(), 250, [&parser_loop, &update_terminal_function]() {
    if VUNLIKELY (has_quit) {
      return;
    }

    if VLIKELY (!has_printed) {
      force_update = true;
      is_changed = true;
      parser_loop->post_task(update_terminal_function);
    }
  });

  if (discovery_viewer) {
    discovery_viewer->wait_for_quit();
  }

  parser_loop->wait_for_quit();

  has_quit = true;

  vlink::Utils::stop_detect_keyboard();

  if (parse_ret == 0) {
    // VLINK_TERM_OUT << std::endl;
    VLINK_TERM_OUT << "\033[H\033[J";
    VLINK_TERM_OUT.flush();
  }

  delete root_msg;
  root_msg = nullptr;

  raw_sub.reset();
  raw_getter.reset();
  factory.reset();
  importer.reset();
  source_tree.reset();
  discovery_viewer.reset();
  parser_loop.reset();

  return parse_ret;
}
#endif

int main(int argc, char* argv[]) {
  std::ios::sync_with_stdio(false);
  vlink::Utils::set_console_utf8_output();

  VLINK_TERM_OUT.init();

#ifdef VLINK_HAS_PROTOBUF_COMPILER
  // init
  vlink::Logger::set_console_level(vlink::Logger::kOff);
  vlink::Logger::set_file_level(vlink::Logger::kOff);
  vlink::Logger::init("vlink-eproto");

  // env
  vlink::Utils::unset_env("VLINK_BAG_PATH");
  // vlink::Utils::set_env("VLINK_DISCOVER_DISABLE", "1");

  // intra_bind
  std::string intra_bind = vlink::Utils::get_env("VLINK_INTRA_BIND");

  if (!intra_bind.empty()) {
    has_intra_bind = true;
  }

  // arg parser
  argparse::ArgumentParser program("vlink-eproto", VLINK_VERSION, argparse::default_arguments::all);

  program.add_description("Note: You may need to add multicast/broadcast [" +
                          vlink::DiscoveryViewer::get_listen_address() + "]");

  argparse::ArgumentParser pub_command("pub", VLINK_VERSION, argparse::default_arguments::help);
  pub_command.add_argument("url").help("Bind url").required();
  pub_command.add_argument("-d", "--proto_dir").help("Proto dir").default_value(std::string());
  pub_command.add_argument("--schema_plugin").help("Path to schema plugin shared library").default_value(std::string());
  pub_command.add_argument("-s", "--ser_type").help("Serialization type").default_value(std::string());
  pub_command.add_argument("-x", "--encoding")
      .help("Encoding (protobuf/flatbuffers/raw/blob/zerocopy)")
      .default_value(std::string());
  pub_command.add_argument("-n", "--native").help("Native mode").default_value(false).implicit_value(true);
  pub_command.add_argument("-j", "--json")
      .help("Use JSON for protobuf input")
      .default_value(false)
      .implicit_value(true);
  pub_command.add_argument("-f", "--prototxt_file").help("Proto txt / JSON file").default_value(std::string());
  pub_command.add_argument("-c", "--prototxt_content").help("Proto txt / JSON content").default_value(std::string());

  pub_command.add_argument("-t", "--times")
      .help("Pub times, times <= 0 means infinite")
      .scan<'d', int>()
      // NOLINTNEXTLINE(readability-redundant-casting)
      .default_value(static_cast<int>(1));
  pub_command.add_argument("-l", "--interval")
      .help("Pub interval(ms)")
      .scan<'d', int>()
      // NOLINTNEXTLINE(readability-redundant-casting)
      .default_value(static_cast<int>(100));

  pub_command.add_description("Publish data");

  std::string pub_example_str =
      "Example:\n  vlink-eproto pub shm://test -d /home/proto_dir -s pb.Test -f test.prototxt";
  pub_example_str += "\n  ";
  pub_example_str += "vlink-eproto pub shm://test -d /home/proto_dir -s pb.Test -c 'width:800 height:600'";
  pub_example_str += "\n  ";
  pub_example_str += R"(vlink-eproto pub shm://test -d /home/proto_dir -s pb.Test -j -c '{"width":800,"height":600}')";

  pub_command.add_epilog(std::move(pub_example_str));

  argparse::ArgumentParser sub_command("sub", VLINK_VERSION, argparse::default_arguments::help);
  sub_command.add_argument("url").help("Bind url").required();
  sub_command.add_argument("-d", "--proto_dir").help("Proto dir").default_value(std::string());
  sub_command.add_argument("--schema_plugin").help("Path to schema plugin shared library").default_value(std::string());
  sub_command.add_argument("-s", "--ser_type").help("Serialization type").default_value(std::string());
  sub_command.add_argument("-x", "--encoding")
      .help("Encoding (protobuf/flatbuffers/raw/blob/zerocopy, blob prints binary bytes as hex)")
      .default_value(std::string());
  sub_command.add_argument("-i", "--filter")
      .help("Property filter list, comma-separated or quoted space-separated")
      .default_value(std::string());
  sub_command.add_argument("-j", "--json")
      .help("Print protobuf message as JSON")
      .default_value(false)
      .implicit_value(true);
  sub_command.add_argument("-g", "--getter")
      .help("Use getter to receive data")
      .default_value(false)
      .implicit_value(true);
  sub_command.add_argument("-k", "--black").help("Blacklist mode").default_value(false).implicit_value(true);
  sub_command.add_argument("-n", "--native").help("Native mode").default_value(false).implicit_value(true);
  sub_command.add_argument("-m", "--max_str_count")
      .help("Max string count")
      .scan<'u', uint64_t>()
      // NOLINTNEXTLINE(readability-redundant-casting)
      .default_value(static_cast<uint64_t>(1000'00000UL));

  sub_command.add_argument("-e", "--print_enum_string")
      .help("Print enum number (Hot key)")
      .default_value(false)
      .implicit_value(true);
  sub_command.add_argument("-r", "--ignore_array")
      .help("Ignore array (Hot key)")
      .default_value(false)
      .implicit_value(true);
  sub_command.add_argument("-t", "--ignore_string")
      .help("Ignore string (Hot key)")
      .default_value(false)
      .implicit_value(true);
  sub_command.add_argument("-y", "--print_time_string")
      .help("Print time string (Hot key)")
      .default_value(false)
      .implicit_value(true);
  sub_command.add_argument("-u", "--print_hex_string")
      .help("Print hex string (Hot key)")
      .default_value(false)
      .implicit_value(true);
  sub_command.add_argument("-o", "--ignore_default")
      .help("Ignore default value (Hot key)")
      .default_value(false)
      .implicit_value(true);
  sub_command.add_argument("-p", "--use_long_repeated")
      .help("Use long repeated (Hot key)")
      .default_value(false)
      .implicit_value(true);

  sub_command.add_argument("--rows")
      .help("Maximum rows(0 means automatic)")
      .scan<'d', int>()
      // NOLINTNEXTLINE(readability-redundant-casting)
      .default_value(static_cast<int>(0));
  sub_command.add_argument("--columns")
      .help("Maximum columns(0 means automatic)")
      .scan<'d', int>()
      // NOLINTNEXTLINE(readability-redundant-casting)
      .default_value(static_cast<int>(0));

  sub_command.add_description("Subscribe data");

  sub_command.add_epilog(
      "Example:\n  vlink-eproto sub shm://test -d /home/proto_dir -s pb.Test\n  "
      "vlink-eproto sub shm://test -d /home/proto_dir -s pb.Test -j");

  argparse::ArgumentParser import_command("import", VLINK_VERSION, argparse::default_arguments::help);
  import_command.add_argument("dir").help("Proto dir path to persist").required();
  import_command.add_description("Save proto dir to $HOME/.vlink_proto_dir");
  import_command.add_epilog("Example:\n  vlink-eproto import /home/proto_dir");

  program.add_subparser(pub_command);
  program.add_subparser(sub_command);
  program.add_subparser(import_command);

  try {
    program.parse_args(argc, argv);
  } catch (const std::exception& e) {
    std::cerr << e.what() << std::endl;

    if (program.is_subcommand_used("pub")) {
      std::cerr << pub_command << std::endl;
    } else if (program.is_subcommand_used("sub")) {
      std::cerr << sub_command << std::endl;
    } else if (program.is_subcommand_used("import")) {
      std::cerr << import_command << std::endl;
    }

    return 1;
  }

  int ret = 0;
  std::string proto_dir;

  if (program.is_subcommand_used("pub")) {
    const auto& url = pub_command.get<std::string>("url");
    proto_dir = pub_command.get<std::string>("-d");
    auto schema_plugin_path = pub_command.get<std::string>("--schema_plugin");

    if (proto_dir.empty()) {
      proto_dir = vlink::Utils::get_env("VLINK_PROTO_DIR");
    }

    if (proto_dir.empty()) {
      proto_dir = read_home_config(".vlink_proto_dir");
    }

    if (schema_plugin_path.empty()) {
      schema_plugin_path = vlink::Utils::get_env("VLINK_SCHEMA_PLUGIN");
    }

#ifdef _WIN32

    if (pub_command.is_used("-d")) {
      try {
        proto_dir = vlink::Helpers::path_to_string(std::filesystem::path(proto_dir));
      } catch (std::filesystem::filesystem_error&) {
      }
    }

    if (pub_command.is_used("--schema_plugin")) {
      try {
        schema_plugin_path = vlink::Helpers::path_to_string(std::filesystem::path(schema_plugin_path));
      } catch (std::filesystem::filesystem_error&) {
      }
    }
#endif

    auto prototxt_file = pub_command.get<std::string>("-f");
    auto prototxt_content = pub_command.get<std::string>("-c");
    const auto& ser = pub_command.get<std::string>("-s");
    auto encoding = pub_command.get<std::string>("-x");
    std::transform(encoding.begin(), encoding.end(), encoding.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    auto use_blob_encoding = encoding == "blob";
    auto schema_type = vlink::SchemaData::convert_encoding(encoding);

    auto native_mode = pub_command.is_used("-n");
    auto use_json_format = pub_command.is_used("-j");

    int times = pub_command.get<int>("-t");
    int interval = pub_command.get<int>("-l");

    if VUNLIKELY (prototxt_file.empty() && prototxt_content.empty()) {
      std::cerr << "One of prototxt_file and prototxt_content must be specified." << std::endl;
      return -1;
    } else if VUNLIKELY (!prototxt_file.empty() && !prototxt_content.empty()) {
      std::cerr << "One of prototxt_file and prototxt_content must be specified." << std::endl;
      return -1;
    } else if VUNLIKELY (schema_type == vlink::SchemaType::kUnknown && !encoding.empty() && encoding != "unknown") {
      std::cerr << "Invalid encoding." << std::endl;
      return -1;
    }

#ifdef _WIN32
    std::replace(proto_dir.begin(), proto_dir.end(), '\\', '/');
    std::replace(schema_plugin_path.begin(), schema_plugin_path.end(), '\\', '/');

    if (!prototxt_file.empty()) {
      std::replace(prototxt_file.begin(), prototxt_file.end(), '\\', '/');
    }
#endif

    if (!proto_dir.empty() && proto_dir.back() == '/') {
      proto_dir.pop_back();
    }

    if (!schema_plugin_path.empty() && schema_plugin_path.back() == '/') {
      schema_plugin_path.pop_back();
    }

    if (!prototxt_file.empty()) {
      if (prototxt_file.back() == '/') {
        prototxt_file.pop_back();
      }
    }

    (void)vlink::SchemaPluginManager::get(schema_plugin_path);
    ret = start_eproto_pub(url, proto_dir, prototxt_file, prototxt_content, ser, schema_type, use_blob_encoding,
                           native_mode, times, interval, use_json_format);

    return ret;

  } else if (program.is_subcommand_used("sub")) {
    const auto& url = sub_command.get<std::string>("url");
    const auto& ser = sub_command.get<std::string>("-s");
    auto encoding = sub_command.get<std::string>("-x");
    std::transform(encoding.begin(), encoding.end(), encoding.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    auto use_blob_encoding = encoding == "blob";
    auto schema_type = vlink::SchemaData::convert_encoding(encoding);
    proto_dir = sub_command.get<std::string>("-d");
    auto schema_plugin_path = sub_command.get<std::string>("--schema_plugin");

    if (proto_dir.empty()) {
      proto_dir = vlink::Utils::get_env("VLINK_PROTO_DIR");
    }

    if (proto_dir.empty()) {
      proto_dir = read_home_config(".vlink_proto_dir");
    }

    if (schema_plugin_path.empty()) {
      schema_plugin_path = vlink::Utils::get_env("VLINK_SCHEMA_PLUGIN");
    }

#ifdef _WIN32

    if (sub_command.is_used("-d")) {
      try {
        proto_dir = vlink::Helpers::path_to_string(std::filesystem::path(proto_dir));
      } catch (std::filesystem::filesystem_error&) {
      }
    }

    if (sub_command.is_used("--schema_plugin")) {
      try {
        schema_plugin_path = vlink::Helpers::path_to_string(std::filesystem::path(schema_plugin_path));
      } catch (std::filesystem::filesystem_error&) {
      }
    }
#endif

    auto native_mode = sub_command.is_used("-n");
    auto filter = sub_command.get<std::string>("-i");
    auto use_json_format = sub_command.is_used("-j");
    auto use_getter = sub_command.is_used("-g");

    if VUNLIKELY (schema_type == vlink::SchemaType::kUnknown && !encoding.empty() && encoding != "unknown") {
      std::cerr << "Invalid encoding." << std::endl;
      return -1;
    }

    black_mode = sub_command.is_used("-k");

    print_enum_string = sub_command.is_used("-e");
    ignore_array = sub_command.is_used("-r");
    ignore_string = sub_command.is_used("-t");
    print_time_string = sub_command.is_used("-y");
    print_hex_string = sub_command.is_used("-u");
    ignore_default = sub_command.is_used("-o");
    use_long_repeated = sub_command.is_used("-p");

    max_str_count = sub_command.get<uint64_t>("-m");

    max_rows = sub_command.get<int>("--rows");

    max_columns = sub_command.get<int>("--columns");

#ifdef _WIN32
    std::replace(proto_dir.begin(), proto_dir.end(), '\\', '/');
    std::replace(schema_plugin_path.begin(), schema_plugin_path.end(), '\\', '/');
#endif

    if (!proto_dir.empty() && proto_dir.back() == '/') {
      proto_dir.pop_back();
    }

    if (!schema_plugin_path.empty() && schema_plugin_path.back() == '/') {
      schema_plugin_path.pop_back();
    }

    (void)vlink::SchemaPluginManager::get(schema_plugin_path);

    VLINK_TERM_OUT << "\033[?25l";
    VLINK_TERM_OUT.flush();

    ret = start_eproto_sub(url, proto_dir, ser, schema_type, use_blob_encoding, native_mode, filter, use_getter,
                           use_json_format);

    VLINK_TERM_OUT << "\033[?25h";
    VLINK_TERM_OUT.flush();

    return ret;
  } else if (program.is_subcommand_used("import")) {
    auto dir = import_command.get<std::string>("dir");

#ifdef _WIN32
    try {
      dir = vlink::Helpers::path_to_string(std::filesystem::path(dir));
    } catch (const std::filesystem::filesystem_error&) {
    } catch (const std::exception&) {
    }

    std::replace(dir.begin(), dir.end(), '\\', '/');
#endif

    while (!dir.empty() && dir.back() == '/') {
      dir.pop_back();
    }

    if VUNLIKELY (dir.empty()) {
      std::cerr << "Proto dir cannot be empty." << std::endl;
      return -1;
    }

    auto input_path = utf8_to_path(dir);

    if VUNLIKELY (input_path.empty()) {
      std::cerr << "Invalid proto dir path: " << dir << "." << std::endl;
      return -1;
    }

    std::error_code ec;
    auto absolute_path = std::filesystem::absolute(input_path, ec);

    if VUNLIKELY (ec) {
      std::cerr << "Invalid proto dir path: " << dir << " (" << ec.message() << ")." << std::endl;
      return -1;
    }

    if VUNLIKELY (!std::filesystem::exists(absolute_path, ec) || ec) {
      std::cerr << "Proto dir does not exist: " << vlink::Helpers::path_to_string(absolute_path) << "." << std::endl;
      return -1;
    }

    if VUNLIKELY (!std::filesystem::is_directory(absolute_path, ec) || ec) {
      std::cerr << "Proto dir is not a directory: " << vlink::Helpers::path_to_string(absolute_path) << "."
                << std::endl;
      return -1;
    }

    auto saved = vlink::Helpers::path_to_string(absolute_path);

#ifdef _WIN32
    std::replace(saved.begin(), saved.end(), '\\', '/');
#endif

    while (saved.size() > 1 && saved.back() == '/') {
      saved.pop_back();
    }

    if VUNLIKELY (saved.empty()) {
      std::cerr << "Failed to resolve absolute path for: " << dir << "." << std::endl;
      return -1;
    }

    auto config_path = get_home_config_path(".vlink_proto_dir");

    if VUNLIKELY (config_path.empty()) {
      std::cerr << "Cannot resolve HOME directory." << std::endl;
      return -1;
    }

    if VUNLIKELY (!write_home_config(".vlink_proto_dir", saved)) {
      std::cerr << "Failed to write " << config_path << "." << std::endl;
      return -1;
    }

    std::cout << "Saved VLINK_PROTO_DIR to " << config_path << ": " << saved << std::endl;

    return 0;
  }

  std::cerr << program << std::endl;

  return 1;
#else
  (void)argc;
  (void)argv;

  std::cerr << "The lower version of protobuf is not supported. Please change to a higher version of protobuf."
            << std::endl;
  return -1;
#endif
}
