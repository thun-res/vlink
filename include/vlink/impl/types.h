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
 * @file types.h
 * @brief Core enumerations and small value types shared by the entire VLink implementation layer.
 *
 * @details
 * This is an internal implementation header consumed by every node template
 * (@c Publisher, @c Subscriber, @c Client, @c Server, @c Setter, @c Getter),
 * every @c Conf subclass and every @c NodeImpl backend.  It is also re-exported
 * to applications via @c vlink.h so that user code can refer to enums such as
 * @c SecurityType when instantiating the public node templates.
 *
 * @par ImplType
 * Role bitmask consumed by @c VLINK_ALLOW_IMPL_TYPE to restrict which node
 * categories a given @c Conf can produce.
 *
 * | Value               | Hex  | Meaning                                  |
 * | ------------------- | ---- | ---------------------------------------- |
 * | @c kUnknownImplType | 0x00 | Type not yet determined.                 |
 * | @c kPublisher       | 0x01 | Event publisher node.                    |
 * | @c kSubscriber      | 0x02 | Event subscriber node.                   |
 * | @c kSetter          | 0x04 | Field setter node.                       |
 * | @c kGetter          | 0x08 | Field getter node.                       |
 * | @c kServer          | 0x10 | Method server node.                      |
 * | @c kClient          | 0x20 | Method client node.                      |
 *
 * @par TransportType
 * Resolved at URL construction time from the URI scheme.
 *
 * | Value        | URL prefix    | Backend                              |
 * | ------------ | ------------- | ------------------------------------ |
 * | @c kUnknown  | (none)        | Unknown or unsupported.              |
 * | @c kIntra    | @c intra://   | In-process queue (no serialisation). |
 * | @c kShm      | @c shm://     | Iceoryx shared memory.               |
 * | @c kShm2     | @c shm2://    | Iceoryx2 shared memory.              |
 * | @c kZenoh    | @c zenoh://   | Zenoh publish / subscribe.           |
 * | @c kDds      | @c dds://     | Fast-DDS RTPS.                       |
 * | @c kDdsc     | @c ddsc://    | CycloneDDS.                          |
 * | @c kDdsr     | @c ddsr://    | RTI DDS.                             |
 * | @c kSomeip   | @c someip://  | SOME/IP through vsomeip.             |
 * | @c kMqtt     | @c mqtt://    | MQTT publish / subscribe.            |
 * | @c kFdbus    | @c fdbus://   | FDBus IPC.                           |
 *
 * @par InitType
 * Controls whether the public Node<> template runs @c init() immediately or
 * defers it so the user can adjust properties beforehand.
 *
 * | Value             | Meaning                                        |
 * | ----------------- | ---------------------------------------------- |
 * | @c kWithoutInit   | Defer initialisation; call @c init() manually. |
 * | @c kWithInit      | Initialise immediately in the constructor.     |
 *
 * @par Cross-references
 * - @c ImplType -- chosen at the @c NodeImpl base level; surfaced via
 *   @c NodeImpl::impl_type.
 * - @c SecurityType -- second template parameter on every public node type
 *   (@c Publisher<T, SecurityType>, @c Subscriber<T, SecurityType>, ...).
 * - @c InitType -- argument to the public node constructors that toggles
 *   immediate versus deferred initialisation.
 */

#pragma once

#include <chrono>
#include <cstdint>
#include <iostream>
#include <string>
#include <string_view>

#include "../base/bytes.h"
#include "../base/functional.h"
#include "../base/macros.h"

