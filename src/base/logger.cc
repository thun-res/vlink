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
#include <vector>

#include "./base/cached_timestamp.h"
#include "./base/logger_plugin_interface.h"
#include "./base/utils.h"
#include "./vlink/version.h"

#if defined(VLINK_ENABLE_LOG_SPD)

#define SPDLOG_LEVEL_NAMES                                                                                     \
  {                                                                                                            \
      spdlog::string_view_t("TRACE", 5), spdlog::string_view_t("DEBUG", 5), spdlog::string_view_t("INFO ", 5), \
      spdlog::string_view_t("WARN ", 5), spdlog::string_view_t("ERROR", 5), spdlog::string_view_t("FATAL", 5), \
      spdlog::string_view_t("EMPTY", 5),                                                                       \
  }

#define SPDLOG_SHORT_LEVEL_NAMES {"T", "D", "I", "W", "E", "F", " "}

#include <spdlog/async.h>
#include <spdlog/sinks/rotating_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

#include "./private/spdlog_time_rolling_file_sink.h"
#elif defined(VLINK_ENABLE_LOG_QUI)
#include <quill/Backend.h>
#include <quill/Frontend.h>
#include <quill/LogMacros.h>
#include <quill/Logger.h>
#include <quill/sinks/ConsoleSink.h>
#include <quill/sinks/RotatingFileSink.h>
#elif defined(VLINK_ENABLE_LOG_DLT)
#include <dlt/dlt.h>
DLT_DECLARE_CONTEXT(dlt_global_ctx_);
#elif defined(VLINK_ENABLE_LOG_NAT) && defined(__ANDROID__)
#include <android/log.h>
#elif defined(VLINK_ENABLE_LOG_NAT) && defined(__QNX__)
#include <process.h>
#include <sys/slog2.h>
#elif defined(VLINK_ENABLE_LOG_NAT) && defined(__linux__)
#include <linux/kernel.h>
#define VLINK_KMSG_DEV_PATH "/dev/kmsg"
#endif

