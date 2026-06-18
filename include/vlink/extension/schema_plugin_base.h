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

/**
 * @file schema_plugin_base.h
 * @brief Reference base class mapping @c SchemaPluginInterface onto Protobuf descriptors and a BFBS registry.
 *
 * @details
 * @c SchemaPluginBase is a header-only implementation that downstream schema plugins normally
 * derive from.  It supplies the canonical Protobuf and FlatBuffers backends so that custom
 * plugins only need to specialise version information and any extra metadata; behavioural
 * compatibility with the original protobuf-only runtime is preserved by funnelling every
 * Protobuf lookup through @c DescriptorPool::generated_pool() and serialising payloads as a
 * @c FileDescriptorSet captured from the linked descriptor graph.  FlatBuffers does not provide
 * an analogous global pool, so the concrete plugin is expected to register BFBS blobs through
 * the @c FlatbuffersRegistry singleton (or the @c VLINK_REGISTER_FLATBUFFERS macro) before any
 * lookups occur.
 *
 * | Implemented method               | Behaviour                                                            |
 * | -------------------------------- | -------------------------------------------------------------------- |
 * | @c search_schema()               | probe over Protobuf, FlatBuffers, and @c vlink::zerocopy::*          |
 * | @c get_all_schemas()             | import all known BFBS blobs and return the cached schema set         |
 * | @c search_protobuf_descriptor()  | cache and return descriptors from the generated Protobuf pool        |
 * | @c create_protobuf_message()     | build a dynamic message via @c DynamicMessageFactory and cache it    |
 * | @c search_flatbuffers_schema()   | verify the BFBS blob and return a stable @c reflection::Schema       |
 * | @c create_flatbuffers_parser()   | deserialise the BFBS blob into a parser and remember it for cleanup  |
 *
 * @par Integration diagram
 * @code
 *  +---------------------+        +-----------------------+        +--------------------------+
 *  | downstream plugin   | -----> | SchemaPluginBase      | -----> | DescriptorPool / generated|
 *  | (overrides version) |        | (this header)         |        +--------------------------+
 *  +---------------------+        |                       |        +--------------------------+
 *                                 |                       | -----> | FlatbuffersRegistry      |
 *                                 |                       |        | (static BFBS slots)      |
 *                                 +-----------------------+        +--------------------------+
 * @endcode
 */

#pragma once

#include <algorithm>
#include <cctype>
#include <cstring>
#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "../base/helpers.h"
#include "./schema_plugin_interface.h"

//
#include "./flatbuffers_registry.h"
#include "./protobuf_registry.h"

namespace vlink {

/**
 * @class SchemaPluginBase
 * @brief Reusable base implementation of @c SchemaPluginInterface for mixed Protobuf / FlatBuffers metadata.
 *
 * @details
 * Concrete plugins derive from this class and typically only override @c get_version_info().
 * The base class is header-only so that linked Protobuf descriptors and registered BFBS blobs
 * remain in the same translation unit as the plugin binary, which is required for the static
 * registries to publish the expected types.
 *
 * Internal caches are protected by a single mutex; all public methods are safe to call from
 * multiple threads.
 */
class SchemaPluginBase : public SchemaPluginInterface {
 protected:
  /**
   * @brief Initialises the schema caches and primes the VLink memory pool.
   */
  SchemaPluginBase();

  /**
   * @brief Releases cached dynamic messages and FlatBuffers parsers owned by the plugin.
   */
  ~SchemaPluginBase() override;

  /**
   * @brief Locates a schema by name within an optional family.
   *
   * @param name         Serialisation type or fully qualified message name.
   * @param schema_type  Family hint, or @c SchemaType::kUnknown for cross-family probing.
   * @return Cached or freshly imported @c SchemaData, or an empty record on miss.
   */
  [[nodiscard]] SchemaData search_schema(const std::string& name,
                                         SchemaType schema_type = SchemaType::kUnknown) override;

  /**
   * @brief Returns every cached schema, optionally restricted to a single family.
   *
   * @param schema_type  Family filter, or @c SchemaType::kUnknown for everything.
   * @return Snapshot vector of cached entries.
   */
  [[nodiscard]] std::vector<SchemaData> get_all_schemas(SchemaType schema_type = SchemaType::kUnknown) override;

