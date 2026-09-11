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

#include "./zerocopy/fast_buffer_manager.h"

#include <array>
#include <atomic>
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <string>
#include <string_view>
#include <unordered_map>

#include "./base/sys_semaphore.h"
#include "./base/sys_sharemem.h"
#include "./base/utils.h"
#include "./base/uuid.h"

namespace vlink {

// HostBufferPlugin
class HostBufferPlugin final : public FastBufferPluginInterface {
 public:
  uint64_t get_protocol_id() const noexcept override { return 0x564C484F53540001ULL; }

  bool create(size_t size, const zerocopy::FastBuffer::Config& config,
              zerocopy::FastBuffer::Buffer& buffer) noexcept override {
    if VUNLIKELY (size == 0 || config.device != -1 ||
                  (config.memory_type != zerocopy::FastBuffer::kMemoryDefault &&
                   config.memory_type != zerocopy::FastBuffer::kMemoryHost)) {
      return false;
    }

    auto* resource = new (std::nothrow) Bytes;

    if VUNLIKELY (!resource) {
      return false;
    }

    *resource = Bytes::create(size);

    if VUNLIKELY (resource->empty()) {
      delete resource;
      return false;
    }

    buffer = {resource->data(), size, resource, -1, zerocopy::FastBuffer::kMemoryHost};
    return true;
  }

  bool release(zerocopy::FastBuffer::Buffer& buffer) noexcept override {
    delete static_cast<Bytes*>(buffer.handle);
    buffer = {};
    return true;
  }

  bool synchronize(const zerocopy::FastBuffer::Buffer&) noexcept override { return true; }

  bool copy(const zerocopy::FastBuffer::Buffer& source,
            const zerocopy::FastBuffer::Buffer& destination) noexcept override {
    if VUNLIKELY (source.size != destination.size) {
      return false;
    }

    std::memmove(destination.data, source.data, source.size);
    return true;
  }

  bool copy_to_host(const zerocopy::FastBuffer::Buffer& source, Bytes& destination) noexcept override {
    if VUNLIKELY (source.size != destination.size()) {
      return false;
    }

    std::memmove(destination.data(), source.data, source.size);
    return true;
  }

  bool copy_from_host(const Bytes& source, const zerocopy::FastBuffer::Buffer& destination) noexcept override {
    if VUNLIKELY (source.size() != destination.size) {
      return false;
    }

    std::memmove(destination.data, source.data(), source.size());
    return true;
  }

  uint8_t* map(const zerocopy::FastBuffer::Buffer& buffer, zerocopy::FastBuffer::Access access) noexcept override {
    return access == zerocopy::FastBuffer::kRead || access == zerocopy::FastBuffer::kWrite ||
                   access == zerocopy::FastBuffer::kReadWrite
               ? buffer.data
               : nullptr;
  }

  bool unmap(const zerocopy::FastBuffer::Buffer&, zerocopy::FastBuffer::Access access) noexcept override {
    return access == zerocopy::FastBuffer::kRead || access == zerocopy::FastBuffer::kWrite ||
           access == zerocopy::FastBuffer::kReadWrite;
  }

  const void* native_handle(const zerocopy::FastBuffer::Buffer& buffer) const noexcept override {
    return buffer.handle;
  }
};

static constexpr size_t kFastBufferHandleSize = 64;

// FastBufferControl
struct alignas(8) FastBufferControl final {
  uint64_t protocol{0};
  uint64_t size{0};
  uint64_t descriptor_size{0};
  std::atomic<uint32_t> references{1};
};

// FastBufferIpcState
struct FastBufferIpcState final {
  ~FastBufferIpcState() {
    if (owner) {
      if (shared.is_attached()) {
        shared.detach();
      }

      if (done.is_attached()) {
        done.detach();
      }
    }
  }

  void drop() {
    if (control->references.fetch_sub(1, std::memory_order_acq_rel) == 1) {
      done.release();
    }
  }

