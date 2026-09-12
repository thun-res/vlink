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
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>

#include "./base/memory_pool.h"
#include "./base/sys_sharemem.h"
#include "./base/utils.h"
#include "./base/uuid.h"

#if defined(__linux__)
#include <unistd.h>
#endif

namespace vlink {

static constexpr size_t kFastBufferNameSize = 32;
static constexpr size_t kFastBufferMaxReaders = 32;
static constexpr std::string_view kFastBufferControlPrefix = "/vfb_";
static constexpr std::string_view kFastBufferMemoryPrefix = "/vfbm_";

template <typename T, typename... ArgsT>
static T* pool_new(ArgsT&&... args) noexcept {
  void* memory = MemoryPool::global_instance().allocate(sizeof(T), alignof(T));
  return memory ? new (memory) T(std::forward<ArgsT>(args)...) : nullptr;
}

template <typename T>
static void pool_delete(T* object) noexcept {
  if (object) {
    object->~T();
    MemoryPool::global_instance().deallocate(object, sizeof(T), alignof(T));
  }
}

// PoolDeleter
struct PoolDeleter final {
  template <typename T>
  void operator()(T* object) const noexcept {
    pool_delete(object);
  }
};

template <typename T>
using PoolPtr = std::unique_ptr<T, PoolDeleter>;

// PoolAllocator
template <typename T>
struct PoolAllocator final {
  using value_type = T;

  PoolAllocator() noexcept = default;

  template <typename U>
  PoolAllocator(const PoolAllocator<U>&) noexcept {}  // NOLINT(google-explicit-constructor)

  [[nodiscard]] T* allocate(size_t count) {
    void* memory = MemoryPool::global_instance().allocate(count * sizeof(T), alignof(T));

    if VUNLIKELY (!memory) {
      throw std::bad_alloc();
    }

    return static_cast<T*>(memory);
  }

  void deallocate(T* memory, size_t count) noexcept {
    MemoryPool::global_instance().deallocate(static_cast<void*>(memory), count * sizeof(T), alignof(T));
  }

  template <typename U>
  bool operator==(const PoolAllocator<U>&) const noexcept {
    return true;
  }

  template <typename U>
  bool operator!=(const PoolAllocator<U>&) const noexcept {
    return false;
  }
};

static uint64_t generate_id() noexcept {
  const Uuid uuid = Uuid::generate_random();
  uint64_t id = 0;
  std::memcpy(&id, uuid.bytes().data(), sizeof(id));
  return id;
}

static void format_name(std::string_view prefix, uint64_t id, char (&name)[kFastBufferNameSize]) noexcept {
  static constexpr char kHexDigits[] = "0123456789abcdef";
  char* cursor = name;

  std::memcpy(cursor, prefix.data(), prefix.size());
  cursor += prefix.size();

  for (int shift = 60; shift >= 0; shift -= 4) {
    *cursor++ = kHexDigits[(id >> shift) & 0x0F];
  }

  *cursor = '\0';
}

static bool parse_name(const char* name, std::string_view prefix, uint64_t& id) noexcept {
  if (std::memchr(name, '\0', kFastBufferNameSize) == nullptr || std::strlen(name) != prefix.size() + 16 ||
      std::strncmp(name, prefix.data(), prefix.size()) != 0) {
    return false;
  }

  uint64_t parsed = 0;

  for (const char* cursor = name + prefix.size(); *cursor != '\0'; ++cursor) {
    const char digit = *cursor;
    uint64_t value = 0;

    if (digit >= '0' && digit <= '9') {
      value = static_cast<uint64_t>(digit - '0');
    } else if (digit >= 'a' && digit <= 'f') {
      value = static_cast<uint64_t>(digit - 'a') + 10;
    } else {
      return false;
    }

    parsed = (parsed << 4) | value;
  }

  id = parsed;
  return true;
}

static uint32_t fold_start_time(uint64_t start_time) noexcept {
  return static_cast<uint32_t>(start_time ^ (start_time >> 32));
}

static uint64_t process_identity(int32_t pid) noexcept {
  const uint64_t start_time = Utils::get_process_start_time(pid);

  if (start_time == 0 || start_time == Utils::kUnknownProcessStartTime) {
    return 0;
  }

  return (static_cast<uint64_t>(static_cast<uint32_t>(pid)) << 32) | fold_start_time(start_time);
}

static uint64_t namespace_inode(const char* path) noexcept {
#if defined(__linux__)
  char link[64];
  const ssize_t length = ::readlink(path, link, sizeof(link) - 1);

  if (length <= 0) {
    return 0;
  }

  link[length] = '\0';
  const char* open = std::strchr(link, '[');
  return open ? std::strtoull(open + 1, nullptr, 10) : 0;
#else
  return 0;
#endif
}

static bool is_identity_alive(uint64_t identity) noexcept {
  const uint64_t start_time = Utils::get_process_start_time(static_cast<int32_t>(identity >> 32));

  if (start_time == 0) {
    return false;
  }

  return start_time == Utils::kUnknownProcessStartTime ||
         fold_start_time(start_time) == static_cast<uint32_t>(identity);
}

template <typename FnT>
static void sweep_segments(std::string_view prefix, FnT&& fn) noexcept {
#if defined(__linux__) || defined(__QNX__)
#if defined(__QNX__)
  static constexpr const char* kDirectory = "/dev/shmem";
#else
  static constexpr const char* kDirectory = "/dev/shm";
#endif
  std::error_code ec;
  std::filesystem::directory_iterator iter(kDirectory, ec);

  for (; !ec && iter != std::filesystem::directory_iterator(); iter.increment(ec)) {
    const std::string filename = iter->path().filename().string();
    char name[kFastBufferNameSize];
    uint64_t id = 0;

    if (filename.size() >= kFastBufferNameSize - 1) {
      continue;
    }

    name[0] = '/';
    std::memcpy(name + 1, filename.c_str(), filename.size() + 1);

    if (!parse_name(name, prefix, id)) {
      continue;
    }

    SysSharemem shared;

    if (shared.attach(name)) {
      fn(shared);
    }
  }
#else
  (void)prefix;
  (void)fn;
#endif
}

// HostBufferPlugin
class HostBufferPlugin : public FastBufferPluginInterface {
 public:
  uint64_t get_protocol_id() const noexcept override { return 0x564C484F53540001ULL; }

