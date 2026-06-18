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

#include <filesystem>
#include <fstream>
#include <string>
#include <type_traits>

#include "./extension/schema_plugin_base.h"
#include "./extension/schema_plugin_manager.h"

#if __has_include(<google/protobuf/any.pb.h>)
#include <google/protobuf/any.pb.h>
#endif

#include "../common_test.h"

namespace {

struct InvalidBinarySchema final {
  [[nodiscard]] static const uint8_t* data() {
    static const uint8_t kData[]{0};
    return kData;
  }

  [[nodiscard]] static size_t size() { return 1U; }
};

class TestSchemaPlugin final : public SchemaPluginBase {
 public:
  TestSchemaPlugin() = default;

  using SchemaPluginBase::create_flatbuffers_parser;
  using SchemaPluginBase::create_protobuf_message;
  using SchemaPluginBase::get_all_schemas;
  using SchemaPluginBase::search_flatbuffers_schema;
  using SchemaPluginBase::search_protobuf_descriptor;
  using SchemaPluginBase::search_schema;

  [[nodiscard]] VersionInfo get_version_info() const override {
    return {"TestSchemaPlugin", "1.0.0", "2026-01-01", "", ""};
  }
};

#ifdef VLINK_HAS_SCHEMA_PLUGIN_FLATBUFFERS
VLINK_REGISTER_FLATBUFFERS("invalid.Schema", InvalidBinarySchema)
VLINK_REGISTER_FLATBUFFERS("invalid.Schema.second", InvalidBinarySchema)
#endif

[[maybe_unused]] static std::filesystem::path test_idl_dir() {
  return std::filesystem::path(__FILE__).parent_path().parent_path() / "idl";
}

}  // namespace

TEST_SUITE("extension-SchemaPluginInterface") {
  TEST_CASE("VersionInfo default construction has all empty fields") {
    SchemaPluginInterface::VersionInfo vi;
    CHECK(vi.name.empty());
    CHECK(vi.version.empty());
    CHECK(vi.timestamp.empty());
    CHECK(vi.tag.empty());
    CHECK(vi.commit_id.empty());
  }

  TEST_CASE("VersionInfo fields are independently assignable") {
    SchemaPluginInterface::VersionInfo vi;
    vi.name = "TestPlugin";
    vi.version = "1.0.0";
    vi.timestamp = "2026-01-01";
    vi.tag = "v1.0.0";
    vi.commit_id = "abc123";
    CHECK_EQ(vi.name, "TestPlugin");
    CHECK_EQ(vi.version, "1.0.0");
    CHECK_EQ(vi.timestamp, "2026-01-01");
    CHECK_EQ(vi.tag, "v1.0.0");
    CHECK_EQ(vi.commit_id, "abc123");
  }

  TEST_CASE("concrete plugin returns version info from get_version_info") {
    TestSchemaPlugin plugin;
    auto vi = plugin.get_version_info();
    CHECK_EQ(vi.name, "TestSchemaPlugin");
    CHECK_EQ(vi.version, "1.0.0");
  }
}

TEST_SUITE("extension-SchemaPluginManager") {
  TEST_CASE("get returns a valid singleton reference") {
    SchemaPluginManager& mgr = SchemaPluginManager::get("");
    (void)mgr.is_valid();
  }

  TEST_CASE("repeated get calls return the same singleton") {
    SchemaPluginManager& a = SchemaPluginManager::get();
    SchemaPluginManager& b = SchemaPluginManager::get();
    CHECK_EQ(&a, &b);
  }

  TEST_CASE("get_interface returns nullptr when no plugin is loaded") {
    SchemaPluginManager& mgr = SchemaPluginManager::get();
    if (!mgr.is_valid()) {
      CHECK_EQ(mgr.get_interface(), nullptr);
    } else {
      CHECK_NE(mgr.get_interface(), nullptr);
    }
  }

#if defined(VLINK_TEST_SUPPORT_PROTOBUF)
  TEST_CASE("manager is accessible when protobuf support is enabled") {
    SchemaPluginManager& mgr = SchemaPluginManager::get();
    (void)mgr.is_valid();
  }
#endif  // VLINK_TEST_SUPPORT_PROTOBUF
}

#if !defined(_WIN32) && !defined(__CYGWIN__) && defined(VLINK_HAS_SCHEMA_PLUGIN_PROTOBUF) && \
    defined(VLINK_HAS_SCHEMA_PLUGIN_FLATBUFFERS)

