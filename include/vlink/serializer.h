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
 * @file serializer.h
 * @brief Compile-time codec dispatch for VLink message payloads.
 *
 * @details
 * The @c Serializer namespace is the codec router used by every VLink
 * primitive.  Given a C++ type @c T it determines, at compile time, which
 * encoding family applies (raw bytes, Protobuf, FlatBuffers, FastDDS CDR,
 * standard-layout POD, etc.) and dispatches @c serialize() / @c deserialize()
 * to the appropriate code path with zero runtime cost.
 *
 * Application code rarely calls these helpers directly; @c Publisher,
 * @c Subscriber, @c Client, @c Server, @c Setter, and @c Getter call them
 * internally as part of their @c publish() / @c listen() / @c invoke() /
 * @c set() / @c get() implementations.
 *
 * @par Codec Table -- @c Serializer::Type Enum
 * | Constant             | C++ criterion                    | Trait                   | Notes              |
 * | -------------------- | -------------------------------- | ----------------------- | ------------------ |
 * | @c kBytesType        | @c T is @c Bytes                 | @c is_bytes_type        | Pass-through.      |
 * | @c kDynamicType      | Has @c is_vlink_dynamic_data()   | @c is_dynamic_type      | Dynamic data.      |
 * | @c kCustomType       | Has @c operator>>/<<(Bytes&)     | @c is_custom_type       | Custom codec.      |
 * | @c kSomeipType       | Has @c is_vlink_someip_type()    | @c is_someip_type       | SOME/IP codec.     |
 * | @c kCdrType          | FastDDS IDL or ROS 2 type        | @c is_cdr_type          | CDR bytes.         |
 * | @c kProtoType        | Protobuf-like value              | @c is_proto_type        | Protobuf value.    |
 * | @c kProtoPtrType     | Protobuf-like pointer            | @c is_proto_ptr_type    | Caller-owned.      |
 * | @c kFlatTableType    | FlatBuffers NativeTable          | @c is_flat_table_type   | Object API.        |
 * | @c kFlatPtrType      | Pointer to @c flatbuffers::Table | @c is_flat_ptr_type     | Zero-copy view.    |
 * | @c kFlatBuilderType  | Has @c fbb_ and @c Finish()      | @c is_flat_builder_type | Builder.           |
 * | @c kStringType       | @c T is @c std::string           | @c is_string_type       | Byte string.       |
 * | @c kCharsType        | Character pointer or array       | @c is_chars_type        | Serialise only.    |
 * | @c kStreamType       | Supports @c std::stringstream    | @c is_stream_type       | Fallback.          |
 * | @c kStandardType     | Trivial standard-layout value    | @c is_standard_type     | Byte copy.         |
 * | @c kStandardPtrType  | Pointer to trivial standard type | @c is_standard_ptr_type | Zero-copy pointer. |
 *
 * Most value-like detectors unwrap @c std::shared_ptr\<T\> before matching
 * (e.g. Protobuf values, CDR values, FlatBuffers native tables, custom
 * codecs, strings, stream types, and standard-layout values).
 * @par Detection Precedence Flow
 * @verbatim
 *   get_type_of<T>() probes traits in this fixed order; first match wins:
 *
 *     Bytes  --(no)-->  Dynamic  --(no)-->  SOME/IP  --(no)-->  CDR
 *                                                                |
 *                                                                v (no)
 *     FlatTable  <--(no)--  ProtoPtr  <--(no)--  Proto  <----(no)--+
 *         |
 *         v (no)
 *     FlatPtr  --(no)-->  FlatBuilder  --(no)-->  Custom  --(no)-->  String
 *                                                                    |
 *                                                                    v (no)
 *     Stream  <--(no)--  StandardPtr  <--(no)--  Standard  <--(no)--  Chars
 *                                                                      |
 *                                                                      v (no)
 *                                                                kUnknownType
 * @endverbatim
 *
 * @par Type Detection Example
 * @code
 * constexpr auto t = vlink::Serializer::get_type_of<MyProto>();   // -> kProtoType
 * static_assert(vlink::Serializer::is_supported(t));
 *
 * constexpr auto u = vlink::Serializer::get_type_of<int>();         // -> kStandardType (POD)
 * constexpr auto v = vlink::Serializer::get_type_of<std::string>(); // -> kStringType
 * constexpr auto w = vlink::Serializer::get_type_of<const char*>(); // -> kCharsType
 * @endcode
 *
 * @par Serialise and Deserialise
 * @code
 * MyProto msg;
 * vlink::Bytes bytes;
 * vlink::Serializer::serialize(msg, bytes);
 *
 * MyProto out;
 * vlink::Serializer::deserialize(bytes, out);
 * @endcode
 *
 * @par Custom Codec
 * @code
 * struct MyCustomMsg {
 *   int x;
 *   void operator>>(vlink::Bytes& out) const { ... }   // serialise
 *   void operator<<(const vlink::Bytes& in)        { ... }  // deserialise
 * };
 * // vlink::Serializer::get_type_of<MyCustomMsg>() == vlink::Serializer::kCustomType
 * @endcode
 *
 * @par Explicit Codec Selection
 * CDR serialization produces a byte stream containing the 4-byte DDS
 * encapsulation header. Use the explicit overload when the codec cannot be
 * inferred from @c T:
 * @code
 * vlink::Serializer::serialize<vlink::Serializer::kCdrType>(msg, bytes, vlink::TransportType::kDds);
 * @endcode
 *
 * @note Most entry points are header-defined templates; a few non-template
 *       overloads are declared @c static or @c inline where appropriate.
 *
 * @see base/bytes.h, impl/types.h
 */

