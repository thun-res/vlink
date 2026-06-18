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
 * @file url_remap.h
 * @brief JSON-driven substring rewriter for VLink topic URLs.
 *
 * @details
 * @c UrlRemap turns a flat JSON dictionary into a runtime topic-renaming layer.  Each entry maps
 * a fragment that should appear in an input URL to the replacement URL emitted to the transport,
 * allowing operators to switch backends, redirect topics, or stage migrations without rebuilding
 * application binaries.
 *
 * @par Rewrite rules
 *
 * | Source key (substring of input)   | Target value (replacement URL)        | Effect                            |
 * | --------------------------------- | ------------------------------------- | --------------------------------- |
 * | @c "intra://sensor/lidar"         | @c "dds://vehicle/lidar"              | promote local topic to DDS        |
 * | @c "shm://camera/front"           | @c "zenoh://camera/front"             | switch transport to Zenoh         |
 * | @c "dds://"                       | @c "ddst://"                          | force TCP DDS transport globally  |
 * | @c "fdbus://"                     | @c "intra://"                         | merge to intra-process bus        |
 *
 * Lookup is linear in the configured order; the first key that occurs as a substring of the
 * input URL wins.  Results are cached so repeated lookups for the same input are O(1).
 *
 * @par Environment-driven remap
 * @code
 *  +-------------------------+   load(path)      +---------------------+   convert(url)
 *  | VLINK_URL_REMAP=path    | ----------------> | UrlRemap (JSON map) | -----------------> rewritten URL
 *  | or load(path) at init   |                   | + result cache      |    cache hit -> O(1)
 *  +-------------------------+                   +---------------------+
 *                                                          |
 *                                                          v
 *                                                     transports receive
 *                                                     the rewritten URL
 * @endcode
 *
 * @par Example
 * @code
 *   vlink::UrlRemap remap;
 *
 *   if (remap.load("/etc/vlink/remap.json")) {
 *     auto target = remap.convert("intra://sensor/lidar");
 *     auto pub = vlink::Publisher<MyMsg>::create_unique(target);
 *   } else {
 *     VLOG_W("remap unavailable: ", remap.get_error_string());
 *   }
 *
 *   remap.reload("/etc/vlink/remap-v2.json");
 * @endcode
 *
 * @note
 * - @c UrlRemap is not thread-safe; serialise calls externally or confine to a single thread.
 * - @c convert() returns its input unchanged when the table is empty or unloaded.
 */

#pragma once

#include <string>
#include <unordered_map>
#include <vector>

#include "../base/macros.h"

namespace vlink {

/**
 * @class UrlRemap
 * @brief Loads a JSON rewrite table and substitutes VLink URLs at runtime.
 *
 * @details
 * Each instance holds an ordered rewrite list, a result cache, and a status string used for
 * diagnostics.  Copy and assignment are disabled.
 */
class VLINK_EXPORT UrlRemap {
 public:
  /**
   * @brief Constructs an empty, unloaded instance.
   */
  UrlRemap() noexcept;

  /**
   * @brief Destroys the instance and clears caches.
   */
  ~UrlRemap() noexcept;

  /**
   * @brief Parses @p file_path and installs the rewrite table.
   *
   * @param file_path  Absolute or relative path to a flat JSON object of string pairs.
   * @return @c true on success; @c false when already loaded, the file is missing, unreadable,
   *         or contains invalid JSON.  Errors are surfaced via @c get_error_string().
   */
  bool load(const std::string& file_path) noexcept;

  /**
   * @brief Clears the rewrite table and the result cache.
   *
   * @return @c true when the table was cleared; @c false when no table was loaded.
   */
  bool unload() noexcept;

  /**
   * @brief Atomically unloads the current table and loads @p file_path.
   *
   * @param file_path  Path of the replacement JSON file.
   * @return @c true when the new file loaded successfully.
   */
  bool reload(const std::string& file_path) noexcept;

  /**
   * @brief Translates @p url using the loaded rules; returns @p url unchanged on miss.
   *
   * @param url  Input URL to rewrite.
   * @return Reference to the rewritten URL, owned by the internal cache.
   */
  const std::string& convert(const std::string& url) noexcept;

  /**
   * @brief Toggles per-conversion INFO logging.
   *
   * @param enable_log  @c true emits one log line per successful rewrite.
   */
  void set_enable_log(bool enable_log) noexcept;

  /**
   * @brief Reports whether per-conversion logging is enabled.
   *
   * @return @c true when logging is currently enabled.
   */
  [[nodiscard]] bool is_enable_log() const noexcept;

  /**
   * @brief Reports whether a rewrite table has been successfully loaded.
   *
   * @return @c true after a successful @c load() or @c reload() and before the next @c unload().
   */
  [[nodiscard]] bool is_valid() const noexcept;

  /**
   * @brief Returns the diagnostic message produced by the last failure.
   *
   * @return Error string or empty when no failure has occurred since the last successful load.
   */
  [[nodiscard]] const std::string& get_error_string() const noexcept;

 private:
  bool is_enable_log_{false};
  bool is_valid_{false};

  std::string error_string_;

  std::vector<std::pair<std::string, std::string>> remap_list_;
  std::unordered_map<std::string, std::string> cache_map_;

  VLINK_DISALLOW_COPY_AND_ASSIGN(UrlRemap)
};

////////////////////////////////////////////////////////////////
/// Details
////////////////////////////////////////////////////////////////

inline void UrlRemap::set_enable_log(bool enable_log) noexcept { is_enable_log_ = enable_log; }

inline bool UrlRemap::is_enable_log() const noexcept { return is_enable_log_; }

inline bool UrlRemap::is_valid() const noexcept { return is_valid_; }

inline const std::string& UrlRemap::get_error_string() const noexcept { return error_string_; }

}  // namespace vlink
