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
 * @file process.h
 * @brief Portable child-process driver with asynchronous I/O notifications.
 *
 * @details
 * @c vlink::Process spawns a single child program at a time and exposes a QProcess-style
 * surface for inspecting its lifecycle, reading captured output, writing to its standard
 * input, and triggering termination.  Output capture, exit notification, and timer-driven
 * polling all run on an internal monitor thread.
 *
 * Capability groups exposed by the class:
 *
 * | Group                 | Representative members                                                   |
 * | --------------------- | ------------------------------------------------------------------------ |
 * | Lifecycle             | @c start, @c start_command, @c terminate, @c kill, @c close              |
 * | Process metadata      | @c get_pid, @c get_state, @c get_error, @c get_exit_code                 |
 * | Environment & cwd     | @c set_environment, @c set_inherit_environment, @c set_working_directory |
 * | I/O routing           | @c set_process_mode, @c get_process_mode                                 |
 * | stdout/stderr capture | @c read_line_stdout, @c read_all, @c bytes_available_stdout              |
 * | stdin writing         | @c write, @c close_write_channel                                         |
 * | Async notifications   | @c register_finished_callback, @c register_ready_read_stdout_callback    |
 * | Synchronous waits     | @c wait_for_started, @c wait_for_finished, @c wait_for_ready_read        |
 * | Static helpers        | @c execute, @c start_detached                                            |
 *
 * I/O channel routing modes:
 *
 * | Mode                    | stdout              | stderr              |
 * | ----------------------- | ------------------- | ------------------- |
 * | @c kSeparateMode        | Buffered pipe       | Buffered pipe       |
 * | @c kMergedMode          | Buffered pipe       | Merged into stdout  |
 * | @c kForwardedMode       | Inherits parent     | Inherits parent     |
 * | @c kForwardedOutputMode | Inherits parent     | Buffered pipe       |
 * | @c kForwardedErrorMode  | Buffered pipe       | Inherits parent     |
 *
 * @note
 * - Every callback fires from the monitor thread; protect shared caller state accordingly.
 * - The destructor first sends @c SIGTERM, drains pending I/O, then optionally escalates to
 *   @c SIGKILL after @c kDestructorWaitTimeoutMs (5000 ms) before joining.
 * - The class is non-copyable and non-movable.
 *
 * @par Example
 * @code
 * vlink::Process proc;
 * proc.set_process_mode(vlink::Process::kSeparateMode);
 * proc.register_ready_read_stdout_callback([&proc] {
 *   std::string line;
 *   while (proc.can_read_line_stdout()) {
 *     proc.read_line_stdout(line);
 *   }
 * });
 * proc.register_finished_callback([](int code, vlink::Process::ExitStatus status) {
 *   (void)code;
 *   (void)status;
 * });
 * proc.start("/usr/bin/ls", {"-la"});
 * proc.wait_for_finished();
 * @endcode
 */

#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "./functional.h"
#include "./macros.h"

namespace vlink {

/**
 * @class Process
 * @brief Owns and drives a single child process with asynchronous I/O reporting.
 *
 * @details
 * The instance is non-copyable and non-movable.  A new child may only be launched after
 * any previous one has terminated and the internal state has been cleaned up.
 */
class VLINK_EXPORT Process {
 public:
  /**
   * @enum State
   * @brief Coarse lifecycle stage of the child process.
   */
  enum State : uint8_t {
    kNotRunningState = 0,  ///< No child is currently active.
    kStartingState = 1,    ///< @c start() has been called and the OS is launching the binary.
    kRunningState = 2,     ///< Child has been successfully launched and is executing.
  };

  /**
   * @enum ExitStatus
   * @brief How the most recent child exited.
   */
  enum ExitStatus : uint8_t {
    kNormalExitStatus = 0,  ///< Child returned from @c main or called @c exit normally.
    kCrashExitStatus = 1,   ///< Child was terminated by a signal or crashed.
  };