  bool create(size_t size, const zerocopy::FastBuffer::Config& config,
              zerocopy::FastBuffer::Buffer& buffer) noexcept override {
    if VUNLIKELY (size == 0 || config.device != -1 ||
                  (config.memory_type != zerocopy::FastBuffer::kMemoryDefault &&
                   config.memory_type != zerocopy::FastBuffer::kMemoryHost)) {
      return false;
    }

    auto* resource = pool_new<Bytes>();

    if VUNLIKELY (!resource) {
      return false;
    }

    *resource = Bytes::create(size);

    if VUNLIKELY (resource->empty()) {
      pool_delete(resource);
      return false;
    }

    buffer = {resource->data(), size, resource, -1, zerocopy::FastBuffer::kMemoryHost};
    return true;
  }

  bool release(zerocopy::FastBuffer::Buffer& buffer) noexcept override {
    pool_delete(static_cast<Bytes*>(buffer.handle));
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

// SharedHostBufferPlugin
class SharedHostBufferPlugin final : public HostBufferPlugin {
 public:
  SharedHostBufferPlugin(uint64_t identity, uint64_t pid_namespace, uint64_t time_namespace) noexcept
      : identity_(identity), pid_namespace_(pid_namespace), time_namespace_(time_namespace) {
    sweep_segments(kFastBufferMemoryPrefix, [this](SysSharemem& shared) {
      if (shared.size() < kSegmentHeaderSize) {
        return;
      }

      const auto* header = static_cast<const std::atomic<uint64_t>*>(shared.data());
      const uint64_t owner = header[0].load(std::memory_order_acquire);

      if (owner != 0 && header[1].load(std::memory_order_relaxed) == pid_namespace_ &&
          header[2].load(std::memory_order_relaxed) == time_namespace_ && !is_identity_alive(owner)) {
        (void)shared.detach();
      }
    });
  }

  uint64_t get_protocol_id() const noexcept override { return 0x564C5348534D0001ULL; }

  bool create(size_t size, const zerocopy::FastBuffer::Config& config,
              zerocopy::FastBuffer::Buffer& buffer) noexcept override {
    if VUNLIKELY (size == 0 || size > std::numeric_limits<size_t>::max() - kSegmentHeaderSize || config.device != -1 ||
                  (config.memory_type != zerocopy::FastBuffer::kMemoryDefault &&
                   config.memory_type != zerocopy::FastBuffer::kMemoryShared)) {
      return false;
    }

    auto* segment = pool_new<Segment>();

    if VUNLIKELY (!segment) {
      return false;
    }

    format_name(kFastBufferMemoryPrefix, generate_id(), segment->name);

    if VUNLIKELY (!segment->shared.create(segment->name, kSegmentHeaderSize + size)) {
      pool_delete(segment);
      return false;
    }

    segment->owner = true;
    auto* base = static_cast<uint8_t*>(segment->shared.data());
    auto* header = new (base) std::atomic<uint64_t>[3]{};
    header[1].store(pid_namespace_, std::memory_order_relaxed);
    header[2].store(time_namespace_, std::memory_order_relaxed);
    header[0].store(identity_, std::memory_order_release);
    buffer = {base + kSegmentHeaderSize, size, segment, -1, zerocopy::FastBuffer::kMemoryShared};
    return true;
  }

  bool release(zerocopy::FastBuffer::Buffer& buffer) noexcept override {
    auto* segment = static_cast<Segment*>(buffer.handle);
    (void)segment->shared.detach(segment->owner);
    pool_delete(segment);
    buffer = {};
    return true;
  }

  size_t get_handle_size(const zerocopy::FastBuffer::Buffer&) const noexcept override { return sizeof(Descriptor); }

  bool export_handle(const zerocopy::FastBuffer::Buffer& buffer, Bytes& descriptor) noexcept override {
    if VUNLIKELY (descriptor.size() != sizeof(Descriptor)) {
      return false;
    }

    Descriptor exported;
    std::memcpy(exported.name, static_cast<const Segment*>(buffer.handle)->name, sizeof(exported.name));
    exported.size = buffer.size;
    std::memcpy(descriptor.data(), &exported, sizeof(exported));
    return true;
  }

  bool import_handle(const Bytes& descriptor, zerocopy::FastBuffer::Buffer& buffer) noexcept override {
    Descriptor decoded;

    if VUNLIKELY (!decode(descriptor, decoded) || decoded.size == 0) {
      return false;
    }

    auto* segment = pool_new<Segment>();

    if VUNLIKELY (!segment) {
      return false;
    }

    if VUNLIKELY (!segment->shared.attach(decoded.name) || segment->shared.size() < kSegmentHeaderSize ||
                  segment->shared.size() - kSegmentHeaderSize < decoded.size) {
      pool_delete(segment);
      return false;
    }

    std::memcpy(segment->name, decoded.name, sizeof(segment->name));
    buffer = {static_cast<uint8_t*>(segment->shared.data()) + kSegmentHeaderSize, static_cast<size_t>(decoded.size),
              segment, -1, zerocopy::FastBuffer::kMemoryShared};
    return true;
  }

  bool destroy_handle(const Bytes& descriptor) noexcept override {
    Descriptor decoded;

    if VUNLIKELY (!decode(descriptor, decoded)) {
      return false;
    }

    SysSharemem shared;
    return shared.attach(decoded.name) && shared.detach();
  }

 private:
  static constexpr size_t kSegmentHeaderSize = 64;

  struct Segment final {
    char name[kFastBufferNameSize]{};
    SysSharemem shared;
    bool owner{false};
  };

  struct Descriptor final {
    char name[kFastBufferNameSize]{};
    uint64_t size{0};
  };

  static bool decode(const Bytes& descriptor, Descriptor& decoded) noexcept {
    if VUNLIKELY (descriptor.size() != sizeof(Descriptor)) {
      return false;
    }

    std::memcpy(&decoded, descriptor.data(), sizeof(decoded));
    uint64_t id = 0;
    return parse_name(decoded.name, kFastBufferMemoryPrefix, id);
  }

  uint64_t identity_{0};
  uint64_t pid_namespace_{0};
  uint64_t time_namespace_{0};
};

// FastBufferControl
struct alignas(8) FastBufferControl final {
  std::atomic<uint64_t> protocol{0};
  uint64_t size{0};
  uint64_t descriptor_size{0};
  std::atomic<uint64_t> owner{0};
  std::atomic<uint64_t> pid_namespace{0};
  std::atomic<uint64_t> time_namespace{0};
  std::atomic<uint64_t> generation{0};
  std::atomic<uint64_t> readers[kFastBufferMaxReaders]{};
};

// FastBufferSystemDescriptor
struct FastBufferSystemDescriptor final {
  char name[kFastBufferNameSize]{};
  uint64_t generation{0};
};

static constexpr size_t kFastBufferHandleSize = sizeof(FastBufferSystemDescriptor);

static_assert(std::atomic<uint64_t>::is_always_lock_free, "Shared control blocks require lock-free atomics.");

// FastBufferShare
struct FastBufferShare final {
  ~FastBufferShare() {
    if (owner && shared.is_attached()) {
      (void)shared.detach();
    }
  }

  uint64_t id{0};
  SysSharemem shared;
  FastBufferControl* control{nullptr};
  size_t reader{0};
  bool owner{false};
};

// FastBufferResource
struct FastBufferResource final {
  ~FastBufferResource() { pool_delete(share.load(std::memory_order_acquire)); }

  zerocopy::FastBuffer::Buffer native;
  std::atomic<size_t> references{1};
  std::atomic<FastBufferShare*> share{nullptr};
  FastBufferResource* next{nullptr};
};

enum class LocalImport : uint8_t {
  kMissing = 0,
  kImported = 1,
  kExpired = 2,
};

// FastBufferManager::Impl
struct FastBufferManager::Impl final {
  [[nodiscard]] std::mutex& stripe_of(uint64_t id) noexcept { return stripes[id % stripes.size()]; }

  void free_resource(FastBufferResource* resource) noexcept;

  void retire_owner(FastBufferResource* resource) noexcept;

  void reclaim_retired() noexcept;

  [[nodiscard]] LocalImport import_local(uint64_t id, uint64_t generation,
                                         zerocopy::FastBuffer::Buffer& buffer) noexcept;

  void destroy_dead_owner(const FastBufferControl& control, SysSharemem& shared) noexcept;

  Plugin plugin;
  std::shared_ptr<FastBufferPluginInterface> interface;
  uint64_t identity{0};
  uint64_t pid_namespace{0};
  uint64_t time_namespace{0};
  std::mutex mutex;
  std::unordered_map<uint64_t, FastBufferResource*, std::hash<uint64_t>, std::equal_to<>,
                     PoolAllocator<std::pair<const uint64_t, FastBufferResource*>>>
      resources;
  std::atomic<FastBufferResource*> retired{nullptr};
  std::array<std::mutex, 64> stripes;
};

static bool purge_readers(FastBufferControl& control) noexcept {
  for (auto& reader : control.readers) {
    uint64_t identity = reader.load(std::memory_order_seq_cst);

    while (identity != 0) {
      if (is_identity_alive(identity)) {
        return false;
      }

      if (reader.compare_exchange_strong(identity, 0, std::memory_order_seq_cst)) {
        break;
      }
    }
  }

  return true;
}

void FastBufferManager::Impl::free_resource(FastBufferResource* resource) noexcept {
  if VUNLIKELY (!interface->release(resource->native)) {
    VLOG_E("FastBufferManager: Keeping resource after SDK release failure.");
    return;
  }

  pool_delete(resource);
}

void FastBufferManager::Impl::retire_owner(FastBufferResource* resource) noexcept {
  if (purge_readers(*resource->share.load(std::memory_order_acquire)->control)) {
    free_resource(resource);
    return;
  }

  std::lock_guard lock(mutex);
  resource->next = retired.load(std::memory_order_relaxed);
  retired.store(resource, std::memory_order_relaxed);
}

void FastBufferManager::Impl::reclaim_retired() noexcept {
  if VLIKELY (retired.load(std::memory_order_relaxed) == nullptr) {
    return;
  }

  FastBufferResource* pending = nullptr;

  {
    std::lock_guard lock(mutex);
    pending = retired.exchange(nullptr, std::memory_order_relaxed);
  }

  FastBufferResource* busy = nullptr;

  while (pending) {
    auto* resource = pending;
    pending = resource->next;
    resource->next = nullptr;

    if (purge_readers(*resource->share.load(std::memory_order_acquire)->control)) {
      free_resource(resource);
    } else {
      resource->next = busy;
      busy = resource;
    }
  }

  if (busy) {
    auto* tail = busy;

    while (tail->next) {
      tail = tail->next;
    }

    std::lock_guard lock(mutex);
    tail->next = retired.load(std::memory_order_relaxed);
    retired.store(busy, std::memory_order_relaxed);
  }
}

LocalImport FastBufferManager::Impl::import_local(uint64_t id, uint64_t generation,
                                                  zerocopy::FastBuffer::Buffer& buffer) noexcept {
  std::lock_guard lock(mutex);
  const auto iter = resources.find(id);

  if (iter == resources.end()) {
    return LocalImport::kMissing;
  }

  auto* resource = iter->second;
  const auto* share = resource->share.load(std::memory_order_acquire);

  if VUNLIKELY (share->control->generation.load(std::memory_order_acquire) != generation) {
    return LocalImport::kExpired;
  }

  resource->references.fetch_add(1, std::memory_order_relaxed);
  buffer = resource->native;
  buffer.handle = resource;
  return LocalImport::kImported;
}

void FastBufferManager::Impl::destroy_dead_owner(const FastBufferControl& control, SysSharemem& shared) noexcept {
  if (control.descriptor_size != 0 && control.descriptor_size <= shared.size() - sizeof(FastBufferControl)) {
    (void)interface->destroy_handle(Bytes::shallow_copy(reinterpret_cast<const uint8_t*>(&control + 1),
                                                        static_cast<size_t>(control.descriptor_size)));
  }

  (void)shared.detach();
}

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
  impl_->reclaim_retired();

  PoolPtr<FastBufferResource> resource(pool_new<FastBufferResource>());

  if VUNLIKELY (!resource || !impl_->interface->create(size, config, resource->native)) {
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

  impl_->reclaim_retired();

  auto* share = resource->share.load(std::memory_order_acquire);
  std::unique_lock stripe(share ? impl_->stripe_of(share->id) : impl_->stripes[0], std::defer_lock);
  std::unique_lock registry(impl_->mutex, std::defer_lock);

  if (share) {
    if (!share->owner) {
      stripe.lock();
    }

    registry.lock();
  }

  if (resource->references.fetch_sub(1, std::memory_order_acq_rel) != 1) {
    return;
  }

  if (share) {
    impl_->resources.erase(share->id);

    if (share->owner) {
      share->control->generation.fetch_add(1, std::memory_order_seq_cst);
    }

    registry.unlock();
  }

  if VUNLIKELY (!impl_->interface->synchronize(resource->native)) {
    VLOG_E("FastBufferManager: Keeping resource after synchronization failure.");
    return;
  }

  if (share && share->owner) {
    impl_->retire_owner(resource);
    return;
  }

  if VUNLIKELY (!impl_->interface->release(resource->native)) {
    VLOG_E("FastBufferManager: Keeping resource after SDK release failure.");
    return;
  }

  if (share) {
    share->control->readers[share->reader].store(0, std::memory_order_release);
  }

  pool_delete(resource);
}

bool FastBufferManager::reclaim(const zerocopy::FastBuffer::Buffer& buffer) noexcept {
  auto* resource = static_cast<FastBufferResource*>(buffer.handle);

  if (resource->references.load(std::memory_order_acquire) != 1) {
    return false;
  }

  const auto* share = resource->share.load(std::memory_order_acquire);

  if (!share) {
    return true;
  }

  auto& control = *share->control;

  if (!purge_readers(control)) {
    return false;
  }

  {
    std::lock_guard lock(impl_->mutex);

    if (resource->references.load(std::memory_order_relaxed) != 1) {
      return false;
    }

    control.generation.fetch_add(1, std::memory_order_seq_cst);
  }

  for (const auto& reader : control.readers) {
    if (reader.load(std::memory_order_seq_cst) != 0) {
      return false;
    }
  }

  return true;
}

bool FastBufferManager::import_native(const void* handle, zerocopy::FastBuffer::Buffer& buffer) noexcept {
  PoolPtr<FastBufferResource> resource(pool_new<FastBufferResource>());

  if VUNLIKELY (!resource || !impl_->interface->import_native(handle, resource->native)) {
    return false;
  }

  buffer = resource->native;
  buffer.handle = resource.release();
  return true;
}

size_t FastBufferManager::get_handle_size(const zerocopy::FastBuffer::Buffer& buffer) const noexcept {
#if defined(__ANDROID__)
  (void)buffer;
  return 0;
#else
  const size_t size = impl_->interface->get_handle_size(native_buffer(buffer));
  return size == 0 || impl_->identity == 0 ? 0 : kFastBufferHandleSize;
#endif
}

bool FastBufferManager::export_handle(const zerocopy::FastBuffer::Buffer& buffer, Bytes& descriptor) noexcept {
  auto* resource = static_cast<FastBufferResource*>(buffer.handle);

  if VUNLIKELY (!impl_->interface->synchronize(resource->native)) {
    return false;
  }

  auto* share = resource->share.load(std::memory_order_acquire);

  if (!share) {
    std::lock_guard stripe(impl_->stripes[std::hash<const void*>{}(resource) % impl_->stripes.size()]);
    share = resource->share.load(std::memory_order_acquire);

    if (!share) {
      const size_t size = impl_->interface->get_handle_size(resource->native);

      if VUNLIKELY (size == 0) {
        return false;
      }

      PoolPtr<FastBufferShare> created(pool_new<FastBufferShare>());

      if VUNLIKELY (!created) {
        return false;
      }

      created->id = generate_id();
      char name[kFastBufferNameSize];
      format_name(kFastBufferControlPrefix, created->id, name);

      if VUNLIKELY (!created->shared.create(name, sizeof(FastBufferControl) + size)) {
        return false;
      }

      created->owner = true;
      created->control = new (created->shared.data()) FastBufferControl;
      created->control->protocol.store(impl_->interface->get_protocol_id(), std::memory_order_relaxed);
      created->control->size = resource->native.size;
      created->control->descriptor_size = size;
      created->control->pid_namespace.store(impl_->pid_namespace, std::memory_order_relaxed);
      created->control->time_namespace.store(impl_->time_namespace, std::memory_order_relaxed);
      created->control->generation.store(1, std::memory_order_relaxed);
      created->control->owner.store(impl_->identity, std::memory_order_release);
      auto native_descriptor = Bytes::shallow_copy(reinterpret_cast<uint8_t*>(created->control + 1), size);

      if VUNLIKELY (!impl_->interface->export_handle(resource->native, native_descriptor)) {
        return false;
      }

      share = created.release();

      {
        std::lock_guard registry(impl_->mutex);
        impl_->resources.emplace(share->id, resource);
      }

      resource->share.store(share, std::memory_order_release);
    }
  }

  FastBufferSystemDescriptor exported;
  format_name(kFastBufferControlPrefix, share->id, exported.name);
  exported.generation = share->control->generation.load(std::memory_order_acquire);
  std::memcpy(descriptor.data(), &exported, sizeof(exported));
  return true;
}

bool FastBufferManager::import_handle(const Bytes& descriptor, zerocopy::FastBuffer::Buffer& buffer) noexcept {
#if defined(__ANDROID__)
  (void)descriptor;
  (void)buffer;
  return false;
#else
  if VUNLIKELY (descriptor.size() != kFastBufferHandleSize || impl_->identity == 0) {
    return false;
  }

  FastBufferSystemDescriptor decoded;
  std::memcpy(&decoded, descriptor.data(), sizeof(decoded));
  uint64_t id = 0;

  if VUNLIKELY (!parse_name(decoded.name, kFastBufferControlPrefix, id) || decoded.generation == 0) {
    return false;
  }

  LocalImport local = impl_->import_local(id, decoded.generation, buffer);

  if (local != LocalImport::kMissing) {
    return local == LocalImport::kImported;
  }

  std::lock_guard stripe(impl_->stripe_of(id));
  local = impl_->import_local(id, decoded.generation, buffer);

  if (local != LocalImport::kMissing) {
    return local == LocalImport::kImported;
  }

  PoolPtr<FastBufferResource> resource(pool_new<FastBufferResource>());
  PoolPtr<FastBufferShare> share(pool_new<FastBufferShare>());

  if VUNLIKELY (!resource || !share) {
    return false;
  }

  if VUNLIKELY (!share->shared.attach(decoded.name) || share->shared.size() <= sizeof(FastBufferControl)) {
    return false;
  }

  share->control = static_cast<FastBufferControl*>(share->shared.data());
  auto& control = *share->control;

  const uint64_t owner = control.owner.load(std::memory_order_acquire);

  if VUNLIKELY (owner == 0 || control.protocol.load(std::memory_order_relaxed) != impl_->interface->get_protocol_id() ||
                control.pid_namespace.load(std::memory_order_relaxed) != impl_->pid_namespace ||
                control.time_namespace.load(std::memory_order_relaxed) != impl_->time_namespace || control.size == 0 ||
                control.descriptor_size == 0 ||
                control.descriptor_size > share->shared.size() - sizeof(FastBufferControl)) {
    return false;
  }

  if VUNLIKELY (!is_identity_alive(owner)) {
    impl_->destroy_dead_owner(control, share->shared);
    return false;
  }

  size_t index = 0;

  for (; index < kFastBufferMaxReaders; ++index) {
    uint64_t expected = 0;

    if (control.readers[index].compare_exchange_strong(expected, impl_->identity, std::memory_order_seq_cst)) {
      break;
    }
  }

  if VUNLIKELY (index == kFastBufferMaxReaders) {
    VLOG_E("FastBufferManager: Reader table of the shared allocation is full.");
    return false;
  }

  auto& reader = control.readers[index];

  if VUNLIKELY (control.generation.load(std::memory_order_seq_cst) != decoded.generation) {
    reader.store(0, std::memory_order_release);
    return false;
  }

  const auto native_descriptor = Bytes::shallow_copy(reinterpret_cast<const uint8_t*>(share->control + 1),
                                                     static_cast<size_t>(control.descriptor_size));

  if VUNLIKELY (!impl_->interface->import_handle(native_descriptor, resource->native)) {
    reader.store(0, std::memory_order_release);
    return false;
  }

  if VUNLIKELY (resource->native.size != control.size) {
    if (impl_->interface->release(resource->native)) {
      reader.store(0, std::memory_order_release);
    } else {
      VLOG_E("FastBufferManager: Leaking rejected import after SDK release failure.");
    }

    return false;
  }

  share->id = id;
  share->reader = index;
  resource->share.store(share.release(), std::memory_order_release);

  {
    std::lock_guard registry(impl_->mutex);
    impl_->resources.emplace(id, resource.get());
  }

  buffer = resource->native;
  buffer.handle = resource.release();
  return true;
#endif
}

FastBufferManager::FastBufferManager() : impl_(std::make_unique<Impl>()) {
  (void)MemoryPool::global_instance();
  impl_->identity = process_identity(Utils::get_pid());
  impl_->pid_namespace = namespace_inode("/proc/self/ns/pid");
  impl_->time_namespace = namespace_inode("/proc/self/ns/time");

  if VUNLIKELY (impl_->identity == 0) {
    VLOG_W("FastBufferManager: Process identity is unavailable; descriptor sharing is disabled.");
  }

  const std::string plugin_name = Utils::get_env("VLINK_FASTBUFFER_PLUGIN");

  if (plugin_name.empty()) {
    impl_->interface = std::make_shared<HostBufferPlugin>();
  } else if (plugin_name == "shm") {
    impl_->interface =
        std::make_shared<SharedHostBufferPlugin>(impl_->identity, impl_->pid_namespace, impl_->time_namespace);
  } else {
    impl_->interface = impl_->plugin.load<FastBufferPluginInterface>(plugin_name, 1, 0);

    if VUNLIKELY (impl_->interface && impl_->interface->get_protocol_id() == 0) {
      VLOG_E("FastBufferManager: Plugin descriptor protocol ID must not be zero.");
      impl_->interface.reset();
    }
  }

  if (impl_->interface && impl_->identity != 0) {
    sweep_segments(kFastBufferControlPrefix, [this](SysSharemem& shared) {
      if (shared.size() <= sizeof(FastBufferControl)) {
        return;
      }

      const auto& control = *static_cast<const FastBufferControl*>(shared.data());
      const uint64_t owner = control.owner.load(std::memory_order_acquire);

      if (owner != 0 && control.protocol.load(std::memory_order_relaxed) == impl_->interface->get_protocol_id() &&
          control.pid_namespace.load(std::memory_order_relaxed) == impl_->pid_namespace &&
          control.time_namespace.load(std::memory_order_relaxed) == impl_->time_namespace &&
          !is_identity_alive(owner)) {
        impl_->destroy_dead_owner(control, shared);
      }
    });
  }
}

FastBufferManager::~FastBufferManager() {
#ifdef _WIN32
  if (Utils::is_terminating()) {
    (void)impl_.release();
    return;
  }
#endif

  impl_->reclaim_retired();
  auto* pending = impl_->retired.exchange(nullptr, std::memory_order_relaxed);

  while (pending) {
    auto* resource = pending;
    pending = resource->next;
    VLOG_W("FastBufferManager: Releasing a retired allocation still imported by a live process at shutdown.");
    impl_->free_resource(resource);
  }

  impl_->interface.reset();
  impl_->plugin.clear();
}

}  // namespace vlink
