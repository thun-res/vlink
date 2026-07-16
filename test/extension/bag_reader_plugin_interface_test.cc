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

#include <doctest/doctest.h>

#include <optional>
#include <string>
#include <type_traits>

#include "../common_test.h"
#include "./extension/bag_plugin_interface.h"

namespace {

class FakePlugin final : public BagPluginInterface {
 public:
  FakePlugin() = default;

  bool convert_url_meta(std::string& url, std::string& ser_type, SchemaType& schema_type) override {
    called_convert = true;
    captured_url = url;
    captured_ser_type = ser_type;
    captured_schema_type = schema_type;

    if (remap_to.has_value()) {
      url = *remap_to;
    }

    return accept;
  }

  void on_read(const Frame& frame) override {
    called_push = true;
    captured_ts = frame.timestamp;
    captured_push_url = frame.url;
    captured_action = frame.action_type;
    captured_size = frame.data.size();

    do_callback(frame);
  }

  void on_write(const Frame& frame) override {
    called_write = true;
    captured_ts = frame.timestamp;
    captured_push_url = frame.url;
    captured_ser_type = frame.ser_type;
    captured_schema_type = frame.schema_type;
    captured_action = frame.action_type;
    captured_size = frame.data.size();

    do_callback(frame);
  }

  bool called_convert{false};
  bool called_push{false};
  bool called_write{false};
  bool accept{true};
  std::string captured_url;
  std::string captured_ser_type;
  SchemaType captured_schema_type{SchemaType::kUnknown};
  std::optional<std::string> remap_to;
  int64_t captured_ts{0};
  std::string captured_push_url;
  ActionType captured_action{ActionType::kUnknownAction};
  size_t captured_size{0};
};

class DefaultPlugin final : public BagPluginInterface {
 public:
  void on_read(const Frame& frame) override { do_callback(frame); }

  void on_write(const Frame& frame) override { do_callback(frame); }
};

Frame frame_of(int64_t timestamp, const std::string& url, ActionType action_type, const Bytes& data,
               const std::string& ser_type = "", SchemaType schema_type = SchemaType::kUnknown) {
  Frame frame;
  frame.timestamp = timestamp;
  frame.url = url;
  frame.ser_type = ser_type;
  frame.schema_type = schema_type;
  frame.action_type = action_type;
  frame.data = data;

  return frame;
}

}  // namespace

