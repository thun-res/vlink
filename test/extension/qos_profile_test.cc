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

#include "./extension/qos_profile.h"

#include <doctest/doctest.h>

#include <string>

TEST_SUITE("extension-QosProfile") {
  TEST_CASE("every named profile has valid=true") {
    SUBCASE("kEvent") { CHECK(vlink::QosProfile::kEvent.valid); }
    SUBCASE("kMethod") { CHECK(vlink::QosProfile::kMethod.valid); }
    SUBCASE("kField") { CHECK(vlink::QosProfile::kField.valid); }
    SUBCASE("kSensor") { CHECK(vlink::QosProfile::kSensor.valid); }
    SUBCASE("kParameter") { CHECK(vlink::QosProfile::kParameter.valid); }
    SUBCASE("kService") { CHECK(vlink::QosProfile::kService.valid); }
    SUBCASE("kClock") { CHECK(vlink::QosProfile::kClock.valid); }
    SUBCASE("kStatic") { CHECK(vlink::QosProfile::kStatic.valid); }
    SUBCASE("kLight") { CHECK(vlink::QosProfile::kLight.valid); }
    SUBCASE("kPoor") { CHECK(vlink::QosProfile::kPoor.valid); }
    SUBCASE("kBetter") { CHECK(vlink::QosProfile::kBetter.valid); }
    SUBCASE("kBest") { CHECK(vlink::QosProfile::kBest.valid); }
    SUBCASE("kLarge") { CHECK(vlink::QosProfile::kLarge.valid); }
    SUBCASE("kAlarm") { CHECK(vlink::QosProfile::kAlarm.valid); }
    SUBCASE("kCommand") { CHECK(vlink::QosProfile::kCommand.valid); }
    SUBCASE("kLog") { CHECK(vlink::QosProfile::kLog.valid); }
  }

  TEST_CASE("kEvent is reliable keep-last-5 volatile sync realtime") {
    const vlink::Qos& q = vlink::QosProfile::kEvent;
    CHECK_EQ(q.reliability.kind, vlink::Qos::Reliability::kReliable);
    CHECK_EQ(q.history.kind, vlink::Qos::History::kKeepLast);
    CHECK_EQ(q.history.depth, 5);
    CHECK_EQ(q.durability.kind, vlink::Qos::Durability::kVolatile);
    CHECK_EQ(q.publish_mode.kind, vlink::Qos::PublishMode::kSync);
    CHECK_EQ(q.additions.priority, vlink::Qos::Additions::kPriorityRealTime);
    CHECK_FALSE(q.additions.is_express);
    CHECK_EQ(q.liveliness.kind, vlink::Qos::Liveliness::kAutomatic);
    CHECK_EQ(q.liveliness.duration, 1000);
    CHECK_EQ(q.deadline.period, -1);
    CHECK_EQ(q.lifespan.duration, -1);
    CHECK_EQ(std::string(q.name), "event");
  }

  TEST_CASE("kMethod is reliable keep-all volatile sync high priority") {
    const vlink::Qos& q = vlink::QosProfile::kMethod;
    CHECK_EQ(q.reliability.kind, vlink::Qos::Reliability::kReliable);
    CHECK_EQ(q.history.kind, vlink::Qos::History::kKeepAll);
    CHECK_EQ(q.durability.kind, vlink::Qos::Durability::kVolatile);
    CHECK_EQ(q.publish_mode.kind, vlink::Qos::PublishMode::kSync);
    CHECK_EQ(q.additions.priority, vlink::Qos::Additions::kPriorityHigh);
    CHECK_EQ(q.liveliness.duration, 2000);
    CHECK_EQ(q.deadline.period, -1);
    CHECK_EQ(q.lifespan.duration, -1);
    CHECK_EQ(std::string(q.name), "method");
  }

  TEST_CASE("kField is reliable keep-last-1 transient-local sync high priority") {
    const vlink::Qos& q = vlink::QosProfile::kField;
    CHECK_EQ(q.reliability.kind, vlink::Qos::Reliability::kReliable);
    CHECK_EQ(q.history.kind, vlink::Qos::History::kKeepLast);
    CHECK_EQ(q.history.depth, 1);
    CHECK_EQ(q.durability.kind, vlink::Qos::Durability::kTransientLocal);
    CHECK_EQ(q.publish_mode.kind, vlink::Qos::PublishMode::kSync);
    CHECK_EQ(q.additions.priority, vlink::Qos::Additions::kPriorityHigh);
    CHECK_EQ(q.liveliness.duration, 2000);
    CHECK_EQ(q.deadline.period, -1);
    CHECK_EQ(q.lifespan.duration, -1);
    CHECK_EQ(std::string(q.name), "field");
  }

  TEST_CASE("kSensor is best-effort keep-last-10 volatile async normal express") {
    const vlink::Qos& q = vlink::QosProfile::kSensor;
    CHECK_EQ(q.reliability.kind, vlink::Qos::Reliability::kBestEffort);
    CHECK_EQ(q.history.kind, vlink::Qos::History::kKeepLast);
    CHECK_EQ(q.history.depth, 10);
    CHECK_EQ(q.durability.kind, vlink::Qos::Durability::kVolatile);
    CHECK_EQ(q.publish_mode.kind, vlink::Qos::PublishMode::kASync);
    CHECK_EQ(q.additions.priority, vlink::Qos::Additions::kPriorityNormal);
    CHECK(q.additions.is_express);
    CHECK_EQ(q.liveliness.duration, 500);
    CHECK_EQ(q.deadline.period, -1);
    CHECK_EQ(q.lifespan.duration, -1);
    CHECK_EQ(std::string(q.name), "sensor");
  }

  TEST_CASE("kParameter is reliable keep-last-500 transient-local sync normal") {
    const vlink::Qos& q = vlink::QosProfile::kParameter;
    CHECK_EQ(q.reliability.kind, vlink::Qos::Reliability::kReliable);
    CHECK_EQ(q.history.kind, vlink::Qos::History::kKeepLast);
    CHECK_EQ(q.history.depth, 500);
    CHECK_LE(q.history.depth, q.resource_limits.max_samples_per_instance);
    CHECK_EQ(q.durability.kind, vlink::Qos::Durability::kTransientLocal);
    CHECK_EQ(q.publish_mode.kind, vlink::Qos::PublishMode::kSync);
    CHECK_EQ(q.additions.priority, vlink::Qos::Additions::kPriorityNormal);
    CHECK_EQ(q.liveliness.duration, 5000);
    CHECK_EQ(q.deadline.period, -1);
    CHECK_EQ(q.lifespan.duration, -1);
    CHECK_EQ(std::string(q.name), "parameter");
  }

  TEST_CASE("kService is reliable keep-last-10 transient-local sync normal") {
    const vlink::Qos& q = vlink::QosProfile::kService;
    CHECK_EQ(q.reliability.kind, vlink::Qos::Reliability::kReliable);
    CHECK_EQ(q.history.kind, vlink::Qos::History::kKeepLast);
    CHECK_EQ(q.history.depth, 10);
    CHECK_EQ(q.durability.kind, vlink::Qos::Durability::kTransientLocal);
    CHECK_EQ(q.publish_mode.kind, vlink::Qos::PublishMode::kSync);
    CHECK_EQ(q.liveliness.duration, 3000);
    CHECK_EQ(q.deadline.period, -1);
    CHECK_EQ(q.lifespan.duration, -1);
    CHECK_EQ(std::string(q.name), "service");
  }

  TEST_CASE("kClock is best-effort keep-last-1 volatile sync low priority express") {
    const vlink::Qos& q = vlink::QosProfile::kClock;
    CHECK_EQ(q.reliability.kind, vlink::Qos::Reliability::kBestEffort);
    CHECK_EQ(q.history.kind, vlink::Qos::History::kKeepLast);
    CHECK_EQ(q.history.depth, 1);
    CHECK_EQ(q.durability.kind, vlink::Qos::Durability::kVolatile);
    CHECK_EQ(q.publish_mode.kind, vlink::Qos::PublishMode::kSync);
    CHECK_EQ(q.additions.priority, vlink::Qos::Additions::kPriorityLow);
    CHECK(q.additions.is_express);
    CHECK_EQ(q.liveliness.duration, 1000);
    CHECK_EQ(q.deadline.period, -1);
    CHECK_EQ(q.lifespan.duration, -1);
    CHECK_EQ(std::string(q.name), "clock");
  }

  TEST_CASE("kStatic is reliable keep-all transient-local sync normal") {
    const vlink::Qos& q = vlink::QosProfile::kStatic;
    CHECK_EQ(q.reliability.kind, vlink::Qos::Reliability::kReliable);
    CHECK_EQ(q.history.kind, vlink::Qos::History::kKeepAll);
    CHECK_EQ(q.durability.kind, vlink::Qos::Durability::kTransientLocal);
    CHECK_EQ(q.publish_mode.kind, vlink::Qos::PublishMode::kSync);
    CHECK_EQ(q.liveliness.duration, 10000);
    CHECK_EQ(q.deadline.period, -1);
    CHECK_EQ(q.lifespan.duration, -1);
    CHECK_EQ(std::string(q.name), "static");
  }

  TEST_CASE("kLight is reliable keep-last-1 volatile async high priority") {
    const vlink::Qos& q = vlink::QosProfile::kLight;
    CHECK_EQ(q.reliability.kind, vlink::Qos::Reliability::kReliable);
    CHECK_EQ(q.history.kind, vlink::Qos::History::kKeepLast);
    CHECK_EQ(q.history.depth, 1);
    CHECK_EQ(q.durability.kind, vlink::Qos::Durability::kVolatile);
    CHECK_EQ(q.publish_mode.kind, vlink::Qos::PublishMode::kASync);
    CHECK_EQ(q.additions.priority, vlink::Qos::Additions::kPriorityHigh);
    CHECK_EQ(q.liveliness.duration, 1000);
    CHECK_EQ(q.deadline.period, -1);
    CHECK_EQ(q.lifespan.duration, -1);
    CHECK_EQ(std::string(q.name), "light");
  }

  TEST_CASE("kPoor is best-effort keep-last-5 volatile async background priority") {
    const vlink::Qos& q = vlink::QosProfile::kPoor;
    CHECK_EQ(q.reliability.kind, vlink::Qos::Reliability::kBestEffort);
    CHECK_EQ(q.history.kind, vlink::Qos::History::kKeepLast);
    CHECK_EQ(q.history.depth, 5);
    CHECK_EQ(q.durability.kind, vlink::Qos::Durability::kVolatile);
    CHECK_EQ(q.publish_mode.kind, vlink::Qos::PublishMode::kASync);
    CHECK_EQ(q.additions.priority, vlink::Qos::Additions::kPriorityBackground);
    CHECK_EQ(q.liveliness.duration, 5000);
    CHECK_EQ(q.deadline.period, -1);
    CHECK_EQ(q.lifespan.duration, -1);
    CHECK_EQ(std::string(q.name), "poor");
  }

  TEST_CASE("kBetter is best-effort keep-last-50 volatile sync realtime priority") {
    const vlink::Qos& q = vlink::QosProfile::kBetter;
    CHECK_EQ(q.reliability.kind, vlink::Qos::Reliability::kBestEffort);
    CHECK_EQ(q.history.kind, vlink::Qos::History::kKeepLast);
    CHECK_EQ(q.history.depth, 50);
    CHECK_EQ(q.publish_mode.kind, vlink::Qos::PublishMode::kSync);
    CHECK_EQ(q.additions.priority, vlink::Qos::Additions::kPriorityRealTime);
    CHECK_EQ(q.liveliness.duration, 1000);
    CHECK_EQ(q.deadline.period, -1);
    CHECK_EQ(q.lifespan.duration, -1);
    CHECK_EQ(std::string(q.name), "better");
  }

  TEST_CASE("kBest is reliable keep-last-200 volatile sync realtime priority") {
    const vlink::Qos& q = vlink::QosProfile::kBest;
    CHECK_EQ(q.reliability.kind, vlink::Qos::Reliability::kReliable);
    CHECK_EQ(q.history.kind, vlink::Qos::History::kKeepLast);
    CHECK_EQ(q.history.depth, 200);
    CHECK_EQ(q.publish_mode.kind, vlink::Qos::PublishMode::kSync);
    CHECK_EQ(q.additions.priority, vlink::Qos::Additions::kPriorityRealTime);
    CHECK_EQ(q.liveliness.duration, 1000);
    CHECK_EQ(q.deadline.period, -1);
    CHECK_EQ(q.lifespan.duration, -1);
    CHECK_EQ(std::string(q.name), "best");
  }

  TEST_CASE("kLarge is reliable keep-last-500 shorter-heartbeat volatile sync low priority") {
    const vlink::Qos& q = vlink::QosProfile::kLarge;
    CHECK_EQ(q.reliability.kind, vlink::Qos::Reliability::kReliable);
    CHECK_EQ(q.history.kind, vlink::Qos::History::kKeepLast);
    CHECK_EQ(q.history.depth, 500);
    CHECK_EQ(q.reliability.heartbeat_time, 500);
    CHECK_EQ(q.durability.kind, vlink::Qos::Durability::kVolatile);
    CHECK_EQ(q.publish_mode.kind, vlink::Qos::PublishMode::kSync);
    CHECK_EQ(q.additions.priority, vlink::Qos::Additions::kPriorityLow);
    CHECK_EQ(q.liveliness.duration, 3000);
    CHECK_EQ(q.deadline.period, -1);
    CHECK_EQ(q.lifespan.duration, -1);
    CHECK_EQ(std::string(q.name), "large");
  }

  TEST_CASE("kAlarm is reliable keep-all transient-local sync realtime express") {
    const vlink::Qos& q = vlink::QosProfile::kAlarm;
    CHECK_EQ(q.reliability.kind, vlink::Qos::Reliability::kReliable);
    CHECK_EQ(q.history.kind, vlink::Qos::History::kKeepAll);
    CHECK_EQ(q.durability.kind, vlink::Qos::Durability::kTransientLocal);
    CHECK_EQ(q.publish_mode.kind, vlink::Qos::PublishMode::kSync);
    CHECK_EQ(q.additions.priority, vlink::Qos::Additions::kPriorityRealTime);
    CHECK(q.additions.is_express);
    CHECK_EQ(q.liveliness.duration, 500);
    CHECK_EQ(q.deadline.period, -1);
    CHECK_EQ(q.lifespan.duration, -1);
    CHECK_EQ(std::string(q.name), "alarm");
  }

  TEST_CASE("kCommand is reliable keep-last-1 volatile sync realtime") {
    const vlink::Qos& q = vlink::QosProfile::kCommand;
    CHECK_EQ(q.reliability.kind, vlink::Qos::Reliability::kReliable);
    CHECK_EQ(q.history.kind, vlink::Qos::History::kKeepLast);
    CHECK_EQ(q.history.depth, 1);
    CHECK_EQ(q.durability.kind, vlink::Qos::Durability::kVolatile);
    CHECK_EQ(q.publish_mode.kind, vlink::Qos::PublishMode::kSync);
    CHECK_EQ(q.additions.priority, vlink::Qos::Additions::kPriorityRealTime);
    CHECK_FALSE(q.additions.is_express);
    CHECK_EQ(q.liveliness.duration, 500);
    CHECK_EQ(q.deadline.period, -1);
    CHECK_EQ(q.lifespan.duration, -1);
    CHECK_EQ(std::string(q.name), "command");
  }

  TEST_CASE("kLog is reliable keep-last-100 volatile async background") {
    const vlink::Qos& q = vlink::QosProfile::kLog;
    CHECK_EQ(q.reliability.kind, vlink::Qos::Reliability::kReliable);
    CHECK_EQ(q.history.kind, vlink::Qos::History::kKeepLast);
    CHECK_EQ(q.history.depth, 100);
    CHECK_LE(q.history.depth, q.resource_limits.max_samples_per_instance);
    CHECK_EQ(q.durability.kind, vlink::Qos::Durability::kVolatile);
    CHECK_EQ(q.publish_mode.kind, vlink::Qos::PublishMode::kASync);
    CHECK_EQ(q.additions.priority, vlink::Qos::Additions::kPriorityBackground);
    CHECK_EQ(q.liveliness.duration, 5000);
    CHECK_EQ(q.deadline.period, -1);
    CHECK_EQ(q.lifespan.duration, -1);
    CHECK_EQ(std::string(q.name), "log");
  }

  TEST_CASE("get_available_qos_map contains all 16 expected profiles") {
    const auto& m = vlink::QosProfile::get_available_qos_map();
    CHECK_FALSE(m.empty());
    CHECK(m.count("event") == 1u);
    CHECK(m.count("method") == 1u);
    CHECK(m.count("field") == 1u);
    CHECK(m.count("sensor") == 1u);
    CHECK(m.count("parameter") == 1u);
    CHECK(m.count("service") == 1u);
    CHECK(m.count("clock") == 1u);
    CHECK(m.count("static") == 1u);
    CHECK(m.count("light") == 1u);
    CHECK(m.count("poor") == 1u);
    CHECK(m.count("better") == 1u);
    CHECK(m.count("best") == 1u);
    CHECK(m.count("large") == 1u);
    CHECK(m.count("alarm") == 1u);
    CHECK(m.count("command") == 1u);
    CHECK(m.count("log") == 1u);
    CHECK_GE(m.size(), 16u);
  }

  TEST_CASE("every map entry has valid=true") {
    const auto& m = vlink::QosProfile::get_available_qos_map();
    for (const auto& [name, qos] : m) {
      CHECK(qos.valid);
    }
  }

  TEST_CASE("get_available_qos_map returns a stable reference") {
    const auto& m1 = vlink::QosProfile::get_available_qos_map();
    const auto& m2 = vlink::QosProfile::get_available_qos_map();
    CHECK_EQ(&m1, &m2);
  }

  TEST_CASE("sensor map entry is best-effort async") {
    const auto& m = vlink::QosProfile::get_available_qos_map();
    auto it = m.find("sensor");
    REQUIRE(it != m.end());
    CHECK_EQ(it->second.reliability.kind, vlink::Qos::Reliability::kBestEffort);
    CHECK_EQ(it->second.publish_mode.kind, vlink::Qos::PublishMode::kASync);
  }

  TEST_CASE("field map entry is transient-local") {
    const auto& m = vlink::QosProfile::get_available_qos_map();
    auto it = m.find("field");
    REQUIRE(it != m.end());
    CHECK_EQ(it->second.durability.kind, vlink::Qos::Durability::kTransientLocal);
  }
}

// NOLINTEND
