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

#include "./base/process.h"

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <limits>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "./base/condition_variable.h"
#include "./base/helpers.h"
#include "./base/logger.h"

#ifdef _WIN32
#include <Windows.h>
#include <processthreadsapi.h>
#undef min
#undef max
#else
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <csignal>
#include <cstring>
#endif

#if defined(__APPLE__)
#include <crt_externs.h>
[[maybe_unused]] static inline char**& vlink_get_process_environ() { return *_NSGetEnviron(); }
#elif defined(_WIN32)
[[maybe_unused]] static inline char** vlink_get_process_environ() { return _environ; }
#else
extern "C" {
extern char** environ;
}
[[maybe_unused]] static inline char**& vlink_get_process_environ() { return environ; }
#endif

namespace vlink {

[[maybe_unused]] static constexpr int kExecFailedExitCode{127};
[[maybe_unused]] static constexpr size_t kDefaultMaxBufferSize{16 * 1024 * 1024};
[[maybe_unused]] static constexpr size_t kPipeBufferSize{8192};
[[maybe_unused]] static constexpr int kPollTimeoutMs{50};
[[maybe_unused]] static constexpr int kMonitorLoopDelayMs{10};

#ifdef _WIN32
[[maybe_unused]] static void close_handle_if_valid(HANDLE& handle) {
  if VLIKELY (handle != INVALID_HANDLE_VALUE && handle != nullptr) {
    ::CloseHandle(handle);
    handle = INVALID_HANDLE_VALUE;
  }
}

[[maybe_unused]] static bool is_valid_handle(HANDLE handle) {
  return handle != INVALID_HANDLE_VALUE && handle != nullptr;
}

[[maybe_unused]] static bool duplicate_inheritable_handle(HANDLE source, HANDLE& duplicated) {
  duplicated = INVALID_HANDLE_VALUE;

  if VUNLIKELY (!is_valid_handle(source)) {
    return false;
  }

  if VUNLIKELY (!::DuplicateHandle(::GetCurrentProcess(), source, ::GetCurrentProcess(), &duplicated, 0, TRUE,
                                   DUPLICATE_SAME_ACCESS)) {
    duplicated = INVALID_HANDLE_VALUE;
    return false;
  }

  return true;
}

[[maybe_unused]] static void append_unique_handle(std::vector<HANDLE>& handles, HANDLE handle) {
  if VUNLIKELY (!is_valid_handle(handle)) {
    return;
  }

  if (std::find(handles.begin(), handles.end(), handle) == handles.end()) {
    handles.emplace_back(handle);
  }
}

[[maybe_unused]] static Process::ExitStatus normalize_windows_exit_status(Process::ExitStatus status,
                                                                          bool forced_termination) {
  if VUNLIKELY (forced_termination || status == Process::kCrashExitStatus) {
    return Process::kCrashExitStatus;
  }

  return status;
}

struct WindowsWriteResult final {
  bool ok{false};
  bool timed_out{false};
  DWORD bytes_written{0};
  DWORD error{ERROR_SUCCESS};
};

struct WindowsWriteContext final {
  HANDLE handle{INVALID_HANDLE_VALUE};
  const void* data{nullptr};
  DWORD size{0};
  DWORD bytes_written{0};
  BOOL ok{FALSE};
  DWORD error{ERROR_SUCCESS};
};

static void run_windows_write(WindowsWriteContext* context) {
  if VUNLIKELY (context == nullptr) {
    return;
  }

  context->ok = ::WriteFile(context->handle, context->data, context->size, &context->bytes_written, nullptr);
  context->error = context->ok ? ERROR_SUCCESS : ::GetLastError();
}

[[maybe_unused]] static WindowsWriteResult write_handle_with_timeout(HANDLE handle, const void* data, DWORD size,
                                                                     int timeout_ms) {
  WindowsWriteResult result;

  if (timeout_ms < 0) {
    result.ok = ::WriteFile(handle, data, size, &result.bytes_written, nullptr) != FALSE;
    result.error = result.ok ? ERROR_SUCCESS : ::GetLastError();
    return result;
  }

  WindowsWriteContext ctx{handle, data, size};

  std::thread writer(run_windows_write, &ctx);

  auto* writer_handle = static_cast<HANDLE>(writer.native_handle());
  const DWORD wait_result = ::WaitForSingleObject(writer_handle, static_cast<DWORD>(timeout_ms));
  const DWORD wait_error = (wait_result == WAIT_FAILED) ? ::GetLastError() : ERROR_SUCCESS;

  if (wait_result == WAIT_TIMEOUT) {
    ::CancelSynchronousIo(writer_handle);
    writer.join();

    result.ok = ctx.ok != FALSE;
    result.bytes_written = ctx.bytes_written;
    result.error = result.ok ? ERROR_SUCCESS : ctx.error;
    result.timed_out = !result.ok && ctx.error == ERROR_OPERATION_ABORTED;
    return result;
  }

  writer.join();

  result.ok = ctx.ok != FALSE;
  result.bytes_written = ctx.bytes_written;

  if VLIKELY (result.ok) {
    result.error = ERROR_SUCCESS;
  } else if VUNLIKELY (wait_result == WAIT_FAILED) {
    result.error = wait_error;
  } else {
    result.error = ctx.error;
  }

  return result;
}

[[maybe_unused]] static std::wstring escape_argument(const std::wstring& arg) {
  if VUNLIKELY (arg.empty()) {
    return L"\"\"";
  }

  bool needs_quotes = false;

  for (wchar_t c : arg) {
    if VUNLIKELY (c == L' ' || c == L'\t' || c == L'\n' || c == L'\v' || c == L'"' || c == L'\\') {
      needs_quotes = true;
      break;
    }
  }

  if VLIKELY (!needs_quotes) {
    return arg;
  }

  std::wstring result = L"\"";
  size_t backslash_count = 0;

  for (wchar_t c : arg) {
    if (c == L'\\') {
      backslash_count++;
    } else if (c == L'"') {
      result.append((backslash_count * 2) + 1, L'\\');
      result += L'"';
      backslash_count = 0;
    } else {
      if (backslash_count > 0) {
        result.append(backslash_count, L'\\');
        backslash_count = 0;
      }

      result += c;
    }
  }

  if (backslash_count > 0) {
    result.append(backslash_count * 2, L'\\');
  }

  result += L'"';

  return result;
}

[[maybe_unused]] static std::wstring build_command_line(const std::wstring& program,
                                                        const std::vector<std::wstring>& arguments) {
  std::wstring cmd_line = escape_argument(program);

  for (const auto& arg : arguments) {
    cmd_line += L' ';
    cmd_line += escape_argument(arg);
  }

  return cmd_line;
}

[[maybe_unused]] static std::vector<wchar_t> build_environment_block(const Process::EnvironmentMap& env_map) {
  if (env_map.empty()) {
    return std::vector<wchar_t>{L'\0', L'\0'};
  }

  std::vector<std::pair<std::wstring, std::wstring>> sorted_env;
  sorted_env.reserve(env_map.size());

  for (const auto& [key, value] : env_map) {
    sorted_env.emplace_back(Helpers::string_to_wstring(key), Helpers::string_to_wstring(value));
  }

  std::sort(sorted_env.begin(), sorted_env.end());

  std::vector<wchar_t> env_block;

  for (const auto& [key, value] : sorted_env) {
    env_block.insert(env_block.end(), key.begin(), key.end());
    env_block.emplace_back(L'=');

    env_block.insert(env_block.end(), value.begin(), value.end());
    env_block.emplace_back(L'\0');
  }

  env_block.emplace_back(L'\0');

  return env_block;
}
#endif

#ifndef _WIN32
[[maybe_unused]] static std::string resolve_program_path(const std::string& program,
                                                         const Process::EnvironmentMap& env_map) {
  if VUNLIKELY (program.empty() || program.find('/') != std::string::npos) {
    return program;
  }

  auto iter = env_map.find("PATH");

  if (iter == env_map.end() || iter->second.empty()) {
    return program;
  }

  const std::string& path_env = iter->second;
  size_t start = 0;

  while (start <= path_env.size()) {
    size_t end = path_env.find(':', start);

    std::string directory = (end == std::string::npos) ? path_env.substr(start) : path_env.substr(start, end - start);

    // NOLINTNEXTLINE(performance-inefficient-string-concatenation)
    std::string candidate = directory.empty() ? program : directory + "/" + program;

    if (access(candidate.c_str(), X_OK) == 0) {
      return candidate;
    }

    if (end == std::string::npos) {
      break;
    }

    start = end + 1;
  }

  return program;
}

[[maybe_unused]] static ssize_t write_no_sigpipe(int fd, const void* data, size_t size) {
  sigset_t sigpipe_set;
  sigset_t old_set;
  sigset_t pending_set;

  if VUNLIKELY (sigemptyset(&sigpipe_set) != 0 || sigaddset(&sigpipe_set, SIGPIPE) != 0) {
    return -1;
  }

  int mask_error = ::pthread_sigmask(SIG_BLOCK, &sigpipe_set, &old_set);

  if VUNLIKELY (mask_error != 0) {
    errno = mask_error;
    return -1;
  }

  const bool sigpipe_blocked = sigismember(&old_set, SIGPIPE) == 1;

  if VUNLIKELY (::sigpending(&pending_set) != 0) {
    int pending_error = errno;
    int restore_error = ::pthread_sigmask(SIG_SETMASK, &old_set, nullptr);
    errno = restore_error != 0 ? restore_error : pending_error;
    return -1;
  }

  const bool sigpipe_pending = sigismember(&pending_set, SIGPIPE) == 1;
  ssize_t written = ::write(fd, data, size);
  int write_error = errno;

  if VUNLIKELY (written < 0 && write_error == EPIPE && !sigpipe_blocked && !sigpipe_pending) {
#ifdef __APPLE__
    sigset_t pending_set;
    while (true) {
      if (sigpending(&pending_set) != 0) {
        break;
      }

      if (!sigismember(&pending_set, SIGPIPE)) {
        break;
      }

      int sig = 0;
      int err = sigwait(&sigpipe_set, &sig);

      if (err == EINTR) {
        continue;
      }

      (void)sig;
      break;
    }
#else
    const struct timespec zero_timeout{0, 0};
    while (::sigtimedwait(&sigpipe_set, nullptr, &zero_timeout) == -1 && errno == EINTR) {
    }
#endif
  }

  int restore_error = ::pthread_sigmask(SIG_SETMASK, &old_set, nullptr);

  if VUNLIKELY (restore_error != 0) {
    errno = restore_error;
    return -1;
  }

  errno = write_error;
  return written;
}
#endif

[[maybe_unused]] static size_t find_newline(const std::vector<uint8_t>& buffer) {
  for (size_t i = 0; i < buffer.size(); ++i) {
    if (buffer[i] == '\n') {
      return i;
    }
  }

  return std::string::npos;
}

// Process::Impl
struct Process::Impl final {  // NOLINT(clang-analyzer-optin.performance.Padding)
  alignas(64) std::atomic<Process::State> state{Process::kNotRunningState};
  alignas(64) std::atomic<Process::ExitStatus> exit_status{Process::kNormalExitStatus};
  alignas(64) std::atomic<Process::Error> error{Process::kNoError};
  alignas(64) std::atomic<Process::Mode> mode{Process::kSeparateMode};