TEST_SUITE("extension-BagPluginInterface") {
  TEST_CASE("interface is abstract and non-copyable") {
    CHECK(std::is_abstract_v<BagPluginInterface>);
    CHECK_FALSE(std::is_copy_constructible_v<BagPluginInterface>);
    CHECK_FALSE(std::is_copy_assignable_v<BagPluginInterface>);
  }

  TEST_CASE("virtual dispatch via base pointer works") {
    FakePlugin concrete;
    BagPluginInterface* base = &concrete;

    Bytes data = Bytes::create(4u);
    base->on_read(frame_of(4242, "dds://x", ActionType::kPublish, data));

    CHECK(concrete.called_push);
    CHECK_EQ(concrete.captured_ts, 4242);
  }

  TEST_CASE("convert_url_meta receives arguments by reference") {
    FakePlugin plugin;
    std::string url = "intra://sensor/lidar";
    std::string ser_type = "proto.Lidar";
    SchemaType schema_type = SchemaType::kProtobuf;

    bool ok = plugin.convert_url_meta(url, ser_type, schema_type);

    CHECK(ok);
    CHECK(plugin.called_convert);
    CHECK_EQ(plugin.captured_url, "intra://sensor/lidar");
    CHECK_EQ(plugin.captured_ser_type, "proto.Lidar");
    CHECK_EQ(plugin.captured_schema_type, SchemaType::kProtobuf);
  }

  TEST_CASE("convert_url_meta can rewrite url in-place") {
    FakePlugin plugin;
    plugin.remap_to = "dds://vehicle/lidar";

    std::string url = "intra://sensor/lidar";
    std::string ser_type = "proto.Lidar";
    SchemaType schema_type = SchemaType::kProtobuf;

    CHECK(plugin.convert_url_meta(url, ser_type, schema_type));
    CHECK_EQ(url, "dds://vehicle/lidar");
  }

  TEST_CASE("convert_url_meta can exclude a url by returning false") {
    FakePlugin plugin;
    plugin.accept = false;

    std::string url = "intra://x";
    std::string ser_type = "raw";
    SchemaType schema_type = SchemaType::kUnknown;

    CHECK_FALSE(plugin.convert_url_meta(url, ser_type, schema_type));
  }

  TEST_CASE("on_read records message metadata") {
    FakePlugin plugin;
    Bytes data = Bytes::create(8u);

    plugin.on_read(frame_of(9999, "dds://topic", ActionType::kPublish, data));

    CHECK(plugin.called_push);
    CHECK_EQ(plugin.captured_ts, 9999);
    CHECK_EQ(plugin.captured_push_url, "dds://topic");
    CHECK_EQ(plugin.captured_action, ActionType::kPublish);
    CHECK_EQ(plugin.captured_size, data.size());
  }

  TEST_CASE("register_callback is invoked from on_read") {
    FakePlugin plugin;
    int call_count = 0;
    int64_t observed_ts = -1;
    std::string observed_url;

    plugin.register_callback([&](const Frame& frame) {
      ++call_count;
      observed_ts = frame.timestamp;
      observed_url = frame.url;
    });

    Bytes data = Bytes::create(4u);
    plugin.on_read(frame_of(7777, "intra://x", ActionType::kSubscribe, data));

    CHECK_EQ(call_count, 1);
    CHECK_EQ(observed_ts, 7777);
    CHECK_EQ(observed_url, "intra://x");
  }

  TEST_CASE("register_callback replaces previously registered callback") {
    FakePlugin plugin;
    int first_count = 0;
    int second_count = 0;

    plugin.register_callback([&](const Frame&) { ++first_count; });
    plugin.register_callback([&](const Frame&) { ++second_count; });

    Bytes data = Bytes::create(1u);
    plugin.on_read(frame_of(0, "u", ActionType::kPublish, data));

    CHECK_EQ(first_count, 0);
    CHECK_EQ(second_count, 1);
  }

  TEST_CASE("on_read without registered callback does not crash") {
    FakePlugin plugin;
    Bytes data = Bytes::create(4u);
    plugin.on_read(frame_of(1, "intra://x", ActionType::kPublish, data));
    CHECK(plugin.called_push);
  }

  TEST_CASE("do_callback forwards directly to the registered callback") {
    FakePlugin plugin;
    int64_t observed_ts = -1;
    std::string observed_url;

    plugin.register_callback([&](const Frame& frame) {
      observed_ts = frame.timestamp;
      observed_url = frame.url;
    });

    Bytes data = Bytes::create(2u);
    plugin.do_callback(frame_of(321, "intra://direct", ActionType::kPublish, data));

    CHECK_FALSE(plugin.called_push);
    CHECK_EQ(observed_ts, 321);
    CHECK_EQ(observed_url, "intra://direct");
  }

  TEST_CASE("register_callback is invoked from on_write") {
    FakePlugin plugin;
    int call_count = 0;
    int64_t observed_ts = -1;
    std::string observed_url;
    std::string observed_ser_type;
    SchemaType observed_schema_type = SchemaType::kUnknown;

    plugin.register_callback([&](const Frame& frame) {
      ++call_count;
      observed_ts = frame.timestamp;
      observed_url = frame.url;
      observed_ser_type = frame.ser_type;
      observed_schema_type = frame.schema_type;
    });

    Bytes data = Bytes::create(4u);
    plugin.on_write(frame_of(8888, "dds://jpeg", ActionType::kPublish, data, "jpeg", SchemaType::kProtobuf));

    CHECK(plugin.called_write);
    CHECK_EQ(call_count, 1);
    CHECK_EQ(observed_ts, 8888);
    CHECK_EQ(observed_url, "dds://jpeg");
    CHECK_EQ(observed_ser_type, "jpeg");
    CHECK_EQ(observed_schema_type, SchemaType::kProtobuf);
  }

  TEST_CASE("on_write without registered callback does not crash") {
    FakePlugin plugin;
    Bytes data = Bytes::create(4u);
    plugin.on_write(frame_of(1, "dds://x", ActionType::kPublish, data, "raw", SchemaType::kUnknown));
    CHECK(plugin.called_write);
  }

  TEST_CASE("default on_read forwards unchanged through do_callback to the registered callback") {
    DefaultPlugin plugin;
    int call_count = 0;
    int64_t observed_ts = -1;

    plugin.register_callback([&](const Frame& frame) {
      ++call_count;
      observed_ts = frame.timestamp;
    });

    Bytes data = Bytes::create(4u);
    plugin.on_read(frame_of(1234, "intra://x", ActionType::kPublish, data));

    CHECK_EQ(call_count, 1);
    CHECK_EQ(observed_ts, 1234);
  }

  TEST_CASE("default on_write forwards unchanged through do_callback to the registered callback") {
    DefaultPlugin plugin;
    int call_count = 0;
    std::string observed_url;
    std::string observed_ser_type;

    plugin.register_callback([&](const Frame& frame) {
      ++call_count;
      observed_url = frame.url;
      observed_ser_type = frame.ser_type;
    });

    Bytes data = Bytes::create(4u);
    plugin.on_write(frame_of(5, "dds://raw", ActionType::kPublish, data, "raw", SchemaType::kUnknown));

    CHECK_EQ(call_count, 1);
    CHECK_EQ(observed_url, "dds://raw");
    CHECK_EQ(observed_ser_type, "raw");
  }

  TEST_CASE("default reset and flush hooks are no-ops") {
    DefaultPlugin plugin;
    CHECK_NOTHROW(plugin.reset());
    CHECK_NOTHROW(plugin.flush());
  }
}

// NOLINTEND
