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
 * @file spdlog_time_rolling_file_sink.h
 * @brief Private spdlog sink that rolls log files by both size and wall-clock timestamp.
 *
 * @details
 * The VLink @c Logger needs a sink that combines spdlog's size-based rotation with timestamped
 * filenames so that operators can correlate log slices with bag recordings.  This private,
 * non-installed header provides that sink as a header-only template, plus two factory functions
 * matching spdlog's usual @c _mt / @c _st convention.
 *
 * Compiled only when @c VLINK_ENABLE_LOG_SPD is defined; otherwise the file expands to nothing.
 *
 * | Component                          | Role                                                     |
 * | ---------------------------------- | -------------------------------------------------------- |
 * | @c TimeRollingFile<MutexT>         | The sink itself; @c MutexT selects multi/single-threaded |
 * | @c TimeRollingFile_mt / @c _st     | Convenience type aliases                                 |
 * | @c time_rolling_logger_mt / @c _st | Factory functions returning a complete @c spdlog::logger |
 *
 * Filenames follow the @c YYYY-MM-DD_HH-MM-SS.<index>.log pattern under a base directory; the
 * sink keeps at most @c max_files such files, deleting the oldest on rotation.
 */

#pragma once

#if defined(VLINK_ENABLE_LOG_SPD)

#include <spdlog/details/file_helper.h>
#include <spdlog/details/null_mutex.h>
#include <spdlog/details/synchronous_factory.h>
#include <spdlog/sinks/base_sink.h>

#include <algorithm>
#include <charconv>
#include <chrono>
#include <ctime>
#include <deque>
#include <filesystem>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <utility>

#include "./base/macros.h"

namespace vlink {
namespace spdlog_custom_sink {

/**
 * @enum TimeZone
 * @brief Selects the wall-clock interpretation used when stamping rolled log files.
 */
enum class TimeZone : uint8_t {
  kTimezoneLocal = 0,

  kTimezoneUtc = 1,
};

/**
 * @class TimeRollingFile
 * @brief Header-only spdlog sink that rolls log files by size with timestamped, indexed filenames.
 *
 * @details
 * The sink writes to @c <base>/<timestamp>.<index>.log; once a file exceeds @c max_size it is
 * rotated to a fresh timestamped file and indexed-rotated entries are pruned until at most
 * @c max_files remain.  On construction the sink also scans the existing directory so that a
 * crashed process can continue appending into the most recent file without losing the index.
 */
template <typename MutexT>
class TimeRollingFile final : public spdlog::sinks::base_sink<MutexT> {
 public:
  static constexpr size_t kMaxFiles = 10000;

  TimeRollingFile(spdlog::filename_t base_filename, size_t max_size, size_t max_files,
                  TimeZone timezone = TimeZone::kTimezoneLocal, bool rotate_on_open = false,
                  const spdlog::file_event_handlers& event_handlers = {});

  [[nodiscard]] spdlog::filename_t filename();

  void rotate_now();

  void set_max_size(size_t max_size);

  [[nodiscard]] size_t get_max_size();

  void set_max_files(size_t max_files);

  [[nodiscard]] size_t get_max_files();

  void set_timezone(TimeZone timezone);

  [[nodiscard]] TimeZone get_timezone();

 private:
  struct FileInfo final {
    spdlog::filename_t path;
    std::string timestamp;
    size_t index{0};
    bool is_valid{false};
  };

  // NOLINTNEXTLINE(readability-identifier-naming)
  static FileInfo parse_file_(const spdlog::filename_t& filename);

  // NOLINTNEXTLINE(readability-identifier-naming)
  FileInfo generate_file_(size_t index);

  // NOLINTNEXTLINE(readability-identifier-naming)
  void sink_it_(const spdlog::details::log_msg& msg) override;

  // NOLINTNEXTLINE(readability-identifier-naming)
  void flush_() override;

  // NOLINTNEXTLINE(readability-identifier-naming)
  void rotate_();

  spdlog::filename_t base_filename_;
  size_t max_size_{0};
  size_t max_files_{0};
  size_t current_size_{0};
  size_t current_index_{0};
  TimeZone timezone_{TimeZone::kTimezoneLocal};
  spdlog::details::file_helper file_helper_;
  std::deque<FileInfo> file_list_;