namespace vlink {

[[maybe_unused]] static constexpr size_t kDefaultWriteDepth = 1024L * 8;
[[maybe_unused]] static constexpr size_t kDefaultLogMaxSize = 1024L * 1024L * 10U;
[[maybe_unused]] static constexpr size_t kDefaultLogMaxCount = 10U;
[[maybe_unused]] static constexpr int kDefaultLogFlushDelay = 500;

#if defined(VLINK_ENABLE_LOG_QUI)
template <quill::QueueType QueueTypeT>
struct QuillFrontendOptions final : quill::FrontendOptions {
  // NOLINTNEXTLINE(readability-identifier-naming)
  static constexpr quill::QueueType queue_type{QueueTypeT};
  // NOLINTNEXTLINE(readability-identifier-naming)
  inline static size_t initial_queue_capacity{quill::FrontendOptions::initial_queue_capacity};
  // NOLINTNEXTLINE(readability-identifier-naming)
  inline static size_t unbounded_queue_max_capacity{quill::FrontendOptions::unbounded_queue_max_capacity};
};

using QuillBlockingFrontend = quill::FrontendImpl<QuillFrontendOptions<quill::QueueType::UnboundedBlocking>>;
using QuillDroppingFrontend = quill::FrontendImpl<QuillFrontendOptions<quill::QueueType::BoundedDropping>>;

template <bool ImmediateFlushT>
static void log_quill(QuillBlockingFrontend::logger_t* blocking_logger,
                      QuillDroppingFrontend::logger_t* dropping_logger, const quill::MacroMetadata* metadata,
                      std::string_view log) {
  if VLIKELY (dropping_logger) {
    (void)dropping_logger->template log_statement<ImmediateFlushT>(metadata, log);
  } else {
    (void)blocking_logger->template log_statement<ImmediateFlushT>(metadata, log);
  }
}
#endif

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
  LoggerGlobal() = default;
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

#if defined(VLINK_ENABLE_LOG_SPD)
  std::shared_ptr<spdlog::logger> spd;
  std::shared_ptr<spdlog::sinks::sink> spd_console_sink;
  std::shared_ptr<spdlog::sinks::sink> spd_file_sink;
  std::shared_ptr<spdlog::details::thread_pool> spd_thread_pool;
#elif defined(VLINK_ENABLE_LOG_QUI)
  QuillBlockingFrontend::logger_t* quill_blocking_log{nullptr};
  QuillDroppingFrontend::logger_t* quill_dropping_log{nullptr};
  std::shared_ptr<quill::StreamSink> quill_console_sink;
  std::shared_ptr<quill::StreamSink> quill_file_sink;
  quill::MacroMetadata quill_metadata_trace{
      "", "", "{}", nullptr, quill::LogLevel::TraceL1, quill::MacroMetadata::Event::Log};
  quill::MacroMetadata quill_metadata_debug{
      "", "", "{}", nullptr, quill::LogLevel::Debug, quill::MacroMetadata::Event::Log};
  quill::MacroMetadata quill_metadata_info{
      "", "", "{}", nullptr, quill::LogLevel::Info, quill::MacroMetadata::Event::Log};
  quill::MacroMetadata quill_metadata_warn{
      "", "", "{}", nullptr, quill::LogLevel::Warning, quill::MacroMetadata::Event::Log};
  quill::MacroMetadata quill_metadata_error{
      "", "", "{}", nullptr, quill::LogLevel::Error, quill::MacroMetadata::Event::Log};
  quill::MacroMetadata quill_metadata_fatal{
      "", "", "{}", nullptr, quill::LogLevel::Critical, quill::MacroMetadata::Event::Log};
  quill::MacroMetadata quill_metadata_backtrace{
      "", "", "{}", nullptr, quill::LogLevel::Backtrace, quill::MacroMetadata::Event::Log};
#elif defined(VLINK_ENABLE_LOG_DLT)
#elif defined(VLINK_ENABLE_LOG_NAT) && defined(__ANDROID__)
#elif defined(VLINK_ENABLE_LOG_NAT) && defined(__QNX__)
  slog2_buffer_t slog2_buffer{nullptr};
#elif defined(VLINK_ENABLE_LOG_NAT) && defined(__linux__)
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
    instance.impl_->interface->flush();
    return;
  }

  if VUNLIKELY (instance.impl_->disk_emergency.load(std::memory_order_acquire)) {
    return;  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
  }

  try {
#if defined(VLINK_ENABLE_LOG_SPD)
    instance.impl_->spd->flush();
#elif defined(VLINK_ENABLE_LOG_QUI)
    quill::Backend::notify();
    instance.impl_->quill_file_sink->flush_sink();
#endif
    // LCOV_EXCL_START GCOVR_EXCL_START
  } catch (std::exception& e) {
    instance.impl_->disk_emergency.store(true, std::memory_order_release);
    std::cerr << "VLink logger disk emergency: " << e.what() << std::endl;
  }
  // LCOV_EXCL_STOP GCOVR_EXCL_STOP
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

#if defined(VLINK_ENABLE_LOG_SPD) || defined(VLINK_ENABLE_LOG_QUI)
  std::lock_guard lock(instance.impl_->backtrace_mtx);

  if (instance.impl_->is_enable_backtrace.load(std::memory_order_acquire)) {
    return;  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
  }

#if defined(VLINK_ENABLE_LOG_SPD)
  instance.impl_->spd_console_sink->set_level(spdlog::level::trace);
  instance.impl_->spd->set_level(spdlog::level::warn);
  instance.impl_->spd->enable_backtrace(size);
#elif defined(VLINK_ENABLE_LOG_QUI)
  if VLIKELY (instance.impl_->quill_dropping_log) {
    instance.impl_->quill_dropping_log->init_backtrace(size, quill::LogLevel::TraceL3);
  } else {
    instance.impl_->quill_blocking_log->init_backtrace(size, quill::LogLevel::TraceL3);
  }
