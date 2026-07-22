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
 * @file object_array.h
 * @brief Zero-copy variable-length array of fixed-size 3-D detection / tracking records.
 *
 * @details
 * @c ObjectArray is the canonical message produced by 3-D perception, sensor
 * fusion, and multi-object tracking pipelines.  It packs a homogeneous array
 * of @c Object records, each capturing the pose, dimensions, kinematics,
 * classification, identity, and covariance of one obstacle.  A 40-byte
 * @c Header prefix carries sequencing and dual-timestamp metadata.
 *
 * @par Container vs. record layout
 * The container struct is 112 bytes; each @c Object record is 144 bytes.
 * Both sizes are locked by @c static_assert.  @c Object uses 4-byte alignment
 * because its widest field is 4 bytes; with the wire envelope (magic 4 +
 * version 4 + struct 112) the payload starts at offset 120, an 8-byte boundary,
 * so the requirement is comfortably met and a typed @c objects() pointer stays
 * safe on strict-alignment architectures (ARM, AArch64).
 *
 * @par Object schema
 * | Field                     | Type                | Meaning                                       |
 * | ------------------------- | ------------------- | --------------------------------------------- |
 * | @c label                  | @c char[32]         | Class name (e.g. "car", "pedestrian")         |
 * | @c position[3]            | @c float            | Centre in metres (world frame)                |
 * | @c yaw                    | @c float            | Heading in radians (REP-103)                  |
 * | @c size[3]                | @c float            | Length, width, height in metres               |
 * | @c yaw_rate               | @c float            | Angular velocity in rad/s                     |
 * | @c velocity[3]            | @c float            | Linear velocity in m/s                        |
 * | @c score                  | @c float            | Detection confidence in [0, 1]                |
 * | @c acceleration[3]        | @c float            | Linear acceleration in m/s^2                  |
 * | @c existence_probability  | @c float            | Existence probability in [0, 1]               |
 * | @c position_covariance[6] | @c float            | Upper triangle of 3x3 covariance              |
 * | @c class_id               | @c uint32_t         | Numeric class identifier                      |
 * | @c track_id               | @c uint32_t         | Tracking id (0 = unassociated detection)      |
 * | @c age                    | @c uint32_t         | Tracking age in frames                        |
 * | @c num_observations       | @c uint32_t         | Cumulative observation count                  |
 * | @c motion_state           | @c MotionState      | Stationary / moving / stopped / parked        |
 * | @c source_type            | @c SourceType       | LiDAR / camera / radar / fusion / ultrasonic  |
 * | @c subtype_id             | @c uint16_t         | Fine-grained subtype                          |
 * | @c reserved_buf            | @c uint32_t         | Reserved (wire-locked)                        |
 *
 * @par Wire format
 * Both the container and the @c Object record are POD; @c memcpy is the
 * canonical serialiser.  The @c sizeof values are locked by @c static_assert
 * and form a permanent contract: @c vlink::zerocopy::* containers offer NO
 * forward and NO backward binary compatibility, and every field including
 * reserved bytes is part of the wire contract.
 * @code
 * static_assert(sizeof(ObjectArray) == 112, "Sizeof must be 112 bytes.");
 * static_assert(sizeof(ObjectArray::Object) == 144, "Sizeof must be 144 bytes.");
 * @endcode
 *
 * @par Memory layout
 * @code
 * Offset  Size  Field
 * ------  ----  --------------------------
 *      0    40  Header   header
 *     40     8  uint8_t* data_
 *     48     8  size_t   capacity_
 *     56     8  uint64_t update_time_ns_
 *     64    16  char     source_id_[16]
 *     80     4  uint32_t channel_
 *     84     4  uint32_t freq_
 *     88     4  uint32_t count_
 *     92     4  uint32_t pack_size_
 *     96     1  bool     is_owner_
 *     97     1  uint8_t  reserved_buf_
 *     98     2  uint16_t reserved_buf2_
 *    100     4  uint32_t reserved_buf3_
 *    104     4  uint32_t reserved_buf4_
 *    108     4  uint32_t reserved_buf5_
 * ------  ----  --------------------------
 *  Total   112  bytes (alignas 8)
 *
 * Wire envelope:
 * [ magic_begin (4) | version (4) | ObjectArray (112) | Object[0..count) (count*144) | magic_end (4) ]
 * @endcode
 *
 * @par Reserved bytes
 * @c reserved_buf_, @c reserved_buf2_, @c reserved_buf3_, @c reserved_buf4_, @c reserved_buf5_
 * are exposed through @c get_reserved* helpers and survive @c clear() and
 * the copy / move helpers so producers can stamp minor flags.  None of these
 * slots may be repurposed by application code: future library revisions may
 * bind them to real fields, silently breaking peers that abused the slot.
 *
 * @par Example
 * @code
 * vlink::zerocopy::ObjectArray arr;
 * arr.create(256);
 * arr.set_source_id("fusion_v2");
 *
 * vlink::zerocopy::ObjectArray::Object obj;
 * std::strncpy(obj.label, "car", sizeof(obj.label) - 1);
 * obj.position[0] = 12.0F;
 * obj.size[0]     = 4.5F;
 * obj.velocity[0] = 8.5F;
 * obj.class_id    = 1;
 * obj.track_id    = 42;
 * arr.push_value(obj);
 *
 * vlink::Bytes wire;
 * arr >> wire;
 * @endcode
 */