namespace vlink {

/**
 * @enum ImplType
 * @brief Bitmask identifying the role of a VLink node implementation.
 *
 * @details
 * Values may be combined with bitwise OR to express compound capabilities,
 * for example @c (kPublisher | kSubscriber) for a backend that handles both
 * roles on the same topic.  @c VLINK_ALLOW_IMPL_TYPE uses these combined flags
 * to gate the @c Conf factory at compile time.
 */
enum ImplType : uint8_t {
  kUnknownImplType = 0,  ///< Type not yet determined.
  kServer = 16,          ///< Method server (RPC responder).
  kClient = 32,          ///< Method client (RPC caller).
  kPublisher = 1,        ///< Event publisher (one-to-many broadcast).
  kSubscriber = 2,       ///< Event subscriber (receives broadcasts).
  kSetter = 4,           ///< Field setter (writes latest value).
  kGetter = 8,           ///< Field getter (reads latest value).
};

/**
 * @enum TransportType
 * @brief Enumeration of every transport backend recognised by VLink.
 *
 * @details
 * Resolved by @c Url when the URL string is parsed.  Concrete @c Conf classes
 * report their own backend through @c get_transport_type().
 */
enum class TransportType : uint8_t {
  kUnknown = 0,  ///< Unknown or unsupported transport.
  kIntra = 1,    ///< In-process queue (@c intra://).
  kShm = 2,      ///< Iceoryx shared memory (@c shm://).
  kShm2 = 3,     ///< Iceoryx2 shared memory (@c shm2://).
  kZenoh = 4,    ///< Zenoh publish / subscribe (@c zenoh://).
  kDds = 5,      ///< Fast-DDS RTPS (@c dds://).
  kDdsc = 6,     ///< CycloneDDS (@c ddsc://).
  kDdsr = 7,     ///< RTI DDS (@c ddsr://).
  kSomeip = 9,   ///< SOME/IP through vsomeip (@c someip://).
  kMqtt = 10,    ///< MQTT (@c mqtt://).
  kFdbus = 11,   ///< FDBus IPC (@c fdbus://).
};

/**
 * @enum InitType
 * @brief Selects between immediate and deferred node initialisation.
 *
 * @details
 * Pass @c kWithoutInit when the application needs to call
 * @c Publisher::set_property() (or similar) before the underlying transport
 * starts; otherwise the default @c kWithInit performs the full init inside
 * the constructor.
 */
enum class InitType : uint8_t {
  kWithoutInit = 0,  ///< Defer initialisation; call init() manually.
  kWithInit = 1,     ///< Initialise immediately in the constructor.
};

/**
 * @enum SecurityType
 * @brief Compile-time selector for the per-node message security variant.
 *
 * @details
 * Used as the second template argument of @c Publisher<T, SecurityType>,
 * @c Subscriber<T, SecurityType> and the rest of the public node templates.
 * @c kWithSecurity enables authenticated AES-128-GCM encryption (optionally
 * wrapped with RSA-OAEP and signed with RSA-PSS) over the serialised payload.
 * Both the @c intra:// transport and DDS variants using native CDR rejection
 * the configuration; on those transports the security-prefixed node simply
 * has no usable security object once @c init() runs.
 */
enum class SecurityType : uint8_t {
  kWithoutSecurity = 0,  ///< Plain, unauthenticated transport.
  kWithSecurity = 1,     ///< Encrypted and authenticated transport.
};

/**
 * @enum ActionType
 * @brief Labels for messages captured by the recording infrastructure.
 *
 * @details
 * Used by @c NodeImpl::try_record() so the bag writer can reconstruct message
 * flow across nodes during playback.
 */
enum class ActionType : uint8_t {
  kUnknownAction = 0,   ///< Action category is not classified.
  kClientRequest = 1,   ///< RPC request emitted by a Client node.
  kClientResponse = 2,  ///< RPC response observed by a Client node.
  kServerRequest = 3,   ///< RPC request observed by a Server node.
  kServerResponse = 4,  ///< RPC response emitted by a Server node.
  kPublish = 5,         ///< Message emitted by a Publisher node.
  kSubscribe = 6,       ///< Message observed by a Subscriber node.
  kSet = 7,             ///< Value written by a Setter node.
  kGet = 8              ///< Value observed by a Getter node.
};

/**
 * @enum SchemaType
 * @brief Coarse runtime schema family used by discovery, bag metadata and proxy routing.
 *
 * @details
 * The wire / type identifier proper lives in @c ser_type (for example a
 * protobuf fully qualified name or a FlatBuffers table name); @c SchemaType
 * captures only the high-level decoding family so tools can pick the matching
 * decoder without having to mirror the @c ser_type enum.
 */
enum class SchemaType : uint8_t {
  kUnknown = 0,      ///< Decoding family unknown.
  kRaw = 1,          ///< Treat the payload as opaque bytes.
  kZeroCopy = 2,     ///< Decode through the VLink zero-copy structs.
  kProtobuf = 3,     ///< Decode through the Protocol Buffers stack.
  kFlatbuffers = 4,  ///< Decode through the FlatBuffers stack.
};

/**
 * @struct Frame
 * @brief One recorded or replayed message as it flows through the bag pipeline.
 *
 * @details
 * The single unit passed through every bag push / callback / record hook (@c BagReader,
 * @c BagWriter, @c BagProcessor, @c BagPluginInterface).  Passed @b by @b const @b reference through
 * each hop so the payload is never deep-copied on the synchronous path; a stage that must take
 * ownership (an async reorder cache) or rewrite a field (playback URL remap) copies explicitly, and
 * the payload @c Bytes itself is normally a shallow view at the source.  @c ser_type / @c schema_type
 * are supplied by the caller on the write side; on the read side @c BagReader fills them from the
 * effective URL metadata before invoking either a plugin's @c on_read() hook or the user's output
 * callback.
 */
struct Frame final {
  int64_t timestamp{-1};                         ///< Canonical time in microseconds (record / playback).
  std::string url;                               ///< Topic URL.
  std::string ser_type;                          ///< Serialisation type; reader fills it before read-side hooks.
  SchemaType schema_type{SchemaType::kUnknown};  ///< Coarse schema family; reader fills it before read-side hooks.
  ActionType action_type{ActionType::kUnknownAction};  ///< Recorded action kind.
  Bytes data;                                          ///< Serialised payload bytes.
};

/**
 * @brief Frame sink reused by every bag frame interface.
 *
 * @details
 * Takes the frame by @c const reference so no copy occurs on the synchronous path; a stage that must
 * take ownership (an async reorder cache) or rewrite a field (playback URL remap) copies explicitly,
 * and the payload @c Bytes itself is normally a shallow view at the source.
 */
using FrameCallback = MoveFunction<void(const Frame&)>;

/**
 * @struct Timeout
 * @brief Compile-time timeout constants used by the public blocking wait helpers.
 *
 * @details
 * Provides canonical values for the @c wait_for_* family on @c Publisher,
 * @c Subscriber, @c Client and friends.
 */
struct Timeout final {
  [[maybe_unused]] static constexpr std::chrono::milliseconds kDefaultInterval{
      5'000};                                                                 ///< Default wait timeout: 5 seconds.
  [[maybe_unused]] static constexpr std::chrono::milliseconds kInfinite{-1};  ///< Wait indefinitely (negative timeout).
};

/**
 * @struct SampleLostInfo
 * @brief Aggregate of cumulative delivered / lost sample counts.
 *
 * @details
 * Returned by @c SubscriberImpl::get_lost() and @c GetterImpl::get_lost().
 * @c total counts every message that was expected (delivered or lost);
 * @c lost counts the subset that did not arrive.  Stream-friendly through
 * @c operator<<.
 */
struct SampleLostInfo final {
  uint64_t total{0};  ///< Total number of samples expected (delivered + lost).
  uint64_t lost{0};   ///< Number of samples that were dropped or missed.

