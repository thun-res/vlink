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
 * @file camera_frame.h
 * @brief Zero-copy container for a single image / video frame plus pixel-format metadata.
 *
 * @details
 * @c CameraFrame is the canonical conduit for camera capture, ISP outputs, and
 * compressed video streams in the VLink autonomous-driving stack.  One frame
 * carries pixel data (raw or codec-encoded), resolution, pixel format, camera
 * channel id, capture frequency, video stream-frame type (I/P/B), plus a
 * 40-byte @c Header for sequencing and dual-timestamp latency measurement.
 *
 * @par Pixel formats
 * | Enum                    | Family            | Description                            |
 * | ----------------------- | ----------------- | -------------------------------------- |
 * | @c kFormatYuv420        | Planar YUV        | 4:2:0 (I420)                           |
 * | @c kFormatYuv422        | Planar YUV        | 4:2:2                                  |
 * | @c kFormatYuv444        | Planar YUV        | 4:4:4                                  |
 * | @c kFormatNv12          | Semi-planar YUV   | Y plane + interleaved UV (4:2:0)       |
 * | @c kFormatNv21          | Semi-planar YUV   | Y plane + interleaved VU (4:2:0)       |
 * | @c kFormatYuyv          | Packed YUV        | YUYV 4:2:2                             |
 * | @c kFormatYvyu          | Packed YUV        | YVYU 4:2:2                             |
 * | @c kFormatUyvy          | Packed YUV        | UYVY 4:2:2                             |
 * | @c kFormatVyuy          | Packed YUV        | VYUY 4:2:2                             |
 * | @c kFormatBgr888Packed  | Packed RGB        | 24-bit BGR, 3 bytes per pixel          |
 * | @c kFormatRgb888Packed  | Packed RGB        | 24-bit RGB, 3 bytes per pixel          |
 * | @c kFormatRgb888Planar  | Planar RGB        | Separate R, G, B planes                |
 * | @c kFormatMono8, @c kFormatMono16 | Grayscale | 8/16-bit mono image                    |
 * | @c kFormatRgba8888Packed, @c kFormatBgra8888Packed | Packed RGB | 32-bit RGBA/BGRA |
 * | @c kFormatUint8C1 .. @c kFormatFloat64C4 | OpenCV/ROS style | Generic numeric images |
 * | @c kFormatBayerRggb8 .. @c kFormatBayerGrbg16 | Bayer | RGGB/BGGR/GBRG/GRBG, 8/16-bit |
 * | @c kFormatJpeg, @c kFormatPng, @c kFormatMjpeg, @c kFormatWebp | Compressed image | Still images |
 * | @c kFormatH264, @c kFormatH265, @c kFormatH266, @c kFormatAv1 | Compressed video | Video frames |
 *
 * @par Image buffer layout
 * @code
 * Planar YUV 4:2:0 (I420):
 *     +---------------------------+
 *     | Y plane  (width * height) |
 *     +---------------------------+
 *     | U plane  (width/2 * h/2)  |
 *     +---------------------------+
 *     | V plane  (width/2 * h/2)  |
 *     +---------------------------+
 *
 * Semi-planar NV12/NV21:
 *     +---------------------------+
 *     | Y plane   (width * height)|
 *     +---------------------------+
 *     | UV or VU  (width * h / 2) |
 *     +---------------------------+
 *
 * Packed RGB888:
 *     [ R G B | R G B | R G B | ... ]   stride = width * 3
 *
 * Compressed JPEG/PNG/WebP/MJPEG/H.26x/AV1:
 *     opaque codec bitstream of size_ bytes
 * @endcode
 *
 * @par Wire format
 * @c CameraFrame is POD; the canonical serialiser is @c memcpy.  The @c sizeof
 * value is locked by @c static_assert and forms a permanent contract:
 * @c vlink::zerocopy::* containers offer NO forward and NO backward binary
 * compatibility -- every field, including reserved bytes, is wire-locked.
 * @code
 * static_assert(sizeof(CameraFrame) == 80, "Sizeof must be 80 bytes.");
 * @endcode
 *
 * @par Memory layout
 * @code
 * Offset  Size  Field
 * ------  ----  ----------------------
 *      0    40  Header   header
 *     40     8  uint8_t* data_
 *     48     8  size_t   size_
 *     56     4  uint32_t channel_
 *     60     4  uint32_t width_
 *     64     4  uint32_t height_
 *     68     4  uint32_t freq_
 *     72     1  Format   format_
 *     73     1  Stream   stream_
 *     74     1  bool     is_owner_
 *     75     1  (padding)
 *     76     4  uint32_t reserved_buf_
 * ------  ----  ----------------------
 *  Total    80  bytes (alignas 8)
 *
 * Wire envelope:
 * [ magic_begin (4) | version (4) | CameraFrame struct (80) | pixel bytes (size_) | magic_end (4) ]
 * @endcode
 *
 * @par Reserved bytes
 * @c reserved_buf_ is exposed through @c get_reserved() and persists through both
 * @c clear() and the copy / move helpers so application bridges can stash
 * minor identifiers.  It is part of the wire contract and MUST NOT be
 * redefined: future library revisions may bind the slot to a real field.
 *
 * @par Example
 * @code
 * vlink::zerocopy::CameraFrame frame;
 * frame.set_width(1920);
 * frame.set_height(1080);
 * frame.set_format(vlink::zerocopy::CameraFrame::kFormatNv12);
 * frame.create(1920 * 1080 * 3 / 2);
 *
 * // Intra-process zero-copy publishing (no serialisation hop):
 * VLINK_INTRA_DATA_DECLARE(vlink::zerocopy::CameraFrame, "camera/front");
 * vlink::Bytes wire;
 * frame >> wire;
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
 * @struct CameraFrame
 * @brief 80-byte POD container holding one camera / video frame plus image-format metadata.
 *
 * @details
 * The struct size is locked at 80 bytes on 64-bit targets via @c static_assert;
 * 32-bit toolchains emit a build-time warning.  The struct embeds a @c Header
 * prefix and exposes resolution, pixel format, video stream-type, capture
 * frequency, and camera channel id.
 */
