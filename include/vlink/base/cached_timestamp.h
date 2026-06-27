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
 * @file cached_timestamp.h
 * @brief Sub-second-cached timestamp string generator used by the VLink logger hot path.
 *
 * @details
 * @c CachedTimestamp renders a wall-clock or UTC timestamp into a stable internal buffer and
 * patches only the millisecond suffix when the seconds field has not advanced.  This avoids the
 * repeated @c snprintf / @c localtime_r round trip that dominates naive per-line logger output.
 *
 * @par Resolution and field layout
 *
 * | Field                         | Width | Source                                |
 * | ----------------------------- | ----- | ------------------------------------- |
 * | Month                         |   2   | @c %02d                               |
 * | Day                           |   2   | @c %02d                               |
 * | Hour                          |   2   | @c %02d                               |
 * | Minute                        |   2   | @c %02d                               |
 * | Second                        |   2   | @c %02d                               |
 * | Millisecond (in-place patch)  |   3   | @c %03d, last three chars of buffer   |
 *
 * The default format @c "%02d-%02d @c %02d:%02d:%02d.%03d" produces 18 characters such as
 * @c "03-18 14:30:01.042".  The internal buffer is 32 bytes; longer formats are dropped.
 *
 * @par Example
 * @code
 *   vlink::CachedTimestamp ts;
 *   for (int i = 0; i < 1000; ++i) {
 *     // Wall clock (local time) - patched in place on the same second.
 *     std::string_view local = ts.get();
 *     // UTC variant on demand:
 *     std::string_view utc   = ts.get("%02d-%02d %02d:%02d:%02d.%03d", true);
 *     write_to_log(local);
 *   }
 * @endcode
 *
 * @note Used internally by @c vlink::Logger.  Application code rarely needs to construct one
 *       directly; doing so is harmless and inexpensive.
 */

#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <string_view>

#include "./macros.h"

namespace vlink {

/**
 * @class CachedTimestamp
 * @brief Mutable cached generator for short formatted timestamps.
 *
 * @details
 * Holds a 32-byte buffer protected by a mutex plus an atomic last-second counter so concurrent
 * @c get calls share a single formatted prefix and only the millisecond suffix is rewritten.
 * The first call seeds the cache; subsequent same-second calls patch only the trailing three
 * characters in place.
 */
class VLINK_EXPORT CachedTimestamp final {
 public:
  /**
   * @brief Constructs the generator with an empty internal cache.
   *
   * @details
   * The cache is populated lazily on the first @c get call.
   */
  CachedTimestamp();

  /**
   * @brief Destructor.
   */
  ~CachedTimestamp();

  /**
   * @brief Returns a view of the current formatted timestamp.
   *
   * @details
   * The returned view points into the internal 32-byte buffer.  It is invalidated by the next
   * call to @c get on any thread sharing this instance.  The format must end with a 3-digit
   * millisecond field because the cache patches the last three bytes in place; the entire
   * formatted string must fit within 31 characters.  Switching @p format or @p use_utc forces
   * a full reformat on the next second boundary.
   *
   * @param format   @c snprintf format consuming six @c int arguments in this order:
   *                 month, day, hour, minute, second, milliseconds.  Default:
   *                 @c "%02d-%02d @c %02d:%02d:%02d.%03d".
   * @param use_utc  @c true for UTC formatting; @c false for local time.  Default: @c false.
   * @return @c std::string_view over the rendered timestamp.
   */
  [[nodiscard]] std::string_view get(const char* format = "%02d-%02d %02d:%02d:%02d.%03d", bool use_utc = false);

 private:
  void format_full_timestamp(const char* format, std::chrono::system_clock::time_point now, bool use_utc, int ms);

  void update_milliseconds(int ms);

  alignas(64) std::atomic<int64_t> last_sec_{0};
  std::mutex mtx_;
  char buffer_[32]{};
  size_t buffer_len_{0};
  size_t ms_offset_{0};
  bool is_utc_{false};
};

}  // namespace vlink
