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
 * @file point_cloud.h
 * @brief Zero-copy, schema-aware 3-D point cloud container with per-field type protocol.
 *
 * @details
 * @c PointCloud carries a contiguous array of fixed-size point records together
 * with a compact, self-describing schema (the embedded @c Protocol).  Schemas
 * are encoded as two @c uint64_t nibble-packed integers plus a comma-separated
 * 152-byte name string, so a receiver can introspect every field without
 * sharing an external IDL.  The 40-byte @c Header prefix carries sequencing
 * and dual-timestamp metadata.
 *
 * @par Point format diagram
 * @code
 * Per-point layout (tightly packed, no padding):
 *
 *     XYZ float        :  [ x: float32 | y: float32 | z: float32 ]                 (12 B)
 *     XYZI float       :  [ x | y | z | intensity: float32 ]                       (16 B)
 *     XYZRGB           :  [ x | y | z | r: u8 | g: u8 | b: u8 | pad: u8 ]          (16 B)
 *     XYZ + features   :  [ x | y | z | t: u32 | label: u8 | conf: f32 | ... ]     (variable)
 *     XYZ double       :  [ x: float64 | y: float64 | z: float64 ]                 (24 B)
 *
 * Schema is encoded into two uint64_t integers, one nibble per field
 * (high nibble = first field):
 *
 *     size_num: 0x0408...  (each nibble is the field byte-size)
 *     type_num: 0x0A0B...  (each nibble is a PointCloud::Type enum value)
 *     names:    "x,y,z,intensity,label"  (stored in 152 bytes)
 * @endcode
 *
 * @par Supported field types
 * | Enum               | C++ type    | Bytes |
 * | ------------------ | ----------- | ----- |
 * | @c kBoolType       | @c bool     | 1     |
 * | @c kInt8Type       | @c int8_t   | 1     |
 * | @c kUint8Type      | @c uint8_t  | 1     |
 * | @c kInt16Type      | @c int16_t  | 2     |
 * | @c kUint16Type     | @c uint16_t | 2     |
 * | @c kInt32Type      | @c int32_t  | 4     |
 * | @c kUint32Type     | @c uint32_t | 4     |
 * | @c kInt64Type      | @c int64_t  | 8     |
 * | @c kUint64Type     | @c uint64_t | 8     |
 * | @c kFloatType      | @c float    | 4     |
 * | @c kDoubleType     | @c double   | 8     |
 *
 * @par Protocol nested struct
 * | Member          | Type           | Purpose                                        |
 * | --------------- | -------------- | ---------------------------------------------- |
 * | @c size_num     | @c uint64_t    | Per-field byte sizes packed in nibbles         |
 * | @c type_num     | @c uint64_t    | Per-field @c Type tags packed in nibbles       |
 * | @c names[152]   | @c char[]      | Comma-separated field names (3..16 fields)     |
 *
 * @par Wire format
 * @c PointCloud is POD; @c memcpy of the struct snapshot plus the packed point
 * buffer forms the wire payload.  The @c sizeof value is locked by
 * @c static_assert and forms a permanent contract: @c vlink::zerocopy::*
 * containers offer NO forward and NO backward binary compatibility -- every
 * field, including reserved bytes and the @c Protocol descriptor, is part of
 * the wire contract.
 * @code
 * static_assert(sizeof(PointCloud) == 256, "Sizeof must be 256 bytes.");
 * @endcode
 *
 * @par Memory layout
 * @code
 * Offset  Size  Field
 * ------  ----  ----------------------------------------------------------------
 *      0    40  Header   header
 *     40   168  Protocol protocol_  (size_num + type_num + names[152])
 *    208     8  uint8_t* data_
 *    216     8  size_t   capacity_
 *    224     8  size_t   size_
 *    232     4  uint32_t reserved_buf_
 *    236     4  uint32_t reserved_buf2_
 *    240     2  uint16_t pack_size_
 *    242     2  uint16_t extent_
 *    244     1  uint8_t  downsample_
 *    245     1  uint8_t  reserved_buf3_
 *    246     1  bool     vertical_
 *    247     1  bool     is_owner_
 *    248     8  uint64_t index_
 * ------  ----  ----------------------------------------------------------------
 *  Total   256  bytes (alignas 8)
 *
 * Wire envelope:
 * [ magic_begin (4) | version (4) | PointCloud (256) | point data (size_ * pack_size_) | magic_end (4) ]
 * @endcode
 *
 * @par Reserved bytes
 * @c reserved_buf_ (offset 232), @c reserved_buf2_ (offset 236) and @c reserved_buf3_
 * (offset 245) are wire-locked padding bytes.  They travel through the wire format
 * unchanged but MUST NOT be repurposed by application code because future library
 * revisions may bind them to real fields.  The @c uint8_t @c downsample_ at
 * offset 244 records the voxel-grid downsampling level applied to the payload (see
 * @c downsample()): @c 0 disables downsampling, @c 255 is the most aggressive.
 *
 * @par Optional storage compression
 * The @c extent and @c vertical create options trade fidelity for footprint along
 * two independent axes.  When @c extent_ > 0 the three leading XYZ
 * coordinates are linearly quantised with @c Quantize::encode<int16_t>(extent, value)
 * and restored with @c Quantize::decode<float/double>(extent, stored), halving
 * (or better) their in-memory footprint; the
 * embedded schema is rewritten so the XYZ fields advertise @c kInt16Type and
 * @c get_value_v3f() / @c get_value_v3d() transparently dequantise back to floating
 * point.  The generic @c get_value<int16_t>() returns the raw @c int16_t storage,
 * whereas @c get_value<float>() / @c get_value<double>() also dequantise (matching
 * @c v3f / @c v3d).  Points whose XYZ fall outside @c (-extent, +extent) cannot be represented and are
 * discarded on insert rather than saturated.  When @c vertical_ is @c true the serialised wire
 * payload is emitted in structure-of-arrays order (@c xx..x @c yy..y @c zz..z, one field column after
 * another) which packs better under a downstream entropy coder; the in-memory
 * buffer stays interleaved (row-major) and @c operator<< restores that layout, so
 * the field accessors are unaffected.
 *
 * @par Optional voxel-grid downsampling
 * @c downsample() collapses spatially-close points by mapping each point's quantised
 * XYZ onto a voxel grid and keeping the first point seen in each voxel, compacting the
 * survivors to the front of the buffer and shrinking @c size().  It applies only to
 * quantised clouds (@c extent_ > 0) and records the requested level into the
 * @c downsample_ marker, which @c get_downsample() reports.  The marker is
 * producer-set, not an automatically maintained invariant: freshly created clouds
 * record @c 0, and @c clear() resets it to @c 0; it is NOT recomputed when points are
 * subsequently added or modified.
 *
 * @par Example -- typical fill and publish
 * @code
 * vlink::zerocopy::PointCloud pc;
 * pc.create_v3f<float>(1000, {"intensity"});   // schema: x, y, z, intensity (all f32)
 *
 * pc.push_value_v3f(1.0F, 2.0F, 3.0F, 0.8F);
 * pc.push_value_v3f(4.0F, 5.0F, 6.0F, 0.5F);
 *
 * auto key_map = pc.get_key_map();
 * float intensity = pc.get_value<float>(0, key_map, "intensity");
 *
 * vlink::Bytes wire;
 * pc >> wire;
 *
 * vlink::zerocopy::PointCloud rx;
 * rx << wire;
 * @endcode
 */

