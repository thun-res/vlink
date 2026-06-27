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
 * @file audio_frame.h
 * @brief Zero-copy audio frame container carrying one PCM or codec-encoded packet.
 *
 * @details
 * @c AudioFrame is the canonical conduit for in-vehicle and embedded audio
 * pipelines: microphone capture, in-cabin voice command, TTS playback, alarm
 * tones, and intercom routing.  Each frame carries one acquisition window of
 * audio together with sample-format metadata (sample rate, channels, bit
 * depth, layout, codec hint, language tag), a capture timestamp, the
 * advertised frame duration, plus a 40-byte @c Header for sequencing and
 * dual-timestamp latency measurement.
 *
 * @par Audio formats
 * | Enum             | Encoding                 | Typical bit depth | Compression |
 * | ---------------- | ------------------------ | ----------------- | ----------- |
 * | @c kFormatPcmS16 | Signed linear PCM        | 16                | None        |
 * | @c kFormatPcmS24 | Signed linear PCM packed | 24                | None        |
 * | @c kFormatPcmS32 | Signed linear PCM        | 32                | None        |
 * | @c kFormatPcmF32 | IEEE-754 float PCM       | 32                | None        |
 * | @c kFormatPcmU8  | Unsigned linear PCM      | 8                 | None        |
 * | @c kFormatOpus   | Opus                     | N/A               | Lossy       |
 * | @c kFormatAac    | AAC                      | N/A               | Lossy       |
 * | @c kFormatMp3    | MP3                      | N/A               | Lossy       |
 * | @c kFormatFlac   | FLAC                     | N/A               | Lossless    |
 *
 * Common sample rates: 8 kHz / 16 kHz / 32 kHz / 44.1 kHz / 48 kHz / 96 kHz.
 * Channel layouts: @c kLayoutInterleaved (L R L R ...) or @c kLayoutPlanar
 * (separate per-channel planes).
 *
 * @par Wire format
 * @c AudioFrame is POD; @c memcpy of the struct snapshot plus the audio buffer
 * forms the wire payload.  The @c sizeof value is locked by @c static_assert
 * and forms a permanent contract: @c vlink::zerocopy::* containers have NO
 * forward and NO backward binary compatibility -- every field, including
 * reserved bytes, is part of the wire contract.
 * @code
 * static_assert(sizeof(AudioFrame) == 128, "Sizeof must be 128 bytes.");
 * @endcode
 *
 * @par Memory layout
 * @code
 * Offset  Size  Field
 * ------  ----  --------------------------
 *      0    40  Header   header
 *     40     8  uint8_t* data_
 *     48     8  size_t   size_
 *     56     8  uint64_t update_time_ns_
 *     64     8  uint64_t duration_ns_
 *     72    16  char     codec_[16]
 *     88     8  char     language_[8]
 *     96     4  uint32_t channel_
 *    100     4  uint32_t freq_
 *    104     4  uint32_t sample_rate_
 *    108     4  uint32_t num_samples_
 *    112     4  uint32_t bitrate_
 *    116     2  uint16_t num_channels_
 *    118     2  uint16_t bit_depth_
 *    120     1  Format   format_
 *    121     1  Layout   layout_
 *    122     1  bool     is_owner_
 *    123     1  uint8_t  reserved_buf_
 *    124     4  uint32_t reserved_buf2_
 * ------  ----  --------------------------
 *  Total   128  bytes (alignas 8)
 *
 * Wire envelope:
 * [ magic_begin (4) | version (4) | AudioFrame struct (128) | audio bytes (size_) | magic_end (4) ]
 * @endcode
 *
 * @par Reserved bytes
 * @c reserved_buf_ and @c reserved_buf2_ are exposed through the @c get_reserved* helpers.
 * They travel through the wire format and are deliberately preserved by
 * @c clear() and the copy/move helpers.  They MUST NOT be repurposed by
 * application code: a future library revision may bind them to real fields,
 * which would silently break peers that abused the slot.
 *
 * @par Example
 * @code
 * vlink::zerocopy::AudioFrame frame;
 * frame.set_sample_rate(48000);
 * frame.set_num_channels(2);
 * frame.set_num_samples(960);
 * frame.set_bit_depth(16);
 * frame.set_format(vlink::zerocopy::AudioFrame::kFormatPcmS16);
 * frame.set_layout(vlink::zerocopy::AudioFrame::kLayoutInterleaved);
 * frame.set_codec("PCM");
 * frame.create(960 * 2 * sizeof(int16_t));
 *
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
 * @struct AudioFrame
 * @brief 128-byte POD container holding one audio packet with full sample-format metadata.
 *
 * @details
 * The struct size is locked at 128 bytes on 64-bit targets via @c static_assert;
 * 32-bit toolchains emit a compile-time warning.  The struct embeds a @c Header
 * prefix and exposes both PCM-specific (sample rate, channels, bit depth) and
 * codec-specific (codec name, bitrate) metadata.
 */
