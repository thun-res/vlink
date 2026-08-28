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

#include "./base/logger.h"

#include <atomic>
#include <charconv>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <utility>

#include "./base/cached_timestamp.h"
#include "./base/logger_plugin_interface.h"
#include "./base/memory_pool.h"
#include "./base/utils.h"
#include "./vlink/version.h"

#if defined(VLINK_ENABLE_LOG_BACKEND)
#include "./base/logger_backend.h"
#elif defined(__ANDROID__)
#include <android/log.h>
#elif defined(__QNX__)
#include <process.h>
#include <sys/slog2.h>
#elif defined(__linux__)
#include <linux/kernel.h>
#define VLINK_KMSG_DEV_PATH "/dev/kmsg"
#endif

namespace vlink {

[[maybe_unused]] static constexpr size_t kDefaultWriteDepth = 1024L * 8;
[[maybe_unused]] static constexpr size_t kDefaultLogMaxSize = 1024L * 1024L * 10U;
[[maybe_unused]] static constexpr size_t kDefaultLogMaxCount = 10U;
[[maybe_unused]] static constexpr int kDefaultLogFlushDelay = 500;

[[maybe_unused]] static std::string get_current_date(bool use_utc = false) {
  auto now = std::chrono::system_clock::now();
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

  char buffer[32];
  std::strftime(buffer, sizeof(buffer), "%Y-%m-%d", &now_tm);
  return std::string(buffer);
}

[[maybe_unused]] static std::string_view get_current_time(bool use_utc = false) {
  thread_local CachedTimestamp cache;
  return cache.get("%02d-%02d %02d:%02d:%02d.%03d", use_utc);
}

[[maybe_unused]] static constexpr std::string_view get_log_level_str(Logger::Level level) {
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

[[maybe_unused]] static int get_log_level(const std::string& key) {
  std::string str_value = Utils::get_env(key);

  if (str_value.empty()) {
    return -1;
  }

  if (str_value == "Trace" || str_value == "TRACE" || str_value == "trace") {
    return Logger::kTrace;
  } else if (str_value == "Debug" || str_value == "DEBUG" || str_value == "debug") {
    return Logger::kDebug;
  } else if (str_value == "Info" || str_value == "INFO" || str_value == "info") {
    return Logger::kInfo;
  } else if (str_value == "Warn" || str_value == "WARN" || str_value == "warn") {
    return Logger::kWarn;
  } else if (str_value == "Error" || str_value == "ERROR" || str_value == "error") {
    return Logger::kError;
  } else if (str_value == "Fatal" || str_value == "FATAL" || str_value == "fatal") {
    return Logger::kFatal;
  } else if (str_value == "Off" || str_value == "OFF" || str_value == "off") {
    return Logger::kOff;
  }

  int value = -1;

  auto [p, error] = std::from_chars(str_value.data(), str_value.data() + str_value.size(), value);

  if VUNLIKELY (error != std::errc()) {
    return -1;
  }

  if VUNLIKELY (value < 0 || value > Logger::kOff) {
    return Logger::kOff;
  }

  return value;
}

[[maybe_unused]] static std::string_view get_thread_id_str() {
  thread_local char buffer[32];
  thread_local bool initialized = false;

  if VUNLIKELY (!initialized) {
    auto [p, ec] = std::to_chars(buffer, buffer + sizeof(buffer) - 1, Utils::get_native_thread_id());

    if (ec == std::errc()) {
      *p = '\0';
    } else {
      std::snprintf(  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
          buffer, sizeof(buffer), "%llu",
          static_cast<unsigned long long>(Utils::get_native_thread_id()));  // NOLINT(runtime/int, google-runtime-int)
                                                                            // // LCOV_EXCL_LINE GCOVR_EXCL_LINE
    }

    initialized = true;
  }

  return buffer;
}

static bool& is_logging_on_current_thread() noexcept {
  static thread_local bool is_logging{false};
  return is_logging;
}

static bool& is_plugin_logging_on_current_thread() noexcept {
  static thread_local bool is_plugin_logging{false};
  return is_plugin_logging;
}

[[maybe_unused]] static std::mutex& get_print_mtx() {
  static std::mutex print_mtx;
  return print_mtx;
}

template <uint8_t PrefixSizeT, uint8_t SuffixSizeT>
[[maybe_unused]] static void print_with_color(const char (&prefix)[PrefixSizeT], std::string_view log,
                                              const char (&suffix)[SuffixSizeT], FILE* file, bool in_order,
                                              bool force_flush) {
  std::unique_lock lock(get_print_mtx(), std::defer_lock);

  if (in_order) {
    lock.lock();
  }

  if constexpr (PrefixSizeT > 1) {
    std::fwrite(prefix, sizeof(char), PrefixSizeT - 1, file);
  }

  if VLIKELY (!log.empty()) {
    std::fwrite(log.data(), sizeof(char), log.size(), file);
  }

  if constexpr (SuffixSizeT > 1) {
    std::fwrite(suffix, sizeof(char), SuffixSizeT - 1, file);
  }

  if (in_order && force_flush) {
    std::fflush(file);
  }
}

// LoggerGlobal
struct LoggerGlobal final {  // NOLINT(clang-analyzer-optin.performance.Padding)
  std::atomic_bool is_busy{false};
  std::atomic_bool is_initializing{false};
  std::atomic_bool is_stopping{false};

  std::string app_name;
  std::string log_path;
  std::string version_log;
  std::atomic<int> console_level{Logger::kDebug};
  std::atomic<int> file_level{Logger::kDebug};
  std::atomic_bool console_in_order{true};
  std::atomic_bool console_level_by_user{false};
  std::atomic_bool file_level_by_user{false};
  std::atomic_bool console_format_enable{false};
  std::atomic_bool utc_enable{false};
  std::mutex level_mtx;
  std::atomic_bool has_console_callback{false};
  std::atomic_bool has_file_callback{false};
  // Protected by callback_mtx (shared on read/invoke, exclusive on register).
  Logger::Callback console_callback;
  Logger::Callback file_callback;
  mutable std::shared_mutex callback_mtx;
  std::atomic<std::ios_base::fmtflags> stream_flags{std::ios_base::dec | std::ios_base::skipws};
  std::atomic<int> stream_precision{6};
  std::atomic<int> stream_width{0};

  static LoggerGlobal& get() {
    static LoggerGlobal instance;
    return instance;
  }

 private:
  LoggerGlobal() { MemoryPool::global_instance(); }
};

// Logger::Impl
struct Logger::Impl final {  // NOLINT(clang-analyzer-optin.performance.Padding)
  enum class PluginInitState : uint8_t {
    kPending,
    kInitializing,
    kComplete,
  };

  struct PluginFinalizer final {
    Logger& instance;

    ~PluginFinalizer() {
      auto& global_instance = LoggerGlobal::get();

      global_instance.is_stopping.store(true, std::memory_order_release);

#if defined(_WIN32)
      if (Utils::is_terminating()) {
        return;
      }
#endif

      instance.impl_->is_enable_file_channel.store(false, std::memory_order_release);
      instance.impl_->interface->flush();
      instance.impl_->interface.reset();
      instance.impl_->plugin.clear();
    }
  };

  std::atomic_bool disk_emergency{false};
  std::atomic_bool is_enable_backtrace{false};
  std::atomic_bool is_enable_file_channel{false};
  std::mutex backtrace_mtx;

  std::atomic<PluginInitState> plugin_init_state{PluginInitState::kPending};
  std::string plugin_name;
  Plugin plugin;
  std::shared_ptr<LoggerPluginInterface> interface;

#if defined(VLINK_ENABLE_LOG_BACKEND)
  std::unique_ptr<LoggerBackend> backend;
#elif defined(__ANDROID__)
#elif defined(__QNX__)
  slog2_buffer_t slog2_buffer{nullptr};
#elif defined(__linux__)
  std::ofstream kmsg_dev;
  std::mutex file_mtx;
#endif
};

// Logger
void Logger::init(const std::string& app_name, const std::string& log_path) noexcept {
  static auto& global_instance = LoggerGlobal::get();

  if (!app_name.empty()) {
    global_instance.app_name = app_name;

    if (!log_path.empty()) {
      global_instance.log_path = log_path;
    }
  }

  Logger::get();
}

Logger& Logger::get() noexcept {
  static Logger instance;

  if VUNLIKELY (!instance.impl_->plugin_name.empty() &&
                instance.impl_->plugin_init_state.load(std::memory_order_acquire) != Impl::PluginInitState::kComplete) {
    auto expected = Impl::PluginInitState::kPending;

    if (instance.impl_->plugin_init_state.compare_exchange_strong(
            expected, Impl::PluginInitState::kInitializing, std::memory_order_acq_rel, std::memory_order_acquire)) {
      auto& global_instance = LoggerGlobal::get();
      bool plugin_inited = false;

      global_instance.is_busy.store(true, std::memory_order_release);
      instance.impl_->plugin.set_log_level(kOff);
      instance.impl_->interface = instance.impl_->plugin.load<LoggerPluginInterface>(instance.impl_->plugin_name, 1, 0);

      if (instance.impl_->interface) {
        plugin_inited = instance.impl_->interface->init(global_instance.app_name);
      }

      if (plugin_inited) {
        static Logger::Impl::PluginFinalizer plugin_finalizer{instance};
        (void)plugin_finalizer;

        instance.impl_->is_enable_file_channel.store(true, std::memory_order_release);
        std::cout << "Successfully loaded plugin for env 'VLINK_LOG_PLUGIN', libname: " << instance.impl_->plugin_name
                  << std::endl;

        if (kInfo >= global_instance.file_level.load(std::memory_order_acquire)) {
          instance.write_to_file(kInfo, global_instance.version_log);
        }
      } else {
        instance.impl_->interface.reset();
        instance.impl_->plugin.clear();
        std::cerr << "Failed to load plugin for env 'VLINK_LOG_PLUGIN', libname: " << instance.impl_->plugin_name
                  << std::endl;
      }

      global_instance.is_busy.store(false, std::memory_order_release);
      instance.impl_->plugin_init_state.store(Impl::PluginInitState::kComplete, std::memory_order_release);
    }
  }

  return instance;
}

void Logger::flush() noexcept {
  static Logger& instance = Logger::get();

  if (!instance.impl_->is_enable_file_channel.load(std::memory_order_acquire)) {
    return;
  }

  if VUNLIKELY (instance.impl_->interface) {
    auto& is_plugin_logging = is_plugin_logging_on_current_thread();

    if VUNLIKELY (is_plugin_logging) {
      return;
    }

    is_plugin_logging = true;
    instance.impl_->interface->flush();
    is_plugin_logging = false;

    return;
  }

  if VUNLIKELY (instance.impl_->disk_emergency.load(std::memory_order_acquire)) {
    return;  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
  }

#if defined(VLINK_ENABLE_LOG_BACKEND)
  instance.impl_->backend->flush();
#endif
}

void Logger::register_console_handler(Callback&& callback) noexcept {
  auto& global_instance = LoggerGlobal::get();

  std::unique_lock lock(global_instance.callback_mtx);

  global_instance.console_callback = std::move(callback);
  global_instance.has_console_callback.store(static_cast<bool>(global_instance.console_callback),
                                             std::memory_order_release);
}

void Logger::register_file_handler(Callback&& callback) noexcept {
  auto& global_instance = LoggerGlobal::get();

  std::unique_lock lock(global_instance.callback_mtx);

  global_instance.file_callback = std::move(callback);
  global_instance.has_file_callback.store(static_cast<bool>(global_instance.file_callback), std::memory_order_release);
}

void Logger::set_console_level(Level level) noexcept {
  auto& global_instance = LoggerGlobal::get();
  std::lock_guard lock(global_instance.level_mtx);

  global_instance.console_level.store(level, std::memory_order_release);
  global_instance.console_level_by_user.store(true, std::memory_order_release);
}

void Logger::set_file_level(Level level) noexcept {
  auto& global_instance = LoggerGlobal::get();
  std::lock_guard lock(global_instance.level_mtx);

  global_instance.file_level.store(level, std::memory_order_release);
  global_instance.file_level_by_user.store(true, std::memory_order_release);
}

void Logger::set_console_fmt_enable(bool enable) noexcept {
  LoggerGlobal::get().console_format_enable.store(enable, std::memory_order_release);
}

Logger::Level Logger::get_console_level() noexcept {
  return static_cast<Logger::Level>(LoggerGlobal::get().console_level.load(std::memory_order_acquire));
}

Logger::Level Logger::get_file_level() noexcept {
  return static_cast<Logger::Level>(LoggerGlobal::get().file_level.load(std::memory_order_acquire));
}

bool Logger::get_console_fmt_enable() noexcept {
  return LoggerGlobal::get().console_format_enable.load(std::memory_order_acquire);
}

void Logger::set_stream_flag(std::ios_base::fmtflags flags) noexcept {
  LoggerGlobal::get().stream_flags.store(flags, std::memory_order_release);
}

void Logger::set_stream_precision(int precision) noexcept {
  LoggerGlobal::get().stream_precision.store(precision, std::memory_order_release);
}

void Logger::set_stream_width(int width) noexcept {
  LoggerGlobal::get().stream_width.store(width, std::memory_order_release);
}

std::ios_base::fmtflags Logger::get_stream_flag() noexcept {
  return LoggerGlobal::get().stream_flags.load(std::memory_order_acquire);
}

int Logger::get_stream_precision() noexcept {
  return LoggerGlobal::get().stream_precision.load(std::memory_order_acquire);
}

int Logger::get_stream_width() noexcept { return LoggerGlobal::get().stream_width.load(std::memory_order_acquire); }

void Logger::enable_backtrace(size_t size) noexcept {
  static Logger& instance = Logger::get();

  if VUNLIKELY (instance.impl_->disk_emergency.load(std::memory_order_acquire)) {
    return;  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
  }

  if VUNLIKELY (!instance.impl_->is_enable_file_channel.load(std::memory_order_acquire)) {
    return;  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
  }

  if VUNLIKELY (instance.impl_->interface) {
    return;  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
  }

#if defined(VLINK_ENABLE_LOG_BACKEND)
  std::lock_guard lock(instance.impl_->backtrace_mtx);

  if (instance.impl_->is_enable_backtrace.load(std::memory_order_acquire)) {
    return;  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
  }

  instance.impl_->backend->enable_backtrace(size);

  instance.impl_->is_enable_backtrace.store(true, std::memory_order_release);
#else
  (void)size;
#endif
}

void Logger::disable_backtrace() noexcept {
  static Logger& instance = Logger::get();

  std::lock_guard lock(instance.impl_->backtrace_mtx);

  if (!instance.impl_->is_enable_backtrace.load(std::memory_order_acquire)) {
    return;  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
  }

  if VUNLIKELY (!instance.impl_->is_enable_file_channel.load(std::memory_order_acquire)) {
    return;  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
  }

  if VUNLIKELY (instance.impl_->interface) {
    return;  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
  }

  instance.impl_->is_enable_backtrace.store(false, std::memory_order_release);

#if defined(VLINK_ENABLE_LOG_BACKEND)
  instance.impl_->backend->disable_backtrace();
#else
  (void)instance;
#endif
}

void Logger::dump_backtrace() noexcept {
  static Logger& instance = Logger::get();

  if VUNLIKELY (instance.impl_->disk_emergency.load(std::memory_order_acquire)) {
    return;  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
  }

  std::lock_guard lock(instance.impl_->backtrace_mtx);

  if (!instance.impl_->is_enable_backtrace.load(std::memory_order_acquire)) {
    return;  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
  }

  if VUNLIKELY (!instance.impl_->is_enable_file_channel.load(std::memory_order_acquire)) {
    return;  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
  }

  if VUNLIKELY (instance.impl_->interface) {
    return;  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
  }

#if defined(VLINK_ENABLE_LOG_BACKEND)
  LoggerBackend::ConsoleWriter console_writer = [](Level level, std::string_view log) {
    Logger::write_to_console_line(level, log);
  };
  instance.impl_->backend->dump_backtrace(console_writer);
#else
  (void)instance;
#endif
}

bool Logger::is_busy() noexcept { return LoggerGlobal::get().is_busy.load(std::memory_order_acquire); }

bool Logger::is_writable(Level level) noexcept {
  if VUNLIKELY (level >= kOff) {
    return false;
  }

  return can_log(level);
}

bool Logger::try_acquire_periodic_log(Level level, int64_t interval_ms,
                                      std::atomic<uint64_t>& last_log_time_ns) noexcept {
  if VUNLIKELY (level >= kFatal) {
    return false;
  }

  if (!is_writable(level)) {
    return false;
  }

  if VUNLIKELY (interval_ms <= 0) {
    return true;
  }

  constexpr uint64_t kNanosecondsPerMillisecond = 1000U * 1000U;
  const auto unsigned_interval_ms = static_cast<uint64_t>(interval_ms);
  const auto interval_ns = unsigned_interval_ms > std::numeric_limits<uint64_t>::max() / kNanosecondsPerMillisecond
                               ? std::numeric_limits<uint64_t>::max()
                               : unsigned_interval_ms * kNanosecondsPerMillisecond;
  static const auto kStartTime = std::chrono::steady_clock::now();
  const auto now_ns =
      static_cast<uint64_t>(
          std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - kStartTime).count()) +
      1U;
  auto last_ns = last_log_time_ns.load(std::memory_order_relaxed);

  for (;;) {
    if VLIKELY (last_ns != 0U) {
      if VUNLIKELY (last_ns >= now_ns) {
        return false;
      }

      if VLIKELY (now_ns - last_ns < interval_ns) {
        return false;
      }
    }

    if VLIKELY (last_log_time_ns.compare_exchange_weak(last_ns, now_ns, std::memory_order_relaxed,
                                                       std::memory_order_relaxed)) {
      return true;
    }
  }
}

Logger::Logger() noexcept {
  static auto& global_instance = LoggerGlobal::get();
  global_instance.is_initializing.store(true, std::memory_order_release);

  auto& is_logging = is_logging_on_current_thread();
  const bool was_logging = is_logging;

  is_logging = true;
  impl_ = std::make_unique<Impl>();
  global_instance.is_busy.store(true, std::memory_order_release);

  int common_level = get_log_level("VLINK_LOG_LEVEL");

  {
    std::lock_guard lock(global_instance.level_mtx);

    if (!global_instance.console_level_by_user.load(std::memory_order_acquire)) {
      int console_level = get_log_level("VLINK_LOG_CONSOLE_LEVEL");

      if (console_level >= 0) {
        global_instance.console_level.store(console_level, std::memory_order_release);
      } else if (common_level >= 0) {
        global_instance.console_level.store(common_level, std::memory_order_release);
      }
    }

    if (!global_instance.file_level_by_user.load(std::memory_order_acquire)) {
      int file_level = get_log_level("VLINK_LOG_FILE_LEVEL");

      if (file_level >= 0) {
        global_instance.file_level.store(file_level, std::memory_order_release);
      } else if (common_level >= 0) {
        global_instance.file_level.store(common_level, std::memory_order_release);
      }
    }
  }

  if (global_instance.console_level.load(std::memory_order_acquire) < kOff ||
      global_instance.file_level.load(std::memory_order_acquire) < kOff) {
    std::string enable_console_unorder = Utils::get_env("VLINK_LOG_CONSOLE_UNORDER");
    global_instance.console_in_order.store(enable_console_unorder != "1", std::memory_order_release);

    std::string enable_utc_str = Utils::get_env("VLINK_LOG_ENABLE_UTC");
    global_instance.utc_enable.store(enable_utc_str == "1", std::memory_order_release);

    if (global_instance.app_name.empty()) {
      global_instance.app_name = Utils::get_app_name();

      if VUNLIKELY (global_instance.app_name.empty()) {
        global_instance.app_name = "unknown";                          // LCOV_EXCL_LINE GCOVR_EXCL_LINE
        std::cerr << "Can not get app name for logger!" << std::endl;  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
      }
    }

    std::string console_format = Utils::get_env("VLINK_LOG_CONSOLE_FMT");
    global_instance.console_format_enable.store(console_format == "1", std::memory_order_release);
  }

  if (global_instance.file_level.load(std::memory_order_acquire) < kOff) {
    global_instance.version_log.reserve(128);

    global_instance.version_log.append("***** ");
    global_instance.version_log.append("[PNAME: ");
    global_instance.version_log.append(global_instance.app_name);
    global_instance.version_log.append("] ");

    global_instance.version_log.append("[PID: ");
    global_instance.version_log.append(Utils::get_pid_str());
    global_instance.version_log.append("] ");

    if (global_instance.utc_enable.load(std::memory_order_acquire)) {
      global_instance.version_log.append("[DATE (UTC): ");
      global_instance.version_log.append(get_current_date(true));
      global_instance.version_log.append("] ");
    } else {
      global_instance.version_log.append("[DATE: ");
      global_instance.version_log.append(get_current_date(false));
      global_instance.version_log.append("] ");
    }

    global_instance.version_log.append("[VERSION: ");
    global_instance.version_log.append(VLINK_VERSION);
    global_instance.version_log.append("] ");
    global_instance.version_log.append("*****");

    impl_->plugin_name = Utils::get_env("VLINK_LOG_PLUGIN");

    if (!impl_->plugin_name.empty()) {
      is_logging = was_logging;
      global_instance.is_busy.store(false, std::memory_order_release);
      global_instance.is_initializing.store(false, std::memory_order_release);

      return;
    }

    impl_->is_enable_file_channel.store(true, std::memory_order_release);

    if (global_instance.log_path.empty()) {
      std::string log_dir = Utils::get_env("VLINK_LOG_DIR");

      if (log_dir.empty()) {
        log_dir = Utils::get_tmp_dir() + "/" + "vlink-log";
      } else if (log_dir.back() == '/') {
        log_dir.pop_back();
      }

      try {
        if (!std::filesystem::exists(log_dir)) {
          std::filesystem::create_directories(log_dir);
        }
      } catch (std::exception&) {        // LCOV_EXCL_LINE GCOVR_EXCL_LINE
        log_dir = Utils::get_tmp_dir();  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
      }  // LCOV_EXCL_LINE GCOVR_EXCL_LINE

      global_instance.log_path = log_dir + "/" + global_instance.app_name;
    }

    size_t log_max_size = kDefaultLogMaxSize;
    size_t log_max_count = kDefaultLogMaxCount;
    int log_flush_delay_ms = kDefaultLogFlushDelay;

    {
      std::string log_max_size_str = Utils::get_env("VLINK_LOG_MAX_SIZE");
      std::string log_max_count_str = Utils::get_env("VLINK_LOG_MAX_COUNT");
      std::string log_flush_delay_str = Utils::get_env("VLINK_LOG_FLUSH_DELAY");

      if (!log_max_size_str.empty()) {
        std::from_chars(log_max_size_str.data(), log_max_size_str.data() + log_max_size_str.size(), log_max_size);
      }

      if (!log_max_count_str.empty()) {
        std::from_chars(log_max_count_str.data(), log_max_count_str.data() + log_max_count_str.size(), log_max_count);
      }

      if (!log_flush_delay_str.empty()) {
        std::from_chars(log_flush_delay_str.data(), log_flush_delay_str.data() + log_flush_delay_str.size(),
                        log_flush_delay_ms);
      }
    }

#if defined(VLINK_ENABLE_LOG_BACKEND)
    std::string log_strategy = Utils::get_env("VLINK_LOG_STORE_STRATEGY");
    std::string log_append = Utils::get_env("VLINK_LOG_OPEN_APPEND");
    std::string log_block = Utils::get_env("VLINK_LOG_BLOCK_SYNC");
    std::string log_depth = Utils::get_env("VLINK_LOG_WRITE_DEPTH");

    size_t log_write_depth = kDefaultWriteDepth;

    if (!log_depth.empty()) {
      std::from_chars(log_depth.data(), log_depth.data() + log_depth.size(), log_write_depth);
    }

    LoggerBackend::Config config;
    config.app_name = global_instance.app_name;
    config.log_path = global_instance.log_path;
    config.max_file_size = log_max_size;
    config.max_files = log_max_count;
    config.queue_size = log_write_depth;
    config.flush_interval_ms = log_flush_delay_ms > 0 ? static_cast<uint32_t>(log_flush_delay_ms) : 0U;
    config.fixed_filename = log_strategy == "1";
    config.append = log_append == "1";
    config.block_when_full = log_block == "1";
    config.use_utc = global_instance.utc_enable.load(std::memory_order_acquire);

    try {
      impl_->backend = std::make_unique<LoggerBackend>(
          std::move(config),
          [this](std::string_view message) {
            impl_->disk_emergency.store(true, std::memory_order_release);
            std::cerr << "VLink logger disk emergency: " << message << std::endl;
          },
          [](Level level, std::string_view message) { write_to_console_line(level, message); });
    } catch (const std::exception& error) {
      impl_->disk_emergency.store(true, std::memory_order_release);
      std::cerr << "VLink logger disk emergency: " << error.what() << std::endl;
    }

#elif defined(__ANDROID__)
#elif defined(__QNX__)
    slog2_buffer_set_config_t buffer_cfg;

    buffer_cfg.num_buffers = 1;
    buffer_cfg.buffer_set_name = "vlink-log";
    buffer_cfg.verbosity_level = SLOG2_DEBUG2;
    buffer_cfg.buffer_config[0].buffer_name = global_instance.app_name.c_str();
    buffer_cfg.buffer_config[0].num_pages = 32;
    buffer_cfg.max_retries = 3;

    if VUNLIKELY (slog2_register(&buffer_cfg, &impl_->slog2_buffer, 0) != 0) {
      impl_->disk_emergency.store(true, std::memory_order_release);
      std::cerr << "Failed to register slog2 buffer" << std::endl;
    } else {
      slog2_set_default_buffer(impl_->slog2_buffer);
    }

#elif defined(__linux__)
    impl_->kmsg_dev.open(VLINK_KMSG_DEV_PATH, std::ofstream::out | std::ofstream::app);

    std::error_code ec(errno, std::generic_category());

    if VUNLIKELY (!impl_->kmsg_dev.is_open()) {
      impl_->disk_emergency.store(true, std::memory_order_release);
      std::cerr << "Failed to open " << VLINK_KMSG_DEV_PATH << ": " << ec.message() << std::endl;

      is_logging = was_logging;
      global_instance.is_busy.store(false, std::memory_order_release);
      global_instance.is_initializing.store(false, std::memory_order_release);

      return;
    }

#endif

    if (kInfo >= global_instance.file_level.load(std::memory_order_acquire)) {
      write_to_file(kInfo, global_instance.version_log);
    }
  }

  is_logging = was_logging;
  global_instance.is_busy.store(false, std::memory_order_release);
  global_instance.is_initializing.store(false, std::memory_order_release);
}

Logger::~Logger() noexcept {
  auto& global_instance = LoggerGlobal::get();
  global_instance.is_stopping.store(true, std::memory_order_release);

#if defined(_WIN32)
  if (Utils::is_terminating()) {
    (void)impl_.release();
    return;
  }
#endif

  if (!impl_->is_enable_file_channel.exchange(false, std::memory_order_acq_rel)) {
    return;
  }

#if defined(VLINK_ENABLE_LOG_BACKEND)
  impl_->backend.reset();

#elif defined(__ANDROID__)
#elif defined(__QNX__)
#elif defined(__linux__)

  if (impl_->kmsg_dev.is_open()) {
    impl_->kmsg_dev.close();
  }

#endif
}

bool Logger::can_log(Level level) noexcept {
  auto& global_instance = LoggerGlobal::get();

  if (level != kFatal && level < global_instance.console_level.load(std::memory_order_acquire) &&
      level < global_instance.file_level.load(std::memory_order_acquire)) {
    return false;
  }

  if VUNLIKELY (is_logging_on_current_thread() || global_instance.is_initializing.load(std::memory_order_acquire) ||
                global_instance.is_stopping.load(std::memory_order_acquire)) {
    return false;
  }

  return true;
}

void Logger::write(Level level, std::string_view log) noexcept {
  Logger& instance = Logger::get();
  auto& global_instance = LoggerGlobal::get();

  if (level >= global_instance.console_level.load(std::memory_order_acquire)) {
    instance.write_to_console(level, log);
  }

  if (level >= global_instance.file_level.load(std::memory_order_acquire)) {
    instance.write_to_file(level, log);
  }
}

char* Logger::get_local_buffer() noexcept {
  thread_local char buffer[kLocalBufferSize];

  return buffer;
}

FastStream& Logger::get_local_stream() noexcept {
  static auto& global_instance = LoggerGlobal::get();

  thread_local FastStream stream;

  stream.reset();

  auto flags = global_instance.stream_flags.load(std::memory_order_acquire);

  if VUNLIKELY (stream.flags() != flags) {
    stream.flags(flags);
  }

  auto precision = global_instance.stream_precision.load(std::memory_order_acquire);

  if VUNLIKELY (stream.precision() != precision) {
    stream.precision(precision);
  }

  auto width = global_instance.stream_width.load(std::memory_order_acquire);

  if VUNLIKELY (stream.width() != width) {
    stream.width(width);
  }

  return stream;
}

void Logger::write_to_console(Level level, std::string_view log) noexcept {
  static auto& global_instance = LoggerGlobal::get();

  if VUNLIKELY (global_instance.has_console_callback.load(std::memory_order_acquire)) {
    std::shared_lock callback_lock(global_instance.callback_mtx);

    if (global_instance.console_callback) {
      auto& is_logging = is_logging_on_current_thread();
      const bool was_logging = is_logging;

      is_logging = true;

      try {
        global_instance.console_callback(level, log);
      } catch (const std::exception& error) {
        std::cerr << "VLink console logger handler failed: " << error.what() << std::endl;
      } catch (...) {                                                     // LCOV_EXCL_LINE GCOVR_EXCL_LINE
        std::cerr << "VLink console logger handler failed" << std::endl;  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
      }

      is_logging = was_logging;

      return;
    }
  }

  if VUNLIKELY (impl_->is_enable_backtrace.load(std::memory_order_acquire)) {
    return;
  }

  if VUNLIKELY (global_instance.console_format_enable.load(std::memory_order_acquire)) {
    thread_local std::string fmt_log;

    fmt_log.clear();

    auto tid_str = get_thread_id_str();

    if VUNLIKELY (global_instance.utc_enable.load(std::memory_order_acquire)) {
      fmt_log.append(get_current_time(true));
      fmt_log.append(" UTC");
    } else {
      fmt_log.append(get_current_time(false));
    }

    fmt_log.append(" @");
    fmt_log.append(tid_str);
    fmt_log.append(" - ");
    fmt_log.append(get_log_level_str(level).data());
    fmt_log.append(" - ");

    fmt_log.append(log);

    write_to_console_line(level, fmt_log);
  } else {
    write_to_console_line(level, log);
  }
}

void Logger::write_to_console_line(Level level, std::string_view log) noexcept {
  static auto& global_instance = LoggerGlobal::get();
  const bool console_in_order = global_instance.console_in_order.load(std::memory_order_acquire);

  switch (level) {
    case kTrace:
      print_with_color("", log, "\n", stdout, console_in_order, false);
      return;
    case kDebug:
      print_with_color("", log, "\n", stdout, console_in_order, true);
      return;
    case kInfo:
      print_with_color("\033[32m", log, "\033[0m\n", stdout, console_in_order, true);
      return;
    case kWarn:
      print_with_color("\033[33m", log, "\033[0m\n", stderr, console_in_order, true);
      return;
    case kError:
      print_with_color("\033[31m", log, "\033[0m\n", stderr, console_in_order, true);
      return;
    case kFatal:
      print_with_color("\033[41;37;1m", log, "\033[0m\n", stderr, console_in_order, true);
      return;
    // LCOV_EXCL_START GCOVR_EXCL_START
    case kOff:
      return;
    default:
      return;
      // LCOV_EXCL_STOP GCOVR_EXCL_STOP
  }
}

void Logger::write_to_file(Level level, std::string_view log) noexcept {
  static auto& global_instance = LoggerGlobal::get();

  if VUNLIKELY (!impl_->is_enable_file_channel.load(std::memory_order_acquire)) {
    return;
  }

  if VUNLIKELY (global_instance.has_file_callback.load(std::memory_order_acquire)) {
    std::shared_lock callback_lock(global_instance.callback_mtx);

    if (global_instance.file_callback) {
      auto& is_logging = is_logging_on_current_thread();
      const bool was_logging = is_logging;

      is_logging = true;

      try {
        global_instance.file_callback(level, log);
      } catch (const std::exception& error) {
        std::cerr << "VLink file logger handler failed: " << error.what() << std::endl;
      } catch (...) {                                                  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
        std::cerr << "VLink file logger handler failed" << std::endl;  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
      }

      is_logging = was_logging;

      return;
    }
  }

  if VUNLIKELY (impl_->interface) {
    auto& is_plugin_logging = is_plugin_logging_on_current_thread();

    if VUNLIKELY (is_plugin_logging) {
      return;
    }

    thread_local std::string plugin_log;
    plugin_log.assign(log);

    is_plugin_logging = true;
    impl_->interface->log(level, plugin_log);
    is_plugin_logging = false;

    return;
  }

  if VUNLIKELY (impl_->disk_emergency.load(std::memory_order_acquire)) {
    return;  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
  }

#if defined(VLINK_ENABLE_LOG_BACKEND)
  if VUNLIKELY (!impl_->backend || !impl_->backend->log(level, log)) {
    if (impl_->backend && impl_->backend->has_error()) {
      impl_->disk_emergency.store(true, std::memory_order_release);
    }
  }

#elif defined(__ANDROID__)

  int android_level = ANDROID_LOG_DEBUG;
  switch (level) {
    case kTrace:
      android_level = ANDROID_LOG_VERBOSE;
      break;
    case kDebug:
      android_level = ANDROID_LOG_DEBUG;
      break;
    case kInfo:
      android_level = ANDROID_LOG_INFO;
      break;
    case kWarn:
      android_level = ANDROID_LOG_WARN;
      break;
    case kError:
      android_level = ANDROID_LOG_ERROR;
      break;
    case kFatal:
      android_level = ANDROID_LOG_FATAL;
      break;
    case kOff:
      return;
    default:
      return;
  }

  __android_log_write(android_level, global_instance.app_name.c_str(), log.data());

#elif defined(__QNX__)
  int platform_level = SLOG2_DEBUG1;
  switch (level) {
    case kTrace:
      platform_level = SLOG2_DEBUG2;
      break;
    case kDebug:
      platform_level = SLOG2_DEBUG1;
      break;
    case kInfo:
      platform_level = SLOG2_INFO;
      break;
    case kWarn:
      platform_level = SLOG2_WARNING;
      break;
    case kError:
      platform_level = SLOG2_ERROR;
      break;
    case kFatal:
      platform_level = SLOG2_CRITICAL;
      break;
    case kOff:
      return;
    default:
      return;
  }

  slog2c(impl_->slog2_buffer, ::gettid(), platform_level, log.data());

#elif defined(__linux__)

  if (impl_->kmsg_dev.is_open()) {
    std::lock_guard lock(impl_->file_mtx);

    switch (level) {
      case kTrace:
        impl_->kmsg_dev << "<7>";
        break;
      case kDebug:
        impl_->kmsg_dev << "<6>";
        break;
      case kInfo:
        impl_->kmsg_dev << "<5>";
        break;
      case kWarn:
        impl_->kmsg_dev << "<4>";
        break;
      case kError:
        impl_->kmsg_dev << "<3>";
        break;
      case kFatal:
        impl_->kmsg_dev << "<2>";
        break;
      case kOff:
        return;
      default:
        return;
    }

    impl_->kmsg_dev << log << std::endl;
  }

#endif
}

}  // namespace vlink