struct VLINK_EXPORT_AND_ALIGNED(8) CameraFrame final {
  /**
   * @brief Pixel / codec encoding of the image payload.
   */
  enum Format : uint8_t {
    kFormatUnknown = 0,  ///< Uninitialised / unspecified format.

    kFormatYuv420 = 1,         ///< Planar YUV 4:2:0 (I420).
    kFormatYuv422 = 2,         ///< Planar YUV 4:2:2.
    kFormatYuv444 = 3,         ///< Planar YUV 4:4:4.
    kFormatNv12 = 4,           ///< Semi-planar Y + interleaved UV (4:2:0).
    kFormatNv21 = 5,           ///< Semi-planar Y + interleaved VU (4:2:0).
    kFormatYuyv = 6,           ///< Packed YUYV 4:2:2.
    kFormatYvyu = 7,           ///< Packed YVYU 4:2:2.
    kFormatUyvy = 8,           ///< Packed UYVY 4:2:2.
    kFormatVyuy = 9,           ///< Packed VYUY 4:2:2.
    kFormatBgr888Packed = 10,  ///< Packed 24-bit BGR (3 bytes per pixel).
    kFormatRgb888Packed = 11,  ///< Packed 24-bit RGB (3 bytes per pixel).
    kFormatRgb888Planar = 12,  ///< Planar 24-bit RGB (separate R, G, B planes).

    kFormatMono8 = 13,           ///< 8-bit grayscale / luminance.
    kFormatMono16 = 14,          ///< 16-bit grayscale / luminance.
    kFormatRgba8888Packed = 15,  ///< Packed 32-bit RGBA (4 bytes per pixel).
    kFormatBgra8888Packed = 16,  ///< Packed 32-bit BGRA (4 bytes per pixel).