  std::atomic<int> exit_code{-1};
  std::atomic<bool> exit_processed{false};
  std::atomic<bool> error_reported{false};
  std::atomic<bool> stdout_closed{false};
  std::atomic<bool> stderr_closed{false};
  std::atomic<bool> inherit_environment{false};
  std::atomic<bool> is_being_destroyed{false};
  std::atomic<bool> forced_termination{false};
  std::atomic<size_t> max_buffer_size{kDefaultMaxBufferSize};
  std::atomic<bool> monitor_running{false};
  std::atomic<bool> monitor_should_stop{false};

#ifdef _WIN32
  HANDLE process{INVALID_HANDLE_VALUE};
  HANDLE thread{INVALID_HANDLE_VALUE};
  HANDLE stdin_write{INVALID_HANDLE_VALUE};
  HANDLE stdin_read{INVALID_HANDLE_VALUE};
  HANDLE stdout_write{INVALID_HANDLE_VALUE};
  HANDLE stdout_read{INVALID_HANDLE_VALUE};
  HANDLE stderr_write{INVALID_HANDLE_VALUE};
  HANDLE stderr_read{INVALID_HANDLE_VALUE};
  DWORD process_id{0};
#else
  int64_t process_id{-1};
  int stdin_pipe[2]{-1, -1};
  int stdout_pipe[2]{-1, -1};
  int stderr_pipe[2]{-1, -1};
#endif

  std::vector<uint8_t> stdout_buffer;
  std::vector<uint8_t> stderr_buffer;
  std::mutex buffer_mtx;

  std::mutex state_mtx;
  ConditionVariable state_cv;

  std::unique_ptr<std::thread> monitor_thread;

  Process::ErrorCallback error_callback;
  Process::FinishedCallback finished_callback;
  Process::ReadyReadCallback ready_read_stdout_callback;
  Process::ReadyReadCallback ready_read_stderr_callback;
  Process::StateChangedCallback state_changed_callback;
  std::shared_mutex shared_mtx;

  Process::EnvironmentMap environment_map;
  std::string working_directory;
};

// Process
Process::Process() : impl_(std::make_unique<Impl>()) {}

Process::~Process() {
  impl_->is_being_destroyed.store(true, std::memory_order_release);

  if (is_running()) {
    CLOG_W("Process: Still running (PID=%d).", static_cast<int>(impl_->process_id));

    terminate();

    read_from_pipes_with_lock();

    wait_for_finished(kDestructorWaitTimeoutMs);

    if VUNLIKELY (is_running()) {
      CLOG_E("Process: Force to kill (PID=%d).", static_cast<int>(impl_->process_id));

      kill();

      read_from_pipes_with_lock();

      wait_for_finished(1000);
    }
  }

  cleanup();
}

Process::State Process::get_state() const { return impl_->state.load(std::memory_order_acquire); }

Process::Error Process::get_error() const { return impl_->error.load(std::memory_order_acquire); }

int Process::get_exit_code() const { return impl_->exit_code.load(std::memory_order_acquire); }

Process::ExitStatus Process::get_exit_status() const { return impl_->exit_status.load(std::memory_order_acquire); }

bool Process::is_running() const { return impl_->state.load(std::memory_order_acquire) == kRunningState; }

int64_t Process::get_process_id() const {
  if (impl_->state.load(std::memory_order_acquire) != kRunningState) {
    return -1;
  }

#ifdef _WIN32
  return static_cast<int64_t>(impl_->process_id);
#else
  return impl_->process_id;
#endif
}

void Process::set_max_buffer_size(size_t size) {
  impl_->max_buffer_size.store(size > 0 ? size : kDefaultMaxBufferSize, std::memory_order_release);
}

size_t Process::get_max_buffer_size() const { return impl_->max_buffer_size.load(std::memory_order_acquire); }

void Process::set_environment(const EnvironmentMap& env_map) {
  std::lock_guard lock(impl_->shared_mtx);
  impl_->environment_map = env_map;
}

Process::EnvironmentMap Process::get_environment() const {
  std::shared_lock lock(impl_->shared_mtx);
  return impl_->environment_map;
}

void Process::set_process_mode(Mode mode) { impl_->mode.store(mode, std::memory_order_release); }

Process::Mode Process::get_process_mode() const { return impl_->mode.load(std::memory_order_acquire); }

void Process::set_inherit_environment(bool inherit) {
  impl_->inherit_environment.store(inherit, std::memory_order_release);
}

bool Process::get_inherit_environment() const { return impl_->inherit_environment.load(std::memory_order_acquire); }

void Process::set_working_directory(const std::string& dir) {
  std::lock_guard lock(impl_->shared_mtx);
  impl_->working_directory = dir;
}

std::string Process::get_working_directory() const {
  std::shared_lock lock(impl_->shared_mtx);
  return impl_->working_directory;
}

void Process::register_error_callback(ErrorCallback&& callback) {
  std::lock_guard lock(impl_->shared_mtx);
  impl_->error_callback = std::move(callback);
}

void Process::register_finished_callback(FinishedCallback&& callback) {
  std::lock_guard lock(impl_->shared_mtx);
  impl_->finished_callback = std::move(callback);
}

void Process::register_ready_read_stdout_callback(ReadyReadCallback&& callback) {
  std::lock_guard lock(impl_->shared_mtx);
  impl_->ready_read_stdout_callback = std::move(callback);
}

void Process::register_ready_read_stderr_callback(ReadyReadCallback&& callback) {
  std::lock_guard lock(impl_->shared_mtx);
  impl_->ready_read_stderr_callback = std::move(callback);
}

void Process::register_state_changed_callback(StateChangedCallback&& callback) {
  std::lock_guard lock(impl_->shared_mtx);
  impl_->state_changed_callback = std::move(callback);
}

void Process::start(const std::string& program, const std::vector<std::string>& arguments) {
  State expected = kNotRunningState;

  if VUNLIKELY (!impl_->state.compare_exchange_strong(expected, kStartingState, std::memory_order_acq_rel)) {
    set_error(kStartError);
    return;
  }

  impl_->error.store(kNoError, std::memory_order_release);
  impl_->exit_processed.store(false, std::memory_order_release);
  impl_->error_reported.store(false, std::memory_order_release);
  impl_->exit_code.store(-1, std::memory_order_release);
  impl_->exit_status.store(kNormalExitStatus, std::memory_order_release);
  impl_->stdout_closed.store(false, std::memory_order_release);
  impl_->stderr_closed.store(false, std::memory_order_release);
  impl_->forced_termination.store(false, std::memory_order_release);

  {
    std::lock_guard lock(impl_->buffer_mtx);
    impl_->stdout_buffer.clear();
    impl_->stderr_buffer.clear();
  }

  impl_->state_cv.notify_all();

  invoke_callbacks_outside_lock(kNoError, false, -1, kNormalExitStatus, kStartingState, true, false, false);

  bool pipes_ready = setup_pipes();
  bool success = false;

  if VUNLIKELY (!pipes_ready) {
    set_error(kStartError);
    impl_->state.store(kNotRunningState, std::memory_order_release);
    impl_->state_cv.notify_all();

    invoke_callbacks_outside_lock(kStartError, false, -1, kNormalExitStatus,
                                  impl_->state.load(std::memory_order_acquire), true, false, false);
    return;
  }

  success = start_program(program, arguments);

  if VLIKELY (success) {
    impl_->state.store(kRunningState, std::memory_order_release);
    impl_->state_cv.notify_all();

    invoke_callbacks_outside_lock(kNoError, false, -1, kNormalExitStatus, kRunningState, true, false, false);

    start_monitor_thread();
  } else {
    set_error(kStartError);
    impl_->state.store(kNotRunningState, std::memory_order_release);
    impl_->state_cv.notify_all();

    invoke_callbacks_outside_lock(kStartError, false, -1, kNormalExitStatus,
                                  impl_->state.load(std::memory_order_acquire), true, false, false);

    cleanup();
  }
}

void Process::start_command(const std::string& command) {
  if VUNLIKELY (command.empty()) {
    set_error(kStartError);
    return;
  }

  std::vector<std::string> parts;
  std::string current;
  bool in_dquotes = false;
  bool in_squotes = false;
  bool escape = false;

  for (auto c : command) {
    if (escape) {
      switch (c) {
        case 'n':
          current += '\n';
          break;
        case 't':
          current += '\t';
          break;
        case 'r':
          current += '\r';
          break;
        case '\\':
          current += '\\';
          break;
        case '"':
          current += '"';
          break;
        case '\'':
          current += '\'';
          break;
        case ' ':
          current += ' ';
          break;
        default:
          current += '\\';
          current += c;
          break;
      }
      escape = false;
      continue;
    }

    if (!in_squotes && c == '\\') {
      escape = true;
      continue;
    }

    if (!in_squotes && c == '"') {
      in_dquotes = !in_dquotes;
      continue;
    }

    if (!in_dquotes && c == '\'') {
      in_squotes = !in_squotes;
      continue;
    }

    if ((c == ' ' || c == '\t') && !in_dquotes && !in_squotes) {
      if (!current.empty()) {
        parts.emplace_back(current);
        current.clear();
      }
    } else {
      current += c;
    }
  }

  if (escape) {
    current += '\\';
  }

  if (!current.empty()) {
    parts.emplace_back(current);
  }

  if VUNLIKELY (parts.empty()) {
    set_error(kStartError);
    return;
  }

  std::string program = parts[0];
  std::vector<std::string> arguments(parts.begin() + 1, parts.end());

  start(program, arguments);
}

bool Process::wait_for_started(int msecs) {
  State current_state = impl_->state.load(std::memory_order_acquire);

  if VLIKELY (current_state == kRunningState) {
    return true;
  }

  if VUNLIKELY (current_state == kNotRunningState) {
    return false;
  }

  std::unique_lock lock(impl_->state_mtx);

  if (msecs < 0) {
    impl_->state_cv.wait(lock, [this]() {
      State state = impl_->state.load(std::memory_order_acquire);
      return state != kStartingState;
    });
  } else {
    auto timeout = std::chrono::milliseconds(msecs);

    if VUNLIKELY (!impl_->state_cv.wait_for(lock, timeout, [this]() {
                    State state = impl_->state.load(std::memory_order_acquire);
                    return state != kStartingState;
                  })) {
      return false;
    }
  }

  return impl_->state.load(std::memory_order_acquire) == kRunningState;
}

bool Process::wait_for_finished(int msecs) {
  if VUNLIKELY (impl_->state.load(std::memory_order_acquire) == kNotRunningState) {
    return true;
  }

#ifdef _WIN32

  if (impl_->process == INVALID_HANDLE_VALUE) {
    return true;
  }

  DWORD timeout = (msecs < 0) ? INFINITE : static_cast<DWORD>(msecs);

  DWORD result = ::WaitForSingleObject(impl_->process, timeout);

  if (result == WAIT_OBJECT_0) {
    DWORD exit_code;

    if (::GetExitCodeProcess(impl_->process, &exit_code)) {
      read_from_pipes_with_lock();
      handle_process_exit(static_cast<int>(exit_code), kNormalExitStatus);
    }

    return true;
  }

  return false;
#else
  std::unique_lock lock(impl_->state_mtx);

  if (msecs < 0) {
    impl_->state_cv.wait(lock, [this]() { return impl_->state.load(std::memory_order_acquire) == kNotRunningState; });

    return true;
  } else {
    auto timeout = std::chrono::milliseconds(msecs);

    return impl_->state_cv.wait_for(
        lock, timeout, [this]() { return impl_->state.load(std::memory_order_acquire) == kNotRunningState; });
  }
#endif
}

bool Process::wait_for_ready_read(int msecs) {
  auto start_time = std::chrono::steady_clock::now();
  size_t initial_size;

  {
    std::lock_guard lock(impl_->buffer_mtx);
    initial_size = impl_->stdout_buffer.size() + impl_->stderr_buffer.size();
  }

#ifdef _WIN32
  while (true) {
    read_from_pipes_with_lock();

    {
      std::lock_guard lock(impl_->buffer_mtx);
      size_t current_size = impl_->stdout_buffer.size() + impl_->stderr_buffer.size();

      if VLIKELY (current_size > initial_size) {
        return true;
      }
    }

    auto elapsed =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start_time).count();

    if (msecs >= 0 && elapsed >= msecs) {
      return false;
    }

    if VUNLIKELY (impl_->state.load(std::memory_order_acquire) == kNotRunningState) {
      std::lock_guard lock2(impl_->buffer_mtx);
      return (impl_->stdout_buffer.size() + impl_->stderr_buffer.size()) > initial_size;
    }

    DWORD wait_slice = 50;

    if (msecs >= 0) {
      wait_slice = static_cast<DWORD>(std::min<int64_t>(msecs - elapsed, 50));
    }

    if (impl_->process != INVALID_HANDLE_VALUE) {
      DWORD wait_result = ::WaitForSingleObject(impl_->process, wait_slice);

      if (wait_result == WAIT_OBJECT_0) {
        DWORD exit_code = 0;

        if (::GetExitCodeProcess(impl_->process, &exit_code)) {
          read_from_pipes_with_lock();
          handle_process_exit(static_cast<int>(exit_code), kNormalExitStatus);
        }
      }
    } else if (wait_slice > 0) {
      std::this_thread::sleep_for(std::chrono::milliseconds(wait_slice));
    }
  }
#else
  while (true) {
    struct pollfd fds[2];
    int nfds = 0;

    {
      if (impl_->stdout_pipe[0] >= 0 && !impl_->stdout_closed.load(std::memory_order_acquire)) {
        fds[nfds].fd = impl_->stdout_pipe[0];
        fds[nfds].events = POLLIN;
        fds[nfds].revents = 0;
        ++nfds;
      }

      if (impl_->stderr_pipe[0] >= 0 && !impl_->stderr_closed.load(std::memory_order_acquire)) {
        fds[nfds].fd = impl_->stderr_pipe[0];
        fds[nfds].events = POLLIN;
        fds[nfds].revents = 0;
        ++nfds;
      }
    }

    int timeout_ms = -1;

    if (msecs >= 0) {
      auto elapsed =
          std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start_time).count();

      if (elapsed >= msecs) {
        std::lock_guard lock(impl_->buffer_mtx);
        return (impl_->stdout_buffer.size() + impl_->stderr_buffer.size()) > initial_size;
      }

      timeout_ms = static_cast<int>(msecs - elapsed);
    }

