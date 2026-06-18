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

#include "./impl/types.h"

#include <doctest/doctest.h>

#include <sstream>
#include <string>
#include <type_traits>

#include "../common_test.h"

TEST_SUITE("impl-ImplType") {
  TEST_CASE("enumerator values match the documented bitmask layout") {
    CHECK_EQ(static_cast<int>(kUnknownImplType), 0);
    CHECK_EQ(static_cast<int>(kPublisher), 1);
    CHECK_EQ(static_cast<int>(kSubscriber), 2);
    CHECK_EQ(static_cast<int>(kSetter), 4);
    CHECK_EQ(static_cast<int>(kGetter), 8);
    CHECK_EQ(static_cast<int>(kServer), 16);
    CHECK_EQ(static_cast<int>(kClient), 32);
  }

  TEST_CASE("all six roles have unique power-of-two values") {
    CHECK_EQ((kPublisher & kSubscriber), 0);
    CHECK_EQ((kSetter & kGetter), 0);
    CHECK_EQ((kServer & kClient), 0);
    CHECK_EQ((kPublisher & kServer), 0);
    CHECK_EQ((kSubscriber & kGetter), 0);
  }

  TEST_CASE("bitwise OR of all roles equals 63") {
    uint8_t all = kPublisher | kSubscriber | kSetter | kGetter | kServer | kClient;

    CHECK_EQ(all, 63);
  }

  TEST_CASE("bitwise AND tests individual roles in combined mask") {
    uint8_t combined = kPublisher | kServer;

    CHECK_NE((combined & kPublisher), 0);
    CHECK_NE((combined & kServer), 0);
    CHECK_EQ((combined & kSubscriber), 0);
    CHECK_EQ((combined & kGetter), 0);
  }

  TEST_CASE("setter | getter combined mask equals 12") {
    uint8_t mask = kSetter | kGetter;

    CHECK_EQ(mask, 12);
  }

  TEST_CASE("server | client combined mask equals 48") {
    uint8_t mask = kServer | kClient;

    CHECK_EQ(mask, 48);
  }
}

TEST_SUITE("impl-TransportType") {
  TEST_CASE("enumerator values match the documented transport table") {
    CHECK_EQ(static_cast<uint8_t>(TransportType::kUnknown), 0);
    CHECK_EQ(static_cast<uint8_t>(TransportType::kIntra), 1);
    CHECK_EQ(static_cast<uint8_t>(TransportType::kShm), 2);
    CHECK_EQ(static_cast<uint8_t>(TransportType::kShm2), 3);
    CHECK_EQ(static_cast<uint8_t>(TransportType::kZenoh), 4);
    CHECK_EQ(static_cast<uint8_t>(TransportType::kDds), 5);
    CHECK_EQ(static_cast<uint8_t>(TransportType::kDdsc), 6);
    CHECK_EQ(static_cast<uint8_t>(TransportType::kDdsr), 7);
    CHECK_EQ(static_cast<uint8_t>(TransportType::kDdst), 8);
    CHECK_EQ(static_cast<uint8_t>(TransportType::kSomeip), 9);
    CHECK_EQ(static_cast<uint8_t>(TransportType::kMqtt), 10);
    CHECK_EQ(static_cast<uint8_t>(TransportType::kFdbus), 11);
    CHECK_EQ(static_cast<uint8_t>(TransportType::kQnx), 12);
  }

  TEST_CASE("distinct transports compare not-equal") {
    CHECK_NE(TransportType::kIntra, TransportType::kShm);
    CHECK_NE(TransportType::kDds, TransportType::kDdsc);
    CHECK_NE(TransportType::kZenoh, TransportType::kMqtt);
  }
}

TEST_SUITE("impl-InitType") {
  TEST_CASE("enumerator values are 0 and 1") {
    CHECK_EQ(static_cast<uint8_t>(InitType::kWithoutInit), 0);
    CHECK_EQ(static_cast<uint8_t>(InitType::kWithInit), 1);
  }

  TEST_CASE("kWithoutInit and kWithInit are distinct") { CHECK_NE(InitType::kWithoutInit, InitType::kWithInit); }
}

TEST_SUITE("impl-SecurityType") {
  TEST_CASE("enumerator values are 0 and 1") {
    CHECK_EQ(static_cast<uint8_t>(SecurityType::kWithoutSecurity), 0);
    CHECK_EQ(static_cast<uint8_t>(SecurityType::kWithSecurity), 1);
  }

  TEST_CASE("kWithoutSecurity and kWithSecurity are distinct") {
    CHECK_NE(SecurityType::kWithoutSecurity, SecurityType::kWithSecurity);
  }
}

