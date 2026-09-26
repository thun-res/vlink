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

#include <cuda_runtime_api.h>
#include <vlink/zerocopy/fast_buffer_plugin_interface.h>

#include <cstring>

class CudaFastBufferPlugin final : public vlink::FastBufferPluginInterface {
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

    if (device == -1 && !check(cudaGetDevice(&device))) {
      return false;
    }

    DeviceScope scope(device);

    if (!scope.valid) {
      return false;
    }

    void* address = nullptr;
    cudaError_t result = cudaErrorInvalidValue;

    switch (memory_type) {
      case vlink::zerocopy::FastBuffer::kMemoryDevice:
        result = cudaMalloc(&address, size);
        break;
      case vlink::zerocopy::FastBuffer::kMemoryHost:
        result = cudaHostAlloc(&address, size, cudaHostAllocPortable);
        break;
      case vlink::zerocopy::FastBuffer::kMemoryVirtual:
        result = cudaMallocManaged(&address, size, cudaMemAttachGlobal);
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
        buffer.handle ? check(cudaIpcCloseMemHandle(buffer.handle)) : free_memory(buffer.data, buffer.memory_type);

    if (!released) {
      return false;
    }

    buffer = {};
    return true;
  }

  [[nodiscard]] bool synchronize(const vlink::zerocopy::FastBuffer::Buffer& buffer) noexcept override {
    DeviceScope scope(buffer.device);
    return scope.valid && check(cudaDeviceSynchronize());
  }

  [[nodiscard]] bool copy(const vlink::zerocopy::FastBuffer::Buffer& source,
                          const vlink::zerocopy::FastBuffer::Buffer& destination) noexcept override {
    if (source.size != destination.size || source.device != destination.device) {
      return false;
    }

    DeviceScope scope(source.device);
    return scope.valid && check(cudaDeviceSynchronize()) &&
           check(cudaMemcpy(destination.data, source.data, source.size, cudaMemcpyDefault)) &&
           check(cudaDeviceSynchronize());
  }

  [[nodiscard]] bool copy_to_host(const vlink::zerocopy::FastBuffer::Buffer& source,
                                  vlink::Bytes& destination) noexcept override {
    if (source.size != destination.size()) {
      return false;
    }

    DeviceScope scope(source.device);
    return scope.valid && check(cudaDeviceSynchronize()) &&
           check(cudaMemcpy(destination.data(), source.data, source.size, cudaMemcpyDefault));
  }

  [[nodiscard]] bool copy_from_host(const vlink::Bytes& source,
                                    const vlink::zerocopy::FastBuffer::Buffer& destination) noexcept override {
    if (source.size() != destination.size) {
      return false;
    }

    DeviceScope scope(destination.device);
    return scope.valid && check(cudaDeviceSynchronize()) &&
           check(cudaMemcpy(destination.data, source.data(), source.size(), cudaMemcpyDefault)) &&
           check(cudaDeviceSynchronize());
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

    if (!scope.valid || !check(cudaDeviceGetPCIBusId(exported.bus_id, sizeof(exported.bus_id), buffer.device)) ||
        !check(cudaIpcGetMemHandle(&exported.handle, buffer.data))) {
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

    if (!check(cudaDeviceGetByPCIBusId(&device, decoded.bus_id))) {
      return false;
    }

    DeviceScope scope(device);
    void* address = nullptr;

    if (!scope.valid || !check(cudaIpcOpenMemHandle(&address, decoded.handle, cudaIpcMemLazyEnablePeerAccess))) {
      return false;
    }

    buffer = {static_cast<uint8_t*>(address), static_cast<size_t>(decoded.size), address, device,
              vlink::zerocopy::FastBuffer::kMemoryDevice};
    return true;
  }

 private:
  static constexpr uint64_t kProtocol = 0x564C435544410001ULL;

  static bool check(cudaError_t result) noexcept {
    if (result != cudaSuccess) {
      VLOG_E("Cuda FastBuffer: ", cudaGetErrorString(result));
      return false;
    }

    return true;
  }

  static bool free_memory(uint8_t* data, vlink::zerocopy::FastBuffer::MemoryType type) noexcept {
    return check(type == vlink::zerocopy::FastBuffer::kMemoryHost ? cudaFreeHost(data) : cudaFree(data));
  }

  struct Descriptor final {
    uint64_t size{0};
    char bus_id[32]{};
    cudaIpcMemHandle_t handle{};
  };

  struct DeviceScope final {
    explicit DeviceScope(int device) {
      valid = check(cudaGetDevice(&previous));
      changed = valid && previous != device;

      if (changed) {
        valid = check(cudaSetDevice(device));
      }
    }

    ~DeviceScope() {
      if (valid && changed && !check(cudaSetDevice(previous))) {
        VLOG_E("FastBuffer: Failed to restore the calling thread's device.");
      }
    }

    int previous{-1};
    bool valid{false};
    bool changed{false};
  };
};

VLINK_PLUGIN_DECLARE(CudaFastBufferPlugin, 1, 0)
