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
 * @file dynamic_data.h
 * @brief Self-describing serialised payload tagged with an inline type name.
 *
 * @details
 * @c DynamicData carries a serialised VLink-compatible message together with a short
 * type-name tag inside a single @c Bytes buffer.  The first @c kOffset = 20 bytes of the
 * buffer hold the type name (NUL-padded), and the remainder holds the payload as produced
 * by @c Serializer.  This layout lets transports route, log and inspect a payload without
 * knowing its concrete C++ type at compile time -- the recipient picks the matching
 * deserialiser at run time using the embedded tag.
 *
 * Supported dynamic categories (chosen automatically by @c Serializer):
 *
 * | Category   | Examples                          | Notes                                      |
 * | ---------- | --------------------------------- | ------------------------------------------ |
 * | Protobuf   | @c MyProto, @c std::shared_ptr<P> | Encoded with the message's wire format     |
 * | FlatBuffer | @c MyTable, @c FlatPtr<T>         | Encoded via @c FlatBuilder                 |
 * | POD/Std    | @c int, @c struct Foo (trivial)   | Standard layout copy                       |
 * | Bytes      | @c vlink::Bytes                   | Stored verbatim                            |
 * | String     | @c std::string                    | Length-prefixed                            |
 * | Chars      | @c char[N], @c const char*        | NUL-terminated                             |
 * | Custom     | Types with explicit @c Serializer | User-provided serialise/deserialise hooks  |
 *
 * Two categories are forbidden and rejected by @c static_assert: nested @c DynamicData
 * (avoids recursive type-name layout) and CDR types (their wire format does not allow
 * the in-place prefix used here).  The type-name literal (including its trailing NUL)
 * must fit in fewer than @c kOffset bytes.
 *
 * Buffer layout:
 *
 * @verbatim
 *   +----------------------------+-----------------------------------------+
 *   |  type name (20 bytes max)  |  serialised payload                     |
 *   +----------------------------+-----------------------------------------+
 *   ^                            ^
 *   data_.real_data()            data_.real_data() + kOffset
 * @endverbatim
 *
 * @par Example
 * @code
 * vlink::DynamicData dd;
 * MyProtoMsg msg;
 * msg.set_value(42);
 * dd.load("MyProtoMsg", msg);                     // serialise + tag
 *
 * // Send over any transport that handles vlink::Bytes:
 * vlink::Bytes wire;
 * dd >> wire;
 *
 * vlink::DynamicData received;
 * received << wire;                               // recover the tagged blob
 * if (received.get_type() == "MyProtoMsg") {
 *   MyProtoMsg out = received.as<MyProtoMsg>();   // deserialise
 *   (void)out;
 * }
 * @endcode
 */

#pragma once

#include <memory>
#include <string_view>
#include <utility>

#include "../serializer.h"

namespace vlink {

/**
 * @class DynamicData
 * @brief Type-erased wrapper that pairs an inline type name with a serialised payload.
 *
 * @details
 * Stores its payload in an internal @c Bytes buffer; the first @c kOffset bytes are
 * reserved for the type name and exposed through @c get_type() as a @c string_view.
 * Copy and move operations rebind the view to the local buffer to avoid dangling.
 */
class VLINK_EXPORT DynamicData final {
 public:
  /**
   * @brief Builds an empty container with no buffer and no type tag.
   */
  DynamicData();

  /**
   * @brief Copies the buffer and rebinds the local type view to point into the new buffer.
   */
  DynamicData(const DynamicData& target);

  /**
   * @brief Adopts the source buffer and rebinds the type view to point at this object's storage.
   *
   * @details
   * The moved-from object is left in an empty state with no payload and no type tag.
   */
  DynamicData(DynamicData&& target) noexcept;

  /**
   * @brief Copy-assignment that rebinds the local type view to point into the new buffer.
   */
  DynamicData& operator=(const DynamicData& target);

  /**
   * @brief Move-assignment that rebinds the local type view and empties the source.
   */
  DynamicData& operator=(DynamicData&& target) noexcept;

  /**
   * @brief Serialises @p t under the type tag @p type into the internal buffer.
   *
   * @details
   * The type literal (including its trailing NUL) must be shorter than @c kOffset, which
   * is checked at compile time.  On serialiser failure the container is reset to empty
   * and an error is logged.
   *
   * @tparam SizeT Length of the type-name string literal including the NUL terminator.
   * @tparam T     Source message type (must be supported by @c Serializer).
   * @param type   String literal containing the type-name tag.
   * @param t      Message instance to serialise.
   * @return Reference to @c *this for chaining.
   */
  template <uint8_t SizeT, typename T>
  DynamicData& load(const char (&type)[SizeT], const T& t);

  /**
   * @brief Deserialises the stored payload into @p t.
   *
   * @details
   * Fails when the container is empty or the @c Serializer cannot decode the buffer
   * (for example because @p T is incompatible with the embedded payload).
   *
   * @tparam T Destination type.
   * @param t  Output reference populated on success.
   * @return @c true on success; @c false otherwise.
   */
  template <typename T>
  bool convert(T& t) const;

