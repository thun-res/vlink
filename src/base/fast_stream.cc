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

#include "./base/fast_stream.h"

#include <algorithm>
#include <cstring>
#include <limits>
#include <string>
#include <string_view>

namespace vlink {

// FastStream
FastStream::FastStream() noexcept : std::ostream(nullptr), buf_(kDefaultCapacity) { rdbuf(&buf_); }

FastStream::~FastStream() noexcept = default;

void FastStream::reset() noexcept {
  clear();
  buf_.reset();
}

void FastStream::append_to(std::string& target) const noexcept { buf_.append_to(target); }

std::string_view FastStream::take_view() { return buf_.take_view(); }

size_t FastStream::size() const noexcept { return buf_.size(); }

size_t FastStream::capacity() const noexcept { return buf_.capacity(); }

void FastStream::shrink_to_fit() noexcept { buf_.shrink_to_fit(); }

FastStream& FastStream::write_raw(const char* data, size_t len) {
  buf_.xsputn(data, static_cast<std::streamsize>(len));
  return *this;
}

// FastStream::StringBuf
FastStream::StringBuf::StringBuf(size_t initial_capacity) {
  auto actual_capacity = std::max(initial_capacity, kMinCapacity);
  buffer_.resize(actual_capacity);
  reset();
}

void FastStream::StringBuf::reset() noexcept { setp(buffer_.data(), buffer_.data() + buffer_.size()); }

void FastStream::StringBuf::shrink_to_fit() noexcept {
  buffer_.shrink_to_fit();
  setp(buffer_.data(), buffer_.data() + buffer_.size());
}

void FastStream::StringBuf::append_to(std::string& target) const noexcept {
  target.reserve(target.size() + size());
  target.append(pbase(), size());
}

std::string_view FastStream::StringBuf::take_view() {
  auto current_size = static_cast<size_t>(pptr() - pbase());

  if VUNLIKELY (current_size >= buffer_.size()) {
    grow_buffer(current_size + 1);
  }

  buffer_[current_size] = '\0';

  return {pbase(), current_size};
}

size_t FastStream::StringBuf::size() const noexcept { return static_cast<size_t>(pptr() - pbase()); }

size_t FastStream::StringBuf::capacity() const noexcept { return buffer_.size(); }

void FastStream::StringBuf::grow_buffer(size_t required_size) {
  auto pos = static_cast<size_t>(pptr() - pbase());
  auto new_size = buffer_.size();

  if VUNLIKELY (new_size > kMaxExpandSize) {
    new_size = required_size + kMaxExpandSize;
  } else {
    while (new_size < required_size) {
      new_size *= 2;
    }
  }

  buffer_.resize(new_size);
  setp(buffer_.data(), buffer_.data() + buffer_.size());
  advance_pptr(pos);
}

void FastStream::StringBuf::advance_pptr(size_t count) noexcept {
  constexpr auto kMaxBump = static_cast<size_t>(std::numeric_limits<int>::max());

  while (count > kMaxBump) {
    pbump(std::numeric_limits<int>::max());
    count -= kMaxBump;
  }

  pbump(static_cast<int>(count));
}

std::ostream::int_type FastStream::StringBuf::overflow(int_type ch) {
  if VUNLIKELY (traits_type::eq_int_type(ch, traits_type::eof())) {
    return traits_type::eof();
  }

  grow_buffer(buffer_.size() + 1);

  *pptr() = traits_type::to_char_type(ch);
  pbump(1);

  return traits_type::not_eof(ch);
}

std::streamsize FastStream::StringBuf::xsputn(const char* s, std::streamsize n) {
  if VUNLIKELY (n <= 0) {
    return 0;
  }

  auto count = static_cast<size_t>(n);
  auto available = static_cast<size_t>(epptr() - pptr());

  if VLIKELY (count <= available) {
    std::memcpy(pptr(), s, count);
    advance_pptr(count);
    return n;
  }

  auto pos = static_cast<size_t>(pptr() - pbase());
  auto required = pos + count;

  grow_buffer(required);

  std::memcpy(pptr(), s, count);
  advance_pptr(count);

  return n;
}

}  // namespace vlink
