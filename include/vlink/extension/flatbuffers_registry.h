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
 * @file flatbuffers_registry.h
 * @brief Process-local lookup table for compiled-in FlatBuffers BFBS reflection blobs.
 *
 * @details
 * Protobuf ships with a built-in process-wide registry of generated descriptors
 * (@c google::protobuf::DescriptorPool::generated_pool()).  FlatBuffers offers no such
 * runtime pool, so VLink keeps a small singleton table that stores BFBS reflection
 * blobs embedded inside the running binary.  The schema plugin reads from this table
 * when @c BagWriter or the proxy frontends need to attach FlatBuffers schemas to
 * recorded URLs.
 *
 * The registry is intentionally minimal and never touches the filesystem -- it does not
 * read @c VLINK_FBS_DIR / @c VLINK_PROTO_DIR and does not own the BFBS buffer memory.
 * Three operations are supported:
 *
 * | API                                                              | Purpose                                       |
 * | ---------------------------------------------------------------- | --------------------------------------------- |
 * | @c register_schema<BinarySchema>(name)                           | Register a generated @c *BinarySchema helper  |
 * | @c register_schema(name, bfbs_data, bfbs_size)                   | Register raw BFBS bytes                       |
 * | @c search_schema(name)                                           | Look up one entry by root type name           |
 * | @c get_all_schemas()                                             | Enumerate every registered entry              |
 *
 * Type resolution flow:
 *
 * @verbatim
 *   generated *BinarySchema  --register_schema-->  +-------------------+
 *                                                  |   BFBS map        |
 *                                                  | (name -> Schema)  |
 *   user lookup (root type)  --search_schema--->   +-------------------+   --> SchemaData
 * @endverbatim
 *
 * @par Example
 * @code
 * #include <vlink/extension/flatbuffers_registry.h>
 * #include "helloworld/fbs/User_generated.h"
 *
 * // Auto-register at static initialisation time:
 * VLINK_REGISTER_FLATBUFFERS("Helloworld.fbs.User", Helloworld::fbs::UserBinarySchema);
 *
 * void inspect() {
 *   auto schema = vlink::FlatbuffersRegistry::get().search_schema("Helloworld.fbs.User");
 *   if (!schema.data.empty()) {
 *     // Use schema.data to populate bag metadata or webviz channel info.
 *   }
 * }
 * @endcode
 */

#pragma once

#include <cstddef>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "../impl/types.h"

#if __has_include(<flatbuffers/idl.h>)
#include <flatbuffers/flatbuffers.h>
#include <flatbuffers/idl.h>
#define VLINK_HAS_SCHEMA_PLUGIN_FLATBUFFERS
#endif

#ifdef VLINK_HAS_SCHEMA_PLUGIN_FLATBUFFERS

namespace vlink {

/**
 * @class FlatbuffersRegistry
 * @brief Singleton storing non-owning views of compiled-in FlatBuffers BFBS reflection blobs.
 *
 * @details
 * The registry keeps a @c SchemaData entry per fully-qualified FlatBuffers root type
 * name.  Callers must guarantee that the BFBS bytes outlive every registry reference;
 * generated @c *BinarySchema helpers point into static link-time storage and satisfy
 * this requirement automatically.  All public operations are thread-safe.
 */
class FlatbuffersRegistry final {
 public:
  /**
   * @brief Registers the BFBS data exposed by a generated @c *BinarySchema helper type.
   *
   * @tparam BinarySchema Generated helper exposing @c data() and @c size() static accessors.
   * @param name          Fully-qualified FlatBuffers root type name (table or struct).
   * @return @c true when the blob is valid FlatBuffers reflection data and is now stored.
   */
  template <typename BinarySchema>
  static bool register_schema(const std::string& name);

  /**
   * @brief Registers raw BFBS bytes under the given root type name.
   *
   * @param name      Fully-qualified FlatBuffers root type name.
   * @param bfbs_data Pointer to BFBS reflection bytes.
   * @param bfbs_size Length of @p bfbs_data in bytes.
   * @return @c true when the blob is valid FlatBuffers reflection data and is now stored.
   */
  static bool register_schema(const std::string& name, const uint8_t* bfbs_data, size_t bfbs_size);

  /**
   * @brief Looks up a single BFBS entry by root type name.
   *
   * @param name Fully-qualified root type name.
   * @return Copy of the stored @c SchemaData, or an empty schema when no entry matches.
   */
  [[nodiscard]] SchemaData search_schema(const std::string& name);

  /**
   * @brief Returns every BFBS entry currently held by the registry.
   *
   * @return Vector of @c SchemaData copies.
   */
  [[nodiscard]] std::vector<SchemaData> get_all_schemas();

 private:
  FlatbuffersRegistry() = default;

  ~FlatbuffersRegistry() = default;

