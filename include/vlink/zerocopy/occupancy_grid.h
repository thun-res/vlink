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
 * @file occupancy_grid.h
 * @brief Zero-copy 2-D occupancy / cost-map grid container with typed cell storage.
 *
 * @details
 * @c OccupancyGrid is the canonical 2-D map message for VLink autonomous-driving
 * stacks: local costmaps, global occupancy, lane-level reachability, signed
 * distance fields, log-odds buffers, etc.  Each grid carries the cell buffer,
 * the world-to-grid transform, value range, default value, occupied / free
 * thresholds, a 40-byte @c Header for sequencing, and a map identifier so
 * that multiple map types can share one topic.
 *
 * @par Cell-value semantics
 * | @c CellType    | C++ type   | Bytes | Free   | Occupied | Unknown                         |
 * | -------------- | ---------- | ----- | ------ | -------- | ------------------------------- |
 * | @c kCellInt8   | @c int8_t  | 1     | 0      | 100      | -1 (ROS @c nav_msgs convention) |
 * | @c kCellUint8  | @c uint8_t | 1     | 0      | 254      | 255                             |
 * | @c kCellUint16 | @c uint16_t| 2     | 0      | 65534    | 65535                           |
 * | @c kCellFloat32| @c float   | 4     | <= ft  | >= ot    | NaN (ft / ot = thresholds)      |
 *
 * @par Coordinate system
 * @code
 *     +y
 *      ^
 *      |   (origin_x, origin_y) lies at the bottom-left
 *      |   corner of the cell at column 0 / row 0.  The
 *      |   grid is rotated by origin_yaw radians (REP-103)
 *      |   around that origin.
 *      |
 *      +---->-->-->-->-->  +x
 *      origin_x,origin_y   each cell spans resolution metres
 *
 *     world(x, y) = origin + R(origin_yaw) * (column, row) * resolution
 * @endcode
 *
 * @par Wire format
 * @c OccupancyGrid is POD; @c memcpy is the canonical serialiser.  The
 * @c sizeof value is locked by @c static_assert and forms a permanent contract:
 * @c vlink::zerocopy::* containers offer NO forward and NO backward binary
 * compatibility -- every field, including reserved bytes, is wire-locked.
 * @code
 * static_assert(sizeof(OccupancyGrid) == 152, "Sizeof must be 152 bytes.");
 * @endcode
 *
 * @par Memory layout
 * @code
 * Offset  Size  Field
 * ------  ----  --------------------------------
 *      0    40  Header   header
 *     40     8  uint8_t* data_
 *     48     8  size_t   size_
 *     56     8  uint64_t update_time_ns_
 *     64    16  char     map_id_[16]
 *     80     4  uint32_t channel_
 *     84     4  uint32_t freq_
 *     88     4  uint32_t width_
 *     92     4  uint32_t height_
 *     96     4  uint32_t valid_cell_count_
 *    100     4  float    resolution_
 *    104     4  float    origin_x_
 *    108     4  float    origin_y_
 *    112     4  float    origin_z_
 *    116     4  float    origin_yaw_
 *    120     4  float    value_min_
 *    124     4  float    value_max_
 *    128     4  int32_t  default_value_
 *    132     4  float    occupied_threshold_
 *    136     4  float    free_threshold_
 *    140     1  CellType cell_type_
 *    141     1  bool     is_owner_
 *    142     2  uint16_t reserved_buf_
 *    144     4  uint32_t reserved_buf2_
 *    148     4  uint32_t reserved_buf3_
 * ------  ----  --------------------------------
 *  Total   152  bytes (alignas 8)
 *
 * Wire envelope:
 * [ magic_begin (4) | version (4) | OccupancyGrid (152) | cell bytes (width*height*cell_size) | magic_end (4) ]
 * @endcode
 *
 * @par Reserved bytes
 * @c reserved_buf_, @c reserved_buf2_, @c reserved_buf3_ are exposed through
 * @c get_reserved* helpers and survive @c clear() and the copy / move helpers.
 * These slots MUST NOT be repurposed by application code: future library
 * revisions may bind them to real fields, silently breaking peers that abused
 * the slot.
 *
 * @par Example
 * @code
 * vlink::zerocopy::OccupancyGrid og;
 * og.set_width(400);
 * og.set_height(400);
 * og.set_resolution(0.05F);
 * og.set_origin_x(-10.0F);
 * og.set_origin_y(-10.0F);
 * og.set_cell_type(vlink::zerocopy::OccupancyGrid::kCellInt8);
 * og.set_default_value(-1);
 * og.set_occupied_threshold(0.65F);
 * og.set_free_threshold(0.20F);
 * og.create(400 * 400);
 *
 * vlink::Bytes wire;
 * og >> wire;
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
 * @struct OccupancyGrid
 * @brief 152-byte POD container holding a typed 2-D occupancy / cost grid plus pose metadata.
 *
 * @details
 * Cells are stored in row-major order with a stride of @c cell_size() bytes.
 * The struct size is locked at 152 bytes on 64-bit targets via
 * @c static_assert.  Copy semantics are deep; the move constructor / assignment
 * transfer ownership without allocation.
 */
