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

#include <hip/hip_runtime_api.h>
#include <vlink/zerocopy/fast_buffer_plugin_interface.h>

#include <cstring>

class HipFastBufferPlugin final : public vlink::FastBufferPluginInterface {
  VLINK_PLUGIN_REGISTER(FastBufferPluginInterface)

 public:
  [[nodiscard]] uint64_t get_protocol_id() const noexcept override { return kProtocol; }

  [[nodiscard]] bool create(size_t size, const vlink::zerocopy::FastBuffer::Config& config,
                            vlink::zerocopy::FastBuffer::Buffer& buffer) noexcept override {
    const vlink::zerocopy::FastBuffer::MemoryType memory_type =
        config.memory_type == vlink::zerocopy::FastBuffer::kMemoryDefault ? vlink::zerocopy::FastBuffer::kMemoryDevice
                                                                          : config.memory_type;

    if (size == 0 || (memory_type != vlink::zerocopy::FastBuffer::kMemoryDevice &&
                      memory_type != vlink::zerocopy::FastBuffer::kMemoryHost &&
                      memory_type != vlink::zerocopy::FastBuffer::kMemoryVirtual)) {
      return false;
    }

    int device = config.device;

    if (device == -1 && !check(hipGetDevice(&device))) {
      return false;
    }

    DeviceScope scope(device);

    if (!scope.valid) {
      return false;
    }

    void* address = nullptr;
    hipError_t result = hipErrorInvalidValue;

    switch (memory_type) {
      case vlink::zerocopy::FastBuffer::kMemoryDevice:
        result = hipMalloc(&address, size);
        break;
      case vlink::zerocopy::FastBuffer::kMemoryHost:
        result = hipHostMalloc(&address, size, hipHostMallocPortable);
        break;
      case vlink::zerocopy::FastBuffer::kMemoryVirtual:
        result = hipMallocManaged(&address, size, hipMemAttachGlobal);
        break;
      default:
        return false;
    }

    if (!check(result)) {
      return false;
    }

    buffer = {static_cast<uint8_t*>(address), size, nullptr, device, memory_type};
    return true;
  }

  [[nodiscard]] bool release(vlink::zerocopy::FastBuffer::Buffer& buffer) noexcept override {
    DeviceScope scope(buffer.device);

    if (!scope.valid) {
      return false;
    }

    // Only imported allocations hold a closeable IPC mapping in handle.
    const bool released =
        buffer.handle ? check(hipIpcCloseMemHandle(buffer.handle)) : free_memory(buffer.data, buffer.memory_type);

    if (!released) {
      return false;
    }

    buffer = {};
    return true;
  }

  [[nodiscard]] bool synchronize(const vlink::zerocopy::FastBuffer::Buffer& buffer) noexcept override {
    DeviceScope scope(buffer.device);
    return scope.valid && check(hipDeviceSynchronize());
  }

  [[nodiscard]] bool copy(const vlink::zerocopy::FastBuffer::Buffer& source,
                          const vlink::zerocopy::FastBuffer::Buffer& destination) noexcept override {
    if (source.size != destination.size || source.device != destination.device) {
      return false;
    }

    DeviceScope scope(source.device);
    return scope.valid && check(hipDeviceSynchronize()) &&
           check(hipMemcpy(destination.data, source.data, source.size, hipMemcpyDefault)) &&
           check(hipDeviceSynchronize());
  }

  [[nodiscard]] bool copy_to_host(const vlink::zerocopy::FastBuffer::Buffer& source,
                                  vlink::Bytes& destination) noexcept override {
    if (source.size != destination.size()) {
      return false;
    }

    DeviceScope scope(source.device);
    return scope.valid && check(hipDeviceSynchronize()) &&
           check(hipMemcpy(destination.data(), source.data, source.size, hipMemcpyDefault));
  }

  [[nodiscard]] bool copy_from_host(const vlink::Bytes& source,
                                    const vlink::zerocopy::FastBuffer::Buffer& destination) noexcept override {
    if (source.size() != destination.size) {
      return false;
    }

    DeviceScope scope(destination.device);
    return scope.valid && check(hipDeviceSynchronize()) &&
           check(hipMemcpy(destination.data, source.data(), source.size(), hipMemcpyDefault)) &&
           check(hipDeviceSynchronize());
  }

