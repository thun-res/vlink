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
 * @file terminal_stream.h
 * @brief Lock-protected buffered stdout writer with TTY detection and ANSI escape support.
 *
 * @details
 * @c TerminalStream is a process singleton that funnels formatted output straight into a
 * one-megabyte internal buffer and emits it through raw @c write() / @c _write() syscalls.
 * It exists because @c std::cout's locale conversion, hidden allocations, and per-character
 * mutex traffic dominate the cost of hot-path logging in the VLink runtime.
 *
 * @par Stream modes
 *
 * | Mode               | Trigger                                       | Behaviour                                    |
 * | ------------------ | --------------------------------------------- | -------------------------------------------- |
 * | Buffered append    | write fits in remaining buffer space          | append to buffer, no syscall                 |
 * | Auto flush         | buffer fills or @c endl is used               | drain buffer with @c write() / @c _write()   |
 * | Direct passthrough | single write >= @c kDefaultBufferSize         | flush buffer first, then write directly      |
 * | Manual flush       | @c flush() or @c flush_manip invoked          | drain buffer (and @c tcdrain on POSIX TTYs)  |
 *
 * @par ANSI colour cheat-sheet (foreground)
 *
 * | Sequence        | Effect                 |   | Sequence        | Effect                       |
 * | --------------- | ---------------------- | - | --------------- | ---------------------------- |
 * | @c "\x1b[0m"    | reset all attributes   |   | @c "\x1b[37m"   | white                        |
 * | @c "\x1b[1m"    | bold / bright          |   | @c "\x1b[90m"   | bright black (grey)          |
 * | @c "\x1b[30m"   | black                  |   | @c "\x1b[91m"   | bright red                   |
 * | @c "\x1b[31m"   | red                    |   | @c "\x1b[92m"   | bright green                 |
 * | @c "\x1b[32m"   | green                  |   | @c "\x1b[93m"   | bright yellow                |
 * | @c "\x1b[33m"   | yellow                 |   | @c "\x1b[94m"   | bright blue                  |
 * | @c "\x1b[34m"   | blue                   |   | @c "\x1b[95m"   | bright magenta               |
 * | @c "\x1b[35m"   | magenta                |   | @c "\x1b[96m"   | bright cyan                  |
 * | @c "\x1b[36m"   | cyan                   |   | @c "\x1b[97m"   | bright white                 |
 *
 * Background colours follow the same pattern shifted by 10 (@c "\x1b[40m" -> @c "\x1b[47m").
 * Always gate colour emission behind @c is_tty() so redirected output stays clean.
 *
 * @par Thread safety
 * Every public method takes the same internal mutex, so a single @c operator<< call cannot
 * interleave with another thread's call.  Chained writes (@c ts << a << b) acquire the mutex
 * per token; wrap the chain in an external lock if cross-token atomicity is required.
 *
 * @par Example
 * @code
 *   auto& ts = vlink::TerminalStream::get();
 *   ts.init();
 *
 *   if (ts.is_tty()) {
 *     ts << "\x1b[32m[OK]\x1b[0m  ";
 *   } else {
 *     ts << "[OK]  ";
 *   }
 *   ts << "boot in " << 12.3 << " ms" << vlink::TerminalStream::endl;
 * @endcode
 */

#pragma once

#include <atomic>
#include <cstddef>
#include <iosfwd>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include "../base/macros.h"

namespace vlink {

/**
 * @class TerminalStream
 * @brief Process-wide buffered stdout writer with TTY detection, ANSI support, and a 1 MiB ring.
 *
 * @details
 * Construction is hidden behind @c get(); copy and move are disabled.  Internally the class
 * keeps the buffer, write cursor, file descriptor, and an atomic TTY flag set by @c init().
 */
class VLINK_EXPORT TerminalStream final {
 public:
  /**
   * @brief Default capacity of the internal buffer (1 MiB).
   *
   * @details
   * Writes smaller than this accumulate in the buffer until @c flush() / a flushing manipulator
   * fires or the buffer fills.  Writes at or above this threshold bypass the buffer.
   */
  static constexpr size_t kDefaultBufferSize{1024 * 1024 * 1};

