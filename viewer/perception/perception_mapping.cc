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

#include "./perception_mapping.h"

#include <flatbuffers/flatbuffers.h>
#include <flatbuffers/reflection.h>
#include <vlink/base/helpers.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <iterator>
#include <numeric>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wcpp"
#endif
#include <google/protobuf/descriptor.h>
#include <google/protobuf/message.h>
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic pop
#endif

//
#ifdef VLINK_ENABLE_EXPRTK
#include <vlink/external/exprtk_api.h>
#endif

#ifdef _WIN32
#undef min
#undef max
#undef GetMessage
#undef ERROR
#endif

namespace perception {
namespace mapping {

struct PathToken final {
  bool is_index{false};
  std::string name;
  uint32_t index{0};
};

bool tokenize_path(const std::string& path, std::vector<PathToken>& out) {
  out.clear();
  std::string current;

  auto flush = [&out, &current]() {
    if (!current.empty()) {
      out.push_back(PathToken{false, current, 0});
      current.clear();
    }
  };

  for (size_t i = 0; i < path.size(); ++i) {
    const char ch = path[i];

    if (ch == '.') {
      flush();
    } else if (ch == '[') {
      flush();
      std::string digits;
      ++i;

      while (i < path.size() && path[i] != ']') {
        digits.push_back(path[i]);
        ++i;
      }

      if (i >= path.size() || path[i] != ']' || digits.empty()) {
        return false;
      }

      for (const char digit : digits) {
        if (digit < '0' || digit > '9') {
          return false;
        }
      }

      out.push_back(PathToken{true, std::string(), static_cast<uint32_t>(std::strtoul(digits.c_str(), nullptr, 10))});
    } else {
      current.push_back(ch);
    }
  }

  flush();
  return !out.empty();
}

const std::vector<PathToken>& tokenize_path_cached(const std::string& path) {
  static thread_local std::unordered_map<std::string, std::vector<PathToken>> cache;

  const auto it = cache.find(path);

  if (it != cache.end()) {
    return it->second;
  }

  std::vector<PathToken> tokens;

  if (!tokenize_path(path, tokens)) {
    tokens.clear();
  }

  return cache.emplace(path, std::move(tokens)).first->second;
}

std::string format_double_string(double value) {
  if (std::isnan(value)) {
    return std::string();
  }

  char buffer[64];
  std::snprintf(buffer, sizeof(buffer), "%g", value);
  return std::string(buffer);
}

bool proto_type_is_numeric(google::protobuf::FieldDescriptor::CppType type) {
  switch (type) {
    case google::protobuf::FieldDescriptor::CPPTYPE_DOUBLE:
    case google::protobuf::FieldDescriptor::CPPTYPE_FLOAT:
    case google::protobuf::FieldDescriptor::CPPTYPE_INT32:
    case google::protobuf::FieldDescriptor::CPPTYPE_INT64:
    case google::protobuf::FieldDescriptor::CPPTYPE_UINT32:
    case google::protobuf::FieldDescriptor::CPPTYPE_UINT64:
    case google::protobuf::FieldDescriptor::CPPTYPE_BOOL:
    case google::protobuf::FieldDescriptor::CPPTYPE_ENUM:
      return true;
    default:
      return false;
  }
}

double proto_field_numeric(const google::protobuf::Message& message, const google::protobuf::FieldDescriptor* field) {
  const auto* ref = message.GetReflection();

  switch (field->cpp_type()) {
    case google::protobuf::FieldDescriptor::CPPTYPE_DOUBLE:
      return ref->GetDouble(message, field);
    case google::protobuf::FieldDescriptor::CPPTYPE_FLOAT:
      return static_cast<double>(ref->GetFloat(message, field));
    case google::protobuf::FieldDescriptor::CPPTYPE_INT32:
      return static_cast<double>(ref->GetInt32(message, field));
    case google::protobuf::FieldDescriptor::CPPTYPE_INT64:
      return static_cast<double>(ref->GetInt64(message, field));
    case google::protobuf::FieldDescriptor::CPPTYPE_UINT32:
      return static_cast<double>(ref->GetUInt32(message, field));
    case google::protobuf::FieldDescriptor::CPPTYPE_UINT64:
      return static_cast<double>(ref->GetUInt64(message, field));
    case google::protobuf::FieldDescriptor::CPPTYPE_BOOL:
      return ref->GetBool(message, field) ? 1.0 : 0.0;
    case google::protobuf::FieldDescriptor::CPPTYPE_ENUM:
      return static_cast<double>(ref->GetEnumValue(message, field));
    default:
      return 0.0;
  }
}

double proto_repeated_numeric(const google::protobuf::Message& message, const google::protobuf::FieldDescriptor* field,
                              int index) {
  const auto* ref = message.GetReflection();

  switch (field->cpp_type()) {
    case google::protobuf::FieldDescriptor::CPPTYPE_DOUBLE:
      return ref->GetRepeatedDouble(message, field, index);
    case google::protobuf::FieldDescriptor::CPPTYPE_FLOAT:
      return static_cast<double>(ref->GetRepeatedFloat(message, field, index));
    case google::protobuf::FieldDescriptor::CPPTYPE_INT32:
      return static_cast<double>(ref->GetRepeatedInt32(message, field, index));
    case google::protobuf::FieldDescriptor::CPPTYPE_INT64:
      return static_cast<double>(ref->GetRepeatedInt64(message, field, index));
    case google::protobuf::FieldDescriptor::CPPTYPE_UINT32:
      return static_cast<double>(ref->GetRepeatedUInt32(message, field, index));
    case google::protobuf::FieldDescriptor::CPPTYPE_UINT64:
      return static_cast<double>(ref->GetRepeatedUInt64(message, field, index));
    case google::protobuf::FieldDescriptor::CPPTYPE_BOOL:
      return ref->GetRepeatedBool(message, field, index) ? 1.0 : 0.0;
    case google::protobuf::FieldDescriptor::CPPTYPE_ENUM:
      return static_cast<double>(ref->GetRepeatedEnumValue(message, field, index));
    default:
      return 0.0;
  }
}

bool resolve_proto_numeric(const google::protobuf::Message& root, const std::string& path, double& out) {
  const std::vector<PathToken>& tokens = tokenize_path_cached(path);

  if (tokens.empty()) {
    return false;
  }

  const google::protobuf::Message* current = &root;

  for (size_t i = 0; i < tokens.size(); ++i) {
    if (tokens[i].is_index) {
      return false;
    }

    const auto* desc = current->GetDescriptor();
    const auto* ref = current->GetReflection();
    const auto* field = desc->FindFieldByName(tokens[i].name);

    if (!field) {
      return false;
    }

    const bool last = i + 1 == tokens.size();

    if (field->is_repeated()) {
      if (i + 1 >= tokens.size() || !tokens[i + 1].is_index) {
        return false;
      }

      const auto index = static_cast<int>(tokens[i + 1].index);

      if (index < 0 || index >= ref->FieldSize(*current, field)) {
        return false;
      }

      if (field->cpp_type() == google::protobuf::FieldDescriptor::CPPTYPE_MESSAGE) {
        current = &ref->GetRepeatedMessage(*current, field, index);
        ++i;
        continue;
      }

      if (i + 2 != tokens.size() || !proto_type_is_numeric(field->cpp_type())) {
        return false;
      }

      out = proto_repeated_numeric(*current, field, index);
      return true;
    }

    if (last) {
      if (!proto_type_is_numeric(field->cpp_type())) {
        return false;
      }

      out = proto_field_numeric(*current, field);
      return true;
    }

    if (field->cpp_type() != google::protobuf::FieldDescriptor::CPPTYPE_MESSAGE) {
      return false;
    }

    current = &ref->GetMessage(*current, field);
  }

  return false;
}

bool resolve_proto_string(const google::protobuf::Message& root, const std::string& path, std::string& out) {
  const std::vector<PathToken>& tokens = tokenize_path_cached(path);

  if (tokens.empty()) {
    return false;
  }

  const google::protobuf::Message* current = &root;

  for (size_t i = 0; i < tokens.size(); ++i) {
    if (tokens[i].is_index) {
      return false;
    }

    const auto* desc = current->GetDescriptor();
    const auto* ref = current->GetReflection();
    const auto* field = desc->FindFieldByName(tokens[i].name);

    if (!field || field->is_repeated()) {
      return false;
    }

    const bool last = i + 1 == tokens.size();

    if (last) {
      if (field->cpp_type() == google::protobuf::FieldDescriptor::CPPTYPE_STRING) {
        out = ref->GetString(*current, field);
        return true;
      }

      if (field->cpp_type() == google::protobuf::FieldDescriptor::CPPTYPE_ENUM) {
        const auto* enum_value = ref->GetEnum(*current, field);

        if (!enum_value) {
          out = std::to_string(ref->GetEnumValue(*current, field));
          return true;
        }

        const std::string name = std::string(enum_value->name());
        const size_t last_underscore = name.find_last_of('_');
        out = (last_underscore != std::string::npos && last_underscore + 1 < name.size())
                  ? name.substr(last_underscore + 1)
                  : name;
        return true;
      }

      return false;
    }

    if (field->cpp_type() != google::protobuf::FieldDescriptor::CPPTYPE_MESSAGE) {
      return false;
    }

    current = &ref->GetMessage(*current, field);
  }

  return false;
}

const reflection::Field* find_fbs_field(const reflection::Object& object, const std::string& name) {
  if (!object.fields()) {
    return nullptr;
  }

  for (const auto* field : *object.fields()) {
    if (field && field->name() && field->name()->str() == name) {
      return field;
    }
  }

  return nullptr;
}

bool fbs_type_is_numeric(reflection::BaseType type) {
  switch (type) {
    case reflection::Bool:
    case reflection::Byte:
    case reflection::UByte:
    case reflection::Short:
    case reflection::UShort:
    case reflection::Int:
    case reflection::UInt:
    case reflection::Long:
    case reflection::ULong:
    case reflection::Float:
    case reflection::Double:
      return true;
    default:
      return false;
  }
}

double fbs_field_numeric(const flatbuffers::Table& table, const reflection::Field& field) {
  switch (field.type()->base_type()) {
    case reflection::Float:
    case reflection::Double:
      return flatbuffers::GetAnyFieldF(table, field);
    case reflection::Bool:
    case reflection::Byte:
    case reflection::UByte:
    case reflection::Short:
    case reflection::UShort:
    case reflection::Int:
    case reflection::UInt:
    case reflection::Long:
    case reflection::ULong:
      return static_cast<double>(flatbuffers::GetAnyFieldI(table, field));
    default:
      return 0.0;
  }
}

bool resolve_fbs_numeric(const flatbuffers::Table& root_table, const reflection::Object& root_obj,
                         const reflection::Schema& schema, const std::string& path, double& out) {
  const std::vector<PathToken>& tokens = tokenize_path_cached(path);

  if (tokens.empty()) {
    return false;
  }

  const flatbuffers::Table* table = &root_table;
  const reflection::Object* obj = &root_obj;

  for (size_t i = 0; i < tokens.size(); ++i) {
    if (tokens[i].is_index) {
      return false;
    }

    const auto* field = find_fbs_field(*obj, tokens[i].name);

    if (!field) {
      return false;
    }

    const bool last = i + 1 == tokens.size();
    const auto base_type = field->type()->base_type();

    if (last) {
      if (!fbs_type_is_numeric(base_type)) {
        return false;
      }

      out = fbs_field_numeric(*table, *field);
      return true;
    }

    if (!schema.objects()) {
      return false;
    }

    if (base_type == reflection::Obj) {
      const auto* sub_table = flatbuffers::GetFieldT(*table, *field);
      const auto* sub_obj = schema.objects()->Get(static_cast<uint32_t>(field->type()->index()));

      if (!sub_table || !sub_obj) {
        return false;
      }

      table = sub_table;
      obj = sub_obj;
      continue;
    }

    if (base_type == reflection::Vector && field->type()->element() == reflection::Obj) {
      if (i + 1 >= tokens.size() || !tokens[i + 1].is_index) {
        return false;
      }

      const auto* vec = flatbuffers::GetFieldAnyV(*table, *field);
      const auto* sub_obj = schema.objects()->Get(static_cast<uint32_t>(field->type()->index()));

      if (!vec || !sub_obj || tokens[i + 1].index >= vec->size()) {
        return false;
      }

      const auto* sub_table = flatbuffers::GetAnyVectorElemPointer<const flatbuffers::Table>(vec, tokens[i + 1].index);

      if (!sub_table) {
        return false;
      }

      table = sub_table;
      obj = sub_obj;
      ++i;
      continue;
    }

    return false;
  }

  return false;
}

bool resolve_fbs_string(const flatbuffers::Table& root_table, const reflection::Object& root_obj,
                        const reflection::Schema& schema, const std::string& path, std::string& out) {
  const std::vector<PathToken>& tokens = tokenize_path_cached(path);

  if (tokens.empty()) {
    return false;
  }

  const flatbuffers::Table* table = &root_table;
  const reflection::Object* obj = &root_obj;

  for (size_t i = 0; i < tokens.size(); ++i) {
    if (tokens[i].is_index) {
      return false;
    }

    const auto* field = find_fbs_field(*obj, tokens[i].name);

    if (!field) {
      return false;
    }

    const bool last = i + 1 == tokens.size();
    const auto base_type = field->type()->base_type();

    if (last) {
      if (base_type == reflection::String) {
        const auto* value = flatbuffers::GetFieldS(*table, *field);
        out = value ? value->str() : std::string();
        return true;
      }

      const int enum_index = field->type()->index();

      if (enum_index >= 0 && schema.enums() && enum_index < static_cast<int>(schema.enums()->size())) {
        const auto* enum_def = schema.enums()->Get(static_cast<uint32_t>(enum_index));
        const int64_t enum_value = flatbuffers::GetAnyFieldI(*table, *field);

        if (enum_def && enum_def->values()) {
          for (const auto* enum_val : *enum_def->values()) {
            if (enum_val && enum_val->value() == enum_value && enum_val->name()) {
              const std::string name = enum_val->name()->str();
              const size_t last_underscore = name.find_last_of('_');
              out = (last_underscore != std::string::npos && last_underscore + 1 < name.size())
                        ? name.substr(last_underscore + 1)
                        : name;
              return true;
            }
          }
        }
      }

      return false;
    }

    if (base_type != reflection::Obj || !schema.objects()) {
      return false;
    }

    const auto* sub_table = flatbuffers::GetFieldT(*table, *field);
    const auto* sub_obj = schema.objects()->Get(static_cast<uint32_t>(field->type()->index()));

    if (!sub_table || !sub_obj) {
      return false;
    }

    table = sub_table;
    obj = sub_obj;
  }

  return false;
}

#ifdef VLINK_ENABLE_EXPRTK

bool is_reserved_identifier(const std::string& token) {
  static const std::unordered_set<std::string> kReserved{"pi", "inf", "nan", "epsilon", "e",  "true", "false", "and",
                                                         "or", "not", "xor", "mod",     "if", "else", "while", "for"};
  return kReserved.find(token) != kReserved.end();
}

std::vector<std::string> extract_identifiers(const std::string& expression) {
  std::vector<std::string> out;
  std::unordered_set<std::string> seen;

  size_t i = 0;

  while (i < expression.size()) {
    const auto ch = static_cast<unsigned char>(expression[i]);

    if (std::isalpha(ch) || ch == '_') {
      size_t j = i + 1;

      while (j < expression.size()) {
        const auto next = static_cast<unsigned char>(expression[j]);

        if (std::isalnum(next) || next == '_' || next == '.') {
          ++j;
        } else {
          break;
        }
      }

      std::string token = expression.substr(i, j - i);

      while (!token.empty() && token.back() == '.') {
        token.pop_back();
      }

      size_t k = j;

      while (k < expression.size() && std::isspace(static_cast<unsigned char>(expression[k]))) {
        ++k;
      }

      const bool is_call = k < expression.size() && expression[k] == '(';

      if (!is_call && !token.empty() && !is_reserved_identifier(token) && seen.insert(token).second) {
        out.push_back(token);
      }

      i = j;
    } else {
      ++i;
    }
  }

  return out;
}

void replace_identifier(std::string& expression, const std::string& token, const std::string& replacement) {
  size_t pos = 0;

  while ((pos = expression.find(token, pos)) != std::string::npos) {
    const char before = pos > 0 ? expression[pos - 1] : ' ';
    const size_t after_index = pos + token.size();
    const char after = after_index < expression.size() ? expression[after_index] : ' ';

    const bool boundary_before = !(std::isalnum(static_cast<unsigned char>(before)) || before == '_' || before == '.');
    const bool boundary_after = !(std::isalnum(static_cast<unsigned char>(after)) || after == '_' || after == '.');

    if (boundary_before && boundary_after) {
      expression.replace(pos, token.size(), replacement);
      pos += replacement.size();
    } else {
      pos += token.size();
    }
  }
}

struct CompiledExpression final {
  bool built{false};
  bool ok{false};
  std::vector<std::string> identifiers;
  std::vector<double> values;
  vlink::ExprtkSymbolTable symbols;
  vlink::ExprtkExpression expression;
};

void build_compiled_expression(CompiledExpression& compiled, const std::string& expression) {
  compiled.built = true;
  compiled.identifiers = extract_identifiers(expression);
  compiled.values.assign(compiled.identifiers.size(), 0.0);
  compiled.symbols.add_constants();

  std::vector<size_t> order(compiled.identifiers.size());
  std::iota(order.begin(), order.end(), 0);
  std::sort(order.begin(), order.end(), [&compiled](size_t a, size_t b) {
    return compiled.identifiers[a].size() > compiled.identifiers[b].size();
  });

  std::string rewritten = expression;

  for (const size_t index : order) {
    const std::string name = "vlinkid" + std::to_string(index);
    replace_identifier(rewritten, compiled.identifiers[index], name);
    compiled.symbols.add_variable(name, compiled.values[index]);
  }

  compiled.expression.register_symbol_table(compiled.symbols);

  compiled.ok = compiled.expression.compile(rewritten);
}

double evaluate_expression(const std::string& expression, const std::function<double(const std::string&)>& resolve) {
  static thread_local std::unordered_map<std::string, CompiledExpression> cache;

  auto& compiled = cache[expression];

  if (!compiled.built) {
    build_compiled_expression(compiled, expression);
  }

  if (!compiled.ok) {
    return 0.0;
  }

  for (size_t i = 0; i < compiled.identifiers.size(); ++i) {
    compiled.values[i] = resolve(compiled.identifiers[i]);
  }

  return compiled.expression.value();
}

#else

double evaluate_expression(const std::string& expression, const std::function<double(const std::string&)>& resolve) {
  (void)expression;
  (void)resolve;

  return 0.0;
}

#endif

uint32_t to_uint32(double value) {
  if (!(value > 0.0)) {
    return 0;
  }

  if (value >= 4294967295.0) {
    return 4294967295U;
  }

  return static_cast<uint32_t>(std::llround(value));
}

uint8_t to_uint8(double value) {
  if (!(value > 0.0)) {
    return 0;
  }

  if (value >= 255.0) {
    return 255U;
  }

  return static_cast<uint8_t>(std::llround(value));
}

bool parse_numeric_default(const perception::FieldMapping& mapping, double& value) {
  if (!mapping.has_default_value) {
    return false;
  }

  if (mapping.default_value == "true") {
    value = 1.0;
    return true;
  }

  if (mapping.default_value == "false" || mapping.default_value == "null") {
    value = 0.0;
    return true;
  }

  char* end = nullptr;
  const double parsed = std::strtod(mapping.default_value.c_str(), &end);

  if (end == mapping.default_value.c_str()) {
    return false;
  }

  value = parsed;
  return true;
}

using SlotMap = std::unordered_map<std::string, const perception::FieldMapping*>;

std::vector<std::string> split_paths(const std::string& spec) {
  std::vector<std::string> out;
  const auto input = vlink::Helpers::trim_string_view(spec);

  if VUNLIKELY (input.empty()) {
    return {""};
  }

  size_t start = 0;

  while (start <= input.size()) {
    const size_t comma = input.find(',', start);
    const size_t end = comma == std::string_view::npos ? input.size() : comma;
    const auto field = vlink::Helpers::trim_string_view(input.substr(start, end - start));

    if (field.empty()) {
      out.emplace_back();
    } else {
      auto parts = vlink::Helpers::split_any(std::string(field), " ");
      out.insert(out.end(), std::make_move_iterator(parts.begin()), std::make_move_iterator(parts.end()));
    }

    if (comma == std::string_view::npos) {
      break;
    }

    start = comma + 1;
  }

  if VUNLIKELY (out.empty()) {
    out.emplace_back();
  }
  return out;
}

void parse_target(const std::string& target, int& collection_index, std::string& slot) {
  collection_index = 0;
  slot = target;

  if (target.size() < 2 || target[0] != '$') {
    return;
  }

  const size_t dot = target.find('.');

  if (dot == std::string::npos || dot < 2) {
    return;
  }

  int index = 0;

  for (size_t k = 1; k < dot; ++k) {
    if (target[k] < '0' || target[k] > '9') {
      return;
    }

    if (index > 1000000) {
      return;
    }

    index = index * 10 + (target[k] - '0');
  }

  collection_index = index;
  slot = target.substr(dot + 1);
}

SlotMap build_slot_map(const PerceptionConfig::MappingRule& rule, int collection_index) {
  SlotMap slot_map;
  slot_map.reserve(rule.field_mappings.size());

  for (const auto& mapping : rule.field_mappings) {
    if (mapping.target.empty()) {
      continue;
    }

    int index = 0;
    std::string slot;
    parse_target(mapping.target, index, slot);

    if (slot.empty()) {
      continue;
    }

    if (index == collection_index) {
      slot_map[slot] = &mapping;
    } else if (index == 0) {
      slot_map.emplace(slot, &mapping);
    }
  }

  return slot_map;
}

const perception::FieldMapping* find_slot(const SlotMap& slot_map, const char* name) {
  const auto it = slot_map.find(name);
  return it == slot_map.end() ? nullptr : it->second;
}

template <typename ReaderT, typename ElemT>
void read_double(const ReaderT& reader, ElemT elem, const SlotMap& slot_map, const char* name, double& dst) {
  if (const auto* mapping = find_slot(slot_map, name)) {
    dst = reader.value(elem, *mapping);
  }
}

template <typename ReaderT, typename ElemT>
void read_uint32(const ReaderT& reader, ElemT elem, const SlotMap& slot_map, const char* name, uint32_t& dst) {
  if (const auto* mapping = find_slot(slot_map, name)) {
    dst = to_uint32(reader.value(elem, *mapping));
  }
}

template <typename ReaderT, typename ElemT>
void read_uint8(const ReaderT& reader, ElemT elem, const SlotMap& slot_map, const char* name, uint8_t& dst) {
  if (const auto* mapping = find_slot(slot_map, name)) {
    dst = to_uint8(reader.value(elem, *mapping));
  }
}

template <typename ReaderT, typename ElemT>
void read_int32(const ReaderT& reader, ElemT elem, const SlotMap& slot_map, const char* name, int32_t& dst) {
  if (const auto* mapping = find_slot(slot_map, name)) {
    dst = static_cast<int32_t>(std::llround(reader.value(elem, *mapping)));
  }
}

template <typename ReaderT, typename ElemT>
void read_int(const ReaderT& reader, ElemT elem, const SlotMap& slot_map, const char* name, int& dst) {
  if (const auto* mapping = find_slot(slot_map, name)) {
    dst = static_cast<int>(std::llround(reader.value(elem, *mapping)));
  }
}

template <typename ReaderT, typename ElemT>
void read_float(const ReaderT& reader, ElemT elem, const SlotMap& slot_map, const char* name, float& dst) {
  if (const auto* mapping = find_slot(slot_map, name)) {
    dst = static_cast<float>(reader.value(elem, *mapping));
  }
}

template <typename ReaderT, typename ElemT>
void read_string(const ReaderT& reader, ElemT elem, const SlotMap& slot_map, const char* name, std::string& dst) {
  if (const auto* mapping = find_slot(slot_map, name)) {
    dst = reader.text(elem, *mapping);
  }
}

template <typename ReaderT, typename ElemT>
BoxObject read_box(const ReaderT& reader, ElemT elem, const SlotMap& slot_map) {
  BoxObject box;

  read_double(reader, elem, slot_map, "x", box.position[0]);
  read_double(reader, elem, slot_map, "y", box.position[1]);
  read_double(reader, elem, slot_map, "z", box.position[2]);
  read_double(reader, elem, slot_map, "length", box.size[0]);
  read_double(reader, elem, slot_map, "width", box.size[1]);
  read_double(reader, elem, slot_map, "height", box.size[2]);
  read_double(reader, elem, slot_map, "yaw", box.yaw);
  read_double(reader, elem, slot_map, "vx", box.velocity[0]);
  read_double(reader, elem, slot_map, "vy", box.velocity[1]);
  read_double(reader, elem, slot_map, "vz", box.velocity[2]);
  read_double(reader, elem, slot_map, "score", box.score);
  read_uint32(reader, elem, slot_map, "class_id", box.class_id);
  read_uint32(reader, elem, slot_map, "track_id", box.track_id);
  read_uint32(reader, elem, slot_map, "color", box.color);
  read_string(reader, elem, slot_map, "label", box.label);

  read_uint8(reader, elem, slot_map, "color_state", box.color_state);
  read_float(reader, elem, slot_map, "confidence", box.confidence);
  read_int32(reader, elem, slot_map, "countdown", box.countdown);

  read_uint32(reader, elem, slot_map, "type_id", box.type_id);
  read_double(reader, elem, slot_map, "marker_size", box.marker_size);

  read_double(reader, elem, slot_map, "qx", box.orientation[0]);
  read_double(reader, elem, slot_map, "qy", box.orientation[1]);
  read_double(reader, elem, slot_map, "qz", box.orientation[2]);
  read_double(reader, elem, slot_map, "qw", box.orientation[3]);
  read_double(reader, elem, slot_map, "fov_h", box.fov_h);
  read_double(reader, elem, slot_map, "fov_v", box.fov_v);
  read_double(reader, elem, slot_map, "near", box.near_dist);
  read_double(reader, elem, slot_map, "far", box.far_dist);

  if (const auto* mapping = find_slot(slot_map, "cov_xx")) {
    box.covariance[0] = reader.value(elem, *mapping);
  }

  if (const auto* mapping = find_slot(slot_map, "cov_xy")) {
    box.covariance[1] = reader.value(elem, *mapping);
    box.covariance[2] = box.covariance[1];
  }

  if (const auto* mapping = find_slot(slot_map, "cov_yy")) {
    box.covariance[3] = reader.value(elem, *mapping);
  }

  return box;
}

template <typename ReaderT, typename ElemT>
ParkingSlot read_slot(const ReaderT& reader, ElemT elem, const SlotMap& slot_map) {
  ParkingSlot slot;

  static const char* const kCornerNames[4][3] = {
      {"corner0_x", "corner0_y", "corner0_z"},
      {"corner1_x", "corner1_y", "corner1_z"},
      {"corner2_x", "corner2_y", "corner2_z"},
      {"corner3_x", "corner3_y", "corner3_z"},
  };

  for (int c = 0; c < 4; ++c) {
    for (int axis = 0; axis < 3; ++axis) {
      read_double(reader, elem, slot_map, kCornerNames[c][axis], slot.corners[c][axis]);
    }
  }

  read_uint32(reader, elem, slot_map, "slot_id", slot.slot_id);
  read_uint32(reader, elem, slot_map, "slot_type", slot.slot_type);
  read_uint32(reader, elem, slot_map, "color", slot.color);
  read_float(reader, elem, slot_map, "confidence", slot.confidence);

  return slot;
}

template <typename ReaderT, typename ElemT>
PolyPoint read_point(const ReaderT& reader, ElemT elem, const SlotMap& slot_map) {
  PolyPoint point;

  read_double(reader, elem, slot_map, "x", point.x);
  read_double(reader, elem, slot_map, "y", point.y);
  read_double(reader, elem, slot_map, "z", point.z);
  read_double(reader, elem, slot_map, "yaw", point.yaw);
  read_double(reader, elem, slot_map, "speed", point.speed);
  read_double(reader, elem, slot_map, "timestamp", point.timestamp);

  return point;
}

template <typename ReaderT, typename ElemT>
void read_polyline_attributes(const ReaderT& reader, ElemT elem, const SlotMap& slot_map, Polyline& line) {
  read_uint32(reader, elem, slot_map, "color", line.color);
  read_int(reader, elem, slot_map, "type", line.type);
  read_string(reader, elem, slot_map, "label", line.label);
  read_uint32(reader, elem, slot_map, "track_id", line.track_id);
  read_float(reader, elem, slot_map, "confidence", line.confidence);
}

bool is_finite3(const double (&values)[3]) {
  return std::isfinite(values[0]) && std::isfinite(values[1]) && std::isfinite(values[2]);
}

bool point_is_finite(const PolyPoint& point) {
  return std::isfinite(point.x) && std::isfinite(point.y) && std::isfinite(point.z);
}

template <typename ReaderT>
void fill_boxes(const ReaderT& reader, const std::string& collection, const SlotMap& slot_map,
                std::vector<BoxObject>& out) {
  std::vector<typename ReaderT::Elem> elems;
  reader.collect(reader.root(), collection, elems);
  out.reserve(out.size() + elems.size());

  for (auto elem : elems) {
    BoxObject box = read_box(reader, elem, slot_map);

    if (is_finite3(box.position)) {
      out.emplace_back(std::move(box));
    }
  }
}

template <typename ReaderT>
void fill_slots(const ReaderT& reader, const std::string& collection, const SlotMap& slot_map,
                std::vector<ParkingSlot>& out) {
  std::vector<typename ReaderT::Elem> elems;
  reader.collect(reader.root(), collection, elems);
  out.reserve(out.size() + elems.size());

  for (auto elem : elems) {
    ParkingSlot slot = read_slot(reader, elem, slot_map);

    if (is_finite3(slot.corners[0])) {
      out.emplace_back(std::move(slot));
    }
  }
}

template <typename ReaderT>
void fill_polylines(const ReaderT& reader, const std::string& outer_path, const std::string& inner_path,
                    const SlotMap& slot_map, std::vector<Polyline>& out) {
  std::vector<typename ReaderT::Elem> outer;
  reader.collect(reader.root(), outer_path, outer);

  if (inner_path.empty()) {
    Polyline line;
    read_polyline_attributes(reader, reader.root(), slot_map, line);
    line.points.reserve(outer.size());

    for (auto elem : outer) {
      PolyPoint point = read_point(reader, elem, slot_map);

      if (point_is_finite(point)) {
        line.points.emplace_back(point);
      }
    }

    if (!line.points.empty()) {
      out.emplace_back(std::move(line));
    }

    return;
  }

  out.reserve(out.size() + outer.size());

  for (auto line_elem : outer) {
    Polyline line;
    read_polyline_attributes(reader, line_elem, slot_map, line);

    std::vector<typename ReaderT::Elem> inner;
    reader.collect(line_elem, inner_path, inner);
    line.points.reserve(inner.size());

    for (auto point_elem : inner) {
      PolyPoint point = read_point(reader, point_elem, slot_map);

      if (point_is_finite(point)) {
        line.points.emplace_back(point);
      }
    }

    if (!line.points.empty()) {
      out.emplace_back(std::move(line));
    }
  }
}

template <typename ReaderT>
void fill_cloud(const ReaderT& reader, const std::string& collection, const SlotMap& slot_map, Layer& out) {
  std::vector<typename ReaderT::Elem> elems;
  reader.collect(reader.root(), collection, elems);
  out.cloud.reserve(out.cloud.size() + elems.size());

  const bool has_value = find_slot(slot_map, "intensity") != nullptr;
  out.has_value_channel = out.has_value_channel || has_value;

  for (auto elem : elems) {
    CloudPoint point;
    read_double(reader, elem, slot_map, "x", point.x);
    read_double(reader, elem, slot_map, "y", point.y);
    read_double(reader, elem, slot_map, "z", point.z);

    if (has_value) {
      read_double(reader, elem, slot_map, "intensity", point.value);
    }

    if (std::isfinite(point.x) && std::isfinite(point.y) && std::isfinite(point.z)) {
      out.cloud.emplace_back(point);
    }
  }
}

template <typename ReaderT>
void fill_grid(const ReaderT& reader, const std::string& collection, const SlotMap& slot_map, Layer& out) {
  std::vector<typename ReaderT::Elem> elems;
  reader.collect(reader.root(), collection, elems);

  if (elems.empty()) {
    return;
  }

  const auto elem = elems.front();
  Grid& grid = out.grid;

  read_double(reader, elem, slot_map, "origin_x", grid.origin_x);
  read_double(reader, elem, slot_map, "origin_y", grid.origin_y);
  read_double(reader, elem, slot_map, "origin_z", grid.origin_z);
  read_double(reader, elem, slot_map, "resolution", grid.resolution);
  read_uint32(reader, elem, slot_map, "width", grid.width);
  read_uint32(reader, elem, slot_map, "height", grid.height);

  if (const auto* mapping = find_slot(slot_map, "cells"); mapping && !mapping->source.empty()) {
    std::vector<double> values;
    reader.collect_scalars(elem, mapping->source, values);
    grid.cells.reserve(values.size());

    for (const double value : values) {
      if (std::isnan(value)) {
        grid.cells.emplace_back(static_cast<int8_t>(-1));
        continue;
      }

      const double clamped = std::min(std::max(value, -128.0), 127.0);
      grid.cells.emplace_back(static_cast<int8_t>(std::llround(clamped)));
    }
  }

  out.grid_valid = grid.width > 0 && grid.height > 0 && !grid.cells.empty();
}

template <typename ReaderT>
void decode_with_reader(const ReaderT& reader, const PerceptionConfig::MappingRule& rule, Layer& out) {
  out.type = rule.type;

  const std::vector<std::string> collections = split_paths(rule.collection.toStdString());
  const std::vector<std::string> inners = split_paths(rule.inner_collection.toStdString());

  for (size_t ci = 0; ci < collections.size(); ++ci) {
    const SlotMap slot_map = build_slot_map(rule, static_cast<int>(ci + 1));
    const std::string& collection = collections[ci];
    const std::string& inner = ci < inners.size() ? inners[ci] : inners.back();

    switch (rule.type) {
      case RenderType::kObjectDetection:
      case RenderType::kTrafficLight:
      case RenderType::kTrafficSign:
      case RenderType::kCameraFrustum:
      case RenderType::kCovarianceEllipse: {
        fill_boxes(reader, collection, slot_map, out.boxes);
        break;
      }

      case RenderType::kParkingSlot: {
        fill_slots(reader, collection, slot_map, out.parking_slots);
        break;
      }

      case RenderType::kPointCloud: {
        fill_cloud(reader, collection, slot_map, out);
        break;
      }

      case RenderType::kOccupancyGrid: {
        if (ci == 0) {
          fill_grid(reader, collection, slot_map, out);
        }

        break;
      }

      case RenderType::kLaneLine:
      case RenderType::kPrediction:
      case RenderType::kStopLine:
      case RenderType::kFreespace:
      case RenderType::kHdMap:
      case RenderType::kEgoTrajectory: {
        fill_polylines(reader, collection, inner, slot_map, out.polylines);
        break;
      }

      default: {
        break;
      }
    }
  }
}

void collect_proto_elements(const google::protobuf::Message* msg, const std::vector<PathToken>& tokens, size_t i,
                            std::vector<const google::protobuf::Message*>& out) {
  if (i >= tokens.size()) {
    out.emplace_back(msg);
    return;
  }

  if (tokens[i].is_index) {
    return;
  }

  const auto* field = msg->GetDescriptor()->FindFieldByName(tokens[i].name);

  if (!field || field->cpp_type() != google::protobuf::FieldDescriptor::CPPTYPE_MESSAGE) {
    return;
  }

  const auto* ref = msg->GetReflection();
  const bool last = i + 1 == tokens.size();

  if (field->is_repeated()) {
    if (i + 1 < tokens.size() && tokens[i + 1].is_index) {
      const auto index = static_cast<int>(tokens[i + 1].index);

      if (index < 0 || index >= ref->FieldSize(*msg, field)) {
        return;
      }

      const auto* sub = &ref->GetRepeatedMessage(*msg, field, index);

      if (i + 2 >= tokens.size()) {
        out.emplace_back(sub);
      } else {
        collect_proto_elements(sub, tokens, i + 2, out);
      }

      return;
    }

    const int count = ref->FieldSize(*msg, field);

    for (int k = 0; k < count; ++k) {
      const auto* sub = &ref->GetRepeatedMessage(*msg, field, k);

      if (last) {
        out.emplace_back(sub);
      } else {
        collect_proto_elements(sub, tokens, i + 1, out);
      }
    }

    return;
  }

  const auto* sub = &ref->GetMessage(*msg, field);

  if (last) {
    out.emplace_back(sub);
  } else {
    collect_proto_elements(sub, tokens, i + 1, out);
  }
}

class ProtoReader final {
 public:
  using Elem = const google::protobuf::Message*;

