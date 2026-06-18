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

#include "./extension/terminal_stream.h"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <limits>
#include <mutex>
#include <ostream>
#include <string>
#include <string_view>

#ifdef _WIN32
#include <Windows.h>
#include <io.h>
#undef min
#undef max
#undef GetMessage
#define VLINK_TERM_STDOUT_FILENO _fileno(stdout)
#define VLINK_TERM_WRITE _write
#define VLINK_TERM_ISATTY _isatty
#else
#include <termios.h>
#include <unistd.h>
#define VLINK_TERM_STDOUT_FILENO STDOUT_FILENO
#define VLINK_TERM_WRITE ::write
#define VLINK_TERM_ISATTY ::isatty
#endif

namespace vlink {

// TerminalStream
TerminalStream& TerminalStream::get() noexcept {
  static TerminalStream instance;

  return instance;
}

TerminalStream& TerminalStream::endl(TerminalStream& stream) noexcept {
  std::lock_guard lock(stream.mutex_);
  stream.write_to_buffer("\n", 1);
  stream.flush_unlocked();

  return stream;
}

TerminalStream& TerminalStream::flush_manip(TerminalStream& stream) noexcept {
  std::lock_guard lock(stream.mutex_);
  stream.flush_unlocked();

  return stream;
}

TerminalStream::TerminalStream() noexcept : buffer_(kDefaultBufferSize) {}

TerminalStream::~TerminalStream() noexcept {
  std::lock_guard lock(mutex_);
  flush_unlocked();
}

void TerminalStream::init() noexcept {
  bool expected = false;

  if (!initialized_.compare_exchange_strong(expected, true)) {
    return;
  }

  std::lock_guard lock(mutex_);

  fd_ = VLINK_TERM_STDOUT_FILENO;

#ifdef _WIN32
  HANDLE h_out = GetStdHandle(STD_OUTPUT_HANDLE);

  if (h_out != INVALID_HANDLE_VALUE) {
    DWORD dw_mode = 0;

    if (GetConsoleMode(h_out, &dw_mode)) {
      dw_mode |= 0x0004;  // ENABLE_VIRTUAL_TERMINAL_PROCESSING
      SetConsoleMode(h_out, dw_mode);
    }
  }
#endif

  is_tty_.store(VLINK_TERM_ISATTY(fd_) != 0, std::memory_order_release);
}

bool TerminalStream::is_tty() const noexcept { return is_tty_.load(std::memory_order_acquire); }

bool TerminalStream::is_initialized() const noexcept { return initialized_.load(std::memory_order_acquire); }

void TerminalStream::flush() {
  std::lock_guard lock(mutex_);
  flush_unlocked();
}

TerminalStream& TerminalStream::write_raw(const char* data, size_t len) noexcept {
  std::lock_guard lock(mutex_);
  write_to_buffer(data, len);

  return *this;
}

TerminalStream& TerminalStream::operator<<(char c) noexcept {
  std::lock_guard lock(mutex_);
  write_to_buffer(&c, 1);

  return *this;
}

TerminalStream& TerminalStream::operator<<(const char* str) noexcept {
  if VUNLIKELY (str == nullptr) {
    return *this;
  }

  std::lock_guard lock(mutex_);
  write_to_buffer(str, std::strlen(str));

  return *this;
}

TerminalStream& TerminalStream::operator<<(const std::string& str) noexcept {
  std::lock_guard lock(mutex_);
  write_to_buffer(str.data(), str.size());

  return *this;
}

TerminalStream& TerminalStream::operator<<(std::string_view str) noexcept {
  std::lock_guard lock(mutex_);
  write_to_buffer(str.data(), str.size());

  return *this;
}

TerminalStream& TerminalStream::operator<<(bool value) noexcept { return *this << (value ? "true" : "false"); }

TerminalStream& TerminalStream::operator<<(short value) noexcept {  // NOLINT(runtime/int,google-runtime-int)
  return write_signed(static_cast<long long>(value));               // NOLINT(runtime/int,google-runtime-int)
}

TerminalStream& TerminalStream::operator<<(unsigned short value) noexcept {  // NOLINT(runtime/int,google-runtime-int)
  return write_unsigned(static_cast<unsigned long long>(value));             // NOLINT(runtime/int,google-runtime-int)
}

TerminalStream& TerminalStream::operator<<(int value) noexcept {  // NOLINT(runtime/int,google-runtime-int)
  return write_signed(static_cast<long long>(value));             // NOLINT(runtime/int,google-runtime-int)
}

TerminalStream& TerminalStream::operator<<(unsigned int value) noexcept {  // NOLINT(runtime/int,google-runtime-int)
  return write_unsigned(static_cast<unsigned long long>(value));           // NOLINT(runtime/int,google-runtime-int)
}

TerminalStream& TerminalStream::operator<<(long value) noexcept {  // NOLINT(runtime/int,google-runtime-int)
  return write_signed(static_cast<long long>(value));              // NOLINT(runtime/int,google-runtime-int)
}

TerminalStream& TerminalStream::operator<<(unsigned long value) noexcept {  // NOLINT(runtime/int,google-runtime-int)
  return write_unsigned(static_cast<unsigned long long>(value));            // NOLINT(runtime/int,google-runtime-int)
}

TerminalStream& TerminalStream::operator<<(long long value) noexcept {  // NOLINT(runtime/int,google-runtime-int)
  return write_signed(value);                                           // NOLINT(runtime/int,google-runtime-int)
}

TerminalStream& TerminalStream::operator<<(
    unsigned long long value) noexcept {  // NOLINT(runtime/int,google-runtime-int)
  return write_unsigned(value);
}

TerminalStream& TerminalStream::operator<<(float value) noexcept { return write_double(static_cast<double>(value)); }

TerminalStream& TerminalStream::operator<<(double value) noexcept { return write_double(value); }

TerminalStream& TerminalStream::operator<<(long double value) noexcept {  // NOLINT(google-runtime-float)
  return write_long_double(value);
}

TerminalStream& TerminalStream::operator<<(const void* ptr) noexcept {
  std::lock_guard lock(mutex_);

  char buf[32];
  int len = std::snprintf(buf, sizeof(buf), "%p", ptr);

  if VLIKELY (len > 0) {
    write_to_buffer(buf, static_cast<size_t>(len));
  }

  return *this;
}

TerminalStream& TerminalStream::operator<<(ManipType manip) noexcept { return manip(*this); }

TerminalStream& TerminalStream::operator<<(std::ostream& (*)(std::ostream&)) noexcept {
  std::lock_guard lock(mutex_);

  write_to_buffer("\n", 1);
  flush_unlocked();

  return *this;
}

int TerminalStream::default_stdout_fd() noexcept { return VLINK_TERM_STDOUT_FILENO; }

void TerminalStream::flush_unlocked() noexcept {
  if (write_pos_ == 0) {
    return;
  }

  const char* data = buffer_.data();
  size_t remaining = write_pos_;
  size_t flushed = 0;

  while (remaining > 0) {
    auto written = VLINK_TERM_WRITE(fd_, data, static_cast<unsigned int>(remaining));

    if VUNLIKELY (written < 0) {
      if VUNLIKELY (errno == EINTR) {
        continue;
      }

      break;
    }

    if VUNLIKELY (written == 0) {
      break;
    }

    const auto written_size = static_cast<size_t>(written);
    data += written_size;
    remaining -= written_size;
    flushed += written_size;
  }

  if (remaining > 0) {
    if (flushed > 0) {
      std::memmove(buffer_.data(), buffer_.data() + flushed, remaining);
    }

    write_pos_ = remaining;

    return;
  }

  write_pos_ = 0;

#ifndef _WIN32

  if (is_tty_.load(std::memory_order_acquire)) {
    ::tcdrain(fd_);
  }
#endif
}

void TerminalStream::write_to_buffer(const char* data, size_t len) noexcept {
  if VUNLIKELY (len == 0) {
    return;
  }

  if (len >= buffer_.size()) {
    flush_unlocked();

    if (write_pos_ != 0) {
      // if VUNLIKELY (len > std::numeric_limits<size_t>::max() - write_pos_) {
      //   return;
      // }
      //
      // const size_t target_size = write_pos_ + len;
      // if (target_size > buffer_.size()) {
      //   try {
      //     buffer_.resize(target_size);
      //   } catch (...) {
      //     return;
      //   }
      // }
      //
      // std::memcpy(buffer_.data() + write_pos_, data, len);
      // write_pos_ = target_size;
      return;
    }

    size_t remaining = len;
    while (remaining > 0) {
      auto written = VLINK_TERM_WRITE(fd_, data, static_cast<unsigned int>(remaining));

      if VUNLIKELY (written < 0) {
        if VUNLIKELY (errno == EINTR) {
          continue;
        }

        break;
      }

      if VUNLIKELY (written == 0) {
        break;
      }

      const auto written_size = static_cast<size_t>(written);
      data += written_size;
      remaining -= written_size;
    }

    if (remaining > 0) {
      if (remaining > buffer_.size()) {
        try {
          buffer_.resize(remaining);
        } catch (...) {
          return;
        }
      }

      std::memcpy(buffer_.data(), data, remaining);
      write_pos_ = remaining;
    }

    return;
  }

  if (write_pos_ + len > buffer_.size()) {
    flush_unlocked();
  }

  if (write_pos_ + len > buffer_.size()) {
    try {
      buffer_.resize(write_pos_ + len);
    } catch (...) {
      return;
    }
  }

  std::memcpy(buffer_.data() + write_pos_, data, len);
  write_pos_ += len;
}

TerminalStream& TerminalStream::write_signed(long long value) noexcept {  // NOLINT(runtime/int,google-runtime-int)
  std::lock_guard lock(mutex_);

  char buf[32];
  char* ptr = buf + sizeof(buf);
  bool negative = false;

  if (value < 0) {
    negative = true;

    if VUNLIKELY (value == std::numeric_limits<long long>::min()) {  // NOLINT(runtime/int,google-runtime-int)
      int len = std::snprintf(buf, sizeof(buf), "%lld", value);

      if VLIKELY (len > 0) {
        write_to_buffer(buf, static_cast<size_t>(len));
      }

      return *this;
    }

    value = -value;
  }

  if (value == 0) {
    *--ptr = '0';
  } else {
    while (value != 0) {
      *--ptr = '0' + static_cast<char>(value % 10);
      value /= 10;
    }
  }

  if (negative) {
    *--ptr = '-';
  }

  write_to_buffer(ptr, static_cast<size_t>(buf + sizeof(buf) - ptr));

  return *this;
}

TerminalStream& TerminalStream::write_unsigned(
    unsigned long long value) noexcept {  // NOLINT(runtime/int,google-runtime-int)
  std::lock_guard lock(mutex_);

  char buf[32];
  char* ptr = buf + sizeof(buf);

  if (value == 0) {
    *--ptr = '0';
  } else {
    while (value != 0) {
      *--ptr = '0' + static_cast<char>(value % 10);
      value /= 10;
    }
  }

  write_to_buffer(ptr, static_cast<size_t>(buf + sizeof(buf) - ptr));

  return *this;
}

TerminalStream& TerminalStream::write_double(double value) noexcept {
  std::lock_guard lock(mutex_);

  char buf[64];
  int len = std::snprintf(buf, sizeof(buf), "%g", value);

  if VLIKELY (len > 0) {
    write_to_buffer(buf, static_cast<size_t>(len));
  }

  return *this;
}

// NOLINTNEXTLINE(google-runtime-float)
TerminalStream& TerminalStream::write_long_double(long double value) noexcept {
  std::lock_guard lock(mutex_);

  char buf[64];
  int len = std::snprintf(buf, sizeof(buf), "%Lg", value);

  if VLIKELY (len > 0) {
    write_to_buffer(buf, static_cast<size_t>(len));
  }

  return *this;
}

}  // namespace vlink
