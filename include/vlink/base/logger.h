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
 * @file logger.h
 * @brief Singleton logger with stream / format / printf / RAII-stream entry points.
 *
 * @details
 * @c vlink::Logger is the central logging facility.  A single instance is constructed via
 * @c Logger::init and reused for the rest of the process; each call routes through a console
 * sink and an optional file sink whose minimum levels are independently configurable.
 *
 * @par Entry point cheat sheet
 *
 * | Style           | Macro family    | Backing API                   | Argument shape                  |
 * | --------------- | --------------- | ----------------------------- | ------------------------------- |
 * | Stream          | @c VLOG_x       | @c FastStream operator<<      | @c VLOG_I("x=", x, " y=", y)    |
 * | Placeholder     | @c MLOG_x       | @c vlink::format::format_to_n | @c MLOG_W("x={} y={}", x, y)    |
 * | printf          | @c CLOG_x       | @c std::snprintf              | @c CLOG_E("errno=%d", err)      |
 * | RAII stream     | @c SLOG_x       | @c WrapperStream              | @c SLOG_D @c << "a=" @c << a    |
 *
 * @par Severity ladder
 *
 * | Value | Name      | Use case                                  |
 * | ----- | --------- | ----------------------------------------- |
 * | 0     | @c kTrace | Verbose internals                         |
 * | 1     | @c kDebug | Developer diagnostics                     |
 * | 2     | @c kInfo  | Normal operational messages               |
 * | 3     | @c kWarn  | Unusual but recoverable conditions        |
 * | 4     | @c kError | Errors that may affect operation          |
 * | 5     | @c kFatal | Unrecoverable; throws @c RuntimeError     |
 * | 6     | @c kOff   | Disables the corresponding sink           |
 *
 * @par ASCII priority diagram
 *
 * @verbatim
 *    higher severity  ->  kFatal  (throws)
 *                         kError
 *                         kWarn   <- kDetailLevel (default): adds {file:line}
 *                         kInfo
 *                         kDebug
 *                         kTrace
 *    lower severity   ->  kOff    (sink disabled)
 * @endverbatim
 *
 * @par Compile-time gating
 *  - @c VLINK_LOG_LEVEL @c =N strips levels below @c N at compile time (zero overhead).
 *  - @c VLINK_LOG_DETAIL_LEVEL @c =N changes the level at which file/line is appended.
 *  - @c VLINK_LOG_DISABLE_SHORT removes the @c VLOG_* / @c MLOG_* / @c CLOG_* / @c SLOG_* aliases.
 *
 * @par Formatting cheat sheet
 *
 * | Need                              | Snippet                                                |
 * | --------------------------------- | ------------------------------------------------------ |
 * | Hex with 4 digits                 | @c VLOG_I(VLINK_LOG_HEX(4), value)                     |
 * | Inline hex inside @c SLOG_*       | @c SLOG_I @c << VLINK_LOG_HEXSS(4) @c << value         |
 * | Gate around expensive log args    | @c if @c (VLINK_LOG_IF_D) @c { ... @c }                |
 *
 * @par Example
 * @code
 *   vlink::Logger::init("my_app", "/var/log/my_app.log");
 *   vlink::Logger::set_console_level(vlink::Logger::kInfo);
 *
 *   VLOG_I("node started, id=", node_id);
 *   MLOG_W("temperature is {} C", temp);
 *   CLOG_E("errno=%d", errno);
 *   SLOG_D << "values: " << a << " " << b;
 * @endcode
 *
 * @note @c kFatal messages call @c Logger::flush and then throw @c Exception::RuntimeError so
 *       the application can perform a controlled shutdown.  Backends include spdlog, quill,
 *       DLT, Android logcat, QNX slog2 and kmsg depending on build options.
 */

#pragma once

#include <cstdio>
#include <iomanip>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

#include "./exception.h"
#include "./fast_stream.h"
#include "./format.h"
#include "./functional.h"
#include "./macros.h"