  VLINK_DISALLOW_COPY_AND_ASSIGN(TimeRollingFile)
};

using TimeRollingFile_mt = TimeRollingFile<std::mutex>;
using TimeRollingFile_st = TimeRollingFile<spdlog::details::null_mutex>;

/** @brief Factory: create a thread-safe spdlog logger backed by @c TimeRollingFile_mt. */
template <typename FactoryT = spdlog::synchronous_factory>
static std::shared_ptr<spdlog::logger> time_rolling_logger_mt(const std::string& logger_name,
                                                              const spdlog::filename_t& base_filename,
                                                              size_t max_file_size, size_t max_files,
                                                              TimeZone timezone = TimeZone::kTimezoneLocal,
                                                              bool rotate_on_open = false,
                                                              const spdlog::file_event_handlers& event_handlers = {});

/** @brief Factory: create a single-threaded spdlog logger backed by @c TimeRollingFile_st. */
template <typename FactoryT = spdlog::synchronous_factory>
static std::shared_ptr<spdlog::logger> time_rolling_logger_st(const std::string& logger_name,
                                                              const spdlog::filename_t& base_filename,
                                                              size_t max_file_size, size_t max_files,
                                                              TimeZone timezone = TimeZone::kTimezoneLocal,
                                                              bool rotate_on_open = false,
                                                              const spdlog::file_event_handlers& event_handlers = {});

template <typename MutexT>
inline TimeRollingFile<MutexT>::TimeRollingFile(spdlog::filename_t base_filename, size_t max_size, size_t max_files,
                                                TimeZone timezone, bool rotate_on_open,
                                                const spdlog::file_event_handlers& event_handlers)
    : base_filename_(std::move(base_filename)),
      max_size_(max_size),
      max_files_(max_files),
      timezone_(timezone),
      file_helper_{event_handlers} {
  if VUNLIKELY (max_size == 0) {
    spdlog::throw_spdlog_ex("custom rolling sink constructor: max_size argument cannot be zero");
  }

  if VUNLIKELY (max_files > kMaxFiles) {
    spdlog::throw_spdlog_ex("rotating sink constructor: max_files arg cannot exceed MaxFiles");
  }

  if VUNLIKELY (max_files == 0) {
    spdlog::throw_spdlog_ex("custom rolling sink constructor: max_files argument cannot be zero");
  }

  try {
    auto base_path = std::filesystem::path(base_filename_);

    if (std::filesystem::exists(base_path)) {
      if (std::filesystem::is_directory(base_path)) {
        for (const auto& entry : std::filesystem::directory_iterator(base_path)) {
          if VLIKELY (entry.is_regular_file() && entry.path().extension() == ".log") {
            auto file_info = parse_file_(entry.path().string());

            if VLIKELY (file_info.is_valid) {
              file_list_.emplace_back(std::move(file_info));
            }
          }
        }
      } else {
        std::string new_filename = base_filename_ + "_dir";
        std::filesystem::create_directories(new_filename);
        base_filename_ = std::move(new_filename);
      }
    } else {
      std::filesystem::create_directories(base_path);
    }
  } catch (const std::filesystem::filesystem_error& e) {
    std::cerr << "[spdlog_custom_sink] Failed to initialize log directory: " << e.what() << std::endl;
  }

  if (file_list_.empty()) {
    rotate_();
  } else {
    std::sort(file_list_.begin(), file_list_.end(), [](const FileInfo& a, const FileInfo& b) {
      if (a.index != b.index) {
        return a.index < b.index;
      }

      return a.timestamp < b.timestamp;
    });

    const auto& last_file = file_list_.back();

    current_index_ = last_file.index;

    if (rotate_on_open) {
      rotate_();
    } else {
      try {
        file_helper_.open(last_file.path);
        current_size_ = file_helper_.size();
      } catch (std::exception&) {
        rotate_();
      }
    }
  }
}

template <typename MutexT>
inline spdlog::filename_t TimeRollingFile<MutexT>::filename() {
  std::lock_guard<MutexT> lock(spdlog::sinks::base_sink<MutexT>::mutex_);
  return file_helper_.filename();
}

template <typename MutexT>
inline void TimeRollingFile<MutexT>::rotate_now() {
  std::lock_guard<MutexT> lock(spdlog::sinks::base_sink<MutexT>::mutex_);
  rotate_();
}

template <typename MutexT>
inline void TimeRollingFile<MutexT>::set_max_size(size_t max_size) {
  std::lock_guard<MutexT> lock(spdlog::sinks::base_sink<MutexT>::mutex_);

  if VUNLIKELY (max_size == 0) {
    spdlog::throw_spdlog_ex("custom rolling sink set_max_size: max_size argument cannot be zero");
  }

  max_size_ = max_size;
}

template <typename MutexT>
inline size_t TimeRollingFile<MutexT>::get_max_size() {
  std::lock_guard<MutexT> lock(spdlog::sinks::base_sink<MutexT>::mutex_);
  return max_size_;
}

template <typename MutexT>
inline void TimeRollingFile<MutexT>::set_max_files(size_t max_files) {
  std::lock_guard<MutexT> lock(spdlog::sinks::base_sink<MutexT>::mutex_);

  if VUNLIKELY (max_files == 0) {
    spdlog::throw_spdlog_ex("custom rolling sink set_max_files: max_files argument cannot be zero");
  }

  if VUNLIKELY (max_files > kMaxFiles) {
    spdlog::throw_spdlog_ex("rotating sink set_max_files: max_files arg cannot exceed MaxFiles");
  }

  max_files_ = max_files;
}

template <typename MutexT>
inline size_t TimeRollingFile<MutexT>::get_max_files() {
  std::lock_guard<MutexT> lock(spdlog::sinks::base_sink<MutexT>::mutex_);
  return max_files_;
}

template <typename MutexT>
inline void TimeRollingFile<MutexT>::set_timezone(TimeZone timezone) {
  std::lock_guard<MutexT> lock(spdlog::sinks::base_sink<MutexT>::mutex_);
  timezone_ = timezone;
}

template <typename MutexT>
inline TimeZone TimeRollingFile<MutexT>::get_timezone() {
  std::lock_guard<MutexT> lock(spdlog::sinks::base_sink<MutexT>::mutex_);
  return timezone_;
}

template <typename MutexT>
inline typename TimeRollingFile<MutexT>::FileInfo TimeRollingFile<MutexT>::parse_file_(
    const spdlog::filename_t& filename) {
  FileInfo info;
  info.path = filename;
  info.index = 0;
  info.is_valid = false;

  auto path = std::filesystem::path(filename);
  auto stem = path.stem().string();

  if VUNLIKELY (stem.size() < 21) {
    return info;
  }

  auto last_dot = stem.rfind('.');

  if VUNLIKELY (last_dot == std::string::npos || last_dot != 19) {
    return info;
  }

  info.timestamp.assign(stem, 0, last_dot);

  std::string_view index_str(stem.data() + last_dot + 1, stem.size() - last_dot - 1);

  auto [p, error] = std::from_chars(index_str.data(), index_str.data() + index_str.size(), info.index);

  if VLIKELY (error == std::errc() && p == index_str.data() + index_str.size()) {
    info.is_valid = true;
  }

  return info;
}

template <typename MutexT>
inline typename TimeRollingFile<MutexT>::FileInfo TimeRollingFile<MutexT>::generate_file_(size_t index) {
  auto now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());