  /**
   * @enum Error
   * @brief Sticky error indicator set by failing operations.
   */
  enum Error : uint8_t {
    kNoError = 0,              ///< No error has been observed.
    kUnknownError = 1,         ///< Catch-all bucket for unmapped errors.
    kStartError = 2,           ///< Failed to launch the requested program.
    kCrashedError = 3,         ///< Child process crashed.
    kTimedOutError = 4,        ///< A blocking wait expired before its condition was met.
    kWriteError = 5,           ///< Writing to the child's stdin failed.
    kReadError = 6,            ///< Reading from a captured pipe failed.
    kBufferOverflowError = 7,  ///< Captured output exceeded @c set_max_buffer_size.
  };

  /**
   * @enum Mode
   * @brief Routing of the child's stdout/stderr file descriptors.
   */
  enum Mode : uint8_t {
    kSeparateMode = 0,         ///< Capture stdout and stderr through independent pipes.
    kMergedMode = 1,           ///< Capture stdout; merge stderr into the stdout pipe.
    kForwardedMode = 2,        ///< Inherit parent stdout/stderr without capture.
    kForwardedOutputMode = 3,  ///< Inherit stdout; capture stderr.
    kForwardedErrorMode = 4,   ///< Capture stdout; inherit stderr.
  };

  /**
   * @brief Map type used to pass environment overrides to @c set_environment().
   */
  using EnvironmentMap = std::unordered_map<std::string, std::string>;

  /**
   * @brief Callback signature invoked when the @c Error state changes.
   */
  using ErrorCallback = Function<void(Error)>;

  /**
   * @brief Callback signature invoked when the child exits.
   *
   * @details
   * The first parameter is the exit code; the second is @c kNormalExitStatus or
   * @c kCrashExitStatus.
   */
  using FinishedCallback = Function<void(int, ExitStatus)>;

  /**
   * @brief Callback signature invoked whenever new data is available on a pipe.
   */
  using ReadyReadCallback = Function<void()>;

  /**
   * @brief Callback signature invoked when the process @c State transitions.
   */
  using StateChangedCallback = Function<void(State)>;

  /**
   * @brief Sentinel value used to request an unbounded wait.
   */
  static constexpr int kInfinite{-1};

  /**
   * @brief Default deadline for @c wait_for_started / @c wait_for_finished (3000 ms).
   */
  static constexpr int kDefaultWaitTimeoutMs{3000};

  /**
   * @brief Default deadline for blocking writes to the child's stdin (5000 ms).
   */
  static constexpr int kDefaultWriteTimeoutMs{5000};

  /**
   * @brief Default deadline for the synchronous @c execute() helper (30000 ms).
   */
  static constexpr int kDefaultExecuteTimeoutMs{30000};

  /**
   * @brief Grace period the destructor allows after sending @c SIGTERM before escalating.
   */
  static constexpr int kDestructorWaitTimeoutMs{5000};

  /**
   * @brief Constructs an idle process driver with no child attached.
   */
  Process();

  /**
   * @brief Destructor.  Terminates the child if still alive then frees driver resources.
   *
   * @details
   * The sequence sends @c SIGTERM via @c terminate(), drains any pending pipe data,
   * waits up to @c kDestructorWaitTimeoutMs for a graceful exit, then escalates to
   * @c SIGKILL via @c kill() and waits up to a further 1000 ms before releasing
   * the I/O thread, pipes and child handles.
   */
  ~Process();

  Process(Process&& other) noexcept = delete;

  Process& operator=(Process&& other) noexcept = delete;

  /**
   * @brief Returns the lifecycle state of the currently attached child.
   *
   * @return Current @c State value.
   */
  [[nodiscard]] State get_state() const;

  /**
   * @brief Returns the most recently latched error code.
   *
   * @return Sticky @c Error value; @c kNoError if no error has occurred.
   */
  [[nodiscard]] Error get_error() const;

  /**
   * @brief Returns the exit code of the most recent child process.
   *
   * @details
   * Only meaningful once @c get_state() reports @c kNotRunningState.
   *
   * @return Exit code passed to @c exit() or implied by signal termination.
   */
  [[nodiscard]] int get_exit_code() const;

  /**
   * @brief Returns how the most recent child terminated.
   *
   * @return @c kNormalExitStatus on graceful exit, @c kCrashExitStatus on signal/crash.
   */
  [[nodiscard]] ExitStatus get_exit_status() const;