  explicit ProtoReader(const google::protobuf::Message& root) : root_(&root) {}

  [[nodiscard]] Elem root() const { return root_; }

  void collect(Elem base, const std::string& path, std::vector<Elem>& out) const {
    if (path.empty()) {
      out.emplace_back(base);
      return;
    }

    const std::vector<PathToken>& tokens = tokenize_path_cached(path);

    if (tokens.empty()) {
      return;
    }

    collect_proto_elements(base, tokens, 0, out);
  }

  [[nodiscard]] double value(Elem elem, const perception::FieldMapping& mapping) const {
    if (!mapping.expression.empty()) {
      return evaluate_expression(mapping.expression, [elem](const std::string& identifier) {
        double resolved = 0.0;
        return resolve_proto_numeric(*elem, identifier, resolved) ? resolved : 0.0;
      });
    }

    double resolved = 0.0;

    if (!mapping.source.empty() && resolve_proto_numeric(*elem, mapping.source, resolved)) {
      return resolved;
    }

    double fallback = 0.0;
    parse_numeric_default(mapping, fallback);
    return fallback;
  }

  [[nodiscard]] std::string text(Elem elem, const perception::FieldMapping& mapping) const {
    if (!mapping.expression.empty()) {
      return format_double_string(evaluate_expression(mapping.expression, [elem](const std::string& identifier) {
        double resolved = 0.0;
        return resolve_proto_numeric(*elem, identifier, resolved) ? resolved : 0.0;
      }));
    }

    std::string resolved;

    if (!mapping.source.empty() && resolve_proto_string(*elem, mapping.source, resolved)) {
      return resolved;
    }

    return mapping.has_default_value ? mapping.default_value : std::string();
  }

