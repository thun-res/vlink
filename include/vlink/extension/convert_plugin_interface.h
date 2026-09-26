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
 * @file convert_plugin_interface.h
 * @brief Plugin contract for converting VLink payloads to visualisation backend formats.
 *
 * @details
 * @c ConvertPluginInterface lets users supply custom encoders that translate raw VLink
 * messages -- in any serialisation -- into the payload format expected by a particular
 * webviz frontend.  The plugin is loaded as a shared library via the VLink @c Plugin
 * framework and has no third-party dependencies of its own: it consumes @c Bytes and
 * emits @c Bytes, so consumers may implement it without linking Protobuf, FlatBuffers,
 * the Rerun SDK or any JSON library.
 *
 * Conversion pipeline:
 *
 * @verbatim
 *                          can_convert(ser, target)?
 *   VLink Bytes  ----->  +-----------------------+
 *                        |  ConvertPluginInterface |  --get_schema(ser, target, info)--> channel registration
 *                        +-----------------------+
 *                                 |
 *                                 v convert(ser, raw, target, payload)
 *                              backend Bytes  --->  Foxglove / Rerun frontend
 * @endverbatim
 *
 * Supported source/target combinations and the meaning of each output field:
 *
 * | @c Target          | Wire payload                        | @c SchemaInfo::type_name meaning      |
 * | ------------------ | ----------------------------------- | ------------------------------------- |
 * | @c kFoxglove       | FlatBuffer / Protobuf binary bytes  | Foxglove schema name                  |
 * | @c kRerun          | UTF-8 JSON describing components    | Rerun archetype name                  |
 *
 * @par Rerun JSON payload format
 * Plugins targeting Rerun emit a UTF-8 JSON object whose fields match the Rerun
 * archetype in the linked SDK. Byte arrays accept a JSON array or a @c {"base64":"..."} object.
 *
 * @code{.json}
 * // Points3D
 * { "positions": [[1.0, 2.0, 3.0], [4.0, 5.0, 6.0]],
 *   "colors":    [[255, 0, 0, 255], [0, 255, 0, 255]],
 *   "radii":     [0.1, 0.2] }
 *
 * // EncodedImage  (binary payload base64-encoded)
 * { "media_type": "image/jpeg",
 *   "blob": {"base64": "<base64 image bytes>"} }
 *
 * // GeoPoints
 * { "positions": [[37.7749, -122.4194], [37.7750, -122.4195]] }
 *
 * // TextLog
 * { "text": "Hello world", "level": "INFO" }
 *
 * // Scalars
 * { "scalars": [3.14] }
 *
 * // Transform3D
 * { "translation":   [1.0, 2.0, 3.0],
 *   "quaternion": [0.0, 0.0, 0.0, 1.0] }
 *
 * // Boxes3D
 * { "half_sizes":  [[1.0, 2.0, 3.0]],
 *   "centers":     [[0.0, 0.0, 0.0]],
 *   "quaternions": [[0.0, 0.0, 0.0, 1.0]],
 *   "colors":      [[255, 0, 0, 255]],
 *   "labels":      ["box1"] }
 *
 * // Pinhole
 * { "image_from_camera": [fx, 0, 0, 0, fy, 0, cx, cy, 1],
 *   "resolution":        [1920, 1080] }
 * @endcode
 *
 * Fields and component layouts come from the linked SDK: vectors are arrays,
 * matrices are flat column-major arrays, structs are objects, and tagged unions
 * are single-key objects such as @c {"F32":[1.0,2.0]}. Enum strings use SDK names.
 * Image components use @c format and @c buffer; Tensor uses
 * @c {"data":{"shape":[2],"buffer":{"F32":[1.0,2.0]}}}.
 * Binary numeric lists use little-endian values of the declared component type.
 * Integer components reject fractions and overflow; floating components retain
 * NaN and infinity when the source representation supports them.
 * Missing components are omitted. Empty component batches clear their values;
 * an empty Blob array is one empty Blob. @c RecordingInfo is logged statically
 * at the SDK recording-properties path. Field mappings use this same writer.
 *
 * Plugin lifecycle:
 * 1. @c init() runs once after dynamic load, with an opaque configuration string.
 * 2. @c can_convert() is queried per discovered VLink serialisation type, per target.
 * 3. @c get_schema() registers the channel and refreshes dynamic output schemas.
 * 4. @c convert() runs for every incoming payload on accepted types.
 * 5. Optional reverse hooks (@c can_publish(), @c get_publish(),
 *    @c convert_publish()) handle inbound frontend command/control flows.
 * 6. The destructor runs when the host unloads the plugin.
 *
 * @par Example
 * @code
 * #include <vlink/extension/convert_plugin_interface.h>
 *
 * class MyConvertPlugin : public vlink::ConvertPluginInterface {
 *   VLINK_PLUGIN_REGISTER(ConvertPluginInterface)
 *
 *  public:
 *   bool init(const std::string& config) override {
 *     (void)config;
 *     return true;
 *   }
 *
 *   bool can_convert(const std::string& ser_type, Target target) override {
 *     return ser_type == "my_pkg.MyMessage";
 *   }
 *
 *   bool get_schema(const std::string& ser_type, Target target,
 *                   SchemaInfo& schema_info) override {
 *     if (target == Target::kFoxglove) {
 *       schema_info.type_name = "foxglove.LocationFix";
 *       schema_info.encoding = "flatbuffers";
 *       schema_info.schema_encoding = "flatbuffers";
 *       // schema_info.schema_data = compiled BFBS bytes
 *     } else {
 *       schema_info.type_name = "GeoPoints";
 *       schema_info.encoding = "json";
 *     }
 *     return true;
 *   }
 *
 *   bool convert(const std::string& ser_type, const vlink::Bytes& raw,
 *                Target target, vlink::Bytes& payload) override {
 *     if (target == Target::kRerun) {
 *       std::string json = R"({"positions":[[37.77,-122.41]]})";
 *       payload = vlink::Bytes::deep_copy(json.data(), json.size());
 *     }
 *     return true;
 *   }
 * };
 * VLINK_PLUGIN_DECLARE(MyConvertPlugin, 4, 0)
 * @endcode
 */

