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

#include <cstring>
#include <limits>
#include <utility>

#include "../common_test.h"
#include "./base/process.h"
#include "./zerocopy/fast_buffer_manager.h"
#include "./zerocopy/message_parser.h"

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
}

// NOLINTEND
