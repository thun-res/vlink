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

namespace vlink {

CachedTimestamp::CachedTimestamp() = default;

CachedTimestamp::~CachedTimestamp() = default;

std::string_view CachedTimestamp::get(const char* format, bool use_utc) {
  auto now = std::chrono::system_clock::now();
  auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch());

  int64_t sec = now_ms.count() / 1000;
  int ms = now_ms.count() % 1000;

  std::lock_guard lock(mtx_);

  int64_t cached_sec = last_sec_.load(std::memory_order_relaxed);

  if VLIKELY (sec == cached_sec && is_utc_ == use_utc) {
    update_milliseconds(ms);
    return std::string_view(buffer_, buffer_len_);
  }

  format_full_timestamp(format, now, use_utc, ms);

  last_sec_.store(sec, std::memory_order_release);
  is_utc_ = use_utc;

  return std::string_view(buffer_, buffer_len_);
}

void CachedTimestamp::format_full_timestamp(const char* format, std::chrono::system_clock::time_point now, bool use_utc,
                                            int ms) {
  std::time_t now_time_t = std::chrono::system_clock::to_time_t(now);
  std::tm now_tm{};

#if defined(_WIN32)

  if (use_utc) {
    gmtime_s(&now_tm, &now_time_t);
  } else {
    localtime_s(&now_tm, &now_time_t);
  }
#else

  if (use_utc) {
    gmtime_r(&now_time_t, &now_tm);
  } else {
    localtime_r(&now_time_t, &now_tm);
  }
#endif

  int len = std::snprintf(buffer_, sizeof(buffer_), format, now_tm.tm_mon + 1, now_tm.tm_mday, now_tm.tm_hour,
                          now_tm.tm_min, now_tm.tm_sec, ms);

  if VUNLIKELY (len < 3 || len >= static_cast<int>(sizeof(buffer_))) {
    buffer_len_ = 0;
    ms_offset_ = 0;
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
