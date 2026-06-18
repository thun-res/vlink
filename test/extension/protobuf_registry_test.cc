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

#include "./extension/protobuf_registry.h"

#include <doctest/doctest.h>

#include "../common_test.h"

#ifdef VLINK_HAS_SCHEMA_PLUGIN_PROTOBUF

TEST_SUITE("extension-ProtobufRegistry") {
  TEST_CASE("generated descriptor pool is accessible") {
    const auto* pool = google::protobuf::DescriptorPool::generated_pool();
    CHECK(pool != nullptr);
  }

  TEST_CASE("generated message factory is accessible") {
    auto* factory = google::protobuf::MessageFactory::generated_factory();
    CHECK(factory != nullptr);
  }
}

#endif  // VLINK_HAS_SCHEMA_PLUGIN_PROTOBUF

// NOLINTEND