  void collect_scalars(Elem base, const std::string& path, std::vector<double>& out) const {
    const std::vector<PathToken>& tokens = tokenize_path_cached(path);

    if (tokens.empty()) {
      return;
    }

    const google::protobuf::Message* current = base;

    for (size_t i = 0; i + 1 < tokens.size(); ++i) {
      if (tokens[i].is_index) {
        return;
      }

      const auto* field = current->GetDescriptor()->FindFieldByName(tokens[i].name);

      if (!field || field->cpp_type() != google::protobuf::FieldDescriptor::CPPTYPE_MESSAGE || field->is_repeated()) {
        return;
      }

      current = &current->GetReflection()->GetMessage(*current, field);
    }

    if (tokens.back().is_index) {
      return;
    }

    const auto* field = current->GetDescriptor()->FindFieldByName(tokens.back().name);

    if (!field || !field->is_repeated() || !proto_type_is_numeric(field->cpp_type())) {
      return;
    }

    const auto* ref = current->GetReflection();
    const int count = ref->FieldSize(*current, field);
    out.reserve(out.size() + static_cast<size_t>(count));

    for (int i = 0; i < count; ++i) {
      out.emplace_back(proto_repeated_numeric(*current, field, i));
    }
  }

 private:
  const google::protobuf::Message* root_;
};

class FbsReader final {
 public:
  struct Elem final {
    const flatbuffers::Table* table{nullptr};
    const reflection::Object* obj{nullptr};
  };