#endif

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

#if defined(VLINK_ENABLE_LOG_SPD)
  instance.impl_->spd->disable_backtrace();
  instance.impl_->spd_console_sink->set_level(spdlog::level::off);
  instance.impl_->spd->set_level(spdlog::level::trace);
#elif defined(VLINK_ENABLE_LOG_QUI)
  if VLIKELY (instance.impl_->quill_dropping_log) {
    instance.impl_->quill_dropping_log->flush_log();
    instance.impl_->quill_dropping_log->init_backtrace(0, quill::LogLevel::None);
  } else {
    instance.impl_->quill_blocking_log->flush_log();
    instance.impl_->quill_blocking_log->init_backtrace(0, quill::LogLevel::None);
  }
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

#if defined(VLINK_ENABLE_LOG_SPD)
  instance.impl_->spd->dump_backtrace();
  instance.impl_->spd_console_sink->flush();
  std::this_thread::sleep_for(std::chrono::milliseconds(100));  // wait for dump to console
#elif defined(VLINK_ENABLE_LOG_QUI)
  if VLIKELY (instance.impl_->quill_dropping_log) {
    instance.impl_->quill_dropping_log->flush_backtrace();
  } else {
    instance.impl_->quill_blocking_log->flush_backtrace();
  }
#else
  (void)instance;
#endif
}

bool Logger::is_busy() noexcept { return LoggerGlobal::get().is_busy.load(std::memory_order_acquire); }

bool Logger::can_log(Level level) noexcept {
  auto& global_instance = LoggerGlobal::get();

  if VUNLIKELY (global_instance.is_stopping.load(std::memory_order_acquire)) {
    return false;
  }

  return level == kFatal || level >= global_instance.console_level.load(std::memory_order_acquire) ||
         level >= global_instance.file_level.load(std::memory_order_acquire);
}

