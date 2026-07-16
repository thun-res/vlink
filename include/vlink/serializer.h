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
 * | Constant            | C++ Type Criterion                  | Trait check             | Notes                       |
 * | ------------------- | ----------------------------------- | ----------------------- | --------------------------- |
 * | @c kBytesType       | @c T == @c Bytes                    | @c is_bytes_type        | Pass-through, no codec.     |
 * | @c kDynamicType     | Has @c is_vlink_dynamic_data()      | @c is_dynamic_type      | Dynamic typed data.         |
 * | @c kCdrType         | Has @c serialize/deserialize(Cdr&)  | @c is_cdr_type          | FastDDS CDR fast path.      |
 * | @c kProtoType       | Has SerializeToArray/ParseFromArray | @c is_proto_type        | Protobuf-like value.        |
 * | @c kProtoPtrType    | Pointer with proto serialize/parse  | @c is_proto_ptr_type    | Caller owns the pointee.    |
 * | @c kFlatTableType   | Derives from FB NativeTable         | @c is_flat_table_type   | FlatBuffers object API.     |
 * | @c kFlatPtrType     | Pointer to @c flatbuffers::Table    | @c is_flat_ptr_type     | Zero-copy FlatBuffers view. |
 * | @c kFlatBuilderType | Has @c fbb_ member and @c Finish()  | @c is_flat_builder_type | FlatBuffers builder.        |
 * | @c kCustomType      | Has @c operator>>/<<(Bytes&)        | @c is_custom_type       | User-supplied codec.        |
 * | @c kStringType      | @c T == @c std::string              | @c is_string_type       | UTF-8 string.               |
 * | @c kCharsType       | Pointer/array of non-volatile char   | @c is_chars_type        | C string / @c char*. | | @c
 * kStreamType      | Streamable via @c std::stringstream | @c is_stream_type       | Reached only as fallback.   | | @c
 * kStandardType    | Trivial standard-layout value (POD) | @c is_standard_type     | @c sizeof(T) byte copy.     | | @c
 * kStandardPtrType | Pointer to trivial standard-layout  | @c is_standard_ptr_type | Zero-copy POD pointer.      |
 *
 * Most value-like detectors unwrap @c std::shared_ptr\<T\> before matching
 * (e.g. Protobuf values, CDR values, FlatBuffers native tables, custom
 * codecs, strings, stream types, and standard-layout values).
 * @par Detection Precedence Flow
 * @verbatim
 *   get_type_of<T>() probes traits in this fixed order; first match wins:
 *
 *     Bytes  --(no)-->  Dynamic  --(no)-->  CDR  --(no)-->  Proto
 *                                                              |
 *                                                              v (no)
 *     FlatPtr  <--(no)--  FlatTable  <--(no)--  ProtoPtr  <--(no)--+
 *         |
 *         v (no)
 *     FlatBuilder  --(no)-->  Custom  --(no)-->  String  --(no)-->  Chars
 *                                                                      |
 *                                                                      v (no)
 *     Stream  <--(no)--  StandardPtr  <--(no)--  Standard  <--(no)----+
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
 * constexpr auto u = vlink::Serializer::get_type_of<int>();       // -> kStandardType (POD)
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
 * @par Transport-aware Fast Path
 * Some transports (e.g. @c dds://) use pointer passing for CDR types.  Use
 * the explicit overloads to opt into the transport-specific path:
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

#include <string>

#include "./base/bytes.h"
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
  kCdrType = 4,           ///< FastDDS CDR via @c serialize(Cdr&) / @c deserialize(Cdr&).
  kProtoType = 5,         ///< Protobuf-like value.
  kProtoPtrType = 6,      ///< Protobuf-like raw pointer; caller-owned.
  kFlatTableType = 7,     ///< FlatBuffers NativeTable (object API).
  kFlatPtrType = 8,       ///< Pointer to @c flatbuffers::Table (zero-copy view).
  kFlatBuilderType = 9,   ///< FlatBuffers builder (@c fbb_ + @c Finish()).
  kStringType = 10,       ///< @c std::string -- UTF-8 text.
  kCharsType = 11,        ///< C string / @c char*.
  kStreamType = 12,       ///< Stream-serialisable via @c std::stringstream.
  kStandardType = 13,     ///< Trivial standard-layout struct (POD value).
  kStandardPtrType = 14,  ///< Pointer to trivial standard-layout struct (POD pointer).
};

/**
 * @brief Reports whether @p type identifies a usable codec.
 *
 * @details
 * @c kUnknownType is the only unsupported value.  This function is invoked
 * from the @c static_assert in every primitive constructor so unsupported
 * message types fail at compile time with a clear diagnostic.
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
 * @return        @c SchemaType::kProtobuf, @c kFlatbuffers, @c kZeroCopy, or @c kRaw.
 */
template <Type TypeT, typename T>
[[nodiscard]] static constexpr SchemaType get_schema_type() noexcept;

/**
 * @brief Returns the coarse schema family inferred from @c T alone.
 *
 * @tparam T  C++ message type to classify.
 * @return    @c SchemaType::kProtobuf, @c kFlatbuffers, @c kZeroCopy, or @c kRaw.
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
 * @p transport activates transport-specific fast paths (e.g. CDR pointer
 * passing on @c kDds).  Pass @c TransportType::kUnknown for the default
 * copy-based path.  @p offset prepends that many zero bytes before the
 * payload (used internally by some transports for framing).
 *
 * For @c kFlatBuilderType, serialisation calls the builder's @c Finish()
 * path so @p src may be mutated.  Because the final size is unavailable
 * before @c Finish(), its size hint is @c 0 and a loaned destination is
 * rejected without changing either the loan or the builder.  Successful
 * serialisation returns an owning copy.
 *
 * @tparam TypeT       Codec kind.
 * @tparam T           C++ message type.
 * @param src          Source value to serialise.
 * @param des          Destination @c Bytes buffer (may be loaned).
 * @param transport    Active transport back-end for fast-path selection.
 * @param offset       Number of header bytes to prepend (default @c 0).
 * @return             @c true on success; @c false on codec failure.
 */
template <Type TypeT, typename T>
static bool serialize(const T& src, Bytes& des, TransportType transport = TransportType::kUnknown, uint8_t offset = 0);

/**
 * @brief Serialises @p src into @p des (codec and transport auto-detected).
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
 * @p transport activates transport-specific fast paths (e.g. @c kDds
 * dereferences a CDR pointer stored in @p src instead of copying bytes).
 * For @c kCharsType, @p des points to null-terminated thread-local scratch
 * storage that remains valid until the next chars deserialisation on the same
 * thread.  Prefer @c std::string when the value must be retained.
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
 * Requires @c fastcdr to be available, plus either both
 * @c serialize(Cdr&) and @c deserialize(Cdr&) methods, or a type name
 * carrying the @c VLINK_FASTDDS_IDL_PREFIX prefix.
 *
 * @tparam T  Type to test.
 * @return    @c true for CDR types when @c fastcdr is available.
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
 * Matches @c char*, @c const char*, and string literal source types.  A chars
 * deserialisation destination must be @c char* or @c const @c char*.  The trait deliberately
 * excludes other string-like types such as @c std::string_view because the
 * chars codec requires a null-terminated source when serialising.  Arrays must
 * contain a null terminator within their extent; pointer callers must guarantee
 * that a terminator is reachable.  Bytes after the first terminator are ignored.
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