#pragma once

#include <cstdint>
#include <iostream>
#include <limits>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <vector>

#include "../base/bytes.h"
#include "../base/quantize.h"
#include "./header.h"

namespace vlink {

namespace zerocopy {

/**
 * @struct PointCloud
 * @brief 256-byte POD container holding a schema-described array of N-field point records.
 *
 * @details
 * The struct size is locked at 256 bytes on 64-bit targets via
 * @c static_assert.  Points are stored in tightly packed row-major order; one
 * row equals @c pack_size() bytes.  Schemas are inspected by calling
 * @c get_key_map() / @c get_key_list() and field values read through the
 * @c get_value family of templates.
 */
struct VLINK_EXPORT_AND_ALIGNED(8) PointCloud final {
  /**
   * @brief Fundamental field type tag used by the embedded @c Protocol.
   */
  enum Type : uint8_t {
    kUnknownType = 0,  ///< Uninitialised / unsupported type.
    kBoolType = 1,     ///< @c bool (1 byte).
    kInt8Type = 2,     ///< @c int8_t (1 byte).
    kUint8Type = 3,    ///< @c uint8_t (1 byte).
    kInt16Type = 4,    ///< @c int16_t (2 bytes).
    kUint16Type = 5,   ///< @c uint16_t (2 bytes).
    kInt32Type = 6,    ///< @c int32_t (4 bytes).
    kUint32Type = 7,   ///< @c uint32_t (4 bytes).
    kInt64Type = 8,    ///< @c int64_t (8 bytes).
    kUint64Type = 9,   ///< @c uint64_t (8 bytes).
    kFloatType = 10,   ///< @c float (4 bytes).
    kDoubleType = 11,  ///< @c double (8 bytes).
  };

  /**
   * @struct Key
   * @brief Descriptor for one named field within a packed point record.
   */
  struct Key final {
    std::string name;            ///< Field name (e.g. @c "x", @c "intensity").
    uint8_t type{kUnknownType};  ///< @c Type tag for the field.
    uint8_t size{0};             ///< Field byte size derived from @c type.
  };

  /**
   * @brief Maps a field name to its byte offset inside one packed point record.
   */
  using KeyMap = std::unordered_map<std::string, uint16_t>;

  /**
   * @brief Ordered list of field descriptors for one packed point record.
   */
  using KeyList = std::vector<Key>;

  /**
   * @struct Vector3f
   * @brief Compact single-precision XYZ vector (12 bytes, 4-byte aligned).
   */
  struct VLINK_EXPORT_AND_ALIGNED(4) Vector3f final {
    float x{0};  ///< X component.
    float y{0};  ///< Y component.
    float z{0};  ///< Z component.

    /**
     * @brief Default-constructs a zeroed vector and asserts the 12-byte contract.
     */
    Vector3f() noexcept;

    /**
     * @brief Value constructor.
     *
     * @param _x X component.
     * @param _y Y component.
     * @param _z Z component.
     */
    explicit Vector3f(float _x, float _y, float _z) noexcept;

    /**
     * @brief Formats the vector as @c "(x, y, z)" on @p ostream.
     *
     * @param ostream Output stream.
     * @param v3f Vector to format.
     * @return Reference to @p ostream.
     */
    friend std::ostream& operator<<(std::ostream& ostream, const Vector3f& v3f) noexcept;
  };

  /**
   * @struct Vector3d
   * @brief Compact double-precision XYZ vector (24 bytes, 8-byte aligned).
   */
  struct VLINK_EXPORT_AND_ALIGNED(8) Vector3d final {
    double x{0};  ///< X component.
    double y{0};  ///< Y component.
    double z{0};  ///< Z component.

    /**
     * @brief Default-constructs a zeroed vector and asserts the 24-byte contract.
     */
    Vector3d() noexcept;

    /**
     * @brief Value constructor.
     *
     * @param _x X component.
     * @param _y Y component.
     * @param _z Z component.
     */
    explicit Vector3d(double _x, double _y, double _z) noexcept;

    /**
     * @brief Formats the vector as @c "(x, y, z)" on @p ostream.
     *
     * @param ostream Output stream.
     * @param v3d Vector to format.
     * @return Reference to @p ostream.
     */
    friend std::ostream& operator<<(std::ostream& ostream, const Vector3d& v3d) noexcept;
  };

  /**
   * @brief Default-constructs an empty cloud and asserts the 256-byte contract.
   */
  PointCloud() noexcept;

  /**
   * @brief Frees the owned point buffer when @c is_owner() is @c true.
   */
  ~PointCloud() noexcept;

  /**
   * @brief Deep-copies @p target into a freshly allocated cloud.
   *
   * @param target Source cloud to clone.
   */
  PointCloud(const PointCloud& target) noexcept;

  /**
   * @brief Steals @p target's allocation and metadata; @p target ends empty.
   *
   * @param target Source cloud moved from.
   */
  PointCloud(PointCloud&& target) noexcept;

  /**
   * @brief Deep-copy-assigns @p target; self-assignment is a no-op.
   *
   * @param target Source cloud to clone.
   * @return Reference to @c *this.
   */
  PointCloud& operator=(const PointCloud& target) noexcept;

  /**
   * @brief Move-assigns @p target; self-assignment is a no-op.
   *
   * @param target Source cloud moved from.
   * @return Reference to @c *this.
   */
  PointCloud& operator=(PointCloud&& target) noexcept;

  /**
   * @brief Deserialises a @c PointCloud from @p bytes with zero-copy borrowing semantics.
   *
   * @param bytes Wire buffer produced by @c operator>>.
   * @return @c true on success; @c false on magic mismatch or size mismatch.
   */
  bool operator<<(const Bytes& bytes) noexcept;