bool Logger::is_writable(Level level) noexcept {
  if VUNLIKELY (level >= kOff) {
    return false;
  }

  return can_log(level);
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

Logger::Logger() noexcept : impl_(std::make_unique<Impl>()) {
  static auto& global_instance = LoggerGlobal::get();

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
      global_instance.is_busy.store(false, std::memory_order_release);
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

#if defined(VLINK_ENABLE_LOG_SPD)
    std::string log_strategy = Utils::get_env("VLINK_LOG_STORE_STRATEGY");
    std::string log_append = Utils::get_env("VLINK_LOG_OPEN_APPEND");
    std::string log_block = Utils::get_env("VLINK_LOG_BLOCK_SYNC");
    std::string log_depth = Utils::get_env("VLINK_LOG_WRITE_DEPTH");

    bool use_log_strategy = (log_strategy == "1");
    bool use_log_append = (log_append == "1");
    bool use_log_block = (log_block == "1");
    int log_write_depth = kDefaultWriteDepth;

    if (!log_depth.empty()) {
      std::from_chars(log_depth.data(), log_depth.data() + log_depth.size(), log_write_depth);
    }

    impl_->spd_thread_pool = std::make_shared<spdlog::details::thread_pool>(log_write_depth, 1);

    impl_->spd_console_sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
    impl_->spd_console_sink->set_level(spdlog::level::off);

    if (use_log_strategy) {
      std::string log_path = global_instance.log_path + "/" + global_instance.app_name + ".log";

      try {
        impl_->spd_file_sink = std::make_shared<spdlog::sinks::rotating_file_sink_mt>(log_path, log_max_size,
                                                                                      log_max_count, !use_log_append);
      } catch (std::exception& e) {                                    // LCOV_EXCL_LINE GCOVR_EXCL_LINE
        impl_->disk_emergency.store(true, std::memory_order_release);  // LCOV_EXCL_LINE GCOVR_EXCL_LINE

        std::cerr << "VLink logger disk emergency: " << e.what() << std::endl;  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
      }  // LCOV_EXCL_LINE GCOVR_EXCL_LINE

    } else {
      spdlog_custom_sink::TimeZone timezone = global_instance.utc_enable.load(std::memory_order_acquire)
                                                  ? spdlog_custom_sink::TimeZone::kTimezoneUtc
                                                  : spdlog_custom_sink::TimeZone::kTimezoneLocal;

      try {
        impl_->spd_file_sink = std::make_shared<spdlog_custom_sink::TimeRollingFile_mt>(
            global_instance.log_path, log_max_size, log_max_count, timezone, !use_log_append);
      } catch (std::exception& e) {                                    // LCOV_EXCL_LINE GCOVR_EXCL_LINE
        impl_->disk_emergency.store(true, std::memory_order_release);  // LCOV_EXCL_LINE GCOVR_EXCL_LINE

        std::cerr << "VLink logger disk emergency: " << e.what() << std::endl;  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
      }  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
    }

    std::vector<spdlog::sink_ptr> spd_sinks;
    spd_sinks.emplace_back(impl_->spd_console_sink);

    if (impl_->spd_file_sink) {
      spd_sinks.emplace_back(impl_->spd_file_sink);
    }

    impl_->spd = std::make_shared<spdlog::async_logger>(
        global_instance.app_name, spd_sinks.begin(), spd_sinks.end(), impl_->spd_thread_pool,
        use_log_block ? spdlog::async_overflow_policy::block : spdlog::async_overflow_policy::overrun_oldest);

    spdlog::register_logger(impl_->spd);

    impl_->spd->set_error_handler([this](const std::string& msg) {
      impl_->disk_emergency.store(true, std::memory_order_release);      // LCOV_EXCL_LINE GCOVR_EXCL_LINE
      std::cerr << "VLink logger disk emergency: " << msg << std::endl;  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
    });                                                                  // LCOV_EXCL_LINE GCOVR_EXCL_LINE

    spdlog::set_default_logger(impl_->spd);

    if (global_instance.utc_enable.load(std::memory_order_acquire)) {
      impl_->spd->set_pattern("%m-%d %H:%M:%S.%e UTC @%t - %l - %v", spdlog::pattern_time_type::utc);
    } else {
      impl_->spd->set_pattern("%m-%d %H:%M:%S.%e @%t - %l - %v", spdlog::pattern_time_type::local);
    }

    impl_->spd->set_level(spdlog::level::level_enum::trace);

    if (log_flush_delay_ms > 0) {
      impl_->spd->flush_on(spdlog::level::level_enum::err);
      spdlog::flush_every(std::chrono::milliseconds(log_flush_delay_ms));

    } else {
      impl_->spd->flush_on(spdlog::level::level_enum::trace);
    }

#elif defined(VLINK_ENABLE_LOG_QUI)
    std::string log_append = Utils::get_env("VLINK_LOG_OPEN_APPEND");
    std::string log_block = Utils::get_env("VLINK_LOG_BLOCK_SYNC");
    std::string log_depth = Utils::get_env("VLINK_LOG_WRITE_DEPTH");

    bool use_log_append = (log_append == "1");
    bool use_log_block = (log_block == "1");
    size_t log_queue_capacity = quill::FrontendOptions::initial_queue_capacity;

    if (!log_depth.empty()) {
      std::from_chars(log_depth.data(), log_depth.data() + log_depth.size(), log_queue_capacity);
    }

    static constexpr size_t kMinimumQuillQueueCapacity = static_cast<size_t>(Logger::kLocalBufferSize) * 2U;

    if (log_queue_capacity < kMinimumQuillQueueCapacity) {
      log_queue_capacity = kMinimumQuillQueueCapacity;
    }

    QuillFrontendOptions<quill::QueueType::UnboundedBlocking>::initial_queue_capacity = log_queue_capacity;
    QuillFrontendOptions<quill::QueueType::UnboundedBlocking>::unbounded_queue_max_capacity = log_queue_capacity;
    QuillFrontendOptions<quill::QueueType::BoundedDropping>::initial_queue_capacity = log_queue_capacity;

    std::string log_path = global_instance.log_path + "/" + global_instance.app_name + ".log";

    quill::BackendOptions backend_options;

    // backend_options.cpu_affinity=0;
    backend_options.sleep_duration = std::chrono::microseconds(500);
    backend_options.transit_event_buffer_initial_capacity = 1024U;
    backend_options.transit_events_soft_limit = 8192U;
    backend_options.transit_events_hard_limit = 8192U;
    backend_options.wait_for_queues_to_empty_before_exit = true;
    backend_options.sink_min_flush_interval =
        std::chrono::milliseconds(log_flush_delay_ms > 0 ? log_flush_delay_ms : 0);
    backend_options.log_level_descriptions = {"TRACE", "TRACE", "TRACE", "DEBUG",     "INFO", "NOTICE",
                                              "WARN",  "ERROR", "FATAL", "BACKTRACE", "EMPTY"};
    backend_options.log_level_short_codes = {"T", "T", "T", "D", "I", "N", "W", "E", "F", "BT", " "};

    backend_options.error_notifier = [this](std::string const& msg) {
      if (msg.find(" Quill INFO: ") != std::string::npos) {
        std::cerr << msg << std::endl;
        return;
      }

      impl_->disk_emergency.store(true, std::memory_order_release);

      std::cerr << "VLink logger disk emergency: " << msg << std::endl;
    };

    quill::Backend::start(backend_options);

    quill::RotatingFileSinkConfig sink_config;

    sink_config.set_open_mode('a');
    sink_config.set_rotation_max_file_size(log_max_size);
    sink_config.set_max_backup_files(log_max_count);
    sink_config.set_overwrite_rolled_files(true);
    sink_config.set_remove_old_files(true);
    sink_config.set_fsync_enabled(true);
    sink_config.set_minimum_fsync_interval(std::chrono::milliseconds(log_flush_delay_ms));
    sink_config.set_rotation_on_creation(!use_log_append);
    sink_config.set_filename_append_option(quill::FilenameAppendOption::None);
    sink_config.set_rotation_naming_scheme(quill::RotatingFileSinkConfig::RotationNamingScheme::Index);

    quill::PatternFormatterOptions format_options;

    if (global_instance.utc_enable.load(std::memory_order_acquire)) {
      sink_config.set_timezone(quill::Timezone::GmtTime);
      format_options = quill::PatternFormatterOptions("%(time) UTC @%(thread_id) - %(log_level:<5) - %(message)",
                                                      "%m-%d %H:%M:%S.%Qms", quill::Timezone::GmtTime);
    } else {
      sink_config.set_timezone(quill::Timezone::LocalTime);
      format_options = quill::PatternFormatterOptions("%(time) @%(thread_id) - %(log_level:<5) - %(message)",
                                                      "%m-%d %H:%M:%S.%Qms", quill::Timezone::LocalTime);
    }

    impl_->quill_console_sink = std::make_shared<quill::ConsoleSink>();
    impl_->quill_console_sink->set_log_level_filter(quill::LogLevel::Backtrace);

    try {
      impl_->quill_file_sink = std::make_shared<quill::RotatingFileSink>(log_path, sink_config);
    } catch (const std::exception& e) {                                       // LCOV_EXCL_LINE GCOVR_EXCL_LINE
      impl_->disk_emergency.store(true, std::memory_order_release);           // LCOV_EXCL_LINE GCOVR_EXCL_LINE
      std::cerr << "VLink logger disk emergency: " << e.what() << std::endl;  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
    }

    std::vector<std::shared_ptr<quill::Sink>> quill_sinks;

    if (impl_->quill_file_sink) {
      quill_sinks.emplace_back(impl_->quill_file_sink);
    }

    quill_sinks.emplace_back(impl_->quill_console_sink);

    if (use_log_block) {
      impl_->quill_blocking_log =
          QuillBlockingFrontend::create_or_get_logger(global_instance.app_name, quill_sinks, format_options);
      impl_->quill_blocking_log->set_log_level(quill::LogLevel::None);
    } else {
      impl_->quill_dropping_log =
          QuillDroppingFrontend::create_or_get_logger(global_instance.app_name, quill_sinks, format_options);
      impl_->quill_dropping_log->set_log_level(quill::LogLevel::None);
    }

#elif defined(VLINK_ENABLE_LOG_DLT)
    DLT_REGISTER_APP(global_instance.app_name.c_str(), "Application for Logging");
    DLT_REGISTER_CONTEXT(dlt_global_ctx_, "Context", "Context for Logging");

#elif defined(VLINK_ENABLE_LOG_NAT) && defined(__ANDROID__)
#elif defined(VLINK_ENABLE_LOG_NAT) && defined(__QNX__)
    slog2_buffer_set_config_t buffer_cfg;

    buffer_cfg.num_buffers = 1;
    buffer_cfg.buffer_set_name = "vlink-log";
    buffer_cfg.verbosity_level = SLOG2_DEBUG2;
    buffer_cfg.buffer_config[0].buffer_name = global_instance.app_name.c_str();
    buffer_cfg.buffer_config[0].num_pages = 32;
    buffer_cfg.max_retries = 3;

    if (slog2_register(&buffer_cfg, &impl_->slog2_buffer, 0) == 0) {
      slog2_set_default_buffer(impl_->slog2_buffer);
    }

#elif defined(VLINK_ENABLE_LOG_NAT) && defined(__linux__)
    impl_->kmsg_dev.open(VLINK_KMSG_DEV_PATH, std::ofstream::out | std::ofstream::app);

    std::error_code ec(errno, std::generic_category());

    if VUNLIKELY (!impl_->kmsg_dev.is_open()) {
      std::cerr << "Failed to open " << VLINK_KMSG_DEV_PATH << ": " << ec.message() << std::endl;

      global_instance.is_busy.store(false, std::memory_order_release);

      return;
    }

#endif

    if (kInfo >= global_instance.file_level.load(std::memory_order_acquire)) {
      write_to_file(kInfo, global_instance.version_log);
    }
  }

  global_instance.is_busy.store(false, std::memory_order_release);
}