#pragma once

#include <cstdint>
#include <string>

#include "../base/bytes.h"
#include "../base/plugin.h"
#include "../impl/types.h"

namespace vlink {

/**
 * @class ConvertPluginInterface
 * @brief Abstract plugin base translating between VLink payloads and visualisation backends.
 *
 * @details
 * Loaded via @c Plugin::load<ConvertPluginInterface>().  The plugin must be thread-safe:
 * @c convert() and the inbound @c convert_publish() hooks may run concurrently from
 * multiple ProxyAPI worker threads.  A plugin that only supports one backend should
 * return @c false from @c can_convert() / @c can_publish() for the others.
 */
class ConvertPluginInterface {
  VLINK_PLUGIN_REGISTER(ConvertPluginInterface)

 protected:
  ConvertPluginInterface() = default;

  virtual ~ConvertPluginInterface() = default;

 public:
  /**
   * @enum Target
   * @brief Visualisation backend identifier carried by every conversion hook.
   *
   * @details
   * Allows a single plugin to support multiple backends from one binary -- the plugin
   * branches on this value to produce the appropriate payload format.
   */
  enum class Target : uint8_t {
    kFoxglove = 0,  ///< Foxglove Studio (WebSocket transport, FlatBuffers/Protobuf payloads).
    kRerun = 1,     ///< Rerun Viewer (gRPC + Arrow IPC; plugin payload is UTF-8 JSON).
  };

  /**
   * @struct SchemaInfo
   * @brief Backend channel schema metadata returned by @c get_schema().
   */
  struct SchemaInfo final {
    std::string type_name;        ///< Backend schema or archetype name.
    std::string encoding;         ///< Wire encoding label (e.g. @c "flatbuffers", @c "json").
    std::string schema_encoding;  ///< Encoding of @c schema_data when provided.
    std::string schema_data;      ///< Binary schema bytes or schema text, depending on @c target.
  };

  /**
   * @struct FrontendChannel
   * @brief Frontend-advertised channel description used by inbound conversion hooks.
   *
   * @details
   * Allows plugins to route Foxglove @c clientPublish-style messages onto the right VLink
   * topic by inspecting the channel's topic, encoding and schema metadata.
   */
  struct FrontendChannel final {
    std::string topic;            ///< Channel topic advertised by the frontend client.
    std::string encoding;         ///< Frontend payload encoding (json/protobuf/flatbuffers/...).
    std::string schema_name;      ///< Frontend-side schema or type name.
    std::string schema_encoding;  ///< Encoding of @c schema when provided.
    std::string schema;           ///< Raw schema string or binary payload (transport-specific).
  };

  /**
   * @struct PublishInfo
   * @brief VLink publish destination resolved from an inbound frontend channel.
   */
  struct PublishInfo final {
    std::string url;                               ///< Destination VLink URL (e.g. @c "dds://vehicle/cmd").
    std::string ser_type;                          ///< Destination VLink serialisation type.
    SchemaType schema_type{SchemaType::kUnknown};  ///< Coarse schema family for the published payload.
  };

