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
 * @file proxy_data.h
 * @brief Routing envelope used by the VLink proxy / monitoring path.
 *
 * @details
 * @c ProxyData bundles a serialised user payload with the metadata that the
 * VLink proxy subsystem needs to forward the message across hosts: topic URL,
 * serialisation type label, source hostname, plus control fields (control id,
 * mode, microsecond timestamp, sequence number, coarse schema family).  All
 * variable-length regions share a single owned allocation so a freshly
 * deserialised envelope can expose each region as a @c string_view or
 * shallow @c Bytes without further allocation.
 *
 * | Region    | Meaning                                              |
 * | --------- | ---------------------------------------------------- |
 * | raw       | Serialised user message bytes                        |
 * | url       | Topic URL (e.g. @c "dds://my/topic")                 |
 * | ser       | Serialisation type label (e.g. @c "proto.MyMsg")     |
 * | hostname  | Source machine identifier (optional)                 |
 *
 * @par Wire format
 * @c ProxyData is POD; @c memcpy serialises the struct.  The @c sizeof value
 * is locked by @c static_assert and forms a permanent contract: the
 * @c vlink::zerocopy::* containers have NO forward or backward binary
 * compatibility across releases.  Pointer / ownership fields inside the wire
 * snapshot must NEVER be interpreted by remote consumers.
 * @code
 * static_assert(sizeof(ProxyData) == 80, "Sizeof must be 80 bytes.");
 * @endcode
 *
 * @par Memory layout
 * @code
 * Offset  Size  Field
 * ------  ----  ---------------------------------
 *      0     8  uint8_t* data_
 *      8     8  size_t   size_
 *     16     4  uint32_t control_id_
 *     20     4  uint32_t mode_
 *     24     8  int64_t  timestamp_
 *     32     8  int64_t  seq_
 *     40     4  uint32_t data_pos_
 *     44     4  uint32_t data_size_
 *     48     4  uint32_t url_pos_
 *     52     4  uint32_t url_size_
 *     56     4  uint32_t ser_pos_
 *     60     4  uint32_t ser_size_
 *     64     4  uint32_t hostname_pos_
 *     68     4  uint32_t hostname_size_
 *     72     4  uint32_t schema_
 *     76     1  bool     is_owner_
 *     77     1  uint8_t  reserved_buf_
 *     78     2  uint16_t reserved_buf2_
 * ------  ----  ---------------------------------
 *  Total    80  bytes (alignas 8)
 *
 * Wire envelope:
 * [ magic_begin (4) | version (4) | ProxyData struct (80) | raw | url | ser | hostname | magic_end (4) ]
 * @endcode
 *
 * @par Reserved bytes
 * The 3 bytes that follow @c is_owner_ are exposed as two explicit wire-locked
 * fields: @c uint8_t @c reserved_buf_ (offset 77) and @c uint16_t @c reserved_buf2_
 * (offset 78), reachable through @c get_reserved() / @c get_reserved2().  They
 * travel through the wire format and are carried by the copy / move helpers, but
 * MUST NOT be repurposed by application code: future library revisions may bind
 * them to real fields.
 *
 * @par Example
 * @code
 * vlink::zerocopy::ProxyData envelope;
 * envelope.set_control_id(42);
 * envelope.set_timestamp(vlink::time_us());
 * envelope.create(payload, "dds://lidar/top", "proto.PointCloud",
 *                 static_cast<uint32_t>(SchemaType::kProtobuf), "host-a");
 *
 * vlink::Bytes wire;
 * envelope >> wire;
 *
 * vlink::zerocopy::ProxyData rx;
 * rx << wire;
 * auto url = rx.url();
 * @endcode
 */

#pragma once

#include <cstdint>
#include <string_view>

#include "../base/bytes.h"

namespace vlink {

namespace zerocopy {

/**
 * @struct ProxyData
 * @brief 80-byte POD envelope packing payload, URL, serialisation type and host string.
 *
 * @details
 * Used internally by the VLink proxy / monitoring layer.  All four variable
 * regions share one allocation laid out as @c [raw | url | ser | hostname]
 * so deserialisation is zero-copy and string accessors point straight into
 * the source wire buffer.
 */
struct VLINK_EXPORT_AND_ALIGNED(8) ProxyData final {
  /**
   * @brief Default-constructs an empty envelope and asserts the 80-byte contract.
   */
  ProxyData() noexcept;

