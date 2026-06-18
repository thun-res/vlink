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

#include "./extension/qos.h"

#include <doctest/doctest.h>

#include <cstring>
#include <type_traits>

#include "../common_test.h"

TEST_SUITE("extension-Qos") {
  TEST_CASE("default-constructed qos has valid=false") {
    Qos qos;
    CHECK_FALSE(qos.valid);
  }

  TEST_CASE("default construction yields all expected sub-policy defaults") {
    Qos qos;
    CHECK_EQ(qos.reliability.kind, Qos::Reliability::kReliable);
    CHECK_EQ(qos.reliability.block_time, 100);
    CHECK_EQ(qos.reliability.heartbeat_time, 3000);
    CHECK_EQ(qos.history.kind, Qos::History::kKeepLast);
    CHECK_EQ(qos.history.depth, 1);
    CHECK_EQ(qos.durability.kind, Qos::Durability::kVolatile);
    CHECK_EQ(qos.publish_mode.kind, Qos::PublishMode::kSync);
    CHECK_EQ(qos.liveliness.kind, Qos::Liveliness::kAutomatic);
    CHECK_EQ(qos.liveliness.duration, -1);
    CHECK_EQ(qos.destination_order.kind, Qos::DestinationOrder::kReceptionTimestamp);
    CHECK_EQ(qos.ownership.kind, Qos::Ownership::kShared);
    CHECK_EQ(qos.deadline.period, -1);
    CHECK_EQ(qos.lifespan.duration, -1);
    CHECK_EQ(qos.latency_budget.duration, 0);
    CHECK_EQ(qos.resource_limits.max_samples, 6000);
    CHECK_EQ(qos.resource_limits.max_instances, 10);
    CHECK_EQ(qos.resource_limits.max_samples_per_instance, 500);
    CHECK_EQ(qos.additions.priority, Qos::Additions::kPriorityNormal);
    CHECK_FALSE(qos.additions.is_express);
  }

  TEST_CASE("name field is zero-initialised") {
    Qos qos;
    CHECK_EQ(qos.name[0], '\0');
  }

  TEST_CASE("name field accepts up to 19 characters") {
    Qos qos;
    const char* name = "1234567890123456789";
    const size_t name_len = std::strlen(name);
    const size_t max_len = sizeof(qos.name) - 1;
    const size_t copy_len = std::min(name_len, max_len);
    std::memcpy(qos.name, name, copy_len);
    qos.name[copy_len] = '\0';

    CHECK_EQ(std::string(qos.name), name);
    CHECK_EQ(std::strlen(qos.name), 19u);
  }

  TEST_CASE("qos is a final struct") { CHECK(std::is_final_v<Qos>); }

  TEST_CASE("all sub-policy structs are final") {
    CHECK(std::is_final_v<Qos::Reliability>);
    CHECK(std::is_final_v<Qos::History>);
    CHECK(std::is_final_v<Qos::Durability>);
    CHECK(std::is_final_v<Qos::PublishMode>);
    CHECK(std::is_final_v<Qos::Liveliness>);
    CHECK(std::is_final_v<Qos::DestinationOrder>);
    CHECK(std::is_final_v<Qos::Ownership>);
    CHECK(std::is_final_v<Qos::Deadline>);
    CHECK(std::is_final_v<Qos::Lifespan>);
    CHECK(std::is_final_v<Qos::LatencyBudget>);
    CHECK(std::is_final_v<Qos::ResourceLimits>);
    CHECK(std::is_final_v<Qos::Additions>);
  }

  TEST_CASE("copy construction preserves all fields") {
    Qos a;
    a.valid = true;
    a.history.depth = 50;
    a.reliability.block_time = 200;

    Qos b = a;
    CHECK(b.valid);
    CHECK_EQ(b.history.depth, 50);
    CHECK_EQ(b.reliability.block_time, 200);
  }

  TEST_CASE("copy assignment transfers all sub-policy fields") {
    Qos a;
    a.valid = true;
    a.reliability.kind = Qos::Reliability::kBestEffort;
    a.reliability.block_time = 999;
    a.reliability.heartbeat_time = 1234;
    a.history.kind = Qos::History::kKeepAll;
    a.history.depth = 100;
    a.durability.kind = Qos::Durability::kPersistent;
    a.publish_mode.kind = Qos::PublishMode::kASync;
    a.liveliness.kind = Qos::Liveliness::kManualTopic;
    a.liveliness.duration = 3000;
    a.destination_order.kind = Qos::DestinationOrder::kSourceTimestamp;
    a.ownership.kind = Qos::Ownership::kExclusive;
    a.deadline.period = 500;
    a.lifespan.duration = 10000;
    a.latency_budget.duration = 50;
    a.resource_limits.max_samples = 1000;
    a.resource_limits.max_instances = 5;
    a.resource_limits.max_samples_per_instance = 200;
    a.additions.priority = Qos::Additions::kPriorityRealTime;
    a.additions.is_express = true;

    Qos b;
    b = a;

    CHECK(b.valid);
    CHECK_EQ(b.reliability.kind, Qos::Reliability::kBestEffort);
    CHECK_EQ(b.reliability.block_time, 999);
    CHECK_EQ(b.reliability.heartbeat_time, 1234);
    CHECK_EQ(b.history.kind, Qos::History::kKeepAll);
    CHECK_EQ(b.history.depth, 100);
    CHECK_EQ(b.durability.kind, Qos::Durability::kPersistent);
    CHECK_EQ(b.publish_mode.kind, Qos::PublishMode::kASync);
    CHECK_EQ(b.liveliness.kind, Qos::Liveliness::kManualTopic);
    CHECK_EQ(b.liveliness.duration, 3000);
    CHECK_EQ(b.destination_order.kind, Qos::DestinationOrder::kSourceTimestamp);
    CHECK_EQ(b.ownership.kind, Qos::Ownership::kExclusive);
    CHECK_EQ(b.deadline.period, 500);
    CHECK_EQ(b.lifespan.duration, 10000);
    CHECK_EQ(b.latency_budget.duration, 50);
    CHECK_EQ(b.resource_limits.max_samples, 1000);
    CHECK_EQ(b.resource_limits.max_instances, 5);
    CHECK_EQ(b.resource_limits.max_samples_per_instance, 200);
    CHECK_EQ(b.additions.priority, Qos::Additions::kPriorityRealTime);
    CHECK(b.additions.is_express);
  }

  TEST_CASE("reliability kinds are distinct") {
    CHECK_NE(static_cast<int>(Qos::Reliability::kBestEffort), static_cast<int>(Qos::Reliability::kReliable));
  }

  TEST_CASE("reliability custom block_time and heartbeat_time") {
    Qos::Reliability r{Qos::Reliability::kReliable, 500, 1000};
    CHECK_EQ(r.block_time, 500);
    CHECK_EQ(r.heartbeat_time, 1000);
  }

  TEST_CASE("reliability zero block_time and heartbeat_time are accepted") {
    Qos::Reliability r{Qos::Reliability::kReliable, 0, 0};
    CHECK_EQ(r.block_time, 0);
    CHECK_EQ(r.heartbeat_time, 0);
  }

  TEST_CASE("history keeps last with non-default depth") {
    Qos::History h{Qos::History::kKeepLast, 10};
    CHECK_EQ(h.kind, Qos::History::kKeepLast);
    CHECK_EQ(h.depth, 10);
  }

  TEST_CASE("history keep all") {
    Qos::History h{Qos::History::kKeepAll, 1};
    CHECK_EQ(h.kind, Qos::History::kKeepAll);
  }

  TEST_CASE("durability enum values are sequential 0-3") {
    CHECK_EQ(static_cast<int>(Qos::Durability::kVolatile), 0);
    CHECK_EQ(static_cast<int>(Qos::Durability::kTransientLocal), 1);
    CHECK_EQ(static_cast<int>(Qos::Durability::kTransient), 2);
    CHECK_EQ(static_cast<int>(Qos::Durability::kPersistent), 3);
  }

  TEST_CASE("liveliness manual participant with custom duration") {
    Qos::Liveliness lv{Qos::Liveliness::kManualParticipant, 5000};
    CHECK_EQ(lv.kind, Qos::Liveliness::kManualParticipant);
    CHECK_EQ(lv.duration, 5000);
  }

  TEST_CASE("liveliness with zero duration") {
    Qos::Liveliness lv{Qos::Liveliness::kManualParticipant, 0};
    CHECK_EQ(lv.duration, 0);
  }

  TEST_CASE("resource limits custom values are stored") {
    Qos::ResourceLimits rl{1000, 5, 200};
    CHECK_EQ(rl.max_samples, 1000);
    CHECK_EQ(rl.max_instances, 5);
    CHECK_EQ(rl.max_samples_per_instance, 200);
  }

  TEST_CASE("resource limits negative sentinel is accepted") {
    Qos::ResourceLimits rl{-1, -1, -1};
    CHECK_EQ(rl.max_samples, -1);
    CHECK_EQ(rl.max_instances, -1);
    CHECK_EQ(rl.max_samples_per_instance, -1);
  }

  TEST_CASE("deadline with zero period is accepted") {
    Qos::Deadline d{0};
    CHECK_EQ(d.period, 0);
  }

  TEST_CASE("additions priority enum values are ordered") {
    CHECK_LT(static_cast<int>(Qos::Additions::kPriorityRealTime), static_cast<int>(Qos::Additions::kPriorityHigh));
    CHECK_LT(static_cast<int>(Qos::Additions::kPriorityHigh), static_cast<int>(Qos::Additions::kPriorityNormal));
    CHECK_LT(static_cast<int>(Qos::Additions::kPriorityNormal), static_cast<int>(Qos::Additions::kPriorityLow));
    CHECK_LT(static_cast<int>(Qos::Additions::kPriorityLow), static_cast<int>(Qos::Additions::kPriorityBackground));
  }

  TEST_CASE("additions priority numeric values match specification") {
    CHECK_EQ(static_cast<int>(Qos::Additions::kPriorityRealTime), 1);
    CHECK_EQ(static_cast<int>(Qos::Additions::kPriorityHigh), 2);
    CHECK_EQ(static_cast<int>(Qos::Additions::kPriorityNormal), 4);
    CHECK_EQ(static_cast<int>(Qos::Additions::kPriorityLow), 6);
    CHECK_EQ(static_cast<int>(Qos::Additions::kPriorityBackground), 7);
  }

  TEST_CASE("additions is_express flag is stored correctly") {
    Qos::Additions a{Qos::Additions::kPriorityNormal, true};
    CHECK(a.is_express);
  }
}

// NOLINTEND
