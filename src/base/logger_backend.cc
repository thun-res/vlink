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

#include "./base/logger_backend.h"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <deque>
#include <filesystem>
#include <limits>
#include <mutex>
#include <new>
#include <stdexcept>
#include <string>
#include <system_error>
#include <thread>
#include <utility>

#include "./base/memory_pool.h"
#include "./base/memory_resource.h"
#include "./base/timer.h"
#include "./base/utils.h"

#if defined(_WIN32)
#include <io.h>
#include <sys/stat.h>
#else
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace vlink {

static constexpr size_t kMaxTimestampFiles = 10000U;
static constexpr size_t kMaxFixedFiles = 200000U;
static constexpr size_t kMaxPooledRecordSize = 512U;
static constexpr int kOpenRetryCount = 5;
static constexpr std::string_view kBacktraceStart = "****************** Backtrace Start ******************";
static constexpr std::string_view kBacktraceEnd = "****************** Backtrace End ********************";

struct alignas(std::max_align_t) LoggerRecordHeader final {
  size_t size{0U};
};

struct LoggerBackend::LoggerRecord final {
  LoggerRecord(Logger::Level record_level, std::chrono::system_clock::time_point record_timestamp,
               uint64_t record_thread_id, std::string_view record_message) noexcept
      : level(record_level),
        timestamp(record_timestamp),
        thread_id(record_thread_id),
        message(reinterpret_cast<const char*>(this + 1), record_message.size()) {
    if VLIKELY (!record_message.empty()) {
      std::memcpy(const_cast<char*>(message.data()), record_message.data(), record_message.size());
    }
  }

  LoggerRecord(const LoggerRecord&) = delete;

  LoggerRecord& operator=(const LoggerRecord&) = delete;

  static void* operator new(size_t object_size, size_t message_size) {
    constexpr size_t kHeaderSize = sizeof(LoggerRecordHeader);

    if VUNLIKELY (message_size > std::numeric_limits<size_t>::max() - object_size ||
                  object_size + message_size > std::numeric_limits<size_t>::max() - kHeaderSize) {
      throw std::bad_array_new_length();  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
    }

    const size_t allocation_size = kHeaderSize + object_size + message_size;
    const bool pooled = allocation_size <= kMaxPooledRecordSize;
    std::byte* allocation = nullptr;

    if VLIKELY (pooled) {
      allocation =
          static_cast<std::byte*>(MemoryPool::global_instance().allocate(allocation_size, alignof(LoggerRecordHeader)));
    } else {
      allocation = static_cast<std::byte*>(::operator new(allocation_size));
    }

    if VUNLIKELY (allocation == nullptr) {
      throw std::bad_alloc();  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
    }

    auto* header = ::new (allocation) LoggerRecordHeader{allocation_size};

    return reinterpret_cast<std::byte*>(header) + kHeaderSize;
  }

  static void operator delete(void* memory) noexcept {
    auto* allocation = static_cast<std::byte*>(memory) - sizeof(LoggerRecordHeader);
    auto* header = std::launder(reinterpret_cast<LoggerRecordHeader*>(allocation));
    const size_t allocation_size = header->size;
    header->~LoggerRecordHeader();

    if VLIKELY (allocation_size <= kMaxPooledRecordSize) {
      MemoryPool::global_instance().deallocate(allocation, allocation_size, alignof(LoggerRecordHeader));
    } else {
      ::operator delete(allocation);
    }
  }

  static void operator delete(void* memory, size_t) noexcept {  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
    operator delete(memory);                                    // LCOV_EXCL_LINE GCOVR_EXCL_LINE
  }

  Logger::Level level{Logger::kOff};
  std::chrono::system_clock::time_point timestamp{};
  uint64_t thread_id{0U};
  std::string_view message{};
};

struct LoggerFileInfo final {
  std::filesystem::path path;
  std::string timestamp;
  size_t index{0};
  bool is_valid{false};
};

static std::string_view get_backend_level_string(Logger::Level level) noexcept {
  switch (level) {
    case Logger::kTrace:
      return "TRACE";
    case Logger::kDebug:
      return "DEBUG";
    case Logger::kInfo:
      return "INFO ";
    case Logger::kWarn:
      return "WARN ";
    case Logger::kError:
      return "ERROR";
    case Logger::kFatal:
      return "FATAL";
      // LCOV_EXCL_START GCOVR_EXCL_START
    case Logger::kOff:
      return "EMPTY";
    default:
      return "EMPTY";
      // LCOV_EXCL_STOP GCOVR_EXCL_STOP
  }
}

static std::filesystem::path fixed_file_path(const std::filesystem::path& path, size_t index) {
  if (index == 0U) {
    return path;
  }

  auto parent = path.parent_path();
  auto filename = path.stem();
  filename += ".";
  filename += std::to_string(index);
  filename += path.extension();

  return parent / filename;
}

static FILE* open_log_file_once(const std::filesystem::path& path, bool append, int& error_number) {
#if defined(_WIN32)
  FILE* file = nullptr;
  const wchar_t* mode = append ? L"ab" : L"wb";
  error_number = _wfopen_s(&file, path.c_str(), mode);

  if (error_number != 0) {
    file = nullptr;
  }

  return file;
#else
  FILE* file = std::fopen(path.c_str(), append ? "ab" : "wb");
  error_number = file ? 0 : errno;

  return file;
#endif
}

static FILE* open_log_file(const std::filesystem::path& path, bool append, int& error_number) {
  for (int attempt = 0; attempt < kOpenRetryCount; ++attempt) {
    const auto parent = path.parent_path();

    if (!parent.empty()) {
      std::filesystem::create_directories(parent);
    }

    if (!append) {
      FILE* truncate_file = open_log_file_once(path, false, error_number);

      // LCOV_EXCL_START GCOVR_EXCL_START
      if (truncate_file && std::fclose(truncate_file) != 0) {
        truncate_file = nullptr;
        error_number = errno;
      }
      // LCOV_EXCL_STOP GCOVR_EXCL_STOP

      if (!truncate_file) {
        if (attempt + 1 < kOpenRetryCount) {
          std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }

        continue;
      }
    }

    FILE* file = open_log_file_once(path, true, error_number);

    if (file) {
      return file;
    }

    if (attempt + 1 < kOpenRetryCount) {
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
  }

  return nullptr;
}

static bool get_log_file_size(FILE* file, size_t& size, int& error_number) noexcept {
#if defined(_WIN32)
  const int descriptor = ::_fileno(file);
  struct _stat64 status{};

  if (descriptor < 0 || ::_fstat64(descriptor, &status) != 0 || status.st_size < 0) {
    // LCOV_EXCL_START GCOVR_EXCL_START
    error_number = errno;

    return false;
    // LCOV_EXCL_STOP GCOVR_EXCL_STOP
  }
#else
  const int descriptor = ::fileno(file);
  struct stat status{};

  if (descriptor < 0 || ::fstat(descriptor, &status) != 0 || status.st_size < 0) {
    // LCOV_EXCL_START GCOVR_EXCL_START
    error_number = errno;

    return false;
    // LCOV_EXCL_STOP GCOVR_EXCL_STOP
  }
#endif

  const auto file_size = static_cast<uintmax_t>(status.st_size);
  size = file_size > std::numeric_limits<size_t>::max() ? std::numeric_limits<size_t>::max()
                                                        : static_cast<size_t>(file_size);
  error_number = 0;

  return true;
}

static LoggerFileInfo parse_timestamp_file(const std::filesystem::path& path) {
  LoggerFileInfo info;
  info.path = path;

  const auto stem = path.stem().string();

  if (stem.size() < 21U) {
    return info;
  }

  const auto last_dot = stem.rfind('.');

  if (last_dot == std::string::npos || last_dot != 19U) {
    return info;
  }

  info.timestamp.assign(stem, 0, last_dot);
  std::string_view index_string(stem.data() + last_dot + 1U, stem.size() - last_dot - 1U);
  auto [end, error] = std::from_chars(index_string.data(), index_string.data() + index_string.size(), info.index);
  info.is_valid = error == std::errc() && end == index_string.data() + index_string.size();

  return info;
}

struct LoggerBackend::Impl final {
  Config config;
  ErrorHandler error_handler;
  ConsoleWriter console_writer;

  std::filesystem::path base_path;
  std::filesystem::path current_path;
  size_t current_size{0};
  size_t current_index{0};
  FILE* file{nullptr};
#ifdef VLINK_ENABLE_BASE_MEMORY_RESOURCE
  std::pmr::deque<LoggerFileInfo> timestamp_files{&MemoryResource::global_instance()};
#else
  std::deque<LoggerFileInfo> timestamp_files;
#endif

  int64_t cached_seconds{std::numeric_limits<int64_t>::min()};
  char timestamp_prefix[32]{};
  size_t timestamp_size{0};
#ifdef VLINK_ENABLE_BASE_MEMORY_RESOURCE
  std::pmr::string format_buffer{&MemoryResource::global_instance()};
#else
  std::string format_buffer;
#endif

  Timer flush_timer;
  std::atomic_bool accepting{true};
  std::atomic_bool has_error{false};
  std::mutex stopped_mtx;

  bool backtrace_enabled{false};
  size_t backtrace_capacity{0};
#ifdef VLINK_ENABLE_BASE_MEMORY_RESOURCE
  std::pmr::deque<std::unique_ptr<LoggerRecord>> backtrace{&MemoryResource::global_instance()};
#else
  std::deque<std::unique_ptr<LoggerRecord>> backtrace;
#endif
};

void LoggerBackend::start_backend() {
  if (impl_->config.max_file_size == 0U) {
    throw std::invalid_argument("logger backend: max_file_size cannot be zero");
  }

  const size_t max_supported_files = impl_->config.fixed_filename ? kMaxFixedFiles : kMaxTimestampFiles;

  if (impl_->config.max_files > max_supported_files) {
    throw std::invalid_argument("logger backend: max_files exceeds the supported limit");
  }

  if (!impl_->config.fixed_filename && impl_->config.max_files == 0U) {
    throw std::invalid_argument("logger backend: max_files cannot be zero for timestamp rotation");
  }

  if (impl_->config.fixed_filename) {
    initialize_fixed();
  } else {
    initialize_timestamped();
  }

  set_name("VLinkLogger");
  set_strategy(impl_->config.block_when_full ? MessageLoop::kBlockStrategy : MessageLoop::kPopStrategy);

  if (!async_run()) {
    throw std::runtime_error("logger backend: failed to start message loop");  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
  }

  if (impl_->config.flush_interval_ms > 0U) {
    impl_->flush_timer.start();
  }
}

void LoggerBackend::initialize_fixed() {
  impl_->base_path /= impl_->config.app_name + ".log";
  impl_->current_path = impl_->base_path;

  open_current(true);

  if (!impl_->config.append && impl_->current_size > 0U) {
    rotate_fixed();
  }
}

void LoggerBackend::initialize_timestamped() {
  if (std::filesystem::exists(impl_->base_path) && !std::filesystem::is_directory(impl_->base_path)) {
    impl_->base_path += "_dir";
  }

  std::filesystem::create_directories(impl_->base_path);

  for (const auto& entry : std::filesystem::directory_iterator(impl_->base_path)) {
    if (entry.is_regular_file() && entry.path().extension() == ".log") {
      auto file_info = parse_timestamp_file(entry.path());

      if (file_info.is_valid) {
        impl_->timestamp_files.emplace_back(std::move(file_info));
      }
    }
  }

  if (impl_->timestamp_files.empty()) {
    rotate_timestamped();
    return;
  }

  std::sort(impl_->timestamp_files.begin(), impl_->timestamp_files.end(),
            [](const LoggerFileInfo& lhs, const LoggerFileInfo& rhs) {
              if (lhs.index != rhs.index) {
                return lhs.index < rhs.index;
              }

              return lhs.timestamp < rhs.timestamp;
            });

  while (impl_->timestamp_files.size() > impl_->config.max_files) {
    std::error_code error;
    (void)std::filesystem::remove(impl_->timestamp_files.front().path, error);

    if (error) {
      break;  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
    }

    impl_->timestamp_files.pop_front();
  }

  impl_->current_index = impl_->timestamp_files.back().index;
  impl_->current_path = impl_->timestamp_files.back().path;

  if (impl_->config.append) {
    try {
      open_current(true);
      // LCOV_EXCL_START GCOVR_EXCL_START
    } catch (const std::exception&) {
      rotate_timestamped();
    }
    // LCOV_EXCL_STOP GCOVR_EXCL_STOP
  } else {
    rotate_timestamped();
  }
}

void LoggerBackend::open_current(bool append) {
  close_current();

  int error_number = 0;
  impl_->file = open_log_file(impl_->current_path, append, error_number);

  if (!impl_->file) {
    throw std::runtime_error("logger backend: failed to open log file " + impl_->current_path.string() + ": " +
                             std::error_code(error_number, std::generic_category()).message());
  }

  if (!get_log_file_size(impl_->file, impl_->current_size, error_number)) {
    // LCOV_EXCL_START GCOVR_EXCL_START
    close_current();
    throw std::runtime_error("logger backend: failed to query log file size " + impl_->current_path.string() + ": " +
                             std::error_code(error_number, std::generic_category()).message());
    // LCOV_EXCL_STOP GCOVR_EXCL_STOP
  }
}

void LoggerBackend::close_current() noexcept {
  if (impl_->file) {
    std::fclose(impl_->file);
    impl_->file = nullptr;
  }
}

void LoggerBackend::rotate_fixed() {
  close_current();

  for (size_t index = impl_->config.max_files; index > 0U; --index) {
    const auto source = fixed_file_path(impl_->base_path, index - 1U);
    std::error_code exists_error;
    const bool source_exists = std::filesystem::exists(source, exists_error);

    if (exists_error) {
      // LCOV_EXCL_START GCOVR_EXCL_START
      impl_->current_path = impl_->base_path;
      open_current(true);
      throw std::runtime_error("logger backend: failed to inspect " + source.string() + ": " + exists_error.message());
      // LCOV_EXCL_STOP GCOVR_EXCL_STOP
    }

    if (!source_exists) {
      continue;
    }

    const auto target = fixed_file_path(impl_->base_path, index);
    std::error_code error;
    std::filesystem::remove(target, error);
    error.clear();
    std::filesystem::rename(source, target, error);

    if (error) {
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
      error.clear();
      std::filesystem::remove(target, error);
      error.clear();
      std::filesystem::rename(source, target, error);
    }

    if (error) {
      impl_->current_path = impl_->base_path;
      open_current(true);
      throw std::runtime_error("logger backend: failed to rotate " + source.string() + " to " + target.string() + ": " +
                               error.message());
    }
  }

  impl_->current_path = impl_->base_path;
  open_current(false);
}

void LoggerBackend::rotate_timestamped() {
  const auto now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
  std::tm time_info{};
  bool converted = false;

#if defined(_WIN32)
  if (impl_->config.use_utc) {
    converted = gmtime_s(&time_info, &now) == 0;
  } else {
    converted = localtime_s(&time_info, &now) == 0;
  }
#else
  if (impl_->config.use_utc) {
    converted = gmtime_r(&now, &time_info) != nullptr;
  } else {
    converted = localtime_r(&now, &time_info) != nullptr;
  }
#endif

  if VUNLIKELY (!converted) {
    throw std::runtime_error(  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
        "logger backend: failed to convert timestamped log filename time");
  }

  char timestamp[32];
  const size_t size = std::strftime(timestamp, sizeof(timestamp), "%Y-%m-%d_%H-%M-%S", &time_info);

  if (size == 0U) {
    throw std::runtime_error(  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
        "logger backend: failed to generate timestamped log filename");
  }

  LoggerFileInfo new_file;
  new_file.timestamp.assign(timestamp, size);

  if (impl_->current_index == std::numeric_limits<size_t>::max()) {
    throw std::runtime_error("logger backend: timestamped log file index overflow");
  }

  new_file.index = impl_->current_index + 1U;
  new_file.path = impl_->base_path / (new_file.timestamp + "." + std::to_string(new_file.index) + ".log");

  impl_->current_index = new_file.index;
  impl_->current_path = new_file.path;

  open_current(false);
  impl_->timestamp_files.emplace_back(std::move(new_file));

  while (impl_->timestamp_files.size() > impl_->config.max_files) {
    std::error_code error;
    (void)std::filesystem::remove(impl_->timestamp_files.front().path, error);

    if (error) {
      break;
    }

    impl_->timestamp_files.pop_front();
  }
}

void LoggerBackend::flush_output() {
  if VUNLIKELY (std::fflush(impl_->file) != 0) {
    const int error_number = errno;
    throw std::runtime_error("logger backend: failed to flush log file " + impl_->current_path.string() + ": " +
                             std::error_code(error_number, std::generic_category()).message());
  }
}

void LoggerBackend::write_output(std::string_view data) {
  const auto exceeds_limit = [this, data](size_t current_size) {
    return data.size() > impl_->config.max_file_size || current_size > impl_->config.max_file_size - data.size();
  };

  const bool needs_rotation = exceeds_limit(impl_->current_size);
  size_t new_size = needs_rotation ? impl_->config.max_file_size : impl_->current_size + data.size();

  if VUNLIKELY (needs_rotation) {
    flush_output();

    size_t real_size = 0U;
    int error_number = 0;

    if (!get_log_file_size(impl_->file, real_size, error_number)) {
      // LCOV_EXCL_START GCOVR_EXCL_START
      throw std::runtime_error("logger backend: failed to query active log file size " + impl_->current_path.string() +
                               ": " + std::error_code(error_number, std::generic_category()).message());
      // LCOV_EXCL_STOP GCOVR_EXCL_STOP
    }

    if (real_size > 0U && exceeds_limit(real_size)) {
      if (impl_->config.fixed_filename) {
        rotate_fixed();
      } else {
        rotate_timestamped();
      }

      new_size = data.size();
    } else {
      new_size = real_size + data.size();
    }
  }

  size_t written = 0U;

#if defined(_WIN32)
  written = ::_fwrite_nolock(data.data(), sizeof(char), data.size(), impl_->file);
#elif defined(__linux__) && (!defined(__ANDROID__) || (defined(__ANDROID_API__) && __ANDROID_API__ >= 28))
  written = ::fwrite_unlocked(data.data(), sizeof(char), data.size(), impl_->file);
#else
  written = std::fwrite(data.data(), sizeof(char), data.size(), impl_->file);
#endif

  if VUNLIKELY (written != data.size()) {
    const int error_number = errno;
    throw std::runtime_error("logger backend: failed to write log file " + impl_->current_path.string() + ": " +
                             std::error_code(error_number, std::generic_category()).message());
  }

  impl_->current_size = new_size;
}

void LoggerBackend::update_timestamp(int64_t seconds) {
  if constexpr (!std::numeric_limits<std::time_t>::is_signed) {
    if VUNLIKELY (seconds < 0) {
      throw std::runtime_error("logger backend: timestamp is outside time_t range");
    }
  }

  const auto time = static_cast<std::time_t>(seconds);

  if constexpr (sizeof(std::time_t) < sizeof(int64_t)) {
    if VUNLIKELY (static_cast<int64_t>(time) != seconds) {
      throw std::runtime_error("logger backend: timestamp is outside time_t range");
    }
  }

  std::tm time_info{};
  bool converted = false;

#if defined(_WIN32)
  if (impl_->config.use_utc) {
    converted = gmtime_s(&time_info, &time) == 0;
  } else {
    converted = localtime_s(&time_info, &time) == 0;
  }
#else
  if (impl_->config.use_utc) {
    converted = gmtime_r(&time, &time_info) != nullptr;
  } else {
    converted = localtime_r(&time, &time_info) != nullptr;
  }
#endif

  if VUNLIKELY (!converted) {
    throw std::runtime_error("logger backend: failed to convert log timestamp");  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
  }

  impl_->timestamp_size =
      std::strftime(impl_->timestamp_prefix, sizeof(impl_->timestamp_prefix), "%m-%d %H:%M:%S.", &time_info);

  if (impl_->timestamp_size == 0U) {
    throw std::runtime_error("logger backend: failed to format timestamp");  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
  }

  impl_->cached_seconds = seconds;
}

std::string_view LoggerBackend::format(const LoggerRecord& record) {
  const auto milliseconds =
      std::chrono::duration_cast<std::chrono::milliseconds>(record.timestamp.time_since_epoch()).count();
  auto seconds = milliseconds / 1000;
  int millisecond = static_cast<int>(milliseconds % 1000);

  if VUNLIKELY (millisecond < 0) {
    // LCOV_EXCL_START GCOVR_EXCL_START
    millisecond += 1000;
    --seconds;
    // LCOV_EXCL_STOP GCOVR_EXCL_STOP
  }

  if VUNLIKELY (seconds != impl_->cached_seconds) {
    update_timestamp(seconds);
  }

  impl_->format_buffer.clear();
  impl_->format_buffer.append(impl_->timestamp_prefix, impl_->timestamp_size);

  char number_buffer[32];
  auto [millisecond_end, millisecond_error] =
      std::to_chars(number_buffer, number_buffer + sizeof(number_buffer), millisecond);

  if VUNLIKELY (millisecond_error != std::errc()) {
    throw std::runtime_error("logger backend: failed to format milliseconds");  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
  }

  const auto millisecond_size = static_cast<size_t>(millisecond_end - number_buffer);
  impl_->format_buffer.append(3U - millisecond_size, '0');
  impl_->format_buffer.append(number_buffer, millisecond_size);

  if VUNLIKELY (impl_->config.use_utc) {
    impl_->format_buffer.append(" UTC");
  }

  impl_->format_buffer.append(" @");
  auto [thread_end, thread_error] =
      std::to_chars(number_buffer, number_buffer + sizeof(number_buffer), record.thread_id);

  if VUNLIKELY (thread_error != std::errc()) {
    throw std::runtime_error("logger backend: failed to format thread id");  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
  }

  impl_->format_buffer.append(number_buffer, static_cast<size_t>(thread_end - number_buffer));
  impl_->format_buffer.append(" - ");
  impl_->format_buffer.append(get_backend_level_string(record.level));
  impl_->format_buffer.append(" - ");
  impl_->format_buffer.append(record.message);
  impl_->format_buffer.push_back('\n');

  return impl_->format_buffer;
}

void LoggerBackend::fail(std::string_view message) noexcept {
  bool expected = false;

  if (impl_->has_error.compare_exchange_strong(expected, true, std::memory_order_acq_rel, std::memory_order_acquire) &&
      impl_->error_handler) {
    try {
      impl_->error_handler(message);
    } catch (const std::exception& error) {
      std::fprintf(stderr, "LoggerBackend error handler failed: %s\n", error.what());
      std::fflush(stderr);
    } catch (...) {
      std::fputs("LoggerBackend error handler failed with an unknown exception.\n", stderr);
      std::fflush(stderr);
    }
  }
}

void LoggerBackend::flush_file() noexcept {
  if VUNLIKELY (impl_->has_error.load(std::memory_order_acquire)) {
    return;
  }

  try {
    flush_output();
  } catch (const std::exception& error) {
    fail(error.what());
    // LCOV_EXCL_START GCOVR_EXCL_START
  } catch (...) {
    fail("logger backend: failed to flush log file");
  }
  // LCOV_EXCL_STOP GCOVR_EXCL_STOP
}

void LoggerBackend::write(std::unique_ptr<LoggerRecord>&& record) noexcept {
  if VUNLIKELY (impl_->has_error.load(std::memory_order_acquire)) {
    return;
  }

  try {
    const auto level = record->level;
    bool wrote = false;

    if VUNLIKELY (impl_->backtrace_enabled) {
      if VUNLIKELY (level >= Logger::kWarn) {
        const auto formatted = format(*record);
        write_output(formatted);

        if (impl_->console_writer) {
          impl_->console_writer(level, formatted.substr(0U, formatted.size() - 1U));
        }

        wrote = true;
      }

      if VLIKELY (impl_->backtrace_capacity > 0U) {
        impl_->backtrace.emplace_back(std::move(record));

        if VLIKELY (impl_->backtrace.size() > impl_->backtrace_capacity) {
          impl_->backtrace.pop_front();
        }
      }
    } else {
      write_output(format(*record));
      wrote = true;
    }

    if (wrote && impl_->config.flush_interval_ms == 0U) {
      flush_output();
    } else if VUNLIKELY (wrote && level >= Logger::kError) {
      flush_output();
    }
  } catch (const std::exception& error) {
    fail(error.what());
  } catch (...) {
    fail("logger backend: failed to write a log record");
  }
}

void LoggerBackend::barrier(Callback&& callback) noexcept {
  auto invoke_callback = [this, &callback] {
    try {
      callback();
      // LCOV_EXCL_START GCOVR_EXCL_START
    } catch (const std::exception& error) {
      fail(error.what());
    } catch (...) {
      fail("logger backend: control callback failed");
    }
    // LCOV_EXCL_STOP GCOVR_EXCL_STOP
  };

  if (is_in_same_thread()) {
    invoke_callback();
    return;
  }

  if VUNLIKELY (is_ready_to_quit() || !is_running()) {
    wait_for_quit(Timer::kInfinite, false);
    std::lock_guard lock(impl_->stopped_mtx);
    invoke_callback();

    return;
  }

  try {
    PostTaskOptions options;
    options.overflow_policy = TaskOverflowPolicy::kBlock;
    options.drop_policy = TaskDropPolicy::kProtected;

    auto handle = post_task_handle([&invoke_callback] { invoke_callback(); }, options);
    (void)handle.wait();

    if VUNLIKELY (handle.state() != TaskExecutionState::kCompleted) {
      wait_for_quit(Timer::kInfinite, false);
      std::lock_guard lock(impl_->stopped_mtx);
      invoke_callback();
    }
    // LCOV_EXCL_START GCOVR_EXCL_START
  } catch (const std::exception& error) {
    fail(error.what());
  } catch (...) {
    fail("logger backend: failed to enqueue a control barrier");
  }
  // LCOV_EXCL_STOP GCOVR_EXCL_STOP
}

LoggerBackend::LoggerBackend(Config&& config, ErrorHandler&& error_handler, ConsoleWriter&& console_writer)
    : impl_(std::make_unique<Impl>()) {
  (void)MemoryPool::global_instance();

  impl_->config = std::move(config);
  impl_->config.queue_size = std::max(impl_->config.queue_size, size_t{1});
  impl_->error_handler = std::move(error_handler);
  impl_->console_writer = std::move(console_writer);
  impl_->base_path = impl_->config.log_path;
  impl_->format_buffer.reserve(4096U);

  if (impl_->config.flush_interval_ms > 0U) {
    if (!impl_->flush_timer.attach(this)) {
      throw std::runtime_error("logger backend: failed to attach flush timer");  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
    }

    impl_->flush_timer.set_interval(impl_->config.flush_interval_ms);
    impl_->flush_timer.set_loop_count(Timer::kInfinite);
    impl_->flush_timer.set_callback([this] { flush_file(); });
  }

  try {
    start_backend();
  } catch (...) {
    close_current();
    throw;
  }
}

LoggerBackend::~LoggerBackend() {
  impl_->accepting.store(false, std::memory_order_release);
  impl_->flush_timer.stop();

#ifdef _WIN32
  (void)wait_for_quit(0, false);
#endif

  barrier([this] { flush_file(); });

  quit();
  wait_for_quit(Timer::kInfinite, false);

  close_current();
}

bool LoggerBackend::log(Logger::Level level, std::string_view message) noexcept {
  if VUNLIKELY (!impl_->accepting.load(std::memory_order_acquire) || impl_->has_error.load(std::memory_order_acquire) ||
                level >= Logger::kOff) {
    return false;
  }

  try {
    static thread_local const uint64_t kThreadId = Utils::get_native_thread_id();

    auto record = std::unique_ptr<LoggerRecord>(
        new (message.size()) LoggerRecord(level, std::chrono::system_clock::now(), kThreadId, message));

    if VUNLIKELY (is_in_same_thread()) {
      write(std::move(record));
      return !impl_->has_error.load(std::memory_order_acquire);
    }

    bool protected_record = impl_->config.block_when_full;

    if VUNLIKELY (level >= Logger::kError) {
      protected_record = true;
    }

    if VUNLIKELY (protected_record && !impl_->config.block_when_full) {
      Callback task = [this, record = std::move(record)]() mutable { write(std::move(record)); };

      if (post_untracked_task(std::move(task), TaskOverflowPolicy::kUseDispatcherStrategy,
                              TaskDropPolicy::kProtected)) {
        return true;
      }

      return post_untracked_task(std::move(task), TaskOverflowPolicy::kBlock, TaskDropPolicy::kProtected);
    }

    return post_untracked_task(
        [this, record = std::move(record)]() mutable { write(std::move(record)); },
        protected_record ? TaskOverflowPolicy::kBlock : TaskOverflowPolicy::kUseDispatcherStrategy,
        protected_record ? TaskDropPolicy::kProtected : TaskDropPolicy::kDroppable);
    // LCOV_EXCL_START GCOVR_EXCL_START
  } catch (const std::exception& error) {
    fail(error.what());
    return false;
  } catch (...) {
    fail("logger backend: failed to enqueue a log record");
    return false;
  }
  // LCOV_EXCL_STOP GCOVR_EXCL_STOP
}

void LoggerBackend::flush() noexcept {
  barrier([this] { flush_file(); });
}

void LoggerBackend::enable_backtrace(size_t size) noexcept {
  barrier([this, size] {
    impl_->backtrace.clear();
    impl_->backtrace_capacity = size;
    impl_->backtrace_enabled = true;
  });
}

void LoggerBackend::disable_backtrace() noexcept {
  barrier([this] {
    impl_->backtrace_enabled = false;
    impl_->backtrace_capacity = 0U;
    impl_->backtrace.clear();
  });
}

void LoggerBackend::dump_backtrace(const ConsoleWriter& console_writer) noexcept {
  barrier([this, &console_writer] {
    if VUNLIKELY (impl_->has_error.load(std::memory_order_acquire)) {
      return;
    }

    if (!impl_->backtrace_enabled || impl_->backtrace.empty()) {
      return;
    }

    try {
      auto dump_record = [this, &console_writer](const LoggerRecord& record) {
        const auto formatted = format(record);
        write_output(formatted);

        if (console_writer) {
          console_writer(record.level, formatted.substr(0U, formatted.size() - 1U));
        }
      };

      const uint64_t thread_id = Utils::get_native_thread_id();
      auto marker = std::unique_ptr<LoggerRecord>(new (kBacktraceStart.size()) LoggerRecord(
          Logger::kInfo, std::chrono::system_clock::now(), thread_id, kBacktraceStart));
      dump_record(*marker);

      while (!impl_->backtrace.empty()) {
        dump_record(*impl_->backtrace.front());
        impl_->backtrace.pop_front();
      }

      marker = std::unique_ptr<LoggerRecord>(new (kBacktraceEnd.size()) LoggerRecord(
          Logger::kInfo, std::chrono::system_clock::now(), thread_id, kBacktraceEnd));
      dump_record(*marker);
      flush_output();
    } catch (const std::exception& error) {
      fail(error.what());
    } catch (...) {
      fail("logger backend: failed to dump backtrace");
    }
  });
}

bool LoggerBackend::has_error() const noexcept { return impl_->has_error.load(std::memory_order_acquire); }

size_t LoggerBackend::get_max_task_count() const { return impl_->config.queue_size; }

}  // namespace vlink
