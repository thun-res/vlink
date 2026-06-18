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
 * @file sys_semaphore.h
 * @brief Cross-process named semaphore backed by the host kernel.
 *
 * @details
 * @c vlink::SysSemaphore wraps the platform IPC primitive that is closest to a counting
 * semaphore: a POSIX named semaphore on Linux-like targets, a named Win32 semaphore on
 * Windows, and a dispatch semaphore on macOS.  Cross-process synchronisation works on
 * every backend that exposes a kernel name; the macOS dispatch backend is process-local
 * because Apple deprecated the named POSIX API.
 *
 * Comparison with the in-process @c vlink::Semaphore:
 *
 * | Aspect                 | @c Semaphore (in-process)   | @c SysSemaphore (kernel)                    |
 * | ---------------------- | --------------------------- | ------------------------------------------- |
 * | Scope                  | Single process              | Cross-process via OS name                   |
 * | Backing primitive      | @c std::condition_variable  | @c sem_open / @c CreateSemaphore            |
 * | Naming                 | None (object handle only)   | UTF-8 name (POSIX requires @c "/x")         |
 * | Signal-safe @c release | No                          | Yes on POSIX                                |
 * | Persistence            | Bound to process lifetime   | Persists in kernel namespace until unlinked |
 *
 * Lifecycle:
 * -# Construct with the initial count for the @b creator process.
 * -# Call @c attach(name) to create the kernel object or open an existing one.
 * -# Use @c acquire and @c release for P/V.
 * -# Call @c detach(force) explicitly, or rely on the destructor (force=@c false).
 *
 * @note
 * - The initial count is honoured only on first creation.  Subsequent @c attach() calls
 *   on a pre-existing kernel object inherit its current value.
 * - macOS ignores @p name and produces an in-process dispatch semaphore.
 *
 * @par Example
 * @code
 * // Process A (server)
 * vlink::SysSemaphore sem(0);
 * sem.attach("/vlink_ready");
 * do_init();
 * sem.release();
 *
 * // Process B (client)
 * vlink::SysSemaphore sem;
 * sem.attach("/vlink_ready");
 * sem.acquire();
 * @endcode
 */

#pragma once

#include <memory>
#include <string>

#include "./macros.h"

namespace vlink {

/**
 * @class SysSemaphore
 * @brief Counting semaphore that can be shared across processes through an OS name.
 *
 * @details
 * Resolves to a POSIX named semaphore, a Win32 named semaphore or a macOS dispatch
 * semaphore depending on the host platform.
 */
class VLINK_EXPORT SysSemaphore final {
 public:
  /**
   * @brief Sentinel value for @c acquire() meaning wait indefinitely.
   */
  static constexpr int kInfinite{-1};

  /**
   * @brief Constructs the wrapper with the initial count used when @c attach() creates the kernel object.
   *
   * @param count  Initial permit count for first creation.  Default: @c 0.
   */
  explicit SysSemaphore(size_t count = 0);

  /**
   * @brief Destructor.  Invokes @c detach(false) when the wrapper is still attached.
   */
  ~SysSemaphore();

  /**
   * @brief Creates or opens the kernel object identified by @p name.
   *
   * @details
   * When the kernel object already exists the constructor-supplied initial count is
   * ignored.  POSIX backends require @p name to begin with @c '/'.  macOS ignores @p name
   * and produces a fresh in-process dispatch semaphore.
   *
   * @param name  Platform-specific semaphore name.
   * @return @c true on success, @c false otherwise.
   */
  bool attach(const std::string& name);

  /**
   * @brief Releases the kernel handle and optionally unlinks the named object.
   *
   * @param force  When @c true, POSIX backends unlink the name so it disappears from the
   *               kernel namespace.  Ignored on Windows and macOS.  Default: @c true.
   * @return @c true on success.
   */
  bool detach(bool force = true);

  /**
   * @brief Decrements the kernel counter by @p n, blocking when permits are unavailable.
   *
   * @param n           Number of permits requested.  Default: @c 1.
   * @param timeout_ms  Maximum wait in milliseconds; @c kInfinite waits forever.
   * @return @c true when the permits were acquired; @c false on timeout or error.
   *
   * @pre @c is_attached() must be @c true.
   */
  bool acquire(size_t n = 1, int timeout_ms = kInfinite);

  /**
   * @brief Increments the kernel counter by @p n, waking blocked acquirers.
   *
   * @param n  Number of permits to add.  Default: @c 1.
   *
   * @pre @c is_attached() must be @c true.
   */
  void release(size_t n = 1);

  /**
   * @brief Reports whether the wrapper currently owns a kernel handle.
   *
   * @return @c true when @c attach() succeeded and @c detach() has not been called yet.
   */
  [[nodiscard]] bool is_attached() const;

  /**
   * @brief Returns a non-atomic snapshot of the current counter value.
   *
   * @details
   * Some backends (notably macOS dispatch) do not expose the counter; @c 0 is returned
   * in that case.
   *
   * @return Current permit count snapshot.
   */
  [[nodiscard]] size_t get_count() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;

  VLINK_DISALLOW_COPY_AND_ASSIGN(SysSemaphore)
};

}  // namespace vlink