  /**
   * @brief Serialises the struct snapshot plus point bytes into @p bytes.
   *
   * @param bytes Output buffer; resized automatically when its size differs from the serialized size.
   * @return @c true on success; @c false when output allocation fails.
   */
  bool operator>>(Bytes& bytes) const noexcept;

  /**
   * @brief Validates that @p bytes carries a well-formed @c PointCloud envelope.
   *
   * @param bytes Wire buffer to inspect.
   * @return @c true when both magic sentinels match and the minimum size holds.
   */
  [[nodiscard]] static bool check_valid(const Bytes& bytes) noexcept;

  /**
   * @brief Whether the data pointer is non-null and both @c size() and @c pack_size() are positive.
   *
   * @return @c true when the cloud holds usable points.
   */
  [[nodiscard]] bool is_valid() const noexcept;

  /**
   * @brief Borrows @p target's point buffer and protocol without copying.
   *
   * @param target Source cloud whose buffer must outlive @c *this.
   * @return @c false on self-borrow or owned-buffer aliasing, otherwise @c true.
   */
  bool shallow_copy(const PointCloud& target) noexcept;

  /**
   * @brief Allocates (or reuses) an owned buffer and copies @p target's protocol and point bytes.
   *
   * @param target Source cloud to clone.
   * @return @c false on self-copy, owned-buffer aliasing, size overflow, or allocation failure.
   */
  bool deep_copy(const PointCloud& target) noexcept;

  /**
   * @brief Transfers ownership from @p target; @p target ends empty.
   *
   * @param target Source cloud moved from.
   * @return @c false on self-move, otherwise @c true.
   */
  bool move_copy(PointCloud& target) noexcept;

  /**
   * @brief Total bytes that @c operator>> would write for this cloud.
   *
   * @return @c sizeof(magic_begin) + @c sizeof(version) + @c sizeof(PointCloud) + @c size() * @c pack_size() + @c
   * sizeof(magic_end).
   */
  [[nodiscard]] size_t get_serialized_size() const noexcept;

  /**
   * @brief Builds a @c KeyMap (and optional @c KeyList) from the embedded protocol descriptor.
   *
   * @param key_list Optional output; filled with ordered field descriptors when non-null.
   * @return Map from field name to byte offset within one packed point.
   */
  [[nodiscard]] KeyMap get_key_map(KeyList* key_list = nullptr) const noexcept;

  /**
   * @brief Number of points currently stored.
   *
   * @return Point count, or 0 when empty.
   */
  [[nodiscard]] size_t size() const noexcept;

  /**
   * @brief Byte size of one packed point record (sum of field byte sizes).
   *
   * @return Stride in bytes, or 0 when no schema has been set.
   */
  [[nodiscard]] size_t pack_size() const noexcept;

  /**
   * @brief Whether this cloud owns its point buffer.
   *
   * @return @c true when the destructor would free the buffer.
   */
  [[nodiscard]] bool is_owner() const noexcept;

  /**
   * @brief Quantisation extent currently configured for XYZ (maximum absolute coordinate).
   *
   * @return Extent value; @c 0 when quantisation is disabled.
   */
  [[nodiscard]] uint16_t get_extent() const noexcept;

  /**
   * @brief Whether serialisation emits the payload in vertical (structure-of-arrays) column order.
   *
   * @return @c true when vertical layout is enabled.
   */
  [[nodiscard]] bool get_vertical() const noexcept;

  /**
   * @brief Sets whether serialisation emits the payload in vertical (structure-of-arrays) column order.
   *
   * @details
   * This toggles only the wire payload layout used by @c operator>>.  The owned or borrowed
   * in-memory point buffer remains in row-major order, and the schema, extent, downsample marker,
   * logical size, and point data are left unchanged.
   *
   * @param vertical @c true to serialise field columns contiguously; @c false for row-major payloads.
   */
  void set_vertical(bool vertical) noexcept;

  /**
   * @brief Voxel-grid downsampling level recorded for this cloud (producer-set marker; see @c downsample()).
   *
   * @return Level value in @c [0, 255]; @c 0 when downsampling is disabled.
   */
  [[nodiscard]] uint8_t get_downsample() const noexcept;

  /**
   * @brief Voxel-grid downsamples a quantised owned cloud in place.
   *
   * @details
   * Keeps the first point per occupied voxel, compacts survivors to the front, and shrinks @c size().
   * @p level controls voxel size in @c [0,255]; @c 0 is a no-op that clears the marker.  Successful
   * positive calls record @p level in @c downsample_, while rejected positive calls leave the marker
   * unchanged.  Only clouds created with @c extent > 0 and owned storage can be downsampled.
   *
   * @param level Downsampling aggressiveness in @c [0,255]; @c 0 disables downsampling.
   * @return @c true on success or level-@c 0 no-op; @c false when downsampling is not applicable.
   */
  bool downsample(uint8_t level) noexcept;

  /**
   * @brief Raw @c Protocol::size_num integer (nibbles = per-field byte sizes).
   *
   * @return Stored value.
   */
  [[nodiscard]] uint64_t get_protocol_size_num() const noexcept;

  /**
   * @brief Raw @c Protocol::type_num integer (nibbles = per-field @c Type tags).
   *
   * @return Stored value.
   */
  [[nodiscard]] uint64_t get_protocol_type_num() const noexcept;

  /**
   * @brief Comma-separated field sizes for diagnostic output (e.g. @c "4,4,4,1").
   *
   * @return Printable size string.
   */
  [[nodiscard]] std::string get_protocol_size_str() const noexcept;

  /**
   * @brief Raw comma-separated field name string (e.g. @c "x,y,z,intensity").
   *
   * @return Copy of the embedded name string.  Keep the encoded string below
   *         152 bytes if string-returning helpers must round-trip.
   */
  [[nodiscard]] std::string get_protocol_name_str() const noexcept;

  /**
   * @brief Comma-separated field types for diagnostic output (e.g. @c "float,float,float,uint8").
   *
   * @return Printable type string.
   */
  [[nodiscard]] std::string get_protocol_type_str() const noexcept;

  /**
   * @brief Read-only pointer to the packed point buffer (one row per point).
   *
   * @return Pointer to the first point.
   */
  [[nodiscard]] const uint8_t* get_internal_data() const noexcept;

  /**
   * @brief Pre-allocated point capacity (@c capacity_ / @c pack_size_).
   *
   * @return Reserved point count; 0 for borrowed / deserialised buffers.
   */
  [[nodiscard]] size_t get_reserved_size() const noexcept;