  /**
   * @brief Reports whether the attached child is currently running.
   *
   * @return @c true when @c get_state() equals @c kRunningState.
   */
  [[nodiscard]] bool is_running() const;

  /**
   * @brief Returns the OS-level identifier of the child process.
   *
   * @return Process ID, or @c -1 when no child is attached.
   */
  [[nodiscard]] int64_t get_process_id() const;

  /**
   * @brief Limits the in-memory capture buffer used for stdout/stderr.
   *
   * @details
   * When the captured output exceeds @p size bytes the @c kBufferOverflowError code is
   * latched.  Passing @c 0 restores the default of 16 MiB.
   *
   * @param size  New buffer limit in bytes.
   */
  void set_max_buffer_size(size_t size);

  /**
   * @brief Returns the configured capture-buffer limit.
   *
   * @return Buffer size in bytes; default is 16 MiB.
   */
  [[nodiscard]] size_t get_max_buffer_size() const;

  /**
   * @brief Defines the environment that will be applied at the next @c start() call.
   *
   * @param env_map  Variable map to merge into or replace the parent environment based on
   *                 the @c set_inherit_environment() setting.
   */
  void set_environment(const EnvironmentMap& env_map);

  /**
   * @brief Returns the currently configured environment override.
   *
   * @return Environment variable map.
   */
  [[nodiscard]] EnvironmentMap get_environment() const;

  /**
   * @brief Sets the I/O channel routing that will be applied at the next @c start() call.
   *
   * @param mode  Routing mode for stdout/stderr.
   */
  void set_process_mode(Mode mode);

  /**
   * @brief Returns the currently configured I/O routing mode.
   *
   * @return Current @c Mode.
   */
  [[nodiscard]] Mode get_process_mode() const;

  /**
   * @brief Controls whether the child inherits the parent environment in addition to the override map.
   *
   * @param inherit  @c true to merge with the parent environment, @c false to use only the override map.
   */
  void set_inherit_environment(bool inherit);

  /**
   * @brief Reports whether the child currently inherits the parent environment.
   *
   * @return @c true when the parent environment is merged.
   */
  [[nodiscard]] bool get_inherit_environment() const;

  /**
   * @brief Sets the working directory used at the next @c start() call.
   *
   * @param dir  Absolute path the child should @c chdir to before executing the program.
   */
  void set_working_directory(const std::string& dir);

  /**
   * @brief Returns the currently configured working directory.
   *
   * @return Working directory path.
   */
  [[nodiscard]] std::string get_working_directory() const;

  /**
   * @brief Installs a callback fired when the @c Error state changes.
   *
   * @param callback  Callback invoked from the monitor thread.
   */
  void register_error_callback(ErrorCallback&& callback);

  /**
   * @brief Installs a callback fired when the child exits.
   *
   * @param callback  Callback invoked with exit code and exit status.
   */
  void register_finished_callback(FinishedCallback&& callback);

  /**
   * @brief Installs a callback fired when stdout has new data buffered.
   *
   * @param callback  Callback invoked from the monitor thread.
   */
  void register_ready_read_stdout_callback(ReadyReadCallback&& callback);

  /**
   * @brief Installs a callback fired when stderr has new data buffered.
   *
   * @param callback  Callback invoked from the monitor thread.
   */
  void register_ready_read_stderr_callback(ReadyReadCallback&& callback);

  /**
   * @brief Installs a callback fired on every @c State transition.
   *
   * @param callback  Callback invoked from the monitor thread with the new @c State.
   */
  void register_state_changed_callback(StateChangedCallback&& callback);

  /**
   * @brief Launches @p program with the given argument vector.
   *
   * @details
   * Returns once the launch attempt has either succeeded (state becomes @c kRunningState)
   * or failed (an @c Error is latched).  I/O monitoring continues asynchronously on the
   * internal monitor thread.
   *
   * @param program    Path to the executable.
   * @param arguments  Argument vector excluding @c argv[0].
   */
  void start(const std::string& program, const std::vector<std::string>& arguments = {});

