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

#include "./base/sys_semaphore.h"

#include <chrono>
#include <string>

#include "./base/logger.h"

#if defined(_WIN32) || defined(__CYGWIN__)
#include <Windows.h>
#elif defined(__APPLE__)
#include <dispatch/dispatch.h>
#else
#include <fcntl.h>
#include <semaphore.h>
#endif

namespace vlink {

// SysSemaphore::Impl
struct SysSemaphore::Impl final {
#if defined(_WIN32) || defined(__CYGWIN__)
  HANDLE handle{nullptr};
#elif defined(__APPLE__)
  dispatch_semaphore_t handle{nullptr};
#else
  sem_t* handle{SEM_FAILED};
  std::string name;
  bool is_create{false};
#endif
  size_t count{0};
};

// SysSemaphore
SysSemaphore::SysSemaphore(size_t count) : impl_(std::make_unique<Impl>()) { impl_->count = count; }

SysSemaphore::~SysSemaphore() {
  if VLIKELY (is_attached()) {
    detach(false);
  }
}

bool SysSemaphore::attach(const std::string& name) {
  if VUNLIKELY (is_attached()) {
    VLOG_E("SysSemaphore: Already attached.");
    return false;
  }

#if defined(_WIN32) || defined(__CYGWIN__)
  impl_->handle = ::CreateSemaphore(nullptr, impl_->count, MAXLONG, name.c_str());

  if VUNLIKELY (!impl_->handle) {
    VLOG_E("SysSemaphore: CreateSemaphore failed.");
    return false;
  }

  return true;

#elif defined(__APPLE__)
  (void)name;
  impl_->handle = dispatch_semaphore_create(static_cast<int64_t>(impl_->count));

  if VUNLIKELY (!impl_->handle) {
    VLOG_E("SysSemaphore: dispatch_semaphore_create failed.");
    return false;
  }

  return true;

#else
  // ::sem_unlink(impl_->name.c_str());  // clear
  int oflag = O_CREAT | O_EXCL;
  for (int i = 0, max_try_times = 1; i < max_try_times; ++i) {
    do {
      impl_->handle = ::sem_open(name.c_str(), oflag, 0600, impl_->count);
    } while (impl_->handle == SEM_FAILED && errno == EINTR);

    if (impl_->handle == SEM_FAILED && errno == EEXIST) {
      oflag &= ~O_EXCL;
      max_try_times = 2;
    } else {
      break;
    }
  }

  if VUNLIKELY (impl_->handle == SEM_FAILED) {
    VLOG_E("SysSemaphore: sem_open failed.");
    return false;
  }

  impl_->name = name;
  impl_->is_create = (oflag & O_EXCL) != 0;

  return true;
#endif
}

bool SysSemaphore::detach(bool force) {
  if VUNLIKELY (!is_attached()) {
    VLOG_E("SysSemaphore: Not attached.");
    return false;
  }

#if defined(_WIN32) || defined(__CYGWIN__)
  (void)force;

  if VUNLIKELY (!::CloseHandle(impl_->handle)) {
    VLOG_E("SysSemaphore: CloseHandle failed.");
    return false;
  }

  impl_->handle = nullptr;

  return true;

#elif defined(__APPLE__)
  (void)force;
  impl_->handle = nullptr;
  return true;

#else

  if VUNLIKELY (impl_->handle == SEM_FAILED) {
    VLOG_E("SysSemaphore: Handle is empty.");
    return false;
  }

  bool ok = true;

  if VUNLIKELY (::sem_close(impl_->handle) == -1) {
    VLOG_E("SysSemaphore: sem_close failed.");
    ok = false;
  }

  impl_->handle = SEM_FAILED;

  if (force && !impl_->name.empty()) {
    if VUNLIKELY (::sem_unlink(impl_->name.c_str()) == -1 && errno != ENOENT) {
      VLOG_E("SysSemaphore: sem_unlink failed.");
      ok = false;
    }
  }

  impl_->is_create = false;
  impl_->name.clear();
  return ok;
#endif
}

bool SysSemaphore::acquire(size_t n, int timeout_ms) {
  if VUNLIKELY (!is_attached()) {
    VLOG_E("SysSemaphore: Not attached.");
    return false;
  }

#if defined(_WIN32) || defined(__CYGWIN__)
  size_t acquired = 0;

  if (timeout_ms < 0) {
    for (; n > 0; --n) {
      if VUNLIKELY (::WaitForSingleObjectEx(impl_->handle, INFINITE, FALSE) != WAIT_OBJECT_0) {
        VLOG_E("SysSemaphore: WaitForSingleObjectEx failed.");
        if (acquired > 0) {
          release(acquired);
        }
        return false;
      }

      ++acquired;
    }
  } else {
    const auto start_time = std::chrono::steady_clock::now();

    for (; n > 0; --n) {
      const auto elapsed =
          std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start_time).count();
      const auto remaining_timeout = static_cast<DWORD>(elapsed >= timeout_ms ? 0 : timeout_ms - elapsed);
      const auto wait_result = ::WaitForSingleObjectEx(impl_->handle, remaining_timeout, FALSE);

      if VLIKELY (wait_result == WAIT_OBJECT_0) {
        ++acquired;
        continue;
      }

      if VUNLIKELY (wait_result != WAIT_TIMEOUT) {
        VLOG_E("SysSemaphore: WaitForSingleObjectEx failed.");
      }

      if (acquired > 0) {
        release(acquired);
      }

      return false;
    }
  }