    kFormatUint8C1 = 20,    ///< Generic unsigned 8-bit, 1 channel.
    kFormatUint8C2 = 21,    ///< Generic unsigned 8-bit, 2 channels.
    kFormatUint8C3 = 22,    ///< Generic unsigned 8-bit, 3 channels.
    kFormatUint8C4 = 23,    ///< Generic unsigned 8-bit, 4 channels.
    kFormatInt8C1 = 24,     ///< Generic signed 8-bit, 1 channel.
    kFormatInt8C2 = 25,     ///< Generic signed 8-bit, 2 channels.
    kFormatInt8C3 = 26,     ///< Generic signed 8-bit, 3 channels.
    kFormatInt8C4 = 27,     ///< Generic signed 8-bit, 4 channels.
    kFormatUint16C1 = 28,   ///< Generic unsigned 16-bit, 1 channel.
    kFormatUint16C2 = 29,   ///< Generic unsigned 16-bit, 2 channels.
    kFormatUint16C3 = 30,   ///< Generic unsigned 16-bit, 3 channels.
    kFormatUint16C4 = 31,   ///< Generic unsigned 16-bit, 4 channels.
    kFormatInt16C1 = 32,    ///< Generic signed 16-bit, 1 channel.
    kFormatInt16C2 = 33,    ///< Generic signed 16-bit, 2 channels.
    kFormatInt16C3 = 34,    ///< Generic signed 16-bit, 3 channels.
    kFormatInt16C4 = 35,    ///< Generic signed 16-bit, 4 channels.
    kFormatInt32C1 = 36,    ///< Generic signed 32-bit, 1 channel.
    kFormatInt32C2 = 37,    ///< Generic signed 32-bit, 2 channels.
    kFormatInt32C3 = 38,    ///< Generic signed 32-bit, 3 channels.
    kFormatInt32C4 = 39,    ///< Generic signed 32-bit, 4 channels.
    kFormatFloat32C1 = 40,  ///< Generic 32-bit float, 1 channel.
    kFormatFloat32C2 = 41,  ///< Generic 32-bit float, 2 channels.
    kFormatFloat32C3 = 42,  ///< Generic 32-bit float, 3 channels.
    kFormatFloat32C4 = 43,  ///< Generic 32-bit float, 4 channels.
    kFormatFloat64C1 = 44,  ///< Generic 64-bit float, 1 channel.
    kFormatFloat64C2 = 45,  ///< Generic 64-bit float, 2 channels.
    kFormatFloat64C3 = 46,  ///< Generic 64-bit float, 3 channels.
    kFormatFloat64C4 = 47,  ///< Generic 64-bit float, 4 channels.

    kFormatBayerRggb8 = 60,   ///< Bayer RGGB, 8-bit samples.
    kFormatBayerBggr8 = 61,   ///< Bayer BGGR, 8-bit samples.
    kFormatBayerGbrg8 = 62,   ///< Bayer GBRG, 8-bit samples.
    kFormatBayerGrbg8 = 63,   ///< Bayer GRBG, 8-bit samples.
    kFormatBayerRggb16 = 64,  ///< Bayer RGGB, 16-bit samples.
    kFormatBayerBggr16 = 65,  ///< Bayer BGGR, 16-bit samples.
    kFormatBayerGbrg16 = 66,  ///< Bayer GBRG, 16-bit samples.
    kFormatBayerGrbg16 = 67,  ///< Bayer GRBG, 16-bit samples.

    kFormatJpeg = 101,   ///< JPEG bitstream.
    kFormatH264 = 102,   ///< H.264 / AVC frame.
    kFormatH265 = 103,   ///< H.265 / HEVC frame.
    kFormatPng = 104,    ///< PNG bitstream.
    kFormatMjpeg = 105,  ///< Motion-JPEG frame.
    kFormatH266 = 106,   ///< H.266 / VVC frame.
    kFormatAv1 = 107,    ///< AV1 frame.
    kFormatWebp = 108,   ///< WebP bitstream.
  };

