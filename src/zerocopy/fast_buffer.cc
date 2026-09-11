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

#include "./zerocopy/fast_buffer.h"

#include <atomic>
#include <cstring>
#include <memory>
#include <new>
#include <utility>

#include "./zerocopy/fast_buffer_manager.h"

namespace vlink {

namespace zerocopy {

// FastBufferWireMetadata
struct FastBufferWireMetadata final {
  Header header;
  uint64_t size{0};
  uint64_t protocol{0};
  FastBuffer::Storage storage{FastBuffer::kStorageShared};
  FastBuffer::MemoryType memory_type{FastBuffer::kMemoryDefault};
  uint8_t reserved1[6]{};
  uint64_t metadata_size{0};
  uint64_t reserved[6]{};
};

// FastBuffer::MetadataBuffer
struct FastBuffer::MetadataBuffer final {
  Bytes bytes;
  std::atomic<size_t> references{1};
};

static constexpr size_t kFastBufferPrefixSize = sizeof(uint32_t) * 2 + sizeof(FastBufferWireMetadata);
static constexpr size_t kFastBufferEnvelopeSize = kFastBufferPrefixSize + sizeof(uint32_t);

// FastBuffer
FastBuffer::FastBuffer() {
  (void)FastBufferManager::get();

  static_assert(sizeof(FastBufferWireMetadata) == 120, "Sizeof must be 120 bytes.");
}

FastBuffer::~FastBuffer() noexcept { clear(); }

FastBuffer::FastBuffer(const FastBuffer& target) noexcept : FastBuffer() { (void)deep_copy(target); }

FastBuffer::FastBuffer(FastBuffer&& target) noexcept : FastBuffer() { move_copy(target); }

FastBuffer& FastBuffer::operator=(const FastBuffer& target) noexcept {
  if VLIKELY (this != &target) {
    (void)deep_copy(target);
  }

  return *this;
}

FastBuffer& FastBuffer::operator=(FastBuffer&& target) noexcept {
  if VLIKELY (this != &target) {
    move_copy(target);
  }

  return *this;
}

bool FastBuffer::operator<<(const Bytes& bytes) noexcept {
  auto& manager = FastBufferManager::get();
  auto* plugin = manager.get_interface();
  Metadata metadata;

  if VUNLIKELY (!plugin || !read_metadata(bytes, metadata)) {
    return false;
  }

  if VUNLIKELY (metadata.storage == kStorageShared && metadata.protocol != plugin->get_protocol_id()) {
    return false;
  }

  std::unique_ptr<MetadataBuffer> copied_metadata(copy_metadata(metadata.buffer));

  if VUNLIKELY (!metadata.buffer.empty() && !copied_metadata) {
    return false;
  }

  const auto payload = Bytes::shallow_copy(bytes.data() + kFastBufferPrefixSize + metadata.buffer.size(),
                                           bytes.size() - kFastBufferEnvelopeSize - metadata.buffer.size());
  FastBuffer::Buffer buffer;

  if (metadata.storage == kStorageShared) {
    if VUNLIKELY (!manager.import_handle(payload, buffer)) {
      return false;
    }
  } else {
    if VUNLIKELY (!manager.create(static_cast<size_t>(metadata.size), {}, buffer)) {
      return false;
    }

    if VUNLIKELY (!plugin->copy_from_host(payload, manager.native_buffer(buffer))) {
      manager.release(buffer);
      return false;
    }
  }

  if VUNLIKELY (!buffer.handle || buffer.size != metadata.size) {
    manager.release(buffer);
    return false;
  }

  clear();
  metadata_ = copied_metadata.release();
  buffer_ = buffer;
  header = metadata.header;
  std::memcpy(reserved_buf_.data(), metadata.reserved, sizeof(metadata.reserved));

  return true;
}

bool FastBuffer::operator>>(Bytes& bytes) const noexcept {
  if VUNLIKELY (!is_valid()) {
    return false;
  }

  auto& manager = FastBufferManager::get();
  auto* plugin = manager.get_interface();
  size_t payload_size = manager.get_handle_size(buffer_);
  const Storage storage = payload_size == 0 ? kStorageHost : kStorageShared;

  if (storage == kStorageHost) {
    payload_size = buffer_.size;
  }

  const Bytes& metadata_bytes = get_metadata();

  const size_t wire_size = kFastBufferEnvelopeSize + metadata_bytes.size() + payload_size;

  if (bytes.size() != wire_size) {
    if VUNLIKELY (bytes.is_loaned()) {
      return false;
    }

    bytes = Bytes::create(wire_size);

    if VUNLIKELY (bytes.empty()) {
      return false;
    }
  }

  auto payload = Bytes::shallow_copy(bytes.data() + kFastBufferPrefixSize + metadata_bytes.size(), payload_size);
  const bool copied = storage == kStorageHost ? plugin->copy_to_host(manager.native_buffer(buffer_), payload)
                                              : manager.export_handle(buffer_, payload);

  if VUNLIKELY (!copied) {
    return false;
  }

  FastBufferWireMetadata metadata;
  metadata.header = header;
  metadata.size = buffer_.size;
  metadata.protocol = storage == kStorageShared ? plugin->get_protocol_id() : 0;
  metadata.storage = storage;
  metadata.memory_type = buffer_.memory_type;
  metadata.metadata_size = metadata_bytes.size();
  std::memcpy(metadata.reserved, reserved_buf_.data(), sizeof(metadata.reserved));

  std::memcpy(bytes.data(), &kMagicNumberBegin, sizeof(kMagicNumberBegin));
  std::memcpy(bytes.data() + sizeof(kMagicNumberBegin), &kWireVersion, sizeof(kWireVersion));
  std::memcpy(bytes.data() + sizeof(uint32_t) * 2, &metadata, sizeof(metadata));

  if (!metadata_bytes.empty()) {
    std::memcpy(bytes.data() + kFastBufferPrefixSize, metadata_bytes.data(), metadata_bytes.size());
  }

  std::memcpy(bytes.data() + wire_size - sizeof(kMagicNumberEnd), &kMagicNumberEnd, sizeof(kMagicNumberEnd));

  return true;
}

bool FastBuffer::check_valid(const Bytes& bytes) noexcept {
  Metadata metadata;
  return read_metadata(bytes, metadata);
}

bool FastBuffer::read_metadata(const Bytes& bytes, Metadata& metadata) noexcept {
  if VUNLIKELY (bytes.size() <= kFastBufferEnvelopeSize) {
    return false;
  }

  uint32_t magic = 0;
  uint32_t version = 0;
  std::memcpy(&magic, bytes.data(), sizeof(magic));
  std::memcpy(&version, bytes.data() + sizeof(magic), sizeof(version));

  if VUNLIKELY (magic != kMagicNumberBegin || version_major(version) != version_major(kWireVersion)) {
    return false;
  }

  std::memcpy(&magic, bytes.end() - sizeof(magic), sizeof(magic));

  if VUNLIKELY (magic != kMagicNumberEnd) {
    return false;
  }

  FastBufferWireMetadata decoded;
  std::memcpy(&decoded, bytes.data() + sizeof(uint32_t) * 2, sizeof(decoded));

  if VUNLIKELY (decoded.size == 0 || decoded.metadata_size >= bytes.size() - kFastBufferEnvelopeSize) {
    return false;
  }

  if (decoded.storage == kStorageHost) {
    if VUNLIKELY (decoded.protocol != 0 ||
                  decoded.size != bytes.size() - kFastBufferEnvelopeSize - decoded.metadata_size) {
      return false;
    }
  } else if VUNLIKELY (decoded.storage != kStorageShared || decoded.protocol == 0) {
    return false;
  }

  metadata.header = decoded.header;
  metadata.size = decoded.size;
  metadata.protocol = decoded.protocol;
  metadata.storage = decoded.storage;
  metadata.memory_type = decoded.memory_type;
  std::memcpy(metadata.reserved, decoded.reserved, sizeof(decoded.reserved));
  metadata.buffer =
      Bytes::shallow_copy(bytes.data() + kFastBufferPrefixSize, static_cast<size_t>(decoded.metadata_size));
  return true;
}

size_t FastBuffer::get_serialized_size() const noexcept {
  if VUNLIKELY (!is_valid()) {
    return 0;
  }

  size_t payload_size = FastBufferManager::get().get_handle_size(buffer_);

  if (payload_size == 0) {
    payload_size = buffer_.size;
  }

  const size_t metadata_size = get_metadata().size();
  return kFastBufferEnvelopeSize + metadata_size + payload_size;
}

bool FastBuffer::is_valid() const noexcept { return buffer_.handle != nullptr && buffer_.size != 0; }

bool FastBuffer::create(size_t size, int32_t device) noexcept {
  FastBuffer::Config config;
  config.device = device;
  return create(size, config);
}

bool FastBuffer::create(size_t size, const FastBuffer::Config& config) noexcept {
  auto& manager = FastBufferManager::get();
  auto* plugin = manager.get_interface();

  if VUNLIKELY (!plugin || size == 0) {
    return false;
  }

  FastBuffer::Buffer buffer;

  if VUNLIKELY (!manager.create(size, config, buffer)) {
    return false;
  }

  if (buffer_.handle) {
    manager.release(buffer_);
  }

  buffer_ = buffer;
  return true;
}

bool FastBuffer::shallow_copy(const FastBuffer& target) noexcept {
  if VUNLIKELY (this == &target) {
    return false;
  }

  FastBuffer::Buffer buffer;

  if (target.is_valid()) {
    FastBufferManager::get().retain(target.buffer_, buffer);
  }

  clear();
  buffer_ = buffer;
  header = target.header;
  reserved_buf_ = target.reserved_buf_;

  metadata_ = target.metadata_;

  if (metadata_) {
    metadata_->references.fetch_add(1, std::memory_order_relaxed);
  }

  return true;
}

bool FastBuffer::deep_copy(const FastBuffer& target) noexcept {
  if VUNLIKELY (this == &target) {
    return false;
  }

  FastBuffer::Buffer buffer;
  std::unique_ptr<MetadataBuffer> copied_metadata(copy_metadata(target.get_metadata()));

  if VUNLIKELY (!target.get_metadata().empty() && !copied_metadata) {
    return false;
  }

  if (target.is_valid()) {
    auto& manager = FastBufferManager::get();
    auto* plugin = manager.get_interface();
    FastBuffer::Config config;
    config.device = target.device();
    config.memory_type = target.memory_type();

    if VUNLIKELY (!manager.create(target.size(), config, buffer)) {
      return false;
    }

    if VUNLIKELY (!plugin->copy(manager.native_buffer(target.buffer_), manager.native_buffer(buffer))) {
      manager.release(buffer);
      return false;
    }
  }

  clear();
  metadata_ = copied_metadata.release();
  buffer_ = buffer;
  header = target.header;
  reserved_buf_ = target.reserved_buf_;

  return true;
}

bool FastBuffer::move_copy(FastBuffer& target) noexcept {
  if VUNLIKELY (this == &target) {
    return false;
  }

  clear();
  buffer_ = target.buffer_;
  header = target.header;
  reserved_buf_ = target.reserved_buf_;

  metadata_ = std::exchange(target.metadata_, nullptr);
  target.buffer_ = {};
  target.header = {};
  target.reserved_buf_.fill(0);

  return true;
}

void FastBuffer::clear() noexcept {
  if (buffer_.handle) {
    FastBufferManager::get().release(buffer_);
  }

  clear_metadata();
  header = {};
  reserved_buf_.fill(0);
}

bool FastBuffer::synchronize() const noexcept {
  auto& manager = FastBufferManager::get();
  return is_valid() && manager.get_interface()->synchronize(manager.native_buffer(buffer_));
}

FastBuffer::MetadataBuffer* FastBuffer::copy_metadata(const Bytes& bytes) noexcept {
  if (bytes.empty()) {
    return nullptr;
  }

  auto metadata = std::unique_ptr<MetadataBuffer>(new (std::nothrow) MetadataBuffer);

  if VUNLIKELY (!metadata) {
    return nullptr;
  }

  metadata->bytes = Bytes::deep_copy(bytes.data(), bytes.size());
  return metadata->bytes.empty() ? nullptr : metadata.release();
}

void FastBuffer::clear_metadata() noexcept {
  if (metadata_ && metadata_->references.fetch_sub(1, std::memory_order_acq_rel) == 1) {
    delete metadata_;
  }

  metadata_ = nullptr;
}

bool FastBuffer::set_metadata(const Bytes& bytes) noexcept {
  MetadataBuffer* metadata = copy_metadata(bytes);

  if VUNLIKELY (!bytes.empty() && !metadata) {
    return false;
  }

  clear_metadata();
  metadata_ = metadata;
  return true;
}

bool FastBuffer::set_metadata(Bytes&& bytes) noexcept {
  if (bytes.empty() || !bytes.is_owner()) {
    return set_metadata(static_cast<const Bytes&>(bytes));
  }

  auto* metadata = new (std::nothrow) MetadataBuffer;

  if VUNLIKELY (!metadata) {
    return false;
  }

  metadata->bytes = std::move(bytes);
  clear_metadata();
  metadata_ = metadata;
  return true;
}

const Bytes& FastBuffer::get_metadata() const noexcept {
  static const Bytes kEmpty;
  return metadata_ ? metadata_->bytes : kEmpty;
}

bool FastBuffer::copy_to_host(Bytes& bytes) const noexcept {
  auto& manager = FastBufferManager::get();
  if VUNLIKELY (!is_valid()) {
    return false;
  }

  if (bytes.size() != buffer_.size) {
    if VUNLIKELY (bytes.is_loaned()) {
      return false;
    }

    bytes = Bytes::create(buffer_.size);

    if VUNLIKELY (bytes.empty()) {
      return false;
    }
  }

  return manager.get_interface()->copy_to_host(manager.native_buffer(buffer_), bytes);
}

bool FastBuffer::copy_from_host(const Bytes& bytes) noexcept {
  auto& manager = FastBufferManager::get();
  if VUNLIKELY (!is_valid() || bytes.size() != buffer_.size) {
    return false;
  }

  return manager.get_interface()->copy_from_host(bytes, manager.native_buffer(buffer_));
}

uintptr_t FastBuffer::address() const noexcept { return reinterpret_cast<uintptr_t>(buffer_.data); }

size_t FastBuffer::size() const noexcept { return buffer_.size; }

int32_t FastBuffer::device() const noexcept { return buffer_.device; }

FastBuffer::MemoryType FastBuffer::memory_type() const noexcept { return buffer_.memory_type; }

bool FastBuffer::is_owner() const noexcept { return buffer_.handle != nullptr; }

uint8_t* FastBuffer::map(FastBuffer::Access access) const noexcept {
  auto& manager = FastBufferManager::get();
  return is_valid() ? manager.get_interface()->map(manager.native_buffer(buffer_), access) : nullptr;
}

bool FastBuffer::unmap(FastBuffer::Access access) const noexcept {
  auto& manager = FastBufferManager::get();
  return is_valid() && manager.get_interface()->unmap(manager.native_buffer(buffer_), access);
}

const void* FastBuffer::native_handle() const noexcept {
  auto& manager = FastBufferManager::get();
  return is_valid() ? manager.get_interface()->native_handle(manager.native_buffer(buffer_)) : nullptr;
}

bool FastBuffer::import_native(const void* handle) noexcept {
  auto& manager = FastBufferManager::get();
  auto* plugin = manager.get_interface();

  if VUNLIKELY (!plugin || !handle) {
    return false;
  }

  FastBuffer::Buffer buffer;

  if VUNLIKELY (!manager.import_native(handle, buffer)) {
    return false;
  }

  clear();
  buffer_ = buffer;

  return true;
}

}  // namespace zerocopy

}  // namespace vlink