#pragma once

#include <cstddef>
#include <string>

#include "./base/bytes.h"
#include "./impl/someip_serializer.h"
#include "./impl/types.h"

namespace vlink {

/**
 * @namespace Serializer
 * @brief Compile-time codec detection and dispatch for VLink message payloads.
 *
 * @details
 * Header-defined helper namespace.  Most entry points are templates so the
 * full codec chain is resolved at compile time.  Application code rarely
 * uses this namespace directly; the framework invokes it internally inside
 * @c publish() / @c listen() / @c invoke() / @c set() / @c get().
 */
namespace Serializer {  // NOLINT(readability-identifier-naming)

/**
 * @enum Type
 * @brief Identifies the codec to use for a given C++ message type.
 *
 * @details
 * Resolved at compile time by @c get_type_of\<T\>() and stored as a
 * @c constexpr member on every primitive class, so all codec dispatch is
 * zero-cost at runtime.
 */
enum Type : uint8_t {
  kUnknownType = 0,       ///< Unsupported type; @c is_supported() returns @c false.
  kBytesType = 1,         ///< @c Bytes -- raw byte pass-through.
  kDynamicType = 2,       ///< VLink dynamic typed data.
  kCustomType = 3,        ///< User-defined codec via @c operator>>/<<.
  kSomeipType = 4,        ///< Macro-declared SOME/IP structure via @c operator>>/<<.
  kCdrType = 5,           ///< FastDDS CDR via @c serialize(Cdr&) / @c deserialize(Cdr&).
  kProtoType = 6,         ///< Protobuf-like value.
  kProtoPtrType = 7,      ///< Protobuf-like raw pointer; caller-owned.
  kFlatTableType = 8,     ///< FlatBuffers NativeTable (object API).
  kFlatPtrType = 9,       ///< Pointer to @c flatbuffers::Table (zero-copy view).
  kFlatBuilderType = 10,  ///< FlatBuffers builder (@c fbb_ + @c Finish()).
  kStringType = 11,       ///< @c std::string payload bytes; no text-encoding validation.
  kCharsType = 12,        ///< C string serialisation; deserialise as @c std::string.
  kStreamType = 13,       ///< Stream-serialisable via @c std::stringstream.
  kStandardType = 14,     ///< Trivial standard-layout struct (POD value).
  kStandardPtrType = 15,  ///< Pointer to trivial standard-layout struct (POD pointer).
};

/**
 * @brief Reports whether @p type identifies a usable codec.
 *
 * @details
 * @c kUnknownType is the only unsupported value.  Support may be directional:
 * @c kCharsType supports C-string serialisation but requires @c std::string on
 * the deserialisation side.  This function is invoked from the @c static_assert
 * in every primitive constructor so unknown message types fail at compile time
 * with a clear diagnostic.
 *
 * @param type  Codec enumerator.
 * @return      @c false only for @c kUnknownType.
 */
[[maybe_unused]] [[nodiscard]] static constexpr bool is_supported(Type type) noexcept;

/**
 * @brief Resolves the codec @c Type for @c T at compile time.
 *
 * @details
 * Evaluates the @c if-constexpr chain documented above and returns the
 * first matching enumerator.  Returns @c kUnknownType if no codec matches.
 *
 * @tparam T  C++ message type to classify.
 * @return    Resolved @c Type enumerator.
 */
template <typename T>
[[nodiscard]] static constexpr Type get_type_of() noexcept;

/**
 * @brief Returns the coarse schema family for @c T with an explicit codec tag.
 *
 * @tparam TypeT  Explicit VLink codec kind.
 * @tparam T      C++ message type to classify.
 * @return        @c SchemaType::kProtobuf, @c kFlatbuffers, @c kZeroCopy, @c kCdr, or @c kRaw.
 */
template <Type TypeT, typename T>
[[nodiscard]] static constexpr SchemaType get_schema_type() noexcept;

/**
 * @brief Returns the coarse schema family inferred from @c T alone.
 *
 * @tparam T  C++ message type to classify.
 * @return    @c SchemaType::kProtobuf, @c kFlatbuffers, @c kZeroCopy, @c kCdr, or @c kRaw.
 */
template <typename T>
[[nodiscard]] static constexpr SchemaType get_schema_type() noexcept;

/**
 * @brief Returns the serialised type-name string for @c T with explicit codec tag.
 *
 * @details
 * Used by the framework for cross-peer type matching (DDS topic type name,
 * Protobuf fully-qualified name, FlatBuffers table name, etc.).  Returns an
 * empty string for codecs with no meaningful type name (e.g. @c kBytesType).
 *
 * @tparam TypeT  Explicit codec kind.
 * @tparam T      C++ message type.
 * @return        Type-name string; empty if not applicable.
 */
template <Type TypeT, typename T>
[[nodiscard]] static std::string get_serialized_type() noexcept;

/**
 * @brief Returns the serialised type-name string for @c T (codec auto-detected).
 *
 * @tparam T  C++ message type.
 * @return    Type-name string; empty if not applicable.
 */
template <typename T>
[[nodiscard]] static std::string get_serialized_type() noexcept;

/**
 * @brief Returns a serialised-size hint for @p src with explicit codec tag.
 *
 * @details
 * Used to size loaned buffers ahead of serialisation.  The returned value is
 * an exact byte count only for codecs that can produce one cheaply; it is
 * @c 0 for codecs that cannot report an upfront size (e.g. @c kBytesType,
 * @c kStringType, @c kFlatTableType, @c kStandardType).
 *
 * @tparam TypeT  Codec kind.
 * @tparam T      C++ message type.
 * @param src     Source value to measure.
 * @return        Byte-count hint; @c 0 if unknown.
 */
template <Type TypeT, typename T>
[[nodiscard]] static size_t get_serialized_size(const T& src) noexcept;

/**
 * @brief Returns a serialised-size hint for @p src (codec auto-detected).
 *
 * @tparam T   C++ message type.
 * @param src  Source value to measure.
 * @return     Byte-count hint; @c 0 if unknown.
 */
template <typename T>
[[nodiscard]] static size_t get_serialized_size(const T& src) noexcept;

/**
 * @brief Serialises @p src into @p des with explicit codec and transport tags.
 *
 * @details
 * @p transport identifies the active transport. CDR output is the same
 * encapsulated byte representation for every transport. @p offset reserves
 * that many bytes before the payload for transport framing; their initial
 * contents are unspecified.
 *
 * For @c kFlatBuilderType, serialisation calls the builder's @c Finish()
 * path so @p src may be mutated.  Because the final size is unavailable
 * before @c Finish(), its size hint is @c 0 and a loaned destination is
 * rejected without changing either the loan or the builder.  Successful
 * serialisation returns an owning copy.
 *
 * When @c TypeT is @c kSomeipType, destination storage must not overlap
 * any storage reachable from @p src.
 *
 * @tparam TypeT       Codec kind.
 * @tparam T           C++ message type.
 * @param src          Source value to serialise.
 * @param des          Destination @c Bytes buffer (may be loaned).
 * @param transport    Active transport back-end.
 * @param offset       Number of header bytes to prepend (default @c 0).
 * @return             @c true on success; @c false on codec failure.
 */
template <Type TypeT, typename T>
static bool serialize(const T& src, Bytes& des, TransportType transport = TransportType::kUnknown, uint8_t offset = 0);

/**
 * @brief Serialises @p src into @p des (codec and transport auto-detected).
 *
 * @details
 * When @c T is inferred as @c kSomeipType, destination storage must not
 * overlap any storage reachable from @p src.
 *
 * @tparam T   C++ message type.
 * @param src  Source value.
 * @param des  Destination @c Bytes buffer.
 * @return     @c true on success.
 */
template <typename T>
static bool serialize(const T& src, Bytes& des);

/**
 * @brief Serialises into transport-provided storage when available.
 *
 * @details
 * When @p use_loan is true and a non-zero size hint is available, @p loan is
 * called and may return either loaned or owning storage.  A zero hint falls
 * back to normal owning serialisation.  FlatBuilder sources are finished
 * before requesting their exact-size destination, including when that request
 * subsequently fails.  Other codecs use @c get_serialized_size() followed by
 * the normal @c serialize() path.  A non-zero size hint requires storage of
 * exactly that size.  A codec must not replace transport-loaned storage.
 * For @c kSomeipType, storage returned by @p loan must not overlap any
 * storage reachable from @p src.
 *
 * @tparam TypeT  Codec kind.
 * @tparam T       C++ message type.
 * @tparam LoanCallbackT  Callable compatible with @c Bytes(size_t).
 * @param src      Source value to serialise.
 * @param des      Destination populated on success; it may be modified on failure.
 * @param transport Active transport back-end.
 * @param use_loan Whether to request transport-provided storage.
 * @param loan     Destination provider called at most once.
 * @return @c true on success; @c false on allocation, size, or codec failure.
 */
template <Type TypeT, typename T, typename LoanCallbackT>
static bool serialize_to_transport(const T& src, Bytes& des, TransportType transport, bool use_loan,
                                   LoanCallbackT&& loan);

/**
 * @brief Deserialises @p src into @p des with explicit codec and transport tags.
 *
 * @details
 * CDR input must contain its DDS encapsulation header and is decoded
 * identically for every transport. @c kCharsType destinations are rejected
 * because a raw pointer cannot carry ownership of the required null-terminated
 * storage. Use @c std::string for deserialisation.  When @c TypeT is
 * @c kSomeipType, source storage must not overlap any storage reachable from
 * @p des.  Exceptions raised by Dynamic or Custom decoding operators retain
 * the operator's own exception contract.
 *
 * @tparam TypeT       Codec kind.
 * @tparam T           C++ message type.
 * @param src          Source @c Bytes buffer.
 * @param des          Destination value to fill.
 * @param transport    Active transport back-end.
 * @return             @c true on success; @c false on parse failure.
 */
template <Type TypeT, typename T>
static bool deserialize(const Bytes& src, T& des, TransportType transport = TransportType::kUnknown);

/**
 * @brief Deserialises @p src into @p des (codec and transport auto-detected).
 *
 * @details
 * When @c T is inferred as @c kSomeipType, source storage must not overlap
 * any storage reachable from @p des.  Exceptions raised by inferred Dynamic
 * or Custom decoding operators retain the operator's own exception contract.
 *
 * @tparam T   C++ message type.
 * @param src  Source @c Bytes buffer.
 * @param des  Destination value.
 * @return     @c true on success.
 */
template <typename T>
static bool deserialize(const Bytes& src, T& des);

/**
 * @brief Converts between two types where at least one side is @c Bytes.
 *
 * @details
 * A compile-time @c static_assert enforces that @c SrcT or @c DesT (or both)
 * is @c Bytes.  The three cases are:
 * - Both @c Bytes: shallow-copies @p src to @p des.
 * - @c DesT == @c Bytes: dispatches to @c serialize().
 * - @c SrcT == @c Bytes: dispatches to @c deserialize().
 *
 * @tparam SrcT  Source type.
 * @tparam DesT  Destination type.
 * @param src    Source value.
 * @param des    Destination value.
 * @return       @c true on success.
 */
template <typename SrcT, typename DesT>
static bool convert(const SrcT& src, DesT& des);

/**
 * @brief Dereferences a value, unwrapping @c std::shared_ptr when present.
 *
 * @details
 * If @c T is @c std::shared_ptr\<U\>, returns @c *t; otherwise returns @c t.
 * Internal helper so codec code can treat both value and shared-pointer
 * inputs uniformly.
 *
 * @tparam T  Input type (value or @c shared_ptr).
 * @param t   Input value.
 * @return    Reference to the underlying value.
 */
template <typename T>
[[nodiscard]] static constexpr auto& deref(const T& t) noexcept;

/**
 * @brief Reports whether @c T is exactly @c Bytes.
 *
 * @tparam T  Type to test.
 * @return    @c true for @c Bytes.
 */
template <typename T>
[[nodiscard]] static constexpr bool is_bytes_type() noexcept;

/**
 * @brief Reports whether @c T is a VLink dynamic data type.
 *
 * @details
 * Dynamic types expose an @c is_vlink_dynamic_data() member.
 *
 * @tparam T  Type to test.
 * @return    @c true for dynamic data types.
 */
template <typename T>
[[nodiscard]] static constexpr bool is_dynamic_type() noexcept;

/**
 * @brief Reports whether @c T is a FastDDS CDR-serialisable type.
 *
 * @details
 * Requires @c VLINK_HAS_CDR, plus either both
 * @c serialize(Cdr&) and @c deserialize(Cdr&) methods, or a type name
 * carrying the @c VLINK_DDS_IDL_PREFIX prefix. When @c VLINK_HAS_ROS2 is
 * defined, ROS2 message traits are also recognised.
 *
 * @tparam T  Type to test.
 * @return    @c true for supported CDR types.
 */
template <typename T>
[[nodiscard]] static constexpr bool is_cdr_type() noexcept;

/**
 * @brief Reports whether @c T is a Protobuf-like message value type.
 *
 * @details
 * Requires Protobuf to be available and the type to expose
 * @c SerializeToArray() and @c ParseFromArray() methods.
 *
 * @tparam T  Type to test.
 * @return    @c true for Protobuf-compatible value types.
 */
template <typename T>
[[nodiscard]] static constexpr bool is_proto_type() noexcept;

/**
 * @brief Reports whether @c T is a raw pointer to a Protobuf-like message.
 *
 * @details
 * The pointee is not owned by the serialiser and must be non-null whenever
 * the codec path dereferences it.
 *
 * @tparam T  Pointer type to test.
 * @return    @c true for Protobuf-compatible pointer types.
 */
template <typename T>
[[nodiscard]] static constexpr bool is_proto_ptr_type() noexcept;

/**
 * @brief Reports whether @c T is a FlatBuffers NativeTable type.
 *
 * @details
 * Requires @c flatbuffers and the type (or its @c shared_ptr element type)
 * to derive from @c flatbuffers::NativeTable.
 *
 * @tparam T  Type to test.
 * @return    @c true for FlatBuffers NativeTable types.
 */
template <typename T>
[[nodiscard]] static constexpr bool is_flat_table_type() noexcept;

/**
 * @brief Reports whether @c T is a FlatBuffers builder type.
 *
 * @details
 * Requires @c flatbuffers and the type to expose both an @c fbb_ member
 * and a @c Finish() method.
 *
 * @tparam T  Type to test.
 * @return    @c true for FlatBuffers builder types.
 */
template <typename T>
[[nodiscard]] static constexpr bool is_flat_builder_type() noexcept;

/**
 * @brief Reports whether @c T is a raw pointer to a @c flatbuffers::Table.
 *
 * @tparam T  Pointer type to test.
 * @return    @c true for FlatBuffers Table pointer types.
 */
template <typename T>
[[nodiscard]] static constexpr bool is_flat_ptr_type() noexcept;

/**
 * @brief Reports whether @c T is a macro-declared SOME/IP structure.
 *
 * @details
 * Detects the static @c is_vlink_someip_type() marker generated by
 * @c VLINK_SOMEIP_FIELDS after unwrapping @c std::shared_ptr.
 *
 * @tparam T  Type to test.
 * @return    @c true for macro-declared SOME/IP structures.
 */
template <typename T>
[[nodiscard]] static constexpr bool is_someip_type() noexcept;

/**
 * @brief Reports whether @c T provides a custom @c operator>>/<< codec.
 *
 * @details
 * Checked via @c Traits::Operatorable for @c operator>>(Bytes&) and
 * @c operator<<(const Bytes&).
 *
 * @tparam T  Type to test.
 * @return    @c true for custom-codec types.
 */
template <typename T>
[[nodiscard]] static constexpr bool is_custom_type() noexcept;

/**
 * @brief Reports whether @c T is @c std::string after unwrapping @c shared_ptr.
 *
 * @tparam T  Type to test.
 * @return    @c true for @c std::string and @c std::shared_ptr\<std::string\>.
 */
template <typename T>
[[nodiscard]] static constexpr bool is_string_type() noexcept;

/**
 * @brief Reports whether @c T is a pointer or array of non-volatile @c char.
 *
 * @details
 * Matches @c char*, @c const char*, and string literal source types for
 * serialisation.  Deserialisation into a raw character pointer is rejected
 * because the pointer cannot own the decoded storage; use @c std::string as
 * the destination.  The trait deliberately excludes other string-like types
 * such as @c std::string_view because the chars codec requires a null-terminated
 * source when serialising.  Arrays must contain a null terminator within their
 * extent; pointer callers must guarantee that a terminator is reachable.  Bytes
 * after the first terminator are ignored.
 *
 * @tparam T  Type to test.
 * @return    @c true for C-string-compatible types.
 */
template <typename T>
[[nodiscard]] static constexpr bool is_chars_type() noexcept;

/**
 * @brief Reports whether @c T supports bidirectional @c std::stringstream streaming.
 *
 * @details
 * Detected via @c Traits::Operatorable\<std::stringstream, T\>(); the check
 * requires both @c ss << t and @c ss >> t to be well-formed.  Higher-priority
 * codecs are checked first in @c get_type_of(), so this function is only
 * reached for types that fail every earlier trait.
 *
 * @tparam T  Type to test.
 * @return    @c true for stream-serialisable types.
 */
template <typename T>
[[nodiscard]] static constexpr bool is_stream_type() noexcept;

/**
 * @brief Reports whether @c T is a trivial standard-layout value (POD).
 *
 * @details
 * Matches non-pointer types where both @c std::is_trivial_v and
 * @c std::is_standard_layout_v hold.  Such types are byte-copied into and
 * out of a @c Bytes buffer of @c sizeof(T) bytes.
 *
 * @tparam T  Type to test.
 * @return    @c true for POD value types.
 */
template <typename T>
[[nodiscard]] static constexpr bool is_standard_type() noexcept;

/**
 * @brief Reports whether @c T is a pointer to a trivial standard-layout type.
 *
 * @details
 * Matches @c U* where @c std::is_trivial_v\<U\> && @c std::is_standard_layout_v\<U\>.
 * The pointer is reinterpreted (not copied through) for zero-copy use.
 *
 * @tparam T  Pointer type to test.
 * @return    @c true for POD-pointer types.
 */
template <typename T>
[[nodiscard]] static constexpr bool is_standard_ptr_type() noexcept;

}  // namespace Serializer

}  // namespace vlink

#include "./internal/serializer-inl.h"
