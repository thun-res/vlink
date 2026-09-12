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

// NOLINTBEGIN

#include "./zerocopy/fast_buffer.h"

#include <doctest/doctest.h>

#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <string>
#include <thread>
#include <utility>

#include "../common_test.h"
#include "./base/elapsed_timer.h"
#include "./base/logger.h"
#include "./base/process.h"
#include "./base/sys_sharemem.h"
#include "./zerocopy/fast_buffer_manager.h"
#include "./zerocopy/fast_buffer_pool.h"
#include "./zerocopy/message_parser.h"

static constexpr size_t kFastBufferEnvelope = 132;
static constexpr size_t kFastBufferSystemHandle = 40;
static constexpr int64_t kFastBufferWaitMs = 20000;

class FastBufferTestDir {
 public:
  explicit FastBufferTestDir(const std::string& name)
      : path_(std::filesystem::path(Utils::get_tmp_dir()) / (name + "_" + Utils::get_pid_str())) {
    std::error_code ec;
    std::filesystem::remove_all(path_, ec);
    std::filesystem::create_directories(path_, ec);
  }

  ~FastBufferTestDir() {
    std::error_code ec;
    std::filesystem::remove_all(path_, ec);
  }

  const std::filesystem::path& path() const { return path_; }

 private:
  std::filesystem::path path_;
};

static std::filesystem::path fast_buffer_child_dir() { return Utils::get_env("VLINK_FASTBUFFER_TEST_DIR"); }

static bool fast_buffer_touch(const std::filesystem::path& dir, const std::string& name) {
  std::ofstream file(dir / name, std::ios::binary);
  return file.good();
}

static bool fast_buffer_wait(const std::filesystem::path& dir, const std::string& name,
                             int64_t timeout_ms = kFastBufferWaitMs) {
  const auto path = dir / name;
  ElapsedTimer timer;
  timer.start();
  std::error_code ec;

  while (!std::filesystem::exists(path, ec)) {
    if (timer.get() > timeout_ms) {
      return false;
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }

  return true;
}

static bool fast_buffer_save(const std::filesystem::path& dir, const std::string& name, const Bytes& bytes) {
  const auto staging = dir / (name + ".part");

  {
    std::ofstream file(staging, std::ios::binary);
    file.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));

    if (!file.good()) {
      return false;
    }
  }

  std::error_code ec;
  std::filesystem::rename(staging, dir / name, ec);
  return !ec;
}

static Bytes fast_buffer_load(const std::filesystem::path& dir, const std::string& name) {
  std::ifstream file(dir / name, std::ios::binary | std::ios::ate);
  const auto size = file.tellg();

  if (!file || size <= 0) {
    return {};
  }

  auto bytes = Bytes::create(static_cast<size_t>(size));
  file.seekg(0);
  file.read(reinterpret_cast<char*>(bytes.data()), size);
  return file ? std::move(bytes) : Bytes{};
}

static void fast_buffer_fill(zerocopy::FastBuffer& buffer, uint8_t value) {
  auto* data = buffer.map(zerocopy::FastBuffer::kWrite);
  REQUIRE(data != nullptr);
  std::memset(data, value, buffer.size());
  REQUIRE(buffer.unmap(zerocopy::FastBuffer::kWrite));
}

static void fast_buffer_expect(const zerocopy::FastBuffer& buffer, uint8_t value) {
  const auto* data = buffer.map(zerocopy::FastBuffer::kRead);
  REQUIRE(data != nullptr);
  CHECK_EQ(data[0], value);
  CHECK_EQ(data[buffer.size() - 1], value);
  REQUIRE(buffer.unmap(zerocopy::FastBuffer::kRead));
}

static std::string fast_buffer_segment_name(const zerocopy::FastBuffer& buffer) {
  // The shared host provider's native handle starts with its NUL-terminated 32-byte segment name.
  const auto* name = static_cast<const char*>(buffer.native_handle());
  REQUIRE(name != nullptr);
  REQUIRE(std::memchr(name, '\0', 32) != nullptr);
  REQUIRE_EQ(std::strncmp(name, "/vfbm_", 6), 0);
  REQUIRE_EQ(std::strlen(name), 22);
  return name;
}

static std::string fast_buffer_control_name(const Bytes& wire) {
  REQUIRE_GE(wire.size(), kFastBufferEnvelope + kFastBufferSystemHandle);
  const auto* name = reinterpret_cast<const char*>(wire.data() + kFastBufferEnvelope - 4);
  REQUIRE(std::memchr(name, '\0', 32) != nullptr);
  REQUIRE_EQ(std::strncmp(name, "/vfb_", 5), 0);
  return name;
}

static void fast_buffer_start_child(Process& child, const std::string& test_case, const std::string& mode,
                                    const std::filesystem::path& dir) {
  child.set_process_mode(Process::kForwardedMode);
  child.set_inherit_environment(true);
  child.set_environment({{"VLINK_FASTBUFFER_PLUGIN", "shm"},
                         {"VLINK_FASTBUFFER_TEST_MODE", mode},
                         {"VLINK_FASTBUFFER_TEST_DIR", dir.string()}});
  child.start(Utils::get_app_path(), {"--test-suite=zerocopy-FastBuffer", "--test-case=" + test_case, "--no-version"});
}