  /**
   * @brief Default-constructs a @p T (or @c shared_ptr<T>) and decodes the payload into it.
   *
   * @tparam T Destination type; specialised for @c std::shared_ptr to allocate the inner object.
   * @return Decoded instance, or a default-constructed @p T when decoding fails.
   */
  template <typename T>
  [[nodiscard]] T as() const;

  /**
   * @brief Returns the underlying @c Bytes buffer, including the embedded type name.
   */
  [[nodiscard]] const Bytes& get_data() const;

  /**
   * @brief Returns the embedded type name as a view into the internal buffer.
   *
   * @details
   * Only valid for the lifetime of this @c DynamicData instance.
   */
  [[nodiscard]] const std::string_view& get_type() const;

  /**
   * @brief Returns whether the internal buffer is empty.
   */
  [[nodiscard]] bool is_empty() const;

  /**
   * @brief Byte-equality comparison.
   *
   * @return @c true when both buffers hold identical bytes.
   */
  [[nodiscard]] bool operator==(const DynamicData& target) const;

  /**
   * @brief Byte-inequality comparison.
   */
  [[nodiscard]] bool operator!=(const DynamicData& target) const;

  /**
   * @brief Compile-time trait used by @c Serializer to detect @c DynamicData specialisations.
   *
   * @return Always @c true.
   */
  [[nodiscard]] static constexpr bool is_vlink_dynamic_data();

  /**
   * @brief Returns the byte offset at which the serialised payload begins (= @c kOffset).
   */
  [[nodiscard]] static constexpr uint8_t get_offset();

  /**
   * @brief Reconstructs the container from a wire-format @c Bytes blob.
   *
   * @param bytes Wire blob previously produced by @c operator>>.
   * @return @c true on success.
   */
  bool operator<<(const Bytes& bytes) noexcept;

  /**
   * @brief Emits the container as a wire-format @c Bytes blob.
   *
   * @param bytes Destination buffer.
   * @return @c true on success; @c false when the internal buffer is empty, non-owning,
   *         or does not carry the reserved prefix.
   */
  bool operator>>(Bytes& bytes) const noexcept;

 private:
  void refresh_type_view() noexcept;

  void refresh_type_view(size_t type_size) noexcept;

  static constexpr uint8_t kOffset{20};
  Bytes data_;
  std::string_view type_;
};

////////////////////////////////////////////////////////////////
/// Details
////////////////////////////////////////////////////////////////

template <uint8_t SizeT, typename T>
inline DynamicData& DynamicData::load(const char (&type)[SizeT], const T& t) {
  static_assert(SizeT < get_offset(), "DynamicData name out of size.");
  static_assert(!std::is_same_v<DynamicData, T>, "DynamicData can not serialize self.");
  static_assert(!Serializer::is_cdr_type<T>(), "DynamicData can not serialize cdr data.");

  Bytes next_data;
  bool ret = Serializer::serialize<Serializer::get_type_of<T>()>(t, next_data, TransportType::kUnknown, get_offset());

  if VUNLIKELY (!ret || next_data.real_data() == nullptr) {
    data_ = Bytes();
    type_ = std::string_view();
    VLOG_F("DynamicData serialize failed.");
    return *this;
  }

  data_ = std::move(next_data);

  if constexpr (SizeT > 0) {
    std::memcpy(data_.real_data(), type, SizeT);

    if constexpr (SizeT < kOffset) {
      std::memset(data_.real_data() + SizeT, 0, kOffset - SizeT);
    }

    size_t name_len = SizeT;

    if (name_len > 0 && type[SizeT - 1] == '\0') {
      name_len -= 1;
    }

    type_ = std::string_view(reinterpret_cast<const char*>(data_.real_data()), name_len);
  } else {
    std::memset(data_.real_data(), 0, kOffset);
    type_ = std::string_view();
  }

  return *this;
}

template <typename T>
inline bool DynamicData::convert(T& t) const {
  static_assert(!std::is_same_v<DynamicData, T>, "DynamicData can not deserialize self.");
  static_assert(!Serializer::is_cdr_type<T>(), "DynamicData can not deserialize cdr data.");

  if VUNLIKELY (data_.empty()) {
    return false;
  }

  return Serializer::deserialize<Serializer::get_type_of<T>()>(data_, t, TransportType::kUnknown);
}

template <typename T>
inline T DynamicData::as() const {
  T t;

  if constexpr (Traits::IsSharedPtr<T>()) {
    t = std::make_shared<typename T::element_type>();
  }

  if VUNLIKELY (!convert(t)) {
    VLOG_F("DynamicData deserialize failed.");
    return T{};
  }

  return t;
}

inline constexpr bool DynamicData::is_vlink_dynamic_data() { return true; }

inline constexpr uint8_t DynamicData::get_offset() { return kOffset; }

}  // namespace vlink