  std::tm time_info;

  if (timezone_ == TimeZone::kTimezoneLocal) {
    time_info = spdlog::details::os::localtime(now);
  } else {
    time_info = spdlog::details::os::gmtime(now);
  }

  FileInfo file_info;

  file_info.index = index;
  file_info.is_valid = true;
  file_info.timestamp.resize(64);

  size_t real_size =
      std::strftime(file_info.timestamp.data(), file_info.timestamp.size(), "%Y-%m-%d_%H-%M-%S", &time_info);

  if VUNLIKELY (real_size == 0) {
    file_info.is_valid = false;
    return file_info;
  }

  file_info.timestamp.resize(real_size);

  file_info.path.reserve(base_filename_.size() + file_info.timestamp.size() + 32);

  file_info.path.append(base_filename_);
  file_info.path.append("/");
  file_info.path.append(file_info.timestamp);
  file_info.path.append(".");
  file_info.path.append(std::to_string(file_info.index));
  file_info.path.append(".log");

  return file_info;
}

template <typename MutexT>
inline void TimeRollingFile<MutexT>::sink_it_(const spdlog::details::log_msg& msg) {
  spdlog::memory_buf_t formatted;

  spdlog::sinks::base_sink<MutexT>::formatter_->format(msg, formatted);

  auto new_size = current_size_ + formatted.size();

  if VUNLIKELY (new_size > max_size_) {
    file_helper_.flush();

    if (file_helper_.size() > 0) {
      rotate_();
      new_size = formatted.size();
    }
  }

  file_helper_.write(formatted);

  current_size_ = new_size;
}

template <typename MutexT>
inline void TimeRollingFile<MutexT>::flush_() {
  file_helper_.flush();
}

template <typename MutexT>
inline void TimeRollingFile<MutexT>::rotate_() {
  size_t next_index = current_index_ + 1;
  FileInfo new_file = generate_file_(next_index);

  if VUNLIKELY (!new_file.is_valid) {
    spdlog::throw_spdlog_ex("Failed to generate valid file info for rotation");
  }

  file_helper_.close();
  file_helper_.open(new_file.path, true);

  current_index_ = next_index;
  file_list_.emplace_back(std::move(new_file));

  while (file_list_.size() > max_files_) {
    (void)spdlog::details::os::remove(file_list_.front().path);
    file_list_.pop_front();
  }

  current_size_ = 0;
}

template <typename FactoryT>
inline std::shared_ptr<spdlog::logger> time_rolling_logger_mt(const std::string& logger_name,
                                                              const spdlog::filename_t& base_filename,
                                                              size_t max_file_size, size_t max_files, TimeZone timezone,
                                                              bool rotate_on_open,
                                                              const spdlog::file_event_handlers& event_handlers) {
  return FactoryT::template create<TimeRollingFile_mt>(logger_name, base_filename, max_file_size, max_files, timezone,
                                                       rotate_on_open, event_handlers);
}

template <typename FactoryT>
inline std::shared_ptr<spdlog::logger> time_rolling_logger_st(const std::string& logger_name,
                                                              const spdlog::filename_t& base_filename,
                                                              size_t max_file_size, size_t max_files, TimeZone timezone,
                                                              bool rotate_on_open,
                                                              const spdlog::file_event_handlers& event_handlers) {
  return FactoryT::template create<TimeRollingFile_st>(logger_name, base_filename, max_file_size, max_files, timezone,
                                                       rotate_on_open, event_handlers);
}

}  // namespace spdlog_custom_sink
}  // namespace vlink

#endif