  FbsReader(const flatbuffers::Table& root, const reflection::Schema& schema, const reflection::Object& root_obj)
      : root_{&root, &root_obj}, schema_(&schema) {}

  [[nodiscard]] Elem root() const { return root_; }

  void collect(Elem base, const std::string& path, std::vector<Elem>& out) const {
    if (path.empty()) {
      out.emplace_back(base);
      return;
    }

    const std::vector<PathToken>& tokens = tokenize_path_cached(path);

    if (tokens.empty() || !schema_->objects()) {
      return;
    }

    const flatbuffers::Table* table = base.table;
    const reflection::Object* obj = base.obj;

    for (size_t i = 0; i + 1 < tokens.size(); ++i) {
      if (tokens[i].is_index) {
        return;
      }

      const auto* field = find_fbs_field(*obj, tokens[i].name);

      if (!field || field->type()->base_type() != reflection::Obj) {
        return;
      }

      const auto* sub_table = flatbuffers::GetFieldT(*table, *field);
      const auto* sub_obj = schema_->objects()->Get(static_cast<uint32_t>(field->type()->index()));

      if (!sub_table || !sub_obj) {
        return;
      }

      table = sub_table;
      obj = sub_obj;
    }

    if (tokens.back().is_index) {
      return;
    }

    const auto* field = find_fbs_field(*obj, tokens.back().name);

    if (!field) {
      return;
    }

    if (field->type()->base_type() == reflection::Obj) {
      const auto* sub_table = flatbuffers::GetFieldT(*table, *field);
      const auto* sub_obj = schema_->objects()->Get(static_cast<uint32_t>(field->type()->index()));

      if (sub_table && sub_obj) {
        out.emplace_back(Elem{sub_table, sub_obj});
      }

      return;
    }

    if (field->type()->base_type() != reflection::Vector || field->type()->element() != reflection::Obj) {
      return;
    }

    const auto* vec = flatbuffers::GetFieldAnyV(*table, *field);
    const auto* elem_obj = schema_->objects()->Get(static_cast<uint32_t>(field->type()->index()));

    if (!vec || !elem_obj) {
      return;
    }

    out.reserve(out.size() + vec->size());

    for (flatbuffers::uoffset_t i = 0; i < vec->size(); ++i) {
      const auto* elem_table = flatbuffers::GetAnyVectorElemPointer<const flatbuffers::Table>(vec, i);

      if (elem_table) {
        out.emplace_back(Elem{elem_table, elem_obj});
      }
    }
  }