    int pr = 0;

    if (nfds > 0) {
      pr = ::poll(fds, nfds, timeout_ms);
    } else {
      pr = 0;

      if (timeout_ms > 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(timeout_ms));
      }
    }

    (void)pr;

    read_from_pipes_with_lock();

    {
      std::lock_guard lock(impl_->buffer_mtx);
      size_t cur = impl_->stdout_buffer.size() + impl_->stderr_buffer.size();

      if (cur > initial_size) {
        return true;
      }
    }

    if VUNLIKELY (impl_->state.load(std::memory_order_acquire) == kNotRunningState) {
      return false;
    }

    if (msecs >= 0) {
      auto elapsed =
          std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start_time).count();

      if VUNLIKELY (elapsed >= msecs) {
        return false;
      }
    }
  }
#endif
}

void Process::terminate() {
#ifdef _WIN32

  if VLIKELY (impl_->process != INVALID_HANDLE_VALUE) {
    if (::WaitForSingleObject(impl_->process, 0) == WAIT_OBJECT_0) {
      DWORD exit_code_win = 0;

      if (::GetExitCodeProcess(impl_->process, &exit_code_win)) {
        read_from_pipes_with_lock();
        handle_process_exit(static_cast<int>(exit_code_win), kNormalExitStatus);
      }

      return;
    }

    if (!::TerminateProcess(impl_->process, 1)) {
      return;
    }

    impl_->forced_termination.store(true, std::memory_order_release);

    DWORD wait_ret = ::WaitForSingleObject(impl_->process, 100);

    if (wait_ret == WAIT_OBJECT_0) {
      DWORD exit_code_win;

      if (::GetExitCodeProcess(impl_->process, &exit_code_win)) {
        read_from_pipes_with_lock();
        handle_process_exit(static_cast<int>(exit_code_win), kCrashExitStatus);
      }
    }
  }
#else

  if VLIKELY (impl_->process_id > 0) {
    ::kill(impl_->process_id, SIGTERM);

    for (int i = 0; i < 20; ++i) {
      int status;
      pid_t result = waitpid(impl_->process_id, &status, WNOHANG);

      if (result == impl_->process_id) {
        int exit_code = -1;
        ExitStatus exit_status = kCrashExitStatus;

        if (WIFEXITED(status)) {
          exit_code = WEXITSTATUS(status);
          exit_status = kNormalExitStatus;
        } else if (WIFSIGNALED(status)) {
          int sig = WTERMSIG(status);
          exit_code = 128 + sig;
          exit_status = kCrashExitStatus;
        }

        read_from_pipes_with_lock();
        handle_process_exit(exit_code, exit_status);

        break;
      }

      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
  }
#endif
}

void Process::kill() {
#ifdef _WIN32

  if VLIKELY (impl_->process != INVALID_HANDLE_VALUE) {
    if (::WaitForSingleObject(impl_->process, 0) == WAIT_OBJECT_0) {
      DWORD exit_code_win = 0;

      if (::GetExitCodeProcess(impl_->process, &exit_code_win)) {
        read_from_pipes_with_lock();
        handle_process_exit(static_cast<int>(exit_code_win), kNormalExitStatus);
      }

      return;
    }

    if (!::TerminateProcess(impl_->process, 9)) {
      return;
    }

    impl_->forced_termination.store(true, std::memory_order_release);

    DWORD wait_ret = ::WaitForSingleObject(impl_->process, 100);

    if (wait_ret == WAIT_OBJECT_0) {
      DWORD exit_code_win;

      if (::GetExitCodeProcess(impl_->process, &exit_code_win)) {
        read_from_pipes_with_lock();
        handle_process_exit(static_cast<int>(exit_code_win), kCrashExitStatus);
      }
    }
  }
#else

  if VLIKELY (impl_->process_id > 0) {
    ::kill(impl_->process_id, SIGKILL);

    for (int i = 0; i < 20; ++i) {
      int status;
      pid_t result = waitpid(impl_->process_id, &status, WNOHANG);

      if (result == impl_->process_id) {
        int exit_code = -1;
        ExitStatus exit_status = kCrashExitStatus;

        if (WIFEXITED(status)) {
          exit_code = WEXITSTATUS(status);
          exit_status = kNormalExitStatus;
        } else if (WIFSIGNALED(status)) {
          int sig = WTERMSIG(status);
          exit_code = 128 + sig;
          exit_status = kCrashExitStatus;
        }

        read_from_pipes_with_lock();
        handle_process_exit(exit_code, exit_status);

        break;
      }

      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
  }
#endif
}

void Process::close(bool force_kill_on_timeout) {
  if (is_running()) {
    terminate();
    read_from_pipes_with_lock();

    bool finished = wait_for_finished(kDefaultWaitTimeoutMs);

    if VUNLIKELY (!finished && !force_kill_on_timeout) {
      return;
    }

    if VUNLIKELY (!finished && force_kill_on_timeout) {
      kill();
      read_from_pipes_with_lock();
      finished = wait_for_finished(1000);

      if VUNLIKELY (!finished) {
        return;
      }
    }
  }

  cleanup();
}

size_t Process::bytes_available_stdout() const {
  std::lock_guard lock(impl_->buffer_mtx);
  return impl_->stdout_buffer.size();
}

size_t Process::bytes_available_stderr() const {
  std::lock_guard lock(impl_->buffer_mtx);
  return impl_->stderr_buffer.size();
}

bool Process::can_read_line_stdout() const {
  std::lock_guard lock(impl_->buffer_mtx);
  return find_newline(impl_->stdout_buffer) != std::string::npos;
}

bool Process::can_read_line_stderr() const {
  std::lock_guard lock(impl_->buffer_mtx);
  return find_newline(impl_->stderr_buffer) != std::string::npos;
}

bool Process::read_line_stdout(std::string& line) {
  std::lock_guard lock(impl_->buffer_mtx);

  read_from_pipes();

  if (impl_->stdout_buffer.empty()) {
    line.clear();
    return false;
  }

  size_t newline_pos = find_newline(impl_->stdout_buffer);

  if (newline_pos != std::string::npos) {
    line.assign(reinterpret_cast<const char*>(impl_->stdout_buffer.data()), newline_pos + 1);
    impl_->stdout_buffer.erase(impl_->stdout_buffer.begin(), impl_->stdout_buffer.begin() + newline_pos + 1);
  } else {
    line.assign(reinterpret_cast<const char*>(impl_->stdout_buffer.data()), impl_->stdout_buffer.size());
    impl_->stdout_buffer.clear();
  }

  return !line.empty();
}

bool Process::read_line_stderr(std::string& line) {
  std::lock_guard lock(impl_->buffer_mtx);

  read_from_pipes();

  if (impl_->stderr_buffer.empty()) {
    line.clear();
    return false;
  }

  size_t newline_pos = find_newline(impl_->stderr_buffer);

  if (newline_pos != std::string::npos) {
    line.assign(reinterpret_cast<const char*>(impl_->stderr_buffer.data()), newline_pos + 1);
    impl_->stderr_buffer.erase(impl_->stderr_buffer.begin(), impl_->stderr_buffer.begin() + newline_pos + 1);
  } else {
    line.assign(reinterpret_cast<const char*>(impl_->stderr_buffer.data()), impl_->stderr_buffer.size());
    impl_->stderr_buffer.clear();
  }

  return !line.empty();
}

size_t Process::read_stdout(std::vector<uint8_t>& buffer, size_t max_size) {
  std::lock_guard lock(impl_->buffer_mtx);

  read_from_pipes();

  size_t to_read = std::min(max_size, impl_->stdout_buffer.size());

  if (to_read == 0) {
    buffer.clear();
    return 0;
  }

  buffer.assign(impl_->stdout_buffer.begin(), impl_->stdout_buffer.begin() + to_read);
  impl_->stdout_buffer.erase(impl_->stdout_buffer.begin(), impl_->stdout_buffer.begin() + to_read);

  return to_read;
}

size_t Process::read_stderr(std::vector<uint8_t>& buffer, size_t max_size) {
  std::lock_guard lock(impl_->buffer_mtx);

  read_from_pipes();

  size_t to_read = std::min(max_size, impl_->stderr_buffer.size());

  if (to_read == 0) {
    buffer.clear();
    return 0;
  }

  buffer.assign(impl_->stderr_buffer.begin(), impl_->stderr_buffer.begin() + to_read);
  impl_->stderr_buffer.erase(impl_->stderr_buffer.begin(), impl_->stderr_buffer.begin() + to_read);

  return to_read;
}

bool Process::read_all_output(std::vector<uint8_t>& buffer) {
  std::lock_guard lock(impl_->buffer_mtx);
  read_from_pipes();

  buffer = std::move(impl_->stdout_buffer);
  impl_->stdout_buffer.clear();

  return !buffer.empty();
}

bool Process::read_all_error(std::vector<uint8_t>& buffer) {
  std::lock_guard lock(impl_->buffer_mtx);
  read_from_pipes();

  buffer = std::move(impl_->stderr_buffer);
  impl_->stderr_buffer.clear();

  return !buffer.empty();
}

bool Process::read_all(std::vector<uint8_t>& buffer) {
  std::lock_guard lock(impl_->buffer_mtx);
  read_from_pipes();

  buffer = impl_->stdout_buffer;
  buffer.insert(buffer.end(), impl_->stderr_buffer.begin(), impl_->stderr_buffer.end());

  impl_->stdout_buffer.clear();
  impl_->stderr_buffer.clear();

  return !buffer.empty();
}

bool Process::read_all_output(std::string& str) {
  std::lock_guard lock(impl_->buffer_mtx);
  read_from_pipes();

  str.assign(reinterpret_cast<const char*>(impl_->stdout_buffer.data()), impl_->stdout_buffer.size());

  bool has_data = !impl_->stdout_buffer.empty();
  impl_->stdout_buffer.clear();

  return has_data;
}

bool Process::read_all_error(std::string& str) {
  std::lock_guard lock(impl_->buffer_mtx);
  read_from_pipes();

  str.assign(reinterpret_cast<const char*>(impl_->stderr_buffer.data()), impl_->stderr_buffer.size());

  bool has_data = !impl_->stderr_buffer.empty();
  impl_->stderr_buffer.clear();

  return has_data;
}

bool Process::read_all(std::string& str) {
  std::lock_guard lock(impl_->buffer_mtx);
  read_from_pipes();

  str.clear();
  str.reserve(impl_->stdout_buffer.size() + impl_->stderr_buffer.size());

  str.append(reinterpret_cast<const char*>(impl_->stdout_buffer.data()), impl_->stdout_buffer.size());
  str.append(reinterpret_cast<const char*>(impl_->stderr_buffer.data()), impl_->stderr_buffer.size());

  bool has_data = !str.empty();

  impl_->stdout_buffer.clear();
  impl_->stderr_buffer.clear();

  return has_data;
}

size_t Process::write(const std::vector<uint8_t>& buffer, int timeout_ms) {
  if VUNLIKELY (impl_->state.load(std::memory_order_acquire) != kRunningState) {
    return 0;
  }

  if VUNLIKELY (buffer.empty()) {
    return 0;
  }

#ifdef _WIN32
  auto start_time = std::chrono::steady_clock::now();
  size_t total = 0;
  const uint8_t* data = buffer.data();
  size_t left = buffer.size();

  while (left > 0) {
    int remaining_timeout = timeout_ms;

    if (timeout_ms >= 0) {
      auto elapsed =
          std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start_time).count();

      if VUNLIKELY (elapsed >= timeout_ms) {
        set_error(kTimedOutError);
        invoke_callbacks_outside_lock(kTimedOutError, false, -1, kNormalExitStatus,
                                      impl_->state.load(std::memory_order_acquire), false, false, false);
        break;
      }

      remaining_timeout = static_cast<int>(timeout_ms - elapsed);
    }

    const DWORD chunk_size =
        static_cast<DWORD>(std::min<size_t>(left, static_cast<size_t>(std::numeric_limits<DWORD>::max())));
    WindowsWriteResult write_result =
        write_handle_with_timeout(impl_->stdin_write, data, chunk_size, remaining_timeout);

    if VLIKELY (write_result.ok && write_result.bytes_written > 0) {
      total += static_cast<size_t>(write_result.bytes_written);
      data += write_result.bytes_written;
      left -= static_cast<size_t>(write_result.bytes_written);
      continue;
    }

    set_error(write_result.timed_out ? kTimedOutError : kWriteError);

    invoke_callbacks_outside_lock(write_result.timed_out ? kTimedOutError : kWriteError, false, -1, kNormalExitStatus,
                                  impl_->state.load(std::memory_order_acquire), false, false, false);
    break;
  }

  return total;
#else
  auto start_time = std::chrono::steady_clock::now();
  size_t total = 0;
  const uint8_t* data = buffer.data();
  size_t left = buffer.size();

  while (left > 0) {
    if (timeout_ms >= 0) {
      auto elapsed =
          std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start_time).count();

      if (elapsed >= timeout_ms) {
        set_error(kTimedOutError);
        invoke_callbacks_outside_lock(kTimedOutError, false, -1, kNormalExitStatus,
                                      impl_->state.load(std::memory_order_acquire), false, false, false);
        break;
      }
    }

    ssize_t written = write_no_sigpipe(impl_->stdin_pipe[1], data, left);

    if VLIKELY (written > 0) {
      total += static_cast<size_t>(written);
      data += written;
      left -= static_cast<size_t>(written);

      continue;
    }

    if VUNLIKELY (written < 0) {
      if VLIKELY (errno == EINTR) {
        continue;
      } else if (errno == EAGAIN || errno == EWOULDBLOCK) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));

        continue;
      } else {
        set_error(kWriteError);
        invoke_callbacks_outside_lock(kWriteError, false, -1, kNormalExitStatus,
                                      impl_->state.load(std::memory_order_acquire), false, false, false);
        break;
      }
    }
  }

  return total;
