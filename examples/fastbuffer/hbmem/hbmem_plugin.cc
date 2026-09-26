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

#include <hb_mem_mgr.h>
#include <vlink/zerocopy/fast_buffer_plugin_interface.h>

#include <cstring>
#include <fstream>
#include <limits>
#include <memory>
#include <mutex>
#include <string>

class HbmemFastBufferPlugin final : public vlink::FastBufferPluginInterface {
  VLINK_PLUGIN_REGISTER(FastBufferPluginInterface)

 public:
  HbmemFastBufferPlugin() {
    std::ifstream file("/proc/sys/kernel/random/boot_id");
    std::string boot_id;

    if (std::getline(file, boot_id) && boot_id.size() == 36) {
      std::memcpy(boot_id_, boot_id.data(), boot_id.size());
      valid_ = check(hb_mem_module_open());
    }
  }

  ~HbmemFastBufferPlugin() override {
    if (valid_) {
      (void)check(hb_mem_module_close());
    }
  }

  [[nodiscard]] uint64_t get_protocol_id() const noexcept override { return 0x564C48424D450001ULL; }

  [[nodiscard]] bool create(size_t size, const vlink::zerocopy::FastBuffer::Config& config,
                            vlink::zerocopy::FastBuffer::Buffer& buffer) noexcept override {
    if (!valid_ || size == 0 || config.device != -1 ||
        (config.memory_type != vlink::zerocopy::FastBuffer::kMemoryDefault &&
         config.memory_type != vlink::zerocopy::FastBuffer::kMemoryShared &&
         config.memory_type != vlink::zerocopy::FastBuffer::kMemoryDmaBuf)) {
      return false;
    }

    auto resource = std::make_unique<hb_mem_common_buf_t>();
    const int64_t flags = HB_MEM_USAGE_CPU_READ_OFTEN | HB_MEM_USAGE_CPU_WRITE_OFTEN | HB_MEM_USAGE_CACHED;
    std::lock_guard lock(mutex_);

    if (!check(hb_mem_alloc_com_buf(size, flags, resource.get()))) {
      return false;
    }

    // hbmem may round the allocation up; expose exactly the requested payload size.
    const vlink::zerocopy::FastBuffer::MemoryType memory_type =
        config.memory_type == vlink::zerocopy::FastBuffer::kMemoryDmaBuf ? vlink::zerocopy::FastBuffer::kMemoryDmaBuf
                                                                         : vlink::zerocopy::FastBuffer::kMemoryShared;
    buffer = {resource->virt_addr, size, resource.get(), -1, memory_type};
    (void)resource.release();
    return true;
  }

  [[nodiscard]] bool release(vlink::zerocopy::FastBuffer::Buffer& buffer) noexcept override {
    auto* resource = static_cast<hb_mem_common_buf_t*>(buffer.handle);
    std::lock_guard lock(mutex_);

    if (!check(hb_mem_free_buf(resource->fd))) {
      return false;
    }

    delete resource;
    buffer = {};
    return true;
  }

  // hbmem cache operations do not wait for BPU/VPU/GPU tasks.  The application
  // must finish external tasks before sharing, accessing or releasing a buffer.
  [[nodiscard]] bool synchronize(const vlink::zerocopy::FastBuffer::Buffer&) noexcept override { return valid_; }

  [[nodiscard]] uint8_t* map(const vlink::zerocopy::FastBuffer::Buffer& buffer,
                             vlink::zerocopy::FastBuffer::Access access) noexcept override {
    if (!buffer.data ||
        (access != vlink::zerocopy::FastBuffer::kRead && access != vlink::zerocopy::FastBuffer::kWrite &&
         access != vlink::zerocopy::FastBuffer::kReadWrite)) {
      return nullptr;
    }

    const auto& native = *static_cast<const hb_mem_common_buf_t*>(buffer.handle);

    if ((access & vlink::zerocopy::FastBuffer::kRead) != 0 && (native.flags & HB_MEM_USAGE_CACHED) != 0) {
      std::lock_guard lock(mutex_);

      if (!check(hb_mem_invalidate_buf(native.fd, 0, buffer.size))) {
        return nullptr;
      }
    }

    return buffer.data;
  }

  [[nodiscard]] bool unmap(const vlink::zerocopy::FastBuffer::Buffer& buffer,
                           vlink::zerocopy::FastBuffer::Access access) noexcept override {
    if (!buffer.data ||
        (access != vlink::zerocopy::FastBuffer::kRead && access != vlink::zerocopy::FastBuffer::kWrite &&
         access != vlink::zerocopy::FastBuffer::kReadWrite)) {
      return false;
    }

    const auto& native = *static_cast<const hb_mem_common_buf_t*>(buffer.handle);

    if ((access & vlink::zerocopy::FastBuffer::kWrite) != 0 && (native.flags & HB_MEM_USAGE_CACHED) != 0) {
      std::lock_guard lock(mutex_);
      return check(hb_mem_flush_buf(native.fd, 0, buffer.size));
    }

    return true;
  }

  [[nodiscard]] bool copy(const vlink::zerocopy::FastBuffer::Buffer& source,
                          const vlink::zerocopy::FastBuffer::Buffer& destination) noexcept override {
    if (source.size != destination.size) {
      return false;
    }

    const auto* input = map(source, vlink::zerocopy::FastBuffer::kRead);

    if (!input) {
      return false;
    }

    auto* output = map(destination, vlink::zerocopy::FastBuffer::kWrite);

    if (!output) {
      (void)unmap(source, vlink::zerocopy::FastBuffer::kRead);
      return false;
    }

    std::memmove(output, input, source.size);
    const bool written = unmap(destination, vlink::zerocopy::FastBuffer::kWrite);
    return unmap(source, vlink::zerocopy::FastBuffer::kRead) && written;
  }