namespace vlink {

/**
 * @class Logger
 * @brief Global singleton logger with four formatting styles and independently configurable sinks.
 *
 * @details
 * Construct exactly once via @c Logger::init; subsequent calls reconfigure the existing instance.
 * The console sink is always enabled; the file sink activates when @c log_path is non-empty.
 * Logging entry points are macros that wrap the public @c print_* templates and forward a
 * compile-time @c Level so the bodies disappear when the level is below @c kMinimumLevel.
 */
class VLINK_EXPORT Logger final {
 public:
  /**
   * @brief Internal output style tag used by the @c print_* family.
   *
   * @details
   * Callers normally interact with styles via macros; the enum is exposed for completeness.
   */
  enum Style : uint8_t {
    kStreamStyle = 0,  ///< Stream composition via @c FastStream operator<<.
    kFormatStyle = 1,  ///< Brace placeholders via @c vlink::format.
    kCStyle = 2,       ///< printf-style formatting via @c std::snprintf.
  };

  /**
   * @brief Message severity level.
   *
   * @details
   * Lower numerical values are less severe.  @c kOff is a sentinel that disables a sink.
   */
  enum Level : uint8_t {
    kTrace = 0,  ///< Verbose tracing.
    kDebug = 1,  ///< Developer diagnostics.
    kInfo = 2,   ///< Normal operational message.
    kWarn = 3,   ///< Recoverable but unusual condition.
    kError = 4,  ///< Recoverable error.
    kFatal = 5,  ///< Unrecoverable; throws @c Exception::RuntimeError.
    kOff = 6,    ///< Disable sink.
  };

  /**
   * @brief Compile-time minimum severity level; messages below this are stripped.
   *
   * @details
   * Override by defining @c VLINK_LOG_LEVEL before including this header.  Defaults to
   * @c kTrace so every level is emitted by default.
   */
#ifdef VLINK_LOG_LEVEL
  static constexpr uint8_t kMinimumLevel = VLINK_LOG_LEVEL;
#else
  static constexpr uint8_t kMinimumLevel = kTrace;
#endif

  /**
   * @brief Severity threshold at and above which @c {file:line} is prepended to messages.
   *
   * @details
   * Override by defining @c VLINK_LOG_DETAIL_LEVEL before including this header.  Defaults to
   * @c kWarn.
   */
#ifdef VLINK_LOG_DETAIL_LEVEL
  static constexpr uint8_t kDetailLevel = VLINK_LOG_DETAIL_LEVEL;
#else
  static constexpr uint8_t kDetailLevel = kWarn;
#endif

  /**
   * @brief Size of the thread-local C-style format buffer in bytes.
   *
   * @details
   * Messages longer than @c kLocalBufferSize @c - @c 1 characters are silently truncated.
   */
  static constexpr int kLocalBufferSize = 4096;

  /**
   * @brief Signature for custom console / file sink callbacks.
   *
   * @details
   * Invoked synchronously from the logging thread; the @c std::string_view is valid only for
   * the duration of the call.
   */
  using Callback = MoveFunction<void(Level, std::string_view)>;

  /**
   * @brief Carries the source file name and line number for the detail prefix.
   *
   * @details
   * Built automatically by @c VLINK_LOG_GET_DETAIL when the message level reaches
   * @c kDetailLevel.
   */
  using DetailInfo = std::pair<std::string_view, int>;

  /**
   * @brief Sentinel type indicating no detail prefix is attached.
   *
   * @details
   * Used to avoid capturing @c __FILE__ / @c __LINE__ for messages below @c kDetailLevel.
   */
  struct NoDetail {};

  /**
   * @brief Initialises the logger singleton.
   *
   * @details
   * Must be invoked before any logging macros.  Subsequent calls reconfigure the singleton.
   * Provide a non-empty @p log_path to activate the file sink.
   *
   * @param app_name  Application name embedded in log output.  Default: empty.
   * @param log_path  Absolute path for the file sink.  Default: empty (no file sink).
   */
  static void init(const std::string& app_name = "", const std::string& log_path = "") noexcept;

  /**
   * @brief Returns the global logger instance.
   *
   * @return Reference to the singleton.
   */
  static Logger& get() noexcept;

  /**
   * @brief Flushes every active sink.
   *
   * @details
   * Useful before abnormal termination.  Invoked automatically before a @c kFatal message
   * throws.
   */
  static void flush() noexcept;

  /**
   * @brief Installs a custom console sink callback replacing the built-in console writer.
   *
   * @param callback  Handler called with @c (level, @c message_view) for each record.
   */
  static void register_console_handler(Callback&& callback) noexcept;

  /**
   * @brief Installs a custom file sink callback replacing the built-in file writer.
   *
   * @param callback  Handler called with @c (level, @c message_view) for each record.
   */
  static void register_file_handler(Callback&& callback) noexcept;

