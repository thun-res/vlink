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

#include <memory>
#include <sstream>
#include <string>

#include "../common_test.h"
#include "./extension/status_detail.h"

TEST_SUITE("extension-Status") {
  TEST_CASE("type enum has expected numeric values") {
    CHECK_EQ(static_cast<int>(Status::kUnknown), 0);
    CHECK_EQ(static_cast<int>(Status::kPublicationMatched), 1);
    CHECK_EQ(static_cast<int>(Status::kOfferedDeadlineMissed), 2);
    CHECK_EQ(static_cast<int>(Status::kOfferedIncompatibleQos), 3);
    CHECK_EQ(static_cast<int>(Status::kLivelinessLost), 4);
    CHECK_EQ(static_cast<int>(Status::kSubscriptionMatched), 5);
    CHECK_EQ(static_cast<int>(Status::kRequestedDeadlineMissed), 6);
    CHECK_EQ(static_cast<int>(Status::kLivelinessChanged), 7);
    CHECK_EQ(static_cast<int>(Status::kSampleRejected), 8);
    CHECK_EQ(static_cast<int>(Status::kRequestedIncompatibleQos), 9);
    CHECK_EQ(static_cast<int>(Status::kSampleLost), 10);
  }

  TEST_CASE("is_for_writer returns false for kUnknown") { CHECK_FALSE(Status::is_for_writer(Status::kUnknown)); }

  TEST_CASE("is_for_writer returns true for all writer types") {
    CHECK(Status::is_for_writer(Status::kPublicationMatched));
    CHECK(Status::is_for_writer(Status::kOfferedDeadlineMissed));
    CHECK(Status::is_for_writer(Status::kOfferedIncompatibleQos));
    CHECK(Status::is_for_writer(Status::kLivelinessLost));
  }

  TEST_CASE("is_for_writer returns false for all reader types") {
    CHECK_FALSE(Status::is_for_writer(Status::kSubscriptionMatched));
    CHECK_FALSE(Status::is_for_writer(Status::kRequestedDeadlineMissed));
    CHECK_FALSE(Status::is_for_writer(Status::kLivelinessChanged));
    CHECK_FALSE(Status::is_for_writer(Status::kSampleRejected));
    CHECK_FALSE(Status::is_for_writer(Status::kRequestedIncompatibleQos));
    CHECK_FALSE(Status::is_for_writer(Status::kSampleLost));
  }

  TEST_CASE("is_for_reader returns true for all reader types") {
    CHECK(Status::is_for_reader(Status::kSubscriptionMatched));
    CHECK(Status::is_for_reader(Status::kRequestedDeadlineMissed));
    CHECK(Status::is_for_reader(Status::kLivelinessChanged));
    CHECK(Status::is_for_reader(Status::kSampleRejected));
    CHECK(Status::is_for_reader(Status::kRequestedIncompatibleQos));
    CHECK(Status::is_for_reader(Status::kSampleLost));
  }

  TEST_CASE("is_for_reader returns false for all writer types") {
    CHECK_FALSE(Status::is_for_reader(Status::kPublicationMatched));
    CHECK_FALSE(Status::is_for_reader(Status::kOfferedDeadlineMissed));
    CHECK_FALSE(Status::is_for_reader(Status::kOfferedIncompatibleQos));
    CHECK_FALSE(Status::is_for_reader(Status::kLivelinessLost));
  }

  TEST_CASE("Unknown get_type returns kUnknown and get_string is non-empty") {
    Status::Unknown u;
    CHECK_EQ(u.get_type(), Status::kUnknown);
    CHECK_FALSE(u.get_string().empty());
  }

  TEST_CASE("Unknown ostream operator writes non-empty text") {
    Status::Unknown u;
    std::ostringstream oss;
    oss << u;
    CHECK_FALSE(oss.str().empty());
  }

  TEST_CASE("as() on Unknown throws RuntimeError") {
    auto u = std::make_shared<Status::Unknown>();
    CHECK_THROWS_AS((void)u->as<Status::PublicationMatched>(), Exception::RuntimeError);
  }

  // TEST_CASE("as() with wrong concrete type returns nullptr") {
  //   auto pub = std::make_shared<Status::PublicationMatched>();
  //   auto lost = pub->as<Status::SampleLost>();
  //   CHECK_EQ(lost, nullptr);
  // }

  TEST_CASE("PublicationMatched default fields are zero-initialised") {
    Status::PublicationMatched s;
    CHECK_EQ(s.get_type(), Status::kPublicationMatched);
    CHECK_EQ(s.total_count, 0);
    CHECK_EQ(s.total_count_change, 0);
    CHECK_EQ(s.current_count, 0);
    CHECK_EQ(s.current_count_change, 0);
    CHECK_EQ(s.last_subscription_handle, nullptr);
  }

  TEST_CASE("PublicationMatched fields are mutable and as() round-trips") {
    auto base = std::make_shared<Status::PublicationMatched>();
    base->total_count = 5;
    base->current_count = 2;
    base->total_count_change = 1;
    base->current_count_change = -1;

    auto derived = base->as<Status::PublicationMatched>();
    REQUIRE_NE(derived, nullptr);
    CHECK_EQ(derived->total_count, 5);
    CHECK_EQ(derived->current_count, 2);
    CHECK_FALSE(base->get_string().empty());
  }

  TEST_CASE("OfferedDeadlineMissed default fields and ostream") {
    Status::OfferedDeadlineMissed s;
    CHECK_EQ(s.get_type(), Status::kOfferedDeadlineMissed);
    CHECK_EQ(s.total_count, 0);
    CHECK_EQ(s.total_count_change, 0);
    CHECK_EQ(s.last_instance_handle, nullptr);
    CHECK_FALSE(s.get_string().empty());

    s.total_count = 5;
    s.total_count_change = 2;
    std::ostringstream oss;
    oss << s;
    CHECK_NE(oss.str().find("OfferedDeadlineMissed"), std::string::npos);
    CHECK_NE(oss.str().find("total_count"), std::string::npos);
  }

  TEST_CASE("OfferedIncompatibleQos default fields and mutation") {
    Status::OfferedIncompatibleQos s;
    CHECK_EQ(s.get_type(), Status::kOfferedIncompatibleQos);
    CHECK_EQ(s.total_count, 0);
    CHECK_EQ(s.total_count_change, 0);
    CHECK_EQ(s.last_policy_id, 0);
    CHECK_FALSE(s.get_string().empty());

    s.total_count = 2;
    s.last_policy_id = 7;
    CHECK_EQ(s.total_count, 2);
    CHECK_EQ(s.last_policy_id, 7);

    std::ostringstream oss;
    oss << s;
    CHECK_NE(oss.str().find("OfferedIncompatibleQos"), std::string::npos);
    CHECK_NE(oss.str().find("last_policy_id"), std::string::npos);
  }

  TEST_CASE("LivelinessLost default fields and ostream") {
    Status::LivelinessLost s;
    CHECK_EQ(s.get_type(), Status::kLivelinessLost);
    CHECK_EQ(s.total_count, 0);
    CHECK_EQ(s.total_count_change, 0);
    CHECK_FALSE(s.get_string().empty());

    s.total_count = 1;
    std::ostringstream oss;
    oss << s;
    CHECK_NE(oss.str().find("LivelinessLost"), std::string::npos);
  }

  TEST_CASE("SubscriptionMatched default fields and as() round-trip") {
    Status::SubscriptionMatched s;
    CHECK_EQ(s.get_type(), Status::kSubscriptionMatched);
    CHECK_EQ(s.total_count, 0);
    CHECK_EQ(s.total_count_change, 0);
    CHECK_EQ(s.current_count, 0);
    CHECK_EQ(s.current_count_change, 0);
    CHECK_EQ(s.last_publication_handle, nullptr);
    CHECK_FALSE(s.get_string().empty());

    auto base = std::make_shared<Status::SubscriptionMatched>();
    base->current_count = 3;
    auto derived = base->as<Status::SubscriptionMatched>();
    REQUIRE_NE(derived, nullptr);
    CHECK_EQ(derived->current_count, 3);

    std::ostringstream oss;
    oss << s;
    CHECK_NE(oss.str().find("SubscriptionMatched"), std::string::npos);
    CHECK_NE(oss.str().find("current_count"), std::string::npos);
  }

  TEST_CASE("RequestedDeadlineMissed default fields and ostream") {
    Status::RequestedDeadlineMissed s;
    CHECK_EQ(s.get_type(), Status::kRequestedDeadlineMissed);
    CHECK_EQ(s.total_count, 0);
    CHECK_EQ(s.total_count_change, 0);
    CHECK_EQ(s.last_instance_handle, nullptr);
    CHECK_FALSE(s.get_string().empty());

    s.total_count = 4;
    std::ostringstream oss;
    oss << s;
    CHECK_NE(oss.str().find("RequestedDeadlineMissed"), std::string::npos);
  }

  TEST_CASE("LivelinessChanged default fields and mutation") {
    Status::LivelinessChanged s;
    CHECK_EQ(s.get_type(), Status::kLivelinessChanged);
    CHECK_EQ(s.alive_count, 0);
    CHECK_EQ(s.not_alive_count, 0);
    CHECK_EQ(s.alive_count_change, 0);
    CHECK_EQ(s.not_alive_count_change, 0);
    CHECK_EQ(s.last_publication_handle, nullptr);

    s.alive_count = 2;
    s.not_alive_count = 1;
    s.alive_count_change = 1;
    s.not_alive_count_change = -1;
    CHECK_EQ(s.alive_count, 2);
    CHECK_EQ(s.not_alive_count, 1);

    std::ostringstream oss;
    oss << s;
    CHECK_NE(oss.str().find("LivelinessChanged"), std::string::npos);
    CHECK_NE(oss.str().find("alive_count"), std::string::npos);
  }

  TEST_CASE("SampleRejected default fields and Kind enum values") {
    Status::SampleRejected s;
    CHECK_EQ(s.get_type(), Status::kSampleRejected);
    CHECK_EQ(s.total_count, 0);
    CHECK_EQ(s.total_count_change, 0);
    CHECK_EQ(s.last_reason, Status::SampleRejected::kNotRejected);
    CHECK_EQ(s.last_instance_handle, nullptr);

    CHECK_EQ(static_cast<int>(Status::SampleRejected::kNotRejected), 0);
    CHECK_EQ(static_cast<int>(Status::SampleRejected::kRejectedByInstancesLimit), 1);
    CHECK_EQ(static_cast<int>(Status::SampleRejected::kRejectedBySamplesLimit), 2);
    CHECK_EQ(static_cast<int>(Status::SampleRejected::kRejectedBySamplesPerInstanceLimit), 3);

    s.last_reason = Status::SampleRejected::kRejectedBySamplesLimit;
    CHECK_EQ(s.last_reason, Status::SampleRejected::kRejectedBySamplesLimit);

    s.total_count = 6;
    std::ostringstream oss;
    oss << s;
    CHECK_NE(oss.str().find("SampleRejected"), std::string::npos);
    CHECK_NE(oss.str().find("last_reason"), std::string::npos);
  }

  TEST_CASE("RequestedIncompatibleQos default fields and ostream") {
    Status::RequestedIncompatibleQos s;
    CHECK_EQ(s.get_type(), Status::kRequestedIncompatibleQos);
    CHECK_EQ(s.total_count, 0);
    CHECK_EQ(s.total_count_change, 0);
    CHECK_EQ(s.last_policy_id, 0);

    s.total_count = 2;
    s.last_policy_id = 5;
    std::ostringstream oss;
    oss << s;
    CHECK_NE(oss.str().find("RequestedIncompatibleQos"), std::string::npos);
  }

  TEST_CASE("SampleLost default fields and ostream") {
    Status::SampleLost s;
    CHECK_EQ(s.get_type(), Status::kSampleLost);
    CHECK_EQ(s.total_count, 0);
    CHECK_EQ(s.total_count_change, 0);
    CHECK_FALSE(s.get_string().empty());

    s.total_count = 8;
    s.total_count_change = 3;
    std::ostringstream oss;
    oss << s;
    CHECK_NE(oss.str().find("SampleLost"), std::string::npos);
    CHECK_NE(oss.str().find("total_count"), std::string::npos);
  }

  TEST_CASE("BasePtr ostream writes non-empty text for concrete status types") {
    SUBCASE("PublicationMatched") {
      Status::BasePtr ptr = std::make_shared<Status::PublicationMatched>();
      std::ostringstream oss;
      oss << ptr;
      CHECK_FALSE(oss.str().empty());
    }

    SUBCASE("SubscriptionMatched") {
      Status::BasePtr ptr = std::make_shared<Status::SubscriptionMatched>();
      std::ostringstream oss;
      oss << ptr;
      CHECK_FALSE(oss.str().empty());
    }

    SUBCASE("SampleRejected") {
      Status::BasePtr ptr = std::make_shared<Status::SampleRejected>();
      std::ostringstream oss;
      oss << ptr;
      CHECK_FALSE(oss.str().empty());
    }
  }
}

// NOLINTEND