  /**
   * @brief Function-pointer type accepted by @c operator<<(ManipType) for custom manipulators.
   */
  using ManipType = TerminalStream& (*)(TerminalStream&);

  /**
   * @brief Returns the process-wide singleton, building it on the first call.
   *
   * @return Reference to the singleton instance.
   */
  static TerminalStream& get() noexcept;

  /**
   * @brief Manipulator that appends a newline byte and flushes the buffer.
   *
   * @param stream  Stream to write to.
   * @return Reference to @p stream for chaining.
   */
  static TerminalStream& endl(TerminalStream& stream) noexcept;

  /**
   * @brief Manipulator that flushes the buffer without appending any byte.
   *
   * @param stream  Stream to flush.
   * @return Reference to @p stream for chaining.
   */
  static TerminalStream& flush_manip(TerminalStream& stream) noexcept;

  /**
   * @brief Constructs the stream with a @c kDefaultBufferSize buffer and platform stdout fd.
   *
   * @details
   * No syscalls run at construction time; @c init() detects TTY status and enables Windows
   * virtual-terminal processing when invoked separately.
   */
  TerminalStream() noexcept;

  /**
   * @brief Flushes any pending bytes and releases internal storage.
   */
  ~TerminalStream() noexcept;

  TerminalStream(const TerminalStream&) noexcept = delete;

  TerminalStream& operator=(const TerminalStream&) noexcept = delete;

  TerminalStream(TerminalStream&&) noexcept = delete;

  TerminalStream& operator=(TerminalStream&&) noexcept = delete;

  /**
   * @brief Detects the platform stdout descriptor, TTY status, and enables Windows ANSI handling.
   *
   * @details
   * Idempotent and thread-safe; only the first call performs work.  Call once early in
   * @c main() before relying on @c is_tty() or emitting ANSI escape sequences on Windows.
   */
  void init() noexcept;

  /**
   * @brief Reports whether stdout was a terminal when @c init() ran.
   *
   * @return @c true when @c init() saw a TTY; @c false otherwise.
   */
  [[nodiscard]] bool is_tty() const noexcept;

  /**
   * @brief Reports whether @c init() has been invoked.
   *
   * @return @c true once @c init() completes for the first time.
   */
  [[nodiscard]] bool is_initialized() const noexcept;

  /**
   * @brief Drains every buffered byte to stdout immediately.
   *
   * @details
   * On POSIX TTYs the call additionally invokes @c tcdrain() so the kernel queue is empty
   * before returning, providing a strong "user can see it now" guarantee.
   */
  void flush();

  /**
   * @brief Appends a raw byte array verbatim to the buffer.
   *
   * @param data  Pointer to @p len bytes; not retained beyond the call.
   * @param len   Number of bytes to append; @c 0 is a no-op.
   * @return Reference to @c *this for chaining.
   */
  TerminalStream& write_raw(const char* data, size_t len) noexcept;

  /**
   * @brief Appends a single character to the buffer.
   *
   * @param c  Character to append.
   * @return Reference to @c *this for chaining.
   */
  TerminalStream& operator<<(char c) noexcept;

  /**
   * @brief Appends the bytes of a NUL-terminated C string (NUL excluded).
   *
   * @param str  Source string or @c nullptr (no-op).
   * @return Reference to @c *this for chaining.
   */
  TerminalStream& operator<<(const char* str) noexcept;

  /**
   * @brief Appends the bytes of @p str verbatim, including embedded NULs.
   *
   * @param str  Source string.
   * @return Reference to @c *this for chaining.
   */
  TerminalStream& operator<<(const std::string& str) noexcept;

  /**
   * @brief Appends the bytes of @p str verbatim, including embedded NULs.
   *
   * @param str  Source view.
   * @return Reference to @c *this for chaining.
   */
  TerminalStream& operator<<(std::string_view str) noexcept;