  [[nodiscard]] double value(Elem elem, const perception::FieldMapping& mapping) const {
    if (!mapping.expression.empty()) {
      const auto* table = elem.table;
      const auto* obj = elem.obj;
      const auto* schema = schema_;

      return evaluate_expression(mapping.expression, [table, obj, schema](const std::string& identifier) {
        double resolved = 0.0;
        return resolve_fbs_numeric(*table, *obj, *schema, identifier, resolved) ? resolved : 0.0;
      });
    }

    double resolved = 0.0;

    if (!mapping.source.empty() && resolve_fbs_numeric(*elem.table, *elem.obj, *schema_, mapping.source, resolved)) {
      return resolved;
    }

    double fallback = 0.0;
    parse_numeric_default(mapping, fallback);
    return fallback;
  }

  [[nodiscard]] std::string text(Elem elem, const perception::FieldMapping& mapping) const {
    if (!mapping.expression.empty()) {
      const auto* table = elem.table;
      const auto* obj = elem.obj;
      const auto* schema = schema_;

      return format_double_string(
          evaluate_expression(mapping.expression, [table, obj, schema](const std::string& identifier) {
            double resolved = 0.0;
            return resolve_fbs_numeric(*table, *obj, *schema, identifier, resolved) ? resolved : 0.0;
          }));
    }

    std::string resolved;

    if (!mapping.source.empty() && resolve_fbs_string(*elem.table, *elem.obj, *schema_, mapping.source, resolved)) {
      return resolved;
    }

    return mapping.has_default_value ? mapping.default_value : std::string();
  }