#pragma once

#include <cstdint>
#include <string_view>

#include "../base/bytes.h"
#include "./header.h"

namespace vlink {

namespace zerocopy {

/**
 * @struct ObjectArray
 * @brief 112-byte POD container holding a packed array of 144-byte @c Object records.
 *
 * @details
 * The container size is locked at 112 bytes on 64-bit targets via
 * @c static_assert.  Records share a single owned allocation so a freshly
 * deserialised array can return a typed @c objects() pointer that addresses
 * the wire buffer in place.
 */
struct VLINK_EXPORT_AND_ALIGNED(8) ObjectArray final {
  /**
   * @brief Motion / kinematic state classification for a tracked object.
   */
  enum MotionState : uint8_t {
    kMotionUnknown = 0,     ///< Unknown / uninitialised state.
    kMotionStationary = 1,  ///< Stationary obstacle (never expected to move).
    kMotionMoving = 2,      ///< Actively moving.
    kMotionStopped = 3,     ///< Temporarily stopped (e.g. at a red light).
    kMotionParked = 4,      ///< Long-term parked vehicle.
  };

  /**
   * @brief Origin sensor / pipeline that produced the detection.
   */
  enum SourceType : uint8_t {
    kSourceUnknown = 0,     ///< Unknown / uninitialised source.
    kSourceLidar = 1,       ///< LiDAR-only detection.
    kSourceCamera = 2,      ///< Camera-only detection.
    kSourceRadar = 3,       ///< Radar-only detection.
    kSourceFusion = 4,      ///< Multi-sensor fusion result.
    kSourceUltrasonic = 5,  ///< Ultrasonic-based detection.
  };

  /**
   * @struct Object
   * @brief 144-byte POD record describing one detection / track.
   *
   * @details
   * Public POD; assign fields directly and append with
   * @c ObjectArray::push_value.  The record uses 4-byte alignment because its
   * widest field is 4 bytes; the containing payload starts at an 8-byte-aligned
   * offset within the wire envelope, so typed access stays safe on
   * strict-alignment CPUs.
   */
  struct VLINK_EXPORT_AND_ALIGNED(4) Object final {
    /**
     * @brief Default-constructs a zeroed record and asserts the 144-byte contract.
     */
    Object() noexcept;

    char label[32]{0};                         ///< NUL-terminated class label (e.g. @c "car").
    float position[3]{0};                      ///< Centre position in metres (world frame).
    float yaw{0};                              ///< Heading in radians (REP-103).
    float size[3]{0};                          ///< Length, width, height bounding-box extents in metres.
    float yaw_rate{0};                         ///< Heading rate in radians per second.
    float velocity[3]{0};                      ///< Linear velocity in metres per second.
    float score{0};                            ///< Detection confidence in [0, 1].
    float acceleration[3]{0};                  ///< Linear acceleration in metres per second squared.
    float existence_probability{0};            ///< Existence probability in [0, 1].
    float position_covariance[6]{0};           ///< Upper-triangle of 3x3 position covariance (xx,xy,xz,yy,yz,zz).
    uint32_t class_id{0};                      ///< Numeric class identifier.
    uint32_t track_id{0};                      ///< Tracking identifier; 0 marks an unassociated detection.
    uint32_t age{0};                           ///< Tracking age in frames.
    uint32_t num_observations{0};              ///< Cumulative observation count for this track.
    MotionState motion_state{kMotionUnknown};  ///< Motion / kinematic classification.
    SourceType source_type{kSourceUnknown};    ///< Producing sensor or pipeline.
    uint16_t subtype_id{0};                    ///< Fine-grained subtype (e.g. sedan vs SUV).
    uint32_t reserved_buf{0};                  ///< Reserved (wire-locked); do not repurpose.
  };

  /**
   * @brief Default-constructs an empty array and asserts the 112-byte container contract.
   */
  ObjectArray() noexcept;

  /**
   * @brief Frees the owned record buffer when @c is_owner() is @c true.
   */
  ~ObjectArray() noexcept;

  /**
   * @brief Deep-copies @p target into a freshly allocated array.
   *
   * @param target Source array to clone.
   */
  ObjectArray(const ObjectArray& target) noexcept;