TEST_SUITE("extension-SchemaPluginBase") {
  TEST_CASE("protobuf schema lookup finds linked generated descriptor") {
    TestSchemaPlugin plugin;
    const auto* descriptor =
        google::protobuf::DescriptorPool::generated_pool()->FindMessageTypeByName("google.protobuf.Any");
    REQUIRE(descriptor != nullptr);

    const auto schema = plugin.search_schema("google.protobuf.Any", SchemaType::kProtobuf);
    CHECK_EQ(schema.name, "google.protobuf.Any");
    CHECK_EQ(schema.encoding, "protobuf");
    CHECK_EQ(schema.schema_type, SchemaType::kProtobuf);
    CHECK_FALSE(schema.data.empty());
  }

  TEST_CASE("protobuf schema lookup with wrong family returns empty schema") {
    TestSchemaPlugin plugin;
    const auto schema = plugin.search_schema("google.protobuf.Any", SchemaType::kFlatbuffers);
    CHECK(schema.encoding.empty());
    CHECK(schema.data.empty());
  }

  TEST_CASE("search_protobuf_descriptor returns non-null for linked type") {
    TestSchemaPlugin plugin;
    CHECK_NE(plugin.search_protobuf_descriptor("google.protobuf.Any"), nullptr);
  }

  TEST_CASE("create_protobuf_message returns non-null for linked type") {
    TestSchemaPlugin plugin;
    CHECK_NE(plugin.create_protobuf_message("google.protobuf.Any"), nullptr);
  }

  TEST_CASE("get_all_schemas returns non-empty list for protobuf family") {
    TestSchemaPlugin plugin;
    (void)plugin.search_schema("google.protobuf.Any", SchemaType::kProtobuf);
    const auto all = plugin.get_all_schemas(SchemaType::kProtobuf);
    CHECK_FALSE(all.empty());
  }

  TEST_CASE("flatbuffers bfbs blob enables runtime schema and parser access") {
    auto fbs_path = test_idl_dir() / "test.fbs";
    REQUIRE(std::filesystem::exists(fbs_path));

    std::ifstream input(fbs_path, std::ios::binary);
    REQUIRE(input.is_open());

    std::string schema_text((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    REQUIRE_FALSE(schema_text.empty());

    flatbuffers::Parser bfbs_builder;
    const auto include_dir = fbs_path.parent_path().string();
    const char* include_dirs[] = {include_dir.c_str(), nullptr};
    REQUIRE(bfbs_builder.Parse(schema_text.c_str(), include_dirs));
    REQUIRE(bfbs_builder.SetRootType("fbs.Message"));
    bfbs_builder.Serialize();

    const auto* bfbs_data = bfbs_builder.builder_.GetBufferPointer();
    const auto bfbs_size = bfbs_builder.builder_.GetSize();
    REQUIRE(bfbs_data != nullptr);
    REQUIRE(bfbs_size > 0);

    CHECK(FlatbuffersRegistry::register_schema("fbs.Message", bfbs_data, bfbs_size));

    TestSchemaPlugin plugin;

    const auto schema = plugin.search_schema("fbs.Message", SchemaType::kFlatbuffers);
    CHECK_EQ(schema.name, "fbs.Message");
    CHECK_EQ(schema.encoding, "flatbuffers");
    CHECK_EQ(schema.schema_type, SchemaType::kFlatbuffers);
    CHECK_FALSE(schema.data.empty());

    auto* reflection_schema = static_cast<const reflection::Schema*>(plugin.search_flatbuffers_schema("fbs.Message"));
    REQUIRE(reflection_schema != nullptr);
    CHECK_NE(reflection_schema->root_table(), nullptr);

    auto* p1 = static_cast<flatbuffers::Parser*>(plugin.create_flatbuffers_parser("fbs.Message"));
    auto* p2 = static_cast<flatbuffers::Parser*>(plugin.create_flatbuffers_parser("fbs.Message"));
    REQUIRE(p1 != nullptr);
    REQUIRE(p2 != nullptr);
    CHECK_NE(p1, p2);

    REQUIRE(p1->root_struct_def_ != nullptr);
    const auto fqn = p1->root_struct_def_->defined_namespace->GetFullyQualifiedName(p1->root_struct_def_->name);
    CHECK_EQ(fqn, "fbs.Message");

    const auto all = plugin.get_all_schemas(SchemaType::kFlatbuffers);
    CHECK_FALSE(all.empty());
  }

  TEST_CASE("family-agnostic lookup refuses ambiguous same-name schemas") {
    auto fbs_path = test_idl_dir() / "test.fbs";
    REQUIRE(std::filesystem::exists(fbs_path));

    std::ifstream input(fbs_path, std::ios::binary);
    REQUIRE(input.is_open());

    std::string schema_text((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    REQUIRE_FALSE(schema_text.empty());

    flatbuffers::Parser bfbs_builder;
    const auto include_dir = fbs_path.parent_path().string();
    const char* include_dirs[] = {include_dir.c_str(), nullptr};
    REQUIRE(bfbs_builder.Parse(schema_text.c_str(), include_dirs));
    REQUIRE(bfbs_builder.SetRootType("fbs.Message"));
    bfbs_builder.Serialize();

    const auto* bfbs_data = bfbs_builder.builder_.GetBufferPointer();
    const auto bfbs_size = bfbs_builder.builder_.GetSize();
    REQUIRE(bfbs_data != nullptr);
    REQUIRE(bfbs_size > 0);

    REQUIRE(FlatbuffersRegistry::register_schema("vlink::zerocopy::Collision", bfbs_data, bfbs_size));

    TestSchemaPlugin plugin;

    const auto ambiguous = plugin.search_schema("vlink::zerocopy::Collision");
    CHECK(ambiguous.encoding.empty());
    CHECK(ambiguous.data.empty());

    const auto resolved_fbs = plugin.search_schema("vlink::zerocopy::Collision", SchemaType::kFlatbuffers);
    CHECK_EQ(resolved_fbs.schema_type, SchemaType::kFlatbuffers);
    CHECK_EQ(resolved_fbs.encoding, "flatbuffers");

    const auto resolved_zc = plugin.search_schema("vlink::zerocopy::Collision", SchemaType::kZeroCopy);
    CHECK_EQ(resolved_zc.schema_type, SchemaType::kZeroCopy);
    CHECK_EQ(resolved_zc.encoding, "vlink_msg");
  }
}

#endif

// NOLINTEND
