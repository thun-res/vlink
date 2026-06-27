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

#include "./extension/flatbuffers_registry.h"

#include <doctest/doctest.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "../common_test.h"

#ifdef VLINK_HAS_SCHEMA_PLUGIN_FLATBUFFERS

TEST_SUITE("extension-FlatbuffersRegistry") {
  TEST_CASE("get returns the same singleton instance on repeated calls") {
    FlatbuffersRegistry& a = FlatbuffersRegistry::get();
    FlatbuffersRegistry& b = FlatbuffersRegistry::get();
    CHECK_EQ(&a, &b);
  }

  TEST_CASE("get_all_schemas returns a vector without throwing") {
    std::vector<SchemaData> schemas = FlatbuffersRegistry::get().get_all_schemas();
    (void)schemas;
  }

  TEST_CASE("register_schema rejects empty name") {
    static const uint8_t kData[]{'d', 'a', 't', 'a'};
    CHECK_FALSE(FlatbuffersRegistry::register_schema("", kData, sizeof(kData)));
  }

  TEST_CASE("register_schema rejects null pointer") {
    CHECK_FALSE(FlatbuffersRegistry::register_schema("schema.Test", nullptr, 0u));
  }

  TEST_CASE("register_schema rejects zero-size buffer") {
    static const uint8_t kBuf[]{0};
    CHECK_FALSE(FlatbuffersRegistry::register_schema("schema.Test", kBuf, 0u));
  }

  TEST_CASE("register_schema rejects bytes that fail bfbs verification") {
    static const uint8_t kGarbage[]{0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07};
    CHECK_FALSE(FlatbuffersRegistry::register_schema("schema.Garbage", kGarbage, sizeof(kGarbage)));
  }

  TEST_CASE("register_schema template overload forwards to validation path") {
    struct TinySchema {
      static const uint8_t* data() {
        static const uint8_t kBuf[]{0x00};
        return kBuf;
      }

      static size_t size() { return 1u; }
    };

    CHECK_FALSE(FlatbuffersRegistry::register_schema<TinySchema>("schema.TinyTemplate"));
  }

  TEST_CASE("search_schema returns empty SchemaData for unregistered name") {
    SchemaData result = FlatbuffersRegistry::get().search_schema("definitely_not_registered.Type_xyz");
    CHECK(result.name.empty());
    CHECK(result.encoding.empty());
    CHECK_EQ(result.schema_type, SchemaType::kUnknown);
    CHECK(result.data.empty());
  }
}

#endif  // VLINK_HAS_SCHEMA_PLUGIN_FLATBUFFERS

// NOLINTEND
