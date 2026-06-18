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

#include "./extension/convert_plugin_interface.h"

#include <doctest/doctest.h>

#include <cstdint>
#include <string>
#include <type_traits>

#include "../common_test.h"

namespace {

using FrontendChannel = ConvertPluginInterface::FrontendChannel;
using PublishInfo = ConvertPluginInterface::PublishInfo;
using SchemaInfo = ConvertPluginInterface::SchemaInfo;
using Target = ConvertPluginInterface::Target;

class FakeConvertPlugin final : public ConvertPluginInterface {
 public:
  FakeConvertPlugin() = default;

  bool init(const std::string& config) override {
    init_called = true;
    last_config = config;
    return init_result;
  }

  bool can_convert(const std::string& ser_type, Target target) override {
    return ser_type == handled_ser && (target == Target::kFoxglove || target == Target::kRerun);
  }

  bool get_schema(const std::string& ser_type, Target target, SchemaInfo& schema_info) override {
    if (ser_type != handled_ser) {
      return false;
    }

    if (target == Target::kFoxglove) {
      schema_info.type_name = "foxglove.Test";
      schema_info.encoding = "flatbuffers";
      schema_info.schema_encoding = "flatbuffers";
      schema_info.schema_data = "BFBS_BYTES";
    } else {
      schema_info.type_name = "Points3D";
      schema_info.encoding = "json";
      schema_info.schema_encoding.clear();
      schema_info.schema_data.clear();
    }

    return true;
  }

  bool convert(const std::string& ser_type, const Bytes& raw, Target target, Bytes& payload) override {
    if (ser_type != handled_ser) {
      return false;
    }

    last_target = target;
    last_input_size = raw.size();
    payload = Bytes::create(output_size);
    return true;
  }

  bool init_called{false};
  bool init_result{true};
  std::string last_config;
  std::string handled_ser{"my_pkg.MyMessage"};
  Target last_target{Target::kFoxglove};
  size_t last_input_size{0};
  size_t output_size{16u};
};

}  // namespace

