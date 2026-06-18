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

#include "./base/sys_sharemem.h"

#include <string>

#include "./base/logger.h"

#if __has_include(<unistd.h>)
#include <unistd.h>
#endif

#if defined(_WIN32) || defined(__CYGWIN__)
#include <Windows.h>
#else
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#endif

namespace vlink {

// SysSharemem::Impl
struct SysSharemem::Impl final {
#if defined(_WIN32) || defined(__CYGWIN__)
  HANDLE handle{nullptr};
#else
  int handle{-1};
#endif

  void* data{nullptr};
  size_t size{0};
  std::string name;
  SysSharemem::Mode mode{SysSharemem::kReadWrite};
};

// SysSharemem
SysSharemem::SysSharemem() : impl_(std::make_unique<Impl>()) {}

SysSharemem::~SysSharemem() {
  if VLIKELY (is_attached()) {
    detach(false);
#if defined(_WIN32) || defined(__CYGWIN__)
  } else if VUNLIKELY (impl_->handle != nullptr) {
    ::CloseHandle(impl_->handle);
    impl_->handle = nullptr;
#endif
  }
}

bool SysSharemem::create(const std::string& name, size_t size, Mode mode) {
  if VUNLIKELY (is_attached()) {
    VLOG_E("SysSharemem: Already attached.");
    return false;
  }

#if defined(_WIN32) || defined(__CYGWIN__)
  DWORD high = 0;
  DWORD low = 0;

  if constexpr (sizeof(size_t) == 8) {
    high = static_cast<DWORD>(static_cast<uint64_t>(size) >> 32);
  }

  low = static_cast<DWORD>(static_cast<size_t>(size) & 0xFFFFFFFF);  // NOLINT(readability-redundant-casting)
  impl_->handle = ::CreateFileMapping(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, high, low, name.c_str());

  if VUNLIKELY (!impl_->handle) {
    VLOG_E("SysSharemem: CreateFileMapping failed.");
    return false;
  }

  if VUNLIKELY (::GetLastError() == ERROR_ALREADY_EXISTS) {
    ::CloseHandle(impl_->handle);
    impl_->handle = nullptr;
    VLOG_E("SysSharemem: Shared memory already exists.");
    return false;
  }

  return attach(name, mode);
#elif defined(__ANDROID__)
  (void)name;
  (void)size;
  (void)mode;

  VLOG_E("SysSharemem: shm_open is not supported on this platform.");

  return false;
#else

  int fd = ::shm_open(name.c_str(), O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC, 0600);

  if VUNLIKELY (fd == -1) {
    return false;
  }

  int ret = ::ftruncate(fd, size);

  if VUNLIKELY (ret == -1) {
    ::close(fd);
    ::shm_unlink(name.c_str());
    VLOG_E("SysSharemem: ftruncate failed.");
    return false;
  }

  ::close(fd);

  if VUNLIKELY (!attach(name, mode)) {
    ::shm_unlink(name.c_str());
    return false;
  }

  return true;
#endif
}

bool SysSharemem::attach(const std::string& name, Mode mode) {
  if VUNLIKELY (is_attached()) {
    VLOG_E("SysSharemem: Already attached.");
    return false;
  }

#if defined(_WIN32) || defined(__CYGWIN__)
  const DWORD access = (mode == kReadOnly ? FILE_MAP_READ : FILE_MAP_ALL_ACCESS);

  if (!impl_->handle) {
    // NOLINTNEXTLINE(readability-implicit-bool-conversion)
    impl_->handle = ::OpenFileMapping(access, false, name.c_str());

    if (!impl_->handle) {
      return false;
    }
  }

  impl_->data = ::MapViewOfFile(impl_->handle, access, 0, 0, 0);

  if VUNLIKELY (!impl_->data) {
    VLOG_E("SysSharemem: MapViewOfFile failed.");
    ::CloseHandle(impl_->handle);
    impl_->handle = nullptr;

    return false;
  }

  MEMORY_BASIC_INFORMATION info;

  if VUNLIKELY (!::VirtualQuery(impl_->data, &info, sizeof(info))) {
    VLOG_E("SysSharemem: VirtualQuery failed.");
    ::UnmapViewOfFile(impl_->data);
    impl_->data = nullptr;
    ::CloseHandle(impl_->handle);
    impl_->handle = nullptr;

    return false;
  }

  impl_->size = static_cast<size_t>(info.RegionSize);
  impl_->name = name;
  impl_->mode = mode;

  return true;
#elif defined(__ANDROID__)
  (void)name;
  (void)mode;
  VLOG_E("SysSharemem: shm_open is not supported on this platform.");

  return false;
#else
  // ::shm_unlink(impl_->name.c_str());  // clear
  const int oflag = (mode == kReadOnly ? O_RDONLY : O_RDWR);
  const mode_t omode = (mode == kReadOnly ? 0400 : 0600);

  impl_->handle = ::shm_open(name.c_str(), oflag | O_CLOEXEC, omode);

  if VUNLIKELY (impl_->handle == -1) {
    return false;
  }

  struct stat st;

  if VUNLIKELY (::fstat(impl_->handle, &st) == -1) {
    VLOG_E("SysSharemem: fstat failed.");
    ::close(impl_->handle);
    impl_->handle = -1;
    impl_->size = 0;
    return false;
  }

  impl_->size = static_cast<size_t>(st.st_size);

  const int mprot = (mode == kReadOnly ? PROT_READ : PROT_READ | PROT_WRITE);
  impl_->data = ::mmap(nullptr, impl_->size, mprot, MAP_SHARED, impl_->handle, 0);

  if VUNLIKELY (impl_->data == MAP_FAILED || !impl_->data) {
    VLOG_E("SysSharemem: mmap failed.");

    impl_->data = nullptr;
    impl_->size = 0;
    ::close(impl_->handle);
    impl_->handle = -1;

    return false;
  }

#ifdef F_ADD_SEALS
  ::fcntl(impl_->handle, F_ADD_SEALS, F_SEAL_SHRINK);
#endif

  impl_->name = name;
  impl_->mode = mode;
  return true;
#endif
}

bool SysSharemem::detach(bool force) {
  if VUNLIKELY (!is_attached()) {
    VLOG_E("SysSharemem: Not attached.");
    return false;
  }

#if defined(_WIN32) || defined(__CYGWIN__)
  (void)force;
  bool ok = true;

  if VUNLIKELY (!::UnmapViewOfFile(impl_->data)) {
    VLOG_E("SysSharemem: UnmapViewOfFile failed.");
    ok = false;
  }

  impl_->data = nullptr;
  impl_->size = 0;
  impl_->name.clear();
  impl_->mode = kReadWrite;

  // NOLINTNEXTLINE(readability-implicit-bool-conversion)

  if VUNLIKELY (!::CloseHandle(impl_->handle)) {
    VLOG_E("SysSharemem: CloseHandle failed.");
    ok = false;
  }

  impl_->handle = nullptr;

  return ok;
#elif defined(__ANDROID__)
  (void)force;
  VLOG_E("SysSharemem: shm_unlink is not supported on this platform.");

  return false;
#else
  bool ok = true;

  if VUNLIKELY (::munmap(impl_->data, size_t(impl_->size)) == -1) {
    VLOG_E("SysSharemem: munmap failed.");
    ok = false;
  }

  int shm_nattch = force ? 0 : -1;

#ifdef __QNX__
  struct stat st;

  if (::fstat(impl_->handle, &st) == 0) {
    shm_nattch = st.st_nlink - 2;
  }
#endif

  if (impl_->handle != -1) {
    ::close(impl_->handle);
    impl_->handle = -1;
  }

  if (shm_nattch == 0 && !impl_->name.empty()) {
    if VUNLIKELY (::shm_unlink(impl_->name.c_str()) == -1 && errno != ENOENT) {
      VLOG_E("SysSharemem: shm_unlink failed.");
      ok = false;
    }
  }

  impl_->data = nullptr;
  impl_->size = 0;
  impl_->name.clear();
  impl_->mode = kReadWrite;

  return ok;
#endif
}

bool SysSharemem::is_attached() const {
#if defined(_WIN32) || defined(__CYGWIN__)
  return impl_->handle != nullptr && impl_->data != nullptr && impl_->size != 0 && !impl_->name.empty();
#else
  return impl_->handle != -1 && impl_->data != nullptr && impl_->size != 0 && !impl_->name.empty();
#endif
}

void* SysSharemem::data() {
  if VUNLIKELY (!is_attached()) {
    VLOG_E("SysSharemem: Not attached.");
    return nullptr;
  }

  if VUNLIKELY (impl_->mode == kReadOnly) {
    return nullptr;
  }

  return impl_->data;
}

const void* SysSharemem::data() const {
  if VUNLIKELY (!is_attached()) {
    VLOG_E("SysSharemem: Not attached.");
    return nullptr;
  }

  return impl_->data;
}

size_t SysSharemem::size() const {
  if VUNLIKELY (!is_attached()) {
    VLOG_E("SysSharemem: Not attached.");
    return 0;
  }

  return impl_->size;
}

}  // namespace vlink