  /**
   * @brief Appends the literal @c "true" or @c "false".
   *
   * @param value  Boolean to print.
   * @return Reference to @c *this for chaining.
   */
  TerminalStream& operator<<(bool value) noexcept;

  /**
   * @name Integer overloads
   * @brief Format a signed or unsigned integer as decimal and append it.
   *
   * @param value  Integer to print.
   * @return Reference to @c *this for chaining.
   * @{
   */
  TerminalStream& operator<<(short value) noexcept;           // NOLINT(runtime/int,google-runtime-int)
  TerminalStream& operator<<(unsigned short value) noexcept;  // NOLINT(runtime/int,google-runtime-int)
  TerminalStream& operator<<(int value) noexcept;
  TerminalStream& operator<<(unsigned int value) noexcept;
  TerminalStream& operator<<(long value) noexcept;                // NOLINT(runtime/int,google-runtime-int)
  TerminalStream& operator<<(unsigned long value) noexcept;       // NOLINT(runtime/int,google-runtime-int)
  TerminalStream& operator<<(long long value) noexcept;           // NOLINT(runtime/int,google-runtime-int)
  TerminalStream& operator<<(unsigned long long value) noexcept;  // NOLINT(runtime/int,google-runtime-int)
  /** @} */

  /**
   * @brief Formats a @c float through @c snprintf with the @c "%g" format.
   *
   * @param value  Float to print.
   * @return Reference to @c *this for chaining.
   */
  TerminalStream& operator<<(float value) noexcept;

  /**
   * @brief Formats a @c double through @c snprintf with the @c "%g" format.
   *
   * @param value  Double to print.
   * @return Reference to @c *this for chaining.
   */
  TerminalStream& operator<<(double value) noexcept;

  /**
   * @brief Formats a @c long @c double through @c snprintf with the @c "%Lg" format.
   *
   * @param value  Long double to print.
   * @return Reference to @c *this for chaining.
   */
  TerminalStream& operator<<(long double value) noexcept;  // NOLINT(google-runtime-float)

  /**
   * @brief Formats a pointer using @c snprintf with the @c "%p" format.
   *
   * @param ptr  Pointer to format.
   * @return Reference to @c *this for chaining.
   */
  TerminalStream& operator<<(const void* ptr) noexcept;

  /**
   * @brief Applies a custom or built-in @c ManipType manipulator to the stream.
   *
   * @param manip  Function pointer invoked with @c *this.
   * @return Whatever @p manip returns, normally a reference to @c *this.
   */
  TerminalStream& operator<<(ManipType manip) noexcept;

  /**
   * @brief Accepts @c std::endl and similar standard manipulators.
   *
   * @details
   * Standard manipulators expect a @c std::ostream; this overload ignores the function
   * pointer and emits a newline followed by a flush, matching @c TerminalStream::endl.
   *
   * @return Reference to @c *this for chaining.
   */
  TerminalStream& operator<<(std::ostream& (*)(std::ostream&)) noexcept;

 private:
  static int default_stdout_fd() noexcept;

  void flush_unlocked() noexcept;

  void write_to_buffer(const char* data, size_t len) noexcept;

  TerminalStream& write_signed(long long value) noexcept;             // NOLINT(runtime/int,google-runtime-int)
  TerminalStream& write_unsigned(unsigned long long value) noexcept;  // NOLINT(runtime/int,google-runtime-int)

  TerminalStream& write_double(double value) noexcept;
  TerminalStream& write_long_double(long double value) noexcept;  // NOLINT(google-runtime-float)

  std::atomic_bool initialized_{false};
  std::atomic_bool is_tty_{false};

  mutable std::mutex mutex_;
  std::vector<char> buffer_;
  size_t write_pos_{0};
  int fd_{default_stdout_fd()};
};

}  // namespace vlink

#ifndef VLINK_TERM_OUT
#define VLINK_TERM_OUT vlink::TerminalStream::get()
#endif
