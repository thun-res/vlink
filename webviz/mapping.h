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

#include <memory>
#include <mutex>
#include <unordered_map>

#include "./message_view.h"
#include "./webviz_types.h"

#ifdef VLINK_ENABLE_EXPRTK
#include <vlink/external/exprtk_api.h>
#endif

namespace vlink {
namespace webviz {

[[nodiscard]] std::string native_ser(std::string_view converter);

class FieldReader;

class FieldExpression final {
 public:
  explicit FieldExpression(const std::string& expression);
  [[nodiscard]] double evaluate(const FieldReader& fields) const;
  [[nodiscard]] bool valid() const { return valid_; }

 private:
#ifdef VLINK_ENABLE_EXPRTK
  struct Binding final {
    FieldPath path;
    bool size{false};
  };
  std::vector<Binding> paths_;
  mutable std::vector<double> values_;
  ExprtkSymbolTable symbols_;
  ExprtkExpression expression_;
  mutable std::mutex mutex_;
#endif
  bool valid_{false};
};

struct MappedField final {
  FieldPath source;
  std::string target;
  nlohmann::json default_value;
  bool has_default{false};
  uint64_t time_scale{0};
  std::unique_ptr<FieldExpression> expression;
};

struct MessageMapping final {
  std::string ser;
  UrlSelector urls;
  std::string target;
  std::string encoding;
  std::string schema_encoding;
  std::string converter;
  std::string entity_path;
  bool is_static{false};
  FieldPath timestamp;
  uint64_t timestamp_scale{1000};
  std::vector<MappedField> fields;
};

class MappingSet final {
 public:
  MappingSet(const std::vector<std::string>& files, std::string_view target_key);
  [[nodiscard]] bool valid() const { return valid_; }
  void validate(bool (*validator)(const MessageMapping&));
  [[nodiscard]] std::vector<const MessageMapping*> select(std::string_view url, const std::string& ser,
                                                          bool* ambiguous = nullptr) const;
  [[nodiscard]] bool has_converter(std::string_view converter) const;

 private:
  bool valid_{true};
  std::unordered_map<std::string, std::vector<MessageMapping>> mappings_;
};

class FieldReader final {
 public:
  FieldReader(const MessageView& source, const MessageMapping* mapping);
  [[nodiscard]] MessageView source() const { return source_; }
  [[nodiscard]] MessageView source(const FieldPath& path) const;
  [[nodiscard]] size_t index() const { return index_; }
  [[nodiscard]] const MappedField* field(std::string_view target) const;
  [[nodiscard]] bool has_descendant(std::string_view target) const;
  [[nodiscard]] std::vector<size_t> indices(std::string_view target) const;
  [[nodiscard]] FieldValue value(std::string_view target) const;
  [[nodiscard]] double number(std::string_view target, double fallback = 0.0) const;
  [[nodiscard]] uint64_t integer(std::string_view target, uint64_t fallback = 0) const;
  [[nodiscard]] std::string text(std::string_view target, std::string_view fallback = {}) const;
  [[nodiscard]] MessageView view(std::string_view target, std::string_view default_source = {}) const;
  [[nodiscard]] Bytes bytes(std::string_view target) const;
  [[nodiscard]] FieldReader child(const MessageView& source) const;
  [[nodiscard]] FieldReader child(const MessageView& source, size_t index) const;
  [[nodiscard]] int64_t timestamp() const;

 private:
  MessageView source_;
  MessageView root_;
  size_t index_{0};
  const MessageMapping* mapping_{nullptr};
};

[[nodiscard]] bool write_flatbuffer_mapping(const reflection::Schema& schema, const FieldReader& fields,
                                            flatbuffers::FlatBufferBuilder& builder);

}  // namespace webviz
}  // namespace vlink