  /**
   * @brief Sets the minimum severity for the console sink; pass @c kOff to mute it.
   *
   * @param level  Minimum output level.
   */
  static void set_console_level(Level level) noexcept;

  /**
   * @brief Sets the minimum severity for the file sink; pass @c kOff to mute it.
   *
   * @param level  Minimum output level.
   */
  static void set_file_level(Level level) noexcept;

  /**
   * @brief Enables or disables ANSI colour / formatting on the console sink.
   *
   * @param enable  @c true to keep ANSI escapes (default), @c false for plain text.
   */
  static void set_console_fmt_enable(bool enable) noexcept;

  /**
   * @brief Returns the current console sink severity threshold.
   *
   * @return Current level.
   */
  [[nodiscard]] static Level get_console_level() noexcept;

  /**
   * @brief Returns the current file sink severity threshold.
   *
   * @return Current level.
   */
  [[nodiscard]] static Level get_file_level() noexcept;

  /**
   * @brief Returns whether ANSI colour codes are enabled on the console sink.
   *
   * @return @c true when ANSI escapes are emitted.
   */
  [[nodiscard]] static bool get_console_fmt_enable() noexcept;

  /**
   * @brief Sets @c std::ios_base format flags applied to stream-style records.
   *
   * @param flags  Stream format flags.
   */
  static void set_stream_flag(std::ios_base::fmtflags flags) noexcept;

  /**
   * @brief Sets the floating-point precision for stream-style records.
   *
   * @param precision  Precision passed to @c std::setprecision.
   */
  static void set_stream_precision(int precision) noexcept;

  /**
   * @brief Sets the minimum field width for stream-style records.
   *
   * @param width  Field width passed to @c std::setw.
   */
  static void set_stream_width(int width) noexcept;

  /**
   * @brief Returns the stream format flags currently applied to stream-style records.
   *
   * @return Format flags.
   */
  [[nodiscard]] static std::ios_base::fmtflags get_stream_flag() noexcept;

  /**
   * @brief Returns the floating-point precision used for stream-style records.
   *
   * @return Precision value.
   */
  [[nodiscard]] static int get_stream_precision() noexcept;

  /**
   * @brief Returns the field width used for stream-style records.
   *
   * @return Width value.
   */
  [[nodiscard]] static int get_stream_width() noexcept;

  /**
   * @brief Enables a ring-buffer backtrace of the most recent @p size records.
   *
   * @param size  Capacity of the backtrace ring buffer.
   */
  static void enable_backtrace(size_t size) noexcept;

  /**
   * @brief Disables backtrace capture and discards the ring buffer.
   */
  static void disable_backtrace() noexcept;

  /**
   * @brief Flushes the backtrace ring buffer to the active sinks.
   */
  static void dump_backtrace() noexcept;

  /**
   * @brief Reports whether the logger is currently writing a record.
   *
   * @return @c true while a write is in progress.
   */
  [[nodiscard]] static bool is_busy() noexcept;

  /**
   * @brief Reports whether a record at @p level would currently be emitted.
   *
   * @details
   * Use the result to gate expensive argument computation before a macro call.
   *
   * @param level  Severity level under test.
   * @return @c true when the level passes either sink threshold.
   */
  [[nodiscard]] static bool is_writable(Level level) noexcept;

  /**
   * @brief Strips a path down to its final filename component at compile time.
   *
   * @param path  Source path, typically @c __FILE__.
   * @return View covering the filename portion.
   */
  [[nodiscard]] static constexpr std::string_view extract_filename(std::string_view path) noexcept;

  /**
   * @brief Stream-style entry point (used by @c VLOG_* / @c print).
   *
   * @details
   * Returns immediately when @c should_log<LevelT>() is @c false.  Otherwise streams @p args
   * into a thread-local @c FastStream and hands the resulting view to the sinks.
   *
   * @tparam LevelT   Compile-time severity.
   * @tparam DetailT  Either @c DetailInfo or @c NoDetail.
   * @tparam ArgsT    Stream argument types.
   * @param detail    Source location info or @c NoDetail{}.
   * @param args      Values to stream.
   */
  template <Level LevelT, typename DetailT, typename... ArgsT>
  static void print_stream_style(DetailT&& detail, ArgsT&&... args);