  /**
   * @brief Frees the owned tail buffer when @c is_owner() is @c true.
   */
  ~ProxyData() noexcept;

  /**
   * @brief Deep-copies @p target into a freshly allocated envelope.
   *
   * @param target Source envelope to clone.
   */
  ProxyData(const ProxyData& target) noexcept;

  /**
   * @brief Steals @p target's allocation and metadata; @p target ends empty.
   *
   * @param target Source envelope moved from.
   */
  ProxyData(ProxyData&& target) noexcept;

  /**
   * @brief Deep-copy-assigns @p target; self-assignment is a no-op.
   *
   * @param target Source envelope to clone.
   * @return Reference to @c *this.
   */
  ProxyData& operator=(const ProxyData& target) noexcept;

  /**
   * @brief Move-assigns @p target; self-assignment is a no-op.
   *
   * @param target Source envelope moved from.
   * @return Reference to @c *this.
   */
  ProxyData& operator=(ProxyData&& target) noexcept;

  /**
   * @brief Deserialises an envelope from @p bytes with zero-copy borrowing semantics.
   *
   * @param bytes Wire buffer previously produced by @c operator>>.
   * @return @c true on success; @c false on magic mismatch or region inconsistency.
   */
  bool operator<<(const Bytes& bytes) noexcept;

  /**
   * @brief Serialises the struct snapshot plus tail buffer into @p bytes.
   *
   * @param bytes Output buffer; reallocated automatically when undersized.
   * @return @c true on success; @c false when output allocation fails.
   */
  bool operator>>(Bytes& bytes) const noexcept;

  /**
   * @brief Proxy control identifier set by @c set_control_id().
   *
   * @return Stored control id.
   */
  [[nodiscard]] uint32_t control_id() const noexcept;

  /**
   * @brief Proxy operation mode set by @c set_mode().
   *
   * @return Stored mode value.
   */
  [[nodiscard]] uint32_t mode() const noexcept;

  /**
   * @brief Microsecond-resolution timestamp set by @c set_timestamp().
   *
   * @return Timestamp.
   */
  [[nodiscard]] int64_t timestamp() const noexcept;

  /**
   * @brief Sequence number set by @c set_seq().
   *
   * @return Sequence number.
   */
  [[nodiscard]] int64_t seq() const noexcept;

  /**
   * @brief Coarse schema family tag set by @c set_schema().
   *
   * @return Numeric @c SchemaType value.
   */
  [[nodiscard]] uint32_t schema() const noexcept;

  /**
   * @brief Shallow @c Bytes view of the raw payload region (no copy).
   *
   * @return @c Bytes pointing into the internal tail buffer; lifetime tracks @c *this.
   */
  [[nodiscard]] Bytes raw() const noexcept;

  /**
   * @brief View of the URL region within the internal tail buffer.
   *
   * @return Non-owning view, or empty when no URL was provided.
   */
  [[nodiscard]] std::string_view url() const noexcept;

  /**
   * @brief View of the serialisation-type region within the internal tail buffer.
   *
   * @return Non-owning view, or empty when no serialisation label was provided.
   */
  [[nodiscard]] std::string_view ser() const noexcept;

  /**
   * @brief View of the optional source hostname region.
   *
   * @return Non-owning view, or empty when no hostname was provided.
   */
  [[nodiscard]] std::string_view hostname() const noexcept;

  /**
   * @brief Stores the proxy control identifier.
   *
   * @param control_id Identifier value.
   */
  void set_control_id(uint32_t control_id) noexcept;

  /**
   * @brief Stores the proxy operation mode.
   *
   * @param mode Mode value.
   */
  void set_mode(uint32_t mode) noexcept;

  /**
   * @brief Stores the microsecond-resolution timestamp.
   *
   * @param timestamp Timestamp value.
   */
  void set_timestamp(int64_t timestamp) noexcept;

  /**
   * @brief Stores the message sequence number.
   *
   * @param seq Sequence number.
   */
  void set_seq(int64_t seq) noexcept;

  /**
   * @brief Stores the coarse schema family tag.
   *
   * @param schema Numeric @c SchemaType value.
   */
  void set_schema(uint32_t schema) noexcept;

  /**
   * @brief Validates that @p bytes carries a well-formed @c ProxyData envelope.
   *
   * @param bytes Wire buffer to inspect.
   * @return @c true on magic match and minimum-size check.
   */
  [[nodiscard]] static bool check_valid(const Bytes& bytes) noexcept;

