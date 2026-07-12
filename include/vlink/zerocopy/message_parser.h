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
 * @file message_parser.h
 * @brief Unified runtime parser for built-in VLink zero-copy wire messages.
 *
 * @details
 * @c MessageParser centralises type detection, deserialization, indexed field
 * access, collection bounds, and exact integer representation for all eight
 * built-in VLink zero-copy wire types. Root paths use names such as
 * @c header.time_meas and @c width. Indexed payloads use @c data[N].field;
 * tensors additionally expose @c shape[N] and @c strides[N].
 *
 * Integer fields remain @c int64_t or @c uint64_t in @c MessageParser::Value.
 * Conversion through @c numeric reports values outside the exact IEEE-754
 * integer range. Deserialized containers borrow the input wire storage; the
 * input must therefore outlive the parser and any returned @c Bytes view.
 *
 * @par Parsing and access flow
 * @verbatim
 *   serialized type + wire Bytes
 *               |
 *               v
 *      MessageParser::detect_type
 *               |
 *               v
 *      MessageParser::parse  ---- failure ----> invalid / empty state
 *               |
 *               +----> exact field: value(path)
 *               |
 *               +----> collection: value(name, index, field)
 * @endverbatim
 *
 * @par Path and value mapping
 * | Payload kind       | Example path                 | @c Value alternative |
 * | ------------------ | ---------------------------- | -------------------- |
 * | Header / metadata  | @c header.time_meas          | @c uint64_t          |
 * | Point record       | @c data[4].intensity         | Schema-dependent     |
 * | Object record      | @c objects[2].track_id       | @c uint64_t          |
 * | Grid / tensor cell | @c data[7]                   | Dtype-dependent      |
 * | Tensor dimension   | @c shape[1] / @c strides[1]  | @c uint64_t          |
 * | Raw payload        | @c data / @c raw             | Borrowed @c Bytes    |
 *
 * @par Collection mapping
 * | Message type       | Indexed collections                     | Element field form       |
 * | ------------------ | --------------------------------------- | ------------------------ |
 * | @c RawData         | None                                    | —                        |
 * | @c CameraFrame     | None                                    | —                        |
 * | @c PointCloud      | @c data / @c points                     | Dynamic point key        |
 * | @c ProxyData       | None                                    | —                        |
 * | @c OccupancyGrid   | @c data / @c cells                      | Empty or @c value        |
 * | @c Tensor          | @c data / @c elements, shape, strides   | Empty or @c value        |
 * | @c ObjectArray     | @c data / @c objects                    | Object member or alias   |
 * | @c AudioFrame      | @c data                                 | Empty or @c value        |
 *
 * @par Example -- parse and read an exact integer field
 * @code
 * vlink::zerocopy::MessageParser parser;
 * if (!parser.parse("vlink::zerocopy::ObjectArray", wire)) {
 *   return;
 * }
 *
 * vlink::zerocopy::MessageParser::Value value;
 * if (parser.value("data[0].track_id", value)) {
 *   consume(std::get<uint64_t>(value));
 * }
 * @endcode
 */

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "../base/bytes.h"
#include "../base/macros.h"
#include "./audio_frame.h"
#include "./camera_frame.h"
#include "./object_array.h"
#include "./occupancy_grid.h"
#include "./point_cloud.h"
#include "./proxy_data.h"
#include "./raw_data.h"
#include "./tensor.h"

namespace vlink {

namespace zerocopy {

/**
 * @class MessageParser
 * @brief Unified parser and field reader for built-in zero-copy messages.
 *
 * @details
 * A successful parse retains the decoded message and exposes schema-neutral
 * scalar access through @c value(), @c numeric(), and @c text(). Indexed
 * payloads use paths such as @c data[4].x, @c shape[1], and
 * @c objects[2].track_id. Integer values remain exact in @c Value; callers
 * converting them to double can request a precision loss report. Decoded
 * containers and @c Bytes values borrow payload storage from the input. The
 * input must outlive the parser and every returned byte view; scalar and
 * string values are copied into @c Value.
 */
class VLINK_EXPORT MessageParser final {
 public:
  /**
   * @enum Type
   * @brief Built-in zero-copy wire type retained by the parser.
   */
  enum Type : uint8_t {
    kUnknown = 0,        ///< No message is parsed, or the serialized type is unsupported.
    kRawData = 1,        ///< Opaque bytes with a common zero-copy header.
    kCameraFrame = 2,    ///< Image or encoded video frame.
    kPointCloud = 3,     ///< Dynamically described packed point records.
    kProxyData = 4,      ///< Proxy routing envelope and nested raw payload.
    kOccupancyGrid = 5,  ///< Typed two-dimensional occupancy or cost cells.
    kTensor = 6,         ///< Typed multidimensional tensor payload.
    kObjectArray = 7,    ///< Detection or tracking object records.
    kAudioFrame = 8,     ///< PCM or encoded audio frame.
  };