  /**
   * @brief Reads the first three @c float fields of point @p loop_index as XYZ.
   *
   * @param x Output X value.
   * @param y Output Y value.
   * @param z Output Z value.
   * @param loop_index Zero-based point index.
   * @return @c false on out-of-range.
   */
  bool get_value_v3f(float& x, float& y, float& z, size_t loop_index) const noexcept;

  /**
   * @brief Reads the first three @c float fields of point @p loop_index into a @c Vector3f.
   *
   * @param v3f Output vector.
   * @param loop_index Zero-based point index.
   * @return @c false on out-of-range.
   */
  bool get_value_v3f(Vector3f& v3f, size_t loop_index) const noexcept;

  /**
   * @brief Returns the first three @c float fields of point @p loop_index as a @c Vector3f.
   *
   * @param loop_index Zero-based point index.
   * @return Vector; zero-initialised on out-of-range.
   */
  [[nodiscard]] Vector3f get_value_v3f(size_t loop_index) const noexcept;

  /**
   * @brief Reads the first three @c double fields of point @p loop_index as XYZ.
   *
   * @param x Output X value.
   * @param y Output Y value.
   * @param z Output Z value.
   * @param loop_index Zero-based point index.
   * @return @c false on out-of-range.
   */
  bool get_value_v3d(double& x, double& y, double& z, size_t loop_index) const noexcept;

  /**
   * @brief Reads the first three @c double fields of point @p loop_index into a @c Vector3d.
   *
   * @param v3d Output vector.
   * @param loop_index Zero-based point index.
   * @return @c false on out-of-range.
   */
  bool get_value_v3d(Vector3d& v3d, size_t loop_index) const noexcept;

  /**
   * @brief Returns the first three @c double fields of point @p loop_index as a @c Vector3d.
   *
   * @param loop_index Zero-based point index.
   * @return Vector; zero-initialised on out-of-range.
   */
  [[nodiscard]] Vector3d get_value_v3d(size_t loop_index) const noexcept;

  /**
   * @brief Reads a field at byte @p offset within point @p loop_index into @p t.
   *
   * @tparam T Field type; must be fundamental.
   * @param t Output value.
   * @param loop_index Zero-based point index.
   * @param offset Byte offset within one packed record.
   * @return @c false (and zeroes @p t) on out-of-bounds access.
   */
  template <typename T>
  bool get_value(T& t, size_t loop_index, uint16_t offset) const noexcept;

  /**
   * @brief Returns a field at byte @p offset within point @p loop_index by value.
   *
   * @tparam T Field type; must be fundamental.
   * @param loop_index Zero-based point index.
   * @param offset Byte offset within one packed record.
   * @return Field value; zero-initialised on out-of-bounds access.
   */
  template <typename T>
  [[nodiscard]] T get_value(size_t loop_index, uint16_t offset) const noexcept;

  /**
   * @brief Reads a named field from point @p loop_index using a cached @c KeyMap.
   *
   * @tparam T Field type; must be fundamental.
   * @param t Output value.
   * @param loop_index Zero-based point index.
   * @param key_map Map obtained from @c get_key_map().
   * @param key Full field name to look up.
   * @return @c false (and zeroes @p t) on unknown key or out-of-bounds access.
   */
  template <typename T>
  bool get_value(T& t, size_t loop_index, KeyMap& key_map, std::string_view key) const noexcept;

  /**
   * @brief Returns a named field from point @p loop_index using a cached @c KeyMap.
   *
   * @tparam T Field type; must be fundamental.
   * @param loop_index Zero-based point index.
   * @param key_map Map obtained from @c get_key_map().
   * @param key Full field name.
   * @return Field value; zero-initialised on unknown key or out-of-bounds access.
   */
  template <typename T>
  [[nodiscard]] T get_value(size_t loop_index, KeyMap& key_map, std::string_view key) const noexcept;

  /**
   * @brief Returns a field value cast to @c double, dispatched on the runtime @c Type tag.
   *
   * @param loop_index Zero-based point index.
   * @param offset Byte offset within one packed record.
   * @param type @c Type tag for this field.
   * @return Field value as @c double, or 0 on unknown type.
   */
  [[nodiscard]] double get_value_for_double_float(size_t loop_index, uint16_t offset, uint8_t type) const noexcept;

  /**
   * @brief Returns a named field value cast to @c double using a cached @c KeyMap.
   *
   * @param loop_index Zero-based point index.
   * @param key_map Map obtained from @c get_key_map().
   * @param key Full field name.
   * @param type @c Type tag for this field.
   * @return Field value as @c double, or 0 on unknown key / type.
   */
  [[nodiscard]] double get_value_for_double_float(size_t loop_index, KeyMap& key_map, std::string_view key,
                                                  uint8_t type) const noexcept;

  /**
   * @brief Returns a field value formatted as a printable string.
   *
   * @param loop_index Zero-based point index.
   * @param offset Byte offset within one packed record.
   * @param type @c Type tag.
   * @return Stringified value, or empty on unknown type.
   */
  [[nodiscard]] std::string get_value_for_print(size_t loop_index, uint16_t offset, uint8_t type) const noexcept;

  /**
   * @brief Returns a named field value formatted as a printable string.
   *
   * @param loop_index Zero-based point index.
   * @param key_map Map obtained from @c get_key_map().
   * @param key Full field name.
   * @param type @c Type tag.
   * @return Stringified value; empty for unknown @p type.
   */
  [[nodiscard]] std::string get_value_for_print(size_t loop_index, KeyMap& key_map, std::string_view key,
                                                uint8_t type) const noexcept;

  /**
   * @brief Creates a cloud from pre-built protocol integers and a name string.
   *
   * @details
   * Low-level entry point used when @p size_num and @p type_num are already
   * known (e.g. reconstructed from a wire buffer).  Prefer the type-safe
   * @c create<T...>() template for new producers.
   *
   * @param size Maximum number of points to pre-allocate.
   * @param size_num Protocol size encoding.
   * @param type_num Protocol type encoding.
   * @param key_str Comma-separated field names (3..16 fields, up to 152 bytes).
   * @param extent Optional XYZ quantisation extent; @c 0 disables quantisation.
   * @param vertical When @c true the serialised payload uses structure-of-arrays column order.
   * @return @c false on protocol validation failure, size overflow, or allocation failure (including XYZ that is not
   * @c float / @c double / @c int16_t when @p extent is non-zero).
   */
  bool create(size_t size, uint64_t size_num, uint64_t type_num, std::string_view key_str, uint16_t extent = 0,
              bool vertical = false) noexcept;