  /**
   * @brief Streams a human-readable summary to @p ostream.
   *
   * @details
   * Output format: @c "SampleLostInfo:[total]N[lost]M".
   *
   * @param ostream  Destination stream.
   * @param info     Instance to print.
   * @return Reference to @p ostream.
   */
  VLINK_EXPORT friend std::ostream& operator<<(std::ostream& ostream, const SampleLostInfo& info) noexcept;
};

/**
 * @struct SchemaData
 * @brief Wire-format-neutral wrapper around one serialised schema blob.
 *
 * @details
 * Consumed by schema-aware tooling such as bag readers, MCAP writers and
 * schema plugins.  @c encoding stores the original schema payload encoding
 * (for example @c "protobuf", @c "flatbuffers" or @c "vlink_msg"); the
 * @c schema_type field exposes the coarse runtime family used by discovery,
 * bag routing and proxy consumers.
 */
struct VLINK_EXPORT SchemaData final {
  std::string name;      ///< Schema subject (usually a fully qualified message or table name).
  std::string encoding;  ///< Schema encoding identifier, e.g. @c "protobuf" or @c "flatbuffers".
  SchemaType schema_type{SchemaType::kUnknown};  ///< Coarse runtime family derived from @c encoding.
  Bytes data;                                    ///< Raw serialised schema bytes (FileDescriptorSet, BFBS, ...).

  /**
   * @brief Returns whether @p schema_type is within the supported enum range.
   *
   * @param schema_type  Value to validate.
   * @return @c true when @p schema_type names a defined enum member.
   */
  [[nodiscard]] static bool is_valid_type(SchemaType schema_type) noexcept;

  /**
   * @brief Returns whether @p schema_type carries concrete schema metadata.
   *
   * @details
   * Unlike @c is_valid_type(), excludes @c kUnknown and @c kRaw.  Used by
   * schema caching / bag embedding code to decide whether the schema can be
   * indexed or persisted as a real schema entry.
   *
   * @param schema_type  Value to classify.
   * @return @c true for protobuf, flatbuffers and zero-copy families.
   */
  [[nodiscard]] static bool is_real_type(SchemaType schema_type) noexcept;

  /**
   * @brief Converts a schema type to its canonical persisted encoding label.
   *
   * @param schema_type  Value to convert.
   * @return Canonical encoding string, or an empty view for unknown values.
   */
  [[nodiscard]] static std::string_view convert_type(SchemaType schema_type) noexcept;

  /**
   * @brief Parses an encoding string back into a @c SchemaType value.
   *
   * @param encoding  Encoding label such as @c "protobuf", @c "fbs", @c "blob" or @c "zerocopy".
   * @return Matching @c SchemaType, or @c SchemaType::kUnknown.
   */
  [[nodiscard]] static SchemaType convert_encoding(std::string_view encoding) noexcept;