  /**
   * @enum ValueType
   * @brief Exact C++ alternative stored by @c Value for a field.
   */
  enum ValueType : uint8_t {
    kValueUnknown = 0,  ///< Field type has not been initialised.
    kInt64 = 1,         ///< Signed integral value retained as @c int64_t.
    kUInt64 = 2,        ///< Unsigned integral value retained as @c uint64_t.
    kDouble = 3,        ///< Floating-point value represented as @c double.
    kString = 4,        ///< UTF-8 or protocol text represented as @c std::string.
    kBytes = 5,         ///< Opaque payload returned as a borrowed @c Bytes view.
  };

  /**
   * @enum EnumKind
   * @brief Built-in enumeration a scalar field maps to, for symbolic-name rendering.
   *
   * @details
   * Structural reflection alone cannot recover an integer field's symbolic
   * enumerator name. @c EnumKind names the concrete built-in enum a field
   * encodes so a presentation layer can resolve its label without re-hardcoding
   * per-type knowledge. @c kEnumNone marks a plain numeric field.
   */
  enum EnumKind : uint8_t {
    kEnumNone = 0,            ///< Plain numeric field with no symbolic enumeration.
    kEnumCameraFormat = 1,    ///< @c CameraFrame::Format image or codec format.
    kEnumCameraStream = 2,    ///< @c CameraFrame::Stream stream role.
    kEnumGridCellType = 3,    ///< @c OccupancyGrid::CellType cell storage type.
    kEnumTensorDataType = 4,  ///< @c Tensor::DataType element data type.
    kEnumTensorDevice = 5,    ///< @c Tensor::Device residency device.
    kEnumAudioFormat = 6,     ///< @c AudioFrame::Format sample format.
    kEnumAudioLayout = 7,     ///< @c AudioFrame::Layout channel layout.
  };

  /**
   * @struct Field
   * @brief Schema-neutral field descriptor returned by field enumeration.
   */
  struct Field final {
    std::string name;                          ///< Canonical path or collection element field name.
    ValueType type{ValueType::kValueUnknown};  ///< Exact @c Value alternative returned for the field.
    uint16_t native_type{0};                   ///< Message-specific schema type tag, or zero when not applicable.
    uint16_t storage_size{0};                  ///< Encoded field width in bytes, or zero for variable-width fields.
    EnumKind enum_kind{EnumKind::kEnumNone};   ///< Built-in enumeration the field encodes, for symbolic rendering.
    bool is_time{false};                       ///< Field is a nanosecond timestamp eligible for date rendering.
    bool is_bool{false};                       ///< Field encodes a boolean and should render as @c true / @c false.
    bool is_reserved{false};                   ///< Field is a reserved slot a presentation layer may hide.
    size_t byte_offset{0};                     ///< Byte offset within an indexed packed record, when applicable.
    size_t element_index{static_cast<size_t>(-1)};  ///< Declaration index within an indexed record, when applicable.
  };

  /**
   * @typedef Value
   * @brief Exact schema-neutral field value without implicit integer-to-double conversion.
   */
  using Value = std::variant<int64_t, uint64_t, double, std::string, Bytes>;

  /**
   * @brief Constructs an invalid parser with no retained message.
   */
  MessageParser() = default;

  /**
   * @brief Releases the retained typed message.
   */
  ~MessageParser() = default;

  /**
   * @brief Parsers are non-copyable because decoded containers may borrow wire storage.
   *
   * @param target Parser whose borrowed state would otherwise be copied.
   */
  MessageParser(const MessageParser&) = delete;
  MessageParser& operator=(const MessageParser&) = delete;

  /**
   * @brief Move-constructs or move-assigns the retained parser state.
   *
   * @param target Parser whose retained state is transferred.
   */
  MessageParser(MessageParser&&) noexcept = default;
  MessageParser& operator=(MessageParser&&) noexcept = default;