  return true;
#elif defined(__APPLE__)
  for (; n > 0; --n) {
    if (timeout_ms < 0) {
      dispatch_semaphore_wait(impl_->handle, DISPATCH_TIME_FOREVER);
    } else {
      int64_t timeout_ns = static_cast<int64_t>(timeout_ms) * 1000000;
      if VUNLIKELY (dispatch_semaphore_wait(impl_->handle, dispatch_time(DISPATCH_TIME_NOW, timeout_ns)) != 0) {
        VLOG_E("SysSemaphore: dispatch_semaphore_wait timeout or failed.");
        return false;
      }
    }
  }

  return true;

#else
  size_t acquired = 0;

  if (timeout_ms < 0) {
    for (; n > 0; --n) {
      int rc = -1;
      do {
        rc = ::sem_wait(impl_->handle);
      } while (rc == -1 && errno == EINTR);

      if VUNLIKELY (rc == -1) {
        // if (is_attached()) {
        //   VLOG_E("Sys semaphore sem_wait failed.");
        // }

        if (acquired > 0) {
          release(acquired);
        }

        return false;
      }
      ++acquired;
    }
  } else {
    struct timespec ts;

    if VUNLIKELY (::clock_gettime(CLOCK_REALTIME, &ts) == -1) {
      VLOG_E("SysSemaphore: clock_gettime failed.");
      return false;
    }

    ts.tv_sec += timeout_ms / 1000;
    ts.tv_nsec += (timeout_ms % 1000) * 1000'000;

    if (ts.tv_nsec >= 1000'000'000) {
      ts.tv_sec++;
      ts.tv_nsec -= 1000'000'000;
    }

    for (; n > 0; --n) {
      int rc = -1;
      do {
        rc = ::sem_timedwait(impl_->handle, &ts);
      } while (rc == -1 && errno == EINTR);

      if VUNLIKELY (rc == -1) {
        // if (is_attached()) {
        //   if (errno == ETIMEDOUT) {
        //     VLOG_E("Sys semaphore sem_timedwait timed out.");
        //   } else {
        //     VLOG_E("Sys semaphore sem_timedwait failed.");
        //   }
        // }

        if (acquired > 0) {
          release(acquired);
        }

        return false;
      }
      ++acquired;
    }
  }

  return true;
#endif
}

void SysSemaphore::release(size_t n) {
  if VUNLIKELY (!is_attached()) {
    VLOG_E("SysSemaphore: Not attached.");
    return;
  }

  if VUNLIKELY (n == 0) {
    return;
  }

#if defined(_WIN32) || defined(__CYGWIN__)

  if VUNLIKELY (!::ReleaseSemaphore(impl_->handle, n, nullptr)) {
    VLOG_E("SysSemaphore: ReleaseSemaphore failed.");
    return;
  }

#elif defined(__APPLE__)
  for (; n > 0; --n) {
    dispatch_semaphore_signal(impl_->handle);
  }

#else
  for (; n > 0; --n) {
    if VUNLIKELY (::sem_post(impl_->handle) == -1) {
      VLOG_E("SysSemaphore: sem_post failed.");
      return;
    }
  }
#endif
}

bool SysSemaphore::is_attached() const {
#if defined(_WIN32) || defined(__CYGWIN__)
  return impl_->handle != nullptr;
#elif defined(__APPLE__)
  return impl_->handle != nullptr;
#else
  return impl_->handle != SEM_FAILED && !impl_->name.empty();
#endif
}

size_t SysSemaphore::get_count() const {
  if VUNLIKELY (!is_attached()) {
    VLOG_E("SysSemaphore: Not attached.");
    return 0;
  }

#if defined(_WIN32) || defined(__CYGWIN__)
  VLOG_E("SysSemaphore: Querying current count is not supported.");
  return 0;

#elif defined(__APPLE__)
  VLOG_E("SysSemaphore: Querying current count is not supported.");

  return 0;

#else
  int count = 0;

  if VUNLIKELY (::sem_getvalue(impl_->handle, &count) != 0) {
    VLOG_E("SysSemaphore: sem_getvalue failed.");
    return 0;
  }

  return (count >= 0) ? static_cast<size_t>(count) : 0;
#endif
}

}  // namespace vlink
