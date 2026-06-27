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
 * @file schema_plugin_interface.h
 * @brief Abstract contract for runtime Protobuf / FlatBuffers schema discovery plugins.
 *
 * @details
 * @c SchemaPluginInterface is the plugin-side contract shared between VLink and any out-of-tree
 * shared library that supplies serialisation metadata for runtime introspection.  Plugins are
 * loaded by @c SchemaPluginManager through the generic @c Plugin loader and surface a uniform
 * lookup API regardless of whether the underlying metadata lives in a linked Protobuf descriptor
 * pool, in @c flatc-generated BFBS blobs, or in some bespoke registry.
 *
 * | Capability                | Method                              | Returns when missing    |
 * | ------------------------- | ----------------------------------- | ----------------------- |
 * | Version probe             | @c get_version_info()               | populated info          |
 * | Generic schema lookup     | @c search_schema()                  | name-only @c SchemaData |
 * | Bulk enumeration          | @c get_all_schemas()                | empty vector            |
 * | Protobuf descriptor       | @c search_protobuf_descriptor()     | @c nullptr              |
 * | Protobuf dynamic message  | @c create_protobuf_message()        | @c nullptr              |
 * | FlatBuffers BFBS schema   | @c search_flatbuffers_schema()      | @c nullptr              |
 * | FlatBuffers parser        | @c create_flatbuffers_parser()      | @c nullptr              |
 *
 * @par Plugin lifecycle
 * @code
 *   load .so  -->  factory ctor  -->  search/get_all  -->  create_message/parser  -->  dtor
 *                       |                                                                ^
 *                       v                                                                |
 *                  caches built lazily under an internal mutex of the implementation ----+
 * @endcode
 *
 * Concrete plugins normally derive from @c SchemaPluginBase, which already implements every
 * method against a linked Protobuf descriptor pool plus the process-local FlatBuffers registry.
 */

#pragma once

#include <string>
#include <vector>

#include "../base/plugin.h"
#include "../impl/types.h"

namespace vlink {

/**
 * @class SchemaPluginInterface
 * @brief Polymorphic contract for runtime schema lookup and dynamic message construction.
 *
 * @details
 * Instances are obtained through @c Plugin::load<SchemaPluginInterface>() or, more commonly,
 * via the @c SchemaPluginManager singleton.  Opaque @c void* aliases keep the contract free
 * of Protobuf and FlatBuffers headers so that downstream binaries that do not link those
 * libraries can still consume the interface; callers that statically link the matching
 * library are expected to @c reinterpret_cast back to the concrete pointer type.
 */
class SchemaPluginInterface {
  VLINK_PLUGIN_REGISTER(SchemaPluginInterface)

 protected:
  SchemaPluginInterface() = default;

  virtual ~SchemaPluginInterface() = default;

 public:
  /**
   * @brief Opaque alias for a @c google::protobuf::Descriptor pointer.
   *
   * @details
   * Callers that statically link Protobuf may cast this back to
   * @c google::protobuf::Descriptor* and use the full reflection API.
   */
  using ProtobufDescriptorPtr = void*;

  /**
   * @brief Opaque alias for a @c google::protobuf::Message pointer.
   *
   * @details
   * The lifetime of the underlying message is owned by the plugin; callers must
   * not @c delete the returned pointer.
   */
  using ProtobufMessagePtr = void*;

  /**
   * @brief Opaque alias for a @c reflection::Schema pointer (FlatBuffers BFBS).
   */
  using FlatbuffersSchemaPtr = void*;

  /**
   * @brief Opaque alias for a @c flatbuffers::Parser pointer pre-loaded with a root type.
   */
  using FlatbuffersParserPtr = void*;

  /**
   * @struct VersionInfo
   * @brief Build provenance reported by a concrete plugin implementation.
   *
   * @details
   * Provides enough metadata for diagnostic logging and version pinning when a process
   * needs to assert that loaded plugins match the running binary.
   */
  struct VersionInfo final {
    std::string name;       ///< Human-readable plugin identifier.
    std::string version;    ///< Semantic version string such as @c "2.0.0".
    std::string timestamp;  ///< ISO-8601 build timestamp captured at compile time.
    std::string tag;        ///< Optional source-control tag tied to the binary.
    std::string commit_id;  ///< Optional source-control commit hash.
  };

  /**
   * @brief Reports the plugin's version and build identity.
   *
   * @return Populated @c VersionInfo describing the loaded plugin binary.
   */
  [[nodiscard]] virtual VersionInfo get_version_info() const = 0;

  /**
   * @brief Resolves a single schema record by name, optionally restricted to one family.
   *
   * @details
   * When @p schema_type is @c SchemaType::kUnknown the plugin probes every supported family
   * and returns the unique match; ambiguous names yield an empty record.  Supplying a concrete
   * @c SchemaType selects the matching backend directly and skips the probing.
   *
   * @param name         Serialisation type name or fully qualified message name.
   * @param schema_type  Family hint, or @c SchemaType::kUnknown for family-agnostic lookup.
   * @return Matching @c SchemaData on success, or a name-only record on miss.
   */
  [[nodiscard]] virtual SchemaData search_schema(const std::string& name,
                                                 SchemaType schema_type = SchemaType::kUnknown) = 0;

  /**
   * @brief Enumerates every schema known to the plugin, optionally filtered by family.
   *
   * @param schema_type  Family filter, or @c SchemaType::kUnknown to return all families.
   * @return Snapshot vector of @c SchemaData entries owned by the caller.
   */
  [[nodiscard]] virtual std::vector<SchemaData> get_all_schemas(SchemaType schema_type = SchemaType::kUnknown) = 0;

  /**
   * @brief Returns the Protobuf descriptor for a fully qualified message name.
   *
   * @param name  Fully qualified Protobuf message name (e.g. @c "demo.proto.PointCloud").
   * @return Opaque @c Descriptor pointer, or @c nullptr when the name is unknown.
   */
  [[nodiscard]] virtual ProtobufDescriptorPtr search_protobuf_descriptor(const std::string& name) = 0;

  /**
   * @brief Returns a cached dynamic @c Message instance for the given Protobuf type.
   *
   * @details
   * The instance is owned by the plugin; callers may copy from it but must not delete it.
   *
   * @param name  Fully qualified Protobuf message name.
   * @return Opaque @c Message pointer, or @c nullptr when the name is unknown.
   */
  [[nodiscard]] virtual ProtobufMessagePtr create_protobuf_message(const std::string& name) = 0;

  /**
   * @brief Returns a verified BFBS reflection schema handle for a FlatBuffers root type.
   *
   * @param name  Fully qualified FlatBuffers root type name.
   * @return Opaque @c reflection::Schema pointer, or @c nullptr when the name is unknown.
   */
  [[nodiscard]] virtual FlatbuffersSchemaPtr search_flatbuffers_schema(const std::string& name) = 0;

  /**
   * @brief Returns a freshly built @c flatbuffers::Parser pre-loaded with the requested root.
   *
   * @details
   * Each successful call returns a distinct parser instance whose lifetime is owned by
   * the plugin.  This avoids cross-thread parser reuse, which is unsafe.
   *
   * @param name  Fully qualified FlatBuffers root type name.
   * @return Opaque parser pointer, or @c nullptr when the name is unknown.
   */
  [[nodiscard]] virtual FlatbuffersParserPtr create_flatbuffers_parser(const std::string& name) = 0;

 private:
  VLINK_DISALLOW_COPY_AND_ASSIGN(SchemaPluginInterface)
};

}  // namespace vlink