  /**
   * @brief Placeholder-style entry point (used by @c MLOG_*).
   *
   * @tparam LevelT   Compile-time severity.
   * @tparam DetailT  Either @c DetailInfo or @c NoDetail.
   * @tparam ArgsT    Format argument types.
   * @param detail    Source location info or @c NoDetail{}.
   * @param format    Format string with @c {} placeholders.
   * @param args      Format arguments.
   */
  template <Level LevelT, typename DetailT, typename... ArgsT>
  static void print_format_style(DetailT&& detail, format::format_string<ArgsT...> format, ArgsT&&... args);

  /**
   * @brief printf-style entry point (used by @c CLOG_*).
   *
   * @tparam LevelT   Compile-time severity.
   * @tparam DetailT  Either @c DetailInfo or @c NoDetail.
   * @tparam FormatT  Format string type, typically @c const @c char*.
   * @tparam ArgsT    printf argument types.
   * @param detail    Source location info or @c NoDetail{}.
   * @param format    printf-style format string.
   * @param args      printf arguments.
   */
  template <Logger::Level LevelT, typename DetailT, typename FormatT, typename... ArgsT>
  static void print_c_style(DetailT&& detail, FormatT&& format, ArgsT&&... args);

  /**
   * @brief Convenience stream-style entry point without source location.
   *
   * @tparam LevelT  Compile-time severity.
   * @tparam ArgsT   Stream argument types.
   * @param args  Values to stream.
   */
  template <Level LevelT, typename... ArgsT>
  static void print(ArgsT&&... args);

  /**
   * @class WrapperStream
   * @brief RAII helper backing @c SLOG_*; collects tokens and flushes on destruction.
   *
   * @details
   * When @c kIsEnabled is @c false at the chosen level the type and its methods compile to
   * nothing, so disabled-level call sites have zero runtime overhead.
   *
   * @tparam LevelT  Compile-time severity level.
   */
  template <Logger::Level LevelT>
  class WrapperStream final {
   public:
    /**
     * @brief Static gate indicating whether the wrapper emits at the chosen level.
     */
    static constexpr bool kIsEnabled = (LevelT >= kMinimumLevel && LevelT < Logger::kOff);

    explicit WrapperStream(Logger::NoDetail) noexcept {
      if constexpr (kIsEnabled) {
        if (should_log<LevelT>()) {
          enabled_ = true;
          stream_ = &Logger::get_local_stream();
        }
      }
    }

    explicit WrapperStream(DetailInfo&& detail) noexcept {
      if constexpr (kIsEnabled) {
        if (should_log<LevelT>()) {
          enabled_ = true;
          stream_ = &Logger::get_local_stream();

          push_detail_to_stream(detail, *stream_);
        }
      }
    }

    WrapperStream(WrapperStream&& other) noexcept : stream_(other.stream_), enabled_(other.enabled_) {
      other.stream_ = nullptr;
      other.enabled_ = false;
    }

    WrapperStream& operator=(WrapperStream&&) = delete;

    ~WrapperStream() noexcept(LevelT != Level::kFatal) {
      if constexpr (kIsEnabled) {
        if (enabled_) {
          finalize_log<LevelT>(stream_->take_view());
        }
      }
    }

    template <typename T>
    WrapperStream& operator<<(T&& t) noexcept {
      if constexpr (kIsEnabled) {
        if (enabled_) {
          *stream_ << std::forward<T>(t);
        }
      }

      return *this;
    }

   private:
    VLINK_DISALLOW_COPY_AND_ASSIGN(WrapperStream)

    FastStream* stream_{nullptr};
    bool enabled_{false};
  };

 private:
  Logger() noexcept;

  ~Logger() noexcept;

  template <Level LevelT>
  static bool should_log() noexcept;

  template <Level LevelT>
  static void finalize_log(std::string_view log_view);

  template <typename DetailT>
  static void push_detail_to_stream(DetailT&& detail, FastStream& stream) noexcept;

  template <typename DetailT>
  static std::string_view format_with_detail(DetailT&& detail, const char* msg, int len) noexcept;

  static char* get_local_buffer() noexcept;

  static FastStream& get_local_stream() noexcept;

  void write_to_console(Level level, std::string_view log) noexcept;

  void write_to_file(Level level, std::string_view log) noexcept;

  struct Impl;
  std::unique_ptr<Impl> impl_;

  template <Logger::Level LevelT>
  friend class WrapperStream;

  VLINK_SINGLETON_CHECK(Logger)

