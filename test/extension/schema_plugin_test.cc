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

#include <cstring>
#include <filesystem>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

#include "./base/process.h"
#include "./base/utils.h"
#include "./extension/schema_plugin_base.h"
#include "./extension/schema_plugin_manager.h"

#if __has_include(<google/protobuf/descriptor.pb.h>)
#include <google/protobuf/descriptor.pb.h>
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

constexpr const char* kLinkedProtobufSchemaName = "google.protobuf.FileDescriptorSet";

#ifdef VLINK_HAS_SCHEMA_PLUGIN_FLATBUFFERS
VLINK_REGISTER_FLATBUFFERS("invalid.Schema", InvalidBinarySchema)
VLINK_REGISTER_FLATBUFFERS("invalid.Schema.second", InvalidBinarySchema)
#endif

#ifdef VLINK_HAS_SCHEMA_PLUGIN_FLATBUFFERS
std::vector<uint8_t> build_test_bfbs(const char* root_type = "fbs.Message") {
  static constexpr char kSchemaText[] = R"(
namespace fbs;

table Request {
  type:uint32;
}

table Response {
  value:string;
}

table Message {
  type:uint32;
  value:string;
}
)";

  flatbuffers::Parser bfbs_builder;
  REQUIRE(bfbs_builder.Parse(kSchemaText));
  REQUIRE(bfbs_builder.SetRootType(root_type));
  bfbs_builder.Serialize();

  const auto* bfbs_data = bfbs_builder.builder_.GetBufferPointer();
  const auto bfbs_size = bfbs_builder.builder_.GetSize();
  REQUIRE(bfbs_data != nullptr);
  REQUIRE(bfbs_size > 0);

  return std::vector<uint8_t>(bfbs_data, bfbs_data + bfbs_size);
}

const std::vector<uint8_t>& stable_test_bfbs(std::string_view root_type = "fbs.Message") {
  static const std::vector<uint8_t> message_bfbs = build_test_bfbs("fbs.Message");
  static const std::vector<uint8_t> response_bfbs = build_test_bfbs("fbs.Response");

  return root_type == "fbs.Response" ? response_bfbs : message_bfbs;
}
#endif