  /**
   * @brief Returns the Protobuf descriptor for a fully qualified type name.
   *
   * @param name  Fully qualified Protobuf message name.
   * @return Opaque descriptor pointer, or @c nullptr when unknown.
   */
  [[nodiscard]] ProtobufDescriptorPtr search_protobuf_descriptor(const std::string& name) override;

  /**
   * @brief Returns a cached dynamic Protobuf message instance for the type.
   *
   * @param name  Fully qualified Protobuf message name.
   * @return Opaque message pointer, or @c nullptr when unknown.
   */
  [[nodiscard]] ProtobufMessagePtr create_protobuf_message(const std::string& name) override;

  /**
   * @brief Returns a verified FlatBuffers BFBS reflection schema.
   *
   * @param name  Fully qualified FlatBuffers root type name.
   * @return Opaque @c reflection::Schema pointer, or @c nullptr when unknown.
   */
  [[nodiscard]] FlatbuffersSchemaPtr search_flatbuffers_schema(const std::string& name) override;

  /**
   * @brief Returns a freshly constructed FlatBuffers parser for the named root type.
   *
   * @param name  Fully qualified FlatBuffers root type name.
   * @return Opaque @c flatbuffers::Parser pointer, or @c nullptr when unknown.
   */
  [[nodiscard]] FlatbuffersParserPtr create_flatbuffers_parser(const std::string& name) override;

 private:
  static std::string normalize_schema_encoding(std::string_view encoding);

  static bool is_flatbuffers_schema_type(std::string_view encoding);

  bool cache_schema_data_locked(const SchemaData& schema);

#ifdef VLINK_HAS_SCHEMA_PLUGIN_PROTOBUF
  ProtobufDescriptorPtr find_protobuf_descriptor_locked(const std::string& name);

  static SchemaData build_protobuf_schema_data(const google::protobuf::Descriptor& descriptor);
#endif

#ifdef VLINK_HAS_SCHEMA_PLUGIN_FLATBUFFERS
  void import_all_flatbuffers_schema_data_locked();

  bool import_flatbuffers_schema_data_locked(const std::string& name);

  const SchemaData* find_cached_flatbuffers_schema_locked(const std::string& name) const;

  void clear_flatbuffers_parser_cache_locked(const std::string& name);
#endif

#ifdef VLINK_HAS_SCHEMA_PLUGIN_PROTOBUF
  google::protobuf::DynamicMessageFactory factory_;
#endif
  std::unordered_map<std::string, ProtobufDescriptorPtr> protobuf_descriptor_map_;
  std::unordered_map<std::string, std::vector<SchemaData>> schema_map_;
  std::unordered_map<std::string, ProtobufMessagePtr> protobuf_message_map_;
  std::unordered_map<std::string, std::vector<FlatbuffersParserPtr>> flatbuffers_parser_map_;
  std::vector<FlatbuffersParserPtr> retired_flatbuffers_parsers_;
  std::unordered_map<std::string, std::unique_ptr<Bytes>> flatbuffers_schema_snapshots_;
  std::vector<std::unique_ptr<Bytes>> retired_flatbuffers_schema_snapshots_;
  std::mutex mtx_;