  VLINK_DISALLOW_COPY_AND_ASSIGN(Logger)
};

////////////////////////////////////////////////////////////////
/// Details
////////////////////////////////////////////////////////////////

inline constexpr std::string_view Logger::extract_filename(std::string_view path) noexcept {
  auto pos = path.find_last_of("/\\");
  return (pos == std::string_view::npos) ? path : path.substr(pos + 1);
}

template <Logger::Level LevelT, typename DetailT, typename... ArgsT>
inline void Logger::print_stream_style([[maybe_unused]] DetailT&& detail, [[maybe_unused]] ArgsT&&... args) {
  if (!should_log<LevelT>()) {
    return;
  }

  auto& stream = get_local_stream();

  if constexpr (std::is_same_v<std::decay_t<DetailT>, DetailInfo>) {
    push_detail_to_stream(detail, stream);
  }

  (void)(stream << ... << args);

  finalize_log<LevelT>(stream.take_view());
}

template <Logger::Level LevelT, typename DetailT, typename... ArgsT>
inline void Logger::print_format_style([[maybe_unused]] DetailT&& detail,
                                       [[maybe_unused]] format::format_string<ArgsT...> format,
                                       [[maybe_unused]] ArgsT&&... args) {
  if (!should_log<LevelT>()) {
    return;
  }

  std::string_view log_view;

  auto* local_buffer = get_local_buffer();
  auto result = format::format_to_n(local_buffer, kLocalBufferSize - 1, format, std::forward<ArgsT>(args)...);
  auto written = static_cast<int>(result.out - local_buffer);

  local_buffer[written] = '\0';

  log_view = format_with_detail(detail, local_buffer, written);

  finalize_log<LevelT>(log_view);
}

template <Logger::Level LevelT, typename DetailT, typename FormatT, typename... ArgsT>
inline void Logger::print_c_style([[maybe_unused]] DetailT&& detail, [[maybe_unused]] FormatT&& format,
                                  [[maybe_unused]] ArgsT&&... args) {
  if (!should_log<LevelT>()) {
    return;
  }

  std::string_view log_view;

  if constexpr (sizeof...(ArgsT) == 0) {
    auto& stream = get_local_stream();

    if constexpr (std::is_same_v<std::decay_t<DetailT>, DetailInfo>) {
      push_detail_to_stream(detail, stream);
    }

    stream << format;
    log_view = stream.take_view();
  } else {
    auto* local_buffer = get_local_buffer();
    auto written = std::snprintf(local_buffer, kLocalBufferSize - 1, format, args...);

    if VUNLIKELY (written < 0) {
      written = 0;
    } else if VUNLIKELY (written > kLocalBufferSize - 1) {
      written = kLocalBufferSize - 1;
    }

    log_view = format_with_detail(detail, local_buffer, written);
  }

  finalize_log<LevelT>(log_view);
}

template <Logger::Level LevelT, typename... ArgsT>
inline void Logger::print([[maybe_unused]] ArgsT&&... args) {
  print_stream_style<LevelT>(NoDetail{}, args...);
}

template <Logger::Level LevelT>
inline bool Logger::should_log() noexcept {
  if constexpr (LevelT < Logger::kMinimumLevel || LevelT >= Logger::kOff) {
    return false;
  } else if constexpr (LevelT == Logger::kFatal) {
    return true;
  } else {
    return Logger::is_writable(LevelT);
  }
}

template <Logger::Level LevelT>
inline void Logger::finalize_log(std::string_view log_view) {
  Logger& instance = Logger::get();

  instance.write_to_console(LevelT, log_view);
  instance.write_to_file(LevelT, log_view);

  if constexpr (LevelT == Logger::kFatal) {
    Logger::flush();
    throw Exception::RuntimeError(std::string(log_view));
  }
}

template <typename DetailT>
inline void Logger::push_detail_to_stream(DetailT&& detail, FastStream& stream) noexcept {
  auto& [file, line] = detail;
  stream << "{" << file << ":" << line << "} ";
}

template <typename DetailT>
inline std::string_view Logger::format_with_detail(DetailT&& detail, const char* msg, int len) noexcept {
  if constexpr (std::is_same_v<std::decay_t<DetailT>, Logger::DetailInfo>) {
    auto& stream = Logger::get_local_stream();

    push_detail_to_stream(detail, stream);

    if VLIKELY (len > 0) {
      stream.write_raw(msg, static_cast<size_t>(len));
    }

    return stream.take_view();
  } else {
    if VLIKELY (len > 0) {
      return std::string_view(msg, static_cast<size_t>(len));
    }

    return {};
  }
}

}  // namespace vlink