  void collect_scalars(Elem base, const std::string& path, std::vector<double>& out) const {
    const std::vector<PathToken>& tokens = tokenize_path_cached(path);

    if (tokens.empty() || !schema_->objects()) {
      return;
    }

    const flatbuffers::Table* table = base.table;
    const reflection::Object* obj = base.obj;

    for (size_t i = 0; i + 1 < tokens.size(); ++i) {
      if (tokens[i].is_index) {
        return;
      }

      const auto* field = find_fbs_field(*obj, tokens[i].name);

      if (!field || field->type()->base_type() != reflection::Obj) {
        return;
      }

      const auto* sub_table = flatbuffers::GetFieldT(*table, *field);
      const auto* sub_obj = schema_->objects()->Get(static_cast<uint32_t>(field->type()->index()));

      if (!sub_table || !sub_obj) {
        return;
      }

      table = sub_table;
      obj = sub_obj;
    }

    if (tokens.back().is_index) {
      return;
    }

    const auto* field = find_fbs_field(*obj, tokens.back().name);

    if (!field || field->type()->base_type() != reflection::Vector || !fbs_type_is_numeric(field->type()->element())) {
      return;
    }

    const auto* vec = flatbuffers::GetFieldAnyV(*table, *field);

    if (!vec) {
      return;
    }

    out.reserve(out.size() + vec->size());

    for (flatbuffers::uoffset_t i = 0; i < vec->size(); ++i) {
      out.emplace_back(flatbuffers::GetAnyVectorElemF(vec, field->type()->element(), i));
    }
  }