#endif
}

size_t Process::write(const std::string& str, int timeout_ms) {
  if VUNLIKELY (impl_->state.load(std::memory_order_acquire) != kRunningState) {
    return 0;
  }

  if VUNLIKELY (str.empty()) {
    return 0;
  }

#ifdef _WIN32
  auto start_time = std::chrono::steady_clock::now();
  size_t total = 0;
  const char* data = str.data();
  size_t left = str.size();

  while (left > 0) {
    int remaining_timeout = timeout_ms;

    if (timeout_ms >= 0) {
      auto elapsed =
          std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start_time).count();

      if VUNLIKELY (elapsed >= timeout_ms) {
        set_error(kTimedOutError);
        invoke_callbacks_outside_lock(kTimedOutError, false, -1, kNormalExitStatus,
                                      impl_->state.load(std::memory_order_acquire), false, false, false);
        break;
      }

      remaining_timeout = static_cast<int>(timeout_ms - elapsed);
    }

    const DWORD chunk_size =
        static_cast<DWORD>(std::min<size_t>(left, static_cast<size_t>(std::numeric_limits<DWORD>::max())));
    WindowsWriteResult write_result =
        write_handle_with_timeout(impl_->stdin_write, data, chunk_size, remaining_timeout);

    if VLIKELY (write_result.ok && write_result.bytes_written > 0) {
      total += static_cast<size_t>(write_result.bytes_written);
      data += write_result.bytes_written;
      left -= static_cast<size_t>(write_result.bytes_written);
      continue;
    }

    set_error(write_result.timed_out ? kTimedOutError : kWriteError);

    invoke_callbacks_outside_lock(write_result.timed_out ? kTimedOutError : kWriteError, false, -1, kNormalExitStatus,
                                  impl_->state.load(std::memory_order_acquire), false, false, false);

    break;
  }

  return total;
#else
  auto start_time = std::chrono::steady_clock::now();
  size_t total = 0;
  const char* data = str.data();
  size_t left = str.size();

  while (left > 0) {
    if (timeout_ms >= 0) {
      auto elapsed =
          std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start_time).count();

      if (elapsed >= timeout_ms) {
        set_error(kTimedOutError);
        invoke_callbacks_outside_lock(kTimedOutError, false, -1, kNormalExitStatus,
                                      impl_->state.load(std::memory_order_acquire), false, false, false);

        break;
      }
    }

    ssize_t written = write_no_sigpipe(impl_->stdin_pipe[1], data, left);

    if VLIKELY (written > 0) {
      total += static_cast<size_t>(written);
      data += written;
      left -= static_cast<size_t>(written);

      continue;
    }

    if VUNLIKELY (written < 0) {
      if VLIKELY (errno == EINTR) {
        continue;
      } else if (errno == EAGAIN || errno == EWOULDBLOCK) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));

        continue;
      } else {
        set_error(kWriteError);
        invoke_callbacks_outside_lock(kWriteError, false, -1, kNormalExitStatus,
                                      impl_->state.load(std::memory_order_acquire), false, false, false);

        break;
      }
    }
  }

  return total;
#endif
}

void Process::close_write_channel() {
#ifdef _WIN32

  if VLIKELY (impl_->stdin_write != INVALID_HANDLE_VALUE) {
    close_handle_if_valid(impl_->stdin_write);
  }
#else

  if VLIKELY (impl_->stdin_pipe[1] >= 0) {
    ::close(impl_->stdin_pipe[1]);
    impl_->stdin_pipe[1] = -1;
  }
#endif
}