TEST_SUITE("impl-ActionType") {
  TEST_CASE("enumerator values follow documented sequence") {
    CHECK_EQ(static_cast<uint8_t>(ActionType::kUnknownAction), 0);
    CHECK_EQ(static_cast<uint8_t>(ActionType::kClientRequest), 1);
    CHECK_EQ(static_cast<uint8_t>(ActionType::kClientResponse), 2);
    CHECK_EQ(static_cast<uint8_t>(ActionType::kServerRequest), 3);
    CHECK_EQ(static_cast<uint8_t>(ActionType::kServerResponse), 4);
    CHECK_EQ(static_cast<uint8_t>(ActionType::kPublish), 5);
    CHECK_EQ(static_cast<uint8_t>(ActionType::kSubscribe), 6);
    CHECK_EQ(static_cast<uint8_t>(ActionType::kSet), 7);
    CHECK_EQ(static_cast<uint8_t>(ActionType::kGet), 8);
  }
}

TEST_SUITE("impl-SchemaType") {
  TEST_CASE("enumerator values follow documented sequence") {
    CHECK_EQ(static_cast<uint8_t>(SchemaType::kUnknown), 0);
    CHECK_EQ(static_cast<uint8_t>(SchemaType::kRaw), 1);
    CHECK_EQ(static_cast<uint8_t>(SchemaType::kZeroCopy), 2);
    CHECK_EQ(static_cast<uint8_t>(SchemaType::kProtobuf), 3);
    CHECK_EQ(static_cast<uint8_t>(SchemaType::kFlatbuffers), 4);
  }
}

TEST_SUITE("impl-Timeout") {
  TEST_CASE("kDefaultInterval is 5000 milliseconds") { CHECK_EQ(Timeout::kDefaultInterval.count(), 5000); }

  TEST_CASE("kInfinite is negative (-1 milliseconds)") { CHECK_EQ(Timeout::kInfinite.count(), -1); }

  TEST_CASE("kDefaultInterval is positive and kInfinite is negative") {
    CHECK_GT(Timeout::kDefaultInterval.count(), 0);
    CHECK_LT(Timeout::kInfinite.count(), 0);
  }

  TEST_CASE("Timeout is a final struct") { CHECK(std::is_final_v<Timeout>); }
}

TEST_SUITE("impl-SampleLostInfo") {
  TEST_CASE("default construction yields total and lost equal to zero") {
    SampleLostInfo info;

    CHECK_EQ(info.total, 0u);
    CHECK_EQ(info.lost, 0u);
  }

  TEST_CASE("total and lost fields are independently assignable") {
    SampleLostInfo info;
    info.total = 1000;
    info.lost = 7;

    CHECK_EQ(info.total, 1000u);
    CHECK_EQ(info.lost, 7u);
  }

  TEST_CASE("operator<< produces non-empty output containing numeric values") {
    SampleLostInfo info;
    info.total = 42;
    info.lost = 3;

    std::ostringstream oss;
    oss << info;
    std::string out = oss.str();

    CHECK_FALSE(out.empty());
    CHECK_NE(out.find("42"), std::string::npos);
    CHECK_NE(out.find("3"), std::string::npos);
  }

  TEST_CASE("operator<< works with zero values") {
    SampleLostInfo info;
    std::ostringstream oss;
    oss << info;

    CHECK_FALSE(oss.str().empty());
  }

  TEST_CASE("operator<< works with maximum uint64 values") {
    SampleLostInfo info;
    info.total = UINT64_MAX;
    info.lost = UINT64_MAX;
    std::ostringstream oss;
    oss << info;

    CHECK_FALSE(oss.str().empty());
  }
}