  /**
   * @brief Creates a cloud with a type-safe variadic field schema.
   *
   * @tparam T Field types; must all be fundamental.  3..16 types required.
   * @param _size Maximum number of points.
   * @param keys Field names in the same order as @c T... .
   * @param extent Optional XYZ quantisation extent; @c 0 disables quantisation.
   * @param vertical When @c true the serialised payload uses structure-of-arrays column order.
   * @return @c false when key count is wrong, schema packing fails, size overflows, or allocation fails.
   */
  template <typename... T>
  bool create(size_t _size, const std::vector<std::string>& keys = {}, uint16_t extent = 0,
              bool vertical = false) noexcept;

  /**
   * @brief Creates a cloud with @c float XYZ followed by additional variadic fields.
   *
   * @tparam T Types for additional fields beyond XYZ.
   * @param _size Maximum number of points.
   * @param keys Names for the additional fields (excluding x, y, z).
   * @param extent Optional XYZ quantisation extent; @c 0 disables quantisation.
   * @param vertical When @c true the serialised payload uses structure-of-arrays column order.
   * @return @c false on schema packing failure, size overflow, or allocation failure.
   */
  template <typename... T>
  bool create_v3f(size_t _size, const std::vector<std::string>& keys = {}, uint16_t extent = 0,
                  bool vertical = false) noexcept;

  /**
   * @brief Creates a cloud with @c double XYZ followed by additional variadic fields.
   *
   * @tparam T Types for additional fields beyond XYZ.
   * @param _size Maximum number of points.
   * @param keys Names for the additional fields (excluding x, y, z).
   * @param extent Optional XYZ quantisation extent; @c 0 disables quantisation.
   * @param vertical When @c true the serialised payload uses structure-of-arrays column order.
   * @return @c false on schema packing failure, size overflow, or allocation failure.
   */
  template <typename... T>
  bool create_v3d(size_t _size, const std::vector<std::string>& keys = {}, uint16_t extent = 0,
                  bool vertical = false) noexcept;

  /**
   * @brief Bulk-fills the cloud from a pre-packed external buffer.
   *
   * @param src_data Source buffer in packed row order; must be non-null.
   * @param _size Number of points to copy; must be non-zero.
   * @return @c false on invalid arguments or capacity overflow.
   */
  bool fill_packed_data(const uint8_t* src_data, size_t _size) noexcept;

  /**
   * @brief Appends one point with the given field values (must match schema).
   *
   * @tparam T Field value types; must all be fundamental.  3..16 required.
   * @param args Field values in schema order.
   * @return @c false on capacity exhaustion or pack-size mismatch.
   */
  template <typename... T>
  bool push_value(T... args) noexcept;

  /**
   * @brief Appends one point with @c float XYZ followed by additional fields.
   *
   * @tparam T Types for additional fields beyond XYZ.
   * @param x X coordinate.
   * @param y Y coordinate.
   * @param z Z coordinate.
   * @param args Additional field values.
   * @return @c false on capacity exhaustion, pack-size mismatch, or any XYZ coordinate outside
   *         @c (-extent, +extent) on a quantised cloud (the out-of-range point is discarded).
   */
  template <typename... T>
  bool push_value_v3f(float x, float y, float z, T... args) noexcept;

  /**
   * @brief Appends one point from a @c Vector3f plus additional fields.
   *
   * @tparam T Types for additional fields beyond XYZ.
   * @param v3f XYZ coordinates.
   * @param args Additional field values.
   * @return @c false on capacity exhaustion, pack-size mismatch, or any XYZ coordinate outside
   *         @c (-extent, +extent) on a quantised cloud (the out-of-range point is discarded).
   */
  template <typename... T>
  bool push_value_v3f(Vector3f v3f, T... args) noexcept;

  /**
   * @brief Appends one point with @c double XYZ followed by additional fields.
   *
   * @tparam T Types for additional fields beyond XYZ.
   * @param x X coordinate.
   * @param y Y coordinate.
   * @param z Z coordinate.
   * @param args Additional field values.
   * @return @c false on capacity exhaustion, pack-size mismatch, or any XYZ coordinate outside
   *         @c (-extent, +extent) on a quantised cloud (the out-of-range point is discarded).
   */
  template <typename... T>
  bool push_value_v3d(double x, double y, double z, T... args) noexcept;

  /**
   * @brief Appends one point from a @c Vector3d plus additional fields.
   *
   * @tparam T Types for additional fields beyond XYZ.
   * @param v3d XYZ coordinates.
   * @param args Additional field values.
   * @return @c false on capacity exhaustion, pack-size mismatch, or any XYZ coordinate outside
   *         @c (-extent, +extent) on a quantised cloud (the out-of-range point is discarded).
   */
  template <typename... T>
  bool push_value_v3d(Vector3d v3d, T... args) noexcept;

  /**
   * @brief Sets the logical point count without reallocating.
   *
   * @details
   * Updates the internal write cursor to @c size * @c pack_size_.  Required
   * before random overwrites via @c set_value().
   *
   * @param size New logical point count; must not exceed allocated capacity.
   * @return @c false on capacity overflow or invalid state.
   */
  bool resize(size_t size) noexcept;

  /**
   * @brief Overwrites the record at @p loop_index with new field values.
   *
   * @details
   * Requires the logical size and internal write cursor to be consistent;
   * call @c resize() (or finish a full @c push_value() sequence) first.
   *
   * @tparam T Field value types; must all be fundamental.  3..16 required.
   * @param loop_index Zero-based point index to overwrite.
   * @param args Field values in schema order.
   * @return @c false on out-of-range, cursor inconsistency, or pack-size mismatch.
   */
  template <typename... T>
  bool set_value(size_t loop_index, T... args) noexcept;

  /**
   * @brief Overwrites point @p loop_index with @c float XYZ plus additional fields.
   *
   * @tparam T Types for additional fields beyond XYZ.
   * @param loop_index Zero-based point index.
   * @param x X coordinate.
   * @param y Y coordinate.
   * @param z Z coordinate.
   * @param args Additional field values.
   * @return @c false on failure, or when any XYZ coordinate is outside @c (-extent, +extent) on a
   *         quantised cloud (the out-of-range point is discarded).
   */
  template <typename... T>
  bool set_value_v3f(size_t loop_index, float x, float y, float z, T... args) noexcept;