  /**
   * @brief Stream-frame type for compressed video payloads.
   */
  enum Stream : uint8_t {
    kStreamUnknown = 0,  ///< Uninitialised / unspecified frame type.
    kStreamI,            ///< Intra-coded key frame.
    kStreamP,            ///< Predicted frame referencing earlier frames.
    kStreamB,            ///< Bi-directionally predicted frame.
  };

  /**
   * @brief Default-constructs an empty frame and asserts the 80-byte contract.
   */
  CameraFrame() noexcept;

  /**
   * @brief Frees the owned pixel buffer when @c is_owner() is @c true.
   */
  ~CameraFrame() noexcept;

  /**
   * @brief Deep-copies @p target into a freshly allocated frame.
   *
   * @param target Source frame to clone.
   */
  CameraFrame(const CameraFrame& target) noexcept;

  /**
   * @brief Steals @p target's allocation and metadata; @p target ends empty.
   *
   * @param target Source frame moved from.
   */
  CameraFrame(CameraFrame&& target) noexcept;

  /**
   * @brief Deep-copy-assigns @p target; self-assignment is a no-op.
   *
   * @param target Source frame to clone.
   * @return Reference to @c *this.
   */
  CameraFrame& operator=(const CameraFrame& target) noexcept;

  /**
   * @brief Move-assigns @p target; self-assignment is a no-op.
   *
   * @param target Source frame moved from.
   * @return Reference to @c *this.
   */
  CameraFrame& operator=(CameraFrame&& target) noexcept;

  /**
   * @brief Deserialises a @c CameraFrame from @p bytes with zero-copy borrowing semantics.
   *
   * @details
   * Validates the magic-number envelope and total length, then borrows the
   * pixel pointer from @p bytes.  Callers must keep @p bytes alive for as
   * long as this @c CameraFrame is in use.
   *
   * @param bytes Wire buffer previously produced by @c operator>>.
   * @return @c true on success; @c false on magic mismatch or size mismatch.
   */
  bool operator<<(const Bytes& bytes) noexcept;

  /**
   * @brief Serialises the struct snapshot plus pixel bytes into @p bytes.
   *
   * @param bytes Output buffer; resized automatically when its size differs from the serialized size.
   * @return Always @c true.
   */
  bool operator>>(Bytes& bytes) const noexcept;

  /**
   * @brief Validates that @p bytes carries a well-formed @c CameraFrame envelope.
   *
   * @param bytes Wire buffer to inspect.
   * @return @c true when both magic sentinels match and the minimum size holds.
   */
  [[nodiscard]] static bool check_valid(const Bytes& bytes) noexcept;

  /**
   * @brief Maps common ROS/OpenCV/codec encoding names to @c Format.
   *
   * @details Matching is case-insensitive.  Examples include @c rgb8, @c bgr8,
   * @c mono16, @c 32FC1, @c bayer_rggb8, @c jpeg and @c h264.
   */
  [[nodiscard]] static Format format_from_encoding(std::string_view encoding) noexcept;

  /**
   * @brief Returns the canonical ROS/OpenCV/codec encoding name for @p format.
   */
  [[nodiscard]] static std::string_view encoding_from_format(Format format) noexcept;

  /**
   * @brief Total bytes that @c operator>> would write for this frame.
   *
   * @return @c sizeof(magic_begin) + @c sizeof(version) + @c sizeof(CameraFrame) + @c size() + @c sizeof(magic_end).
   */
  [[nodiscard]] size_t get_serialized_size() const noexcept;

  /**
   * @brief Whether the pixel buffer pointer is non-null and its size is positive.
   *
   * @return @c true when the frame holds usable pixel data.
   */
  [[nodiscard]] bool is_valid() const noexcept;

  /**
   * @brief Borrows @p target's pixel buffer without copying.
   *
   * @param target Source frame whose buffer must outlive @c *this.
   * @return @c false on self-borrow, otherwise @c true.
   */
  bool shallow_copy(const CameraFrame& target) noexcept;

