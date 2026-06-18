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
 * @file utils.h
 * @brief Portable host-system utility surface used across the VLink runtime.
 *
 * @details
 * @c vlink::Utils gathers free functions that paper over differences between Linux, macOS,
 * Windows, QNX and Android.  All public entry points are @c noexcept and report failure
 * via empty strings, @c false return values or sentinel values such as @c pid @c == @c -1.
 *
 * Helper categories provided by the namespace:
 *
 * | Category               | Representative entry points                                       |
 * | ---------------------- | ----------------------------------------------------------------- |
 * | Process introspection  | @c get_app_path, @c get_app_dir, @c get_app_name, @c get_pid      |
 * | Host identity          | @c get_host_name, @c get_machine_id, @c get_timezone_diff         |
 * | Filesystem helpers     | @c get_tmp_dir, @c wait_for_device                                |
 * | Environment management | @c get_env, @c set_env, @c unset_env                              |
 * | Network discovery      | @c get_all_ipv4_address, @c get_dds_default_address               |
 * | Thread control         | @c set_thread_name, @c set_thread_priority, @c set_thread_stick   |
 * | Signal handling        | @c register_terminate_signal, @c register_crash_signal            |
 * | Input/terminal         | @c start_detect_keyboard, @c get_terminal_size                    |
 * | System metrics         | @c get_cpu_usage, @c get_memory_usage, @c is_process_running      |
 * | Memory hints           | @c try_release_sys_memory                                         |
 * | Low-level primitives   | @c yield_cpu                                                      |
 *
 * @note
 * - @c yield_cpu() is inlined to emit the optimal idle hint per ISA: @c PAUSE on x86,
 *   @c YIELD on ARM, an explicit fence on RISC-V, otherwise @c std::this_thread::yield().
 *
 * @par Example
 * @code
 * vlink::Utils::set_thread_name("worker");
 * vlink::Utils::set_thread_stick(0b11);
 *
 * vlink::Utils::register_terminate_signal([](int sig) {
 *   (void)sig;
 *   shutdown_application();
 * });
 * @endcode
 */

#pragma once

#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "./functional.h"
#include "./macros.h"

#ifdef _MSC_VER
extern "C" void _mm_pause(void);
#endif