  /**
   * @brief Overwrites point @p loop_index from a @c Vector3f plus additional fields.
   *
   * @tparam T Types for additional fields beyond XYZ.
   * @param loop_index Zero-based point index.
   * @param v3f XYZ coordinates.
   * @param args Additional field values.
   * @return @c false on failure, or when any XYZ coordinate is outside @c (-extent, +extent) on a
   *         quantised cloud (the out-of-range point is discarded).
   */
  template <typename... T>
  bool set_value_v3f(size_t loop_index, Vector3f v3f, T... args) noexcept;

  /**
   * @brief Overwrites point @p loop_index with @c double XYZ plus additional fields.
   *
   * @tparam T Types for additional fields beyond XYZ.
   * @param loop_index Zero-based point index.
   * @param x X coordinate.
   * @param y Y coordinate.
   * @param z Z coordinate.
   * @param args Additional field values.
   * @return @c false on failure, or when any XYZ coordinate is outside @c (-extent, +extent) on a
   *         quantised cloud (the out-of-range point is discarded).
   */
  template <typename... T>
  bool set_value_v3d(size_t loop_index, double x, double y, double z, T... args) noexcept;

  /**
   * @brief Overwrites point @p loop_index from a @c Vector3d plus additional fields.
   *
   * @tparam T Types for additional fields beyond XYZ.
   * @param loop_index Zero-based point index.
   * @param v3d XYZ coordinates.
   * @param args Additional field values.
   * @return @c false on failure, or when any XYZ coordinate is outside @c (-extent, +extent) on a
   *         quantised cloud (the out-of-range point is discarded).
   */
  template <typename... T>
  bool set_value_v3d(size_t loop_index, Vector3d v3d, T... args) noexcept;

  /**
   * @brief Resets the logical count, optionally releasing every resource.
   *
   * @details
   * With @p force == @c false (default) only the write cursor and logical
   * count are zeroed so the buffer can be refilled in place.  With
   * @p force == @c true the owned buffer is freed and every protocol /
   * header field is cleared.
   *
   * @param force When @c true, free the buffer and reset all state.
   */
  void clear(bool force = false) noexcept;

  /**
   * @brief Mutable reference to the first wire-locked reserved field (@c uint32_t at offset 232).
   *
   * @details
   * Lets application code stash a small flag or minor sub-type id that travels through the
   * wire format unchanged.  It is part of the binary contract and MUST NOT be repurposed once
   * a future library revision binds it to a real field.
   *
   * @return Reference to @c reserved_buf_.
   */
  uint32_t& get_reserved() noexcept { return reserved_buf_; }

  /**
   * @brief Mutable reference to the second wire-locked reserved field (@c uint32_t at offset 236).
   *
   * @details
   * Lets application code stash a small flag or minor sub-type id that travels through the
   * wire format unchanged.  It is part of the binary contract and MUST NOT be repurposed once
   * a future library revision binds it to a real field.
   *
   * @return Reference to @c reserved_buf2_.
   */
  uint32_t& get_reserved2() noexcept { return reserved_buf2_; }

  /**
   * @brief Mutable reference to the third wire-locked reserved field (@c uint8_t at offset 245).
   *
   * @details
   * Lets application code stash a small flag or minor sub-type id that travels through the
   * wire format unchanged.  It is part of the binary contract and MUST NOT be repurposed once
   * a future library revision binds it to a real field.
   *
   * @return Reference to @c reserved_buf3_.
   */
  uint8_t& get_reserved3() noexcept { return reserved_buf3_; }

  Header header;  ///< Sequencing and timestamp metadata prefix.

  static constexpr bool kZerocopyTypes{true};  ///< Marker probed by the VLink type-trait machinery.

 private:
  struct VLINK_EXPORT_AND_ALIGNED(8) Protocol final {
    uint64_t size_num{0};
    uint64_t type_num{0};
    char names[152]{0};

    Protocol() noexcept = default;

    template <typename... T>
    static uint64_t get_size_num() noexcept;

    template <typename... T>
    static uint64_t get_type_num() noexcept;

    template <typename T>
    static constexpr uint8_t get_type() noexcept;

    constexpr uint64_t get_pack_size() const noexcept;

    static bool check_valid(uint64_t _size_num, std::string_view _names) noexcept;

    static std::string get_names(const std::vector<std::string>& keys) noexcept;

    KeyList get_key_list() const noexcept;

    std::string get_size_for_print() const noexcept;

    std::string get_type_for_print() const noexcept;
  };

  template <typename T>
  static bool xyz_in_extent(T x, T y, T z, uint16_t extent) noexcept;

  bool compress_protocol_xyz() noexcept;

  Protocol protocol_;
  uint8_t* data_{nullptr};
  size_t capacity_{0};
  size_t size_{0};
  uint32_t reserved_buf_{0};
  uint32_t reserved_buf2_{0};
  uint16_t pack_size_{0};
  uint16_t extent_{0};
  uint8_t downsample_{0};
  uint8_t reserved_buf3_{0};
  bool vertical_{false};
  bool is_owner_{false};
  uint64_t index_{0};