struct VLINK_EXPORT_AND_ALIGNED(8) AudioFrame final {
  /**
   * @brief Encoded sample format of the audio payload.
   */
  enum Format : uint8_t {
    kFormatUnknown = 0,  ///< Uninitialised / unspecified format.

    kFormatPcmS16 = 1,  ///< Signed 16-bit linear PCM.
    kFormatPcmS24 = 2,  ///< Signed 24-bit linear PCM packed into 3 bytes.
    kFormatPcmS32 = 3,  ///< Signed 32-bit linear PCM.
    kFormatPcmF32 = 4,  ///< IEEE-754 single-precision linear PCM.
    kFormatPcmU8 = 5,   ///< Unsigned 8-bit linear PCM.

    kFormatOpus = 100,  ///< Opus codec payload.
    kFormatAac = 101,   ///< AAC codec payload.
    kFormatMp3 = 102,   ///< MP3 codec payload.
    kFormatFlac = 103,  ///< FLAC codec payload.
  };

  /**
   * @brief Arrangement of multi-channel samples within the payload buffer.
   */
  enum Layout : uint8_t {
    kLayoutUnknown = 0,      ///< Uninitialised / unspecified layout.
    kLayoutInterleaved = 1,  ///< Channels interleaved per-sample (L R L R ...).
    kLayoutPlanar = 2,       ///< Channels stored in consecutive contiguous planes.
  };

  /**
   * @brief Default-constructs an empty frame and asserts the 128-byte contract.
   */
  AudioFrame() noexcept;

  /**
   * @brief Frees the owned audio buffer when @c is_owner() is @c true.
   */
  ~AudioFrame() noexcept;

  /**
   * @brief Deep-copies @p target into a freshly allocated frame.
   *
   * @param target Source frame to clone.
   */
  AudioFrame(const AudioFrame& target) noexcept;

  /**
   * @brief Steals @p target's allocation and metadata; @p target ends empty.
   *
   * @param target Source frame moved from.
   */
  AudioFrame(AudioFrame&& target) noexcept;

  /**
   * @brief Deep-copy-assigns @p target; self-assignment is a no-op.
   *
   * @param target Source frame to clone.
   * @return Reference to @c *this.
   */
  AudioFrame& operator=(const AudioFrame& target) noexcept;

  /**
   * @brief Move-assigns @p target; self-assignment is a no-op.
   *
   * @param target Source frame moved from.
   * @return Reference to @c *this.
   */
  AudioFrame& operator=(AudioFrame&& target) noexcept;

  /**
   * @brief Deserialises an @c AudioFrame from @p bytes with zero-copy borrowing semantics.
   *
   * @param bytes Wire buffer previously produced by @c operator>>.
   * @return @c true on success; @c false on magic mismatch or size mismatch.
   */
  bool operator<<(const Bytes& bytes) noexcept;

  /**
   * @brief Serialises the struct snapshot plus audio bytes into @p bytes.
   *
   * @param bytes Output buffer; resized automatically when too small.
   * @return Always @c true.
   */
  bool operator>>(Bytes& bytes) const noexcept;