  /**
   * @brief Detects @p serialized_type and parses @p bytes; failure clears the parser.
   *
   * @param serialized_type Exact type name or namespace-delimited built-in type suffix.
   * @param bytes Zero-copy wire envelope to decode; its storage must outlive this parser.
   * @return @c true on successful type detection and deserialization.
   */
  bool parse(std::string_view serialized_type, const Bytes& bytes);

  /**
   * @brief Parses @p bytes as the explicitly selected type; failure clears the parser.
   *
   * @param type Built-in zero-copy type expected in @p bytes.
   * @param bytes Zero-copy wire envelope to decode; its storage must outlive this parser.
   * @return @c true when the envelope is valid for @p type.
   */
  bool parse(Type type, const Bytes& bytes);

  /**
   * @brief Releases the current decoded message and returns to the invalid state.
   */
  void clear() noexcept;

  /**
   * @brief Returns the retained built-in type, or @c kUnknown when invalid.
   */
  [[nodiscard]] Type type() const noexcept;

  /**
   * @brief Returns whether the parser currently retains a successfully decoded message.
   */
  [[nodiscard]] bool valid() const noexcept;

  /**
   * @brief Reads a root or indexed field without losing its integer representation.
   *
   * @param path Root path or indexed path such as @c data[3].x.
   * @param out Exact field value written only when the path resolves.
   * @return @c true when @p path exists and is readable.
   */
  bool value(std::string_view path, Value& out) const;

  /**
   * @brief Reads @p field from element @p index of a named collection.
   *
   * @param collection Canonical collection or supported alias.
   * @param index Zero-based element index.
   * @param field Element field name, or @c value for scalar collections.
   * @param out Exact field value written on success.
   * @return @c false for an unknown collection, field, or out-of-range index.
   */
  bool value(std::string_view collection, size_t index, std::string_view field, Value& out) const;

  /**
   * @brief Reads a collection element through a descriptor returned by @c element_fields().
   *
   * @details This overload avoids repeated name resolution in packed-record hot paths. The
   * descriptor is validated against the parser's current schema, so a stale descriptor from
   * an incompatible subsequent parse is rejected.
   */
  bool value(std::string_view collection, size_t index, const Field& field, Value& out) const;

  /**
   * @brief Reads a numeric path and optionally reports integer-to-double precision loss.
   *
   * @param path Root or indexed numeric field path.
   * @param out Numeric value converted to @c double.
   * @param precision_loss Optional flag set when an integer exceeds the exact IEEE-754 range.
   * @return @c true when @p path resolves to a numeric value.
   */
  bool numeric(std::string_view path, double& out, bool* precision_loss = nullptr) const;

  /**
   * @brief Reads a numeric collection element and optionally reports precision loss.
   *
   * @param collection Canonical collection or supported alias.
   * @param index Zero-based element index.
   * @param field Element field name, or @c value for scalar collections.
   * @param out Numeric value converted to @c double.
   * @param precision_loss Optional flag set when integer conversion is inexact.
   * @return @c true when the selected element resolves to a numeric value.
   */
  bool numeric(std::string_view collection, size_t index, std::string_view field, double& out,
               bool* precision_loss = nullptr) const;

  /**
   * @brief Reads a numeric collection element through a pre-resolved field descriptor.
   */
  bool numeric(std::string_view collection, size_t index, const Field& field, double& out,
               bool* precision_loss = nullptr) const;

  /**
   * @brief Reads a string-valued path.
   *
   * @param path Root or indexed string field path.
   * @param out String copied from the decoded field on success.
   * @return @c true when @p path resolves to a string value.
   */
  bool text(std::string_view path, std::string& out) const;

  /**
   * @brief Reads a string-valued collection element.
   *
   * @param collection Canonical collection or supported alias.
   * @param index Zero-based element index.
   * @param field String field name.
   * @param out String copied from the decoded field on success.
   * @return @c true when the selected element resolves to a string value.
   */
  bool text(std::string_view collection, size_t index, std::string_view field, std::string& out) const;

  /**
   * @brief Returns the validated accessible element count of @p collection.
   *
   * @param collection Canonical collection or supported alias.
   * @return Number of elements safe to read from the decoded payload.
   */
  [[nodiscard]] size_t collection_size(std::string_view collection) const noexcept;

  /**
   * @brief Enumerates root fields available for the parsed type.
   *
   * @return Stable declaration-order descriptors; empty when the parser is invalid.
   */
  [[nodiscard]] std::vector<Field> fields() const;

