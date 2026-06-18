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

// NOLINTBEGIN

#pragma once

#include <vlink/base/bytes.h>
#include <vlink/impl/types.h>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>

#if __has_include(<flatbuffers/idl.h>)
#include <flatbuffers/flatbuffers.h>
#include <flatbuffers/idl.h>
#include <flatbuffers/reflection.h>
#define VLINK_ENABLE_VIEWER_FLATBUFFERS
#endif

#ifdef VLINK_ENABLE_VIEWER_FLATBUFFERS

struct FlatbuffersSchemaContext final {
  std::shared_ptr<flatbuffers::Parser> parser;
  vlink::Bytes bfbs;
  const reflection::Schema* schema{nullptr};
  const reflection::Object* root_object{nullptr};
  std::string type_name;
  std::string schema_file;

  [[nodiscard]] bool valid() const noexcept {
    return parser != nullptr && schema != nullptr && root_object != nullptr && !bfbs.empty();
  }
};

class FlatbuffersRuntime final {
 public:
  FlatbuffersRuntime() = default;

  [[nodiscard]] bool load_dir(const std::string& dir, std::string* error = nullptr);

  void clear();

  [[nodiscard]] const std::string& dir() const noexcept { return dir_; }

  [[nodiscard]] bool empty() const noexcept { return dir_.empty(); }

  [[nodiscard]] std::shared_ptr<FlatbuffersSchemaContext> find_context(const std::string& ser,
                                                                       std::string* error = nullptr);

  [[nodiscard]] vlink::SchemaData search_schema(const std::string& ser, std::string* error = nullptr);

  [[nodiscard]] std::vector<std::string> type_names() const {
    std::vector<std::string> names;
    names.reserve(context_map_.size());

    for (const auto& [key, context] : context_map_) {
      names.emplace_back(context && !context->type_name.empty() ? context->type_name : key);
    }

    return names;
  }

 private:
  static void import_fbs(std::shared_ptr<FlatbuffersSchemaContext>& ctx, const std::string& target_ser,
                         const std::filesystem::path& root_dir, const std::filesystem::path& sub_dir, bool& has_import,
                         int depth, std::string* error);

  std::string dir_;
  std::unordered_map<std::string, std::shared_ptr<FlatbuffersSchemaContext>> context_map_;
};

enum class FlatbuffersViewKind : uint8_t {
  kTable = 0,
  kStruct = 1,
};

struct FlatbuffersObjectView final {
  FlatbuffersViewKind kind{FlatbuffersViewKind::kTable};
  const void* data{nullptr};
  const reflection::Object* object{nullptr};

  [[nodiscard]] bool valid() const noexcept { return data != nullptr && object != nullptr; }

  [[nodiscard]] const reflection::Field* find_field(const reflection::Object& object, const std::string& name);

  [[nodiscard]] std::string get_field_type_name(const reflection::Field& field, const reflection::Schema& schema);

  [[nodiscard]] bool make_root_view(const FlatbuffersSchemaContext& ctx, const vlink::Bytes& raw,
                                    FlatbuffersObjectView& out);

  [[nodiscard]] bool get_child_view(const FlatbuffersObjectView& parent, const reflection::Field& field,
                                    const reflection::Schema& schema, FlatbuffersObjectView& out);

  [[nodiscard]] bool get_vector_elem_view(const FlatbuffersObjectView& parent, const reflection::Field& field,
                                          size_t index, const reflection::Schema& schema, FlatbuffersObjectView& out);

  [[nodiscard]] size_t get_vector_size(const FlatbuffersObjectView& parent, const reflection::Field& field);

  [[nodiscard]] std::optional<double> get_numeric(const FlatbuffersObjectView& parent, const reflection::Field& field);

  [[nodiscard]] std::string get_string(const FlatbuffersObjectView& parent, const reflection::Field& field,
                                       const reflection::Schema* schema = nullptr);

  [[nodiscard]] bool get_bytes(const FlatbuffersObjectView& parent, const reflection::Field& field, vlink::Bytes& out);

  [[nodiscard]] bool is_bytes_field(const reflection::Field& field);

  [[nodiscard]] std::optional<double> get_vector_numeric(const FlatbuffersObjectView& parent,
                                                         const reflection::Field& field, size_t index);

  [[nodiscard]] std::string get_vector_string(const FlatbuffersObjectView& parent, const reflection::Field& field,
                                              size_t index);

  [[nodiscard]] bool split_indexed_token(const std::string& token, std::string& name, int& index);

  [[nodiscard]] bool is_enum_field(const reflection::Field& field, const reflection::Schema& schema);

  [[nodiscard]] int get_fbs_edit_type(reflection::BaseType base_type);

  [[nodiscard]] std::string get_enum_value_name(const reflection::Field& field, const reflection::Schema& schema,
                                                int64_t value);
};

#endif

// NOLINTEND