using VLinkLogger = vlink::Logger;

////////////////////////////////////////////////////////////////
/// Macro Definitions
////////////////////////////////////////////////////////////////

#define VLINK_LOG_GET_DETAIL(level)                                                      \
  ([]() -> auto {                                                                        \
    if constexpr ((level) >= VLinkLogger::kDetailLevel) {                                \
      return VLinkLogger::DetailInfo{VLinkLogger::extract_filename(__FILE__), __LINE__}; \
    } else {                                                                             \
      return VLinkLogger::NoDetail{};                                                    \
    }                                                                                    \
  })()

#define VLINK_LOG_HEX(offset) std::hex, std::uppercase, std::setw(offset), std::setfill('0')

#define VLINK_LOG_HEXSS(offset) std::hex << std::uppercase << std::setw(offset) << std::setfill('0')

#define VLINK_LOG_IF_T VLinkLogger::is_writable(VLinkLogger::kTrace)

#define VLINK_LOG_IF_D VLinkLogger::is_writable(VLinkLogger::kDebug)

#define VLINK_LOG_IF_I VLinkLogger::is_writable(VLinkLogger::kInfo)

#define VLINK_LOG_IF_W VLinkLogger::is_writable(VLinkLogger::kWarn)

#define VLINK_LOG_IF_E VLinkLogger::is_writable(VLinkLogger::kError)

#define VLINK_LOG_IF_F VLinkLogger::is_writable(VLinkLogger::kFatal)

#define VLINK_LOG_T(...) \
  VLinkLogger::print_stream_style<VLinkLogger::kTrace>(VLINK_LOG_GET_DETAIL(VLinkLogger::kTrace), __VA_ARGS__)

#define VLINK_LOG_D(...) \
  VLinkLogger::print_stream_style<VLinkLogger::kDebug>(VLINK_LOG_GET_DETAIL(VLinkLogger::kDebug), __VA_ARGS__)

#define VLINK_LOG_I(...) \
  VLinkLogger::print_stream_style<VLinkLogger::kInfo>(VLINK_LOG_GET_DETAIL(VLinkLogger::kInfo), __VA_ARGS__)

#define VLINK_LOG_W(...) \
  VLinkLogger::print_stream_style<VLinkLogger::kWarn>(VLINK_LOG_GET_DETAIL(VLinkLogger::kWarn), __VA_ARGS__)

#define VLINK_LOG_E(...) \
  VLinkLogger::print_stream_style<VLinkLogger::kError>(VLINK_LOG_GET_DETAIL(VLinkLogger::kError), __VA_ARGS__)

#define VLINK_LOG_F(...) \
  VLinkLogger::print_stream_style<VLinkLogger::kFatal>(VLINK_LOG_GET_DETAIL(VLinkLogger::kFatal), __VA_ARGS__)

#define VLINK_MLOG_T(...) \
  VLinkLogger::print_format_style<VLinkLogger::kTrace>(VLINK_LOG_GET_DETAIL(VLinkLogger::kTrace), __VA_ARGS__)

#define VLINK_MLOG_D(...) \
  VLinkLogger::print_format_style<VLinkLogger::kDebug>(VLINK_LOG_GET_DETAIL(VLinkLogger::kDebug), __VA_ARGS__)

#define VLINK_MLOG_I(...) \
  VLinkLogger::print_format_style<VLinkLogger::kInfo>(VLINK_LOG_GET_DETAIL(VLinkLogger::kInfo), __VA_ARGS__)

#define VLINK_MLOG_W(...) \
  VLinkLogger::print_format_style<VLinkLogger::kWarn>(VLINK_LOG_GET_DETAIL(VLinkLogger::kWarn), __VA_ARGS__)

#define VLINK_MLOG_E(...) \
  VLinkLogger::print_format_style<VLinkLogger::kError>(VLINK_LOG_GET_DETAIL(VLinkLogger::kError), __VA_ARGS__)

#define VLINK_MLOG_F(...) \
  VLinkLogger::print_format_style<VLinkLogger::kFatal>(VLINK_LOG_GET_DETAIL(VLinkLogger::kFatal), __VA_ARGS__)