  /**
   * @brief Infers a coarse schema family from a concrete @c ser_type string.
   *
   * @details
   * Intentionally conservative:
   * - Zero-copy types are recognised by the @c "vlink::zerocopy::" prefix.
   * - Textual / raw payload types map to @c SchemaType::kRaw.
   * - Protobuf and FlatBuffers are not guessed from name alone.
   *
   * @param ser_type  Concrete serialisation type string.
   * @return Inferred schema family, or @c SchemaType::kUnknown.
   */
  [[nodiscard]] static constexpr SchemaType infer_ser_type(std::string_view ser_type) noexcept;

  /**
   * @brief Resolves the best available schema family from explicit, encoding and ser hints.
   *
   * @details
   * Resolution order:
   * -# Use @p schema_type when it is already known.
   * -# Otherwise infer from @p encoding.
   * -# Otherwise infer from @p ser_type.
   *
   * @param schema_type  Explicit schema family hint.
   * @param ser_type     Concrete serialisation type string.
   * @param encoding     Persisted schema encoding label.
   * @return Best-effort schema family, or @c SchemaType::kUnknown.
   */
  [[nodiscard]] static SchemaType resolve_type(SchemaType schema_type, std::string_view ser_type = {},
                                               std::string_view encoding = {}) noexcept;
};

/**
 * @struct Version
 * @brief Semantic version number with comparison and string-conversion helpers.
 *
 * @details
 * Used by @c NodeImpl::check_version() to compare a build-time application
 * version against the live VLink library version.  All components default to
 * @c -1, which marks the value as not yet set.
 */
struct VLINK_EXPORT Version final {
  int major{-1};  ///< Major version number; @c -1 when unset.
  int minor{-1};  ///< Minor version number; @c -1 when unset.
  int patch{-1};  ///< Patch version number; @c -1 when unset.

  /**
   * @brief Tests equality with @p target.
   *
   * @param target  Version to compare against.
   * @return @c true when major, minor and patch all match.
   */
  [[nodiscard]] bool operator==(const Version& target) const noexcept;

  /**
   * @brief Tests inequality with @p target.
   *
   * @param target  Version to compare against.
   * @return Logical negation of @c operator==.
   */
  [[nodiscard]] bool operator!=(const Version& target) const noexcept;

  /**
   * @brief Reports whether this version is strictly older than @p target.
   *
   * @details
   * Numeric ordering: compares major first, then minor, then patch.
   *
   * @param target  Version to compare against.
   * @return @c true when this version is less than @p target.
   */
  [[nodiscard]] bool operator<(const Version& target) const noexcept;

  /**
   * @brief Reports whether this version is strictly newer than @p target.
   *
   * @param target  Version to compare against.
   * @return @c true when this version is greater than @p target.
   */
  [[nodiscard]] bool operator>(const Version& target) const noexcept;

  /**
   * @brief Parses a version string in @c "major.minor.patch" form.
   *
   * @details
   * Each component is decoded with @c std::from_chars; components missing
   * from @p version_str retain the @c -1 sentinel.
   *
   * @param version_str  Source string such as @c "2.1.0".
   * @return Parsed @c Version.
   */
  [[nodiscard]] static Version from_string(const std::string& version_str) noexcept;

  /**
   * @brief Serialises this version back to a @c "major.minor.patch" string.
   *
   * @return Formatted version string, e.g. @c "2.1.0".
   */
  [[nodiscard]] std::string to_string() const noexcept;

  /**
   * @brief Returns whether every component has been set to a non-negative value.
   *
   * @return @c true when the version has been parsed or assigned explicitly.
   */
  [[nodiscard]] bool is_valid() const noexcept;
};

////////////////////////////////////////////////////////////////
/// Details
////////////////////////////////////////////////////////////////

constexpr SchemaType SchemaData::infer_ser_type(std::string_view ser_type) noexcept {
  constexpr auto kHasPrefixFunction = [](std::string_view value, std::string_view prefix) noexcept {
    if VUNLIKELY (prefix.size() > value.size()) {
      return false;
    }

    for (size_t i = 0; i < prefix.size(); ++i) {
      if (value[i] != prefix[i]) {
        return false;
      }
    }

    return true;
  };

  if (kHasPrefixFunction(ser_type, "vlink::zerocopy::")) {
    return SchemaType::kZeroCopy;
  }

  if (ser_type == "raw" || ser_type == "string" || ser_type == "std::string" || ser_type == "text" ||
      ser_type == "json" || ser_type == "application/json" || ser_type == "text/json") {
    return SchemaType::kRaw;
  }

  return SchemaType::kUnknown;
}

}  // namespace vlink