  /**
   * @brief Validates that @p bytes carries a well-formed @c AudioFrame envelope.
   *
   * @param bytes Wire buffer to inspect.
   * @return @c true when the magic sentinels match and the minimum length holds.
   */
  [[nodiscard]] static bool check_valid(const Bytes& bytes) noexcept;

  /**
   * @brief Total bytes that @c operator>> would write for this frame.
   *
   * @return @c sizeof(magic_begin) + @c sizeof(version) + @c sizeof(AudioFrame) + @c size() + @c sizeof(magic_end).
   */
  [[nodiscard]] size_t get_serialized_size() const noexcept;

  /**
   * @brief Whether the audio buffer pointer is non-null and its size is positive.
   *
   * @return @c true when the frame holds usable audio data.
   */
  [[nodiscard]] bool is_valid() const noexcept;

  /**
   * @brief Borrows @p target's audio buffer without copying.
   *
   * @param target Source frame whose buffer must outlive @c *this.
   * @return @c false on self-borrow, otherwise @c true.
   */
  bool shallow_copy(const AudioFrame& target) noexcept;

  /**
   * @brief Allocates (or reuses) an owned buffer and copies @p target's audio.
   *
   * @param target Source frame to clone.
   * @return @c false on self-copy, otherwise @c true.
   */
  bool deep_copy(const AudioFrame& target) noexcept;

  /**
   * @brief Transfers ownership from @p target; @p target ends empty.
   *
   * @param target Source frame moved from.
   * @return @c false on self-move, otherwise @c true.
   */
  bool move_copy(AudioFrame& target) noexcept;

  /**
   * @brief Allocates an uninitialised owned audio buffer of @p size bytes.
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
   * @brief Borrows an externally owned audio buffer without copying.
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
   * @brief Capture-side timestamp in nanoseconds.
   *
   * @return Stored value.
   */
  [[nodiscard]] uint64_t update_time_ns() const noexcept;

  /**
   * @brief Advertised frame duration in nanoseconds.
   *
   * @return Stored value.
   */
  [[nodiscard]] uint64_t duration_ns() const noexcept;

  /**
   * @brief Codec hint string (e.g. @c "PCM", @c "OPUS").
   *
   * @return Non-owning view into the embedded buffer.
   */
  [[nodiscard]] std::string_view codec() const noexcept;

  /**
   * @brief Language tag used by speech-to-text consumers (e.g. @c "en", @c "zh").
   *
   * @return Non-owning view into the embedded buffer.
   */
  [[nodiscard]] std::string_view language() const noexcept;

  /**
   * @brief Microphone / sensor channel identifier.
   *
   * @return Stored value.
   */
  [[nodiscard]] uint32_t channel() const noexcept;

  /**
   * @brief Nominal publish frequency in Hz.
   *
   * @return Stored value.
   */
  [[nodiscard]] uint32_t freq() const noexcept;

  /**
   * @brief PCM sample rate in Hz (e.g. 48 000).
   *
   * @return Stored value.
   */
  [[nodiscard]] uint32_t sample_rate() const noexcept;

  /**
   * @brief Number of samples per channel within the buffer.
   *
   * @return Stored value.
   */
  [[nodiscard]] uint32_t num_samples() const noexcept;

  /**
   * @brief Compressed bitrate in bits per second (zero for uncompressed PCM).
   *
   * @return Stored value.
   */
  [[nodiscard]] uint32_t bitrate() const noexcept;

  /**
   * @brief Channel count (1 = mono, 2 = stereo, ...).
   *
   * @return Stored value.
   */
  [[nodiscard]] uint16_t num_channels() const noexcept;

  /**
   * @brief Bit depth per PCM sample (16, 24, 32, ...).
   *
   * @return Stored value.
   */
  [[nodiscard]] uint16_t bit_depth() const noexcept;

  /**
   * @brief Encoded sample format tag.
   *
   * @return @c Format enum value.
   */
  [[nodiscard]] Format format() const noexcept;

  /**
   * @brief Multi-channel layout tag.
   *
   * @return @c Layout enum value.
   */
  [[nodiscard]] Layout layout() const noexcept;