  /**
   * @brief Enumerates fields of one element in @p collection.
   *
   * @param collection Canonical collection or supported alias.
   * @return Stable element field descriptors, or an empty vector for an unknown collection.
   */
  [[nodiscard]] std::vector<Field> element_fields(std::string_view collection) const;

  /**
   * @brief Deep-copies the retained message into a matching typed container.
   *
   * @tparam T One of the built-in zero-copy container types.
   * @param out Destination container replaced only when @p T matches the parsed type.
   * @return @c true when a matching retained message was copied.
   *
   * @note This compatibility operation is intended for typed container APIs. Dynamic
   *       readers should use @c value(), @c fields(), and collection access instead.
   */
  template <typename T>
  bool copy_to(T& out) const;

  /**
   * @brief Detects a built-in type using an exact name or namespace-delimited suffix.
   *
   * @param serialized_type Serialized type name supplied by schema metadata.
   * @return Detected built-in type, or @c kUnknown when no exact suffix matches.
   */
  static Type detect_type(std::string_view serialized_type) noexcept;

  /**
   * @brief Returns the canonical unqualified serialized name for @p type.
   *
   * @param type Built-in parser type.
   * @return Stable unqualified name, or an empty view for @c kUnknown.
   */
  static std::string_view type_name(Type type) noexcept;

 private:
  using Message = std::variant<std::monostate, RawData, CameraFrame, PointCloud, ProxyData, OccupancyGrid, Tensor,
                               ObjectArray, AudioFrame>;

  template <typename T>
  [[nodiscard]] const T* get() const noexcept;

  /**
   * @brief Resolves a non-indexed field from the retained message.
   */
  bool root_value(std::string_view path, Value& out) const;

  /**
   * @brief Resolves one field from a validated collection element.
   */
  bool element_value(std::string_view collection, size_t index, std::string_view field, Value& out) const;

  /**
   * @brief Reads a PointCloud field whose schema descriptor has already been validated.
   */
  bool point_value(size_t index, const Field& field, Value& out) const;

  Type type_{Type::kUnknown};
  Message message_;
  std::array<uint64_t, 5> reserved_{};
  std::vector<Field> point_fields_;
  std::vector<size_t> point_field_buckets_;
  std::vector<size_t> point_field_next_;
};

////////////////////////////////////////////////////////////////
/// Details
////////////////////////////////////////////////////////////////

inline MessageParser::Type MessageParser::type() const noexcept { return type_; }

inline bool MessageParser::valid() const noexcept { return type_ != Type::kUnknown; }

template <typename T>
inline const T* MessageParser::get() const noexcept {
  return std::get_if<T>(&message_);
}

template <typename T>
inline bool MessageParser::copy_to(T& out) const {
  const auto* message = get<T>();

  if VUNLIKELY (message == nullptr) {
    return false;
  }

  out = *message;
  return true;
}

/**
 * @struct MessageFormatOptions
 * @brief Presentation toggles for @c format_message.
 */
struct MessageFormatOptions final {
  bool hex{false};             ///< Render integer fields as hexadecimal instead of decimal.
  bool date{false};            ///< Render nanosecond timestamp fields as calendar dates.
  bool enum_name{false};       ///< Render enumeration fields as symbolic names instead of numbers.
  bool expand_arrays{true};    ///< Expand indexed collections (e.g. PointCloud points) element by element.
  size_t max_elements{10000};  ///< Upper bound on expanded collection elements.
};

/**
 * @brief Renders a parsed zero-copy message as canonical human-readable text.
 *
 * @details
 * Walks @p parser purely through its field reflection (@c fields, @c element_fields,
 * @c collection_size, @c value) and renders the canonical text form shared by
 * @c vlink-dump, @c vlink-efbs and @c vlink-eproto. Per-type presentation -- header
 * grouping, hidden reserved slots, symbolic enumerator names, nanosecond timestamps,
 * boolean rendering, the PointCloud protocol block and Tensor shape line -- is driven
 * by the @c MessageParser::Field metadata rather than by per-message branches.
 *
 * @param parser Valid parser retained for the duration of this call.
 * @param options Presentation toggles.
 * @param truncated Optional flag set when collection expansion hit @c max_elements.
 * @return Rendered text, or an empty string when @p parser is invalid.
 */
VLINK_EXPORT std::string format_message(const MessageParser& parser, const MessageFormatOptions& options,
                                        bool* truncated = nullptr);

}  // namespace zerocopy

}  // namespace vlink