  /**
   * @brief Steals @p target's allocation and metadata; @p target ends empty.
   *
   * @param target Source array moved from.
   */
  ObjectArray(ObjectArray&& target) noexcept;

  /**
   * @brief Deep-copy-assigns @p target; self-assignment is a no-op.
   *
   * @param target Source array to clone.
   * @return Reference to @c *this.
   */
  ObjectArray& operator=(const ObjectArray& target) noexcept;

  /**
   * @brief Move-assigns @p target; self-assignment is a no-op.
   *
   * @param target Source array moved from.
   * @return Reference to @c *this.
   */
  ObjectArray& operator=(ObjectArray&& target) noexcept;

  /**
   * @brief Deserialises an array from @p bytes using zero-copy borrowing semantics.
   *
   * @param bytes Wire buffer previously produced by @c operator>>.
   * @return @c true on success; @c false on magic mismatch or size mismatch.
   */
  bool operator<<(const Bytes& bytes) noexcept;

  /**
   * @brief Serialises the struct snapshot plus record buffer into @p bytes.
   *
   * @param bytes Output buffer; resized automatically when too small.
   * @return @c true on success; @c false when output allocation fails.
   */
  bool operator>>(Bytes& bytes) const noexcept;

  /**
   * @brief Validates that @p bytes carries a well-formed @c ObjectArray envelope.
   *
   * @param bytes Wire buffer to inspect.
   * @return @c true when both magic sentinels match and the minimum size holds.
   */
  [[nodiscard]] static bool check_valid(const Bytes& bytes) noexcept;

  /**
   * @brief Total bytes that @c operator>> would write for this array.
   *
   * @return @c sizeof(magic_begin) + @c sizeof(version) + @c sizeof(ObjectArray) + @c count * @c pack_size + @c
   * sizeof(magic_end).
   */
  [[nodiscard]] size_t get_serialized_size() const noexcept;

  /**
   * @brief Whether the record buffer pointer is non-null and the array holds at least one record.
   *
   * @return @c true when the array holds usable records.
   */
  [[nodiscard]] bool is_valid() const noexcept;

  /**
   * @brief Borrows @p target's record buffer without copying.
   *
   * @param target Source array whose buffer must outlive @c *this.
   * @return @c false on self-borrow or owned-buffer aliasing, otherwise @c true.
   */
  bool shallow_copy(const ObjectArray& target) noexcept;

  /**
   * @brief Allocates (or reuses) an owned buffer and copies @p target's records.
   *
   * @param target Source array to clone.
   * @return @c false on self-copy, owned-buffer aliasing, or allocation failure.
   */
  bool deep_copy(const ObjectArray& target) noexcept;

  /**
   * @brief Transfers ownership from @p target; @p target ends empty.
   *
   * @param target Source array moved from.
   * @return @c false on self-move, otherwise @c true.
   */
  bool move_copy(ObjectArray& target) noexcept;

  /**
   * @brief Pre-allocates capacity for @p count records and resets the logical count.
   *
   * @param count Maximum number of records; must be non-zero.
   * @return @c false when @p count is zero, its byte size overflows, or allocation fails.
   */
  bool create(size_t count) noexcept;

  /**
   * @brief Releases the owned buffer (if any) and resets metadata except reserved fields.
   */
  void clear() noexcept;

  /**
   * @brief Appends a record at the end of the buffer.
   *
   * @param object Source record.
   * @return @c false on capacity overflow or invalid state.
   */
  bool push_value(const Object& object) noexcept;

  /**
   * @brief Overwrites the record at @p index.
   *
   * @param index Zero-based slot index; must be less than @c count().
   * @param object New record contents.
   * @return @c false on out-of-range or invalid state.
   */
  bool set_value(uint32_t index, const Object& object) noexcept;

  /**
   * @brief Reads the record at @p index into @p object (zeroed on out-of-range).
   *
   * @param index Zero-based slot index.
   * @param object Destination.
   * @return @c false on out-of-range.
   */
  bool get_value(uint32_t index, Object& object) const noexcept;

  /**
   * @brief Returns the record at @p index by value (zero-initialised on out-of-range).
   *
   * @param index Zero-based slot index.
   * @return Copy of the record, or a zero-initialised @c Object when out of range.
   */
  [[nodiscard]] Object get_value(uint32_t index) const noexcept;

  /**
   * @brief Resets the logical record count without reallocating.
   *
   * @param count New logical count; must not exceed allocated capacity.
   * @return @c false on capacity overflow.
   */
  bool resize(uint32_t count) noexcept;

  /**
   * @brief Producer-side timestamp recording when the array was assembled.
   *
   * @return Stored nanosecond timestamp.
   */
  [[nodiscard]] uint64_t update_time_ns() const noexcept;

