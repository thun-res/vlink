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

#include "./base/cached_timestamp.h"

#include <cstdio>
#include <limits>

namespace vlink {

CachedTimestamp::CachedTimestamp() = default;

CachedTimestamp::~CachedTimestamp() = default;

std::string_view CachedTimestamp::get(const char* format, bool use_utc) {
  return get_at(std::chrono::system_clock::now(), format, use_utc);
}

std::string_view CachedTimestamp::get_at(std::chrono::system_clock::time_point now, const char* format, bool use_utc) {
  const auto milliseconds = std::chrono::floor<std::chrono::milliseconds>(now.time_since_epoch()).count();
  int64_t sec = milliseconds / 1000;
  int ms = static_cast<int>(milliseconds % 1000);

  if VUNLIKELY (ms < 0) {
    --sec;
    ms += 1000;
  }

  std::lock_guard lock(mtx_);

  int64_t cached_sec = last_sec_;

  static constexpr const char* kCanonicalFormat = "%02d-%02d %02d:%02d:%02d.%03d";

  if VUNLIKELY (format == nullptr) {
    format = kCanonicalFormat;
  }

  const bool canonical = (format == kCanonicalFormat) || (std::string_view(format) == kCanonicalFormat);

  if VLIKELY (cache_valid_ && canonical && sec == cached_sec && is_utc_ == use_utc) {
    update_milliseconds(ms);
    return std::string_view(buffer_, buffer_len_);
  }

  format_full_timestamp(format, sec, use_utc, ms);
  cache_valid_ = canonical && buffer_len_ != 0;

  last_sec_ = sec;
  is_utc_ = use_utc;

  return std::string_view(buffer_, buffer_len_);
}

void CachedTimestamp::format_full_timestamp(const char* format, int64_t seconds, bool use_utc, int ms) {
  buffer_len_ = 0;

  if constexpr (!std::numeric_limits<std::time_t>::is_signed) {
    if VUNLIKELY (seconds < 0) {
      return;
    }
  }

  const auto now_time_t = static_cast<std::time_t>(seconds);

  if constexpr (sizeof(std::time_t) < sizeof(int64_t)) {
    if VUNLIKELY (static_cast<int64_t>(now_time_t) != seconds) {
      return;
    }
  }

  std::tm now_tm{};
  bool converted = false;

#if defined(_WIN32)

  if (use_utc) {
    converted = gmtime_s(&now_tm, &now_time_t) == 0;
  } else {
    converted = localtime_s(&now_tm, &now_time_t) == 0;
  }
#else

  if (use_utc) {
    converted = gmtime_r(&now_time_t, &now_tm) != nullptr;
  } else {
    converted = localtime_r(&now_time_t, &now_tm) != nullptr;
  }
#endif

  if VUNLIKELY (!converted) {
    return;
  }

  int len = std::snprintf(buffer_, sizeof(buffer_), format, now_tm.tm_mon + 1, now_tm.tm_mday, now_tm.tm_hour,
                          now_tm.tm_min, now_tm.tm_sec, ms);

  if VUNLIKELY (len < 0 || len >= static_cast<int>(sizeof(buffer_))) {
    return;
  }

  buffer_len_ = static_cast<size_t>(len);
  ms_offset_ = buffer_len_ - 3;
}

void CachedTimestamp::update_milliseconds(int ms) {
  char* ms_ptr = buffer_ + ms_offset_;

  ms_ptr[0] = '0' + (ms / 100);
  ms_ptr[1] = '0' + (ms / 10) % 10;
  ms_ptr[2] = '0' + (ms % 10);
}

}  // namespace vlink