  /**
   * @brief Read-only pointer to the audio bytes.
   *
   * @return Pointer to payload start.
   */
  [[nodiscard]] const uint8_t* data() const noexcept;

  /**
   * @brief Audio buffer size in bytes.
   *
   * @return Byte count.
   */
  [[nodiscard]] size_t size() const noexcept;

  /**
   * @brief Whether this frame owns its audio buffer.
   *
   * @return @c true when the destructor would free the buffer.
   */
  [[nodiscard]] bool is_owner() const noexcept;

  /**
   * @brief Stores the capture-side timestamp.
   *
   * @param update_time_ns Timestamp in nanoseconds.
   */
  void set_update_time_ns(uint64_t update_time_ns) noexcept;

  /**
   * @brief Stores the advertised frame duration.
   *
   * @param duration_ns Duration in nanoseconds.
   */
  void set_duration_ns(uint64_t duration_ns) noexcept;

  /**
   * @brief Stores the codec hint, truncated to @c sizeof(codec) - 1 bytes.
   *
   * @param codec Codec label.
   */
  void set_codec(std::string_view codec) noexcept;

  /**
   * @brief Stores the language tag, truncated to @c sizeof(language) - 1 bytes.
   *
   * @param language Language identifier.
   */
  void set_language(std::string_view language) noexcept;

  /**
   * @brief Stores the microphone / sensor channel identifier.
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
   * @brief Stores the PCM sample rate.
   *
   * @param sample_rate Sample rate in Hz.
   */
  void set_sample_rate(uint32_t sample_rate) noexcept;

  /**
   * @brief Stores the number of samples per channel.
   *
   * @param num_samples Sample count.
   */
  void set_num_samples(uint32_t num_samples) noexcept;

  /**
   * @brief Stores the compressed bitrate.
   *
   * @param bitrate Bitrate in bits per second.
   */
  void set_bitrate(uint32_t bitrate) noexcept;

  /**
   * @brief Stores the channel count.
   *
   * @param num_channels Channel count.
   */
  void set_num_channels(uint16_t num_channels) noexcept;

  /**
   * @brief Stores the PCM bit depth.
   *
   * @param bit_depth Bit depth.
   */
  void set_bit_depth(uint16_t bit_depth) noexcept;

  /**
   * @brief Stores the sample format tag.
   *
   * @param format @c Format enum value.
   */
  void set_format(Format format) noexcept;

  /**
   * @brief Stores the channel layout tag.
   *
   * @param layout @c Layout enum value.
   */
  void set_layout(Layout layout) noexcept;

  /**
   * @brief Mutable accessor for the first wire-locked reserved field (@c uint8_t).
   *
   * @return Reference to @c reserved_buf_.
   */
  uint8_t& get_reserved() noexcept { return reserved_buf_; }

  /**
   * @brief Mutable accessor for the second wire-locked reserved field (@c uint32_t).
   *
   * @return Reference to @c reserved_buf2_.
   */
  uint32_t& get_reserved2() noexcept { return reserved_buf2_; }

  Header header;  ///< Sequencing and timestamp metadata prefix.

  static constexpr bool kZerocopyTypes{true};  ///< Marker probed by the VLink type-trait machinery.

 private:
  uint8_t* data_{nullptr};
  size_t size_{0};
  uint64_t update_time_ns_{0};
  uint64_t duration_ns_{0};
  char codec_[16]{0};
  char language_[8]{0};
  uint32_t channel_{0};
  uint32_t freq_{0};
  uint32_t sample_rate_{0};
  uint32_t num_samples_{0};
  uint32_t bitrate_{0};
  uint16_t num_channels_{0};
  uint16_t bit_depth_{0};
  Format format_{kFormatUnknown};
  Layout layout_{kLayoutUnknown};
  bool is_owner_{false};
  uint8_t reserved_buf_{0};
  uint32_t reserved_buf2_{0};

  static constexpr uint32_t kMagicNumberBegin{0x98B7F1AA};
  static constexpr uint32_t kMagicNumberEnd{0x98B7F1AF};
};

}  // namespace zerocopy

}  // namespace vlink