TEST_SUITE("extension-ConvertPluginInterface") {
  TEST_CASE("convert target enum values are sequential and distinct") {
    CHECK_EQ(static_cast<uint8_t>(Target::kFoxglove), 0u);
    CHECK_EQ(static_cast<uint8_t>(Target::kRerun), 1u);
    CHECK_NE(Target::kFoxglove, Target::kRerun);
  }

  TEST_CASE("web channel default construction yields all empty fields") {
    FrontendChannel ch;
    CHECK(ch.topic.empty());
    CHECK(ch.encoding.empty());
    CHECK(ch.schema_name.empty());
    CHECK(ch.schema_encoding.empty());
    CHECK(ch.schema.empty());
  }

  TEST_CASE("web channel fields are independently mutable") {
    FrontendChannel ch;
    ch.topic = "/cmd";
    ch.encoding = "json";
    ch.schema_name = "Cmd";
    ch.schema_encoding = "jsonschema";
    ch.schema = "{}";
    CHECK_EQ(ch.topic, "/cmd");
    CHECK_EQ(ch.encoding, "json");
    CHECK_EQ(ch.schema_name, "Cmd");
    CHECK_EQ(ch.schema_encoding, "jsonschema");
    CHECK_EQ(ch.schema, "{}");
  }

  TEST_CASE("vlink publish default construction yields kUnknown schema type") {
    PublishInfo p;
    CHECK(p.url.empty());
    CHECK(p.ser_type.empty());
    CHECK_EQ(p.schema_type, SchemaType::kUnknown);
  }

  TEST_CASE("interface is abstract and non-copyable") {
    CHECK(std::is_abstract_v<ConvertPluginInterface>);
    CHECK_FALSE(std::is_copy_constructible_v<ConvertPluginInterface>);
    CHECK_FALSE(std::is_copy_assignable_v<ConvertPluginInterface>);
  }

  TEST_CASE("init forwards config string and returns plugin's return value") {
    FakeConvertPlugin plugin;
    CHECK(plugin.init("{\"key\":\"value\"}"));
    CHECK(plugin.init_called);
    CHECK_EQ(plugin.last_config, "{\"key\":\"value\"}");
  }

  TEST_CASE("init returns false when plugin signals failure") {
    FakeConvertPlugin plugin;
    plugin.init_result = false;
    CHECK_FALSE(plugin.init(""));
  }

  TEST_CASE("can_convert accepts handled type for both targets") {
    FakeConvertPlugin plugin;
    CHECK(plugin.can_convert("my_pkg.MyMessage", Target::kFoxglove));
    CHECK(plugin.can_convert("my_pkg.MyMessage", Target::kRerun));
    CHECK_FALSE(plugin.can_convert("other.Type", Target::kFoxglove));
    CHECK_FALSE(plugin.can_convert("other.Type", Target::kRerun));
  }

  TEST_CASE("get_schema fills foxglove schema fields for kFoxglove") {
    FakeConvertPlugin plugin;
    SchemaInfo schema_info;

    CHECK(plugin.get_schema("my_pkg.MyMessage", Target::kFoxglove, schema_info));
    CHECK_EQ(schema_info.type_name, "foxglove.Test");
    CHECK_EQ(schema_info.encoding, "flatbuffers");
    CHECK_EQ(schema_info.schema_encoding, "flatbuffers");
    CHECK_EQ(schema_info.schema_data, "BFBS_BYTES");
  }

  TEST_CASE("get_schema clears schema fields for kRerun") {
    FakeConvertPlugin plugin;
    SchemaInfo schema_info;
    schema_info.schema_encoding = "preset";
    schema_info.schema_data = "preset";

    CHECK(plugin.get_schema("my_pkg.MyMessage", Target::kRerun, schema_info));
    CHECK_EQ(schema_info.type_name, "Points3D");
    CHECK_EQ(schema_info.encoding, "json");
    CHECK(schema_info.schema_encoding.empty());
    CHECK(schema_info.schema_data.empty());
  }

  TEST_CASE("get_schema returns false for unhandled type") {
    FakeConvertPlugin plugin;
    SchemaInfo schema_info;
    CHECK_FALSE(plugin.get_schema("other.Type", Target::kFoxglove, schema_info));
  }

  TEST_CASE("convert produces payload of expected size and records input info") {
    FakeConvertPlugin plugin;
    Bytes raw = Bytes::create(32u);
    Bytes payload;

    CHECK(plugin.convert("my_pkg.MyMessage", raw, Target::kFoxglove, payload));
    CHECK_EQ(plugin.last_input_size, 32u);
    CHECK_EQ(plugin.last_target, Target::kFoxglove);
    CHECK_EQ(payload.size(), 16u);
  }

  TEST_CASE("convert returns false for unhandled type") {
    FakeConvertPlugin plugin;
    Bytes raw = Bytes::create(8u);
    Bytes payload;
    CHECK_FALSE(plugin.convert("other.Type", raw, Target::kFoxglove, payload));
  }

  TEST_CASE("get_timestamp default implementation returns -1") {
    FakeConvertPlugin plugin;
    Bytes raw = Bytes::create(8u);
    int64_t ts = plugin.get_timestamp("my_pkg.MyMessage", raw, Target::kFoxglove);
    CHECK_EQ(ts, -1);
  }

  TEST_CASE("can_publish default implementation returns false") {
    FakeConvertPlugin plugin;
    FrontendChannel ch;
    ch.topic = "any";
    CHECK_FALSE(plugin.can_publish(ch, Target::kFoxglove));
  }

  TEST_CASE("get_publish default implementation returns false and leaves output unchanged") {
    FakeConvertPlugin plugin;
    FrontendChannel ch;
    PublishInfo pub;
    pub.url = "preset";
    CHECK_FALSE(plugin.get_publish(ch, Target::kFoxglove, pub));
    CHECK_EQ(pub.url, "preset");
  }

  TEST_CASE("convert_publish default implementation returns false") {
    FakeConvertPlugin plugin;
    FrontendChannel ch;
    Bytes raw = Bytes::create(4u);
    Bytes payload;
    CHECK_FALSE(plugin.convert_publish(ch, raw, Target::kFoxglove, payload));
  }
}

// NOLINTEND