TEST_SUITE("impl-Version") {
  TEST_CASE("default construction leaves all fields at -1") {
    Version v;

    CHECK_EQ(v.major, -1);
    CHECK_EQ(v.minor, -1);
    CHECK_EQ(v.patch, -1);
  }

  TEST_CASE("default-constructed version is not valid") {
    Version v;

    CHECK_FALSE(v.is_valid());
  }

  TEST_CASE("from_string parses canonical major.minor.patch format") {
    Version v = Version::from_string("2.1.0");

    CHECK_EQ(v.major, 2);
    CHECK_EQ(v.minor, 1);
    CHECK_EQ(v.patch, 0);
    CHECK(v.is_valid());
  }

  TEST_CASE("from_string with all-zero components produces valid version") {
    Version v = Version::from_string("0.0.0");

    CHECK_EQ(v.major, 0);
    CHECK_EQ(v.minor, 0);
    CHECK_EQ(v.patch, 0);
    CHECK(v.is_valid());
  }

  TEST_CASE("from_string with large components") {
    Version v = Version::from_string("100.200.300");

    CHECK_EQ(v.major, 100);
    CHECK_EQ(v.minor, 200);
    CHECK_EQ(v.patch, 300);
    CHECK(v.is_valid());
  }

  TEST_CASE("from_string with empty string leaves components at -1") {
    Version v = Version::from_string("");

    CHECK_FALSE(v.is_valid());
  }

  TEST_CASE("from_string with only major parses major component") {
    Version v = Version::from_string("3");

    CHECK_EQ(v.major, 3);
  }

  TEST_CASE("from_string with major.minor only parses two components") {
    Version v = Version::from_string("2.5");

    CHECK_EQ(v.major, 2);
    CHECK_EQ(v.minor, 5);
  }

  TEST_CASE("is_valid requires all three components to be non-negative") {
    Version v;
    v.major = 1;
    CHECK_FALSE(v.is_valid());

    v.minor = 0;
    CHECK_FALSE(v.is_valid());

    v.patch = 0;
    CHECK(v.is_valid());
  }

  TEST_CASE("to_string formats as major.minor.patch") {
    Version v;
    v.major = 2;
    v.minor = 3;
    v.patch = 4;

    CHECK_EQ(v.to_string(), "2.3.4");
  }

  TEST_CASE("to_string of default version uses -1 components") {
    Version v;

    CHECK_EQ(v.to_string(), "-1.-1.-1");
  }

  TEST_CASE("operator== and operator!= compare all three components") {
    Version a = Version::from_string("1.2.3");
    Version b = Version::from_string("1.2.3");
    Version c = Version::from_string("1.2.4");

    CHECK(a == b);
    CHECK_FALSE(a != b);
    CHECK(a != c);
    CHECK_FALSE(a == c);
  }

  TEST_CASE("operator< orders by major then minor then patch") {
    SUBCASE("major ordering") {
      Version lo = Version::from_string("1.0.0");
      Version hi = Version::from_string("2.0.0");
      CHECK(lo < hi);
      CHECK_FALSE(hi < lo);
    }

    SUBCASE("minor ordering when major equal") {
      Version lo = Version::from_string("1.1.0");
      Version hi = Version::from_string("1.2.0");
      CHECK(lo < hi);
      CHECK_FALSE(hi < lo);
    }

    SUBCASE("patch ordering when major and minor equal") {
      Version lo = Version::from_string("1.0.0");
      Version hi = Version::from_string("1.0.1");
      CHECK(lo < hi);
      CHECK_FALSE(hi < lo);
    }

    SUBCASE("equal versions are not less-than") {
      Version a = Version::from_string("1.2.3");
      Version b = Version::from_string("1.2.3");
      CHECK_FALSE(a < b);
      CHECK_FALSE(b < a);
    }
  }

  TEST_CASE("operator< is transitive") {
    Version a = Version::from_string("1.0.0");
    Version b = Version::from_string("1.1.0");
    Version c = Version::from_string("2.0.0");

    CHECK(a < b);
    CHECK(b < c);
    CHECK(a < c);
  }

  TEST_CASE("operator> is the inverse of operator<") {
    Version a = Version::from_string("2.0.0");
    Version b = Version::from_string("1.0.0");

    CHECK(a > b);
    CHECK_FALSE(b > a);
  }

  TEST_CASE("equal versions satisfy neither operator< nor operator>") {
    Version a = Version::from_string("1.2.3");
    Version b = Version::from_string("1.2.3");

    CHECK_FALSE(a < b);
    CHECK_FALSE(a > b);
  }
}