struct VLINK_EXPORT_AND_ALIGNED(8) OccupancyGrid final {
  /**
   * @brief Per-cell storage type tag (drives @c cell_size()).
   */
  enum CellType : uint8_t {
    kCellUnknown = 0,  ///< Uninitialised / unspecified cell type.
    kCellInt8 = 1,     ///< Signed 8-bit cell (ROS @c nav_msgs/OccupancyGrid style).
    kCellUint8 = 2,    ///< Unsigned 8-bit cell (0..255 costmap / grayscale).
    kCellUint16 = 3,   ///< Unsigned 16-bit cell (high-resolution costmap).
    kCellFloat32 = 4,  ///< IEEE-754 single-precision cell (log-odds, probability, SDF).
  };

  /**
   * @brief Default-constructs an empty grid and asserts the 152-byte contract.
   */
  OccupancyGrid() noexcept;

  /**
   * @brief Frees the owned cell buffer when @c is_owner() is @c true.
   */
  ~OccupancyGrid() noexcept;

  /**
   * @brief Deep-copies @p target into a freshly allocated grid.
   *
   * @param target Source grid to clone.
   */
  OccupancyGrid(const OccupancyGrid& target) noexcept;

  /**
   * @brief Steals @p target's allocation and metadata; @p target ends empty.
   *
   * @param target Source grid moved from.
   */
  OccupancyGrid(OccupancyGrid&& target) noexcept;

  /**
   * @brief Deep-copy-assigns @p target; self-assignment is a no-op.
   *
   * @param target Source grid to clone.
   * @return Reference to @c *this.
   */
  OccupancyGrid& operator=(const OccupancyGrid& target) noexcept;

  /**
   * @brief Move-assigns @p target; self-assignment is a no-op.
   *
   * @param target Source grid moved from.
   * @return Reference to @c *this.
   */
  OccupancyGrid& operator=(OccupancyGrid&& target) noexcept;

  /**
   * @brief Deserialises an @c OccupancyGrid from @p bytes with zero-copy borrowing semantics.
   *
   * @param bytes Wire buffer previously produced by @c operator>>.
   * @return @c true on success; @c false on magic mismatch or size mismatch.
   */
  bool operator<<(const Bytes& bytes) noexcept;

  /**
   * @brief Serialises the struct snapshot plus cell bytes into @p bytes.
   *
   * @param bytes Output buffer; resized automatically when its size differs from the serialized size.
   * @return @c true on success; @c false when output allocation fails.
   */
  bool operator>>(Bytes& bytes) const noexcept;

  /**
   * @brief Validates that @p bytes carries a well-formed @c OccupancyGrid envelope.
   *
   * @param bytes Wire buffer to inspect.
   * @return @c true when both magic sentinels match and the minimum size holds.
   */
  [[nodiscard]] static bool check_valid(const Bytes& bytes) noexcept;