  VLINK_DISALLOW_COPY_AND_ASSIGN(SchemaPluginBase)
};

////////////////////////////////////////////////////////////////
/// Details
////////////////////////////////////////////////////////////////

inline SchemaPluginBase::SchemaPluginBase()
#ifdef VLINK_HAS_SCHEMA_PLUGIN_PROTOBUF
    : factory_(google::protobuf::DescriptorPool::generated_pool())
#endif
{
  Bytes::init_memory_pool();
}

inline SchemaPluginBase::~SchemaPluginBase() {
#ifdef VLINK_HAS_SCHEMA_PLUGIN_PROTOBUF
  for (auto& [name, ptr] : protobuf_message_map_) {
    (void)name;
    delete reinterpret_cast<google::protobuf::Message*>(ptr);
  }
#endif

  for (auto& [name, ptr_list] : flatbuffers_parser_map_) {
    (void)name;
#ifdef VLINK_HAS_SCHEMA_PLUGIN_FLATBUFFERS
    for (auto* ptr : ptr_list) {
      delete reinterpret_cast<flatbuffers::Parser*>(ptr);
    }
#endif
  }

#ifdef VLINK_HAS_SCHEMA_PLUGIN_FLATBUFFERS
  for (auto* ptr : retired_flatbuffers_parsers_) {
    delete reinterpret_cast<flatbuffers::Parser*>(ptr);
  }
#endif

  protobuf_message_map_.clear();
  flatbuffers_parser_map_.clear();
  retired_flatbuffers_parsers_.clear();
  flatbuffers_schema_snapshots_.clear();
  retired_flatbuffers_schema_snapshots_.clear();
}

inline SchemaData SchemaPluginBase::search_schema(const std::string& name, SchemaType schema_type) {
  std::lock_guard lock(mtx_);

  auto iter = schema_map_.find(name);

  if VLIKELY (iter != schema_map_.end()) {
    if (schema_type != SchemaType::kUnknown) {
      for (const auto& schema : iter->second) {
        if (schema.schema_type == schema_type) {
          return schema;
        }
      }
    }
  }

  SchemaData schema;
  schema.name = name;

  if (schema_type == SchemaType::kZeroCopy && Helpers::has_startwith(name, "vlink::zerocopy::")) {
    schema.encoding = "vlink_msg";
    schema.schema_type = SchemaType::kZeroCopy;
    (void)cache_schema_data_locked(schema);
    return schema;
  }

  if (schema_type == SchemaType::kUnknown) {
    std::vector<SchemaData> matches;
    matches.reserve(3);

    [[maybe_unused]] bool has_cached_protobuf = false;
    [[maybe_unused]] bool has_cached_flatbuffers = false;
    [[maybe_unused]] bool has_cached_zerocopy = false;

    if VLIKELY (iter != schema_map_.end()) {
      for (const auto& cached_schema : iter->second) {
        switch (cached_schema.schema_type) {
          case SchemaType::kProtobuf:
            has_cached_protobuf = true;
            break;
          case SchemaType::kFlatbuffers:
            has_cached_flatbuffers = true;
            break;
          case SchemaType::kZeroCopy:
            has_cached_zerocopy = true;
            break;
          default:
            break;
        }

        matches.emplace_back(cached_schema);
      }
    }

#ifdef VLINK_HAS_SCHEMA_PLUGIN_PROTOBUF

    if VUNLIKELY (!has_cached_protobuf) {
      if (auto* desc_ptr = find_protobuf_descriptor_locked(name); desc_ptr != nullptr) {
        auto* desc = reinterpret_cast<google::protobuf::Descriptor*>(desc_ptr);
        auto proto_schema = build_protobuf_schema_data(*desc);

        if VLIKELY (!proto_schema.encoding.empty()) {
          (void)cache_schema_data_locked(proto_schema);
          matches.emplace_back(std::move(proto_schema));
        }
      }
    }
#endif

#ifdef VLINK_HAS_SCHEMA_PLUGIN_FLATBUFFERS

    if VUNLIKELY (!has_cached_flatbuffers) {
      if (const auto* cached_schema = find_cached_flatbuffers_schema_locked(name)) {
        matches.emplace_back(*cached_schema);
      }
    }
#endif

    if (!has_cached_zerocopy && Helpers::has_startwith(name, "vlink::zerocopy::")) {
      SchemaData zerocopy_schema;
      zerocopy_schema.name = name;
      zerocopy_schema.encoding = "vlink_msg";
      zerocopy_schema.schema_type = SchemaType::kZeroCopy;
      (void)cache_schema_data_locked(zerocopy_schema);
      matches.emplace_back(std::move(zerocopy_schema));
    }

    if (matches.size() == 1) {
      return matches.front();
    }

    return {};
  }

#ifdef VLINK_HAS_SCHEMA_PLUGIN_PROTOBUF

  if (schema_type == SchemaType::kProtobuf) {
    auto* desc_ptr = find_protobuf_descriptor_locked(name);

    if (desc_ptr != nullptr) {
      auto* desc = reinterpret_cast<google::protobuf::Descriptor*>(desc_ptr);
      schema = build_protobuf_schema_data(*desc);

      if VLIKELY (!schema.encoding.empty()) {
        (void)cache_schema_data_locked(schema);
        return schema;
      }
    }
  }
#endif

#ifdef VLINK_HAS_SCHEMA_PLUGIN_FLATBUFFERS

  if (schema_type == SchemaType::kFlatbuffers) {
    if (const auto* cached_schema = find_cached_flatbuffers_schema_locked(name)) {
      return *cached_schema;
    }
  }
#endif

  return schema;
}

inline std::vector<SchemaData> SchemaPluginBase::get_all_schemas(SchemaType schema_type) {
  std::lock_guard lock(mtx_);

#ifdef VLINK_HAS_SCHEMA_PLUGIN_FLATBUFFERS

  if (schema_type == SchemaType::kUnknown || schema_type == SchemaType::kFlatbuffers) {
    import_all_flatbuffers_schema_data_locked();
  }
#endif

  size_t schema_count = 0;

  for (const auto& [name, items] : schema_map_) {
    (void)name;
    schema_count += items.size();
  }

  std::vector<SchemaData> schemas;
  schemas.reserve(schema_count);

  for (const auto& [name, items] : schema_map_) {
    (void)name;

    for (const auto& schema : items) {
      if (schema_type != SchemaType::kUnknown && schema.schema_type != schema_type) {
        continue;
      }

      schemas.emplace_back(schema);
    }
  }

  return schemas;
}

inline SchemaPluginInterface::ProtobufDescriptorPtr SchemaPluginBase::search_protobuf_descriptor(
    const std::string& name) {
  std::lock_guard lock(mtx_);

#ifdef VLINK_HAS_SCHEMA_PLUGIN_PROTOBUF
  return find_protobuf_descriptor_locked(name);
#else
  (void)name;
  return nullptr;
#endif
}

inline SchemaPluginInterface::ProtobufMessagePtr SchemaPluginBase::create_protobuf_message(const std::string& name) {
  std::lock_guard lock(mtx_);

  auto iter = protobuf_message_map_.find(name);

  if VLIKELY (iter != protobuf_message_map_.end()) {
    return iter->second;
  }

#ifdef VLINK_HAS_SCHEMA_PLUGIN_PROTOBUF
  auto* desc_ptr = find_protobuf_descriptor_locked(name);

  if VUNLIKELY (!desc_ptr) {
    return nullptr;
  }

  const auto* prototype = factory_.GetPrototype(reinterpret_cast<google::protobuf::Descriptor*>(desc_ptr));

  if VUNLIKELY (!prototype) {
    return nullptr;
  }

  auto* target_ptr = reinterpret_cast<ProtobufMessagePtr>(prototype->New());
  protobuf_message_map_.emplace(name, target_ptr);
  return target_ptr;
#else
  (void)name;
  return nullptr;
#endif
}

inline SchemaPluginInterface::FlatbuffersSchemaPtr SchemaPluginBase::search_flatbuffers_schema(
    const std::string& name) {
  std::lock_guard lock(mtx_);

#ifdef VLINK_HAS_SCHEMA_PLUGIN_FLATBUFFERS
  const auto* schema = find_cached_flatbuffers_schema_locked(name);

  if VUNLIKELY (!schema) {
    return nullptr;
  }

  flatbuffers::Verifier verifier(schema->data.data(), schema->data.size());

  if VUNLIKELY (!reflection::VerifySchemaBuffer(verifier)) {
    return nullptr;
  }

  auto snapshot_iter = flatbuffers_schema_snapshots_.find(name);

  if VLIKELY (snapshot_iter != flatbuffers_schema_snapshots_.end() && snapshot_iter->second &&
              snapshot_iter->second->size() == schema->data.size() &&
              std::memcmp(snapshot_iter->second->data(), schema->data.data(), schema->data.size()) == 0) {
    return reinterpret_cast<FlatbuffersSchemaPtr>(
        const_cast<reflection::Schema*>(reflection::GetSchema(snapshot_iter->second->data())));
  }

  auto snapshot = std::make_unique<Bytes>(Bytes::deep_copy(schema->data.data(), schema->data.size()));

  if VUNLIKELY (snapshot->empty()) {
    return nullptr;
  }

  const auto* snapshot_data = snapshot->data();

  if VLIKELY (snapshot_iter != flatbuffers_schema_snapshots_.end()) {
    if VLIKELY (snapshot_iter->second) {
      retired_flatbuffers_schema_snapshots_.emplace_back(std::move(snapshot_iter->second));
    }

    snapshot_iter->second = std::move(snapshot);
  } else {
    flatbuffers_schema_snapshots_.emplace(name, std::move(snapshot));
  }

  return reinterpret_cast<FlatbuffersSchemaPtr>(const_cast<reflection::Schema*>(reflection::GetSchema(snapshot_data)));
#else
  (void)name;
  return nullptr;
#endif
}

inline SchemaPluginInterface::FlatbuffersParserPtr SchemaPluginBase::create_flatbuffers_parser(
    const std::string& name) {
  std::lock_guard lock(mtx_);

#ifdef VLINK_HAS_SCHEMA_PLUGIN_FLATBUFFERS
  const auto* schema = find_cached_flatbuffers_schema_locked(name);

  if VUNLIKELY (!schema) {
    return nullptr;
  }

  auto parser = std::make_unique<flatbuffers::Parser>();

  if VUNLIKELY (!parser->Deserialize(reinterpret_cast<const uint8_t*>(schema->data.data()), schema->data.size())) {
    return nullptr;
  }

  if VUNLIKELY (!parser->SetRootType(name.c_str())) {
    return nullptr;
  }

  auto* target_ptr = reinterpret_cast<FlatbuffersParserPtr>(parser.get());
  flatbuffers_parser_map_[name].emplace_back(target_ptr);

  (void)parser.release();  // NOLINT(bugprone-unused-return-value)

  return target_ptr;
#else
  (void)name;
  return nullptr;
#endif
}

inline std::string SchemaPluginBase::normalize_schema_encoding(std::string_view encoding) {
  std::string normalized{encoding};
  std::transform(normalized.begin(), normalized.end(), normalized.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

  if (normalized == "proto") {
    return "protobuf";
  }

  if (normalized == "flatbuffer" || normalized == "flatbuffers" || normalized == "fbs" || normalized == "bfbs") {
    return "flatbuffers";
  }

  return normalized;
}

inline bool SchemaPluginBase::is_flatbuffers_schema_type(std::string_view encoding) {
  return normalize_schema_encoding(encoding) == "flatbuffers";
}

inline bool SchemaPluginBase::cache_schema_data_locked(const SchemaData& schema) {
  if VUNLIKELY (schema.name.empty() || schema.encoding.empty()) {
    return false;
  }

#ifdef VLINK_HAS_SCHEMA_PLUGIN_FLATBUFFERS

  if (is_flatbuffers_schema_type(schema.encoding)) {
    flatbuffers::Verifier verifier(schema.data.data(), schema.data.size());

    if VUNLIKELY (!reflection::VerifySchemaBuffer(verifier)) {
      return false;
    }

    clear_flatbuffers_parser_cache_locked(schema.name);
  }
#endif

  auto cached_schema_type = SchemaData::is_valid_type(schema.schema_type) ? schema.schema_type : SchemaType::kUnknown;

  if VUNLIKELY (!SchemaData::is_real_type(cached_schema_type)) {
    return false;
  }

  auto& cached_schemas = schema_map_[schema.name];

  for (auto& cached_schema : cached_schemas) {
    if (cached_schema.schema_type == cached_schema_type) {
      cached_schema = schema;
      return true;
    }
  }

  cached_schemas.emplace_back(schema);
  return true;
}

#ifdef VLINK_HAS_SCHEMA_PLUGIN_PROTOBUF
inline SchemaPluginInterface::ProtobufDescriptorPtr SchemaPluginBase::find_protobuf_descriptor_locked(
    const std::string& name) {
  auto iter = protobuf_descriptor_map_.find(name);

  if VLIKELY (iter != protobuf_descriptor_map_.end()) {
    return iter->second;
  }

  const auto* desc = google::protobuf::DescriptorPool::generated_pool()->FindMessageTypeByName(name);

  if VUNLIKELY (!desc) {
    return nullptr;
  }

  auto* target_ptr = reinterpret_cast<ProtobufDescriptorPtr>(const_cast<google::protobuf::Descriptor*>(desc));
  protobuf_descriptor_map_.emplace(name, target_ptr);
  return target_ptr;
}

inline SchemaData SchemaPluginBase::build_protobuf_schema_data(const google::protobuf::Descriptor& descriptor) {
  SchemaData schema;
  schema.name = descriptor.full_name();
  schema.encoding = "protobuf";
  schema.schema_type = SchemaType::kProtobuf;

  if VUNLIKELY (!descriptor.file()) {
    schema.encoding.clear();
    schema.schema_type = SchemaType::kUnknown;
    return schema;
  }

  google::protobuf::FileDescriptorSet proto_fd_set;
  std::queue<const google::protobuf::FileDescriptor*> to_add;

  to_add.push(descriptor.file());

#if GOOGLE_PROTOBUF_VERSION >= 6030000
  std::unordered_set<std::string_view> seen_dependencies;
#else
  std::unordered_set<std::string> seen_dependencies;
#endif

  seen_dependencies.insert(descriptor.file()->name());

  while (!to_add.empty()) {
    const auto* next = to_add.front();
    to_add.pop();

    next->CopyTo(proto_fd_set.add_file());

    for (int i = 0; i < next->dependency_count(); ++i) {
      const auto* dep = next->dependency(i);

      if (dep == nullptr || seen_dependencies.find(dep->name()) != seen_dependencies.end()) {
        continue;
      }

      seen_dependencies.insert(dep->name());
      to_add.push(dep);
    }
  }

  schema.data = Bytes::create(proto_fd_set.ByteSizeLong());

  if VUNLIKELY (!proto_fd_set.SerializeToArray(schema.data.data(), schema.data.size())) {
    schema.encoding.clear();
    schema.schema_type = SchemaType::kUnknown;
    schema.data.clear();
  }

  return schema;
}
#endif

#ifdef VLINK_HAS_SCHEMA_PLUGIN_FLATBUFFERS
inline void SchemaPluginBase::import_all_flatbuffers_schema_data_locked() {
  for (const auto& schema : FlatbuffersRegistry::get().get_all_schemas()) {
    (void)cache_schema_data_locked(schema);
  }
}

inline bool SchemaPluginBase::import_flatbuffers_schema_data_locked(const std::string& name) {
  auto schema = FlatbuffersRegistry::get().search_schema(name);

  if (!is_flatbuffers_schema_type(schema.encoding) || schema.data.empty()) {
    return false;
  }

  return cache_schema_data_locked(schema);
}

inline const SchemaData* SchemaPluginBase::find_cached_flatbuffers_schema_locked(const std::string& name) const {
  auto* self = const_cast<SchemaPluginBase*>(this);
  auto iter = self->schema_map_.find(name);

  if (iter == self->schema_map_.end()) {
    (void)self->import_flatbuffers_schema_data_locked(name);
    iter = self->schema_map_.find(name);
  }

  if (iter == self->schema_map_.end()) {
    return nullptr;
  }

  for (const auto& schema : iter->second) {
    if (schema.schema_type != SchemaType::kFlatbuffers) {
      continue;
    }

    if (!is_flatbuffers_schema_type(schema.encoding) || schema.data.empty()) {
      return nullptr;
    }

    return &schema;
  }

  return nullptr;
}

inline void SchemaPluginBase::clear_flatbuffers_parser_cache_locked(const std::string& name) {
  auto iter = flatbuffers_parser_map_.find(name);

  if (iter == flatbuffers_parser_map_.end()) {
    return;
  }

  retired_flatbuffers_parsers_.insert(retired_flatbuffers_parsers_.end(), iter->second.begin(), iter->second.end());
  flatbuffers_parser_map_.erase(iter);
}
#endif

}  // namespace vlink