int Process::execute(const std::string& program, const std::vector<std::string>& arguments, int timeout_ms) {
  Process process;
  process.start(program, arguments);

  if VUNLIKELY (!process.wait_for_started(5000)) {
    return -1;
  }

  if VUNLIKELY (!process.wait_for_finished(timeout_ms)) {
    process.terminate();
    process.wait_for_finished(1000);

    return -1;
  }

  return process.get_exit_code();
}

bool Process::start_detached(const std::string& program, const std::vector<std::string>& arguments) {
#ifdef _WIN32
  STARTUPINFOW si;
  PROCESS_INFORMATION pi;

  ::ZeroMemory(&si, sizeof(si));
  si.cb = sizeof(si);
  ::ZeroMemory(&pi, sizeof(pi));

  std::wstring program_wide = Helpers::string_to_wstring(program);
  std::vector<std::wstring> arguments_wide;
  arguments_wide.reserve(arguments.size());

  for (const auto& arg : arguments) {
    arguments_wide.emplace_back(Helpers::string_to_wstring(arg));
  }

  std::wstring cmd_line = build_command_line(program_wide, arguments_wide);
  std::vector<wchar_t> cmd_line_buf(cmd_line.begin(), cmd_line.end());
  cmd_line_buf.emplace_back(L'\0');

  BOOL result = ::CreateProcessW(nullptr, cmd_line_buf.data(), nullptr, nullptr, FALSE,
                                 DETACHED_PROCESS | CREATE_NEW_PROCESS_GROUP | CREATE_UNICODE_ENVIRONMENT, nullptr,
                                 nullptr, &si, &pi);

  if VLIKELY (result) {
    ::CloseHandle(pi.hProcess);
    ::CloseHandle(pi.hThread);

    return true;
  }

  return false;
#else
  int exec_status_pipe[2];

  ssize_t written;

  if (pipe(exec_status_pipe) != 0) {
    return false;
  }

  fcntl(exec_status_pipe[1], F_SETFD, FD_CLOEXEC);

  pid_t pid = fork();

  if VUNLIKELY (pid < 0) {
    ::close(exec_status_pipe[0]);
    ::close(exec_status_pipe[1]);

    return false;
  }

  if VUNLIKELY (pid == 0) {
    ::close(exec_status_pipe[0]);

    pid_t pid2 = fork();

    if VUNLIKELY (pid2 < 0) {
      int error_code = errno;

      written = ::write(exec_status_pipe[1], &error_code, sizeof(error_code));

      (void)written;

      ::close(exec_status_pipe[1]);

      _exit(kExecFailedExitCode);
    }

    if VLIKELY (pid2 > 0) {
      ::close(exec_status_pipe[1]);
      _exit(0);
    }

    setsid();

    int devnull = open("/dev/null", O_RDWR);

    if VLIKELY (devnull >= 0) {
      dup2(devnull, STDIN_FILENO);
      dup2(devnull, STDOUT_FILENO);
      dup2(devnull, STDERR_FILENO);

      if (devnull > STDERR_FILENO) {
        ::close(devnull);
      }
    }

    int max_fd = static_cast<int>(sysconf(_SC_OPEN_MAX));

    if VUNLIKELY (max_fd < 0) {
      max_fd = 1024;
    }

    for (int fd = STDERR_FILENO + 1; fd < max_fd; ++fd) {
      if (fd != exec_status_pipe[1]) {
        ::close(fd);
      }
    }

    std::vector<std::string> args_storage;
    args_storage.reserve(arguments.size() + 2);
    args_storage.emplace_back(program);
    args_storage.insert(args_storage.end(), arguments.begin(), arguments.end());

    std::vector<char*> args;
    args.reserve(args_storage.size() + 1);

    for (auto& arg : args_storage) {
      args.emplace_back(const_cast<char*>(arg.c_str()));
    }

    args.emplace_back(nullptr);

    execvp(program.c_str(), args.data());

    int error_code = errno;

    written = ::write(exec_status_pipe[1], &error_code, sizeof(error_code));

    (void)written;

    ::close(exec_status_pipe[1]);

    _exit(kExecFailedExitCode);
  }

  ::close(exec_status_pipe[1]);

  int status = 0;
  pid_t result;

  do {
    result = waitpid(pid, &status, 0);
  } while (result < 0 && errno == EINTR);

  if VUNLIKELY (result < 0) {
    ::close(exec_status_pipe[0]);
    return false;
  }

  if VUNLIKELY (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
    ::close(exec_status_pipe[0]);
    return false;
  }

  int error_code = 0;
  ssize_t n = 0;

  do {
    n = ::read(exec_status_pipe[0], &error_code, sizeof(error_code));
  } while (n < 0 && errno == EINTR);

  ::close(exec_status_pipe[0]);

  if (n > 0) {
    errno = error_code;
    return false;
  }

  return true;
#endif
}

void Process::set_state(State state) {
  State old_state = impl_->state.exchange(state, std::memory_order_acq_rel);

  if (old_state != state) {
    impl_->state_cv.notify_all();
  }
}

void Process::set_error(Error error) { impl_->error.store(error, std::memory_order_release); }

void Process::cleanup() {
  stop_monitor_thread();

#ifdef _WIN32

  if (impl_->process != INVALID_HANDLE_VALUE && !impl_->exit_processed.load(std::memory_order_acquire)) {
    DWORD wait_ret = ::WaitForSingleObject(impl_->process, 0);

    if (wait_ret == WAIT_OBJECT_0) {
      DWORD exit_code_win;

      if (::GetExitCodeProcess(impl_->process, &exit_code_win)) {
        read_from_pipes_with_lock();
        handle_process_exit(static_cast<int>(exit_code_win), kNormalExitStatus);
      }
    }
  }

  if (impl_->process != INVALID_HANDLE_VALUE) {
    ::CloseHandle(impl_->process);
    impl_->process = INVALID_HANDLE_VALUE;
  }

  if (impl_->thread != INVALID_HANDLE_VALUE) {
    ::CloseHandle(impl_->thread);
    impl_->thread = INVALID_HANDLE_VALUE;
  }

  close_handle_if_valid(impl_->stdin_write);
  close_handle_if_valid(impl_->stdin_read);
  close_handle_if_valid(impl_->stdout_read);

  const HANDLE stdout_write = impl_->stdout_write;
  close_handle_if_valid(impl_->stdout_write);

  close_handle_if_valid(impl_->stderr_read);

  if (impl_->stderr_write == stdout_write) {
    impl_->stderr_write = INVALID_HANDLE_VALUE;
  }

  close_handle_if_valid(impl_->stderr_write);

  impl_->process_id = 0;
#else

  if (impl_->process_id > 0 && !impl_->exit_processed.load(std::memory_order_acquire)) {
    int status;
    pid_t r;

    do {
      r = waitpid(impl_->process_id, &status, WNOHANG);
    } while (r < 0 && errno == EINTR);
  }

  if (impl_->stdin_pipe[0] >= 0) {
    ::close(impl_->stdin_pipe[0]);
    impl_->stdin_pipe[0] = -1;
  }

  if (impl_->stdin_pipe[1] >= 0) {
    ::close(impl_->stdin_pipe[1]);
    impl_->stdin_pipe[1] = -1;
  }

  if (impl_->stdout_pipe[0] >= 0) {
    ::close(impl_->stdout_pipe[0]);
    impl_->stdout_pipe[0] = -1;
  }

  if (impl_->stdout_pipe[1] >= 0) {
    const int stdout_write = impl_->stdout_pipe[1];
    ::close(impl_->stdout_pipe[1]);
    impl_->stdout_pipe[1] = -1;

    if (impl_->stderr_pipe[1] == stdout_write) {
      impl_->stderr_pipe[1] = -1;
    }
  }

  if (impl_->stderr_pipe[0] >= 0) {
    ::close(impl_->stderr_pipe[0]);
    impl_->stderr_pipe[0] = -1;
  }

  if (impl_->stderr_pipe[1] >= 0) {
    ::close(impl_->stderr_pipe[1]);
    impl_->stderr_pipe[1] = -1;
  }

  impl_->process_id = -1;
#endif

  impl_->exit_processed.store(false, std::memory_order_release);
  impl_->error_reported.store(false, std::memory_order_release);
  impl_->stdout_closed.store(false, std::memory_order_release);
  impl_->stderr_closed.store(false, std::memory_order_release);
}

Process::ReadResult Process::read_from_pipes() {
  ReadResult result;
  size_t max_buffer_size = impl_->max_buffer_size.load(std::memory_order_acquire);

  auto append_to_buffer = [&result, &max_buffer_size](std::vector<uint8_t>& target, const char* data, size_t bytes,
                                                      bool& has_data) {
    if VUNLIKELY (target.size() >= max_buffer_size) {
      if (bytes > 0U) {
        result.truncated = true;
      }

      return;
    }

    const size_t remaining = max_buffer_size - target.size();
    const size_t bytes_to_store = std::min(remaining, bytes);

    if (bytes_to_store > 0U) {
      target.insert(target.end(), data, data + bytes_to_store);
      has_data = true;
    }

    if VUNLIKELY (bytes_to_store < bytes) {
      result.truncated = true;
    }
  };

  if (impl_->stdout_buffer.capacity() < max_buffer_size) {
    impl_->stdout_buffer.reserve(std::min(max_buffer_size, impl_->stdout_buffer.size() + 8192));
  }

  if (impl_->stderr_buffer.capacity() < max_buffer_size) {
    impl_->stderr_buffer.reserve(std::min(max_buffer_size, impl_->stderr_buffer.size() + 8192));
  }

#ifdef _WIN32
  char buffer[8192];
  auto drain_pipe = [&](HANDLE handle, std::vector<uint8_t>& target, std::atomic<bool>& closed, bool& has_data) {
    while (handle != INVALID_HANDLE_VALUE && !closed.load(std::memory_order_acquire)) {
      DWORD available = 0;

      if (!::PeekNamedPipe(handle, nullptr, 0, nullptr, &available, nullptr)) {
        DWORD error = ::GetLastError();

        if (error == ERROR_BROKEN_PIPE || error == ERROR_HANDLE_EOF) {
          closed.store(true, std::memory_order_release);
        } else {
          set_error(kReadError);
          result.read_error = true;
        }

        break;
      }

      if (available == 0) {
        break;
      }

      const DWORD to_read = static_cast<DWORD>(std::min<size_t>(sizeof(buffer), static_cast<size_t>(available)));
      DWORD bytes_read = 0;

      if (!::ReadFile(handle, buffer, to_read, &bytes_read, nullptr)) {
        DWORD error = ::GetLastError();

        if (error == ERROR_BROKEN_PIPE || error == ERROR_HANDLE_EOF) {
          closed.store(true, std::memory_order_release);
        } else {
          set_error(kReadError);
          result.read_error = true;
        }

        break;
      }

      if (bytes_read == 0) {
        closed.store(true, std::memory_order_release);
        break;
      }

      append_to_buffer(target, buffer, bytes_read, has_data);
    }
  };

  drain_pipe(impl_->stdout_read, impl_->stdout_buffer, impl_->stdout_closed, result.has_stdout_data);
  drain_pipe(impl_->stderr_read, impl_->stderr_buffer, impl_->stderr_closed, result.has_stderr_data);
#else
  char buffer[8192];
  ssize_t bytes_read;

  if (!impl_->stdout_closed.load(std::memory_order_acquire)) {
    while (true) {
      bytes_read =
          ::read(impl_->stdout_pipe[0], buffer, sizeof(buffer));  // NOLINT(clang-analyzer-unix.BlockInCriticalSection)

      if VLIKELY (bytes_read > 0) {
        append_to_buffer(impl_->stdout_buffer, buffer, static_cast<size_t>(bytes_read), result.has_stdout_data);
      } else if (bytes_read == 0) {
        impl_->stdout_closed.store(true, std::memory_order_release);
        break;
      } else {
        if VLIKELY (errno == EAGAIN || errno == EWOULDBLOCK) {
          break;
        } else if VLIKELY (errno == EINTR) {
          continue;
        } else {
          set_error(kReadError);
          result.read_error = true;
          break;
        }
      }
    }
  }

  if (!impl_->stderr_closed.load(std::memory_order_acquire)) {
    while (true) {
      bytes_read =
          ::read(impl_->stderr_pipe[0], buffer, sizeof(buffer));  // NOLINT(clang-analyzer-unix.BlockInCriticalSection)

      if VLIKELY (bytes_read > 0) {
        append_to_buffer(impl_->stderr_buffer, buffer, static_cast<size_t>(bytes_read), result.has_stderr_data);
      } else if (bytes_read == 0) {
        impl_->stderr_closed.store(true, std::memory_order_release);
        break;
      } else {
        if VLIKELY (errno == EAGAIN || errno == EWOULDBLOCK) {
          break;
        } else if VLIKELY (errno == EINTR) {
          continue;
        } else {
          set_error(kReadError);
          result.read_error = true;
          break;
        }
      }
    }
  }
#endif

  return result;
}

