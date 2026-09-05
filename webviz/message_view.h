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

#include <flatbuffers/reflection.h>
#include <google/protobuf/message.h>
#include <vlink/base/bytes.h>
#include <vlink/zerocopy/message_parser.h>

#include <cmath>
#include <limits>
#include <nlohmann/json.hpp>
#include <string>
#include <string_view>
#include <type_traits>
#include <variant>
#include <vector>

namespace vlink {
namespace webviz {

struct FieldStep final {
  std::string name;
  size_t index{0};
  bool indexed{false};
};

using FieldPath = std::vector<FieldStep>;
using FieldValue = std::variant<std::monostate, int64_t, uint64_t, double, bool, std::string, Bytes>;

[[nodiscard]] bool parse_field_path(std::string_view text, FieldPath& path, bool allow_wildcard = false);
[[nodiscard]] double field_number(const FieldValue& value, double fallback = 0.0);
[[nodiscard]] int64_t field_integer(const FieldValue& value, int64_t fallback = 0);
[[nodiscard]] uint64_t field_unsigned(const FieldValue& value, uint64_t fallback = 0);
[[nodiscard]] std::string field_text(const FieldValue& value);

template <typename T>
[[nodiscard]] bool field_numeric(const FieldValue& value, T& result) {
  return std::visit(
      [&](const auto& input) {
        using V = std::decay_t<decltype(input)>;
        if constexpr (!std::is_arithmetic_v<V>) {
          return false;
        } else if constexpr (std::is_integral_v<T>) {
          if constexpr (std::is_floating_point_v<V>) {
            const auto bound = std::ldexp(1.0, std::numeric_limits<T>::digits);
            if (!std::isfinite(input) || std::trunc(input) != input || input >= bound ||
                input < (std::is_signed_v<T> ? -bound : 0)) {
              return false;
            }
          } else {
            if constexpr (std::is_signed_v<V>) {
              if (input < 0) {
                if constexpr (!std::is_signed_v<T>) {
                  return false;
                } else if (input < std::numeric_limits<T>::lowest()) {
                  return false;
                }
              } else if (static_cast<uint64_t>(input) > static_cast<uint64_t>(std::numeric_limits<T>::max())) {
                return false;
              }
            } else if (static_cast<uint64_t>(input) > static_cast<uint64_t>(std::numeric_limits<T>::max())) {
              return false;
            }
          }
          result = static_cast<T>(input);
          return true;
        } else {
          if (std::isfinite(static_cast<double>(input)) &&
              std::abs(static_cast<double>(input)) > std::numeric_limits<T>::max()) {
            return false;
          }
          result = static_cast<T>(input);
          return true;
        }
      },
      value);
}

class MessageView final {
 public:
  MessageView() = default;
  explicit MessageView(const google::protobuf::Message& message);
  explicit MessageView(const nlohmann::json& message);
  explicit MessageView(const zerocopy::MessageParser& message);
  MessageView(const uint8_t* data, const reflection::Schema& schema);

  [[nodiscard]] bool valid() const;
  [[nodiscard]] bool is_array() const;
  [[nodiscard]] bool is_bytes() const;
  [[nodiscard]] bool is_null() const { return !valid() || (kind_ == kJson && json_->is_null()); }
  [[nodiscard]] bool is_flatbuffer() const { return kind_ == kFlatbuffer; }
  [[nodiscard]] bool is_json_array() const { return kind_ == kJson && json_->is_array(); }
  [[nodiscard]] bool is_json_object() const { return kind_ == kJson && json_->is_object(); }
  [[nodiscard]] MessageView member(std::string_view name) const;
  [[nodiscard]] MessageView at(size_t index) const;
  [[nodiscard]] MessageView find(const FieldPath& path) const;
  [[nodiscard]] MessageView find(std::string_view path) const;
  [[nodiscard]] size_t size() const;
  [[nodiscard]] FieldValue value(bool schema_default = false) const;
  [[nodiscard]] Bytes bytes() const;
  [[nodiscard]] bool read_bytes(Bytes& output) const;
  [[nodiscard]] std::string text() const;

 private:
  enum Kind : uint8_t { kEmpty, kProto, kFlatbuffer, kJson, kZeroCopy };

  [[nodiscard]] const google::protobuf::Message* proto_object() const;
  [[nodiscard]] FieldValue fbs_value() const;

  Kind kind_{kEmpty};
  const google::protobuf::Message* proto_{nullptr};
  const google::protobuf::FieldDescriptor* proto_field_{nullptr};
  int proto_index_{-1};
  const nlohmann::json* json_{nullptr};
  const zerocopy::MessageParser* zero_{nullptr};
  std::string zero_path_;
  const reflection::Schema* schema_{nullptr};
  const reflection::Object* object_{nullptr};
  const reflection::Field* fbs_field_{nullptr};
  const uint8_t* data_{nullptr};
  reflection::BaseType base_{reflection::None};
  reflection::BaseType element_{reflection::None};
  size_t fixed_size_{0};
};

}  // namespace webviz
}  // namespace vlink