TEST_SUITE("zerocopy-FastBuffer") {
  TEST_CASE("manager resolves the environment once without falling back after explicit failure") {
    const auto env_mode = Utils::get_env("VLINK_FASTBUFFER_MANAGER_TEST");

    if (!env_mode.empty()) {
      zerocopy::FastBuffer buffer;
      const bool expected = env_mode == "empty";
      CHECK_EQ(FastBufferManager::get().is_valid(), expected);
      CHECK_EQ(buffer.create(32), expected);
      REQUIRE(Utils::set_env("VLINK_FASTBUFFER_PLUGIN", expected ? "/vlink-missing-fastbuffer-plugin" : ""));
      CHECK_EQ(FastBufferManager::get().is_valid(), expected);
      CHECK_EQ(buffer.create(64), expected);
      return;
    }

    for (const std::string mode : {"empty", "missing"}) {
      Process child;
      child.set_process_mode(Process::kForwardedMode);
      child.set_inherit_environment(true);
      child.set_environment({{"VLINK_FASTBUFFER_MANAGER_TEST", mode},
                             {"VLINK_FASTBUFFER_PLUGIN", mode == "empty" ? "" : "/vlink-missing-fastbuffer-plugin"}});
      child.start(Utils::get_app_path(),
                  {"--test-suite=zerocopy-FastBuffer",
                   "--test-case=manager resolves the environment once without falling back after explicit failure",
                   "--no-version"});
      REQUIRE(child.wait_for_finished(Process::kDefaultExecuteTimeoutMs));
      CHECK_EQ(child.get_exit_code(), 0);
    }
  }

  TEST_CASE("default provider is CPU memory") {
    CHECK_EQ(sizeof(zerocopy::FastBuffer::MemoryType), 1);
    CHECK_EQ(sizeof(zerocopy::FastBuffer::Access), 1);
    CHECK_EQ(sizeof(zerocopy::FastBuffer::Storage), 1);
    CHECK(FastBufferManager::get().is_valid());
    CHECK_EQ(&FastBufferManager::get(), &FastBufferManager::get());

    zerocopy::FastBuffer buffer;
    CHECK_FALSE(buffer.is_valid());
    CHECK_FALSE(buffer.is_owner());
    CHECK_EQ(buffer.address(), 0);
    CHECK_EQ(buffer.size(), 0);
    CHECK_EQ(buffer.device(), -1);
    CHECK_EQ(buffer.get_serialized_size(), 0);
    CHECK(buffer.get_metadata().empty());
    CHECK_EQ(buffer.get_reserved().size(), 6);
    CHECK_FALSE(buffer.synchronize());
    CHECK_FALSE(buffer.create(0));
    CHECK_FALSE(buffer.import_native(nullptr));
    CHECK_EQ(buffer.native_handle(), nullptr);
    CHECK_EQ(buffer.map(zerocopy::FastBuffer::kRead), nullptr);
    CHECK_FALSE(buffer.unmap(zerocopy::FastBuffer::kRead));

    Bytes bytes;
    CHECK_FALSE((buffer >> bytes));
    CHECK_FALSE(buffer.copy_to_host(bytes));
    CHECK_FALSE(buffer.copy_from_host(bytes));
    CHECK_FALSE((buffer << bytes));
    CHECK_FALSE(zerocopy::FastBuffer::check_valid(bytes));

    for (const size_t size : {size_t{1}, size_t{64}, size_t{4096}, size_t{65536}}) {
      REQUIRE(buffer.create(size));
      CHECK(buffer.is_valid());
      CHECK(buffer.is_owner());
      CHECK_EQ(buffer.memory_type(), zerocopy::FastBuffer::kMemoryHost);
      CHECK_NE(buffer.address(), 0);
      CHECK_NE(buffer.native_handle(), nullptr);
      CHECK(buffer.synchronize());
    }
  }

  TEST_CASE("host mapping and rejected requests preserve the existing allocation") {
    zerocopy::FastBuffer buffer;
    REQUIRE(buffer.create(128));
    auto* data = buffer.map(zerocopy::FastBuffer::kWrite);
    REQUIRE(data != nullptr);
    std::memset(data, 0xA5, buffer.size());
    REQUIRE(buffer.unmap(zerocopy::FastBuffer::kWrite));

    const uintptr_t address = buffer.address();
    CHECK_FALSE(buffer.create(0));
    CHECK_FALSE(buffer.create(32, 0));

    zerocopy::FastBuffer::Config config;
    for (const auto type : {zerocopy::FastBuffer::kMemoryDevice, zerocopy::FastBuffer::kMemoryShared,
                            zerocopy::FastBuffer::kMemoryDmaBuf, zerocopy::FastBuffer::kMemoryVirtual}) {
      config.memory_type = type;
      CHECK_FALSE(buffer.create(32, config));
      CHECK_EQ(buffer.address(), address);
    }

    CHECK_EQ(buffer.map(static_cast<zerocopy::FastBuffer::Access>(0)), nullptr);
    CHECK_FALSE(buffer.unmap(static_cast<zerocopy::FastBuffer::Access>(4)));
    CHECK_FALSE(buffer.import_native(data));

    Bytes output;
    REQUIRE(buffer.copy_to_host(output));
    CHECK_EQ(output.size(), 128);
    CHECK_EQ(output[0], 0xA5);
    auto* output_address = output.data();
    REQUIRE(buffer.copy_to_host(output));
    CHECK_EQ(output.data(), output_address);
    CHECK_FALSE(buffer.copy_from_host(Bytes::create(1)));

    auto input = Bytes::create(128);
    if (!input.empty()) {
      std::memset(input.data(), 0x3C, input.size());
    }
    REQUIRE(buffer.copy_from_host(input));
    const auto* mapped = buffer.map(zerocopy::FastBuffer::kReadWrite);
    REQUIRE(mapped != nullptr);
    CHECK_EQ(mapped[127], 0x3C);
    CHECK(buffer.unmap(zerocopy::FastBuffer::kReadWrite));

    auto wrong_loan = Bytes::loan_internal(output.data(), 1);
    CHECK_FALSE(buffer.copy_to_host(wrong_loan));
    CHECK_FALSE((buffer >> wrong_loan));
    CHECK_EQ(wrong_loan.size(), 1);
  }

  TEST_CASE("shared metadata and payload survive source replacement while deep copies are independent") {
    zerocopy::FastBuffer source;
    REQUIRE(source.create(4096));
    source.header.seq = 19;
    source.get_reserved() = {1, 2, 3, std::numeric_limits<uint64_t>::max(), 5, 6};
    auto input = Bytes::create(source.size());
    if (!input.empty()) {
      std::memset(input.data(), 0x5A, input.size());
    }
    REQUIRE(source.copy_from_host(input));

    auto metadata = Bytes::create(1024);
    if (!metadata.empty()) {
      std::memset(metadata.data(), 0x34, metadata.size());
    }
    const auto* metadata_address = metadata.data();
    REQUIRE(source.set_metadata(std::move(metadata)));
    CHECK(metadata.empty());
    CHECK_EQ(source.get_metadata().data(), metadata_address);

    zerocopy::FastBuffer shared;
    REQUIRE(shared.shallow_copy(source));
    CHECK_EQ(shared.address(), source.address());
    CHECK_EQ(shared.get_metadata().data(), source.get_metadata().data());

    zerocopy::FastBuffer copied(source);
    REQUIRE(copied.is_valid());
    CHECK_NE(copied.address(), source.address());
    CHECK_NE(copied.get_metadata().data(), source.get_metadata().data());
    CHECK(copied.get_metadata() == source.get_metadata());
    CHECK(copied.get_reserved() == source.get_reserved());

    zerocopy::FastBuffer assigned;
    assigned = source;
    CHECK(assigned.get_metadata() == source.get_metadata());
    CHECK_FALSE(source.shallow_copy(source));
    CHECK_FALSE(source.deep_copy(source));
    CHECK_FALSE(source.move_copy(source));

    auto replacement = Bytes::create(128);
    if (!replacement.empty()) {
      std::memset(replacement.data(), 0x76, replacement.size());
    }
    REQUIRE(source.set_metadata(replacement));
    CHECK_EQ(shared.get_metadata().data(), metadata_address);
    source.clear();

    Bytes output;
    REQUIRE(shared.copy_to_host(output));
    CHECK(output == input);
    CHECK_EQ(shared.get_metadata()[0], 0x34);
    CHECK_EQ(shared.get_reserved()[3], std::numeric_limits<uint64_t>::max());

    zerocopy::FastBuffer moved(std::move(shared));
    CHECK_FALSE(shared.is_valid());
    CHECK(shared.get_metadata().empty());
    CHECK_EQ(moved.header.seq, 19);
    CHECK_EQ(moved.get_metadata().data(), metadata_address);
    assigned = std::move(moved);
    CHECK_FALSE(moved.is_valid());
    CHECK_EQ(assigned.get_metadata().data(), metadata_address);

    auto borrowed = Bytes::shallow_copy(replacement.data(), replacement.size());
    REQUIRE(assigned.set_metadata(std::move(borrowed)));
    CHECK_NE(assigned.get_metadata().data(), replacement.data());
    CHECK(assigned.get_metadata() == replacement);
    REQUIRE(assigned.set_metadata(Bytes{}));
    CHECK(assigned.get_metadata().empty());

    zerocopy::FastBuffer empty;
    REQUIRE(assigned.deep_copy(empty));
    CHECK_FALSE(assigned.is_valid());
    REQUIRE(copied.shallow_copy(empty));
    CHECK_FALSE(copied.is_valid());
  }

  TEST_CASE("host wire preserves variable metadata and every reserved slot after source destruction") {
    zerocopy::FastBuffer source;
    REQUIRE(source.create(257));
    source.header.seq = 123;
    source.get_reserved() = {7, 9, 11, 13, 15, 17};
    auto input = Bytes::create(source.size());
    if (!input.empty()) {
      std::memset(input.data(), 0x7B, input.size());
    }
    REQUIRE(source.copy_from_host(input));

    for (const size_t size : {size_t{0}, size_t{3}, size_t{4096}}) {
      auto metadata = Bytes::create(size);
      if (!metadata.empty()) {
        std::memset(metadata.data(), 0x63, metadata.size());
      }
      REQUIRE(source.set_metadata(metadata));

      Bytes wire;
      REQUIRE((source >> wire));
      CHECK_EQ(wire.size(), source.get_serialized_size());
      CHECK_EQ(wire.size(), 132 + source.size() + metadata.size());
      REQUIRE(zerocopy::FastBuffer::check_valid(wire));

      zerocopy::FastBuffer::Metadata decoded;
      REQUIRE(zerocopy::FastBuffer::read_metadata(wire, decoded));
      CHECK_EQ(decoded.storage, zerocopy::FastBuffer::kStorageHost);
      CHECK_EQ(decoded.protocol, 0);
      CHECK_EQ(decoded.size, source.size());
      CHECK(decoded.buffer == metadata);
      CHECK_EQ(decoded.reserved[3], 13);
      CHECK_EQ(decoded.reserved[5], 17);

      zerocopy::FastBuffer received;
      REQUIRE((received << wire));
      wire.clear();
      Bytes output;
      REQUIRE(received.copy_to_host(output));
      CHECK(output == input);
      CHECK(received.get_metadata() == metadata);
      CHECK(received.get_reserved() == source.get_reserved());
      CHECK_EQ(received.header.seq, 123);
    }
  }

  TEST_CASE("malformed wire lengths versions and storage never replace the destination") {
    zerocopy::FastBuffer source;
    REQUIRE(source.create(64));
    auto metadata = Bytes::create(32);
    REQUIRE(source.set_metadata(metadata));
    Bytes wire;
    REQUIRE((source >> wire));
    zerocopy::FastBuffer destination;
    REQUIRE(destination.create(17));
    const uintptr_t original = destination.address();

    for (const size_t length : {size_t{0}, size_t{1}, size_t{131}, wire.size() - 1}) {
      const auto truncated = Bytes::shallow_copy(wire.data(), length);
      CHECK_FALSE((destination << truncated));
      CHECK_EQ(destination.address(), original);
    }

    for (const size_t offset : {size_t{0}, size_t{7}, wire.size() - 1}) {
      Bytes corrupt(wire);
      corrupt[offset] ^= 0xFF;
      CHECK_FALSE((destination << corrupt));
    }

    for (const uint64_t metadata_size : {uint64_t{96}, std::numeric_limits<uint64_t>::max()}) {
      Bytes corrupt(wire);
      std::memcpy(corrupt.data() + 72, &metadata_size, sizeof(metadata_size));
      CHECK_FALSE((destination << corrupt));
    }

    Bytes corrupt(wire);
    uint64_t size = 0;
    std::memcpy(corrupt.data() + 48, &size, sizeof(size));
    CHECK_FALSE((destination << corrupt));

    corrupt = wire;
    uint64_t protocol = 123;
    std::memcpy(corrupt.data() + 56, &protocol, sizeof(protocol));
    CHECK_FALSE((destination << corrupt));
    uint8_t storage = zerocopy::FastBuffer::kStorageShared;
    std::memcpy(corrupt.data() + 64, &storage, sizeof(storage));
    CHECK(zerocopy::FastBuffer::check_valid(corrupt));
    CHECK_FALSE((destination << corrupt));

    protocol = 0;
    std::memcpy(corrupt.data() + 56, &protocol, sizeof(protocol));
    CHECK_FALSE(zerocopy::FastBuffer::check_valid(corrupt));
    storage = 99;
    std::memcpy(corrupt.data() + 64, &storage, sizeof(storage));
    CHECK_FALSE(zerocopy::FastBuffer::check_valid(corrupt));
    CHECK_EQ(destination.address(), original);
  }

  TEST_CASE("message parser exposes metadata bytes and all reserved slots without importing payload") {
    zerocopy::FastBuffer source;
    REQUIRE(source.create(32));
    auto metadata = Bytes::create(256);
    if (!metadata.empty()) {
      std::memset(metadata.data(), 0x42, metadata.size());
    }
    REQUIRE(source.set_metadata(metadata));
    source.get_reserved() = {20, 21, 22, 23, 24, 25};
    Bytes wire;
    REQUIRE((source >> wire));

    zerocopy::MessageParser parser;
    REQUIRE(parser.parse("vlink::zerocopy::FastBuffer", wire));
    CHECK_EQ(parser.type(), zerocopy::MessageParser::kFastBuffer);
    CHECK_EQ(parser.type_name(parser.type()), "FastBuffer");
    CHECK_EQ(parser.collection_size("reserved"), 0);
    CHECK_EQ(parser.element_fields("reserved").size(), 0);
    CHECK_EQ(parser.collection_size("data"), 0);

    zerocopy::MessageParser::Value value;
    REQUIRE(parser.value("metadata", value));
    CHECK(std::get<Bytes>(value) == metadata);
    REQUIRE(parser.value("metadata_size", value));
    CHECK_EQ(std::get<uint64_t>(value), 256);
    const std::array<std::string_view, 6> reserved_names = {"reserved",  "reserved2", "reserved3",
                                                            "reserved4", "reserved5", "reserved6"};

    for (size_t index = 0; index < reserved_names.size(); ++index) {
      REQUIRE(parser.value(reserved_names[index], value));
      CHECK_EQ(std::get<uint64_t>(value), 20 + index);
    }

    size_t reserved_count = 0;

    for (const auto& field : parser.fields()) {
      if (field.is_reserved && field.name.find("reserved") == 0) {
        ++reserved_count;
      }
    }

    CHECK_EQ(reserved_count, 6);
    CHECK_FALSE(parser.value("reserved[6]", value));
    CHECK_FALSE(parser.value("reserved[0].invalid", value));
    CHECK_FALSE(parser.value("data", value));
    CHECK_FALSE(parser.value("unknown", value));
    const auto printed = zerocopy::format_message(parser, {});
    CHECK(printed.find("metadata") != std::string::npos);
    CHECK(printed.find("memory_type") != std::string::npos);
    CHECK(printed.find("reserved4") == std::string::npos);
    zerocopy::MessageFormatOptions options;
    options.enum_name = true;
    const auto named = zerocopy::format_message(parser, options);
    CHECK(named.find("kStorageHost") != std::string::npos);
    CHECK(named.find("kMemoryHost") != std::string::npos);
    zerocopy::FastBuffer::Metadata copied;
    REQUIRE(parser.copy_to(copied));
    parser.clear();
    wire.clear();
    source.clear();
    CHECK(copied.buffer == metadata);
    CHECK_EQ(copied.reserved[5], 25);
    CHECK_FALSE(parser.valid());
  }

  TEST_CASE("pool recycles CPU slots without sharing state") {
    zerocopy::FastBufferPool pool;
    CHECK_FALSE(pool.create(0, 1));
    CHECK_FALSE(pool.create(16, 0));
    CHECK_FALSE(pool.create(1, std::numeric_limits<size_t>::max()));
    CHECK_EQ(pool.depth(), 0);
    CHECK_EQ(pool.size(), 0);

    zerocopy::FastBuffer first;
    CHECK_FALSE(pool.acquire(first));
    REQUIRE(pool.create(16, 2));
    CHECK_EQ(pool.depth(), 2);
    CHECK_EQ(pool.size(), 16);

    zerocopy::FastBuffer second;
    zerocopy::FastBuffer third;
    REQUIRE(pool.acquire(first));
    REQUIRE(pool.acquire(second));
    CHECK(first.is_valid());
    CHECK_NE(first.address(), second.address());
    CHECK_FALSE(pool.acquire(third));
    const uintptr_t first_address = first.address();

    zerocopy::FastBuffer retained;
    REQUIRE(retained.shallow_copy(first));
    first.clear();
    CHECK_FALSE(pool.acquire(third));
    retained.clear();
    REQUIRE(pool.acquire(third));
    CHECK_EQ(third.address(), first_address);
    fast_buffer_fill(third, 0x11);

    Bytes wire;
    REQUIRE((third >> wire));
    CHECK_EQ(wire.size(), kFastBufferEnvelope + 16);
    REQUIRE(pool.create(8, 1));
    CHECK_EQ(pool.size(), 8);
    CHECK(second.is_valid());
    fast_buffer_expect(third, 0x11);
    pool.clear();
    CHECK_EQ(pool.depth(), 0);
    CHECK(third.is_valid());
    CHECK_FALSE(pool.acquire(third));
    CHECK_FALSE(third.is_valid());
  }

#if !defined(__ANDROID__)
  TEST_CASE("retired and reused allocations reject expired descriptors in process") {
    if (Utils::get_env("VLINK_FASTBUFFER_TEST_MODE") == "expire") {
      zerocopy::FastBufferPool pool;
      REQUIRE(pool.create(64, 1));

      zerocopy::FastBuffer first;
      REQUIRE(pool.acquire(first));
      CHECK_EQ(first.memory_type(), zerocopy::FastBuffer::kMemoryShared);
      const uintptr_t slot_address = first.address();
      fast_buffer_fill(first, 0x5A);

      Bytes wire_first;
      REQUIRE((first >> wire_first));
      CHECK_EQ(wire_first.size(), kFastBufferEnvelope + kFastBufferSystemHandle);
      CHECK_EQ(first.get_serialized_size(), wire_first.size());

      zerocopy::FastBuffer same;
      REQUIRE((same << wire_first));
      CHECK_EQ(same.address(), slot_address);
      fast_buffer_expect(same, 0x5A);

      zerocopy::FastBuffer blocked;
      CHECK_FALSE(pool.acquire(blocked));
      first.clear();
      CHECK_FALSE(pool.acquire(blocked));
      same.clear();

      zerocopy::FastBuffer second;
      REQUIRE(pool.acquire(second));
      CHECK_EQ(second.address(), slot_address);
      fast_buffer_fill(second, 0x3C);

      zerocopy::FastBuffer expired;
      CHECK_FALSE((expired << wire_first));
      CHECK_FALSE(expired.is_valid());

      Bytes wire_second;
      REQUIRE((second >> wire_second));
      zerocopy::FastBuffer imported;
      REQUIRE((imported << wire_second));
      fast_buffer_expect(imported, 0x3C);
      CHECK_FALSE(pool.acquire(second));
      CHECK_FALSE(second.is_valid());
      imported.clear();
      REQUIRE(pool.acquire(second));
      CHECK_FALSE((imported << wire_second));

      zerocopy::FastBuffer standalone;
      REQUIRE(standalone.create(32));
      Bytes wire_standalone;
      REQUIRE((standalone >> wire_standalone));
      standalone.clear();

      zerocopy::FastBuffer late;
      CHECK_FALSE((late << wire_standalone));

      pool.clear();
      CHECK(second.is_valid());
      fast_buffer_expect(second, 0x3C);
      second.clear();
      return;
    }

    const FastBufferTestDir dir("vlink_fb_expire");
    Process child;
    fast_buffer_start_child(child, "retired and reused allocations reject expired descriptors in process", "expire",
                            dir.path());
    REQUIRE(child.wait_for_finished(static_cast<int>(kFastBufferWaitMs)));
    CHECK_EQ(child.get_exit_code(), 0);
  }

  TEST_CASE("system sharing imports across processes and reuses slots by generation") {
    static constexpr const char* kCase = "system sharing imports across processes and reuses slots by generation";
    const auto mode = Utils::get_env("VLINK_FASTBUFFER_TEST_MODE");

    if (mode == "share-owner") {
      Logger::get();

      const auto dir = fast_buffer_child_dir();
      zerocopy::FastBufferPool pool;
      REQUIRE(pool.create(4096, 2));

      zerocopy::FastBuffer first;
      REQUIRE(pool.acquire(first));
      first.header.seq = 7;
      first.get_reserved()[5] = 99;
      REQUIRE(first.set_metadata(Bytes::create(16)));
      fast_buffer_fill(first, 0x5A);
      Bytes wire;
      REQUIRE((first >> wire));
      CHECK_EQ(wire.size(), kFastBufferEnvelope + 16 + kFastBufferSystemHandle);
      REQUIRE(fast_buffer_save(dir, "wire1", wire));

      zerocopy::FastBuffer second;
      REQUIRE(pool.acquire(second));
      fast_buffer_fill(second, 0x3C);
      REQUIRE((second >> wire));
      REQUIRE(fast_buffer_save(dir, "wire2", wire));
      first.clear();
      second.clear();

      REQUIRE(fast_buffer_wait(dir, "imported"));
      zerocopy::FastBuffer third;
      CHECK_FALSE(pool.acquire(third));
      REQUIRE(fast_buffer_touch(dir, "owner_checked"));

      REQUIRE(fast_buffer_wait(dir, "released"));
      REQUIRE(pool.acquire(third));
      fast_buffer_fill(third, 0x77);
      REQUIRE((third >> wire));
      REQUIRE(fast_buffer_save(dir, "wire3", wire));
      third.clear();

      zerocopy::FastBuffer standalone;
      REQUIRE(standalone.create(512));
      fast_buffer_fill(standalone, 0x11);
      REQUIRE((standalone >> wire));
      REQUIRE(fast_buffer_save(dir, "wire4", wire));
      REQUIRE(fast_buffer_wait(dir, "held4"));
      standalone.clear();
      REQUIRE(standalone.create(512));
      REQUIRE(fast_buffer_touch(dir, "retired4"));
      REQUIRE(fast_buffer_wait(dir, "released4"));
      standalone.clear();
      REQUIRE(standalone.create(512));
      REQUIRE(fast_buffer_touch(dir, "reclaimed4"));

      fast_buffer_fill(standalone, 0x22);
      const std::string segment = fast_buffer_segment_name(standalone);
      REQUIRE(fast_buffer_save(
          dir, "segment5", Bytes::shallow_copy(reinterpret_cast<const uint8_t*>(segment.data()), segment.size() + 1)));
      REQUIRE((standalone >> wire));
      REQUIRE(fast_buffer_save(dir, "wire5", wire));
      REQUIRE(fast_buffer_wait(dir, "held5"));
      return;
    }

    if (mode == "share-reader") {
      const auto dir = fast_buffer_child_dir();
      REQUIRE(fast_buffer_wait(dir, "wire1"));
      REQUIRE(fast_buffer_wait(dir, "wire2"));
      const Bytes wire_first = fast_buffer_load(dir, "wire1");
      const Bytes wire_second = fast_buffer_load(dir, "wire2");

      zerocopy::FastBuffer first;
      zerocopy::FastBuffer second;
      REQUIRE((first << wire_first));
      REQUIRE((second << wire_second));
      CHECK_EQ(first.memory_type(), zerocopy::FastBuffer::kMemoryShared);
      CHECK_EQ(first.header.seq, 7);
      CHECK_EQ(first.get_reserved()[5], 99);
      CHECK_EQ(first.get_metadata().size(), 16);
      CHECK_NE(first.address(), second.address());
      fast_buffer_expect(first, 0x5A);
      fast_buffer_expect(second, 0x3C);

      zerocopy::FastBuffer again;
      REQUIRE((again << wire_first));
      CHECK_EQ(again.address(), first.address());
      REQUIRE(fast_buffer_touch(dir, "imported"));

      REQUIRE(fast_buffer_wait(dir, "owner_checked"));
      first.clear();
      again.clear();
      second.clear();
      REQUIRE(fast_buffer_touch(dir, "released"));

      REQUIRE(fast_buffer_wait(dir, "wire3"));
      zerocopy::FastBuffer expired;
      CHECK_FALSE((expired << wire_first));
      zerocopy::FastBuffer third;
      REQUIRE((third << fast_buffer_load(dir, "wire3")));
      fast_buffer_expect(third, 0x77);
      third.clear();

      REQUIRE(fast_buffer_wait(dir, "wire4"));
      const Bytes wire_fourth = fast_buffer_load(dir, "wire4");
      const std::string control_fourth = fast_buffer_control_name(wire_fourth);
      zerocopy::FastBuffer fourth;
      REQUIRE((fourth << wire_fourth));
      fast_buffer_expect(fourth, 0x11);
      Bytes relay;
      REQUIRE((fourth >> relay));
      zerocopy::FastBuffer relayed;
      REQUIRE((relayed << relay));
      CHECK_EQ(relayed.address(), fourth.address());
      relayed.clear();
      REQUIRE(fast_buffer_touch(dir, "held4"));
      REQUIRE(fast_buffer_wait(dir, "retired4"));
      fast_buffer_expect(fourth, 0x11);
      REQUIRE((fourth >> relay));
      CHECK_FALSE((relayed << relay));
      SysSharemem shared;
      REQUIRE(shared.attach(control_fourth));
      REQUIRE(shared.detach(false));
      fourth.clear();
      REQUIRE(fast_buffer_touch(dir, "released4"));
      REQUIRE(fast_buffer_wait(dir, "reclaimed4"));
      CHECK_FALSE(shared.attach(control_fourth));

      REQUIRE(fast_buffer_wait(dir, "wire5"));
      const Bytes wire_fifth = fast_buffer_load(dir, "wire5");
      const std::string control_fifth = fast_buffer_control_name(wire_fifth);
      const Bytes segment_fifth = fast_buffer_load(dir, "segment5");
      REQUIRE_EQ(segment_fifth.size(), 23);
      zerocopy::FastBuffer fifth;
      REQUIRE((fifth << wire_fifth));
      fast_buffer_expect(fifth, 0x22);
      REQUIRE(fast_buffer_touch(dir, "held5"));
      REQUIRE(fast_buffer_wait(dir, "owner_exited"));
      fast_buffer_expect(fifth, 0x22);
#if !defined(_WIN32)
      CHECK_FALSE(shared.attach(control_fifth));
      CHECK_FALSE(shared.attach(reinterpret_cast<const char*>(segment_fifth.data())));
#endif
      fifth.clear();
      return;
    }

    const FastBufferTestDir dir("vlink_fb_share");
    Process owner;
    Process reader;
    fast_buffer_start_child(owner, kCase, "share-owner", dir.path());
    fast_buffer_start_child(reader, kCase, "share-reader", dir.path());
    REQUIRE(owner.wait_for_finished(static_cast<int>(kFastBufferWaitMs)));
    CHECK_EQ(owner.get_exit_code(), 0);
    REQUIRE(fast_buffer_touch(dir.path(), "owner_exited"));
    REQUIRE(reader.wait_for_finished(static_cast<int>(kFastBufferWaitMs)));
    CHECK_EQ(reader.get_exit_code(), 0);
  }

  TEST_CASE("crashed reader is purged on the owner's next acquire") {
    static constexpr const char* kCase = "crashed reader is purged on the owner's next acquire";
    const auto mode = Utils::get_env("VLINK_FASTBUFFER_TEST_MODE");

    if (mode == "crash-owner") {
      const auto dir = fast_buffer_child_dir();
      zerocopy::FastBufferPool pool;
      REQUIRE(pool.create(256, 1));

      zerocopy::FastBuffer frame;
      REQUIRE(pool.acquire(frame));
      fast_buffer_fill(frame, 0x42);
      Bytes wire;
      REQUIRE((frame >> wire));
      REQUIRE(fast_buffer_save(dir, "wire1", wire));
      frame.clear();

      REQUIRE(fast_buffer_wait(dir, "imported"));
      CHECK_FALSE(pool.acquire(frame));
      REQUIRE(fast_buffer_touch(dir, "owner_checked"));

      REQUIRE(fast_buffer_wait(dir, "crashed"));
      REQUIRE(pool.acquire(frame));
      fast_buffer_fill(frame, 0x24);
      frame.clear();
      return;
    }

    if (mode == "crash-reader") {
      const auto dir = fast_buffer_child_dir();
      REQUIRE(fast_buffer_wait(dir, "wire1"));
      zerocopy::FastBuffer frame;
      REQUIRE((frame << fast_buffer_load(dir, "wire1")));
      fast_buffer_expect(frame, 0x42);
      REQUIRE(fast_buffer_touch(dir, "imported"));
      REQUIRE(fast_buffer_wait(dir, "never", 60000));
      return;
    }

    const FastBufferTestDir dir("vlink_fb_crash");
    Process owner;
    Process reader;
    fast_buffer_start_child(owner, kCase, "crash-owner", dir.path());
    fast_buffer_start_child(reader, kCase, "crash-reader", dir.path());
    REQUIRE(fast_buffer_wait(dir.path(), "owner_checked"));
    reader.kill();
    REQUIRE(reader.wait_for_finished(static_cast<int>(kFastBufferWaitMs)));
    REQUIRE(fast_buffer_touch(dir.path(), "crashed"));
    REQUIRE(owner.wait_for_finished(static_cast<int>(kFastBufferWaitMs)));
    CHECK_EQ(owner.get_exit_code(), 0);
  }

  TEST_CASE("crashed owner fails late imports and loses its named segments") {
    static constexpr const char* kCase = "crashed owner fails late imports and loses its named segments";
    const auto mode = Utils::get_env("VLINK_FASTBUFFER_TEST_MODE");

    if (mode == "dead-owner") {
      const auto dir = fast_buffer_child_dir();
      zerocopy::FastBuffer frame;
      REQUIRE(frame.create(128));
      Bytes wire;
      REQUIRE((frame >> wire));
      REQUIRE(fast_buffer_save(dir, "wire1", wire));
      const std::string segment = fast_buffer_segment_name(frame);
      REQUIRE(fast_buffer_save(
          dir, "segment", Bytes::shallow_copy(reinterpret_cast<const uint8_t*>(segment.data()), segment.size() + 1)));
      REQUIRE(fast_buffer_touch(dir, "exported"));
      REQUIRE(fast_buffer_wait(dir, "never", 60000));
      return;
    }

    if (mode == "dead-reader" || mode == "dead-sweeper") {
      const auto dir = fast_buffer_child_dir();
      zerocopy::FastBuffer frame;

      if (mode == "dead-reader") {
        REQUIRE(fast_buffer_touch(dir, "ready"));
        REQUIRE(fast_buffer_wait(dir, "crashed"));
      }

      const Bytes wire = fast_buffer_load(dir, "wire1");
      REQUIRE_EQ(wire.size(), kFastBufferEnvelope + kFastBufferSystemHandle);

      if (mode == "dead-reader") {
        CHECK_FALSE((frame << wire));
      }

      const std::string control = fast_buffer_control_name(wire);
      const Bytes segment = fast_buffer_load(dir, "segment");
      REQUIRE_EQ(segment.size(), 23);
      SysSharemem shared;
      CHECK_FALSE(shared.attach(control));
      CHECK_FALSE(shared.attach(reinterpret_cast<const char*>(segment.data())));
      return;
    }

    {
      const FastBufferTestDir dir("vlink_fb_dead");
      Process reader;
      fast_buffer_start_child(reader, kCase, "dead-reader", dir.path());
      REQUIRE(fast_buffer_wait(dir.path(), "ready"));

      Process owner;
      fast_buffer_start_child(owner, kCase, "dead-owner", dir.path());
      REQUIRE(fast_buffer_wait(dir.path(), "exported"));
      owner.kill();
      REQUIRE(owner.wait_for_finished(static_cast<int>(kFastBufferWaitMs)));
      REQUIRE(fast_buffer_touch(dir.path(), "crashed"));
      REQUIRE(reader.wait_for_finished(static_cast<int>(kFastBufferWaitMs)));
      CHECK_EQ(reader.get_exit_code(), 0);
    }

#if defined(__linux__) || defined(__QNX__)
    {
      const FastBufferTestDir dir("vlink_fb_sweep");
      Process owner;
      fast_buffer_start_child(owner, kCase, "dead-owner", dir.path());
      REQUIRE(fast_buffer_wait(dir.path(), "exported"));
      owner.kill();
      REQUIRE(owner.wait_for_finished(static_cast<int>(kFastBufferWaitMs)));

      Process sweeper;
      fast_buffer_start_child(sweeper, kCase, "dead-sweeper", dir.path());
      REQUIRE(sweeper.wait_for_finished(static_cast<int>(kFastBufferWaitMs)));
      CHECK_EQ(sweeper.get_exit_code(), 0);
    }
#endif
  }
#endif
}

// NOLINTEND