void Process::report_read_result(const ReadResult& result) {
  if VLIKELY (result.has_stdout_data || result.has_stderr_data) {
    invoke_callbacks_outside_lock(kNoError, false, -1, kNormalExitStatus, impl_->state.load(std::memory_order_acquire),
                                  false, result.has_stdout_data, result.has_stderr_data);
  }

  if VUNLIKELY (result.read_error) {
    invoke_callbacks_outside_lock(kReadError, false, -1, kNormalExitStatus,
                                  impl_->state.load(std::memory_order_acquire), false, false, false);
  }

  if VUNLIKELY (result.truncated && !impl_->error_reported.exchange(true, std::memory_order_acq_rel)) {
    set_error(kBufferOverflowError);
    invoke_callbacks_outside_lock(kBufferOverflowError, false, -1, kNormalExitStatus,
                                  impl_->state.load(std::memory_order_acquire), false, false, false);
  }
}

void Process::read_from_pipes_with_lock() {
  ReadResult result;

  {
    std::lock_guard lock(impl_->buffer_mtx);
    result = read_from_pipes();
  }

  report_read_result(result);
}

bool Process::setup_pipes() {
  Mode mode = impl_->mode.load(std::memory_order_acquire);

  if (mode == kForwardedMode || mode == kForwardedOutputMode || mode == kForwardedErrorMode) {
#ifdef _WIN32
    SECURITY_ATTRIBUTES sa;
    sa.nLength = sizeof(SECURITY_ATTRIBUTES);
    sa.bInheritHandle = TRUE;
    sa.lpSecurityDescriptor = nullptr;

    if VUNLIKELY (!::CreatePipe(&impl_->stdin_read, &impl_->stdin_write, &sa, 0)) {
      set_error(kStartError);
      return false;
    }

    if VUNLIKELY (!::SetHandleInformation(impl_->stdin_write, HANDLE_FLAG_INHERIT, 0) ||
                  !::SetHandleInformation(impl_->stdin_read, HANDLE_FLAG_INHERIT, 0)) {
      close_handle_if_valid(impl_->stdin_read);
      close_handle_if_valid(impl_->stdin_write);

      set_error(kStartError);

      return false;
    }

    auto close_forwarded_handles = [this]() {
      close_handle_if_valid(impl_->stdin_read);
      close_handle_if_valid(impl_->stdin_write);
      close_handle_if_valid(impl_->stdout_read);
      close_handle_if_valid(impl_->stdout_write);
      close_handle_if_valid(impl_->stderr_read);
      close_handle_if_valid(impl_->stderr_write);
    };

    bool capture_stdout = mode != kForwardedMode && mode != kForwardedOutputMode;
    bool capture_stderr = mode != kForwardedMode && mode != kForwardedErrorMode;

    if (capture_stdout) {
      if VUNLIKELY (!::CreatePipe(&impl_->stdout_read, &impl_->stdout_write, &sa, 0)) {
        close_forwarded_handles();
        set_error(kStartError);
        return false;
      }

      if VUNLIKELY (!::SetHandleInformation(impl_->stdout_read, HANDLE_FLAG_INHERIT, 0) ||
                    !::SetHandleInformation(impl_->stdout_write, HANDLE_FLAG_INHERIT, 0)) {
        close_forwarded_handles();
        set_error(kStartError);
        return false;
      }
    } else {
      impl_->stdout_closed.store(true, std::memory_order_release);
    }

    if (capture_stderr) {
      if VUNLIKELY (!::CreatePipe(&impl_->stderr_read, &impl_->stderr_write, &sa, 0)) {
        close_forwarded_handles();
        set_error(kStartError);
        return false;
      }

      if VUNLIKELY (!::SetHandleInformation(impl_->stderr_read, HANDLE_FLAG_INHERIT, 0) ||
                    !::SetHandleInformation(impl_->stderr_write, HANDLE_FLAG_INHERIT, 0)) {
        close_forwarded_handles();
        set_error(kStartError);
        return false;
      }
    } else {
      impl_->stderr_closed.store(true, std::memory_order_release);
    }
#else

    if VUNLIKELY (pipe(impl_->stdin_pipe) != 0) {
      set_error(kStartError);
      return false;
    }

    fcntl(impl_->stdin_pipe[1], F_SETFL, O_NONBLOCK);

    bool capture_stdout = mode != kForwardedMode && mode != kForwardedOutputMode;
    bool capture_stderr = mode != kForwardedMode && mode != kForwardedErrorMode;

    if (capture_stdout) {
      if VUNLIKELY (pipe(impl_->stdout_pipe) != 0) {
        ::close(impl_->stdin_pipe[0]);
        ::close(impl_->stdin_pipe[1]);
        impl_->stdin_pipe[0] = -1;
        impl_->stdin_pipe[1] = -1;
        set_error(kStartError);
        return false;
      }

      fcntl(impl_->stdout_pipe[0], F_SETFL, O_NONBLOCK);
    } else {
      impl_->stdout_closed.store(true, std::memory_order_release);
    }

    if (capture_stderr) {
      if VUNLIKELY (pipe(impl_->stderr_pipe) != 0) {
        ::close(impl_->stdin_pipe[0]);
        ::close(impl_->stdin_pipe[1]);
        impl_->stdin_pipe[0] = -1;
        impl_->stdin_pipe[1] = -1;

        if (impl_->stdout_pipe[0] >= 0) {
          ::close(impl_->stdout_pipe[0]);
          impl_->stdout_pipe[0] = -1;
        }

        if (impl_->stdout_pipe[1] >= 0) {
          ::close(impl_->stdout_pipe[1]);
          impl_->stdout_pipe[1] = -1;
        }

        set_error(kStartError);
        return false;
      }

      fcntl(impl_->stderr_pipe[0], F_SETFL, O_NONBLOCK);
    } else {
      impl_->stderr_closed.store(true, std::memory_order_release);
    }
#endif

    return true;
  }

#ifdef _WIN32
  SECURITY_ATTRIBUTES sa;
  sa.nLength = sizeof(SECURITY_ATTRIBUTES);
  sa.bInheritHandle = TRUE;
  sa.lpSecurityDescriptor = nullptr;

  if VUNLIKELY (!::CreatePipe(&impl_->stdin_read, &impl_->stdin_write, &sa, 0)) {
    set_error(kStartError);

    return false;
  }

  if VUNLIKELY (!::SetHandleInformation(impl_->stdin_write, HANDLE_FLAG_INHERIT, 0) ||
                !::SetHandleInformation(impl_->stdin_read, HANDLE_FLAG_INHERIT, 0)) {
    close_handle_if_valid(impl_->stdin_read);
    close_handle_if_valid(impl_->stdin_write);

    set_error(kStartError);

    return false;
  }

  if VUNLIKELY (!::CreatePipe(&impl_->stdout_read, &impl_->stdout_write, &sa, 0)) {
    close_handle_if_valid(impl_->stdin_read);
    close_handle_if_valid(impl_->stdin_write);

    set_error(kStartError);

    return false;
  }

  if VUNLIKELY (!::SetHandleInformation(impl_->stdout_read, HANDLE_FLAG_INHERIT, 0) ||
                !::SetHandleInformation(impl_->stdout_write, HANDLE_FLAG_INHERIT, 0)) {
    close_handle_if_valid(impl_->stdin_read);
    close_handle_if_valid(impl_->stdin_write);
    close_handle_if_valid(impl_->stdout_read);
    close_handle_if_valid(impl_->stdout_write);

    set_error(kStartError);

    return false;
  }

  if (mode == kMergedMode) {
    impl_->stderr_write = impl_->stdout_write;
    impl_->stderr_read = INVALID_HANDLE_VALUE;
  } else {
    if VUNLIKELY (!::CreatePipe(&impl_->stderr_read, &impl_->stderr_write, &sa, 0)) {
      close_handle_if_valid(impl_->stdin_read);
      close_handle_if_valid(impl_->stdin_write);
      close_handle_if_valid(impl_->stdout_read);
      close_handle_if_valid(impl_->stdout_write);

      set_error(kStartError);

      return false;
    }

    if VUNLIKELY (!::SetHandleInformation(impl_->stderr_read, HANDLE_FLAG_INHERIT, 0) ||
                  !::SetHandleInformation(impl_->stderr_write, HANDLE_FLAG_INHERIT, 0)) {
      close_handle_if_valid(impl_->stdin_read);
      close_handle_if_valid(impl_->stdin_write);
      close_handle_if_valid(impl_->stdout_read);
      close_handle_if_valid(impl_->stdout_write);
      close_handle_if_valid(impl_->stderr_read);
      close_handle_if_valid(impl_->stderr_write);

      set_error(kStartError);

      return false;
    }
  }

  return true;
#else

  if VUNLIKELY (pipe(impl_->stdin_pipe) != 0) {
    set_error(kStartError);

    return false;
  }

  if VUNLIKELY (pipe(impl_->stdout_pipe) != 0) {
    ::close(impl_->stdin_pipe[0]);
    ::close(impl_->stdin_pipe[1]);

    impl_->stdin_pipe[0] = -1;
    impl_->stdin_pipe[1] = -1;

    set_error(kStartError);

    return false;
  }

  if (mode == kMergedMode) {
    impl_->stderr_pipe[0] = -1;
    impl_->stderr_pipe[1] = impl_->stdout_pipe[1];
    impl_->stderr_closed.store(true, std::memory_order_release);
  } else {
    if VUNLIKELY (pipe(impl_->stderr_pipe) != 0) {
      ::close(impl_->stdin_pipe[0]);
      ::close(impl_->stdin_pipe[1]);
      ::close(impl_->stdout_pipe[0]);
      ::close(impl_->stdout_pipe[1]);

      impl_->stdin_pipe[0] = -1;
      impl_->stdin_pipe[1] = -1;
      impl_->stdout_pipe[0] = -1;
      impl_->stdout_pipe[1] = -1;

      set_error(kStartError);

      return false;
    }
    fcntl(impl_->stderr_pipe[0], F_SETFL, O_NONBLOCK);
  }

  fcntl(impl_->stdout_pipe[0], F_SETFL, O_NONBLOCK);
  fcntl(impl_->stdin_pipe[1], F_SETFL, O_NONBLOCK);

  return true;
#endif
}