  /**
   * @brief Allocates (or reuses) an owned buffer and copies @p target's pixels.
   *
   * @param target Source frame to clone.
   * @return @c false on self-copy, otherwise @c true.
   */
  bool deep_copy(const CameraFrame& target) noexcept;

  /**
   * @brief Transfers ownership from @p target; @p target ends empty.
   *
   * @param target Source frame moved from.
   * @return @c false on self-move, otherwise @c true.
   */
  bool move_copy(CameraFrame& target) noexcept;

  /**
   * @brief Allocates an uninitialised owned pixel buffer of @p size bytes.
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
   * @brief Borrows an externally owned pixel buffer without copying.
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
   * @brief Camera channel / sensor index.
   *
   * @return Stored channel id.
   */
  [[nodiscard]] uint32_t channel() const noexcept;

  /**
   * @brief Image width in pixels.
   *
   * @return Stored width.
   */
  [[nodiscard]] uint32_t width() const noexcept;

  /**
   * @brief Image height in pixels.
   *
   * @return Stored height.
   */
  [[nodiscard]] uint32_t height() const noexcept;

  /**
   * @brief Capture frequency in frames per second.
   *
   * @return Stored frequency.
   */
  [[nodiscard]] uint32_t freq() const noexcept;

  /**
   * @brief Pixel / codec encoding tag.
   *
   * @return @c Format enum value.
   */
  [[nodiscard]] Format format() const noexcept;

  /**
   * @brief Video stream-frame type tag.
   *
   * @return @c Stream enum value.
   */
  [[nodiscard]] Stream stream() const noexcept;

  /**
   * @brief Read-only pointer to the pixel bytes.
   *
   * @return Pointer to payload start; may be non-null with @c size() == 0 for empty deserialised frames.
   */
  [[nodiscard]] const uint8_t* data() const noexcept;

  /**
   * @brief Pixel buffer size in bytes.
   *
   * @return Byte count, or 0 when empty.
   */
  [[nodiscard]] size_t size() const noexcept;

  /**
   * @brief Whether this frame owns its pixel buffer.
   *
   * @return @c true when the destructor would free the buffer.
   */
  [[nodiscard]] bool is_owner() const noexcept;

  /**
   * @brief Stores the camera channel / sensor index.
   *
   * @param channel Channel id.
   */
  void set_channel(uint32_t channel) noexcept;

  /**
   * @brief Stores the image width in pixels.
   *
   * @param width Pixel width.
   */
  void set_width(uint32_t width) noexcept;

  /**
   * @brief Stores the image height in pixels.
   *
   * @param height Pixel height.
   */
  void set_height(uint32_t height) noexcept;

  /**
   * @brief Stores the capture frequency.
   *
   * @param freq Capture rate in Hz.
   */
  void set_freq(uint32_t freq) noexcept;

  /**
   * @brief Stores the pixel / codec encoding tag.
   *
   * @param format @c Format enum value.
   */
  void set_format(Format format) noexcept;

  /**
   * @brief Stores the video stream-frame type tag.
   *
   * @param stream @c Stream enum value.
   */
  void set_stream(Stream stream) noexcept;

  /**
   * @brief Mutable accessor for the 32-bit reserved slot in the wire format.
   *
   * @return Reference to @c reserved_buf_.
   */
  uint32_t& get_reserved() noexcept { return reserved_buf_; }

  Header header;  ///< Sequencing and timestamp metadata prefix.

  static constexpr bool kZerocopyTypes{true};  ///< Marker probed by the VLink type-trait machinery.

 private:
  uint8_t* data_{nullptr};
  size_t size_{0};
  uint32_t channel_{0};
  uint32_t width_{0};
  uint32_t height_{0};
  uint32_t freq_{0};
  Format format_{kFormatUnknown};
  Stream stream_{kStreamUnknown};
  bool is_owner_{false};
  uint32_t reserved_buf_{0};

  static constexpr uint32_t kMagicNumberBegin{0x98B7F15A};
  static constexpr uint32_t kMagicNumberEnd{0x98B7F15F};
};

}  // namespace zerocopy

}  // namespace vlink