 private:
  Elem root_;
  const reflection::Schema* schema_;
};

bool is_hud_text_slot(const std::string& slot) {
  return slot == "gear" || slot == "turn_signal" || slot == "drive_mode";
}

template <typename ReaderT>
void fill_hud(const ReaderT& reader, const PerceptionConfig::MappingRule& rule, std::vector<HudField>& out) {
  out.reserve(rule.field_mappings.size());

  for (const auto& mapping : rule.field_mappings) {
    if (mapping.target.empty()) {
      continue;
    }

    HudField field;
    field.slot = mapping.target;
    field.is_text = is_hud_text_slot(mapping.target);

    if (field.is_text) {
      field.text = reader.text(reader.root(), mapping);
    } else {
      field.value = reader.value(reader.root(), mapping);
    }

    out.emplace_back(std::move(field));
  }
}

void decode_proto(const google::protobuf::Message& root, const PerceptionConfig::MappingRule& rule, Layer& out) {
  const ProtoReader reader(root);
  decode_with_reader(reader, rule, out);
}

void decode_hud_proto(const google::protobuf::Message& root, const PerceptionConfig::MappingRule& rule,
                      std::vector<HudField>& out) {
  const ProtoReader reader(root);
  fill_hud(reader, rule, out);
}

void decode_hud_fbs(const flatbuffers::Table& root, const reflection::Schema& schema,
                    const reflection::Object& root_obj, const PerceptionConfig::MappingRule& rule,
                    std::vector<HudField>& out) {
  const FbsReader reader(root, schema, root_obj);
  fill_hud(reader, rule, out);
}

void decode_fbs(const flatbuffers::Table& root, const reflection::Schema& schema, const reflection::Object& root_obj,
                const PerceptionConfig::MappingRule& rule, Layer& out) {
  const FbsReader reader(root, schema, root_obj);
  decode_with_reader(reader, rule, out);
}

}  // namespace mapping
}  // namespace perception

// NOLINTEND