bool Process::start_program(const std::string& program, const std::vector<std::string>& arguments) {
#ifdef _WIN32
  STARTUPINFOEXW si;
  PROCESS_INFORMATION pi;
  HANDLE child_stdin = INVALID_HANDLE_VALUE;
  HANDLE child_stdout = INVALID_HANDLE_VALUE;
  HANDLE child_stderr = INVALID_HANDLE_VALUE;
  SIZE_T attr_list_size = 0;

  ::ZeroMemory(&si, sizeof(si));
  si.StartupInfo.cb = sizeof(si);

  Mode mode = impl_->mode.load(std::memory_order_acquire);

  if VUNLIKELY (!duplicate_inheritable_handle(impl_->stdin_read, child_stdin)) {
    return false;
  }

  if (mode == kForwardedMode) {
    if VUNLIKELY (!duplicate_inheritable_handle(::GetStdHandle(STD_OUTPUT_HANDLE), child_stdout) ||
                  !duplicate_inheritable_handle(::GetStdHandle(STD_ERROR_HANDLE), child_stderr)) {
      close_handle_if_valid(child_stdin);
      close_handle_if_valid(child_stdout);
      close_handle_if_valid(child_stderr);
      return false;
    }
  } else if (mode == kForwardedOutputMode) {
    if VUNLIKELY (!duplicate_inheritable_handle(::GetStdHandle(STD_OUTPUT_HANDLE), child_stdout) ||
                  !duplicate_inheritable_handle(impl_->stderr_write, child_stderr)) {
      close_handle_if_valid(child_stdin);
      close_handle_if_valid(child_stdout);
      close_handle_if_valid(child_stderr);
      return false;
    }
  } else if (mode == kForwardedErrorMode) {
    if VUNLIKELY (!duplicate_inheritable_handle(impl_->stdout_write, child_stdout) ||
                  !duplicate_inheritable_handle(::GetStdHandle(STD_ERROR_HANDLE), child_stderr)) {
      close_handle_if_valid(child_stdin);
      close_handle_if_valid(child_stdout);
      close_handle_if_valid(child_stderr);
      return false;
    }
  } else if (mode == kMergedMode) {
    if VUNLIKELY (!duplicate_inheritable_handle(impl_->stdout_write, child_stdout)) {
      close_handle_if_valid(child_stdin);
      return false;
    }

    child_stderr = child_stdout;
  } else {
    if VUNLIKELY (!duplicate_inheritable_handle(impl_->stdout_write, child_stdout) ||
                  !duplicate_inheritable_handle(impl_->stderr_write, child_stderr)) {
      close_handle_if_valid(child_stdin);
      close_handle_if_valid(child_stdout);
      close_handle_if_valid(child_stderr);
      return false;
    }
  }

  si.StartupInfo.hStdInput = child_stdin;
  si.StartupInfo.hStdOutput = child_stdout;
  si.StartupInfo.hStdError = child_stderr;
  si.StartupInfo.dwFlags |= STARTF_USESTDHANDLES;

  std::vector<HANDLE> inherit_handles;
  inherit_handles.reserve(3);
  append_unique_handle(inherit_handles, child_stdin);
  append_unique_handle(inherit_handles, child_stdout);
  append_unique_handle(inherit_handles, child_stderr);
  const SIZE_T inherit_handles_bytes = inherit_handles.size() * sizeof(HANDLE);
  void* inherit_handles_ptr = inherit_handles.empty() ? nullptr : static_cast<void*>(inherit_handles.data());

  (void)::InitializeProcThreadAttributeList(nullptr, 1, 0, &attr_list_size);

  std::vector<uint8_t> attr_list_storage(attr_list_size);
  si.lpAttributeList = reinterpret_cast<PPROC_THREAD_ATTRIBUTE_LIST>(attr_list_storage.data());

  if VUNLIKELY (!::InitializeProcThreadAttributeList(si.lpAttributeList, 1, 0, &attr_list_size)) {
    if (child_stderr == child_stdout) {
      child_stderr = INVALID_HANDLE_VALUE;
    }

    close_handle_if_valid(child_stdin);
    close_handle_if_valid(child_stdout);
    close_handle_if_valid(child_stderr);
    return false;
  }

  if VUNLIKELY (!::UpdateProcThreadAttribute(si.lpAttributeList, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                                             inherit_handles_ptr, inherit_handles_bytes, nullptr, nullptr)) {
    ::DeleteProcThreadAttributeList(si.lpAttributeList);

    if (child_stderr == child_stdout) {
      child_stderr = INVALID_HANDLE_VALUE;
    }

    close_handle_if_valid(child_stdin);
    close_handle_if_valid(child_stdout);
    close_handle_if_valid(child_stderr);
    return false;
  }

  ::ZeroMemory(&pi, sizeof(pi));

  std::wstring program_wide = Helpers::string_to_wstring(program);
  std::vector<std::wstring> arguments_wide;
  arguments_wide.reserve(arguments.size());

  for (const auto& arg : arguments) {
    arguments_wide.emplace_back(Helpers::string_to_wstring(arg));
  }

  std::wstring cmd_line = build_command_line(program_wide, arguments_wide);
  std::vector<wchar_t> cmd_line_buf(cmd_line.begin(), cmd_line.end());
  cmd_line_buf.emplace_back(L'\0');

  std::vector<wchar_t> env_block;

  {
    std::shared_lock lock(impl_->shared_mtx);

    if (impl_->inherit_environment.load(std::memory_order_acquire)) {
      wchar_t* parent_env = ::GetEnvironmentStringsW();
      if (parent_env) {
        EnvironmentMap merged_env;

        wchar_t* env_ptr = parent_env;

        while (*env_ptr) {
          std::wstring env_str(env_ptr);
          size_t eq_pos = env_str.find(L'=');

          if (eq_pos != std::wstring::npos && eq_pos > 0) {
            std::wstring key = env_str.substr(0, eq_pos);
            std::wstring value = env_str.substr(eq_pos + 1);

            std::string key_utf8 = Helpers::wstring_to_string(key);
            std::string val_utf8 = Helpers::wstring_to_string(value);

            if (!key_utf8.empty()) {
              merged_env[key_utf8] = val_utf8;
            }
          }

          env_ptr += env_str.length() + 1;
        }

        ::FreeEnvironmentStringsW(parent_env);

        for (const auto& [key, value] : impl_->environment_map) {
          merged_env[key] = value;
        }

        env_block = build_environment_block(merged_env);
      }
    } else {
      env_block = build_environment_block(impl_->environment_map);
    }
  }

  void* env_ptr = env_block.empty() ? nullptr : env_block.data();

  std::wstring work_dir_wide;
  const wchar_t* work_dir = nullptr;

  {
    std::shared_lock lock(impl_->shared_mtx);

    if (!impl_->working_directory.empty()) {
      work_dir_wide = Helpers::string_to_wstring(impl_->working_directory);
      work_dir = work_dir_wide.c_str();
    }
  }

  BOOL result = ::CreateProcessW(nullptr, cmd_line_buf.data(), nullptr, nullptr, TRUE,
                                 CREATE_UNICODE_ENVIRONMENT | EXTENDED_STARTUPINFO_PRESENT, env_ptr, work_dir,
                                 &si.StartupInfo, &pi);

  ::DeleteProcThreadAttributeList(si.lpAttributeList);

  if (child_stderr == child_stdout) {
    child_stderr = INVALID_HANDLE_VALUE;
  }

  close_handle_if_valid(child_stdin);
  close_handle_if_valid(child_stdout);
  close_handle_if_valid(child_stderr);

  if VUNLIKELY (!result) {
    return false;
  }

  impl_->process = pi.hProcess;
  impl_->thread = pi.hThread;
  impl_->process_id = pi.dwProcessId;

  close_handle_if_valid(impl_->stdin_read);

  if (mode != kForwardedMode && mode != kForwardedOutputMode) {
    const HANDLE stdout_write = impl_->stdout_write;
    close_handle_if_valid(impl_->stdout_write);

    if (impl_->stderr_write == stdout_write) {
      impl_->stderr_write = INVALID_HANDLE_VALUE;
    }
  }

  if (mode != kForwardedMode && mode != kForwardedErrorMode && mode != kMergedMode) {
    close_handle_if_valid(impl_->stderr_write);
  }

  return true;
#else
  EnvironmentMap env_map_copy;
  std::string working_directory_copy;
  bool inherit_env = impl_->inherit_environment.load(std::memory_order_acquire);
  Mode mode = impl_->mode.load(std::memory_order_acquire);
  ssize_t written;

  {
    std::shared_lock lock(impl_->shared_mtx);
    env_map_copy = impl_->environment_map;
    working_directory_copy = impl_->working_directory;
  }

  int exec_failure_pipe[2];

  if VUNLIKELY (pipe(exec_failure_pipe) != 0) {
    int err = errno;
    CLOG_E("Process: Failed to create exec failure pipe: %s.", strerror(err));
    return false;
  }

  fcntl(exec_failure_pipe[1], F_SETFD, FD_CLOEXEC);

  impl_->process_id = fork();

  if VUNLIKELY (impl_->process_id < 0) {
    int err = errno;
    CLOG_E("Process: Fork failed: %s.", strerror(err));

    ::close(exec_failure_pipe[0]);
    ::close(exec_failure_pipe[1]);

    return false;
  }

  if VUNLIKELY (impl_->process_id == 0) {
    ::close(exec_failure_pipe[0]);

    dup2(impl_->stdin_pipe[0], STDIN_FILENO);

    if (mode == kForwardedMode) {
    } else if (mode == kForwardedOutputMode) {
      dup2(impl_->stderr_pipe[1], STDERR_FILENO);
    } else if (mode == kForwardedErrorMode) {
      dup2(impl_->stdout_pipe[1], STDOUT_FILENO);
    } else if (mode == kMergedMode) {
      dup2(impl_->stdout_pipe[1], STDOUT_FILENO);
      dup2(impl_->stdout_pipe[1], STDERR_FILENO);
    } else {
      dup2(impl_->stdout_pipe[1], STDOUT_FILENO);
      dup2(impl_->stderr_pipe[1], STDERR_FILENO);
    }

    int max_fd = static_cast<int>(sysconf(_SC_OPEN_MAX));

    if VUNLIKELY (max_fd < 0) {
      max_fd = 1024;
    }

    for (int fd = 0; fd < max_fd; ++fd) {
      if (fd != STDIN_FILENO && fd != STDOUT_FILENO && fd != STDERR_FILENO && fd != exec_failure_pipe[1]) {
        ::close(fd);
      }
    }

    if (inherit_env) {
      for (const auto& [key, value] : env_map_copy) {
        setenv(key.c_str(), value.c_str(), 1);
      }
    }

    if VUNLIKELY (!working_directory_copy.empty() && chdir(working_directory_copy.c_str()) != 0) {
      int error_code = errno;

      written = ::write(exec_failure_pipe[1], &error_code, sizeof(error_code));

      (void)written;

      _exit(kExecFailedExitCode);
    }

    std::vector<std::string> args_storage;
    args_storage.reserve(arguments.size() + 2);
    args_storage.emplace_back(program);
    args_storage.insert(args_storage.end(), arguments.begin(), arguments.end());

    std::vector<char*> args;
    args.reserve(args_storage.size() + 1);

    for (auto& arg : args_storage) {
      args.emplace_back(const_cast<char*>(arg.c_str()));
    }

    args.emplace_back(nullptr);

    if (inherit_env) {
      execvp(program.c_str(), args.data());
    } else {
      std::vector<std::string> envp_storage;
      envp_storage.reserve(env_map_copy.size());

      for (const auto& [key, value] : env_map_copy) {
        std::string tmp_envp;

        tmp_envp.append(key);
        tmp_envp.append("=");
        tmp_envp.append(value);

        envp_storage.emplace_back(std::move(tmp_envp));
      }

      std::vector<char*> envp;
      envp.reserve(envp_storage.size() + 1);

      for (auto& s : envp_storage) {
        envp.emplace_back(const_cast<char*>(s.c_str()));
      }

      envp.emplace_back(nullptr);

      std::string exec_program = resolve_program_path(program, env_map_copy);
      execve(exec_program.c_str(), args.data(), envp.data());
    }

    int error_code = errno;

    written = ::write(exec_failure_pipe[1], &error_code, sizeof(error_code));

    (void)written;

    _exit(kExecFailedExitCode);
  }

  ::close(exec_failure_pipe[1]);

  ::close(impl_->stdin_pipe[0]);
  impl_->stdin_pipe[0] = -1;

  if (mode != kForwardedMode && mode != kForwardedOutputMode) {
    if (impl_->stdout_pipe[1] >= 0) {
      ::close(impl_->stdout_pipe[1]);
      impl_->stdout_pipe[1] = -1;
    }
  }

  if (mode != kForwardedMode && mode != kForwardedErrorMode && mode != kMergedMode) {
    if (impl_->stderr_pipe[1] >= 0) {
      ::close(impl_->stderr_pipe[1]);
      impl_->stderr_pipe[1] = -1;
    }
  }

  int error_code = 0;
  ssize_t n = 0;

  do {
    n = ::read(exec_failure_pipe[0], &error_code, sizeof(error_code));
  } while (n < 0 && errno == EINTR);

  ::close(exec_failure_pipe[0]);

  if (n > 0) {
    int status = 0;
    pid_t wait_result = 0;

    do {
      wait_result = waitpid(impl_->process_id, &status, 0);
    } while (wait_result < 0 && errno == EINTR);

    impl_->process_id = -1;
    errno = error_code;
    return false;
  }

  return true;
#endif
}

