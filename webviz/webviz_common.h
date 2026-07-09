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

#ifdef VLINK_HAS_FBS_COMPILER
#include <flatbuffers/flatbuffers.h>
#include <flatbuffers/reflection.h>
#include <flatbuffers/util.h>
#endif

#ifdef VLINK_HAS_PROTO_COMPILER
#include <google/protobuf/compiler/importer.h>
#endif

#include <google/protobuf/descriptor.h>
#include <google/protobuf/message.h>
#include <vlink/base/bytes.h>
#include <vlink/base/helpers.h>
#include <vlink/base/logger.h>

#include <cmath>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <iomanip>
#include <limits>
#include <sstream>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <vector>

#include "webviz_types.h"
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

namespace vlink {
namespace webviz {

inline std::string make_expression_variable_name(std::string_view prefix, size_t index) {
  return "__vlink_" + std::string(prefix) + "_expr_" + std::to_string(index);
}

inline std::string format_expression_string(double value) {
  if VUNLIKELY (!std::isfinite(value)) {
    return {};
  }

  thread_local std::ostringstream oss;
  oss.str({});
  oss.clear();
  oss << std::setprecision(15) << value;
  auto text = oss.str();

  if VLIKELY (text.find('.') != std::string::npos) {
    while (!text.empty() && text.back() == '0') {
      text.pop_back();
    }

    if VUNLIKELY (!text.empty() && text.back() == '.') {
      text.pop_back();
    }
  }

  return text;
}

inline uint64_t mix_cache_hash(uint64_t hash, uint64_t value) noexcept { return Helpers::hash_combine(hash, value); }

inline uint64_t hash_cache_string(std::string_view value) {
  return static_cast<uint64_t>(std::hash<std::string_view>{}(value));
}

inline uint64_t pointer_cache_identity(const void* ptr) noexcept { return reinterpret_cast<uintptr_t>(ptr); }

template <typename UIntT>
inline UIntT checked_unsigned_cast(double value, UIntT fallback = 0) {
  static_assert(std::is_integral_v<UIntT> && std::is_unsigned_v<UIntT>,
                "checked_unsigned_cast requires an unsigned integer target");

  if VUNLIKELY (!std::isfinite(value) || value < 0.0) {
    return fallback;
  }

  const auto upper_bound = std::ldexp(1.0, std::numeric_limits<UIntT>::digits);

  if VUNLIKELY (value >= upper_bound) {
    return fallback;
  }

  return static_cast<UIntT>(value);
}

#ifdef VLINK_HAS_FBS_COMPILER
inline uint64_t hash_fbs_string(const flatbuffers::String* value) {
  if VUNLIKELY (!value) {
    return 0;
  }

  return hash_cache_string(std::string_view(value->c_str(), value->size()));
}

inline uint64_t fbs_type_cache_identity(const reflection::Type* type) {
  if VUNLIKELY (!type) {
    return 0;
  }

  uint64_t hash = 0x62b9d9ed799705f5ULL;
  hash = mix_cache_hash(hash, static_cast<uint64_t>(type->base_type()));
  hash = mix_cache_hash(hash, static_cast<uint64_t>(type->element()));
  hash = mix_cache_hash(hash, static_cast<uint64_t>(static_cast<uint32_t>(type->index())));
  hash = mix_cache_hash(hash, type->fixed_length());
  hash = mix_cache_hash(hash, type->base_size());
  hash = mix_cache_hash(hash, type->element_size());
  return hash;
}

inline uint64_t fbs_field_cache_identity(const reflection::Field* field) {
  if VUNLIKELY (!field) {
    return 0;
  }

  uint64_t hash = 0x9b264bd3f2d6a9f1ULL;
  hash = mix_cache_hash(hash, hash_fbs_string(field->name()));
  hash = mix_cache_hash(hash, fbs_type_cache_identity(field->type()));
  hash = mix_cache_hash(hash, field->id());
  hash = mix_cache_hash(hash, field->offset());
  hash = mix_cache_hash(hash, static_cast<uint64_t>(field->default_integer()));
  hash = mix_cache_hash(hash, static_cast<uint64_t>(std::hash<double>{}(field->default_real())));
  hash = mix_cache_hash(hash, field->deprecated() ? 1U : 0U);
  hash = mix_cache_hash(hash, field->required() ? 1U : 0U);
  hash = mix_cache_hash(hash, field->key() ? 1U : 0U);
  return hash;
}

inline uint64_t fbs_object_cache_identity(const reflection::Object* obj) {
  if VUNLIKELY (!obj) {
    return 0;
  }

  uint64_t hash = 0x1f3d5b79ace02468ULL;
  hash = mix_cache_hash(hash, hash_fbs_string(obj->name()));
  hash = mix_cache_hash(hash, obj->is_struct() ? 1U : 0U);
  hash = mix_cache_hash(hash, static_cast<uint64_t>(static_cast<uint32_t>(obj->minalign())));
  hash = mix_cache_hash(hash, static_cast<uint64_t>(static_cast<uint32_t>(obj->bytesize())));

  const auto* fields = obj->fields();
  hash = mix_cache_hash(hash, fields ? fields->size() : 0U);

  if VLIKELY (fields) {
    for (uint32_t i = 0; i < fields->size(); ++i) {
      hash = mix_cache_hash(hash, fbs_field_cache_identity(fields->Get(i)));
    }
  }

  return hash;
}

inline uint64_t fbs_schema_cache_identity(const reflection::Schema& schema) {
  uint64_t hash = 0xd6e8feb86659fd93ULL;
  hash = mix_cache_hash(hash, hash_fbs_string(schema.file_ident()));
  hash = mix_cache_hash(hash, hash_fbs_string(schema.file_ext()));
  hash = mix_cache_hash(hash, fbs_object_cache_identity(schema.root_table()));

  const auto* objects = schema.objects();
  hash = mix_cache_hash(hash, objects ? objects->size() : 0U);

  if VLIKELY (objects) {
    for (uint32_t i = 0; i < objects->size(); ++i) {
      hash = mix_cache_hash(hash, fbs_object_cache_identity(objects->Get(i)));
    }
  }

  return hash;
}

inline const reflection::Schema* get_verified_fbs_schema(std::string_view ser, const std::string& schema_data) {
  if VUNLIKELY (schema_data.size() < sizeof(flatbuffers::uoffset_t)) {
    MLOG_W("FlatBuffers schema buffer too small for: {}", ser);
    return nullptr;
  }

  flatbuffers::Verifier verifier(reinterpret_cast<const uint8_t*>(schema_data.data()), schema_data.size());

  if VUNLIKELY (!reflection::VerifySchemaBuffer(verifier)) {
    MLOG_W("Invalid FlatBuffers schema buffer for: {}", ser);
    return nullptr;
  }

  const auto* schema = reflection::GetSchema(reinterpret_cast<const uint8_t*>(schema_data.data()));

  if VUNLIKELY (!schema || !schema->root_table()) {
    MLOG_W("FlatBuffers schema has no root table for: {}", ser);
    return nullptr;
  }

  return schema;
}

inline bool verify_fbs_payload(const reflection::Schema& schema, const Bytes& raw, std::string_view ser,
                               std::string_view context = {}) {
  if VUNLIKELY (!schema.root_table()) {
    MLOG_W("FlatBuffers schema has no root table for: {}", ser);
    return false;
  }

  if VUNLIKELY (raw.size() < sizeof(flatbuffers::uoffset_t) || raw.data() == nullptr) {
    if VUNLIKELY (context.empty()) {
      MLOG_W("FlatBuffers buffer too small for: {}", ser);
    } else {
      MLOG_W("FlatBuffers buffer too small for {} during {}", ser, context);
    }
    return false;
  }

  if VUNLIKELY (!flatbuffers::Verify(schema, *schema.root_table(), raw.data(), raw.size())) {
    if VUNLIKELY (context.empty()) {
      MLOG_W("Invalid FlatBuffers buffer for: {}", ser);
    } else {
      MLOG_W("Invalid FlatBuffers buffer for {} during {}", ser, context);
    }
    return false;
  }

  return true;
}
#endif

struct FieldPathToken final {
  bool is_index{false};
  std::string name;
  size_t index{0};
};

inline constexpr size_t kFieldPathTokenCacheLimit = 128;
inline constexpr size_t kProtoFieldLookupCacheLimit = 256;
#ifdef VLINK_HAS_FBS_COMPILER
inline constexpr size_t kFbsFieldLookupCacheLimit = 256;
#endif

struct FieldPathCacheEntry final {
  std::string path;
  std::vector<FieldPathToken> tokens;
};

struct FieldPathCacheStore final {
  std::unordered_map<size_t, std::vector<FieldPathCacheEntry>> buckets;
  std::vector<std::pair<size_t, std::string>> insertion_order;
  size_t size{0};
  size_t last_hash{0};
  std::string last_path;
  std::vector<FieldPathToken> last_tokens;
  bool has_last_tokens{false};
};

struct ProtoFieldLookupCacheEntry final {
  const google::protobuf::Descriptor* descriptor{nullptr};
  std::string field_name;
  const google::protobuf::FieldDescriptor* field{nullptr};
};

struct ProtoFieldLookupCacheStore final {
  std::unordered_map<uint64_t, std::vector<ProtoFieldLookupCacheEntry>> buckets;
  std::vector<std::pair<uint64_t, ProtoFieldLookupCacheEntry>> insertion_order;
  size_t size{0};
  uint64_t last_hash{0};
  const google::protobuf::Descriptor* last_descriptor{nullptr};
  std::string last_field_name;
  const google::protobuf::FieldDescriptor* last_field{nullptr};
};

struct ProtoDirectFieldRef final {
  const google::protobuf::FieldDescriptor* field{nullptr};
  size_t value_idx{0};
};

struct ProtoPathStep final {
  const google::protobuf::FieldDescriptor* field{nullptr};
  size_t index{0};
  bool has_index{false};
};

struct ProtoIndexedFieldRef final {
  std::vector<ProtoPathStep> steps;
  size_t value_idx{0};
};

#ifdef VLINK_HAS_FBS_COMPILER
struct FbsFieldLookupCacheEntry final {
  const reflection::Object* object{nullptr};
  std::string field_name;
  const reflection::Field* field{nullptr};
};

struct FbsFieldLookupCacheStore final {
  std::unordered_map<uint64_t, std::vector<FbsFieldLookupCacheEntry>> buckets;
  std::vector<std::pair<uint64_t, FbsFieldLookupCacheEntry>> insertion_order;
  size_t size{0};
  uint64_t last_hash{0};
  const reflection::Object* last_object{nullptr};
  std::string last_field_name;
  const reflection::Field* last_field{nullptr};
};

struct FbsDirectFieldRef final {
  const reflection::Field* field{nullptr};
  size_t value_idx{0};
};

struct FbsPathStep final {
  const reflection::Field* field{nullptr};
  const reflection::Object* next_obj{nullptr};
  size_t index{0};
  bool has_index{false};
};

struct FbsIndexedFieldRef final {
  std::vector<FbsPathStep> steps;
  size_t value_idx{0};
};
#endif

inline bool tokenize_field_path(std::string_view path, std::vector<FieldPathToken>& tokens) {
  auto is_ident_start = [](char ch) -> bool { return std::isalpha(static_cast<unsigned char>(ch)) != 0 || ch == '_'; };

  auto is_ident_char = [](char ch) -> bool { return std::isalnum(static_cast<unsigned char>(ch)) != 0 || ch == '_'; };

  tokens.clear();
  size_t pos = 0;
  bool expect_segment = true;

  while (pos < path.size()) {
    if VLIKELY (path[pos] == '.') {
      if VUNLIKELY (expect_segment) {
        tokens.clear();
        return false;
      }

      ++pos;
      expect_segment = true;
      continue;
    }

    if VUNLIKELY (path[pos] == '[') {
      if VUNLIKELY (tokens.empty() && !expect_segment) {
        tokens.clear();
        return false;
      }

      ++pos;
      size_t value = 0;
      bool has_digit = false;

      while (pos < path.size() && std::isdigit(static_cast<unsigned char>(path[pos])) != 0) {
        has_digit = true;
        value = value * 10 + static_cast<size_t>(path[pos] - '0');
        ++pos;
      }

      if VUNLIKELY (!has_digit || pos >= path.size() || path[pos] != ']') {
        tokens.clear();
        return false;
      }

      ++pos;
      tokens.push_back({true, {}, value});
      expect_segment = false;
      continue;
    }

    if VUNLIKELY (!expect_segment || !is_ident_start(path[pos])) {
      tokens.clear();
      return false;
    }

    size_t start = pos++;

    while (pos < path.size() && is_ident_char(path[pos])) {
      ++pos;
    }

    tokens.push_back({false, std::string(path.substr(start, pos - start)), 0});
    expect_segment = false;
  }

  if VUNLIKELY (expect_segment) {
    tokens.clear();
    return false;
  }

  return !tokens.empty();
}

inline const std::vector<FieldPathToken>* get_tokenized_field_path(std::string_view path) {
  thread_local FieldPathCacheStore cache;
  auto hash = std::hash<std::string_view>{}(path);

  if VLIKELY (cache.has_last_tokens && cache.last_hash == hash && cache.last_path == path) {
    return &cache.last_tokens;
  }

  auto bucket_iter = cache.buckets.find(hash);

  if VLIKELY (bucket_iter != cache.buckets.end()) {
    for (const auto& entry : bucket_iter->second) {
      if VLIKELY (entry.path == path) {
        cache.last_hash = hash;
        cache.last_path = entry.path;
        cache.last_tokens = entry.tokens;
        cache.has_last_tokens = true;
        return &cache.last_tokens;
      }
    }
  }

  FieldPathCacheEntry entry;
  entry.path.assign(path.data(), path.size());

  if VUNLIKELY (!tokenize_field_path(path, entry.tokens)) {
    return nullptr;
  }

  if VUNLIKELY (cache.size >= kFieldPathTokenCacheLimit && !cache.insertion_order.empty()) {
    const auto& oldest_entry = cache.insertion_order.front();
    const auto old_hash = oldest_entry.first;
    const auto& old_path = oldest_entry.second;
    auto old_bucket_iter = cache.buckets.find(old_hash);

    if VLIKELY (old_bucket_iter != cache.buckets.end()) {
      auto& old_bucket = old_bucket_iter->second;
      old_bucket.erase(std::remove_if(old_bucket.begin(), old_bucket.end(),
                                      [&old_path](const auto& item) { return item.path == old_path; }),
                       old_bucket.end());

      if VUNLIKELY (old_bucket.empty()) {
        cache.buckets.erase(old_bucket_iter);
      }
    }

    cache.insertion_order.erase(cache.insertion_order.begin());
    --cache.size;
  }

  auto& bucket = cache.buckets[hash];
  bucket.emplace_back(std::move(entry));
  cache.insertion_order.reserve(kFieldPathTokenCacheLimit);
  cache.insertion_order.emplace_back(hash, bucket.back().path);
  ++cache.size;
  cache.last_hash = hash;
  cache.last_path = bucket.back().path;
  cache.last_tokens = bucket.back().tokens;
  cache.has_last_tokens = true;
  return &cache.last_tokens;
}

inline const google::protobuf::FieldDescriptor* find_proto_field_cached(const google::protobuf::Descriptor& descriptor,
                                                                        std::string_view field_name) {
  thread_local ProtoFieldLookupCacheStore cache;
  auto hash =
      Helpers::hash_combine(reinterpret_cast<uintptr_t>(&descriptor), std::hash<std::string_view>{}(field_name));

  if VLIKELY (cache.last_descriptor == &descriptor && cache.last_hash == hash && cache.last_field_name == field_name) {
    return cache.last_field;
  }

  auto bucket_iter = cache.buckets.find(hash);

  if VLIKELY (bucket_iter != cache.buckets.end()) {
    for (const auto& entry : bucket_iter->second) {
      if VLIKELY (entry.descriptor == &descriptor && entry.field_name == field_name) {
        cache.last_hash = hash;
        cache.last_descriptor = &descriptor;
        cache.last_field_name = entry.field_name;
        cache.last_field = entry.field;
        return cache.last_field;
      }
    }
  }

  ProtoFieldLookupCacheEntry entry;
  entry.descriptor = &descriptor;
  entry.field_name.assign(field_name.data(), field_name.size());
  entry.field = descriptor.FindFieldByName(entry.field_name);

  if VUNLIKELY (cache.size >= kProtoFieldLookupCacheLimit && !cache.insertion_order.empty()) {
    const auto& oldest_entry = cache.insertion_order.front();
    const auto old_hash = oldest_entry.first;
    const auto& old_entry = oldest_entry.second;
    auto old_bucket_iter = cache.buckets.find(old_hash);

    if VLIKELY (old_bucket_iter != cache.buckets.end()) {
      auto& old_bucket = old_bucket_iter->second;
      old_bucket.erase(std::remove_if(old_bucket.begin(), old_bucket.end(),
                                      [&old_entry](const auto& item) {
                                        return item.descriptor == old_entry.descriptor &&
                                               item.field_name == old_entry.field_name;
                                      }),
                       old_bucket.end());

      if VUNLIKELY (old_bucket.empty()) {
        cache.buckets.erase(old_bucket_iter);
      }
    }

    cache.insertion_order.erase(cache.insertion_order.begin());
    --cache.size;
  }

  auto& bucket = cache.buckets[hash];
  bucket.emplace_back(std::move(entry));
  cache.insertion_order.reserve(kProtoFieldLookupCacheLimit);
  cache.insertion_order.emplace_back(hash, bucket.back());
  ++cache.size;
  cache.last_hash = hash;
  cache.last_descriptor = &descriptor;
  cache.last_field_name = bucket.back().field_name;
  cache.last_field = bucket.back().field;
  return cache.last_field;
}

inline std::vector<std::string> extract_bracket_paths_from_expression(std::string_view expression) {
  auto is_ident_start = [](char ch) -> bool { return std::isalpha(static_cast<unsigned char>(ch)) != 0 || ch == '_'; };

  auto is_ident_path_char = [](char ch) -> bool {
    return std::isalnum(static_cast<unsigned char>(ch)) != 0 || ch == '_' || ch == '.' || ch == '[' || ch == ']';
  };

  std::vector<std::string> paths;
  std::unordered_set<std::string> seen;

  for (size_t pos = 0; pos < expression.size();) {
    if VUNLIKELY (!is_ident_start(expression[pos])) {
      ++pos;
      continue;
    }

    size_t start = pos++;

    while (pos < expression.size() && is_ident_path_char(expression[pos])) {
      ++pos;
    }

    auto candidate = std::string(expression.substr(start, pos - start));

    if VLIKELY (candidate.find('[') == std::string::npos) {
      continue;
    }

    if VLIKELY (seen.emplace(candidate).second) {
      paths.emplace_back(std::move(candidate));
    }
  }

  return paths;
}

inline bool has_nested_field_path(std::string_view path) {
  return path.find('.') != std::string::npos || path.find('[') != std::string::npos;
}

#ifdef VLINK_HAS_PROTO_COMPILER
inline void collect_proto_descriptor_recursive(
    const google::protobuf::Descriptor* descriptor,
    std::unordered_map<std::string, const google::protobuf::Descriptor*>& descriptor_map) {
  if VUNLIKELY (!descriptor) {
    return;
  }

#if GOOGLE_PROTOBUF_VERSION >= 6030000
  descriptor_map[std::string(descriptor->full_name())] = descriptor;
#else
  descriptor_map[descriptor->full_name()] = descriptor;
#endif

  for (int i = 0; i < descriptor->nested_type_count(); ++i) {
    collect_proto_descriptor_recursive(descriptor->nested_type(i), descriptor_map);
  }
}

inline void collect_proto_file_descriptors(
    const google::protobuf::FileDescriptor* file_descriptor,
    std::unordered_map<std::string, const google::protobuf::Descriptor*>& descriptor_map) {
  if VUNLIKELY (!file_descriptor) {
    return;
  }

  for (int i = 0; i < file_descriptor->message_type_count(); ++i) {
    collect_proto_descriptor_recursive(file_descriptor->message_type(i), descriptor_map);
  }
}

inline void import_protos(
    google::protobuf::compiler::Importer* importer, const std::filesystem::path& root_dir,
    const std::filesystem::path& sub_dir, bool& has_import, int depth = 0,
    std::unordered_map<std::string, const google::protobuf::Descriptor*>* descriptor_map = nullptr) {
  if VUNLIKELY (depth >= 100) {
    return;
  }

  std::error_code ec;

  for (auto it = std::filesystem::directory_iterator(sub_dir, ec); !ec && it != std::filesystem::directory_iterator();
       it.increment(ec)) {
    const auto& entry = *it;
    std::error_code entry_ec;

    if VLIKELY (entry.is_regular_file(entry_ec) && !entry_ec && entry.path().extension() == ".proto") {
      auto rel_path = std::filesystem::relative(entry.path(), root_dir, entry_ec);

      if VUNLIKELY (entry_ec) {
        continue;
      }

      auto rel = Helpers::path_to_string(rel_path);
      std::replace(rel.begin(), rel.end(), '\\', '/');
      const auto* fd = importer->Import(rel);

      if VLIKELY (fd) {
        has_import = true;

        if VLIKELY (descriptor_map) {
          collect_proto_file_descriptors(fd, *descriptor_map);
        }
      }
    } else if VUNLIKELY (entry.is_directory(entry_ec) && !entry_ec) {
      import_protos(importer, root_dir, entry.path(), has_import, depth + 1, descriptor_map);
    }
  }

  if VUNLIKELY (ec) {
    MLOG_W("Failed to iterate proto directory '{}': {}", sub_dir.string(), ec.message());
  }
}
#endif

inline double resolve_nested_double(const google::protobuf::Message& msg, std::string_view path) {
  constexpr double kNotFound = std::numeric_limits<double>::quiet_NaN();
  const auto* current_msg = &msg;
  const auto* tokens = get_tokenized_field_path(path);

  if VUNLIKELY (!tokens) {
    return kNotFound;
  }

  for (size_t i = 0; i < tokens->size(); ++i) {
    if VUNLIKELY ((*tokens)[i].is_index) {
      return kNotFound;
    }

    const auto* desc = current_msg->GetDescriptor();
    const auto* ref = current_msg->GetReflection();
    const auto* field = find_proto_field_cached(*desc, (*tokens)[i].name);

    if VUNLIKELY (!field) {
      return kNotFound;
    }

    if VUNLIKELY (field->is_repeated()) {
      if VUNLIKELY (i + 1 >= tokens->size() || !(*tokens)[i + 1].is_index) {
        return kNotFound;
      }

      auto index = static_cast<int>((*tokens)[i + 1].index);

      if VUNLIKELY (index < 0 || index >= ref->FieldSize(*current_msg, field)) {
        return kNotFound;
      }

      if VLIKELY (field->cpp_type() == google::protobuf::FieldDescriptor::CPPTYPE_MESSAGE) {
        current_msg = &ref->GetRepeatedMessage(*current_msg, field, index);
        ++i;
        continue;
      }

      if VLIKELY (i + 1 != tokens->size() - 1) {
        return kNotFound;
      }

      switch (field->cpp_type()) {
        case google::protobuf::FieldDescriptor::CPPTYPE_DOUBLE:
          return ref->GetRepeatedDouble(*current_msg, field, index);
        case google::protobuf::FieldDescriptor::CPPTYPE_FLOAT:
          return static_cast<double>(ref->GetRepeatedFloat(*current_msg, field, index));
        case google::protobuf::FieldDescriptor::CPPTYPE_INT32:
          return static_cast<double>(ref->GetRepeatedInt32(*current_msg, field, index));
        case google::protobuf::FieldDescriptor::CPPTYPE_INT64:
          return static_cast<double>(ref->GetRepeatedInt64(*current_msg, field, index));
        case google::protobuf::FieldDescriptor::CPPTYPE_UINT32:
          return static_cast<double>(ref->GetRepeatedUInt32(*current_msg, field, index));
        case google::protobuf::FieldDescriptor::CPPTYPE_UINT64:
          return static_cast<double>(ref->GetRepeatedUInt64(*current_msg, field, index));
        case google::protobuf::FieldDescriptor::CPPTYPE_BOOL:
          return ref->GetRepeatedBool(*current_msg, field, index) ? 1.0 : 0.0;
        case google::protobuf::FieldDescriptor::CPPTYPE_ENUM:
          return static_cast<double>(ref->GetRepeatedEnumValue(*current_msg, field, index));
        default:
          return kNotFound;
      }
    }

    if VLIKELY (field->cpp_type() == google::protobuf::FieldDescriptor::CPPTYPE_MESSAGE) {
      current_msg = &ref->GetMessage(*current_msg, field);
      continue;
    }

    if VLIKELY (i != tokens->size() - 1) {
      return kNotFound;
    }

    switch (field->cpp_type()) {
      case google::protobuf::FieldDescriptor::CPPTYPE_DOUBLE:
        return ref->GetDouble(*current_msg, field);
      case google::protobuf::FieldDescriptor::CPPTYPE_FLOAT:
        return static_cast<double>(ref->GetFloat(*current_msg, field));
      case google::protobuf::FieldDescriptor::CPPTYPE_INT32:
        return static_cast<double>(ref->GetInt32(*current_msg, field));
      case google::protobuf::FieldDescriptor::CPPTYPE_INT64:
        return static_cast<double>(ref->GetInt64(*current_msg, field));
      case google::protobuf::FieldDescriptor::CPPTYPE_UINT32:
        return static_cast<double>(ref->GetUInt32(*current_msg, field));
      case google::protobuf::FieldDescriptor::CPPTYPE_UINT64:
        return static_cast<double>(ref->GetUInt64(*current_msg, field));
      case google::protobuf::FieldDescriptor::CPPTYPE_BOOL:
        return ref->GetBool(*current_msg, field) ? 1.0 : 0.0;
      case google::protobuf::FieldDescriptor::CPPTYPE_ENUM:
        return static_cast<double>(ref->GetEnumValue(*current_msg, field));
      default:
        return kNotFound;
    }
  }

  return kNotFound;
}

inline double safe_nested_double(const google::protobuf::Message& msg, std::string_view path) {
  auto val = resolve_nested_double(msg, path);
  return std::isnan(val) ? 0.0 : val;
}

inline std::string resolve_nested_string(const google::protobuf::Message& msg, std::string_view path,
                                         bool* found = nullptr) {
  const auto* current_msg = &msg;
  const auto* tokens = get_tokenized_field_path(path);

  if VLIKELY (found) {
    *found = false;
  }

  if VUNLIKELY (!tokens) {
    return {};
  }

  for (size_t i = 0; i < tokens->size(); ++i) {
    if VUNLIKELY ((*tokens)[i].is_index) {
      return {};
    }

    const auto* desc = current_msg->GetDescriptor();
    const auto* ref = current_msg->GetReflection();
    const auto* field = find_proto_field_cached(*desc, (*tokens)[i].name);

    if VUNLIKELY (!field) {
      return {};
    }

    if VUNLIKELY (field->is_repeated()) {
      if VUNLIKELY (i + 1 >= tokens->size() || !(*tokens)[i + 1].is_index) {
        return {};
      }

      auto index = static_cast<int>((*tokens)[i + 1].index);

      if VUNLIKELY (index < 0 || index >= ref->FieldSize(*current_msg, field)) {
        return {};
      }

      if VLIKELY (field->cpp_type() == google::protobuf::FieldDescriptor::CPPTYPE_MESSAGE) {
        current_msg = &ref->GetRepeatedMessage(*current_msg, field, index);
        ++i;
        continue;
      }

      if VLIKELY (i + 1 != tokens->size() - 1) {
        return {};
      }

      if VUNLIKELY (field->cpp_type() == google::protobuf::FieldDescriptor::CPPTYPE_STRING) {
        if VLIKELY (found) {
          *found = true;
        }
        return ref->GetRepeatedString(*current_msg, field, index);
      }

      return {};
    }

    if VLIKELY (field->cpp_type() == google::protobuf::FieldDescriptor::CPPTYPE_MESSAGE) {
      if VUNLIKELY (field->has_presence() && !ref->HasField(*current_msg, field)) {
        return {};
      }

      current_msg = &ref->GetMessage(*current_msg, field);
      continue;
    }

    if VLIKELY (i != tokens->size() - 1) {
      return {};
    }

    if VUNLIKELY (field->cpp_type() == google::protobuf::FieldDescriptor::CPPTYPE_STRING) {
      if VLIKELY (found) {
        *found = true;
      }
      return ref->GetString(*current_msg, field);
    }

    return {};
  }

  return {};
}

inline bool resolve_proto_message_path(const google::protobuf::Message& root, const std::vector<FieldPathToken>& tokens,
                                       size_t token_count, const google::protobuf::Message*& out_msg) {
  const auto* current_msg = &root;

  for (size_t i = 0; i < token_count; ++i) {
    if VUNLIKELY (tokens[i].is_index) {
      return false;
    }

    const auto* desc = current_msg->GetDescriptor();
    const auto* ref = current_msg->GetReflection();
    const auto* field = find_proto_field_cached(*desc, tokens[i].name);

    if VUNLIKELY (!field) {
      return false;
    }

    if VUNLIKELY (field->is_repeated()) {
      if VUNLIKELY (i + 1 >= token_count || !tokens[i + 1].is_index) {
        return false;
      }

      if VUNLIKELY (field->cpp_type() != google::protobuf::FieldDescriptor::CPPTYPE_MESSAGE) {
        return false;
      }

      auto index = static_cast<int>(tokens[i + 1].index);

      if VUNLIKELY (index < 0 || index >= ref->FieldSize(*current_msg, field)) {
        return false;
      }

      current_msg = &ref->GetRepeatedMessage(*current_msg, field, index);
      ++i;
      continue;
    }

    if VUNLIKELY (field->cpp_type() != google::protobuf::FieldDescriptor::CPPTYPE_MESSAGE) {
      return false;
    }

    if VUNLIKELY (field->has_presence() && !ref->HasField(*current_msg, field)) {
      return false;
    }

    current_msg = &ref->GetMessage(*current_msg, field);
  }

  out_msg = current_msg;
  return true;
}

inline bool resolve_proto_message_path(const google::protobuf::Message& root, const std::string& path,
                                       const google::protobuf::Message*& out_msg) {
  const auto* tokens = get_tokenized_field_path(path);

  if VUNLIKELY (!tokens) {
    return false;
  }

  return resolve_proto_message_path(root, *tokens, tokens->size(), out_msg);
}

inline bool resolve_proto_parent_field_path(const google::protobuf::Message& root, const std::string& path,
                                            const google::protobuf::Message*& out_parent, std::string& out_field) {
  const auto* tokens = get_tokenized_field_path(path);

  if VUNLIKELY (!tokens || tokens->empty() || tokens->back().is_index) {
    return false;
  }

  if VUNLIKELY (tokens->size() == 1U) {
    out_parent = &root;
    out_field = tokens->front().name;
    return true;
  }

  if VUNLIKELY (!resolve_proto_message_path(root, *tokens, tokens->size() - 1, out_parent)) {
    return false;
  }

  out_field = tokens->back().name;
  return true;
}

inline bool is_proto_numeric_type(google::protobuf::FieldDescriptor::CppType type) {
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

inline bool compile_proto_numeric_path(const google::protobuf::Descriptor& root_desc, std::string_view path,
                                       std::vector<ProtoPathStep>& steps) {
  const auto* tokens = get_tokenized_field_path(path);

  if VUNLIKELY (!tokens || tokens->empty()) {
    return false;
  }

  steps.clear();
  steps.reserve(tokens->size());

  const auto* current_desc = &root_desc;

  for (size_t i = 0; i < tokens->size(); ++i) {
    if VUNLIKELY ((*tokens)[i].is_index || !current_desc) {
      steps.clear();
      return false;
    }

    const auto* field = find_proto_field_cached(*current_desc, (*tokens)[i].name);

    if VUNLIKELY (!field) {
      steps.clear();
      return false;
    }

    ProtoPathStep step;
    step.field = field;

    if VUNLIKELY (field->is_repeated()) {
      if VUNLIKELY (i + 1 >= tokens->size() || !(*tokens)[i + 1].is_index) {
        steps.clear();
        return false;
      }

      step.has_index = true;
      step.index = (*tokens)[i + 1].index;
      steps.emplace_back(step);

      if VLIKELY (field->cpp_type() == google::protobuf::FieldDescriptor::CPPTYPE_MESSAGE) {
        current_desc = field->message_type();
        ++i;
        continue;
      }

      if VLIKELY (i + 1 == tokens->size() - 1 && is_proto_numeric_type(field->cpp_type())) {
        return true;
      }

      steps.clear();
      return false;
    }

    steps.emplace_back(step);

    if VLIKELY (field->cpp_type() == google::protobuf::FieldDescriptor::CPPTYPE_MESSAGE) {
      current_desc = field->message_type();
      continue;
    }

    if VLIKELY (i == tokens->size() - 1 && is_proto_numeric_type(field->cpp_type())) {
      return true;
    }

    steps.clear();
    return false;
  }

  steps.clear();
  return false;
}

inline double get_proto_numeric_value(const google::protobuf::Message& msg,
                                      const google::protobuf::FieldDescriptor* field) {
  const auto* ref = msg.GetReflection();

  switch (field->cpp_type()) {
    case google::protobuf::FieldDescriptor::CPPTYPE_DOUBLE:
      return ref->GetDouble(msg, field);
    case google::protobuf::FieldDescriptor::CPPTYPE_FLOAT:
      return static_cast<double>(ref->GetFloat(msg, field));
    case google::protobuf::FieldDescriptor::CPPTYPE_INT32:
      return static_cast<double>(ref->GetInt32(msg, field));
    case google::protobuf::FieldDescriptor::CPPTYPE_INT64:
      return static_cast<double>(ref->GetInt64(msg, field));
    case google::protobuf::FieldDescriptor::CPPTYPE_UINT32:
      return static_cast<double>(ref->GetUInt32(msg, field));
    case google::protobuf::FieldDescriptor::CPPTYPE_UINT64:
      return static_cast<double>(ref->GetUInt64(msg, field));
    case google::protobuf::FieldDescriptor::CPPTYPE_BOOL:
      return ref->GetBool(msg, field) ? 1.0 : 0.0;
    case google::protobuf::FieldDescriptor::CPPTYPE_ENUM:
      return static_cast<double>(ref->GetEnumValue(msg, field));
    default:
      return 0.0;
  }
}

inline double resolve_proto_numeric_path_fast(const google::protobuf::Message& root,
                                              const std::vector<ProtoPathStep>& steps) {
  constexpr double kNotFound = std::numeric_limits<double>::quiet_NaN();
  const auto* current_msg = &root;

  for (const auto& step : steps) {
    if VUNLIKELY (!current_msg || !step.field) {
      return kNotFound;
    }

    const auto* ref = current_msg->GetReflection();
    const auto* field = step.field;

    if VUNLIKELY (field->is_repeated()) {
      auto index = static_cast<int>(step.index);

      if VUNLIKELY (index < 0 || index >= ref->FieldSize(*current_msg, field)) {
        return kNotFound;
      }

      if VLIKELY (field->cpp_type() == google::protobuf::FieldDescriptor::CPPTYPE_MESSAGE) {
        current_msg = &ref->GetRepeatedMessage(*current_msg, field, index);
        continue;
      }

      switch (field->cpp_type()) {
        case google::protobuf::FieldDescriptor::CPPTYPE_DOUBLE:
          return ref->GetRepeatedDouble(*current_msg, field, index);
        case google::protobuf::FieldDescriptor::CPPTYPE_FLOAT:
          return static_cast<double>(ref->GetRepeatedFloat(*current_msg, field, index));
        case google::protobuf::FieldDescriptor::CPPTYPE_INT32:
          return static_cast<double>(ref->GetRepeatedInt32(*current_msg, field, index));
        case google::protobuf::FieldDescriptor::CPPTYPE_INT64:
          return static_cast<double>(ref->GetRepeatedInt64(*current_msg, field, index));
        case google::protobuf::FieldDescriptor::CPPTYPE_UINT32:
          return static_cast<double>(ref->GetRepeatedUInt32(*current_msg, field, index));
        case google::protobuf::FieldDescriptor::CPPTYPE_UINT64:
          return static_cast<double>(ref->GetRepeatedUInt64(*current_msg, field, index));
        case google::protobuf::FieldDescriptor::CPPTYPE_BOOL:
          return ref->GetRepeatedBool(*current_msg, field, index) ? 1.0 : 0.0;
        case google::protobuf::FieldDescriptor::CPPTYPE_ENUM:
          return static_cast<double>(ref->GetRepeatedEnumValue(*current_msg, field, index));
        default:
          return kNotFound;
      }
    }

    if VLIKELY (field->cpp_type() == google::protobuf::FieldDescriptor::CPPTYPE_MESSAGE) {
      if VUNLIKELY (field->has_presence() && !ref->HasField(*current_msg, field)) {
        return kNotFound;
      }

      current_msg = &ref->GetMessage(*current_msg, field);
      continue;
    }

    if VUNLIKELY (field->has_presence() && !ref->HasField(*current_msg, field)) {
      return kNotFound;
    }

    return get_proto_numeric_value(*current_msg, field);
  }

  return kNotFound;
}

inline void collect_nested_numeric_fields(const google::protobuf::Descriptor* desc, const std::string& prefix,
                                          std::vector<std::pair<std::string, std::string>>& out, int depth = 0) {
  if VUNLIKELY (depth > 5) {
    return;
  }

  for (int i = 0; i < desc->field_count(); ++i) {
    const auto* field = desc->field(i);

    if VUNLIKELY (field->is_repeated()) {
      continue;
    }

#if GOOGLE_PROTOBUF_VERSION >= 6030000
    auto field_name = field->name();
#else
    const auto& field_name = field->name();
#endif
    std::string dot_path = prefix;

    if VLIKELY (!prefix.empty()) {
      dot_path += ".";
    }

    dot_path += field_name;

    if VLIKELY (is_proto_numeric_type(field->cpp_type())) {
      if VLIKELY (!prefix.empty()) {
        out.emplace_back(dot_path, make_expression_variable_name("proto", out.size()));
      }
    } else if VLIKELY (field->cpp_type() == google::protobuf::FieldDescriptor::CPPTYPE_MESSAGE) {
      collect_nested_numeric_fields(field->message_type(), dot_path, out, depth + 1);
    }
  }
}

inline std::string preprocess_expression_dots(const std::string& expression,
                                              const std::vector<std::pair<std::string, std::string>>& nested_fields) {
  if VUNLIKELY (nested_fields.empty()) {
    return expression;
  }

  auto sorted = nested_fields;
  std::sort(sorted.begin(), sorted.end(), [](const auto& a, const auto& b) { return a.first.size() > b.first.size(); });

  std::string result = expression;

  for (const auto& [dot_path, var_name] : sorted) {
    size_t pos = 0;

    while ((pos = result.find(dot_path, pos)) != std::string::npos) {
      auto is_ident_char = [](char c) -> bool {
        return (std::isalnum(static_cast<unsigned char>(c)) != 0) || c == '_';
      };

      bool left_ok = (pos == 0) || !is_ident_char(result[pos - 1]);
      auto end_pos = pos + dot_path.size();
      bool right_ok = (end_pos >= result.size()) || (!is_ident_char(result[end_pos]) && result[end_pos] != '.');

      if VLIKELY (left_ok && right_ok) {
        result.replace(pos, dot_path.size(), var_name);
        pos += var_name.size();
      } else {
        pos += dot_path.size();
      }
    }
  }

  return result;
}

struct CachedExpression final {
#ifdef VLINK_ENABLE_EXPRTK
  vlink::ExprtkSymbolTable symbol_table;
  vlink::ExprtkExpression expr;
#endif
  std::vector<double> field_values;
  std::vector<ProtoDirectFieldRef> proto_direct_fields;
  std::vector<ProtoIndexedFieldRef> proto_indexed_field_refs;
#ifdef VLINK_HAS_FBS_COMPILER
  std::vector<FbsDirectFieldRef> fbs_direct_fields;
  std::vector<FbsIndexedFieldRef> fbs_indexed_field_refs;
#endif
  bool compiled{false};
};

inline constexpr size_t kExpressionCacheLimit = 256;

struct CachedExpressionStore final {
  std::unordered_map<std::string, CachedExpression> proto_cache;
  std::vector<std::string> proto_insertion_order;
  uint64_t proto_generation{0};
  std::string last_proto_expression;
  const google::protobuf::Descriptor* last_proto_desc{nullptr};
  CachedExpression* last_proto_cached{nullptr};
  uint64_t last_proto_generation{0};
#ifdef VLINK_HAS_FBS_COMPILER
  std::unordered_map<std::string, CachedExpression> fbs_cache;
  std::vector<std::string> fbs_insertion_order;
  uint64_t fbs_generation{0};
  std::string last_fbs_expression;
  const reflection::Object* last_fbs_obj{nullptr};
  CachedExpression* last_fbs_cached{nullptr};
  uint64_t last_fbs_generation{0};
#endif
};

inline CachedExpressionStore& get_expr_cache_store() {
  thread_local CachedExpressionStore store;
  return store;
}

#ifdef VLINK_ENABLE_EXPRTK

inline double evaluate_expression_with_msg(const std::string& expression, const google::protobuf::Message& msg) {
  if VLIKELY (expression.empty()) {
    return 0.0;
  }

  const auto* desc = msg.GetDescriptor();
  auto& cache_store = get_expr_cache_store();

  if VLIKELY (cache_store.last_proto_generation == cache_store.proto_generation &&
              cache_store.last_proto_expression == expression && cache_store.last_proto_desc == desc &&
              cache_store.last_proto_cached && cache_store.last_proto_cached->compiled) {
    auto& cached = *cache_store.last_proto_cached;

    for (const auto& field_ref : cached.proto_direct_fields) {
      cached.field_values[field_ref.value_idx] = get_proto_numeric_value(msg, field_ref.field);
    }

    for (const auto& field_ref : cached.proto_indexed_field_refs) {
      auto val = resolve_proto_numeric_path_fast(msg, field_ref.steps);
      cached.field_values[field_ref.value_idx] = std::isnan(val) ? 0.0 : val;
    }

    return cached.expr.value();
  }

#if GOOGLE_PROTOBUF_VERSION >= 6030000
  auto cache_key = expression + ":" + std::string(desc->full_name());
#else
  auto cache_key = expression + ":" + desc->full_name();
#endif
  cache_key += ":";
  cache_key += std::to_string(pointer_cache_identity(desc));

  auto& cache = cache_store.proto_cache;
  auto cache_iter = cache.find(cache_key);

  if VUNLIKELY (cache_iter == cache.end()) {
    if VUNLIKELY (cache.size() >= kExpressionCacheLimit) {
      if VLIKELY (!cache_store.proto_insertion_order.empty()) {
        cache.erase(cache_store.proto_insertion_order.front());
        cache_store.proto_insertion_order.erase(cache_store.proto_insertion_order.begin());
      } else {
        cache.clear();
      }

      ++cache_store.proto_generation;
    }

    cache_iter =
        cache.emplace(std::piecewise_construct, std::forward_as_tuple(cache_key), std::forward_as_tuple()).first;
    cache_store.proto_insertion_order.reserve(kExpressionCacheLimit);
    cache_store.proto_insertion_order.emplace_back(cache_key);
    ++cache_store.proto_generation;
    auto& cached = cache_iter->second;

    cached.symbol_table.add_constants();

    std::vector<std::pair<std::string, std::string>> nested_fields;
    collect_nested_numeric_fields(desc, "", nested_fields);

    auto indexed_paths = extract_bracket_paths_from_expression(expression);
    std::vector<std::pair<std::string, std::string>> replacement_paths = nested_fields;

    for (const auto& path : indexed_paths) {
      const auto* tokens = get_tokenized_field_path(path);

      if VUNLIKELY (!tokens) {
        continue;
      }

      const auto* current_desc = desc;
      bool valid_numeric = false;

      for (size_t i = 0; i < tokens->size(); ++i) {
        if VUNLIKELY ((*tokens)[i].is_index || !current_desc) {
          valid_numeric = false;
          break;
        }

        const auto* field = find_proto_field_cached(*current_desc, (*tokens)[i].name);

        if VUNLIKELY (!field) {
          valid_numeric = false;
          break;
        }

        if VUNLIKELY (field->is_repeated()) {
          if VUNLIKELY (i + 1 >= tokens->size() || !(*tokens)[i + 1].is_index) {
            valid_numeric = false;
            break;
          }

          if VLIKELY (field->cpp_type() == google::protobuf::FieldDescriptor::CPPTYPE_MESSAGE) {
            current_desc = field->message_type();
            ++i;
            continue;
          }

          valid_numeric = (i + 1 == tokens->size() - 1) && is_proto_numeric_type(field->cpp_type());
          break;
        }

        if VLIKELY (field->cpp_type() == google::protobuf::FieldDescriptor::CPPTYPE_MESSAGE) {
          current_desc = field->message_type();
          continue;
        }

        valid_numeric = (i == tokens->size() - 1) && is_proto_numeric_type(field->cpp_type());
        break;
      }

      if VUNLIKELY (!valid_numeric) {
        continue;
      }

      if VLIKELY (std::find_if(replacement_paths.begin(), replacement_paths.end(),
                               [&path](const auto& item) { return item.first == path; }) != replacement_paths.end()) {
        continue;
      }

      replacement_paths.emplace_back(path, make_expression_variable_name("proto", replacement_paths.size()));
    }

    cached.field_values.clear();
    cached.proto_direct_fields.clear();
    cached.proto_indexed_field_refs.clear();

    size_t proto_direct_count = 0;
    for (int i = 0; i < desc->field_count(); ++i) {
      const auto* field = desc->field(i);

      if VLIKELY (!field->is_repeated() && is_proto_numeric_type(field->cpp_type())) {
        ++proto_direct_count;
      }
    }
    cached.field_values.reserve(proto_direct_count + replacement_paths.size());

    for (int i = 0; i < desc->field_count(); ++i) {
      const auto* field = desc->field(i);

      if VUNLIKELY (field->is_repeated()) {
        continue;
      }

      if VLIKELY (is_proto_numeric_type(field->cpp_type())) {
        auto value_idx = cached.field_values.size();
        cached.field_values.emplace_back(0.0);
        cached.proto_direct_fields.push_back({field, value_idx});
#if GOOGLE_PROTOBUF_VERSION >= 6030000
        cached.symbol_table.add_variable(std::string(field->name()), cached.field_values[value_idx]);
#else
        cached.symbol_table.add_variable(field->name(), cached.field_values[value_idx]);
#endif
      }
    }

    for (const auto& [path, var_name] : replacement_paths) {
      ProtoIndexedFieldRef field_ref;

      if VUNLIKELY (!compile_proto_numeric_path(*desc, path, field_ref.steps)) {
        continue;
      }

      field_ref.value_idx = cached.field_values.size();
      cached.field_values.emplace_back(0.0);
      cached.symbol_table.add_variable(var_name, cached.field_values[field_ref.value_idx]);
      cached.proto_indexed_field_refs.emplace_back(std::move(field_ref));
    }

    cached.expr.register_symbol_table(cached.symbol_table);

    auto processed_expr = preprocess_expression_dots(expression, replacement_paths);

    if VUNLIKELY (!cached.expr.compile(processed_expr)) {
      MLOG_W("Failed to compile expression: {} (processed: {})", expression, processed_expr);
      return 0.0;
    }

    cached.compiled = true;
  }

  auto& cached = cache_iter->second;

  if VUNLIKELY (!cached.compiled) {
    return 0.0;
  }

  cache_store.last_proto_expression = expression;
  cache_store.last_proto_desc = desc;
  cache_store.last_proto_cached = &cached;
  cache_store.last_proto_generation = cache_store.proto_generation;

  for (const auto& field_ref : cached.proto_direct_fields) {
    cached.field_values[field_ref.value_idx] = get_proto_numeric_value(msg, field_ref.field);
  }

  for (const auto& field_ref : cached.proto_indexed_field_refs) {
    auto val = resolve_proto_numeric_path_fast(msg, field_ref.steps);
    cached.field_values[field_ref.value_idx] = std::isnan(val) ? 0.0 : val;
  }

  return cached.expr.value();
}

#else

inline double evaluate_expression_with_msg(const std::string& expression, const google::protobuf::Message& msg) {
  (void)expression;
  (void)msg;

  return 0.0;
}

#endif

#ifdef VLINK_HAS_FBS_COMPILER

inline void scan_fbs_files(const std::filesystem::path& dir, std::vector<std::filesystem::path>& files, int depth = 0) {
  if VUNLIKELY (depth >= 100) {
    return;
  }

  std::error_code ec;

  for (auto it = std::filesystem::directory_iterator(dir, ec); !ec && it != std::filesystem::directory_iterator();
       it.increment(ec)) {
    const auto& entry = *it;
    std::error_code entry_ec;

    if VLIKELY (entry.is_regular_file(entry_ec) && !entry_ec && entry.path().extension() == ".fbs") {
      files.emplace_back(entry.path());
    } else if VUNLIKELY (entry.is_directory(entry_ec) && !entry_ec) {
      scan_fbs_files(entry.path(), files, depth + 1);
    }
  }
}

inline const reflection::Field* find_fbs_field(const reflection::Object& obj, std::string_view field_name) {
  if VUNLIKELY (!obj.fields()) {
    return nullptr;
  }

  thread_local FbsFieldLookupCacheStore cache;
  auto hash = Helpers::hash_combine(reinterpret_cast<uintptr_t>(&obj), std::hash<std::string_view>{}(field_name));

  if VLIKELY (cache.last_object == &obj && cache.last_hash == hash && cache.last_field_name == field_name) {
    return cache.last_field;
  }

  auto bucket_iter = cache.buckets.find(hash);

  if VLIKELY (bucket_iter != cache.buckets.end()) {
    for (const auto& entry : bucket_iter->second) {
      if VLIKELY (entry.object == &obj && entry.field_name == field_name) {
        cache.last_hash = hash;
        cache.last_object = &obj;
        cache.last_field_name = entry.field_name;
        cache.last_field = entry.field;
        return cache.last_field;
      }
    }
  }

  FbsFieldLookupCacheEntry entry;
  entry.object = &obj;
  entry.field_name = field_name;
  entry.field = obj.fields()->LookupByKey(entry.field_name.c_str());

  if VUNLIKELY (cache.size >= kFbsFieldLookupCacheLimit && !cache.insertion_order.empty()) {
    const auto& oldest_entry = cache.insertion_order.front();
    const auto old_hash = oldest_entry.first;
    const auto& old_entry = oldest_entry.second;
    auto old_bucket_iter = cache.buckets.find(old_hash);

    if VLIKELY (old_bucket_iter != cache.buckets.end()) {
      auto& old_bucket = old_bucket_iter->second;
      old_bucket.erase(std::remove_if(old_bucket.begin(), old_bucket.end(),
                                      [&old_entry](const auto& item) {
                                        return item.object == old_entry.object &&
                                               item.field_name == old_entry.field_name;
                                      }),
                       old_bucket.end());

      if VUNLIKELY (old_bucket.empty()) {
        cache.buckets.erase(old_bucket_iter);
      }
    }

    cache.insertion_order.erase(cache.insertion_order.begin());
    --cache.size;
  }

  auto& bucket = cache.buckets[hash];
  bucket.emplace_back(std::move(entry));
  cache.insertion_order.reserve(kFbsFieldLookupCacheLimit);
  cache.insertion_order.emplace_back(hash, bucket.back());
  ++cache.size;
  cache.last_hash = hash;
  cache.last_object = &obj;
  cache.last_field_name = bucket.back().field_name;
  cache.last_field = bucket.back().field;
  return cache.last_field;
}

inline double resolve_nested_fbs_double(const flatbuffers::Table& table, const reflection::Object& obj,
                                        const reflection::Schema& schema, const std::string& path) {
  constexpr double kNotFound = std::numeric_limits<double>::quiet_NaN();
  const auto* current_table = &table;
  const auto* current_obj = &obj;
  const auto* tokens = get_tokenized_field_path(path);

  if VUNLIKELY (!tokens) {
    return kNotFound;
  }

  for (size_t i = 0; i < tokens->size(); ++i) {
    if VUNLIKELY ((*tokens)[i].is_index) {
      return kNotFound;
    }

    const auto* field = find_fbs_field(*current_obj, (*tokens)[i].name);

    if VUNLIKELY (!field) {
      return kNotFound;
    }

    if VLIKELY (field->type()->base_type() == reflection::Obj) {
      const auto* sub_table = flatbuffers::GetFieldT(*current_table, *field);

      if VUNLIKELY (!sub_table || !schema.objects()) {
        return kNotFound;
      }

      current_obj = schema.objects()->Get(static_cast<uint32_t>(field->type()->index()));

      if VUNLIKELY (!current_obj) {
        return kNotFound;
      }

      current_table = sub_table;
      continue;
    }

    if VUNLIKELY (field->type()->base_type() == reflection::Vector) {
      if VUNLIKELY (i + 1 >= tokens->size() || !(*tokens)[i + 1].is_index) {
        return kNotFound;
      }

      const auto* vec = flatbuffers::GetFieldAnyV(*current_table, *field);
      auto index = (*tokens)[i + 1].index;

      if VUNLIKELY (!vec || index >= vec->size()) {
        return kNotFound;
      }

      const auto elem_type = field->type()->element();

      if VLIKELY (elem_type == reflection::Obj) {
        const auto* sub_table = flatbuffers::GetAnyVectorElemPointer<const flatbuffers::Table>(vec, index);

        if VUNLIKELY (!sub_table || !schema.objects()) {
          return kNotFound;
        }

        current_obj = schema.objects()->Get(static_cast<uint32_t>(field->type()->index()));

        if VUNLIKELY (!current_obj) {
          return kNotFound;
        }

        current_table = sub_table;
        ++i;
        continue;
      }

      if VLIKELY (i + 1 != tokens->size() - 1) {
        return kNotFound;
      }

      switch (elem_type) {
        case reflection::Float:
        case reflection::Double:
          return flatbuffers::GetAnyVectorElemF(vec, elem_type, index);
        case reflection::Byte:
        case reflection::Short:
        case reflection::Int:
        case reflection::Long:
        case reflection::UByte:
        case reflection::UShort:
        case reflection::UInt:
        case reflection::ULong:
        case reflection::Bool:
          return static_cast<double>(flatbuffers::GetAnyVectorElemI(vec, elem_type, index));
        default:
          return kNotFound;
      }
    }

    if VLIKELY (i != tokens->size() - 1) {
      return kNotFound;
    }

    switch (field->type()->base_type()) {
      case reflection::Float:
      case reflection::Double:
        return flatbuffers::GetAnyFieldF(*current_table, *field);
      case reflection::Byte:
      case reflection::Short:
      case reflection::Int:
      case reflection::Long:
      case reflection::UByte:
      case reflection::UShort:
      case reflection::UInt:
      case reflection::ULong:
      case reflection::Bool:
        return static_cast<double>(flatbuffers::GetAnyFieldI(*current_table, *field));
      default:
        return kNotFound;
    }
  }

  return kNotFound;
}

inline double safe_nested_fbs_double(const flatbuffers::Table& table, const reflection::Object& obj,
                                     const reflection::Schema& schema, const std::string& path) {
  auto val = resolve_nested_fbs_double(table, obj, schema, path);
  return std::isnan(val) ? 0.0 : val;
}

inline std::string resolve_nested_fbs_string(const flatbuffers::Table& table, const reflection::Object& obj,
                                             const reflection::Schema& schema, const std::string& path,
                                             bool* found = nullptr) {
  const auto* current_table = &table;
  const auto* current_obj = &obj;
  const auto* tokens = get_tokenized_field_path(path);

  if VLIKELY (found) {
    *found = false;
  }

  if VUNLIKELY (!tokens) {
    return {};
  }

  for (size_t i = 0; i < tokens->size(); ++i) {
    if VUNLIKELY ((*tokens)[i].is_index) {
      return {};
    }

    const auto* field = find_fbs_field(*current_obj, (*tokens)[i].name);

    if VUNLIKELY (!field) {
      return {};
    }

    if VLIKELY (field->type()->base_type() == reflection::Obj) {
      const auto* sub_table = flatbuffers::GetFieldT(*current_table, *field);

      if VUNLIKELY (!sub_table || !schema.objects()) {
        return {};
      }

      current_obj = schema.objects()->Get(static_cast<uint32_t>(field->type()->index()));

      if VUNLIKELY (!current_obj) {
        return {};
      }

      current_table = sub_table;
      continue;
    }

    if VUNLIKELY (field->type()->base_type() == reflection::Vector) {
      if VUNLIKELY (i + 1 >= tokens->size() || !(*tokens)[i + 1].is_index) {
        return {};
      }

      const auto* vec = flatbuffers::GetFieldAnyV(*current_table, *field);
      auto index = (*tokens)[i + 1].index;

      if VUNLIKELY (!vec || index >= vec->size()) {
        return {};
      }

      const auto elem_type = field->type()->element();

      if VLIKELY (elem_type == reflection::Obj) {
        const auto* sub_table = flatbuffers::GetAnyVectorElemPointer<const flatbuffers::Table>(vec, index);

        if VUNLIKELY (!sub_table || !schema.objects()) {
          return {};
        }

        current_obj = schema.objects()->Get(static_cast<uint32_t>(field->type()->index()));

        if VUNLIKELY (!current_obj) {
          return {};
        }

        current_table = sub_table;
        ++i;
        continue;
      }

      if VLIKELY (i + 1 != tokens->size() - 1) {
        return {};
      }

      if VLIKELY (elem_type == reflection::String) {
        if VLIKELY (found) {
          *found = true;
        }
        return flatbuffers::GetAnyVectorElemS(vec, elem_type, index);
      }

      return {};
    }

    if VLIKELY (i != tokens->size() - 1) {
      return {};
    }

    if VLIKELY (field->type()->base_type() == reflection::String) {
      if VLIKELY (found) {
        *found = true;
      }
      return flatbuffers::GetAnyFieldS(*current_table, *field, nullptr);
    }

    return {};
  }

  return {};
}

inline bool resolve_fbs_table_path(const flatbuffers::Table& root_table, const reflection::Object& root_obj,
                                   const reflection::Schema& schema, const std::vector<FieldPathToken>& tokens,
                                   size_t token_count, const flatbuffers::Table*& out_table,
                                   const reflection::Object*& out_obj) {
  const auto* current_table = &root_table;
  const auto* current_obj = &root_obj;

  for (size_t i = 0; i < token_count; ++i) {
    if VUNLIKELY (tokens[i].is_index) {
      return false;
    }

    const auto* field = find_fbs_field(*current_obj, tokens[i].name);

    if VUNLIKELY (!field) {
      return false;
    }

    if VLIKELY (field->type()->base_type() == reflection::Obj) {
      const auto* sub_table = flatbuffers::GetFieldT(*current_table, *field);

      if VUNLIKELY (!sub_table || !schema.objects()) {
        return false;
      }

      current_obj = schema.objects()->Get(static_cast<uint32_t>(field->type()->index()));

      if VUNLIKELY (!current_obj) {
        return false;
      }

      current_table = sub_table;
      continue;
    }

    if VUNLIKELY (field->type()->base_type() == reflection::Vector) {
      if VUNLIKELY (i + 1 >= token_count || !tokens[i + 1].is_index) {
        return false;
      }

      if VUNLIKELY (field->type()->element() != reflection::Obj || !schema.objects()) {
        return false;
      }

      const auto* vec = flatbuffers::GetFieldAnyV(*current_table, *field);
      auto index = tokens[i + 1].index;

      if VUNLIKELY (!vec || index >= vec->size()) {
        return false;
      }

      const auto* sub_table = flatbuffers::GetAnyVectorElemPointer<const flatbuffers::Table>(vec, index);

      if VUNLIKELY (!sub_table) {
        return false;
      }

      current_obj = schema.objects()->Get(static_cast<uint32_t>(field->type()->index()));

      if VUNLIKELY (!current_obj) {
        return false;
      }

      current_table = sub_table;
      ++i;
      continue;
    }

    return false;
  }

  out_table = current_table;
  out_obj = current_obj;
  return true;
}

inline bool resolve_fbs_table_path(const flatbuffers::Table& root_table, const reflection::Object& root_obj,
                                   const reflection::Schema& schema, const std::string& path,
                                   const flatbuffers::Table*& out_table, const reflection::Object*& out_obj) {
  const auto* tokens = get_tokenized_field_path(path);

  if VUNLIKELY (!tokens) {
    return false;
  }

  return resolve_fbs_table_path(root_table, root_obj, schema, *tokens, tokens->size(), out_table, out_obj);
}

inline bool resolve_fbs_parent_field_path(const flatbuffers::Table& root_table, const reflection::Object& root_obj,
                                          const reflection::Schema& schema, const std::string& path,
                                          const flatbuffers::Table*& out_parent,
                                          const reflection::Object*& out_parent_obj, std::string& out_field) {
  const auto* tokens = get_tokenized_field_path(path);

  if VUNLIKELY (!tokens || tokens->empty() || tokens->back().is_index) {
    return false;
  }

  if VUNLIKELY (tokens->size() == 1U) {
    out_parent = &root_table;
    out_parent_obj = &root_obj;
    out_field = tokens->front().name;
    return true;
  }

  if VUNLIKELY (!resolve_fbs_table_path(root_table, root_obj, schema, *tokens, tokens->size() - 1, out_parent,
                                        out_parent_obj)) {
    return false;
  }

  out_field = tokens->back().name;
  return true;
}

inline bool is_fbs_numeric_type(reflection::BaseType type) {
  switch (type) {
    case reflection::Float:
    case reflection::Double:
    case reflection::Byte:
    case reflection::Short:
    case reflection::Int:
    case reflection::Long:
    case reflection::UByte:
    case reflection::UShort:
    case reflection::UInt:
    case reflection::ULong:
    case reflection::Bool:
      return true;
    default:
      return false;
  }
}

inline double get_fbs_field_as_double(const flatbuffers::Table& table, const reflection::Field& field) {
  switch (field.type()->base_type()) {
    case reflection::Float:
    case reflection::Double:
      return flatbuffers::GetAnyFieldF(table, field);
    case reflection::Byte:
    case reflection::Short:
    case reflection::Int:
    case reflection::Long:
    case reflection::UByte:
    case reflection::UShort:
    case reflection::UInt:
    case reflection::ULong:
    case reflection::Bool:
      return static_cast<double>(flatbuffers::GetAnyFieldI(table, field));
    default:
      return 0.0;
  }
}

inline bool compile_fbs_numeric_path(const reflection::Object& root_obj, const reflection::Schema& schema,
                                     std::string_view path, std::vector<FbsPathStep>& steps) {
  const auto* tokens = get_tokenized_field_path(path);

  if VUNLIKELY (!tokens || tokens->empty()) {
    return false;
  }

  steps.clear();
  steps.reserve(tokens->size());

  const reflection::Object* current_obj = &root_obj;

  for (size_t i = 0; i < tokens->size(); ++i) {
    if VUNLIKELY ((*tokens)[i].is_index || !current_obj) {
      steps.clear();
      return false;
    }

    const auto* field = find_fbs_field(*current_obj, (*tokens)[i].name);

    if VUNLIKELY (!field) {
      steps.clear();
      return false;
    }

    FbsPathStep step;
    step.field = field;

    if VUNLIKELY (field->type()->base_type() == reflection::Vector) {
      if VUNLIKELY (i + 1 >= tokens->size() || !(*tokens)[i + 1].is_index) {
        steps.clear();
        return false;
      }

      step.has_index = true;
      step.index = (*tokens)[i + 1].index;

      const auto elem_type = field->type()->element();

      if VLIKELY (elem_type == reflection::Obj && schema.objects()) {
        step.next_obj = schema.objects()->Get(static_cast<uint32_t>(field->type()->index()));

        if VUNLIKELY (!step.next_obj) {
          steps.clear();
          return false;
        }

        steps.emplace_back(step);
        current_obj = step.next_obj;
        ++i;
        continue;
      }

      if VLIKELY (i + 1 == tokens->size() - 1 && is_fbs_numeric_type(elem_type)) {
        steps.emplace_back(step);
        return true;
      }

      steps.clear();
      return false;
    }

    if VLIKELY (field->type()->base_type() == reflection::Obj && schema.objects()) {
      step.next_obj = schema.objects()->Get(static_cast<uint32_t>(field->type()->index()));

      if VUNLIKELY (!step.next_obj) {
        steps.clear();
        return false;
      }

      steps.emplace_back(step);
      current_obj = step.next_obj;
      continue;
    }

    if VLIKELY (i == tokens->size() - 1 && is_fbs_numeric_type(field->type()->base_type())) {
      steps.emplace_back(step);
      return true;
    }

    steps.clear();
    return false;
  }

  steps.clear();
  return false;
}

inline double resolve_fbs_numeric_path_fast(const flatbuffers::Table& root_table,
                                            const std::vector<FbsPathStep>& steps) {
  constexpr double kNotFound = std::numeric_limits<double>::quiet_NaN();
  const auto* current_table = &root_table;

  for (const auto& step : steps) {
    if VUNLIKELY (!current_table || !step.field) {
      return kNotFound;
    }

    const auto* field = step.field;

    if VUNLIKELY (field->type()->base_type() == reflection::Vector) {
      const auto* vec = flatbuffers::GetFieldAnyV(*current_table, *field);
      auto index = step.index;

      if VUNLIKELY (!vec || index >= vec->size()) {
        return kNotFound;
      }

      const auto elem_type = field->type()->element();

      if VLIKELY (elem_type == reflection::Obj) {
        const auto* sub_table = flatbuffers::GetAnyVectorElemPointer<const flatbuffers::Table>(vec, index);

        if VUNLIKELY (!sub_table) {
          return kNotFound;
        }

        current_table = sub_table;
        continue;
      }

      switch (elem_type) {
        case reflection::Float:
        case reflection::Double:
          return flatbuffers::GetAnyVectorElemF(vec, elem_type, index);
        case reflection::Byte:
        case reflection::Short:
        case reflection::Int:
        case reflection::Long:
        case reflection::UByte:
        case reflection::UShort:
        case reflection::UInt:
        case reflection::ULong:
        case reflection::Bool:
          return static_cast<double>(flatbuffers::GetAnyVectorElemI(vec, elem_type, index));
        default:
          return kNotFound;
      }
    }

    if VLIKELY (field->type()->base_type() == reflection::Obj) {
      const auto* sub_table = flatbuffers::GetFieldT(*current_table, *field);

      if VUNLIKELY (!sub_table) {
        return kNotFound;
      }

      current_table = sub_table;
      continue;
    }

    return get_fbs_field_as_double(*current_table, *field);
  }

  return kNotFound;
}

inline void collect_nested_numeric_fbs_fields(const reflection::Object& obj, const reflection::Schema& schema,
                                              const std::string& prefix,
                                              std::vector<std::pair<std::string, std::string>>& out, int depth = 0) {
  if VUNLIKELY (depth > 5 || !obj.fields()) {
    return;
  }

  for (unsigned i = 0; i < obj.fields()->size(); ++i) {
    const auto* field = obj.fields()->Get(i);

    if VUNLIKELY (!field) {
      continue;
    }

    if VUNLIKELY (field->type()->base_type() == reflection::Vector) {
      continue;
    }

    std::string field_name = field->name()->str();

    // NOLINTNEXTLINE(performance-inefficient-string-concatenation)
    std::string dot_path = prefix.empty() ? field_name : (prefix + "." + field_name);

    if VLIKELY (is_fbs_numeric_type(field->type()->base_type())) {
      if VLIKELY (!prefix.empty()) {
        out.emplace_back(dot_path, make_expression_variable_name("fbs", out.size()));
      }
    } else if VLIKELY (field->type()->base_type() == reflection::Obj && schema.objects()) {
      const auto* sub_obj = schema.objects()->Get(static_cast<uint32_t>(field->type()->index()));

      if VLIKELY (sub_obj) {
        collect_nested_numeric_fbs_fields(*sub_obj, schema, dot_path, out, depth + 1);
      }
    }
  }
}

#ifdef VLINK_ENABLE_EXPRTK

inline double evaluate_expression_with_fbs(const std::string& expression, const flatbuffers::Table& table,
                                           const reflection::Object& obj, const reflection::Schema& schema) {
  if VLIKELY (expression.empty()) {
    return 0.0;
  }

  const auto* obj_name = obj.name();

  if VUNLIKELY (!obj_name) {
    return 0.0;
  }

  auto& cache_store = get_expr_cache_store();

  if VLIKELY (cache_store.last_fbs_generation == cache_store.fbs_generation &&
              cache_store.last_fbs_expression == expression && cache_store.last_fbs_obj == &obj &&
              cache_store.last_fbs_cached && cache_store.last_fbs_cached->compiled) {
    auto& cached = *cache_store.last_fbs_cached;

    for (const auto& field_ref : cached.fbs_direct_fields) {
      cached.field_values[field_ref.value_idx] = get_fbs_field_as_double(table, *field_ref.field);
    }

    for (const auto& field_ref : cached.fbs_indexed_field_refs) {
      auto val = resolve_fbs_numeric_path_fast(table, field_ref.steps);
      cached.field_values[field_ref.value_idx] = std::isnan(val) ? 0.0 : val;
    }

    return cached.expr.value();
  }

  auto cache_key = expression + ":fbs:" + obj_name->str() + ":" + std::to_string(pointer_cache_identity(&schema)) +
                   ":" + std::to_string(pointer_cache_identity(&obj)) + ":" +
                   std::to_string(fbs_schema_cache_identity(schema));

  auto& cache = cache_store.fbs_cache;
  auto cache_iter = cache.find(cache_key);

  if VUNLIKELY (cache_iter == cache.end()) {
    if VUNLIKELY (cache.size() >= kExpressionCacheLimit) {
      if VLIKELY (!cache_store.fbs_insertion_order.empty()) {
        cache.erase(cache_store.fbs_insertion_order.front());
        cache_store.fbs_insertion_order.erase(cache_store.fbs_insertion_order.begin());
      } else {
        cache.clear();
      }

      ++cache_store.fbs_generation;
    }

    cache_iter =
        cache.emplace(std::piecewise_construct, std::forward_as_tuple(cache_key), std::forward_as_tuple()).first;
    cache_store.fbs_insertion_order.reserve(kExpressionCacheLimit);
    cache_store.fbs_insertion_order.emplace_back(cache_key);
    ++cache_store.fbs_generation;
    auto& cached = cache_iter->second;

    cached.symbol_table.add_constants();

    std::vector<std::pair<std::string, std::string>> nested_fields;
    collect_nested_numeric_fbs_fields(obj, schema, "", nested_fields);
    auto indexed_paths = extract_bracket_paths_from_expression(expression);
    std::vector<std::pair<std::string, std::string>> replacement_paths = nested_fields;

    for (const auto& path : indexed_paths) {
      const auto* tokens = get_tokenized_field_path(path);

      if VUNLIKELY (!tokens) {
        continue;
      }

      const reflection::Object* current_obj = &obj;
      bool valid_numeric = false;

      for (size_t i = 0; i < tokens->size(); ++i) {
        if VUNLIKELY ((*tokens)[i].is_index || !current_obj) {
          valid_numeric = false;
          break;
        }

        const auto* field = find_fbs_field(*current_obj, (*tokens)[i].name);

        if VUNLIKELY (!field) {
          valid_numeric = false;
          break;
        }

        if VUNLIKELY (field->type()->base_type() == reflection::Vector) {
          if VUNLIKELY (i + 1 >= tokens->size() || !(*tokens)[i + 1].is_index) {
            valid_numeric = false;
            break;
          }

          const auto elem_type = field->type()->element();

          if VLIKELY (elem_type == reflection::Obj && schema.objects()) {
            current_obj = schema.objects()->Get(static_cast<uint32_t>(field->type()->index()));
            ++i;
            continue;
          }

          valid_numeric = (i + 1 == tokens->size() - 1) && is_fbs_numeric_type(elem_type);
          break;
        }

        if VLIKELY (field->type()->base_type() == reflection::Obj && schema.objects()) {
          current_obj = schema.objects()->Get(static_cast<uint32_t>(field->type()->index()));
          continue;
        }

        valid_numeric = (i == tokens->size() - 1) && is_fbs_numeric_type(field->type()->base_type());
        break;
      }

      if VUNLIKELY (!valid_numeric) {
        continue;
      }

      if VLIKELY (std::find_if(replacement_paths.begin(), replacement_paths.end(),
                               [&path](const auto& item) { return item.first == path; }) != replacement_paths.end()) {
        continue;
      }

      replacement_paths.emplace_back(path, make_expression_variable_name("fbs", replacement_paths.size()));
    }

    cached.field_values.clear();
    cached.fbs_direct_fields.clear();
    cached.fbs_indexed_field_refs.clear();

    size_t fbs_direct_count = 0;

    if VLIKELY (obj.fields()) {
      for (unsigned i = 0; i < obj.fields()->size(); ++i) {
        const auto* f = obj.fields()->Get(i);
        if VLIKELY (f && is_fbs_numeric_type(f->type()->base_type())) {
          ++fbs_direct_count;
        }
      }
    }

    cached.field_values.reserve(fbs_direct_count + replacement_paths.size());

    if VLIKELY (obj.fields()) {
      for (unsigned i = 0; i < obj.fields()->size(); ++i) {
        const auto* f = obj.fields()->Get(i);

        if VUNLIKELY (!f || !is_fbs_numeric_type(f->type()->base_type())) {
          continue;
        }

        auto value_idx = cached.field_values.size();
        cached.field_values.emplace_back(0.0);
        cached.fbs_direct_fields.push_back({f, value_idx});
        cached.symbol_table.add_variable(f->name()->str(), cached.field_values[value_idx]);
      }
    }

    for (const auto& [path, var_name] : replacement_paths) {
      FbsIndexedFieldRef field_ref;

      if VUNLIKELY (!compile_fbs_numeric_path(obj, schema, path, field_ref.steps)) {
        continue;
      }

      field_ref.value_idx = cached.field_values.size();
      cached.field_values.emplace_back(0.0);
      cached.symbol_table.add_variable(var_name, cached.field_values[field_ref.value_idx]);
      cached.fbs_indexed_field_refs.emplace_back(std::move(field_ref));
    }

    cached.expr.register_symbol_table(cached.symbol_table);

    auto processed_expr = preprocess_expression_dots(expression, replacement_paths);

    if VUNLIKELY (!cached.expr.compile(processed_expr)) {
      MLOG_W("Failed to compile FBS expression: {} (processed: {})", expression, processed_expr);
      return 0.0;
    }

    cached.compiled = true;
  }

  auto& cached = cache_iter->second;

  if VUNLIKELY (!cached.compiled) {
    return 0.0;
  }

  cache_store.last_fbs_expression = expression;
  cache_store.last_fbs_obj = &obj;
  cache_store.last_fbs_cached = &cached;
  cache_store.last_fbs_generation = cache_store.fbs_generation;

  for (const auto& field_ref : cached.fbs_direct_fields) {
    cached.field_values[field_ref.value_idx] = get_fbs_field_as_double(table, *field_ref.field);
  }

  for (const auto& field_ref : cached.fbs_indexed_field_refs) {
    auto val = resolve_fbs_numeric_path_fast(table, field_ref.steps);
    cached.field_values[field_ref.value_idx] = std::isnan(val) ? 0.0 : val;
  }

  return cached.expr.value();
}

#else

inline double evaluate_expression_with_fbs(const std::string& expression, const flatbuffers::Table& table,
                                           const reflection::Object& obj, const reflection::Schema& schema) {
  (void)expression;
  (void)table;
  (void)obj;
  (void)schema;

  return 0.0;
}

#endif

#endif

inline double get_proto_double(const google::protobuf::Message& msg, std::string_view field_name,
                               const FieldMapping& mapping) {
  if VUNLIKELY (!mapping.expression.empty()) {
    return evaluate_expression_with_msg(mapping.expression, msg);
  }

  if VUNLIKELY (has_nested_field_path(field_name)) {
    auto val = resolve_nested_double(msg, field_name);

    if VLIKELY (!std::isnan(val)) {
      return val;
    }

    double default_value = 0.0;

    if VLIKELY (try_parse_numeric_default(mapping, default_value)) {
      return default_value;
    }

    return 0.0;
  }

  const auto* desc = msg.GetDescriptor();
  const auto* ref = msg.GetReflection();
  const auto* field = find_proto_field_cached(*desc, field_name);

  double value = 0.0;

  if VLIKELY (field) {
    switch (field->cpp_type()) {
      case google::protobuf::FieldDescriptor::CPPTYPE_DOUBLE:
        value = ref->GetDouble(msg, field);
        break;
      case google::protobuf::FieldDescriptor::CPPTYPE_FLOAT:
        value = static_cast<double>(ref->GetFloat(msg, field));
        break;
      case google::protobuf::FieldDescriptor::CPPTYPE_INT32:
        value = static_cast<double>(ref->GetInt32(msg, field));
        break;
      case google::protobuf::FieldDescriptor::CPPTYPE_INT64:
        value = static_cast<double>(ref->GetInt64(msg, field));
        break;
      case google::protobuf::FieldDescriptor::CPPTYPE_UINT32:
        value = static_cast<double>(ref->GetUInt32(msg, field));
        break;
      case google::protobuf::FieldDescriptor::CPPTYPE_UINT64:
        value = static_cast<double>(ref->GetUInt64(msg, field));
        break;
      case google::protobuf::FieldDescriptor::CPPTYPE_BOOL:
        value = ref->GetBool(msg, field) ? 1.0 : 0.0;
        break;
      case google::protobuf::FieldDescriptor::CPPTYPE_ENUM:
        value = static_cast<double>(ref->GetEnumValue(msg, field));
        break;
      default:
        break;
    }
  } else {
    try_parse_numeric_default(mapping, value);
  }

  return value;
}

inline std::string get_proto_string(const google::protobuf::Message& msg, std::string_view field_name,
                                    const FieldMapping& mapping, bool* found = nullptr) {
  if VLIKELY (found) {
    *found = false;
  }

  if VUNLIKELY (!mapping.expression.empty()) {
    if VLIKELY (found) {
      *found = true;
    }

    return format_expression_string(evaluate_expression_with_msg(mapping.expression, msg));
  }

  if VUNLIKELY (has_nested_field_path(field_name)) {
    bool nested_found = false;
    auto val = resolve_nested_string(msg, field_name, &nested_found);

    if VLIKELY (nested_found) {
      if VLIKELY (found) {
        *found = true;
      }
      return val;
    }

    if VUNLIKELY (found && mapping.has_default_value) {
      *found = true;
    }

    return mapping.has_default_value ? mapping.default_value : std::string{};
  }

  const auto* desc = msg.GetDescriptor();
  const auto* ref = msg.GetReflection();
  const auto* field = find_proto_field_cached(*desc, field_name);

  if VLIKELY (field && field->cpp_type() == google::protobuf::FieldDescriptor::CPPTYPE_STRING) {
    if VUNLIKELY (field->has_presence() && !ref->HasField(msg, field)) {
      if VUNLIKELY (found && mapping.has_default_value) {
        *found = true;
      }
      return mapping.has_default_value ? mapping.default_value : std::string{};
    }

    if VLIKELY (found) {
      *found = true;
    }

    return ref->GetString(msg, field);
  }

  if VUNLIKELY (found && mapping.has_default_value) {
    *found = true;
  }

  return mapping.has_default_value ? mapping.default_value : std::string{};
}

inline Bytes get_proto_bytes(const google::protobuf::Message& msg, std::string_view field_name) {
  if VUNLIKELY (has_nested_field_path(field_name)) {
    bool found = false;
    auto val = resolve_nested_string(msg, field_name, &found);

    if VLIKELY (found) {
      return Bytes::deep_copy(reinterpret_cast<const uint8_t*>(val.data()), val.size());
    }

    return {};
  }

  const auto* desc = msg.GetDescriptor();
  const auto* ref = msg.GetReflection();
  const auto* field = find_proto_field_cached(*desc, field_name);

  if VLIKELY (field && field->cpp_type() == google::protobuf::FieldDescriptor::CPPTYPE_STRING) {
    if VUNLIKELY (field->has_presence() && !ref->HasField(msg, field)) {
      return {};
    }

    std::string scratch;
    const auto& val = ref->GetStringReference(msg, field, &scratch);

    if VUNLIKELY (&val == &scratch) {
      return Bytes::deep_copy(reinterpret_cast<const uint8_t*>(val.data()), val.size());
    }

    return Bytes::shallow_copy(reinterpret_cast<const uint8_t*>(val.data()), val.size());
  }

  return {};
}

inline int64_t saturating_mul_to_ns(int64_t value, int64_t factor) {
  if VLIKELY (factor == 1) {
    return value;
  }

  if VUNLIKELY (value > 0 && value > std::numeric_limits<int64_t>::max() / factor) {
    return std::numeric_limits<int64_t>::max();
  }

  if VUNLIKELY (value < 0 && value < std::numeric_limits<int64_t>::min() / factor) {
    return std::numeric_limits<int64_t>::min();
  }

  return value * factor;
}

inline int64_t saturating_mul_to_ns(uint64_t value, uint64_t factor) {
  if VLIKELY (factor == 1) {
    if VLIKELY (value <= static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) {
      return static_cast<int64_t>(value);
    }

    return std::numeric_limits<int64_t>::max();
  }

  if VUNLIKELY (value > static_cast<uint64_t>(std::numeric_limits<int64_t>::max()) / factor) {
    return std::numeric_limits<int64_t>::max();
  }

  return static_cast<int64_t>(value * factor);
}

inline int64_t timestamp_unit_to_ns_scale(std::string_view unit) {
  if VLIKELY (unit == "ns") {
    return 1;
  }

  if VLIKELY (unit == "us") {
    return 1000;
  }

  if VLIKELY (unit == "ms") {
    return 1000000;
  }

  if VLIKELY (unit == "s") {
    return 1000000000;
  }

  return 1000;
}

inline int64_t convert_timestamp_to_ns(int64_t value, std::string_view unit) {
  return saturating_mul_to_ns(value, timestamp_unit_to_ns_scale(unit));
}

inline int64_t convert_timestamp_to_ns(uint64_t value, std::string_view unit) {
  return saturating_mul_to_ns(value, static_cast<uint64_t>(timestamp_unit_to_ns_scale(unit)));
}

inline int64_t convert_timestamp_to_ns(double value, std::string_view unit) {
  if VUNLIKELY (!std::isfinite(value)) {
    return -1;
  }

  auto scale = static_cast<double>(timestamp_unit_to_ns_scale(unit));
  auto scaled = value * scale;

  if VUNLIKELY (scaled >= static_cast<double>(std::numeric_limits<int64_t>::max())) {
    return std::numeric_limits<int64_t>::max();
  }

  if VUNLIKELY (scaled < static_cast<double>(std::numeric_limits<int64_t>::min())) {
    return std::numeric_limits<int64_t>::min();
  }

  return static_cast<int64_t>(scaled);
}

inline int64_t extract_proto_timestamp_ns(const google::protobuf::Message& msg, const std::string& timestamp_field,
                                          std::string_view timestamp_unit) {
  const auto* current_msg = &msg;
  const auto* tokens = get_tokenized_field_path(timestamp_field);

  if VUNLIKELY (!tokens) {
    return -1;
  }

  for (size_t i = 0; i < tokens->size(); ++i) {
    if VUNLIKELY ((*tokens)[i].is_index) {
      return -1;
    }

    const auto* desc = current_msg->GetDescriptor();
    const auto* ref = current_msg->GetReflection();
    const auto* field = find_proto_field_cached(*desc, (*tokens)[i].name);

    if VUNLIKELY (!field) {
      return -1;
    }

    if VUNLIKELY (field->is_repeated()) {
      if VUNLIKELY (i + 1 >= tokens->size() || !(*tokens)[i + 1].is_index) {
        return -1;
      }

      auto index = static_cast<int>((*tokens)[i + 1].index);

      if VUNLIKELY (index < 0 || index >= ref->FieldSize(*current_msg, field)) {
        return -1;
      }

      if VLIKELY (field->cpp_type() == google::protobuf::FieldDescriptor::CPPTYPE_MESSAGE) {
        current_msg = &ref->GetRepeatedMessage(*current_msg, field, index);
        ++i;
        continue;
      }

      if VUNLIKELY (i + 1 != tokens->size() - 1) {
        return -1;
      }

      switch (field->cpp_type()) {
        case google::protobuf::FieldDescriptor::CPPTYPE_DOUBLE:
          return convert_timestamp_to_ns(ref->GetRepeatedDouble(*current_msg, field, index), timestamp_unit);
        case google::protobuf::FieldDescriptor::CPPTYPE_FLOAT:
          return convert_timestamp_to_ns(static_cast<double>(ref->GetRepeatedFloat(*current_msg, field, index)),
                                         timestamp_unit);
        case google::protobuf::FieldDescriptor::CPPTYPE_INT32:
          return convert_timestamp_to_ns(static_cast<int64_t>(ref->GetRepeatedInt32(*current_msg, field, index)),
                                         timestamp_unit);
        case google::protobuf::FieldDescriptor::CPPTYPE_INT64:
          return convert_timestamp_to_ns(ref->GetRepeatedInt64(*current_msg, field, index), timestamp_unit);
        case google::protobuf::FieldDescriptor::CPPTYPE_UINT32:
          return convert_timestamp_to_ns(static_cast<uint64_t>(ref->GetRepeatedUInt32(*current_msg, field, index)),
                                         timestamp_unit);
        case google::protobuf::FieldDescriptor::CPPTYPE_UINT64:
          return convert_timestamp_to_ns(ref->GetRepeatedUInt64(*current_msg, field, index), timestamp_unit);
        case google::protobuf::FieldDescriptor::CPPTYPE_BOOL:
          return convert_timestamp_to_ns(static_cast<int64_t>(ref->GetRepeatedBool(*current_msg, field, index)),
                                         timestamp_unit);
        case google::protobuf::FieldDescriptor::CPPTYPE_ENUM:
          return convert_timestamp_to_ns(static_cast<int64_t>(ref->GetRepeatedEnumValue(*current_msg, field, index)),
                                         timestamp_unit);
        default:
          return -1;
      }
    }

    if VLIKELY (field->cpp_type() == google::protobuf::FieldDescriptor::CPPTYPE_MESSAGE) {
      if VUNLIKELY (field->has_presence() && !ref->HasField(*current_msg, field)) {
        return -1;
      }

      current_msg = &ref->GetMessage(*current_msg, field);
      continue;
    }

    if VUNLIKELY (i != tokens->size() - 1) {
      return -1;
    }

    if VUNLIKELY (field->has_presence() && !ref->HasField(*current_msg, field)) {
      return -1;
    }

    switch (field->cpp_type()) {
      case google::protobuf::FieldDescriptor::CPPTYPE_DOUBLE:
        return convert_timestamp_to_ns(ref->GetDouble(*current_msg, field), timestamp_unit);
      case google::protobuf::FieldDescriptor::CPPTYPE_FLOAT:
        return convert_timestamp_to_ns(static_cast<double>(ref->GetFloat(*current_msg, field)), timestamp_unit);
      case google::protobuf::FieldDescriptor::CPPTYPE_INT32:
        return convert_timestamp_to_ns(static_cast<int64_t>(ref->GetInt32(*current_msg, field)), timestamp_unit);
      case google::protobuf::FieldDescriptor::CPPTYPE_INT64:
        return convert_timestamp_to_ns(ref->GetInt64(*current_msg, field), timestamp_unit);
      case google::protobuf::FieldDescriptor::CPPTYPE_UINT32:
        return convert_timestamp_to_ns(static_cast<uint64_t>(ref->GetUInt32(*current_msg, field)), timestamp_unit);
      case google::protobuf::FieldDescriptor::CPPTYPE_UINT64:
        return convert_timestamp_to_ns(ref->GetUInt64(*current_msg, field), timestamp_unit);
      case google::protobuf::FieldDescriptor::CPPTYPE_BOOL:
        return convert_timestamp_to_ns(static_cast<int64_t>(ref->GetBool(*current_msg, field)), timestamp_unit);
      case google::protobuf::FieldDescriptor::CPPTYPE_ENUM:
        return convert_timestamp_to_ns(static_cast<int64_t>(ref->GetEnumValue(*current_msg, field)), timestamp_unit);
      default:
        return -1;
    }
  }

  return -1;
}

#ifdef VLINK_HAS_FBS_COMPILER
inline int64_t extract_fbs_timestamp_ns(const flatbuffers::Table& table, const reflection::Object& obj,
                                        const reflection::Schema& schema, const std::string& timestamp_field,
                                        std::string_view timestamp_unit) {
  const auto* current_table = &table;
  const auto* current_obj = &obj;
  const auto* tokens = get_tokenized_field_path(timestamp_field);

  if VUNLIKELY (!tokens) {
    return -1;
  }

  for (size_t i = 0; i < tokens->size(); ++i) {
    if VUNLIKELY ((*tokens)[i].is_index) {
      return -1;
    }

    const auto* field = find_fbs_field(*current_obj, (*tokens)[i].name);

    if VUNLIKELY (!field) {
      return -1;
    }

    if VLIKELY (field->type()->base_type() == reflection::Obj) {
      const auto* sub_table = flatbuffers::GetFieldT(*current_table, *field);

      if VUNLIKELY (!sub_table || !schema.objects()) {
        return -1;
      }

      current_obj = schema.objects()->Get(static_cast<uint32_t>(field->type()->index()));

      if VUNLIKELY (!current_obj) {
        return -1;
      }

      current_table = sub_table;
      continue;
    }

    if VUNLIKELY (field->type()->base_type() == reflection::Vector) {
      if VUNLIKELY (i + 1 >= tokens->size() || !(*tokens)[i + 1].is_index) {
        return -1;
      }

      const auto* vec = flatbuffers::GetFieldAnyV(*current_table, *field);
      auto index = (*tokens)[i + 1].index;

      if VUNLIKELY (!vec || index >= vec->size()) {
        return -1;
      }

      const auto elem_type = field->type()->element();

      if VLIKELY (elem_type == reflection::Obj) {
        const auto* sub_table = flatbuffers::GetAnyVectorElemPointer<const flatbuffers::Table>(vec, index);

        if VUNLIKELY (!sub_table || !schema.objects()) {
          return -1;
        }

        current_obj = schema.objects()->Get(static_cast<uint32_t>(field->type()->index()));

        if VUNLIKELY (!current_obj) {
          return -1;
        }

        current_table = sub_table;
        ++i;
        continue;
      }

      if VUNLIKELY (i + 1 != tokens->size() - 1) {
        return -1;
      }

      switch (elem_type) {
        case reflection::Float:
        case reflection::Double:
          return convert_timestamp_to_ns(flatbuffers::GetAnyVectorElemF(vec, elem_type, index), timestamp_unit);
        case reflection::Byte:
        case reflection::Short:
        case reflection::Int:
        case reflection::Long:
        case reflection::UByte:
        case reflection::UShort:
        case reflection::UInt:
        case reflection::ULong:
        case reflection::Bool:
          return convert_timestamp_to_ns(flatbuffers::GetAnyVectorElemI(vec, elem_type, index), timestamp_unit);
        default:
          return -1;
      }
    }

    if VUNLIKELY (i != tokens->size() - 1) {
      return -1;
    }

    switch (field->type()->base_type()) {
      case reflection::Float:
      case reflection::Double:
        return convert_timestamp_to_ns(flatbuffers::GetAnyFieldF(*current_table, *field), timestamp_unit);
      case reflection::Byte:
      case reflection::Short:
      case reflection::Int:
      case reflection::Long:
      case reflection::UByte:
      case reflection::UShort:
      case reflection::UInt:
      case reflection::ULong:
      case reflection::Bool:
        return convert_timestamp_to_ns(flatbuffers::GetAnyFieldI(*current_table, *field), timestamp_unit);
      default:
        return -1;
    }
  }

  return -1;
}
#endif

}  // namespace webviz
}  // namespace vlink