  static constexpr uint32_t kMagicNumberBegin{0x98B7F16A};
  static constexpr uint32_t kMagicNumberEnd{0x98B7F16F};
};

////////////////////////////////////////////////////////////////
/// Details
////////////////////////////////////////////////////////////////

template <typename T>
inline bool PointCloud::get_value(T& t, size_t loop_index, uint16_t offset) const noexcept {
  static_assert(std::is_fundamental_v<T>, "T must be fundamental.");

  if VUNLIKELY (!data_ || loop_index >= size_ || offset > pack_size_) {
    std::memset(&t, 0, sizeof(T));
    return false;
  }

  if (extent_ != 0 && offset < sizeof(int16_t) * 3) {
    if constexpr (std::is_same_v<T, float> || std::is_same_v<T, double>) {
      int16_t value = 0;

      if VUNLIKELY (sizeof(value) > static_cast<size_t>(pack_size_ - offset)) {
        t = 0;
        return false;
      }

      const size_t p = (loop_index * pack_size_) + offset;
      std::memcpy(&value, data_ + p, sizeof(value));
      t = Quantize::decode<T>(extent_, value);

      return true;
    }
  }

  if VUNLIKELY (sizeof(T) > static_cast<size_t>(pack_size_ - offset)) {
    std::memset(&t, 0, sizeof(T));
    return false;
  }

  const size_t p = (loop_index * pack_size_) + offset;
  std::memcpy(&t, data_ + p, sizeof(T));

  return true;
}

template <typename T>
inline T PointCloud::get_value(size_t loop_index, uint16_t offset) const noexcept {
  static_assert(std::is_fundamental_v<T>, "T must be fundamental.");

  T t;

  get_value(t, loop_index, offset);

  return t;
}

template <typename T>
inline bool PointCloud::get_value(T& t, size_t loop_index, KeyMap& key_map, std::string_view key) const noexcept {
  static_assert(std::is_fundamental_v<T>, "T must be fundamental.");

  const std::string lookup(key);
  auto iter = key_map.find(lookup);

  if VUNLIKELY (iter == key_map.end()) {
    std::memset(&t, 0, sizeof(T));
    return false;
  }

  return get_value<T>(t, loop_index, iter->second);
}

template <typename T>
inline T PointCloud::get_value(size_t loop_index, KeyMap& key_map, std::string_view key) const noexcept {
  static_assert(std::is_fundamental_v<T>, "T must be fundamental.");

  T t;

  get_value(t, loop_index, key_map, key);

  return t;
}

template <typename... T>
inline bool PointCloud::create(size_t size, const std::vector<std::string>& keys, uint16_t extent,
                               bool vertical) noexcept {
  static_assert((std::is_fundamental_v<T> && ...), "All types must be fundamental.");

  static_assert(sizeof...(T) >= 3 && sizeof...(T) <= 16, "The number of keys ranges is [3 ~ 16].");

  if VUNLIKELY (sizeof...(T) != keys.size()) {
    return false;
  }

  uint64_t size_num = Protocol::get_size_num<T...>();

  if VUNLIKELY (size_num == 0) {
    return false;
  }

  std::string key_str = Protocol::get_names(keys);

  if VUNLIKELY (key_str.empty()) {
    return false;
  }

  uint64_t type_num = Protocol::get_type_num<T...>();

  if VUNLIKELY (type_num == 0) {
    return false;
  }

  Protocol new_protocol{};
  new_protocol.size_num = size_num;
  std::memset(new_protocol.names, 0, sizeof(new_protocol.names));
  std::memcpy(new_protocol.names, key_str.c_str(), key_str.size());
  new_protocol.type_num = type_num;

  if (extent != 0) {
    PointCloud protocol_probe;
    protocol_probe.protocol_ = new_protocol;

    if VUNLIKELY (!protocol_probe.compress_protocol_xyz()) {
      return false;
    }

    new_protocol = protocol_probe.protocol_;
  }

  const size_t new_pack_size = new_protocol.get_pack_size();

  if VUNLIKELY (new_pack_size != 0 && size > std::numeric_limits<size_t>::max() / new_pack_size) {
    return false;
  }

  if (is_owner_ && data_ && capacity_ != 0) {
    Bytes::bytes_free(data_, capacity_);
  }

  data_ = nullptr;
  is_owner_ = false;
  capacity_ = 0;
  size_ = 0;
  index_ = 0;

  protocol_ = new_protocol;
  extent_ = extent;
  vertical_ = vertical;
  downsample_ = 0;

  pack_size_ = new_pack_size;
  capacity_ = size * pack_size_;

  if VLIKELY (capacity_ != 0) {
    data_ = Bytes::bytes_malloc(capacity_);

    if VUNLIKELY (!data_) {
      capacity_ = 0;
      return false;
    }

    is_owner_ = true;
  }

  return true;
}

template <typename... T>
inline bool PointCloud::create_v3f(size_t _size, const std::vector<std::string>& keys, uint16_t extent,
                                   bool vertical) noexcept {
  std::vector<std::string> target_keys{"x", "y", "z"};

  target_keys.insert(target_keys.end(), keys.begin(), keys.end());

  return create<float, float, float, T...>(_size, target_keys, extent, vertical);
}

template <typename... T>
inline bool PointCloud::create_v3d(size_t _size, const std::vector<std::string>& keys, uint16_t extent,
                                   bool vertical) noexcept {
  std::vector<std::string> target_keys{"x", "y", "z"};

  target_keys.insert(target_keys.end(), keys.begin(), keys.end());

  return create<double, double, double, T...>(_size, target_keys, extent, vertical);
}

inline bool PointCloud::fill_packed_data(const uint8_t* src_data, size_t _size) noexcept {
  bool is_fill_success = false;

  if VUNLIKELY (_size == 0 || !src_data) {
    is_fill_success = false;
  } else if VUNLIKELY (!is_owner_ || !data_ || pack_size_ == 0 || capacity_ == 0) {
    is_fill_success = false;
  } else if VUNLIKELY (_size > std::numeric_limits<size_t>::max() / pack_size_ || _size * pack_size_ > capacity_) {
    is_fill_success = false;
  } else {
    std::memcpy(data_, src_data, _size * pack_size_);
    size_ = _size;
    index_ = _size * pack_size_;
    is_fill_success = true;
  }

  return is_fill_success;
}

template <typename... T>
inline bool PointCloud::push_value(T... args) noexcept {
  static_assert((std::is_fundamental_v<T> && ...), "All types must be fundamental.");

  static_assert(sizeof...(T) >= 3 && sizeof...(T) <= 16, "The number of keys ranges is [3 ~ 16].");

  if VUNLIKELY (!is_owner_ || !data_ || pack_size_ == 0 || capacity_ == 0) {
    return false;
  }

  if VUNLIKELY (size_ * pack_size_ >= capacity_) {
    return false;
  }

  constexpr size_t kTargetPackSize = (sizeof(T) + ...);

  if VUNLIKELY (kTargetPackSize != pack_size_) {
    return false;
  }

  auto* target_ptr = data_ + index_;

  (
      [&]() {
        std::memcpy(target_ptr, &args, sizeof(args));
        target_ptr += sizeof(args);
      }(),
      ...);

  index_ += pack_size_;
  ++size_;

  return true;
}

template <typename... T>
inline bool PointCloud::push_value_v3f(float x, float y, float z, T... args) noexcept {
  if (extent_ != 0) {
    if VUNLIKELY (!xyz_in_extent(x, y, z, extent_)) {
      return false;
    }

    auto qx = Quantize::encode<int16_t>(extent_, x);
    auto qy = Quantize::encode<int16_t>(extent_, y);
    auto qz = Quantize::encode<int16_t>(extent_, z);

    return push_value(qx, qy, qz, args...);
  }

  return push_value(x, y, z, args...);
}

template <typename... T>
inline bool PointCloud::push_value_v3f(Vector3f v3f, T... args) noexcept {
  return push_value_v3f(v3f.x, v3f.y, v3f.z, args...);
}

template <typename... T>
inline bool PointCloud::push_value_v3d(double x, double y, double z, T... args) noexcept {
  if (extent_ != 0) {
    if VUNLIKELY (!xyz_in_extent(x, y, z, extent_)) {
      return false;
    }

    auto qx = Quantize::encode<int16_t>(extent_, x);
    auto qy = Quantize::encode<int16_t>(extent_, y);
    auto qz = Quantize::encode<int16_t>(extent_, z);

    return push_value(qx, qy, qz, args...);
  }

  return push_value(x, y, z, args...);
}

template <typename... T>
inline bool PointCloud::push_value_v3d(Vector3d v3d, T... args) noexcept {
  return push_value_v3d(v3d.x, v3d.y, v3d.z, args...);
}

inline bool PointCloud::resize(size_t size) noexcept {
  if VUNLIKELY (!is_owner_ || !data_ || pack_size_ == 0 || capacity_ == 0) {
    return false;
  }

  if VUNLIKELY (size > std::numeric_limits<size_t>::max() / pack_size_) {
    return false;
  }

  if VUNLIKELY (size * pack_size_ > capacity_) {
    return false;
  }

  size_ = size;
  index_ = size_ * pack_size_;

  return true;
}

template <typename... T>
bool PointCloud::set_value(size_t loop_index, T... args) noexcept {
  static_assert((std::is_fundamental_v<T> && ...), "All types must be fundamental.");

  static_assert(sizeof...(T) >= 3 && sizeof...(T) <= 16, "The number of keys ranges is [3 ~ 16].");

  if VUNLIKELY (!is_owner_ || !data_ || pack_size_ == 0 || capacity_ == 0) {
    return false;
  }

  if VUNLIKELY (loop_index >= size_ || size_ * pack_size_ > capacity_) {
    return false;
  }

  if VUNLIKELY (index_ != size_ * pack_size_) {
    std::cerr << "[PointCloud::set_value] Invalid buffer state: "
              << "The current buffer size does not match the capacity. "
              << "Please call resize(size) before using set_value()." << std::endl;
    return false;
  }

  constexpr size_t kTargetPackSize = (sizeof(T) + ...);

  if VUNLIKELY (kTargetPackSize != pack_size_) {
    return false;
  }

  auto* target_ptr = data_ + (loop_index * pack_size_);

  (
      [&]() {
        std::memcpy(target_ptr, &args, sizeof(args));
        target_ptr += sizeof(args);
      }(),
      ...);

  return true;
}

template <typename... T>
inline bool PointCloud::set_value_v3f(size_t loop_index, float x, float y, float z, T... args) noexcept {
  if (extent_ != 0) {
    if VUNLIKELY (!xyz_in_extent(x, y, z, extent_)) {
      return false;
    }

    auto qx = Quantize::encode<int16_t>(extent_, x);
    auto qy = Quantize::encode<int16_t>(extent_, y);
    auto qz = Quantize::encode<int16_t>(extent_, z);

    return set_value(loop_index, qx, qy, qz, args...);
  }

  return set_value(loop_index, x, y, z, args...);
}

template <typename... T>
inline bool PointCloud::set_value_v3f(size_t loop_index, Vector3f v3f, T... args) noexcept {
  return set_value_v3f(loop_index, v3f.x, v3f.y, v3f.z, args...);
}

template <typename... T>
inline bool PointCloud::set_value_v3d(size_t loop_index, double x, double y, double z, T... args) noexcept {
  if (extent_ != 0) {
    if VUNLIKELY (!xyz_in_extent(x, y, z, extent_)) {
      return false;
    }

    auto qx = Quantize::encode<int16_t>(extent_, x);
    auto qy = Quantize::encode<int16_t>(extent_, y);
    auto qz = Quantize::encode<int16_t>(extent_, z);

    return set_value(loop_index, qx, qy, qz, args...);
  }

  return set_value(loop_index, x, y, z, args...);
}

template <typename... T>
inline bool PointCloud::set_value_v3d(size_t loop_index, Vector3d v3d, T... args) noexcept {
  return set_value_v3d(loop_index, v3d.x, v3d.y, v3d.z, args...);
}

template <typename... T>
inline uint64_t PointCloud::Protocol::get_size_num() noexcept {
  uint64_t target_num = 0;

  uint64_t key_shift = sizeof...(T) * 4;

  (
      [&](auto type) {
        key_shift -= 4;
        target_num |= (static_cast<uint64_t>(sizeof(type)) & 0xF) << key_shift;
      }(T{}),
      ...);

  return target_num;
}

template <typename... T>
inline uint64_t PointCloud::Protocol::get_type_num() noexcept {
  uint64_t target_num = 0;

  uint64_t key_shift = sizeof...(T) * 4;

  (
      [&](auto type) {
        key_shift -= 4;
        target_num |= (static_cast<uint64_t>(get_type<decltype(type)>()) & 0xF) << key_shift;
      }(T{}),
      ...);

  return target_num;
}

template <typename T>
inline constexpr uint8_t PointCloud::Protocol::get_type() noexcept {
  if constexpr (std::is_same_v<T, bool>) {
    return kBoolType;
  } else if constexpr (std::is_same_v<T, int8_t>) {
    return kInt8Type;
  } else if constexpr (std::is_same_v<T, uint8_t>) {
    return kUint8Type;
  } else if constexpr (std::is_same_v<T, int16_t>) {
    return kInt16Type;
  } else if constexpr (std::is_same_v<T, uint16_t>) {
    return kUint16Type;
  } else if constexpr (std::is_same_v<T, int32_t>) {
    return kInt32Type;
  } else if constexpr (std::is_same_v<T, uint32_t>) {
    return kUint32Type;
  } else if constexpr (std::is_same_v<T, int64_t>) {
    return kInt64Type;
  } else if constexpr (std::is_same_v<T, uint64_t>) {
    return kUint64Type;
  } else if constexpr (std::is_same_v<T, float>) {
    return kFloatType;
  } else if constexpr (std::is_same_v<T, double>) {
    return kDoubleType;
  } else {
    return kUnknownType;
  }
}

inline constexpr uint64_t PointCloud::Protocol::get_pack_size() const noexcept {
  int sum = 0;

  auto target_num = size_num;

  while (target_num > 0) {
    sum += target_num & 0xF;
    target_num >>= 4;
  }

  return sum;
}

template <typename T>
inline bool PointCloud::xyz_in_extent(T x, T y, T z, uint16_t extent) noexcept {
  return x > -extent && x < extent && y > -extent && y < extent && z > -extent && z < extent;
}

}  // namespace zerocopy

}  // namespace vlink