void Process::start_monitor_thread() {
  impl_->monitor_running.store(true, std::memory_order_release);
  impl_->monitor_should_stop.store(false, std::memory_order_release);
  impl_->monitor_thread = std::make_unique<std::thread>([this]() { monitor_thread(); });
}

void Process::stop_monitor_thread() {
  impl_->monitor_should_stop.store(true, std::memory_order_release);
  impl_->monitor_running.store(false, std::memory_order_release);

  if VLIKELY (impl_->monitor_thread && impl_->monitor_thread->joinable()) {
    if (impl_->monitor_thread->get_id() == std::this_thread::get_id()) {
      return;
    }

    impl_->monitor_thread->join();
  }

  impl_->monitor_thread.reset();
}

void Process::monitor_thread() {
#ifdef _WIN32
  while (impl_->monitor_running.load(std::memory_order_acquire) &&
         impl_->state.load(std::memory_order_acquire) == kRunningState) {
    if (impl_->monitor_should_stop.load(std::memory_order_acquire)) {
      break;
    }

    if (impl_->exit_processed.load(std::memory_order_acquire)) {
      break;
    }

    DWORD wait_ret = ::WaitForSingleObject(impl_->process, 10);

    if (wait_ret == WAIT_OBJECT_0) {
      DWORD exit_code_win = 0;

      if (::GetExitCodeProcess(impl_->process, &exit_code_win)) {
        read_from_pipes_with_lock();
        handle_process_exit(static_cast<int>(exit_code_win), kNormalExitStatus);
      }

      break;
    }

    read_from_pipes_with_lock();
  }
#else
  struct pollfd fds[3];
  int nfds = 0;

  while (impl_->monitor_running.load(std::memory_order_acquire) &&
         impl_->state.load(std::memory_order_acquire) == kRunningState) {
    if (impl_->monitor_should_stop.load(std::memory_order_acquire)) {
      break;
    }

    if (impl_->exit_processed.load(std::memory_order_acquire)) {
      break;
    }

    int status;

    pid_t result = waitpid(impl_->process_id, &status, WNOHANG);

    if (result == impl_->process_id) {
      int exit_code = -1;
      ExitStatus exit_status = kCrashExitStatus;

      if VLIKELY (WIFEXITED(status)) {
        exit_code = WEXITSTATUS(status);
        exit_status = kNormalExitStatus;
      } else if (WIFSIGNALED(status)) {
        int sig = WTERMSIG(status);
        exit_code = 128 + sig;
        exit_status = kCrashExitStatus;
      }

      read_from_pipes_with_lock();
      handle_process_exit(exit_code, exit_status);

      break;
    } else if (result < 0) {
      if VUNLIKELY (errno != EINTR && errno != ECHILD) {
        break;
      }
    }

    nfds = 0;

    if (impl_->stdout_pipe[0] >= 0 && !impl_->stdout_closed.load(std::memory_order_acquire)) {
      fds[nfds].fd = impl_->stdout_pipe[0];
      fds[nfds].events = POLLIN;
      fds[nfds].revents = 0;
      ++nfds;
    }

    if (impl_->stderr_pipe[0] >= 0 && !impl_->stderr_closed.load(std::memory_order_acquire)) {
      fds[nfds].fd = impl_->stderr_pipe[0];
      fds[nfds].events = POLLIN;
      fds[nfds].revents = 0;
      ++nfds;
    }

    if (nfds > 0) {
      int poll_ret = ::poll(fds, nfds, 50);
      (void)poll_ret;
    } else {
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    read_from_pipes_with_lock();
  }
#endif
}

void Process::handle_process_exit(int exit_code, ExitStatus status) {
  bool expected = false;

  if (impl_->exit_processed.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
#ifdef _WIN32
    status =
        normalize_windows_exit_status(status, impl_->forced_termination.exchange(false, std::memory_order_acq_rel));
#else
    impl_->process_id = -1;
#endif
    impl_->exit_code.store(exit_code, std::memory_order_release);
    impl_->exit_status.store(status, std::memory_order_release);

    set_state(kNotRunningState);

    if (!impl_->is_being_destroyed.load(std::memory_order_acquire)) {
      invoke_callbacks_outside_lock(kNoError, true, exit_code, status, kNotRunningState, true, false, false);
    }
  }
}

void Process::invoke_callbacks_outside_lock(Error error_to_report, bool has_finished, int exit_code_to_report,
                                            ExitStatus exit_status_to_report, State state_to_report,
                                            bool has_state_changed, bool has_stdout_data, bool has_stderr_data) {
  if (impl_->is_being_destroyed.load(std::memory_order_acquire)) {
    return;
  }

  ErrorCallback error_cb;
  FinishedCallback finished_cb;
  ReadyReadCallback stdout_cb;
  ReadyReadCallback stderr_cb;
  StateChangedCallback state_cb;

  {
    std::shared_lock lock(impl_->shared_mtx);

    if (error_to_report != kNoError && !impl_->error_reported.exchange(true, std::memory_order_acq_rel)) {
      error_cb = impl_->error_callback;
    }

    if (has_finished) {
      finished_cb = impl_->finished_callback;
    }

    if (has_stdout_data) {
      stdout_cb = impl_->ready_read_stdout_callback;
    }

    if (has_stderr_data) {
      stderr_cb = impl_->ready_read_stderr_callback;
    }

    if (has_state_changed) {
      state_cb = impl_->state_changed_callback;
    }
  }

  if (stdout_cb) {
    if (impl_->is_being_destroyed.load(std::memory_order_acquire)) {
      return;
    }

    stdout_cb();
  }

  if (stderr_cb) {
    if (impl_->is_being_destroyed.load(std::memory_order_acquire)) {
      return;
    }

    stderr_cb();
  }

  if (state_cb) {
    if (impl_->is_being_destroyed.load(std::memory_order_acquire)) {
      return;
    }

    state_cb(state_to_report);
  }

  if (finished_cb) {
    if (impl_->is_being_destroyed.load(std::memory_order_acquire)) {
      return;
    }

    finished_cb(exit_code_to_report, exit_status_to_report);
  }

  if (error_cb) {
    if (impl_->is_being_destroyed.load(std::memory_order_acquire)) {
      return;
    }

    error_cb(error_to_report);
  }
}

}  // namespace vlink