  /**
   * @brief Total bytes that @c operator>> would write for this grid.
   *
   * @return @c sizeof(magic_begin) + @c sizeof(version) + @c sizeof(OccupancyGrid) + @c size() + @c sizeof(magic_end).
   */
  [[nodiscard]] size_t get_serialized_size() const noexcept;

  /**
   * @brief Whether the cell buffer pointer is non-null and the byte size is positive.
   *
   * @return @c true when the grid holds usable cell data.
   */
  [[nodiscard]] bool is_valid() const noexcept;

  /**
   * @brief Borrows @p target's cell buffer without copying.
   *
   * @param target Source grid whose buffer must outlive @c *this.
   * @return @c false on self-borrow or owned-buffer aliasing, otherwise @c true.
   */
  bool shallow_copy(const OccupancyGrid& target) noexcept;

  /**
   * @brief Allocates (or reuses) an owned buffer and copies @p target's cells.
   *
   * @param target Source grid to clone.
   * @return @c false on self-copy, owned-buffer aliasing, or allocation failure.
   */
  bool deep_copy(const OccupancyGrid& target) noexcept;

  /**
   * @brief Transfers ownership from @p target; @p target ends empty.
   *
   * @param target Source grid moved from.
   * @return @c false on self-move, otherwise @c true.
   */
  bool move_copy(OccupancyGrid& target) noexcept;

  /**
   * @brief Allocates an uninitialised owned cell buffer of @p size bytes.
   *
   * @details
   * The caller is responsible for keeping @p size consistent with
   * @c width() * @c height() * @c cell_size().
   *
   * @param size Byte count; must be non-zero.
   * @return @c false when @p size is zero or allocation fails, otherwise @c true.
   */
  bool create(size_t size) noexcept;

  /**
   * @brief Releases the owned buffer (if any) and resets metadata except reserved fields.
   */
  void clear() noexcept;

  /**
   * @brief Borrows an externally owned cell buffer without copying.
   *
   * @param data Non-null source pointer that must outlive @c *this.
   * @param size Buffer length in bytes; must be non-zero.
   * @return @c false on invalid arguments, unchanged pointer, or owned-buffer aliasing, otherwise @c true.
   */
  bool shallow_copy(uint8_t* data, size_t size) noexcept;

  /**
   * @brief Copies @p size bytes from @p data into an owned cell buffer.
   *
   * @param data Non-null source pointer.
   * @param size Number of bytes to copy; must be non-zero.
   * @return @c false on invalid arguments, aliasing, or allocation failure, otherwise @c true.
   */
  bool deep_copy(uint8_t* data, size_t size) noexcept;

  /**
   * @brief Compatibility alias for @c deep_copy(uint8_t*, size_t).
   *
   * @param data Source pointer.
   * @param size Number of bytes.
   * @return Result of the delegated @c deep_copy call.
   */
  bool fill_data(uint8_t* data, size_t size) noexcept;

  /**
   * @brief Producer-side timestamp recording when the map was last updated.
   *
   * @return Stored nanosecond timestamp.
   */
  [[nodiscard]] uint64_t update_time_ns() const noexcept;

  /**
   * @brief Unique map identifier (e.g. @c "local", @c "global", @c "lane").
   *
   * @return Non-owning view into the embedded buffer.
   */
  [[nodiscard]] std::string_view map_id() const noexcept;

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
   * @brief Grid width in cells (columns).
   *
   * @return Stored width.
   */
  [[nodiscard]] uint32_t width() const noexcept;

  /**
   * @brief Grid height in cells (rows).
   *
   * @return Stored height.
   */
  [[nodiscard]] uint32_t height() const noexcept;

  /**
   * @brief Number of cells whose value differs from @c default_value() (producer-supplied hint).
   *
   * @return Stored count; zero means "not provided".
   */
  [[nodiscard]] uint32_t valid_cell_count() const noexcept;