std::filesystem::path missing_schema_plugin_path(const std::string& stem) {
  return std::filesystem::path(Utils::get_tmp_dir()) / ("vlink-" + stem + "-missing-schema-plugin");
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
  TEST_CASE("first get with missing explicit path returns invalid singleton") {
    const auto missing_path = missing_schema_plugin_path("explicit");
    SchemaPluginManager& mgr = SchemaPluginManager::get(missing_path.string());
    CHECK_FALSE(mgr.is_valid());
    CHECK_EQ(mgr.get_interface(), nullptr);
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

  TEST_CASE("child process covers environment based manager construction") {
    const auto child_case = Utils::get_env("VLINK_SCHEMA_MANAGER_CHILD_CASE");

    if (!child_case.empty()) {
      SchemaPluginManager& mgr = SchemaPluginManager::get();
      CHECK_FALSE(mgr.is_valid());
      CHECK_EQ(mgr.get_interface(), nullptr);
      return;
    }

    const std::vector<Process::EnvironmentMap> child_environments{
        {{"VLINK_SCHEMA_MANAGER_CHILD_CASE", "empty-env"}, {"VLINK_SCHEMA_PLUGIN", ""}},
        {{"VLINK_SCHEMA_MANAGER_CHILD_CASE", "missing-env"},
         {"VLINK_SCHEMA_PLUGIN", missing_schema_plugin_path("env").string()}}};

    for (const auto& environment : child_environments) {
      Process child;
      child.set_process_mode(Process::kForwardedMode);
      child.set_inherit_environment(true);
      child.set_environment(environment);

      child.start(Utils::get_app_path(),
                  {"--test-suite=extension-SchemaPluginManager",
                   "--test-case=child process covers environment based manager construction", "--no-version"});
      REQUIRE(child.wait_for_finished(Process::kDefaultExecuteTimeoutMs));
      CHECK_EQ(child.get_exit_code(), 0);
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
        google::protobuf::DescriptorPool::generated_pool()->FindMessageTypeByName(kLinkedProtobufSchemaName);
    REQUIRE(descriptor != nullptr);

    const auto schema = plugin.search_schema(kLinkedProtobufSchemaName, SchemaType::kProtobuf);
    CHECK_EQ(schema.name, kLinkedProtobufSchemaName);
    CHECK_EQ(schema.encoding, "protobuf");
    CHECK_EQ(schema.schema_type, SchemaType::kProtobuf);
    CHECK_FALSE(schema.data.empty());
  }

  TEST_CASE("protobuf schema lookup with wrong family returns empty schema") {
    TestSchemaPlugin plugin;
    const auto schema = plugin.search_schema(kLinkedProtobufSchemaName, SchemaType::kFlatbuffers);
    CHECK(schema.encoding.empty());
    CHECK(schema.data.empty());
  }

  TEST_CASE("explicit zerocopy schema lookup is cached and can be found family-agnostically") {
    TestSchemaPlugin plugin;

    const auto first = plugin.search_schema("vlink::zerocopy::UnitTest", SchemaType::kZeroCopy);
    CHECK_EQ(first.name, "vlink::zerocopy::UnitTest");
    CHECK_EQ(first.encoding, "vlink_msg");
    CHECK_EQ(first.schema_type, SchemaType::kZeroCopy);

    const auto cached = plugin.search_schema("vlink::zerocopy::UnitTest");
    CHECK_EQ(cached.name, first.name);
    CHECK_EQ(cached.encoding, first.encoding);
    CHECK_EQ(cached.schema_type, first.schema_type);
  }

  TEST_CASE("search_protobuf_descriptor returns non-null for linked type") {
    TestSchemaPlugin plugin;
    auto* first = plugin.search_protobuf_descriptor(kLinkedProtobufSchemaName);
    auto* second = plugin.search_protobuf_descriptor(kLinkedProtobufSchemaName);
    CHECK_NE(first, nullptr);
    CHECK_EQ(first, second);
  }

  TEST_CASE("create_protobuf_message returns non-null for linked type") {
    TestSchemaPlugin plugin;
    auto* first = plugin.create_protobuf_message(kLinkedProtobufSchemaName);
    auto* second = plugin.create_protobuf_message(kLinkedProtobufSchemaName);
    CHECK_NE(first, nullptr);
    CHECK_EQ(first, second);
  }

  TEST_CASE("protobuf lookup helpers return null for missing linked type") {
    TestSchemaPlugin plugin;
    CHECK_EQ(plugin.search_protobuf_descriptor("missing.SchemaForCoverage"), nullptr);
    CHECK_EQ(plugin.create_protobuf_message("missing.SchemaForCoverage"), nullptr);
  }

  TEST_CASE("get_all_schemas returns non-empty list for protobuf family") {
    TestSchemaPlugin plugin;
    (void)plugin.search_schema(kLinkedProtobufSchemaName, SchemaType::kProtobuf);
    const auto all = plugin.get_all_schemas(SchemaType::kProtobuf);
    CHECK_FALSE(all.empty());
  }

  TEST_CASE("flatbuffers bfbs blob enables runtime schema and parser access") {
    const auto& bfbs = stable_test_bfbs();
    CHECK(FlatbuffersRegistry::register_schema("fbs.Message", bfbs.data(), bfbs.size()));

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

  TEST_CASE("invalid flatbuffers registry entries are rejected by schema and parser lookup") {
    TestSchemaPlugin plugin;
    CHECK_EQ(plugin.search_flatbuffers_schema("invalid.Schema"), nullptr);
    CHECK_EQ(plugin.create_flatbuffers_parser("invalid.Schema"), nullptr);
    CHECK(plugin.search_schema("invalid.Schema", SchemaType::kFlatbuffers).data.empty());
  }

  TEST_CASE("unknown schema lookups stay empty across explicit and family-agnostic searches") {
    TestSchemaPlugin plugin;

    CHECK(plugin.search_schema("missing.SchemaForCoverage", SchemaType::kUnknown).encoding.empty());
    CHECK(plugin.search_schema("missing.SchemaForCoverage", SchemaType::kProtobuf).encoding.empty());
    CHECK(plugin.search_schema("missing.SchemaForCoverage", SchemaType::kFlatbuffers).encoding.empty());
    CHECK(plugin.search_schema("vlink::zerocopy::WrongFamily", SchemaType::kProtobuf).encoding.empty());
    CHECK(plugin.search_schema("plain.Name", SchemaType::kZeroCopy).encoding.empty());
    CHECK(plugin.get_all_schemas(SchemaType::kRaw).empty());
  }

  TEST_CASE("flatbuffers registry stores manually registered BFBS data") {
    const auto& bfbs = stable_test_bfbs();
    REQUIRE(FlatbuffersRegistry::register_schema("fbs.RegistryOwnsCopy", bfbs.data(), bfbs.size()));

    const auto stored = FlatbuffersRegistry::get().search_schema("fbs.RegistryOwnsCopy");
    CHECK_EQ(stored.schema_type, SchemaType::kFlatbuffers);
    CHECK_FALSE(stored.data.empty());
    REQUIRE_EQ(stored.data.size(), bfbs.size());
    CHECK_EQ(std::memcmp(stored.data.data(), bfbs.data(), bfbs.size()), 0);
  }

  TEST_CASE("flatbuffers schema cache handles snapshot reuse root mismatch and registry replacement") {
    const auto& message_bfbs = stable_test_bfbs("fbs.Message");
    const auto& response_bfbs = stable_test_bfbs("fbs.Response");
    const std::string schema_name = "fbs.Message.CacheBranch";

    REQUIRE(FlatbuffersRegistry::register_schema(schema_name, message_bfbs.data(), message_bfbs.size()));

    TestSchemaPlugin plugin;
    const auto schema = plugin.search_schema(schema_name, SchemaType::kFlatbuffers);
    CHECK_EQ(schema.schema_type, SchemaType::kFlatbuffers);

    auto* first = static_cast<const reflection::Schema*>(plugin.search_flatbuffers_schema(schema_name));
    auto* second = static_cast<const reflection::Schema*>(plugin.search_flatbuffers_schema(schema_name));
    REQUIRE(first != nullptr);
    CHECK_EQ(first, second);

    CHECK_EQ(plugin.create_flatbuffers_parser(schema_name), nullptr);

    REQUIRE(FlatbuffersRegistry::register_schema(schema_name, response_bfbs.data(), response_bfbs.size()));
    const auto all = plugin.get_all_schemas(SchemaType::kFlatbuffers);
    CHECK_FALSE(all.empty());

    auto* replaced = static_cast<const reflection::Schema*>(plugin.search_flatbuffers_schema(schema_name));
    REQUIRE(replaced != nullptr);
    CHECK_NE(replaced, first);
  }

  TEST_CASE("family-agnostic lookup refuses ambiguous same-name schemas") {
    const auto& bfbs = stable_test_bfbs();
    REQUIRE(FlatbuffersRegistry::register_schema("vlink::zerocopy::Collision", bfbs.data(), bfbs.size()));

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

  TEST_CASE("family-agnostic lookup reuses cached protobuf flatbuffers and zerocopy schemas") {
    const auto& bfbs = stable_test_bfbs();
    REQUIRE(FlatbuffersRegistry::register_schema("fbs.Message", bfbs.data(), bfbs.size()));

    TestSchemaPlugin plugin;

    const auto proto = plugin.search_schema(kLinkedProtobufSchemaName);
    CHECK_EQ(proto.name, kLinkedProtobufSchemaName);
    CHECK_EQ(proto.schema_type, SchemaType::kProtobuf);

    const auto fbs = plugin.search_schema("fbs.Message", SchemaType::kFlatbuffers);
    CHECK_EQ(fbs.name, "fbs.Message");
    CHECK_EQ(fbs.schema_type, SchemaType::kFlatbuffers);

    const auto zc = plugin.search_schema("vlink::zerocopy::CacheBranch", SchemaType::kZeroCopy);
    CHECK_EQ(zc.schema_type, SchemaType::kZeroCopy);

    CHECK_EQ(plugin.search_schema(kLinkedProtobufSchemaName).schema_type, SchemaType::kProtobuf);
    CHECK_EQ(plugin.search_schema("fbs.Message").schema_type, SchemaType::kFlatbuffers);
    CHECK_EQ(plugin.search_schema("vlink::zerocopy::CacheBranch").schema_type, SchemaType::kZeroCopy);

    CHECK_FALSE(plugin.get_all_schemas().empty());
    CHECK_FALSE(plugin.get_all_schemas(SchemaType::kZeroCopy).empty());
  }
}

#endif

// NOLINTEND