  /**
   * @brief Producer / module identifier (e.g. @c "fusion_v2").
   *
   * @return Non-owning view into the embedded buffer.
   */
  [[nodiscard]] std::string_view source_id() const noexcept;

  /**
   * @brief Sensor / producer channel identifier.
   *
   * @return Stored channel id.
   */
  [[nodiscard]] uint32_t channel() const noexcept;

  /**
   * @brief Nominal publish frequency in Hz.
   *
   * @return Stored value.
   */
  [[nodiscard]] uint32_t freq() const noexcept;

  /**
   * @brief Logical record count currently stored.
   *
   * @return Number of populated records.
   */
  [[nodiscard]] uint32_t count() const noexcept;

  /**
   * @brief Byte size of one @c Object record (always @c sizeof(Object)).
   *
   * @return Record stride.
   */
  [[nodiscard]] uint32_t pack_size() const noexcept;

  /**
   * @brief Read-only pointer to the @c Object at @p index.
   *
   * @param index Zero-based slot index (default 0 returns the array base).
   * @return Typed pointer, or @c nullptr on out-of-range / invalid state.
   */
  [[nodiscard]] const Object* objects(uint32_t index = 0) const noexcept;

  /**
   * @brief Read-only pointer to the raw contiguous record buffer.
   *
   * @return Byte pointer to the first record.
   */
  [[nodiscard]] const uint8_t* data() const noexcept;

  /**
   * @brief Total byte capacity of the allocated record buffer.
   *
   * @return Capacity in bytes, or 0 when no buffer is allocated.
   */
  [[nodiscard]] size_t capacity() const noexcept;

  /**
   * @brief Whether this array owns its record buffer.
   *
   * @return @c true when the destructor would free the buffer.
   */
  [[nodiscard]] bool is_owner() const noexcept;

  /**
   * @brief Stores the producer-side timestamp.
   *
   * @param update_time_ns Timestamp in nanoseconds.
   */
  void set_update_time_ns(uint64_t update_time_ns) noexcept;

  /**
   * @brief Stores the producer / module identifier (truncated to @c sizeof(source_id) - 1 bytes).
   *
   * @param source_id Identifier string.
   */
  void set_source_id(std::string_view source_id) noexcept;

  /**
   * @brief Stores the sensor / producer channel identifier.
   *
   * @param channel Channel id.
   */
  void set_channel(uint32_t channel) noexcept;

  /**
   * @brief Stores the nominal publish frequency.
   *
   * @param freq Frequency in Hz.
   */
  void set_freq(uint32_t freq) noexcept;

  /**
   * @brief Mutable accessor for the first wire-locked reserved field (@c uint8_t).
   *
   * @return Reference to @c reserved_buf_.
   */
  uint8_t& get_reserved() noexcept { return reserved_buf_; }

  /**
   * @brief Mutable accessor for the second wire-locked reserved field (@c uint16_t).
   *
   * @return Reference to @c reserved_buf2_.
   */
  uint16_t& get_reserved2() noexcept { return reserved_buf2_; }

  /**
   * @brief Mutable accessor for the third wire-locked reserved field (@c uint32_t).
   *
   * @return Reference to @c reserved_buf3_.
   */
  uint32_t& get_reserved3() noexcept { return reserved_buf3_; }

  /**
   * @brief Mutable accessor for the fourth wire-locked reserved field (@c uint32_t).
   *
   * @return Reference to @c reserved_buf4_.
   */
  uint32_t& get_reserved4() noexcept { return reserved_buf4_; }

  /**
   * @brief Mutable accessor for the fifth wire-locked reserved field (@c uint32_t).
   *
   * @return Reference to @c reserved_buf5_.
   */
  uint32_t& get_reserved5() noexcept { return reserved_buf5_; }

  Header header;  ///< Sequencing and timestamp metadata prefix.

  static constexpr bool kZerocopyTypes{true};  ///< Marker probed by the VLink type-trait machinery.

 private:
  uint8_t* data_{nullptr};
  size_t capacity_{0};
  uint64_t update_time_ns_{0};
  char source_id_[16]{0};
  uint32_t channel_{0};
  uint32_t freq_{0};
  uint32_t count_{0};
  uint32_t pack_size_{0};
  bool is_owner_{false};
  uint8_t reserved_buf_{0};
  uint16_t reserved_buf2_{0};
  uint32_t reserved_buf3_{0};
  uint32_t reserved_buf4_{0};
  uint32_t reserved_buf5_{0};

  static constexpr uint32_t kMagicNumberBegin{0x98B7F18A};
  static constexpr uint32_t kMagicNumberEnd{0x98B7F18F};
};

}  // namespace zerocopy

}  // namespace vlink