  static SchemaData build_data(const std::string& name, const uint8_t* bfbs_data, size_t bfbs_size);

  std::unordered_map<std::string, SchemaData> map_;
  std::shared_mutex mtx_;

  VLINK_SINGLETON_DECLARE(FlatbuffersRegistry)
};

////////////////////////////////////////////////////////////////
// Details
////////////////////////////////////////////////////////////////

template <typename BinarySchema>
inline bool FlatbuffersRegistry::register_schema(const std::string& name) {
  return register_schema(name, BinarySchema::data(), BinarySchema::size());
}

inline bool FlatbuffersRegistry::register_schema(const std::string& name, const uint8_t* bfbs_data, size_t bfbs_size) {
  auto schema = build_data(name, bfbs_data, bfbs_size);

  if VUNLIKELY (schema.encoding != "flatbuffers" || schema.data.empty()) {
    return false;
  }

  auto& registry = get();

  std::lock_guard lock(registry.mtx_);
  registry.map_[schema.name] = std::move(schema);

  return true;
}

inline SchemaData FlatbuffersRegistry::search_schema(const std::string& name) {
  std::shared_lock lock(mtx_);

  auto iter = map_.find(name);

  if (iter == map_.end()) {
    return {};
  }

  return iter->second;
}

inline std::vector<SchemaData> FlatbuffersRegistry::get_all_schemas() {
  std::shared_lock lock(mtx_);

  std::vector<SchemaData> schemas;
  schemas.reserve(map_.size());

  for (const auto& [name, schema] : map_) {
    (void)name;
    schemas.emplace_back(schema);
  }

  return schemas;
}

inline SchemaData FlatbuffersRegistry::build_data(const std::string& name, const uint8_t* bfbs_data, size_t bfbs_size) {
  SchemaData schema;
  Bytes::init_memory_pool();

  if VUNLIKELY (name.empty() || bfbs_data == nullptr || bfbs_size == 0) {
    return schema;
  }

  schema.name = name;
  schema.encoding = "flatbuffers";
  schema.schema_type = SchemaType::kFlatbuffers;
  schema.data = Bytes::shallow_copy(bfbs_data, bfbs_size);

  flatbuffers::Verifier verifier(schema.data.data(), schema.data.size());

  if VUNLIKELY (!reflection::VerifySchemaBuffer(verifier)) {
    schema.encoding.clear();
    schema.schema_type = SchemaType::kUnknown;
    schema.data.clear();
  }

  return schema;
}

}  // namespace vlink

/**
 * @def VLINK_REGISTER_FLATBUFFERS_NOW(schema_name, binary_schema_type)
 * @brief Immediately registers a compiled-in BFBS schema with the global registry.
 *
 * @details
 * Expands to a direct call to
 * @c ::vlink::FlatbuffersRegistry::register_schema<binary_schema_type>().  No static
 * objects are created, so the macro may also be used from inside a function body.
 */
#define VLINK_REGISTER_FLATBUFFERS_NOW(schema_name, binary_schema_type) \
  ::vlink::FlatbuffersRegistry::register_schema<binary_schema_type>(schema_name)

/**
 * @def VLINK_REGISTER_FLATBUFFERS(schema_name, binary_schema_type)
 * @brief Auto-registers a BFBS schema at static initialisation time.
 *
 * @details
 * Defines a translation-unit-local helper type plus an unnamed @c static instance whose
 * constructor invokes @c VLINK_REGISTER_FLATBUFFERS_NOW.  Each expansion uses
 * @c __COUNTER__ to obtain a unique helper specialisation so the macro can be used
 * multiple times in the same translation unit and works correctly with namespaced or
 * templated @p binary_schema_type values.
 *
 * @par Example
 * @code
 * VLINK_REGISTER_FLATBUFFERS("Helloworld.fbs.User",
 *                            Helloworld::fbs::UserBinarySchema);
 * @endcode
 */
#define VLINK_REGISTER_FLATBUFFERS(schema_name, binary_schema_type)    \
  /* NOLINTBEGIN */                                                    \
  namespace {                                                          \
  template <int Id>                                                    \
  struct VlinkAutoRegisterFlatbuffersHelper;                           \
                                                                       \
  template <>                                                          \
  struct VlinkAutoRegisterFlatbuffersHelper<__COUNTER__> {             \
    struct Init {                                                      \
      Init() noexcept {                                                \
        using SchemaType = binary_schema_type;                         \
        (void)VLINK_REGISTER_FLATBUFFERS_NOW(schema_name, SchemaType); \
      }                                                                \
    };                                                                 \
                                                                       \
    [[maybe_unused]] inline static const Init instance{};              \
  };                                                                   \
  }                                                                    \
  /* NOLINTEND */

#endif  // VLINK_HAS_SCHEMA_PLUGIN_FLATBUFFERS