  /**
   * @brief Parses a shell-style command line and launches it.
   *
   * @details
   * Splits @p command on whitespace with quote and backslash handling, then delegates to
   * @c start().
   *
   * @param command  Shell-style command string.
   */
  void start_command(const std::string& command);

  /**
   * @brief Blocks until the child reaches @c kRunningState or the timeout elapses.
   *
   * @param msecs  Maximum wait in milliseconds.  Default: @c kDefaultWaitTimeoutMs.
   * @return @c true when the running state was observed in time.
   */
  bool wait_for_started(int msecs = kDefaultWaitTimeoutMs);

  /**
   * @brief Blocks until the child terminates or the timeout elapses.
   *
   * @param msecs  Maximum wait in milliseconds.  Default: @c kDefaultWaitTimeoutMs.
   * @return @c true when the child has exited within the timeout.
   */
  bool wait_for_finished(int msecs = kDefaultWaitTimeoutMs);

  /**
   * @brief Blocks until new pipe data becomes available or the timeout elapses.
   *
   * @param msecs  Maximum wait in milliseconds.  Default: @c kDefaultWaitTimeoutMs.
   * @return @c true when data is now available on stdout or stderr.
   */
  bool wait_for_ready_read(int msecs = kDefaultWaitTimeoutMs);

  /**
   * @brief Requests cooperative termination of the child.
   *
   * @details
   * POSIX sends @c SIGTERM, which the child may catch.  Windows uses @c TerminateProcess,
   * which is unconditional.  @c kill() must be used for a forced exit on POSIX.
   */
  void terminate();

  /**
   * @brief Forces immediate termination of the child via @c SIGKILL / @c TerminateProcess.
   */
  void kill();

  /**
   * @brief Sends @c SIGTERM and optionally escalates to @c kill() after the wait timeout.
   *
   * @param force_kill_on_timeout  When @c true and the child has not exited within
   *                               @c kDefaultWaitTimeoutMs, @c kill() is invoked.  On
   *                               Windows @c terminate() is already forceful so the flag
   *                               is effectively ignored.  Default: @c false.
   */
  void close(bool force_kill_on_timeout = false);

  /**
   * @brief Returns the number of buffered bytes available to read from stdout.
   *
   * @return Bytes available on stdout.
   */
  [[nodiscard]] size_t bytes_available_stdout() const;

  /**
   * @brief Returns the number of buffered bytes available to read from stderr.
   *
   * @return Bytes available on stderr.
   */
  [[nodiscard]] size_t bytes_available_stderr() const;

  /**
   * @brief Reports whether a newline-terminated line is fully buffered on stdout.
   *
   * @return @c true when @c read_line_stdout() will deliver a complete line.
   */
  [[nodiscard]] bool can_read_line_stdout() const;

  /**
   * @brief Reports whether a newline-terminated line is fully buffered on stderr.
   *
   * @return @c true when @c read_line_stderr() will deliver a complete line.
   */
  [[nodiscard]] bool can_read_line_stderr() const;

  /**
   * @brief Reads one buffered line from stdout.
   *
   * @param line  Destination string; receives buffered data including a trailing newline
   *              when one was buffered.
   * @return @c true when at least one byte was copied into @p line.
   */
  bool read_line_stdout(std::string& line);

  /**
   * @brief Reads one buffered line from stderr.
   *
   * @param line  Destination string; receives buffered data including a trailing newline
   *              when one was buffered.
   * @return @c true when at least one byte was copied into @p line.
   */
  bool read_line_stderr(std::string& line);

  /**
   * @brief Reads up to @p max_size bytes of buffered stdout into @p buffer.
   *
   * @param buffer    Destination byte vector.
   * @param max_size  Upper bound on bytes to copy.
   * @return Number of bytes actually copied.
   */
  size_t read_stdout(std::vector<uint8_t>& buffer, size_t max_size);

  /**
   * @brief Reads up to @p max_size bytes of buffered stderr into @p buffer.
   *
   * @param buffer    Destination byte vector.
   * @param max_size  Upper bound on bytes to copy.
   * @return Number of bytes actually copied.
   */
  size_t read_stderr(std::vector<uint8_t>& buffer, size_t max_size);

