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

#include "./flatbuffers_runtime.h"

#ifdef VLINK_ENABLE_VIEWER_FLATBUFFERS

inline const reflection::Field* find_field(const reflection::Object& object, const std::string& name) {
  FlatbuffersObjectView api;
  return api.find_field(object, name);
}

inline std::string get_field_type_name(const reflection::Field& field, const reflection::Schema& schema) {
  FlatbuffersObjectView api;
  return api.get_field_type_name(field, schema);
}

inline bool make_root_view(const FlatbuffersSchemaContext& ctx, const vlink::Bytes& raw, FlatbuffersObjectView& out) {
  FlatbuffersObjectView api;
  return api.make_root_view(ctx, raw, out);
}

inline bool get_child_view(const FlatbuffersObjectView& parent, const reflection::Field& field,
                           const reflection::Schema& schema, FlatbuffersObjectView& out) {
  FlatbuffersObjectView api;
  return api.get_child_view(parent, field, schema, out);
}

inline bool get_vector_elem_view(const FlatbuffersObjectView& parent, const reflection::Field& field, size_t index,
                                 const reflection::Schema& schema, FlatbuffersObjectView& out) {
  FlatbuffersObjectView api;
  return api.get_vector_elem_view(parent, field, index, schema, out);
}

inline size_t get_vector_size(const FlatbuffersObjectView& parent, const reflection::Field& field) {
  FlatbuffersObjectView api;
  return api.get_vector_size(parent, field);
}

inline std::optional<double> get_numeric(const FlatbuffersObjectView& parent, const reflection::Field& field) {
  FlatbuffersObjectView api;
  return api.get_numeric(parent, field);
}

inline std::string get_string(const FlatbuffersObjectView& parent, const reflection::Field& field,
                              const reflection::Schema* schema = nullptr) {
  FlatbuffersObjectView api;
  return api.get_string(parent, field, schema);
}

inline bool get_bytes(const FlatbuffersObjectView& parent, const reflection::Field& field, vlink::Bytes& out) {
  FlatbuffersObjectView api;
  return api.get_bytes(parent, field, out);
}

inline bool is_bytes_field(const reflection::Field& field) {
  FlatbuffersObjectView api;
  return api.is_bytes_field(field);
}

std::optional<double> get_vector_numeric(const FlatbuffersObjectView& parent, const reflection::Field& field,
                                         size_t index);

inline std::string get_vector_string(const FlatbuffersObjectView& parent, const reflection::Field& field,
                                     size_t index) {
  FlatbuffersObjectView api;
  return api.get_vector_string(parent, field, index);
}

inline bool split_indexed_token(const std::string& token, std::string& name, int& index) {
  FlatbuffersObjectView api;
  return api.split_indexed_token(token, name, index);
}

inline bool is_enum_field(const reflection::Field& field, const reflection::Schema& schema) {
  FlatbuffersObjectView api;
  return api.is_enum_field(field, schema);
}

inline int get_fbs_edit_type(reflection::BaseType base_type) {
  FlatbuffersObjectView api;
  return api.get_fbs_edit_type(base_type);
}

inline std::string get_enum_value_name(const reflection::Field& field, const reflection::Schema& schema,
                                       int64_t value) {
  FlatbuffersObjectView api;
  return api.get_enum_value_name(field, schema, value);
}

#endif

// NOLINTEND
