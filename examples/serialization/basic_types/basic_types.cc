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

#include <vlink/base/logger.h>
#include <vlink/extension/someip_serializer.h>
#include <vlink/vlink.h>

#include <array>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

using namespace std::chrono_literals;  // NOLINT(build/namespaces, google-build-using-namespace)

// ---------------------------------------------------------------------------
// basic_types.cc
//
// Demonstrates VLink's compile-time serialization dispatch for the four
// built-in and macro-declared wire shapes:
//   * vlink::Bytes  -> kBytesType    (raw payload bytes)
//   * std::string   -> kStringType   (string bytes, no terminator)
//   * POD struct    -> kStandardType (sizeof(T) raw memcpy, host endian)
//   * SOME/IP struct -> kSomeipType  (field-wise portable payload)
// Serializer::get_type_of<T>() picks the correct strategy at compile time via
// trait probing (is_bytes_type / is_string_type / is_standard_type / etc.),
// so user code never specifies a wire format manually. Each helper below
// runs a publish-subscribe round trip on the intra-process backend so the
// listen() callback exercises the deserialization path end to end.
// ---------------------------------------------------------------------------

// Plain Old Data demo struct (kStandardType).
// Trivial + standard-layout is the contract that lets Serializer route to the
// raw-memcpy strategy. The explicit reserved field is application data, not an
// ABI guarantee: both endpoints must still agree on compiler layout and endian.
struct SensorReading {
  uint32_t sensor_id;
  double temperature;
  double humidity;
  int64_t timestamp_us;
  uint8_t status;
  uint8_t reserved[7];
};

struct SomeipChild {
  int16_t delta{0};
  std::string label;

  VLINK_SOMEIP_FIELDS(delta, label)
};

struct SomeipMessage {
  uint8_t state{0};
  std::vector<uint16_t> samples;
  std::array<SomeipChild, 2> children;

  VLINK_SOMEIP_FIELDS(state, samples, children)
};

static_assert(std::is_trivial_v<SensorReading>, "SensorReading must be trivial");
static_assert(std::is_standard_layout_v<SensorReading>, "SensorReading must be standard-layout");

// Compile-time check: the Serializer dispatch picks the right strategy per type.
// These static_asserts are the *whole* contract — if a future refactor breaks
// trait routing, the build fails here instead of silently corrupting wire data.
static_assert(vlink::Serializer::get_type_of<vlink::Bytes>() == vlink::Serializer::kBytesType);
static_assert(vlink::Serializer::get_type_of<std::string>() == vlink::Serializer::kStringType);
static_assert(vlink::Serializer::get_type_of<SensorReading>() == vlink::Serializer::kStandardType);
static_assert(vlink::Serializer::get_type_of<SomeipMessage>() == vlink::Serializer::kSomeipType);
static_assert(vlink::Serializer::get_type_of<int>() == vlink::Serializer::kStandardType);

// Wire format for Bytes is the raw payload with no framework length prefix.
// Ordinary Bytes copies are deep; explicit shallow_copy creates a non-owning
// alias. Transport delivery may still copy, so this example makes no storage-
// identity guarantee between publisher and subscriber.
static void demo_bytes() {
  VLOG_I("--- Bytes (kBytesType) ---");

  int received = 0;
  vlink::Subscriber<vlink::Bytes> sub("intra://example/basic/bytes#direct");
  // #direct invokes the callback synchronously before publish() returns.
  sub.listen([&received](const vlink::Bytes& msg) {
    received++;
    VLOG_I("[Bytes] #", received, " size=", msg.size(), " text=", msg.to_string());
  });

  vlink::Publisher<vlink::Bytes> pub("intra://example/basic/bytes#direct");

  // Four publishes exercising different construction paths:
  //   1. from_string -> copy a const char* into a freshly allocated buffer.
  //   2. initializer_list -> raw bytes "Hello".
  //   3. preallocated buffer filled in place (zero-copy fill pattern).
  //   4. empty Bytes -- valid edge case, represented by an empty payload.
  pub.publish(vlink::Bytes::from_string("Hello VLink Bytes!"));
  pub.publish(vlink::Bytes{0x48, 0x65, 0x6C, 0x6C, 0x6F});

  auto buf = vlink::Bytes::create(8);
  std::memset(buf.data(), 0xAB, buf.size());
  pub.publish(buf);

  pub.publish(vlink::Bytes{});

  // Utility round-trip: base64 + crc32. Demonstrates that Bytes carries the
  // standard wire-helpers users will need for logging/debugging/integrity.
  auto sample = vlink::Bytes::from_string("VLink");
  auto b64 = vlink::Bytes::encode_to_base64(sample);
  auto decoded = vlink::Bytes::decode_from_base64(b64);
  VLOG_I("[Bytes] base64=", b64, " decoded=", decoded.to_string(), " crc32=", vlink::Bytes::get_crc_32(sample));
}