  char name[kFastBufferHandleSize]{};
  SysSharemem shared;
  SysSemaphore done;
  FastBufferControl* control{nullptr};
  bool owner{false};
};

// FastBufferResource
struct FastBufferResource final {
  zerocopy::FastBuffer::Buffer native;
  std::atomic<size_t> references{1};
  std::mutex mutex;
  std::unique_ptr<FastBufferIpcState> ipc;
};

static_assert(std::atomic<uint32_t>::is_always_lock_free, "IPC references require lock-free atomics.");

// FastBufferManager::Impl
struct FastBufferManager::Impl final {
  Plugin plugin;
  std::shared_ptr<FastBufferPluginInterface> interface;
  std::mutex mutex;
  std::unordered_map<std::string, FastBufferResource*> resources;
  std::array<std::mutex, 64> ipc_mutexes;
};

// FastBufferManager
FastBufferManager& FastBufferManager::get() {
  static FastBufferManager global_fast_buffer;
  return global_fast_buffer;
}

bool FastBufferManager::is_valid() const { return impl_->interface != nullptr; }

FastBufferPluginInterface* FastBufferManager::get_interface() const { return impl_->interface.get(); }

const zerocopy::FastBuffer::Buffer& FastBufferManager::native_buffer(
    const zerocopy::FastBuffer::Buffer& buffer) noexcept {
  return static_cast<const FastBufferResource*>(buffer.handle)->native;
}

bool FastBufferManager::create(size_t size, const zerocopy::FastBuffer::Config& config,
                               zerocopy::FastBuffer::Buffer& buffer) noexcept {
  auto resource = std::make_unique<FastBufferResource>();

  if VUNLIKELY (!impl_->interface->create(size, config, resource->native)) {
    return false;
  }

  buffer = resource->native;
  buffer.handle = resource.release();
  return true;
}

void FastBufferManager::retain(const zerocopy::FastBuffer::Buffer& source,
                               zerocopy::FastBuffer::Buffer& buffer) noexcept {
  auto* resource = static_cast<FastBufferResource*>(source.handle);
  resource->references.fetch_add(1, std::memory_order_relaxed);
  buffer = source;
}

void FastBufferManager::release(zerocopy::FastBuffer::Buffer& buffer) noexcept {
  auto* resource = static_cast<FastBufferResource*>(buffer.handle);
  buffer = {};
  size_t references = resource->references.load(std::memory_order_acquire);

  while (references > 1) {
    if (resource->references.compare_exchange_weak(references, references - 1, std::memory_order_acq_rel,
                                                   std::memory_order_acquire)) {
      return;
    }
  }

  auto* ipc = resource->ipc.get();
  std::unique_lock ipc_lock(
      impl_->ipc_mutexes[ipc ? std::hash<std::string_view>{}(ipc->name) % impl_->ipc_mutexes.size() : 0],
      std::defer_lock);
  std::unique_lock lock(impl_->mutex, std::defer_lock);

  if (ipc) {
    if (!ipc->owner) {
      ipc_lock.lock();
    }

    lock.lock();
  }

  if (resource->references.fetch_sub(1, std::memory_order_acq_rel) != 1) {
    return;
  }

  if (ipc) {
    impl_->resources.erase(ipc->name);
    lock.unlock();
  }

  if VUNLIKELY (!impl_->interface->synchronize(resource->native)) {
    VLOG_E("FastBufferManager: Keeping resource and remote reference after synchronization failure.");
    return;
  }

  if (ipc && ipc->owner) {
    if (ipc->control->references.fetch_sub(1, std::memory_order_acq_rel) != 1 && !ipc->done.acquire()) {
      VLOG_E("FastBufferManager: Keeping allocation alive after import wait failure.");
      return;
    }
  }

  if VUNLIKELY (!impl_->interface->release(resource->native)) {
    VLOG_E("FastBufferManager: Keeping resource and remote reference after SDK release failure.");
    return;
  }

  if (ipc && !ipc->owner) {
    ipc->drop();
  }

  delete resource;
}

bool FastBufferManager::import_native(const void* handle, zerocopy::FastBuffer::Buffer& buffer) noexcept {
  auto resource = std::make_unique<FastBufferResource>();

  if VUNLIKELY (!impl_->interface->import_native(handle, resource->native)) {
    return false;
  }

  buffer = resource->native;
  buffer.handle = resource.release();
  return true;
}

size_t FastBufferManager::get_handle_size(const zerocopy::FastBuffer::Buffer& buffer) const noexcept {
  const size_t size = impl_->interface->get_handle_size(native_buffer(buffer));

  if (impl_->interface->get_sharing_mode() == FastBufferPluginInterface::kCustom) {
    return size;
  }

#if defined(__APPLE__) || defined(__ANDROID__)
  return 0;
#else
  return size == 0 ? 0 : kFastBufferHandleSize;
#endif
}

bool FastBufferManager::export_handle(const zerocopy::FastBuffer::Buffer& buffer, Bytes& descriptor) noexcept {
  auto* resource = static_cast<FastBufferResource*>(buffer.handle);

  if VUNLIKELY (!impl_->interface->synchronize(resource->native)) {
    return false;
  }

  if (impl_->interface->get_sharing_mode() == FastBufferPluginInterface::kCustom) {
    return impl_->interface->export_handle(resource->native, descriptor);
  }

  std::lock_guard lock(resource->mutex);

  if (!resource->ipc) {
    const size_t size = impl_->interface->get_handle_size(resource->native);

    if VUNLIKELY (size == 0) {
      return false;
    }

    auto ipc = std::make_unique<FastBufferIpcState>();
    const std::string name = "/vlink_fb_" + Uuid::generate_random().to_string();

    if VUNLIKELY (!ipc->shared.create(name, sizeof(FastBufferControl) + size)) {
      return false;
    }

    ipc->owner = true;

    if VUNLIKELY (!ipc->done.attach(name + "_done")) {
      return false;
    }

    ipc->control = new (ipc->shared.data()) FastBufferControl;
    ipc->control->protocol = impl_->interface->get_protocol_id();
    ipc->control->size = resource->native.size;
    ipc->control->descriptor_size = size;
    auto native_descriptor = Bytes::shallow_copy(reinterpret_cast<uint8_t*>(ipc->control + 1), size);

    if VUNLIKELY (!impl_->interface->export_handle(resource->native, native_descriptor)) {
      return false;
    }

    std::memcpy(ipc->name, name.c_str(), name.size() + 1);
    resource->ipc = std::move(ipc);
    std::lock_guard registry_lock(impl_->mutex);
    impl_->resources.emplace(name, resource);
  }

  std::memcpy(descriptor.data(), resource->ipc->name, kFastBufferHandleSize);
  return true;
}

bool FastBufferManager::import_handle(const Bytes& descriptor, zerocopy::FastBuffer::Buffer& buffer) noexcept {
  if (impl_->interface->get_sharing_mode() == FastBufferPluginInterface::kCustom) {
    auto resource = std::make_unique<FastBufferResource>();

    if VUNLIKELY (!impl_->interface->import_handle(descriptor, resource->native)) {
      return false;
    }

    buffer = resource->native;
    buffer.handle = resource.release();
    return true;
  }

#if defined(__APPLE__) || defined(__ANDROID__)
  (void)descriptor;
  (void)buffer;
  return false;
#else
  if VUNLIKELY (descriptor.size() != kFastBufferHandleSize) {
    return false;
  }

  char name[kFastBufferHandleSize];
  std::memcpy(name, descriptor.data(), sizeof(name));

  if VUNLIKELY (std::strncmp(name, "/vlink_fb_", 10) != 0 || std::memchr(name, '\0', sizeof(name)) == nullptr ||
                std::strchr(name + 1, '/') != nullptr) {
    return false;
  }

  std::unique_lock lock(impl_->mutex);
  auto iter = impl_->resources.find(name);

  if (iter != impl_->resources.end()) {
    auto* resource = iter->second;
    resource->references.fetch_add(1, std::memory_order_relaxed);
    buffer = resource->native;
    buffer.handle = resource;
    return true;
  }

  lock.unlock();
  std::lock_guard ipc_lock(impl_->ipc_mutexes[std::hash<std::string_view>{}(name) % impl_->ipc_mutexes.size()]);
  lock.lock();
  iter = impl_->resources.find(name);

  if (iter != impl_->resources.end()) {
    auto* resource = iter->second;
    resource->references.fetch_add(1, std::memory_order_relaxed);
    buffer = resource->native;
    buffer.handle = resource;
    return true;
  }

  lock.unlock();
  auto resource = std::make_unique<FastBufferResource>();
  resource->ipc = std::make_unique<FastBufferIpcState>();
  auto& ipc = *resource->ipc;

  if VUNLIKELY (!ipc.shared.attach(name) || ipc.shared.size() <= sizeof(FastBufferControl)) {
    return false;
  }

  ipc.control = static_cast<FastBufferControl*>(ipc.shared.data());
  const auto& control = *ipc.control;

  if VUNLIKELY (control.protocol != impl_->interface->get_protocol_id() || control.size == 0 ||
                control.descriptor_size == 0 ||
                control.descriptor_size > ipc.shared.size() - sizeof(FastBufferControl) ||
                !ipc.done.attach(std::string(name) + "_done")) {
    return false;
  }

  uint32_t references = ipc.control->references.load(std::memory_order_acquire);

  do {
    if VUNLIKELY (references == 0) {
      ipc.done.detach();
      return false;
    }

    if VUNLIKELY (references == std::numeric_limits<uint32_t>::max()) {
      return false;
    }
  } while (!ipc.control->references.compare_exchange_weak(references, references + 1, std::memory_order_acq_rel));

  const auto native_descriptor = Bytes::shallow_copy(reinterpret_cast<const uint8_t*>(ipc.control + 1),
                                                     static_cast<size_t>(control.descriptor_size));

  if VUNLIKELY (!impl_->interface->import_handle(native_descriptor, resource->native)) {
    ipc.drop();
    return false;
  }

  if VUNLIKELY (resource->native.size != control.size) {
    if (impl_->interface->release(resource->native)) {
      ipc.drop();
    } else {
      VLOG_E("FastBufferManager: Keeping rejected import after SDK release failure.");
      (void)resource.release();
    }

    return false;
  }

  std::memcpy(ipc.name, name, sizeof(name));
  lock.lock();
  impl_->resources.emplace(name, resource.get());
  buffer = resource->native;
  buffer.handle = resource.release();
  return true;
#endif
}

FastBufferManager::FastBufferManager() : impl_(std::make_unique<Impl>()) {
  const std::string plugin_name = Utils::get_env("VLINK_FASTBUFFER_PLUGIN");

  if (plugin_name.empty()) {
    impl_->interface = std::make_shared<HostBufferPlugin>();
    return;
  }

  impl_->interface = impl_->plugin.load<FastBufferPluginInterface>(plugin_name, 1, 0);

  if VUNLIKELY (impl_->interface && impl_->interface->get_protocol_id() == 0) {
    VLOG_E("FastBufferManager: Plugin descriptor protocol ID must not be zero.");
    impl_->interface.reset();
  }
}

FastBufferManager::~FastBufferManager() {
#ifdef _WIN32
  if (Utils::is_terminating()) {
    (void)impl_.release();
    return;
  }
#endif

  impl_->interface.reset();
  impl_->plugin.clear();
}

}  // namespace vlink