  /**
   * @brief Cell side length in metres.
   *
   * @return Stored resolution.
   */
  [[nodiscard]] float resolution() const noexcept;

  /**
   * @brief World-frame X coordinate of the bottom-left grid corner.
   *
   * @return Stored origin X.
   */
  [[nodiscard]] float origin_x() const noexcept;

  /**
   * @brief World-frame Y coordinate of the bottom-left grid corner.
   *
   * @return Stored origin Y.
   */
  [[nodiscard]] float origin_y() const noexcept;

  /**
   * @brief World-frame Z coordinate of the 2-D plane.
   *
   * @return Stored origin Z.
   */
  [[nodiscard]] float origin_z() const noexcept;

  /**
   * @brief Yaw rotation of the grid origin in radians (REP-103).
   *
   * @return Stored origin yaw.
   */
  [[nodiscard]] float origin_yaw() const noexcept;

  /**
   * @brief Lower bound of cell values used for normalisation / visualisation.
   *
   * @return Stored minimum.
   */
  [[nodiscard]] float value_min() const noexcept;

  /**
   * @brief Upper bound of cell values used for normalisation / visualisation.
   *
   * @return Stored maximum.
   */
  [[nodiscard]] float value_max() const noexcept;

  /**
   * @brief Value used for unknown / uninitialised cells.
   *
   * @return Stored default value (e.g. -1 for ROS int8 occupancy).
   */
  [[nodiscard]] int32_t default_value() const noexcept;

  /**
   * @brief Threshold above which a cell is considered occupied.
   *
   * @return Stored threshold.
   */
  [[nodiscard]] float occupied_threshold() const noexcept;

  /**
   * @brief Threshold below which a cell is considered free.
   *
   * @return Stored threshold.
   */
  [[nodiscard]] float free_threshold() const noexcept;

  /**
   * @brief Per-cell storage type tag.
   *
   * @return @c CellType enum value.
   */
  [[nodiscard]] CellType cell_type() const noexcept;

  /**
   * @brief Byte size of one cell derived from @c cell_type().
   *
   * @return Cell stride in bytes (0 for @c kCellUnknown).
   */
  [[nodiscard]] uint8_t cell_size() const noexcept;

  /**
   * @brief Read-only pointer to the cell bytes.
   *
   * @return Pointer to payload start.
   */
  [[nodiscard]] const uint8_t* data() const noexcept;

  /**
   * @brief Cell buffer size in bytes.
   *
   * @return Byte count, or 0 when empty.
   */
  [[nodiscard]] size_t size() const noexcept;

  /**
   * @brief Whether this grid owns its cell buffer.
   *
   * @return @c true when the destructor would free the buffer.
   */
  [[nodiscard]] bool is_owner() const noexcept;

  /**
   * @brief Stores the producer-side map timestamp.
   *
   * @param update_time_ns Timestamp in nanoseconds.
   */
  void set_update_time_ns(uint64_t update_time_ns) noexcept;

  /**
   * @brief Stores the unique map identifier (truncated to @c sizeof(map_id) - 1 bytes).
   *
   * @param map_id Identifier string.
   */
  void set_map_id(std::string_view map_id) noexcept;

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
   * @brief Stores the grid width.
   *
   * @param width Number of columns.
   */
  void set_width(uint32_t width) noexcept;

  /**
   * @brief Stores the grid height.
   *
   * @param height Number of rows.
   */
  void set_height(uint32_t height) noexcept;

  /**
   * @brief Stores the count of cells differing from the default value.
   *
   * @param valid_cell_count Producer-supplied hint.
   */
  void set_valid_cell_count(uint32_t valid_cell_count) noexcept;

  /**
   * @brief Stores the cell resolution.
   *
   * @param resolution Metres per cell.
   */
  void set_resolution(float resolution) noexcept;

