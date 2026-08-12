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

#include <cstdint>
#include <cstring>

#include "../tools/autosar/test/generated_someip_types.h"

static_assert(vlink::Serializer::get_type_of<vlink::autosar::VehicleState>() == vlink::Serializer::kSomeipType);
static_assert(vlink::Serializer::get_type_of<vlink::autosar::VehicleStateApplication>() ==
              vlink::Serializer::kSomeipType);

TEST_SUITE("ser-arxml-generator") {
  TEST_CASE("generated AUTOSAR prototype factory preserves its initial value") {
    auto state = vlink::autosar::make_vehicle_state_event_initial_value();

    CHECK(state.sequence == 7U);
    CHECK(state.valid);
    CHECK(state.mode == vlink::autosar::GearMode::kDrive);
    CHECK(state.temperature == doctest::Approx(20.5F));
    CHECK(state.name == "parked");
    CHECK(state.position.x == -5);
    CHECK(state.position.y == 8);
    CHECK(state.samples == vlink::autosar::SampleWindow{10U, 20U, 30U, 40U});
    REQUIRE(state.objects.size() == 2U);
    CHECK(state.objects[0].x == 1);
    CHECK(state.objects[0].y == 2);
    CHECK(state.objects[1].x == -3);
    CHECK(state.objects[1].y == 4);
    REQUIRE(state.payload.size() == 4U);
    CHECK(state.payload.data()[0] == 0xDEU);
    CHECK(state.payload.data()[1] == 0xADU);
    CHECK(state.payload.data()[2] == 0xBEU);
    CHECK(state.payload.data()[3] == 0xEFU);
    CHECK(state.matrix == std::array<std::array<uint16_t, 3>, 2>{{{1U, 2U, 3U}, {4U, 5U, 6U}}});
  }

  TEST_CASE("generated AUTOSAR type matches its complete SOME/IP wire format") {
    vlink::autosar::VehicleState source;
    source.sequence = 0x01020304U;
    source.valid = true;
    source.mode = vlink::autosar::GearMode::kDrive;
    source.temperature = 36.5F;
    source.name = "vehicle";
    source.position.x = -120;
    source.position.y = 450;
    source.samples = {10U, 20U, 30U, 40U};

    vlink::autosar::Position first;
    first.x = 1;
    first.y = 2;
    source.objects.push_back(first);

    vlink::autosar::Position second;
    second.x = -3;
    second.y = 4;
    source.objects.push_back(second);

    source.payload = vlink::Bytes::create(4U);
    REQUIRE(source.payload.data() != nullptr);
    const uint8_t raw_payload[] = {0xDEU, 0xADU, 0xBEU, 0xEFU};
    std::memcpy(source.payload.data(), raw_payload, sizeof(raw_payload));
    source.matrix = {{{1U, 2U, 3U}, {4U, 5U, 6U}}};

    vlink::Bytes encoded;
    REQUIRE(vlink::Serializer::serialize(source, encoded));
    const std::array<uint8_t, 94> expected = {
        0x00U, 0x5CU, 0x04U, 0x03U, 0x02U, 0x01U, 0x01U, 0x01U, 0x00U, 0x00U, 0x12U, 0x42U, 0x00U, 0x0BU, 0xEFU, 0xBBU,
        0xBFU, 0x76U, 0x65U, 0x68U, 0x69U, 0x63U, 0x6CU, 0x65U, 0x00U, 0x00U, 0x08U, 0x88U, 0xFFU, 0xFFU, 0xFFU, 0xC2U,
        0x01U, 0x00U, 0x00U, 0x0AU, 0x00U, 0x14U, 0x00U, 0x1EU, 0x00U, 0x28U, 0x00U, 0x00U, 0x14U, 0x00U, 0x08U, 0x01U,
        0x00U, 0x00U, 0x00U, 0x02U, 0x00U, 0x00U, 0x00U, 0x00U, 0x08U, 0xFDU, 0xFFU, 0xFFU, 0xFFU, 0x04U, 0x00U, 0x00U,
        0x00U, 0x04U, 0xDEU, 0xADU, 0xBEU, 0xEFU, 0x00U, 0x00U, 0x00U, 0x14U, 0x00U, 0x00U, 0x00U, 0x06U, 0x01U, 0x00U,
        0x02U, 0x00U, 0x03U, 0x00U, 0x00U, 0x00U, 0x00U, 0x06U, 0x04U, 0x00U, 0x05U, 0x00U, 0x06U, 0x00U,
    };
    REQUIRE(encoded.size() == expected.size());
    for (size_t index = 0; index < expected.size(); ++index) {
      CHECK(encoded.data()[index] == expected[index]);
    }

    auto golden = vlink::Bytes::create(expected.size());
    REQUIRE(golden.data() != nullptr);
    std::memcpy(golden.data(), expected.data(), expected.size());

    vlink::autosar::VehicleState target;
    REQUIRE(vlink::Serializer::deserialize(golden, target));
    CHECK(target.sequence == source.sequence);
    CHECK(target.valid == source.valid);
    CHECK(target.mode == source.mode);
    CHECK(target.temperature == doctest::Approx(source.temperature));
    CHECK(target.name == source.name);
    CHECK(target.position.x == source.position.x);
    CHECK(target.position.y == source.position.y);
    CHECK(target.samples == source.samples);
    REQUIRE(target.objects.size() == source.objects.size());
    CHECK(target.objects[0].x == source.objects[0].x);
    CHECK(target.objects[0].y == source.objects[0].y);
    CHECK(target.objects[1].x == source.objects[1].x);
    CHECK(target.objects[1].y == source.objects[1].y);
    CHECK(target.payload == source.payload);
    CHECK(target.matrix == source.matrix);
  }
}

// NOLINTEND