#define VLINK_CLOG_T(...) \
  VLinkLogger::print_c_style<VLinkLogger::kTrace>(VLINK_LOG_GET_DETAIL(VLinkLogger::kTrace), __VA_ARGS__)

#define VLINK_CLOG_D(...) \
  VLinkLogger::print_c_style<VLinkLogger::kDebug>(VLINK_LOG_GET_DETAIL(VLinkLogger::kDebug), __VA_ARGS__)

#define VLINK_CLOG_I(...) \
  VLinkLogger::print_c_style<VLinkLogger::kInfo>(VLINK_LOG_GET_DETAIL(VLinkLogger::kInfo), __VA_ARGS__)

#define VLINK_CLOG_W(...) \
  VLinkLogger::print_c_style<VLinkLogger::kWarn>(VLINK_LOG_GET_DETAIL(VLinkLogger::kWarn), __VA_ARGS__)

#define VLINK_CLOG_E(...) \
  VLinkLogger::print_c_style<VLinkLogger::kError>(VLINK_LOG_GET_DETAIL(VLinkLogger::kError), __VA_ARGS__)

#define VLINK_CLOG_F(...) \
  VLinkLogger::print_c_style<VLinkLogger::kFatal>(VLINK_LOG_GET_DETAIL(VLinkLogger::kFatal), __VA_ARGS__)

#define VLINK_SLOG_T VLinkLogger::WrapperStream<VLinkLogger::kTrace>(VLINK_LOG_GET_DETAIL(VLinkLogger::kTrace))

#define VLINK_SLOG_D VLinkLogger::WrapperStream<VLinkLogger::kDebug>(VLINK_LOG_GET_DETAIL(VLinkLogger::kDebug))

#define VLINK_SLOG_I VLinkLogger::WrapperStream<VLinkLogger::kInfo>(VLINK_LOG_GET_DETAIL(VLinkLogger::kInfo))

#define VLINK_SLOG_W VLinkLogger::WrapperStream<VLinkLogger::kWarn>(VLINK_LOG_GET_DETAIL(VLinkLogger::kWarn))

#define VLINK_SLOG_E VLinkLogger::WrapperStream<VLinkLogger::kError>(VLINK_LOG_GET_DETAIL(VLinkLogger::kError))

#define VLINK_SLOG_F VLinkLogger::WrapperStream<VLinkLogger::kFatal>(VLINK_LOG_GET_DETAIL(VLinkLogger::kFatal))

#ifndef VLINK_LOG_DISABLE_SHORT

#define VLOG_T(...) VLINK_LOG_T(__VA_ARGS__)

#define VLOG_D(...) VLINK_LOG_D(__VA_ARGS__)

#define VLOG_I(...) VLINK_LOG_I(__VA_ARGS__)

#define VLOG_W(...) VLINK_LOG_W(__VA_ARGS__)

#define VLOG_E(...) VLINK_LOG_E(__VA_ARGS__)

#define VLOG_F(...) VLINK_LOG_F(__VA_ARGS__)

#define CLOG_T(...) VLINK_CLOG_T(__VA_ARGS__)

#define CLOG_D(...) VLINK_CLOG_D(__VA_ARGS__)

#define CLOG_I(...) VLINK_CLOG_I(__VA_ARGS__)

#define CLOG_W(...) VLINK_CLOG_W(__VA_ARGS__)

#define CLOG_E(...) VLINK_CLOG_E(__VA_ARGS__)

#define CLOG_F(...) VLINK_CLOG_F(__VA_ARGS__)

#define MLOG_T(...) VLINK_MLOG_T(__VA_ARGS__)

#define MLOG_D(...) VLINK_MLOG_D(__VA_ARGS__)

#define MLOG_I(...) VLINK_MLOG_I(__VA_ARGS__)

#define MLOG_W(...) VLINK_MLOG_W(__VA_ARGS__)

#define MLOG_E(...) VLINK_MLOG_E(__VA_ARGS__)

#define MLOG_F(...) VLINK_MLOG_F(__VA_ARGS__)

#define SLOG_T VLINK_SLOG_T

#define SLOG_D VLINK_SLOG_D

#define SLOG_I VLINK_SLOG_I

#define SLOG_W VLINK_SLOG_W

#define SLOG_E VLINK_SLOG_E

#define SLOG_F VLINK_SLOG_F

#endif