TEST_SUITE("impl-SchemaData") {
  TEST_CASE("default construction leaves all fields at zero/empty/kUnknown") {
    SchemaData sd;

    CHECK(sd.name.empty());
    CHECK(sd.encoding.empty());
    CHECK_EQ(sd.schema_type, SchemaType::kUnknown);
    CHECK(sd.data.empty());
  }

  TEST_CASE("fields are independently assignable") {
    SchemaData sd;
    sd.name = "MyProtoMsg";
    sd.encoding = "protobuf";
    sd.schema_type = SchemaType::kProtobuf;
    sd.data = Bytes::create(64);

    CHECK_EQ(sd.name, "MyProtoMsg");
    CHECK_EQ(sd.encoding, "protobuf");
    CHECK_EQ(sd.schema_type, SchemaType::kProtobuf);
    CHECK_FALSE(sd.data.empty());
  }

  TEST_CASE("infer_ser_type classifies textual and raw aliases as kRaw") {
    CHECK_EQ(SchemaData::infer_ser_type("raw"), SchemaType::kRaw);
    CHECK_EQ(SchemaData::infer_ser_type("string"), SchemaType::kRaw);
    CHECK_EQ(SchemaData::infer_ser_type("std::string"), SchemaType::kRaw);
    CHECK_EQ(SchemaData::infer_ser_type("text"), SchemaType::kRaw);
    CHECK_EQ(SchemaData::infer_ser_type("json"), SchemaType::kRaw);
    CHECK_EQ(SchemaData::infer_ser_type("application/json"), SchemaType::kRaw);
    CHECK_EQ(SchemaData::infer_ser_type("text/json"), SchemaType::kRaw);
  }

  TEST_CASE("infer_ser_type classifies vlink::zerocopy:: prefixed types as kZeroCopy") {
    CHECK_EQ(SchemaData::infer_ser_type("vlink::zerocopy::CameraFrame"), SchemaType::kZeroCopy);
    CHECK_EQ(SchemaData::infer_ser_type("vlink::zerocopy::PointCloud"), SchemaType::kZeroCopy);
    CHECK_EQ(SchemaData::infer_ser_type("vlink::zerocopy::RawData"), SchemaType::kZeroCopy);
  }

  TEST_CASE("infer_ser_type returns kUnknown for unrecognised names") {
    CHECK_EQ(SchemaData::infer_ser_type(""), SchemaType::kUnknown);
    CHECK_EQ(SchemaData::infer_ser_type("demo.proto.PointCloud"), SchemaType::kUnknown);
    CHECK_EQ(SchemaData::infer_ser_type("demo.fbs.Table"), SchemaType::kUnknown);
  }

  TEST_CASE("resolve_type uses explicit schema_type first") {
    CHECK_EQ(SchemaData::resolve_type(SchemaType::kProtobuf, "vlink::zerocopy::X", "flatbuffers"),
             SchemaType::kProtobuf);
  }

  TEST_CASE("resolve_type falls back to encoding when schema_type is kUnknown") {
    CHECK_EQ(SchemaData::resolve_type(SchemaType::kUnknown, "irrelevant", "protobuf"), SchemaType::kProtobuf);
    CHECK_EQ(SchemaData::resolve_type(SchemaType::kUnknown, "irrelevant", "bfbs"), SchemaType::kFlatbuffers);
    CHECK_EQ(SchemaData::resolve_type(SchemaType::kUnknown, "irrelevant", "blob"), SchemaType::kRaw);
  }

  TEST_CASE("resolve_type falls back to ser_type when schema_type and encoding are unknown") {
    CHECK_EQ(SchemaData::resolve_type(SchemaType::kUnknown, "vlink::zerocopy::PointCloud", ""), SchemaType::kZeroCopy);
    CHECK_EQ(SchemaData::resolve_type(SchemaType::kUnknown, "string", ""), SchemaType::kRaw);
  }

  TEST_CASE("SchemaData copy constructor duplicates all fields") {
    SchemaData a;
    a.name = "test.Message";
    a.encoding = "protobuf";
    a.schema_type = SchemaType::kProtobuf;
    a.data = Bytes::create(10);

    SchemaData b = a;

    CHECK_EQ(b.name, a.name);
    CHECK_EQ(b.encoding, a.encoding);
    CHECK_EQ(b.schema_type, a.schema_type);
    CHECK_EQ(b.data.size(), a.data.size());
  }

  TEST_CASE("SchemaData move constructor transfers all fields") {
    SchemaData a;
    a.name = "test.Message";
    a.encoding = "protobuf";
    a.schema_type = SchemaType::kProtobuf;
    a.data = Bytes::create(10);

    SchemaData b = std::move(a);

    CHECK_EQ(b.name, "test.Message");
    CHECK_EQ(b.encoding, "protobuf");
    CHECK_EQ(b.schema_type, SchemaType::kProtobuf);
    CHECK_EQ(b.data.size(), 10u);
  }
}

// NOLINTEND