namespace vlink {

/**
 * @namespace vlink::Utils
 * @brief Portable host-system utility functions.
 */
namespace Utils {  // NOLINT(readability-identifier-naming)

/**
 * @brief Returns the absolute path of the executable that is currently running.
 *
 * @return Full filesystem path, or an empty string when the OS query fails.
 */
[[nodiscard]] VLINK_EXPORT std::string get_app_path() noexcept;

/**
 * @brief Returns the directory portion of the executable's absolute path.
 *
 * @return Directory without a trailing separator, or an empty string on failure.
 */
[[nodiscard]] VLINK_EXPORT std::string get_app_dir() noexcept;

/**
 * @brief Returns the file-name portion of the executable's absolute path.
 *
 * @return Executable name, or an empty string on failure.
 */
[[nodiscard]] VLINK_EXPORT std::string get_app_name() noexcept;

/**
 * @brief Returns the local machine's host name as reported by the operating system.
 *
 * @return Host name string, or an empty string on failure.
 */
[[nodiscard]] VLINK_EXPORT std::string get_host_name() noexcept;

/**
 * @brief Returns the process identifier of the calling process.
 *
 * @return PID, or @c -1 on failure.
 */
[[nodiscard]] VLINK_EXPORT int32_t get_pid() noexcept;

/**
 * @brief Returns the process identifier of the calling process as a decimal string.
 *
 * @return PID string, or an empty string on failure.
 */
[[nodiscard]] VLINK_EXPORT std::string get_pid_str() noexcept;

/**
 * @brief Returns a path suitable for short-lived temporary files.
 *
 * @details
 * Honours the @c VLINK_TMP_DIR environment override.  Otherwise falls back to
 * @c std::filesystem::temp_directory_path(), or @c /var/log on QNX.
 *
 * @return Temporary directory path.
 */
[[nodiscard]] VLINK_EXPORT std::string get_tmp_dir() noexcept;

/**
 * @brief Reads the current value of an environment variable.
 *
 * @param key            Variable name.
 * @param default_value  Value returned when @p key is not set.  Default: empty string.
 * @return Variable value, or @p default_value when unset.
 */
[[nodiscard]] VLINK_EXPORT std::string get_env(const std::string& key, const std::string& default_value = "") noexcept;

/**
 * @brief Sets or updates an environment variable.
 *
 * @param key    Variable name.
 * @param value  Variable value.
 * @param force  When @c true (default), overwrites an existing variable.
 * @return @c true on success.
 */
VLINK_EXPORT bool set_env(const std::string& key, const std::string& value, bool force = true) noexcept;

/**
 * @brief Removes an environment variable.
 *
 * @param key  Variable name.
 * @return @c true on success.
 */
VLINK_EXPORT bool unset_env(const std::string& key) noexcept;

/**
 * @brief Returns every IPv4 address bound to a local network interface.
 *
 * @param filter_available  When @c true, only includes interfaces in the UP state.  Default: @c false.
 * @return Vector of dotted-decimal strings.
 */
[[nodiscard]] VLINK_EXPORT std::vector<std::string> get_all_ipv4_address(bool filter_available = false) noexcept;

/**
 * @brief Returns every IPv6 address bound to a local network interface.
 *
 * @param filter_available  When @c true, only includes interfaces in the UP state.  Default: @c false.
 * @return Vector of IPv6 strings.
 */
[[nodiscard]] VLINK_EXPORT std::vector<std::string> get_all_ipv6_address(bool filter_available = false) noexcept;

/**
 * @brief Returns the interface name that owns a given IPv4 address.
 *
 * @param ipv4  Address to look up.
 * @return Interface name, or an empty string when no match exists.
 */
[[nodiscard]] VLINK_EXPORT std::string get_interface_name_by_ipv4(const std::string& ipv4) noexcept;

/**
 * @brief Returns the interface name that owns a given IPv6 address.
 *
 * @param ipv6  Address to look up.
 * @return Interface name, or an empty string when no match exists.
 */
[[nodiscard]] VLINK_EXPORT std::string get_interface_name_by_ipv6(const std::string& ipv6) noexcept;

/**
 * @brief Selects IPv4 addresses suitable as DDS participant unicast locators.
 *
 * @details
 * Filters out loopback and link-local addresses, preferring routable unicast ones.
 *
 * @param filter_available  When @c true, only includes UP interfaces.  Default: @c false.
 * @param max_count         Upper bound on the number of returned addresses.  Default: @c 5.
 * @return Vector of selected IPv4 strings.
 */
[[nodiscard]] VLINK_EXPORT std::vector<std::string> get_dds_default_address(bool filter_available = false,
                                                                            int max_count = 5) noexcept;

/**
 * @brief Provides a singleton mutual-exclusion check for a program name.
 *
 * @details
 * Uses a Win32 mutex on Windows and a POSIX lock file under @c VLINK_LOCK_DIR (or the
 * platform default) elsewhere.  Returns @c false when another instance already holds
 * the lock.
 *
 * @param program_name  Program tag used to build the lock identity.  Empty defaults to the executable name.
 * @return @c true when this process holds the singleton lock.
 */
[[nodiscard]] VLINK_EXPORT bool check_singleton(const std::string& program_name = "") noexcept;

/**
 * @brief Polls a filesystem path until it exists or a timeout expires.
 *
 * @details
 * Useful for waiting for device nodes (e.g. @c /dev/video0) at startup.
 *
 * @param path        Filesystem path to poll.
 * @param timeout_ms  Maximum total wait in milliseconds.
 * @param poll_ms     Polling interval in milliseconds.  Default: @c 50.
 * @return @c true when the path appears within the timeout.
 */
VLINK_EXPORT bool wait_for_device(const std::string& path, int timeout_ms, int poll_ms = 50) noexcept;

/**
 * @brief Emits the most efficient CPU pause/yield hint for the host ISA.
 *
 * @details
 * Maps to @c PAUSE on x86, @c YIELD on ARMv7/AArch64, an explicit fence on RISC-V, and
 * @c std::this_thread::yield() as a portable fallback.  Always inlined.
 */
VLINK_EXPORT void yield_cpu() noexcept;

/**
 * @brief Sets the Windows console output code page to UTF-8.
 *
 * @details
 * Calls @c SetConsoleOutputCP(CP_UTF8); a no-op on non-Windows targets.
 */
VLINK_EXPORT void set_console_utf8_output() noexcept;

/**
 * @brief Sets the OS-visible name of a thread so debug tools display it.
 *
 * @param name    Thread name; Linux truncates beyond 15 characters.
 * @param thread  Thread to rename, or @c nullptr for the calling thread.  Default: @c nullptr.
 * @return @c true on success.
 */
VLINK_EXPORT bool set_thread_name(const std::string& name, std::thread* thread = nullptr) noexcept;

/**
 * @brief Updates the scheduling policy and priority of a thread.
 *
 * @details
 * On Linux wraps @c pthread_setschedparam.  Real-time policies require @c CAP_SYS_NICE
 * or an adequate @c RLIMIT_RTPRIO.  POSIX policy constants (@c SCHED_FIFO, @c SCHED_RR,
 * @c SCHED_OTHER) come from @c <sched.h>, which callers must include.  On Windows the
 * value is mapped to a thread priority class or silently ignored.
 *
 * @param priority_level  Policy-dependent priority value.
 * @param policy          Scheduling policy; @c -1 leaves the existing policy untouched.
 * @param thread          Thread to configure, or @c nullptr for the calling thread.  Default: @c nullptr.
 * @return @c true on success.
 */
VLINK_EXPORT bool set_thread_priority(int priority_level, int policy = -1, std::thread* thread = nullptr) noexcept;

/**
 * @brief Pins a thread to a set of CPU cores expressed as a bitmask.
 *
 * @details
 * Bit @c i of @p core_mask corresponds to core @c i, so @c 0b0101 pins to cores @c 0 and
 * @c 2.  Wraps @c pthread_setaffinity_np on Linux.
 *
 * @param core_mask  Bitmask of CPU cores.
 * @param thread     Thread to pin, or @c nullptr for the calling thread.  Default: @c nullptr.
 * @return @c true on success.
 */
VLINK_EXPORT bool set_thread_stick(uint32_t core_mask, std::thread* thread = nullptr) noexcept;

/**
 * @brief Returns the native OS thread identifier of the calling thread.
 *
 * @details
 * Maps to:
 *  - Linux / Android / QNX: @c syscall(SYS_gettid) — kernel TID matching @c top / @c perf.
 *  - macOS / iOS: @c pthread_threadid_np() — 64-bit Mach thread id.
 *  - Windows: @c GetCurrentThreadId() — Win32 thread id.
 *
 * @return Native thread identifier.
 */
[[nodiscard]] VLINK_EXPORT uint64_t get_native_thread_id() noexcept;

/**
 * @brief Installs a callback for graceful termination signals.
 *
 * @details
 * Hooks @c SIGINT, @c SIGTERM and @c SIGHUP on POSIX, or @c SIGINT / @c SIGTERM on Windows.
 *
 * @param callback      Callback receiving the signal number.
 * @param is_async      When @c true, runs the callback on a dedicated thread instead of the
 *                      signal context.  Default: @c false.
 * @param pass_through  When @c true, re-raises the signal after the callback returns so the
 *                      default OS behaviour also fires.  Default: @c false.
 */
VLINK_EXPORT void register_terminate_signal(MoveFunction<void(int)>&& callback, bool is_async = false,
                                            bool pass_through = false) noexcept;

/**
 * @brief Installs a callback for crash signals such as @c SIGSEGV, @c SIGABRT, @c SIGFPE, @c SIGBUS.
 *
 * @details
 * Useful for emitting crash diagnostics.  The callback should be async-signal-safe and
 * short.
 *
 * @param callback  Callback receiving the signal number.
 */
VLINK_EXPORT void register_crash_signal(MoveFunction<void(int)>&& callback) noexcept;

/**
 * @brief Starts a background poller that detects keyboard input on stdin.
 *
 * @details
 * Calls @p callback with a key name string (such as @c "enter" or @c "q") whenever a
 * key is detected.  Stop with @c stop_detect_keyboard().
 *
 * @param callback  Callback receiving the key name.  Default: @c nullptr (ignore events).
 * @param poll_ms   Polling interval in milliseconds.  Default: @c 20.
 */
VLINK_EXPORT void start_detect_keyboard(MoveFunction<void(const std::string& key)>&& callback = nullptr,
                                        int poll_ms = 20) noexcept;

/**
 * @brief Stops the keyboard poller started by @c start_detect_keyboard().
 */
VLINK_EXPORT void stop_detect_keyboard() noexcept;

/**
 * @brief Returns the current terminal dimensions in columns and rows.
 *
 * @return Pair @c {columns, @c rows}; @c {-1, @c -1} when stdout is not a TTY.
 */
[[nodiscard]] VLINK_EXPORT std::pair<int, int> get_terminal_size() noexcept;

/**
 * @brief Returns the system-wide CPU usage as a percentage averaged over all logical CPUs.
 *
 * @details
 * Implemented as @c (1 @c - @c idle_delta @c / @c total_delta) @c * @c 100 from
 * @c /proc/stat on Linux/Android or @c GetSystemTimes on Windows.  The first call after
 * process start usually returns @c 0 because there is no previous sample.
 *
 * @return Percentage in @c [0.0, @c 100.0].
 */
[[nodiscard]] VLINK_EXPORT double get_cpu_usage() noexcept;

/**
 * @brief Returns the system-wide memory usage as a percentage of total physical RAM.
 *
 * @return Percentage in @c [0.0, @c 100.0].
 */
[[nodiscard]] VLINK_EXPORT double get_memory_usage() noexcept;

/**
 * @brief Reports whether at least one process with the given executable name is alive.
 *
 * @param process_name  Executable name without a path component.
 * @return @c true when a matching process exists.
 */
[[nodiscard]] VLINK_EXPORT bool is_process_running(const std::string& process_name) noexcept;

/**
 * @brief Returns the local timezone offset from UTC in minutes.
 *
 * @details
 * UTC+8 returns @c 480; UTC-5 returns @c -300.
 *
 * @return Offset in minutes.
 */
[[nodiscard]] VLINK_EXPORT int32_t get_timezone_diff() noexcept;

/**
 * @brief Returns a stable identifier for the host machine.
 *
 * @details
 * Reads @c /etc/machine-id on Linux and uses platform-specific equivalents elsewhere.
 *
 * @return Machine ID string, or an empty string on failure.
 */
[[nodiscard]] VLINK_EXPORT std::string get_machine_id() noexcept;

/**
 * @brief Hints to the OS that any unreferenced cached memory pages can be released.
 *
 * @details
 * On Linux invokes @c malloc_trim(0); on other platforms it is a no-op.
 */
VLINK_EXPORT void try_release_sys_memory() noexcept;

////////////////////////////////////////////////////////////////
/// Details
////////////////////////////////////////////////////////////////

#if defined(__GNUC__) && !defined(_WIN32) && !defined(__CYGWIN__)
inline __attribute__((artificial)) void yield_cpu() noexcept {
#else
inline void yield_cpu() noexcept {
#endif
#if __has_builtin(__yield)
  __yield();
#elif defined(_MSC_VER) && defined(_YIELD_PROCESSOR)
  _YIELD_PROCESSOR();
#elif __has_builtin(__builtin_ia32_pause)
__builtin_ia32_pause();
#elif defined(__x86_64__) || defined(_M_X64) || defined(__i386) || defined(_M_IX86)
#if defined(_MSC_VER)
_mm_pause();
#elif defined(__GNUC__)
__builtin_ia32_pause();
#else
__asm__("pause");
#endif
#elif __has_builtin(__builtin_arm_yield)
__builtin_arm_yield();
#elif defined(__arm__) || defined(__aarch64__)
#if defined(__GNUC__)
__asm__("yield");
#else
std::this_thread::yield();
#endif
#elif defined(__riscv)
__asm__(".word 0x0100000f");
#elif defined(_MSC_VER) && defined(_YIELD_PROCESSOR)
_YIELD_PROCESSOR();
#else
std::this_thread::yield();
#endif
}

}  // namespace Utils

}  // namespace vlink