  /**
   * @brief Total bytes that @c operator>> writes for this envelope.
   *
   * @return @c sizeof(magic_begin) + @c sizeof(version) + @c sizeof(ProxyData) + @c size() + @c sizeof(magic_end).
   */
  [[nodiscard]] size_t get_serialized_size() const noexcept;

  /**
   * @brief Validates that internal region positions and sizes are consistent.
   *
   * @details
   * Confirms the four sub-regions are contiguous, sum to @c size(), and the
   * data pointer is non-null.  Useful after @c operator<< when forwarding
   * untrusted input.
   *
   * @return @c true when the layout is internally consistent.
   */
  [[nodiscard]] bool is_valid() const noexcept;

  /**
   * @brief Borrows @p target's tail buffer without copying.
   *
   * @param target Source envelope; its backing buffer must outlive @c *this.
   * @return @c false on self-borrow or owned-buffer aliasing, otherwise @c true.
   */
  bool shallow_copy(const ProxyData& target) noexcept;

  /**
   * @brief Allocates (or reuses) an owned buffer and clones @p target's payload.
   *
   * @param target Source envelope to clone.
   * @return @c false on self-copy, owned-buffer aliasing, or allocation failure.
   */
  bool deep_copy(const ProxyData& target) noexcept;

  /**
   * @brief Transfers ownership from @p target; @p target ends empty.
   *
   * @param target Source envelope moved from.
   * @return @c false on self-move, otherwise @c true.
   */
  bool move_copy(ProxyData& target) noexcept;

  /**
   * @brief Builds the envelope by packing all four regions into a single allocation.
   *
   * @details
   * Allocates @c raw.size() + @c url.size() + @c ser.size() + @c hostname.size()
   * bytes and copies each region in order.  If any region (or the total) would
   * exceed @c UINT32_MAX, or allocation fails, the envelope is cleared.  Callers passing dynamic
   * input must verify success via @c is_valid() or @c size().
   *
   * @param raw Raw payload bytes.
   * @param url Topic URL string.
   * @param ser Serialisation type label.
   * @param schema Optional coarse schema family tag.
   * @param hostname Optional source hostname.
   */
  void create(const Bytes& raw, std::string_view url, std::string_view ser, uint32_t schema = 0,
              std::string_view hostname = {}) noexcept;

  /**
   * @brief Frees the owned buffer (if any) and zeroes every field.
   */
  void clear() noexcept;

  /**
   * @brief Total size of the variable-length tail buffer.
   *
   * @return Byte count, or 0 when empty.
   */
  [[nodiscard]] size_t size() const noexcept;

  /**
   * @brief Whether this envelope currently owns its tail buffer.
   *
   * @return @c true when the destructor would free the buffer.
   */
  [[nodiscard]] bool is_owner() const noexcept;

  /**
   * @brief Mutable accessor for the first wire-locked reserved field (@c uint8_t).
   *
   * @return Reference to @c reserved_buf_.
   */
  [[nodiscard]] uint8_t& get_reserved() noexcept;

  /**
   * @brief Mutable accessor for the second wire-locked reserved field (@c uint16_t).
   *
   * @return Reference to @c reserved_buf2_.
   */
  [[nodiscard]] uint16_t& get_reserved2() noexcept;

  static constexpr bool kZerocopyTypes{true};  ///< Marker probed by the VLink type-trait machinery.

 private:
  uint8_t* data_{nullptr};
  size_t size_{0};
  uint32_t control_id_{0};
  uint32_t mode_{0};
  int64_t timestamp_{0};
  int64_t seq_{0};
  uint32_t data_pos_{0};
  uint32_t data_size_{0};
  uint32_t url_pos_{0};
  uint32_t url_size_{0};
  uint32_t ser_pos_{0};
  uint32_t ser_size_{0};
  uint32_t hostname_pos_{0};
  uint32_t hostname_size_{0};
  uint32_t schema_{0};
  bool is_owner_{false};
  uint8_t reserved_buf_{0};
  uint16_t reserved_buf2_{0};

  static constexpr uint32_t kMagicNumberBegin{0x98B7F12A};
  static constexpr uint32_t kMagicNumberEnd{0x98B7F12F};
};

}  // namespace zerocopy

}  // namespace vlink