Logger::~Logger() noexcept {
  if (!impl_->is_enable_file_channel.load(std::memory_order_acquire)) {
    return;
  }

#if defined(VLINK_ENABLE_LOG_SPD)
  try {
    impl_->spd->flush();
    impl_->spd.reset();
    spdlog::shutdown();
    impl_->spd_console_sink.reset();
    impl_->spd_file_sink.reset();
    impl_->spd_thread_pool.reset();
  } catch (std::exception&) {  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
  }  // LCOV_EXCL_LINE GCOVR_EXCL_LINE

#elif defined(VLINK_ENABLE_LOG_QUI)
  try {
    quill::Backend::notify();

    if (impl_->quill_file_sink) {
      impl_->quill_file_sink->flush_sink();
    }

    quill::Backend::stop();
    impl_->quill_console_sink.reset();
    impl_->quill_file_sink.reset();

    if (impl_->quill_dropping_log) {
      QuillDroppingFrontend::remove_logger(impl_->quill_dropping_log);
      impl_->quill_dropping_log = nullptr;
    } else {
      QuillBlockingFrontend::remove_logger(impl_->quill_blocking_log);
      impl_->quill_blocking_log = nullptr;
    }
  } catch (std::exception&) {
  }

#elif defined(VLINK_ENABLE_LOG_DLT)
  DLT_UNREGISTER_CONTEXT(dlt_global_ctx_);
  DLT_UNREGISTER_APP();

#elif defined(VLINK_ENABLE_LOG_NAT) && defined(__ANDROID__)
#elif defined(VLINK_ENABLE_LOG_NAT) && defined(__QNX__)
#elif defined(VLINK_ENABLE_LOG_NAT) && defined(__linux__)

  if (impl_->kmsg_dev.is_open()) {
    impl_->kmsg_dev.close();
  }

#endif
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
      global_instance.console_callback(level, log);
      return;
    }
  }

  if VUNLIKELY (impl_->is_enable_backtrace.load(std::memory_order_acquire)) {
    return;
  }

  const bool console_in_order = global_instance.console_in_order.load(std::memory_order_acquire);

  if (global_instance.console_format_enable.load(std::memory_order_acquire)) {
    thread_local std::string fmt_log;

    fmt_log.clear();

    auto tid_str = get_thread_id_str();

    if (global_instance.utc_enable.load(std::memory_order_acquire)) {
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

    switch (level) {
      case kTrace:
        print_with_color("", fmt_log, "\n", stdout, console_in_order, false);
        return;
      case kDebug:
        print_with_color("", fmt_log, "\n", stdout, console_in_order, true);
        return;
      case kInfo:
        print_with_color("\033[32m", fmt_log, "\033[0m\n", stdout, console_in_order, true);
        return;
      case kWarn:
        print_with_color("\033[33m", fmt_log, "\033[0m\n", stderr, console_in_order, true);
        return;
      case kError:
        print_with_color("\033[31m", fmt_log, "\033[0m\n", stderr, console_in_order, true);
        return;
      case kFatal:
        print_with_color("\033[41;37;1m", fmt_log, "\033[0m\n", stderr, console_in_order, true);
        return;
      // LCOV_EXCL_START GCOVR_EXCL_START
      case kOff:
        return;
      default:
        return;
        // LCOV_EXCL_STOP GCOVR_EXCL_STOP
    }
  } else {
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
}

void Logger::write_to_file(Level level, std::string_view log) noexcept {
  static auto& global_instance = LoggerGlobal::get();

  if VUNLIKELY (!impl_->is_enable_file_channel.load(std::memory_order_acquire)) {
    return;
  }

  if VUNLIKELY (global_instance.has_file_callback.load(std::memory_order_acquire)) {
    std::shared_lock callback_lock(global_instance.callback_mtx);

    if (global_instance.file_callback) {
      global_instance.file_callback(level, log);
      return;
    }
  }

  if VUNLIKELY (impl_->interface) {
    thread_local bool is_plugin_logging{false};

    if VUNLIKELY (is_plugin_logging) {
      return;  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
    }

    is_plugin_logging = true;
    impl_->interface->log(level, log);
    is_plugin_logging = false;
    return;
  }

  if VUNLIKELY (impl_->disk_emergency.load(std::memory_order_acquire)) {
    return;  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
  }

#if defined(VLINK_ENABLE_LOG_SPD)
  spdlog::level::level_enum spd_level = spdlog::level::debug;

  switch (level) {
    case kTrace:
      spd_level = spdlog::level::trace;
      break;
    case kDebug:
      spd_level = spdlog::level::debug;
      break;
    case kInfo:
      spd_level = spdlog::level::info;
      break;
    case kWarn:
      spd_level = spdlog::level::warn;
      break;
    case kError:
      spd_level = spdlog::level::err;
      break;
    case kFatal:
      spd_level = spdlog::level::critical;
      break;
    // LCOV_EXCL_START GCOVR_EXCL_START
    case kOff:
      return;
    default:
      return;
      // LCOV_EXCL_STOP GCOVR_EXCL_STOP
  }

  try {
    impl_->spd->log(spd_level, spdlog::string_view_t(log.data(), log.size()));
    // LCOV_EXCL_START GCOVR_EXCL_START
  } catch (const std::exception& e) {
    impl_->disk_emergency.store(true, std::memory_order_release);
    std::cerr << "VLink logger disk emergency: " << e.what() << std::endl;
  }
  // LCOV_EXCL_STOP GCOVR_EXCL_STOP

#elif defined(VLINK_ENABLE_LOG_QUI)
  try {
    if VUNLIKELY (impl_->is_enable_backtrace.load(std::memory_order_acquire)) {
      std::lock_guard lock(impl_->backtrace_mtx);

      if (impl_->is_enable_backtrace.load(std::memory_order_acquire)) {
        log_quill<true>(impl_->quill_blocking_log, impl_->quill_dropping_log, &impl_->quill_metadata_backtrace, log);

        if (level < kWarn) {
          return;
        }
      }
    }

    switch (level) {
      case kTrace:
        log_quill<false>(impl_->quill_blocking_log, impl_->quill_dropping_log, &impl_->quill_metadata_trace, log);
        break;
      case kDebug:
        log_quill<false>(impl_->quill_blocking_log, impl_->quill_dropping_log, &impl_->quill_metadata_debug, log);
        break;
      case kInfo:
        log_quill<false>(impl_->quill_blocking_log, impl_->quill_dropping_log, &impl_->quill_metadata_info, log);
        break;
      case kWarn:
        log_quill<false>(impl_->quill_blocking_log, impl_->quill_dropping_log, &impl_->quill_metadata_warn, log);
        break;
      case kError:
        log_quill<true>(impl_->quill_blocking_log, impl_->quill_dropping_log, &impl_->quill_metadata_error, log);
        quill::Backend::notify();
        break;
      case kFatal:
        log_quill<true>(impl_->quill_blocking_log, impl_->quill_dropping_log, &impl_->quill_metadata_fatal, log);
        quill::Backend::notify();
        break;
      case kOff:
        return;
      default:
        return;
    }
  } catch (const std::exception& e) {
    impl_->disk_emergency.store(true, std::memory_order_release);
    std::cerr << "VLink logger disk emergency: " << e.what() << std::endl;
  }

#elif defined(VLINK_ENABLE_LOG_DLT)

  DltLogLevelType dlt_level = DLT_LOG_DEFAULT;
  switch (level) {
    case kTrace:
      dlt_level = DLT_LOG_VERBOSE;
      break;
    case kDebug:
      dlt_level = DLT_LOG_DEBUG;
      break;
    case kInfo:
      dlt_level = DLT_LOG_INFO;
      break;
    case kWarn:
      dlt_level = DLT_LOG_WARN;
      break;
    case kError:
      dlt_level = DLT_LOG_ERROR;
      break;
    case kFatal:
      dlt_level = DLT_LOG_FATAL;
      break;
    case kOff:
      return;
    default:
      return;
  }

#ifdef DLT_SIZED_STRING
  DLT_LOG(dlt_global_ctx_, dlt_level, DLT_SIZED_STRING(log.data(), log.size()));
#else
  DLT_LOG(dlt_global_ctx_, dlt_level, DLT_STRING(log.data()));
#endif

#elif defined(VLINK_ENABLE_LOG_NAT) && defined(__ANDROID__)

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

#elif defined(VLINK_ENABLE_LOG_NAT) && defined(__QNX__)
  int nat_level = SLOG2_DEBUG1;
  switch (level) {
    case kTrace:
      nat_level = SLOG2_DEBUG2;
      break;
    case kDebug:
      nat_level = SLOG2_DEBUG1;
      break;
    case kInfo:
      nat_level = SLOG2_INFO;
      break;
    case kWarn:
      nat_level = SLOG2_WARNING;
      break;
    case kError:
      nat_level = SLOG2_ERROR;
      break;
    case kFatal:
      nat_level = SLOG2_CRITICAL;
      break;
    case kOff:
      return;
    default:
      return;
  }

  slog2c(impl_->slog2_buffer, ::gettid(), nat_level, log.data());

#elif defined(VLINK_ENABLE_LOG_NAT) && defined(__linux__)

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
