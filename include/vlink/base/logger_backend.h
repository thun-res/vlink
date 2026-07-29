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
 * @file logger_backend.h
 * @brief Asynchronous file backend used by @c vlink::Logger.
 *
 * @details
 * The backend serialises file operations on a @c MessageLoop, periodically flushes through a
 * loop-bound @c Timer, supports fixed-name and timestamped size rotation, and retains an optional
 * backtrace ring.  Applications normally use it when @c ENABLE_LOG_BACKEND is enabled; the class
 * is also exposed for direct integration and focused backend testing.
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

#include "./functional.h"
#include "./logger.h"
#include "./message_loop.h"

namespace vlink {

/**
 * @class LoggerBackend
 * @brief MessageLoop-based asynchronous rotating-file backend.
 *
 * @details
 * Each accepted record owns its message until a single worker thread formats and writes it.
 * Queue overflow either blocks producers or discards an older record from the dispatcher queue
 * according to @c Config::block_when_full; Error and Fatal records always block and remain
 * protected.  Low-frequency control operations use protected queue barriers, so @c flush and
 * backtrace changes cannot be evicted by data-plane overflow.
 *
 * @note A backend must exclusively own its rotating-file set.  Separate processes or backend
 *       instances should use separate paths.  Flush transfers buffered bytes to the operating
 *       system but does not provide power-loss durability equivalent to @c fsync.
 * @note Stop and join all callers before destruction.  Inherited @c MessageLoop lifecycle
 *       controls may be used to stop the worker for diagnostics, but a stopped backend must not
 *       be restarted; direct shutdown may discard records that have not reached the worker batch.
 */
class VLINK_EXPORT LoggerBackend final : public MessageLoop {
 public:
  /**
   * @struct Config
   * @brief Construction-time queue, file and formatting settings.
   */
  struct Config final {
    std::string app_name;           ///< Application name used by fixed-name rotation.
    std::string log_path;           ///< Base directory for generated log files.
    size_t max_file_size{0};        ///< Rotation threshold in bytes; must be non-zero.
    size_t max_files{0};            ///< Timestamp retention target, or fixed-name backups excluding the active file.
    size_t queue_size{0};           ///< Dispatcher queue capacity; zero is clamped to one.
    uint32_t flush_interval_ms{0};  ///< Periodic flush interval; zero flushes each record.
    bool fixed_filename{false};     ///< Use @c <app>.log plus numeric backups when @c true.
    bool append{false};             ///< Continue the active/latest file instead of starting a new file.
    bool block_when_full{false};    ///< When true, block all producers; Error/Fatal are always protected.
    bool use_utc{false};            ///< Format timestamps and filenames in UTC.
  };

  /**
   * @brief Callback invoked once when the backend enters a permanent error state.
   *
   * @details
   * Receives the diagnostic message on the thread that detects the error.  The message view is
   * valid only during the callback.  The callback must not destroy this backend or wait for its
   * worker or invoke this backend; exceptions are isolated at the notification boundary.
   */
  using ErrorHandler = Function<void(std::string_view)>;

  /**
   * @brief Callback used to emit preformatted backtrace records to a console sink.
   *
   * @details
   * Receives the record level and one formatted line without its trailing newline.  When stored
   * by the constructor it receives Warn and higher records while backtrace capture is enabled;
   * @c dump_backtrace may provide a callback for retained records.  The line view is valid only
   * during the callback.  The callback must not destroy, log to or otherwise invoke this backend;
   * exceptions do not escape the notification boundary but put the backend into its permanent
   * error state.
   */
  using ConsoleWriter = Function<void(Logger::Level, std::string_view)>;

  /**
   * @brief Constructs and starts the asynchronous backend.
   *
   * @param config          Queue, file and formatting settings.
   * @param error_handler   Optional permanent-error notification.
   * @param console_writer  Optional backtrace console receiver.
   *
   * @throws std::invalid_argument for invalid size/count settings.
   * @throws std::runtime_error when the file or worker cannot be initialised.
   * @throws std::filesystem::filesystem_error when directory operations fail.
   * @throws std::system_error when a system resource cannot be created.
   * @throws std::bad_alloc when backend storage cannot be allocated.
   */
  explicit LoggerBackend(Config&& config, ErrorHandler&& error_handler, ConsoleWriter&& console_writer = nullptr);

  /**
   * @brief Drains records still pending at shutdown, flushes the file and stops the worker.
   */
  ~LoggerBackend() override;

  /**
   * @brief Enqueues one record.
   *
   * @param level    Record severity.
   * @param message  Message bytes copied into backend-owned storage.
   * @return @c true when accepted for processing; @c false for @c kOff, after shutdown or
   *         permanent error, on allocation/enqueue failure, or when a non-blocking full queue
   *         contains no droppable record.  Acceptance does not guarantee a later file write.
   */
  [[nodiscard]] bool log(Logger::Level level, std::string_view message) noexcept;

  /**
   * @brief Drains non-evicted records preceding the barrier and flushes the active file.
   */
  void flush() noexcept;

  /**
   * @brief Starts retaining the most recent records in a bounded ring.
   *
   * @param size  Maximum retained record count.
   */
  void enable_backtrace(size_t size) noexcept;

  /**
   * @brief Stops backtrace capture and discards retained records.
   */
  void disable_backtrace() noexcept;

  /**
   * @brief Writes retained records to the file and optional console callback.
   *
   * @param console_writer  Optional receiver for each preformatted line without its trailing newline.
   */
  void dump_backtrace(const ConsoleWriter& console_writer) noexcept;

  /**
   * @brief Reports whether the running backend entered a permanent error state.
   *
   * @return @c true after the first permanent backend error.
   */
  [[nodiscard]] bool has_error() const noexcept;

  /**
   * @brief Returns the configured MessageLoop dispatcher queue capacity.
   *
   * @return Number of MessageLoop dispatcher queue slots, excluding the current worker batch.
   */
  [[nodiscard]] size_t get_max_task_count() const override;

 private:
  struct LoggerRecord;

  struct Impl;

  void start_backend();

  void initialize_fixed();

  void initialize_timestamped();

  void open_current(bool append);

  void close_current() noexcept;

  void rotate_fixed();

  void rotate_timestamped();

  void flush_output();

  void write_output(std::string_view data);

  void update_timestamp(int64_t seconds);

  std::string_view format(const LoggerRecord& record);

  void fail(std::string_view message) noexcept;

  void flush_file() noexcept;

  void write(std::unique_ptr<LoggerRecord>&& record) noexcept;

  void barrier(Callback&& callback) noexcept;

  std::unique_ptr<Impl> impl_;

  VLINK_DISALLOW_COPY_AND_ASSIGN(LoggerBackend)
};

}  // namespace vlink
