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
 * @file tensor.h
 * @brief Zero-copy N-dimensional dense tensor container with shape, strides, and dtype metadata.
 *
 * @details
 * @c Tensor is the canonical neural-network input / output message for the
 * VLink autonomous-driving and embodied-intelligence stacks: camera feature
 * maps, point-cloud features, language-model hidden states, action heads,
 * diffusion latents, INT8 quantised activations, and so on.  Each tensor
 * carries the element buffer, shape (up to @c kMaxRank dimensions), strides,
 * dtype, layout label, optional model identifier, optional INT8 quantisation
 * scale / zero point, device hint, plus a 40-byte @c Header for sequencing.
 *
 * @par DType table
 * | Enum            | C++ type    | Element size |
 * | --------------- | ----------- | ------------ |
 * | @c kBool        | @c bool     | 1            |
 * | @c kInt8        | @c int8_t   | 1            |
 * | @c kUint8       | @c uint8_t  | 1            |
 * | @c kInt16       | @c int16_t  | 2            |
 * | @c kUint16      | @c uint16_t | 2            |
 * | @c kInt32       | @c int32_t  | 4            |
 * | @c kUint32      | @c uint32_t | 4            |
 * | @c kInt64       | @c int64_t  | 8            |
 * | @c kUint64      | @c uint64_t | 8            |
 * | @c kFloat16     | half        | 2            |
 * | @c kBfloat16    | bf16        | 2            |
 * | @c kFloat32     | @c float    | 4            |
 * | @c kFloat64     | @c double   | 8            |
 *
 * @par Shape diagram
 * @code
 * rank: 1..kMaxRank (kMaxRank = 8)
 * shape  [d0, d1, ..., d{rank-1}]    e.g. NCHW = [N, C, H, W]
 * strides[s0, s1, ..., s{rank-1}]    elements (not bytes) to step one index
 *
 * Contiguous row-major (computed by set_shape):
 *     s{rank-1} = 1
 *     s{i}      = s{i+1} * shape[i+1]
 *
 * Strided view example (slice the C axis of an NCHW tensor):
 *     shape   = [N, C', H, W]
 *     strides = [s0, s1, s2, s3]    s1 may exceed H*W to skip slabs
 * @endcode
 *
 * @par Wire format
 * @c Tensor is POD; @c memcpy serialises the struct plus the element bytes.
 * The @c sizeof value is locked by @c static_assert and forms a permanent
 * contract: @c vlink::zerocopy::* containers have NO forward and NO backward
 * binary compatibility, and every field, including reserved bytes, is
 * wire-locked.
 * @code
 * static_assert(sizeof(Tensor) == 248, "Sizeof must be 248 bytes.");
 * @endcode
 *
 * @par Memory layout
 * @code
 * Offset  Size  Field
 * ------  ----  ------------------------------------
 *      0    40  Header   header
 *     40     8  uint8_t* data_
 *     48     8  size_t   size_
 *     56     8  uint64_t update_time_ns_
 *     64     8  uint64_t num_elements_
 *     72    32  char     name_[32]
 *    104    32  char     model_id_[32]
 *    136    16  char     layout_[16]
 *    152    32  uint32_t shape_[kMaxRank]
 *    184    32  uint32_t strides_[kMaxRank]
 *    216     4  uint32_t channel_
 *    220     4  uint32_t freq_
 *    224     4  uint32_t batch_size_
 *    228     4  float    quant_scale_
 *    232     4  int32_t  quant_zero_point_
 *    236     1  DataType dtype_
 *    237     1  uint8_t  rank_
 *    238     1  Device   device_
 *    239     1  bool     is_owner_
 *    240     1  uint8_t  element_size_
 *    241     1  uint8_t  reserved_buf_
 *    242     2  uint16_t reserved_buf2_
 *    244     4  uint32_t reserved_buf3_
 * ------  ----  ------------------------------------
 *  Total   248  bytes (alignas 8)
 *
 * Wire envelope:
 * [ magic_begin (4) | version (4) | Tensor struct (248) | element bytes (num_elements*element_size) | magic_end (4) ]
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
 * vlink::zerocopy::Tensor t;
 * t.set_name("image");
 * t.set_layout("NCHW");
 * t.set_dtype(vlink::zerocopy::Tensor::kFloat32);
 * uint32_t shape[] = {1, 3, 224, 224};
 * t.set_shape(shape, 4);
 * t.create(t.num_elements() * sizeof(float));
 *
 * vlink::Bytes wire;
 * t >> wire;
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
 * @struct Tensor
 * @brief 248-byte POD container holding a dense N-D tensor plus shape / dtype / device metadata.
 *
 * @details
 * The struct size is locked at 248 bytes on 64-bit targets via
 * @c static_assert.  Up to @c kMaxRank dimensions are supported; the shape
 * and stride arrays are inline so that strided slices round-trip without
 * extra metadata.
 */
struct VLINK_EXPORT_AND_ALIGNED(8) Tensor final {
  /**
   * @brief Maximum number of dimensions a single @c Tensor may carry.
   */
  static constexpr uint8_t kMaxRank{8};

  /**
   * @brief Scalar element type tag (drives @c element_size()).
   */
  enum DataType : uint8_t {
    kDataUnknown = 0,  ///< Uninitialised / unspecified element type.
    kBool = 1,         ///< @c bool (1 byte).
    kInt8 = 2,         ///< @c int8_t (1 byte).
    kUint8 = 3,        ///< @c uint8_t (1 byte).
    kInt16 = 4,        ///< @c int16_t (2 bytes).
    kUint16 = 5,       ///< @c uint16_t (2 bytes).
    kInt32 = 6,        ///< @c int32_t (4 bytes).
    kUint32 = 7,       ///< @c uint32_t (4 bytes).
    kInt64 = 8,        ///< @c int64_t (8 bytes).
    kUint64 = 9,       ///< @c uint64_t (8 bytes).
    kFloat16 = 10,     ///< IEEE-754 half precision (2 bytes).
    kBfloat16 = 11,    ///< Brain float (2 bytes).
    kFloat32 = 12,     ///< IEEE-754 single precision (4 bytes).
    kFloat64 = 13,     ///< IEEE-754 double precision (8 bytes).
  };

  /**
   * @brief Device hint indicating where the data buffer physically lives.
   */
  enum Device : uint8_t {
    kDeviceCpu = 0,  ///< Host / CPU memory.
    kDeviceGpu = 1,  ///< Discrete or integrated GPU memory.
    kDeviceNpu = 2,  ///< Neural processing unit (e.g. automotive NPU).
    kDeviceDsp = 3,  ///< Digital signal processor.
  };

  /**
   * @brief Default-constructs an empty tensor and asserts the 248-byte contract.
   */
  Tensor() noexcept;

  /**
   * @brief Frees the owned data buffer when @c is_owner() is @c true.
   */
  ~Tensor() noexcept;

  /**
   * @brief Deep-copies @p target into a freshly allocated tensor.
   *
   * @param target Source tensor to clone.
   */
  Tensor(const Tensor& target) noexcept;

  /**
   * @brief Steals @p target's allocation and metadata; @p target ends empty.
   *
   * @param target Source tensor moved from.
   */
  Tensor(Tensor&& target) noexcept;

  /**
   * @brief Deep-copy-assigns @p target; self-assignment is a no-op.
   *
   * @param target Source tensor to clone.
   * @return Reference to @c *this.
   */
  Tensor& operator=(const Tensor& target) noexcept;

  /**
   * @brief Move-assigns @p target; self-assignment is a no-op.
   *
   * @param target Source tensor moved from.
   * @return Reference to @c *this.
   */
  Tensor& operator=(Tensor&& target) noexcept;

  /**
   * @brief Deserialises a @c Tensor from @p bytes with zero-copy borrowing semantics.
   *
   * @details
   * Two fields are sanitised after the struct snapshot is restored:
   * @c rank_ is clamped to @c kMaxRank, and @c element_size_ is re-derived
   * from @c dtype_ to stay consistent regardless of producer-side mistakes.
   *
   * @param bytes Wire buffer previously produced by @c operator>>.
   * @return @c true on success.
   */
  bool operator<<(const Bytes& bytes) noexcept;

  /**
   * @brief Serialises the struct snapshot plus element bytes into @p bytes.
   *
   * @param bytes Output buffer; resized automatically when too small.
   * @return Always @c true.
   */
  bool operator>>(Bytes& bytes) const noexcept;

  /**
   * @brief Validates that @p bytes carries a well-formed @c Tensor envelope.
   *
   * @param bytes Wire buffer to inspect.
   * @return @c true when both magic sentinels match and the minimum size holds.
   */
  [[nodiscard]] static bool check_valid(const Bytes& bytes) noexcept;

  /**
   * @brief Total bytes that @c operator>> would write for this tensor.
   *
   * @return @c sizeof(magic_begin) + @c sizeof(version) + @c sizeof(Tensor) + @c size() + @c sizeof(magic_end).
   */
  [[nodiscard]] size_t get_serialized_size() const noexcept;

  /**
   * @brief Whether the data buffer pointer is non-null and the byte size is positive.
   *
   * @return @c true when the tensor holds usable data.
   */
  [[nodiscard]] bool is_valid() const noexcept;

  /**
   * @brief Borrows @p target's data buffer without copying.
   *
   * @param target Source tensor whose buffer must outlive @c *this.
   * @return @c false on self-borrow, otherwise @c true.
   */
  bool shallow_copy(const Tensor& target) noexcept;

  /**
   * @brief Allocates (or reuses) an owned buffer and copies @p target's elements.
   *
   * @param target Source tensor to clone.
   * @return @c false on self-copy, otherwise @c true.
   */
  bool deep_copy(const Tensor& target) noexcept;

  /**
   * @brief Transfers ownership from @p target; @p target ends empty.
   *
   * @param target Source tensor moved from.
   * @return @c false on self-move, otherwise @c true.
   */
  bool move_copy(Tensor& target) noexcept;

  /**
   * @brief Allocates an uninitialised owned data buffer of @p size bytes.
   *
   * @details
   * The caller is responsible for keeping @p size consistent with
   * @c num_elements() * @c element_size().
   *
   * @param size Byte count; must be non-zero.
   * @return @c false when @p size is zero, otherwise @c true.
   */
  bool create(size_t size) noexcept;

  /**
   * @brief Releases the owned buffer (if any) and resets metadata except reserved fields.
   */
  void clear() noexcept;

  /**
   * @brief Borrows an externally owned data buffer without copying.
   *
   * @param data Non-null source pointer that must outlive @c *this.
   * @param size Buffer length in bytes; must be non-zero.
   * @return @c false on invalid arguments or unchanged pointer, otherwise @c true.
   */
  bool shallow_copy(uint8_t* data, size_t size) noexcept;

  /**
   * @brief Copies @p size bytes from @p data into an owned buffer.
   *
   * @param data Non-null source pointer.
   * @param size Number of bytes to copy; must be non-zero.
   * @return @c false on invalid arguments or aliasing, otherwise @c true.
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
   * @brief Producer-side tensor timestamp in nanoseconds.
   *
   * @return Stored value.
   */
  [[nodiscard]] uint64_t update_time_ns() const noexcept;

  /**
   * @brief Cached total element count (product of @c shape() dimensions).
   *
   * @return Stored element count.
   */
  [[nodiscard]] uint64_t num_elements() const noexcept;

  /**
   * @brief Tensor name (e.g. @c "image", @c "hidden_state").
   *
   * @return Non-owning view into the embedded buffer.
   */
  [[nodiscard]] std::string_view name() const noexcept;

  /**
   * @brief Source model identifier.
   *
   * @return Non-owning view into the embedded buffer.
   */
  [[nodiscard]] std::string_view model_id() const noexcept;

  /**
   * @brief Layout label (e.g. @c "NCHW", @c "NHWC", @c "BLC").
   *
   * @return Non-owning view into the embedded buffer.
   */
  [[nodiscard]] std::string_view layout() const noexcept;

  /**
   * @brief Read-only pointer to the @c kMaxRank-sized shape array.
   *
   * @return Pointer into the embedded array; never @c nullptr.
   */
  [[nodiscard]] const uint32_t* shape() const noexcept;

  /**
   * @brief Shape value at dimension @p dim, or 0 if @p dim is out of range.
   *
   * @param dim Dimension index.
   * @return Shape entry.
   */
  [[nodiscard]] uint32_t shape_at(uint8_t dim) const noexcept;

  /**
   * @brief Read-only pointer to the @c kMaxRank-sized stride array (in elements, not bytes).
   *
   * @return Pointer into the embedded array; never @c nullptr.
   */
  [[nodiscard]] const uint32_t* strides() const noexcept;

  /**
   * @brief Stride value at dimension @p dim, or 0 if @p dim is out of range.
   *
   * @param dim Dimension index.
   * @return Stride entry.
   */
  [[nodiscard]] uint32_t stride_at(uint8_t dim) const noexcept;

  /**
   * @brief Sensor / producer / output-port channel identifier.
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
   * @brief Cached batch size (typically @c shape_at(0)).
   *
   * @return Stored batch size.
   */
  [[nodiscard]] uint32_t batch_size() const noexcept;

  /**
   * @brief INT8 quantisation scale (0 when unused).
   *
   * @return Stored scale.
   */
  [[nodiscard]] float quant_scale() const noexcept;

  /**
   * @brief INT8 quantisation zero point (0 when unused).
   *
   * @return Stored zero point.
   */
  [[nodiscard]] int32_t quant_zero_point() const noexcept;

  /**
   * @brief Scalar element type tag.
   *
   * @return @c DataType enum value.
   */
  [[nodiscard]] DataType dtype() const noexcept;

  /**
   * @brief Actual tensor rank in the range 1..@c kMaxRank.
   *
   * @return Stored rank.
   */
  [[nodiscard]] uint8_t rank() const noexcept;

  /**
   * @brief Device hint indicating where the data buffer lives.
   *
   * @return @c Device enum value.
   */
  [[nodiscard]] Device device() const noexcept;

  /**
   * @brief Cached byte size of one element derived from @c dtype().
   *
   * @return Element size in bytes.
   */
  [[nodiscard]] uint8_t element_size() const noexcept;

  /**
   * @brief Read-only pointer to the element bytes.
   *
   * @return Pointer to payload start.
   */
  [[nodiscard]] const uint8_t* data() const noexcept;

  /**
   * @brief Element buffer size in bytes.
   *
   * @return Byte count.
   */
  [[nodiscard]] size_t size() const noexcept;

  /**
   * @brief Whether this tensor owns its data buffer.
   *
   * @return @c true when the destructor would free the buffer.
   */
  [[nodiscard]] bool is_owner() const noexcept;

  /**
   * @brief Stores the tensor timestamp.
   *
   * @param update_time_ns Timestamp in nanoseconds.
   */
  void set_update_time_ns(uint64_t update_time_ns) noexcept;

  /**
   * @brief Stores the tensor name (truncated to @c sizeof(name) - 1 bytes).
   *
   * @param name Tensor name.
   */
  void set_name(std::string_view name) noexcept;

  /**
   * @brief Stores the source model identifier (truncated to @c sizeof(model_id) - 1 bytes).
   *
   * @param model_id Model identifier.
   */
  void set_model_id(std::string_view model_id) noexcept;

  /**
   * @brief Stores the layout label (truncated to @c sizeof(layout) - 1 bytes).
   *
   * @param layout Layout descriptor.
   */
  void set_layout(std::string_view layout) noexcept;

  /**
   * @brief Stores the full shape array and recomputes row-major strides plus @c num_elements.
   *
   * @details
   * @p rank is clamped to @c kMaxRank.  Strides are derived assuming a
   * contiguous row-major layout (last dimension changes fastest).
   * @c batch_size is cached from @p shape[0] when @p rank > 0.
   *
   * @param shape Pointer to a shape array of length @p rank.
   * @param rank Number of valid dimensions.
   */
  void set_shape(const uint32_t* shape, uint8_t rank) noexcept;

  /**
   * @brief Stores a single shape entry without recomputing strides.
   *
   * @param dim Dimension index.
   * @param value Shape value.
   */
  void set_shape_at(uint8_t dim, uint32_t value) noexcept;

  /**
   * @brief Stores a single stride entry.
   *
   * @param dim Dimension index.
   * @param value Stride value (in elements, not bytes).
   */
  void set_stride_at(uint8_t dim, uint32_t value) noexcept;

  /**
   * @brief Stores the sensor / producer / output-port channel identifier.
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
   * @brief Stores the cached batch size.
   *
   * @param batch_size Stored value.
   */
  void set_batch_size(uint32_t batch_size) noexcept;

  /**
   * @brief Stores the INT8 quantisation scale.
   *
   * @param quant_scale Stored value.
   */
  void set_quant_scale(float quant_scale) noexcept;

  /**
   * @brief Stores the INT8 quantisation zero point.
   *
   * @param quant_zero_point Stored value.
   */
  void set_quant_zero_point(int32_t quant_zero_point) noexcept;

  /**
   * @brief Stores the scalar element type tag and caches the derived element size.
   *
   * @param dtype @c DataType enum value.
   */
  void set_dtype(DataType dtype) noexcept;

  /**
   * @brief Stores the device hint.
   *
   * @param device @c Device enum value.
   */
  void set_device(Device device) noexcept;

  /**
   * @brief Returns the byte size of one element of @p dtype.
   *
   * @param dtype Element type tag.
   * @return Element size in bytes (0 for @c kDataUnknown).
   */
  [[nodiscard]] static uint8_t element_size_of(DataType dtype) noexcept;

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

  Header header;  ///< Sequencing and timestamp metadata prefix.

  static constexpr bool kZerocopyTypes{true};  ///< Marker probed by the VLink type-trait machinery.

 private:
  uint8_t* data_{nullptr};
  size_t size_{0};
  uint64_t update_time_ns_{0};
  uint64_t num_elements_{0};
  char name_[32]{0};
  char model_id_[32]{0};
  char layout_[16]{0};
  uint32_t shape_[kMaxRank]{0};
  uint32_t strides_[kMaxRank]{0};
  uint32_t channel_{0};
  uint32_t freq_{0};
  uint32_t batch_size_{0};
  float quant_scale_{0};
  int32_t quant_zero_point_{0};
  DataType dtype_{kDataUnknown};
  uint8_t rank_{0};
  Device device_{kDeviceCpu};
  bool is_owner_{false};
  uint8_t element_size_{0};
  uint8_t reserved_buf_{0};
  uint16_t reserved_buf2_{0};
  uint32_t reserved_buf3_{0};

  static constexpr uint32_t kMagicNumberBegin{0x98B7F19A};
  static constexpr uint32_t kMagicNumberEnd{0x98B7F19F};
};

}  // namespace zerocopy

}  // namespace vlink