  /**
   * @brief Drains all buffered stdout into @p buffer.
   *
   * @param buffer  Destination byte vector; replaced.
   * @return @c true when any data was copied.
   */
  bool read_all_output(std::vector<uint8_t>& buffer);

  /**
   * @brief Drains all buffered stderr into @p buffer.
   *
   * @param buffer  Destination byte vector; replaced.
   * @return @c true when any data was copied.
   */
  bool read_all_error(std::vector<uint8_t>& buffer);

  /**
   * @brief Drains all buffered stdout and stderr into @p buffer.
   *
   * @param buffer  Destination byte vector; replaced.
   * @return @c true when any data was copied.
   */
  bool read_all(std::vector<uint8_t>& buffer);

  /**
   * @brief Drains all buffered stdout into @p str.
   *
   * @param str  Destination string; replaced.
   * @return @c true when any data was copied.
   */
  bool read_all_output(std::string& str);

  /**
   * @brief Drains all buffered stderr into @p str.
   *
   * @param str  Destination string; replaced.
   * @return @c true when any data was copied.
   */
  bool read_all_error(std::string& str);

  /**
   * @brief Drains all buffered stdout and stderr into @p str.
   *
   * @param str  Destination string; replaced.
   * @return @c true when any data was copied.
   */
  bool read_all(std::string& str);

  /**
   * @brief Writes a byte buffer to the child's stdin.
   *
   * @param buffer     Bytes to write.
   * @param timeout_ms Maximum wait in milliseconds.  Default: @c kDefaultWriteTimeoutMs.
   * @return Number of bytes actually written.
   */
  size_t write(const std::vector<uint8_t>& buffer, int timeout_ms = kDefaultWriteTimeoutMs);

  /**
   * @brief Writes a string to the child's stdin.
   *
   * @param str        String to write.
   * @param timeout_ms Maximum wait in milliseconds.  Default: @c kDefaultWriteTimeoutMs.
   * @return Number of bytes actually written.
   */
  size_t write(const std::string& str, int timeout_ms = kDefaultWriteTimeoutMs);

  /**
   * @brief Closes the stdin pipe, signalling EOF to the child.
   */
  void close_write_channel();

  /**
   * @brief Synchronously executes a program and returns its exit code.
   *
   * @details
   * Standard output and standard error are discarded.  Returns @c -1 when the launch
   * fails or the timeout elapses.
   *
   * @param program     Path to the executable.
   * @param arguments   Argument vector.  Default: empty.
   * @param timeout_ms  Maximum wait in milliseconds.  Default: @c kDefaultExecuteTimeoutMs.
   * @return Exit code on success, or @c -1 on failure.
   */
  static int execute(const std::string& program, const std::vector<std::string>& arguments = {},
                     int timeout_ms = kDefaultExecuteTimeoutMs);

  /**
   * @brief Launches a program detached from the calling process and returns immediately.
   *
   * @param program    Path to the executable.
   * @param arguments  Argument vector.  Default: empty.
   * @return @c true when the OS reports a successful launch.
   */
  static bool start_detached(const std::string& program, const std::vector<std::string>& arguments = {});

 private:
  struct ReadResult final {
    bool has_stdout_data{false};
    bool has_stderr_data{false};
    bool truncated{false};
    bool read_error{false};
  };

  void set_state(State state);

  void set_error(Error error);

  void cleanup();

  ReadResult read_from_pipes();

  void report_read_result(const ReadResult& result);

  void read_from_pipes_with_lock();

  bool setup_pipes();

  bool start_program(const std::string& program, const std::vector<std::string>& arguments);

  void monitor_thread();

  void start_monitor_thread();

  void stop_monitor_thread();

  void handle_process_exit(int exit_code, ExitStatus status);

  void invoke_callbacks_outside_lock(Error error_to_report, bool has_finished, int exit_code_to_report,
                                     ExitStatus exit_status_to_report, State state_to_report, bool has_state_changed,
                                     bool has_stdout_data, bool has_stderr_data);

  struct Impl;
  std::unique_ptr<Impl> impl_;

  VLINK_DISALLOW_COPY_AND_ASSIGN(Process)
};

}  // namespace vlink