  [[nodiscard]] bool copy_to_host(const vlink::zerocopy::FastBuffer::Buffer& source,
                                  vlink::Bytes& destination) noexcept override {
    if (source.size != destination.size()) {
      return false;
    }

    const auto* input = map(source, vlink::zerocopy::FastBuffer::kRead);

    if (!input) {
      return false;
    }

    std::memmove(destination.data(), input, source.size);
    return unmap(source, vlink::zerocopy::FastBuffer::kRead);
  }

  [[nodiscard]] bool copy_from_host(const vlink::Bytes& source,
                                    const vlink::zerocopy::FastBuffer::Buffer& destination) noexcept override {
    if (source.size() != destination.size) {
      return false;
    }

    auto* output = map(destination, vlink::zerocopy::FastBuffer::kWrite);

    if (!output) {
      return false;
    }

    std::memmove(output, source.data(), source.size());
    return unmap(destination, vlink::zerocopy::FastBuffer::kWrite);
  }

  [[nodiscard]] const void* native_handle(const vlink::zerocopy::FastBuffer::Buffer& buffer) const noexcept override {
    return buffer.handle;
  }

  [[nodiscard]] bool import_native(const void* handle, vlink::zerocopy::FastBuffer::Buffer& buffer) noexcept override {
    if (!valid_ || !handle) {
      return false;
    }

    auto source = *static_cast<const hb_mem_common_buf_t*>(handle);

    if (source.size == 0 || source.size > std::numeric_limits<size_t>::max()) {
      return false;
    }

    auto resource = std::make_unique<hb_mem_common_buf_t>();
    std::lock_guard lock(mutex_);

    if (!check(hb_mem_import_com_buf(&source, resource.get()))) {
      return false;
    }

    buffer = {resource->virt_addr, static_cast<size_t>(source.size), resource.get(), -1,
              vlink::zerocopy::FastBuffer::kMemoryShared};
    (void)resource.release();
    return true;
  }

  [[nodiscard]] size_t get_handle_size(const vlink::zerocopy::FastBuffer::Buffer&) const noexcept override {
    return sizeof(Descriptor);
  }

  [[nodiscard]] bool export_handle(const vlink::zerocopy::FastBuffer::Buffer& buffer,
                                   vlink::Bytes& descriptor) noexcept override {
    if (descriptor.size() != sizeof(Descriptor)) {
      return false;
    }

    const auto& native = *static_cast<const hb_mem_common_buf_t*>(buffer.handle);
    Descriptor exported;
    std::memcpy(exported.boot_id, boot_id_, sizeof(boot_id_));
    exported.fd = native.fd;
    exported.share_id = native.share_id;
    exported.flags = native.flags;
    exported.allocation_size = native.size;
    exported.size = buffer.size;
    exported.phys_addr = native.phys_addr;
    exported.offset = native.offset;
    exported.memory_type = buffer.memory_type;
    std::memcpy(descriptor.data(), &exported, sizeof(exported));
    return true;
  }

  [[nodiscard]] bool import_handle(const vlink::Bytes& descriptor,
                                   vlink::zerocopy::FastBuffer::Buffer& buffer) noexcept override {
    if (!valid_ || descriptor.size() != sizeof(Descriptor)) {
      return false;
    }

    Descriptor imported;
    std::memcpy(&imported, descriptor.data(), sizeof(imported));

    if (std::memcmp(imported.boot_id, boot_id_, sizeof(boot_id_)) != 0 || imported.size == 0 ||
        imported.size > imported.allocation_size ||
        (imported.memory_type != vlink::zerocopy::FastBuffer::kMemoryShared &&
         imported.memory_type != vlink::zerocopy::FastBuffer::kMemoryDmaBuf)) {
      return false;
    }

    hb_mem_common_buf_t native{};
    native.fd = imported.fd;
    native.share_id = imported.share_id;
    native.flags = imported.flags;
    native.size = imported.allocation_size;
    native.phys_addr = imported.phys_addr;
    native.offset = imported.offset;

    // The producer's fd is SDK descriptor data, never used as a local OS fd.
    // hb_mem_import_com_buf creates this process's fd and virtual mapping.
    if (!import_native(&native, buffer)) {
      return false;
    }

    buffer.size = static_cast<size_t>(imported.size);
    buffer.memory_type = imported.memory_type;
    return true;
  }

 private:
  struct Descriptor final {
    char boot_id[40]{};
    int32_t fd{-1};
    int32_t share_id{-1};
    int64_t flags{0};
    uint64_t allocation_size{0};
    uint64_t size{0};
    uint64_t phys_addr{0};
    uint64_t offset{0};
    vlink::zerocopy::FastBuffer::MemoryType memory_type{vlink::zerocopy::FastBuffer::kMemoryShared};
    uint8_t reserved[7]{};
  };

  static bool check(int32_t result) noexcept {
    if (result != 0) {
      VLOG_E("hbmem FastBuffer: SDK error ", result);
      return false;
    }

    return true;
  }

  std::mutex mutex_;
  char boot_id_[40]{};
  bool valid_{false};
};

VLINK_PLUGIN_DECLARE(HbmemFastBufferPlugin, 1, 0)
