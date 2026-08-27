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
 * @file fast_stream.h
 * @brief Allocation-light @c std::ostream backed by a growable string buffer.
 *
 * @details
 * @c FastStream is the zero-copy log assembly buffer used inside the VLink logger.  Inheriting
 * from @c std::ostream means every @c operator<< overload and manipulator works unchanged; the
 * difference lies in the @c StringBuf backing storage and in @c take_view, which yields a
 * @c std::string_view directly into the buffer for hand-off to the active sink without an
 * intermediate copy.
 *
 * @par Supported operators
 *
 * | Operation             | Source                           | Notes                                           |
 * | --------------------- | -------------------------------- | ----------------------------------------------- |
 * | @c operator<<         | @c std::ostream interface        | All integral, floating, pointer types           |
 * | Standard manipulators | @c std::ios_base::fmtflags etc.  | @c std::hex, @c std::setw, @c std::setfill, ... |
 * | @c write_raw          | direct buffer write              | Bypasses locale and format flags                |
 * | @c append_to          | append current contents          | Does not reset the buffer                       |
 * | @c take_view          | hand-off as @c string_view       | View invalidated by the next write              |
 *
 * @par Growth policy
 *
 * | Stage              | Capacity                          |
 * | ------------------ | --------------------------------- |
 * | Initial            | @c kDefaultCapacity (@c 256)      |
 * | Doubling           | until @c kMaxExpandSize (@c 8192) |
 * | Linear increments  | @c kMaxExpandSize step thereafter |
 *
 * @par Example
 * @code
 *   vlink::FastStream stream;
 *   stream << "sensor_id=" << 42 << " value=" << 3.14;
 *   std::string_view view = stream.take_view();  // valid until next write
 *   write_to_sink(view);
 *   stream.reset();
 *
 *   stream.write_raw("LITERAL", 7);              // bypass formatting
 * @endcode
 *
 * @note Not thread-safe; the logger keeps one @c FastStream per thread via @c thread_local.
 *       Views returned by @c take_view become invalid on the next stream operation or @c reset.
 */

#pragma once

#include <cstdint>
#include <ostream>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

#include "./macros.h"

namespace vlink {

/**
 * @class FastStream
 * @brief Logger-friendly @c std::ostream with a custom growable string buffer.
 *
 * @details
 * Routes every write through an embedded @c StringBuf that owns a @c std::string.  The buffer
 * grows by doubling up to 8 KiB and by linear 8 KiB increments thereafter.  @c take_view yields
 * a non-owning view into the buffer, so a single log line incurs at most one allocation when
 * the buffer must grow and zero allocations on the steady-state path.
 */
class VLINK_EXPORT FastStream : public std::ostream {
 public:
  /**
   * @brief Constructs the stream with the default initial backing capacity.
   *
   * @details
   * Initial capacity is @c kDefaultCapacity (@c 256 bytes); the buffer grows on demand.
   */
  FastStream() noexcept;

  /**
   * @brief Destructor; releases the underlying string buffer.
   */
  ~FastStream() noexcept override;

  /**
   * @brief Empties the buffer and clears the stream's error state.
   *
   * @details
   * Retains the allocated capacity so subsequent messages avoid reallocation.
   */
  void reset() noexcept;

  /**
   * @brief Appends the current buffer contents to @p target without resetting the stream.
   *
   * @param target  Destination string; contents are appended in place.
   */
  void append_to(std::string& target) const noexcept;

  /**
   * @brief Returns a non-owning view of the current buffer contents.
   *
   * @details
   * Primary zero-copy hand-off used by the logger.  The view remains valid until the next write
   * to this stream or an explicit @c reset.
   *
   * @warning Do not retain the view beyond the next stream operation; doing so dereferences
   *          freed or moved storage.
   *
   * @return View covering the current buffer.
   */
  std::string_view take_view();

  /**
   * @brief Returns the number of bytes currently held in the buffer.
   *
   * @return Buffer length in bytes.
   */
  [[nodiscard]] size_t size() const noexcept;

  /**
   * @brief Returns the current allocated capacity of the backing buffer.
   *
   * @return Capacity in bytes.
   */
  [[nodiscard]] size_t capacity() const noexcept;