// Wire format for std::string is its character bytes without a framework
// length prefix or trailing NUL. The trait routes through kStringType and
// reconstructs the receiving std::string from the payload size supplied by
// the transport.
static void demo_string() {
  VLOG_I("--- std::string (kStringType) ---");

  int received = 0;
  vlink::Subscriber<std::string> sub("intra://example/basic/string#direct");
  // The string parameter is valid only for this callback invocation.
  sub.listen([&received](const std::string& msg) {
    received++;
    VLOG_I("[String] #", received, " len=", msg.size(), " text=\"", msg, "\"");
  });

  vlink::Publisher<std::string> pub("intra://example/basic/string#direct");

  // Five publishes covering: short ASCII, multibyte content, empty, long
  // single-char payload (payload-length preservation), and an embedded JSON
  // literal with quotes (verifies no in-band escaping is required).
  pub.publish(std::string("Hello, VLink!"));
  pub.publish(std::string("UTF-8 supported"));
  pub.publish(std::string(""));
  pub.publish(std::string(200, 'A'));
  pub.publish(std::string(R"({"key":"value","count":42})"));

  // Manual round-trip via Serializer -- bypasses the pub/sub plumbing to show
  // that Serializer::serialize/deserialize is the one true wire codec, callable
  // directly by tooling (bag writers, custom transports, tests).
  std::string original = "round-trip test payload";
  vlink::Bytes buf;
  vlink::Serializer::serialize(original, buf);

  std::string restored;
  vlink::Serializer::deserialize(buf, restored);
  VLOG_I("[String] round-trip match=", original == restored);
}

// Wire format for kStandardType: raw memcpy of sizeof(T), host endian.
// This is the fastest path -- no length prefix, no schema -- but the
// publisher and subscriber must agree on the exact struct layout (same
// compiler ABI, same alignment, same field order). Cross-architecture or
// cross-language interop should prefer Proto/CDR/FlatTable instead.
static void demo_pod() {
  VLOG_I("--- POD struct (kStandardType) ---");

  int received = 0;
  vlink::Subscriber<SensorReading> sub("intra://example/basic/pod#direct");
  // r references a temporary in the subscriber's deserialization buffer,
  // valid only for the duration of the callback.
  sub.listen([&received](const SensorReading& r) {
    received++;
    VLOG_I("[POD] #", received, " id=", r.sensor_id, " temp=", r.temperature, " humidity=", r.humidity,
           " status=", static_cast<int>(r.status));
  });

  vlink::Publisher<SensorReading> pub("intra://example/basic/pod#direct");

  SensorReading reading{};
  reading.sensor_id = 42;
  reading.temperature = 23.5;
  reading.humidity = 65.2;
  reading.timestamp_us = 1711612800000000LL;
  reading.status = 1;
  pub.publish(reading);

  VLOG_I("[POD] sizeof(SensorReading)=", sizeof(SensorReading),
         " serialized_size=", vlink::Serializer::get_serialized_size(reading));
}

static void demo_someip() {
  VLOG_I("--- SOME/IP struct (kSomeipType) ---");

  vlink::Subscriber<SomeipMessage> sub("intra://example/basic/someip#direct");
  sub.listen([](const SomeipMessage& msg) {
    VLOG_I("[SOME/IP] state=", static_cast<int>(msg.state), " samples=", msg.samples.size(),
           " first child=", msg.children[0].label);
  });

  vlink::Publisher<SomeipMessage> pub("intra://example/basic/someip#direct");

  SomeipMessage msg;
  msg.state = 1U;
  msg.samples = {0x1234U, 0xABCDU};
  msg.children = {{{-2, "left"}, {3, "right"}}};
  pub.publish(msg);
}

int main() {
  // #direct keeps each demo synchronous, so its local endpoints remain alive
  // until every callback has completed.
  demo_bytes();
  demo_string();
  demo_pod();
  demo_someip();

  VLOG_I("=== Basic types demo complete ===");
  return 0;
}