  /**
   * @brief Stores the world-frame X origin.
   *
   * @param origin_x World X coordinate of the bottom-left corner.
   */
  void set_origin_x(float origin_x) noexcept;

  /**
   * @brief Stores the world-frame Y origin.
   *
   * @param origin_y World Y coordinate of the bottom-left corner.
   */
  void set_origin_y(float origin_y) noexcept;

  /**
   * @brief Stores the world-frame Z value of the 2-D plane.
   *
   * @param origin_z World Z coordinate of the plane.
   */
  void set_origin_z(float origin_z) noexcept;

  /**
   * @brief Stores the yaw rotation of the grid origin.
   *
   * @param origin_yaw Rotation in radians (REP-103).
   */
  void set_origin_yaw(float origin_yaw) noexcept;

  /**
   * @brief Stores the lower bound for cell values.
   *
   * @param value_min Stored minimum.
   */
  void set_value_min(float value_min) noexcept;

  /**
   * @brief Stores the upper bound for cell values.
   *
   * @param value_max Stored maximum.
   */
  void set_value_max(float value_max) noexcept;

  /**
   * @brief Stores the value used for unknown cells.
   *
   * @param default_value Default cell value.
   */
  void set_default_value(int32_t default_value) noexcept;

  /**
   * @brief Stores the occupied threshold.
   *
   * @param occupied_threshold Stored threshold.
   */
  void set_occupied_threshold(float occupied_threshold) noexcept;

  /**
   * @brief Stores the free threshold.
   *
   * @param free_threshold Stored threshold.
   */
  void set_free_threshold(float free_threshold) noexcept;

  /**
   * @brief Stores the per-cell storage type tag.
   *
   * @param cell_type @c CellType enum value.
   */
  void set_cell_type(CellType cell_type) noexcept;

  /**
   * @brief Returns the byte size of one cell of @p type.
   *
   * @param type Cell type tag.
   * @return Cell stride in bytes (0 for @c kCellUnknown).
   */
  [[nodiscard]] static uint8_t cell_size_of(CellType type) noexcept;

  /**
   * @brief Mutable accessor for the first wire-locked reserved field (@c uint16_t).
   *
   * @return Reference to @c reserved_buf_.
   */
  uint16_t& get_reserved() noexcept { return reserved_buf_; }

  /**
   * @brief Mutable accessor for the second wire-locked reserved field (@c uint32_t).
   *
   * @return Reference to @c reserved_buf2_.
   */
  uint32_t& get_reserved2() noexcept { return reserved_buf2_; }

  /**
   * @brief Mutable accessor for the third wire-locked reserved field (@c uint32_t).
   *
   * @return Reference to @c reserved_buf3_.
   */
  uint32_t& get_reserved3() noexcept { return reserved_buf3_; }

  Header header;  ///< Sequencing and timestamp metadata prefix.

  static constexpr bool kZerocopyTypes{true};  ///< Marker probed by the VLink type-trait machinery.

 private:
  uint8_t* data_{nullptr};
  size_t size_{0};
  uint64_t update_time_ns_{0};
  char map_id_[16]{0};
  uint32_t channel_{0};
  uint32_t freq_{0};
  uint32_t width_{0};
  uint32_t height_{0};
  uint32_t valid_cell_count_{0};
  float resolution_{0};
  float origin_x_{0};
  float origin_y_{0};
  float origin_z_{0};
  float origin_yaw_{0};
  float value_min_{0};
  float value_max_{0};
  int32_t default_value_{0};
  float occupied_threshold_{0};
  float free_threshold_{0};
  CellType cell_type_{kCellUnknown};
  bool is_owner_{false};
  uint16_t reserved_buf_{0};
  uint32_t reserved_buf2_{0};
  uint32_t reserved_buf3_{0};

  static constexpr uint32_t kMagicNumberBegin{0x98B7F17A};
  static constexpr uint32_t kMagicNumberEnd{0x98B7F17F};
};

}  // namespace zerocopy

}  // namespace vlink