  /**
   * @brief Initialises the plugin with an opaque configuration string.
   *
   * @details
   * Called once after the plugin is loaded; the @p config string may be a file path,
   * JSON document or anything the plugin defines.  Returning @c false causes the host
   * to unload the plugin.
   *
   * @param config Configuration payload; may be empty.
   * @return @c true on success.
   */
  virtual bool init(const std::string& config) = 0;

  /**
   * @brief Reports whether this plugin handles a (serialisation, target) pair.
   *
   * @details
   * Polled during channel discovery for each new VLink type.  A @c true answer commits
   * the plugin to subsequent @c get_schema() and @c convert() calls for that pair.
   *
   * @param ser_type VLink serialisation type name (e.g. @c "proto.VehiclePose").
   * @param target    Visualisation backend asking about the conversion.
   * @return @c true when the plugin can produce a payload for @p target.
   */
  [[nodiscard]] virtual bool can_convert(const std::string& ser_type, Target target) = 0;

  /**
   * @brief Provides schema metadata for an accepted (serialisation, target) pair.
   *
   * @details
   * Called when registering a channel and when refreshing a plugin's output schema. Outputs
   * differ per target:
   * - @c kFoxglove: fill @p schema_info with type, encoding and schema bytes
   *   (typically the bytes of a compiled BFBS file).
   * - @c kRerun: fill @p schema_info.type_name with the archetype name and
   *   @p schema_info.encoding with @c "json"; schema payload fields are unused.
   *
   * @param[in]  ser_type     VLink serialisation type name.
   * @param[in]  target       Visualisation backend.
   * @param[out] schema_info  Backend channel schema metadata.
   * @return @c true on success.
   */
  [[nodiscard]] virtual bool get_schema(const std::string& ser_type, Target target, SchemaInfo& schema_info) = 0;

  /**
   * @brief Converts a single raw VLink payload to the backend-specific representation.
   *
   * @details
   * Invoked once per incoming message on accepted types.  Must be thread-safe.
   *
   * @param[in]  ser_type VLink serialisation type name.
   * @param[in]  raw      Raw serialised VLink payload.
   * @param[in]  target   Visualisation backend.
   * @param[out] payload  Output buffer that receives the backend payload.
   * @return @c true on success.
   */
  [[nodiscard]] virtual bool convert(const std::string& ser_type, const Bytes& raw, Target target, Bytes& payload) = 0;

  /**
   * @brief Optionally extracts a per-message timestamp from the raw payload.
   *
   * @details
   * Called after @c convert() so the frontend can prefer a sensor or content timestamp
   * over the proxy transport timestamp.  The default implementation returns @c -1,
   * causing the host to fall back to the transport-level timestamp.
   *
   * @param[in] ser_type VLink serialisation type name.
   * @param[in] raw      Raw serialised VLink payload.
   * @param[in] target   Visualisation backend.
   * @return Timestamp in nanoseconds since epoch, or @c -1 when unavailable.
   */
  [[nodiscard]] virtual int64_t get_timestamp(const std::string& ser_type, const Bytes& raw, Target target) {
    (void)ser_type;
    (void)raw;
    (void)target;
    return -1;
  }

  /**
   * @brief Inbound counterpart of @c can_convert() for frontend-published channels.
   *
   * @details
   * Default implementation returns @c false; override to opt in to clientPublish-style
   * command flows.
   */
  [[nodiscard]] virtual bool can_publish(const FrontendChannel& channel, Target target) {
    (void)channel;
    (void)target;
    return false;
  }

  /**
   * @brief Resolves the VLink publish destination for an inbound frontend channel.
   *
   * @details
   * Returning @c true allows the host to provision the required VLink publishers ahead
   * of time.  Default implementation returns @c false.
   */
  [[nodiscard]] virtual bool get_publish(const FrontendChannel& channel, Target target, PublishInfo& publish_info) {
    (void)channel;
    (void)target;
    (void)publish_info;
    return false;
  }

  /**
   * @brief Converts a frontend-published payload into a raw VLink payload.
   *
   * @details
   * Invoked once per inbound message after @c get_publish() routed the channel.
   * Default implementation returns @c false.
   */
  [[nodiscard]] virtual bool convert_publish(const FrontendChannel& channel, const Bytes& raw, Target target,
                                             Bytes& payload) {
    (void)channel;
    (void)raw;
    (void)target;
    (void)payload;
    return false;
  }

 private:
  VLINK_DISALLOW_COPY_AND_ASSIGN(ConvertPluginInterface)
};

}  // namespace vlink