  /**
   * @brief Trims the underlying buffer to its current backing size.
   *
   * @details
   * Does not shrink to the formatted message length; the put pointer is reset to the start.
   */
  void shrink_to_fit() noexcept;

  /**
   * @brief Writes raw bytes into the buffer without going through @c std::ostream formatting.
   *
   * @details
   * Faster than @c std::ostream::write for pre-formatted C strings because it skips the locale
   * and format-flag plumbing.
   *
   * @param data  Source pointer; non-null when @p len is non-zero.
   * @param len   Number of bytes to write.
   * @return Reference to @c *this for chaining.
   */
  FastStream& write_raw(const char* data, size_t len);

  /**
   * @brief Appends a value using the logger's fast formatting paths when applicable.
   *
   * @note Integer fast formatting is locale-independent.
   *
   * @tparam T Value type.
   * @param value Value to append.
   * @return Reference to @c *this for chaining.
   */
  template <typename T>
  FastStream& push(T&& value);

 private:
  FastStream& push_floating(float value);

  FastStream& push_floating(double value);

  bool push_integer(int64_t value);

  bool push_integer(uint64_t value);

  static constexpr size_t kDefaultCapacity{256};
  static constexpr size_t kMinCapacity{64};
  static constexpr size_t kMaxExpandSize{8192};

  class VLINK_EXPORT StringBuf final : public std::streambuf {
   public:
    explicit StringBuf(size_t initial_capacity = kDefaultCapacity);

    void reset() noexcept;

    void shrink_to_fit() noexcept;

    void append_to(std::string& target) const noexcept;

    [[nodiscard]] std::string_view take_view();

    [[nodiscard]] size_t size() const noexcept;

    [[nodiscard]] size_t capacity() const noexcept;

    [[nodiscard]] int_type overflow(int_type ch) override;

    std::streamsize xsputn(const char* s, std::streamsize n) override;

   private:
    void grow_buffer(size_t required_size);

    void advance_pptr(size_t count) noexcept;

    std::string buffer_;

    VLINK_DISALLOW_COPY_AND_ASSIGN(StringBuf)
  };

  StringBuf buf_;

  VLINK_DISALLOW_COPY_AND_ASSIGN(FastStream)
};

////////////////////////////////////////////////////////////////
/// Details
////////////////////////////////////////////////////////////////

template <typename T>
inline FastStream& FastStream::push(T&& value) {
  using ValueT = std::remove_cv_t<std::remove_reference_t<T>>;

  if constexpr (std::is_array_v<ValueT> && std::is_same_v<std::remove_extent_t<ValueT>, char>) {
    if VLIKELY (width() == 0) {
      return write_raw(value, std::char_traits<char>::length(value));
    }
  } else if constexpr (std::is_same_v<ValueT, char*> || std::is_same_v<ValueT, const char*>) {
    if VLIKELY (value && width() == 0) {
      return write_raw(value, std::char_traits<char>::length(value));
    }
  } else if constexpr (std::is_same_v<ValueT, std::string>) {
    if VLIKELY (width() == 0) {
      return write_raw(value.data(), value.size());
    }
  } else if constexpr (std::is_same_v<ValueT, std::string_view>) {
    if VLIKELY (width() == 0) {
      return write_raw(value.data(), value.size());
    }
  } else if constexpr (std::is_same_v<ValueT, float> || std::is_same_v<ValueT, double>) {
    return push_floating(value);
  } else if constexpr (std::is_integral_v<ValueT> && sizeof(ValueT) <= sizeof(uint64_t) &&
                       !std::is_same_v<ValueT, bool> && !std::is_same_v<ValueT, char> &&
                       !std::is_same_v<ValueT, signed char> && !std::is_same_v<ValueT, unsigned char> &&
                       !std::is_same_v<ValueT, wchar_t> && !std::is_same_v<ValueT, char16_t> &&
                       !std::is_same_v<ValueT, char32_t>) {
    if constexpr (std::is_signed_v<ValueT>) {
      if VLIKELY (push_integer(static_cast<int64_t>(value))) {
        return *this;
      }
    } else {
      if VLIKELY (push_integer(static_cast<uint64_t>(value))) {
        return *this;
      }
    }
  }

  // NOLINTNEXTLINE(bugprone-unintended-char-ostream-output)
  static_cast<std::ostream&>(*this) << std::forward<T>(value);
  return *this;
}

}  // namespace vlink