  [[nodiscard]] uint8_t* map(const vlink::zerocopy::FastBuffer::Buffer& buffer,
                             vlink::zerocopy::FastBuffer::Access access) noexcept override {
    if (buffer.memory_type == vlink::zerocopy::FastBuffer::kMemoryDevice ||
        (access != vlink::zerocopy::FastBuffer::kRead && access != vlink::zerocopy::FastBuffer::kWrite &&
         access != vlink::zerocopy::FastBuffer::kReadWrite)) {
      return nullptr;
    }

    return synchronize(buffer) ? buffer.data : nullptr;
  }

  [[nodiscard]] bool unmap(const vlink::zerocopy::FastBuffer::Buffer& buffer,
                           vlink::zerocopy::FastBuffer::Access access) noexcept override {
    return buffer.memory_type != vlink::zerocopy::FastBuffer::kMemoryDevice &&
           (access == vlink::zerocopy::FastBuffer::kRead || access == vlink::zerocopy::FastBuffer::kWrite ||
            access == vlink::zerocopy::FastBuffer::kReadWrite) &&
           synchronize(buffer);
  }

  [[nodiscard]] const void* native_handle(const vlink::zerocopy::FastBuffer::Buffer& buffer) const noexcept override {
    return buffer.data;
  }

  [[nodiscard]] size_t get_handle_size(const vlink::zerocopy::FastBuffer::Buffer& buffer) const noexcept override {
    return buffer.memory_type == vlink::zerocopy::FastBuffer::kMemoryDevice ? sizeof(Descriptor) : 0;
  }

  [[nodiscard]] bool export_handle(const vlink::zerocopy::FastBuffer::Buffer& buffer,
                                   vlink::Bytes& descriptor) noexcept override {
    if (get_handle_size(buffer) == 0 || descriptor.size() != sizeof(Descriptor)) {
      return false;
    }

    DeviceScope scope(buffer.device);
    Descriptor exported;
    exported.size = buffer.size;

    if (!scope.valid || !check(hipDeviceGetPCIBusId(exported.bus_id, sizeof(exported.bus_id), buffer.device)) ||
        !check(hipIpcGetMemHandle(&exported.handle, buffer.data))) {
      return false;
    }

    std::memcpy(descriptor.data(), &exported, sizeof(exported));
    return true;
  }

  [[nodiscard]] bool import_handle(const vlink::Bytes& descriptor,
                                   vlink::zerocopy::FastBuffer::Buffer& buffer) noexcept override {
    if (descriptor.size() != sizeof(Descriptor)) {
      return false;
    }

    Descriptor decoded;
    std::memcpy(&decoded, descriptor.data(), sizeof(decoded));

    if (decoded.size == 0 || std::memchr(decoded.bus_id, '\0', sizeof(decoded.bus_id)) == nullptr) {
      return false;
    }

    int device = -1;

    if (!check(hipDeviceGetByPCIBusId(&device, decoded.bus_id))) {
      return false;
    }

    DeviceScope scope(device);
    void* address = nullptr;

    if (!scope.valid || !check(hipIpcOpenMemHandle(&address, decoded.handle, hipIpcMemLazyEnablePeerAccess))) {
      return false;
    }

    buffer = {static_cast<uint8_t*>(address), static_cast<size_t>(decoded.size), address, device,
              vlink::zerocopy::FastBuffer::kMemoryDevice};
    return true;
  }

 private:
  static constexpr uint64_t kProtocol = 0x564C484950000001ULL;

  static bool check(hipError_t result) noexcept {
    if (result != hipSuccess) {
      VLOG_E("Hip FastBuffer: ", hipGetErrorString(result));
      return false;
    }

    return true;
  }

  static bool free_memory(uint8_t* data, vlink::zerocopy::FastBuffer::MemoryType type) noexcept {
    return check(type == vlink::zerocopy::FastBuffer::kMemoryHost ? hipHostFree(data) : hipFree(data));
  }

  struct Descriptor final {
    uint64_t size{0};
    char bus_id[32]{};
    hipIpcMemHandle_t handle{};
  };

  struct DeviceScope final {
    explicit DeviceScope(int device) {
      valid = check(hipGetDevice(&previous));
      changed = valid && previous != device;

      if (changed) {
        valid = check(hipSetDevice(device));
      }
    }

    ~DeviceScope() {
      if (valid && changed && !check(hipSetDevice(previous))) {
        VLOG_E("FastBuffer: Failed to restore the calling thread's device.");
      }
    }

    int previous{-1};
    bool valid{false};
    bool changed{false};
  };
};

VLINK_PLUGIN_DECLARE(HipFastBufferPlugin, 1, 0)
