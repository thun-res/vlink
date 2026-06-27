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
#include <vlink/vlink.h>
#include <vlink/zerocopy/raw_data.h>

#include <chrono>
#include <cstring>
#include <thread>

using namespace std::chrono_literals;  // NOLINT(build/namespaces, google-build-using-namespace)

// ---------------------------------------------------------------------------
// zerocopy_basic.cc
//
// Walks the zerocopy primitives -- loan/return_loan, manual_unloan mode,
// and RawData copy semantics. The "zerocopy" promise applies to backends
// that can hand the publisher a pool-allocated buffer (currently shm://,
// shm2://). The publisher writes data directly into that buffer; the
// subscriber receives a reference to the same shared-memory region.
//
// Key contracts:
//
//   loan(size)       -- borrow a buffer from the transport pool. MUST be
//                       paired with either publish(buf) (which consumes
//                       and returns the loan automatically) or
//                       return_loan(buf) (cancel the loan unused).
//   is_support_loan  -- query whether this transport has a pool. dds://
//                       returns false; user must fall back to Bytes::create.
//   manual_unloan    -- when true, the *subscriber* owns the buffer until
//                       it explicitly calls return_loan(). Lets the
//                       subscriber defer release (e.g. while async
//                       processing). When false (default) the buffer is
//                       reclaimed as soon as the listener returns.
//
// RawData copy semantics:
//   shallow_copy -- alias the source storage; is_owner=false; cheapest.
//   deep_copy    -- allocate + memcpy; both sides own independent buffers.
//   move_copy    -- transfer ownership; source.is_valid() becomes false.
// `is_owner` flips when ownership transfers; the destructor only frees if
// is_owner is true at scope exit -- this is what makes pool buffers safe.
// ---------------------------------------------------------------------------

struct SensorSample {
  uint32_t id;
  double value;
  uint64_t timestamp;
};
int main() {
  // ---- Section 1: loan support detection ----
  // Always query first; user code must have a fallback path for transports
  // without a buffer pool (dds, intra, mqtt, ...).
  {
    VLOG_I("[1] is_support_loan()");

    vlink::Publisher<vlink::Bytes> pub_dds("dds://zerocopy_basic/check");
    VLOG_I("  dds:// is_support_loan() = ", pub_dds.is_support_loan());
    VLOG_I("  (shm:// would return true; loans are SHM-specific.)");
  }

  // ---- Section 2: loan() / return_loan() with fallback ----
  // Pattern: ask the publisher, then branch. The hot path writes directly
  // into pool memory and avoids any heap allocation on publish.
  {
    VLOG_I("[2] loan() / return_loan() with fallback");

    static constexpr size_t kPayloadSize = sizeof(SensorSample);
    vlink::Publisher<vlink::Bytes> pub("dds://zerocopy_basic/loan_demo");

    if (pub.is_support_loan()) {
      vlink::Bytes buf = pub.loan(kPayloadSize);

      if (!buf.empty()) {
        auto* sensor = reinterpret_cast<SensorSample*>(buf.data());
        sensor->id = 1;
        sensor->value = 42.0;
        sensor->timestamp = 1234567890;
        pub.publish(buf);
        VLOG_I("  Published via loaned buffer, size=", buf.size());
      }
    } else {
      VLOG_I("  Transport does not support loan -- using regular allocation");
      vlink::Bytes buf = vlink::Bytes::create(kPayloadSize);
      auto* sensor = reinterpret_cast<SensorSample*>(buf.data());
      sensor->id = 1;
      sensor->value = 42.0;
      sensor->timestamp = 1234567890;
      VLOG_I("  Allocated ", buf.size(), " bytes, ready to publish");
    }
  }

  // ---- Section 3: manual unloan mode (Subscriber) ----
  // With set_manual_unloan(true), the listener is responsible for calling
  // return_loan(msg) before the buffer's underlying pool slot can be
  // reused. Forgetting to return drains the pool and stalls the publisher.
  // Use only when listener offloads work to another thread that must keep
  // the buffer alive past listener return.
  {
    VLOG_I("[3] Manual unloan mode (Subscriber)");

    vlink::MessageLoop loop;
    loop.set_name("loan_loop");
    loop.async_run();

    vlink::Subscriber<vlink::Bytes> sub("dds://zerocopy_basic/manual");
    sub.attach(&loop);

    VLOG_I("  default is_manual_unloan() = ", sub.is_manual_unloan());
    sub.set_manual_unloan(true);
    VLOG_I("  after set_manual_unloan(true) = ", sub.is_manual_unloan());

    int received = 0;
    // Listener runs on `loop`'s thread. Because manual_unloan is on, we
    // explicitly return the buffer at the end. Skipping this would leak
    // the pool slot for the lifetime of the subscriber.
    sub.listen([&received, &sub](const vlink::Bytes& msg) {
      received++;
      VLOG_I("  [Sub] received ", msg.size(), " bytes");
      sub.return_loan(msg);
    });

    vlink::Publisher<vlink::Bytes> pub("dds://zerocopy_basic/manual");
    pub.wait_for_subscribers();
    pub.publish(vlink::Bytes::from_string("manual_unloan_test"));

    loop.wait_for_idle(1000);
    VLOG_I("  messages received: ", received);

    loop.quit();
    loop.wait_for_quit();
  }

  // ---- Section 4: RawData -- owned buffer with header and serialization ----
  // RawData is the base of CameraFrame/PointCloud: a header struct + an
  // owned byte buffer + a 16-bit reserved field for transport metadata.
  // operator>> / operator<< produce a self-describing wire format so the
  // type works over non-shm backends too (with a copy).
  {
    VLOG_I("[4] RawData: create + header + serialize");

    vlink::zerocopy::RawData original;
    original.create(1024);
    original.header.seq = 42;
    std::strncpy(original.header.frame_id, "raw_0", sizeof(original.header.frame_id) - 1);
    original.header.frame_id[sizeof(original.header.frame_id) - 1] = '\0';
    original.header.time_meas = 1711612800000000000ULL;
    original.get_reserved() = 0xABCD;

    for (size_t i = 0; i < original.size(); ++i) {
      const_cast<uint8_t*>(original.data())[i] = static_cast<uint8_t>(i & 0xFF);
    }

    VLOG_I("  size=", original.size(), " is_owner=", original.is_owner(), " reserved=0x", std::hex,
           original.get_reserved());

    vlink::Bytes wire;
    original >> wire;
    VLOG_I("  serialized=", wire.size(), " bytes, check_valid=", vlink::zerocopy::RawData::check_valid(wire));

    vlink::zerocopy::RawData restored;
    restored << wire;
    VLOG_I("  restored size=", restored.size(), " seq=", restored.header.seq, " is_owner=", restored.is_owner());
  }

  // ---- Section 5: RawData copy semantics ----
  // Three explicit copy verbs spell out cost. Default-constructing a copy
  // is forbidden; users must pick the semantics they want, eliminating
  // the "is this a deep or shallow copy?" question entirely.
  {
    VLOG_I("[5] RawData: shallow_copy vs deep_copy vs move_copy");

    vlink::zerocopy::RawData source;
    source.create(128);
    source.header.seq = 77;

    vlink::zerocopy::RawData shallow;
    shallow.shallow_copy(source);
    VLOG_I("  shallow: is_owner=", shallow.is_owner(), " data==source=", (shallow.data() == source.data()));

    vlink::zerocopy::RawData deep;
    deep.deep_copy(source);
    VLOG_I("  deep:    is_owner=", deep.is_owner(), " data==source=", (deep.data() == source.data()));

    vlink::zerocopy::RawData moved;
    moved.move_copy(source);
    VLOG_I("  moved:   moved.is_valid=", moved.is_valid(), " source.is_valid=", source.is_valid());
  }

  // ---- Section 6: pub/sub with RawData over SHM ----
  // The full zero-copy path: shm:// + RawData. Subscriber's `rd` references
  // the same shared-memory pages the publisher wrote.
  {
    VLOG_I("[6] Pub/Sub with RawData (shm://)");

    vlink::MessageLoop loop;
    loop.set_name("rawdata_loop");
    loop.async_run();

    int received = 0;
    vlink::Subscriber<vlink::zerocopy::RawData> sub("shm://zerocopy_basic/rawdata");
    sub.attach(&loop);
    // Listener fires on `loop`'s thread. `rd` is a non-owning view into
    // SHM; treat it as read-only.
    sub.listen([&received](const vlink::zerocopy::RawData& rd) {
      received++;
      VLOG_I("  [Sub] seq=", rd.header.seq, " size=", rd.size(), " bytes");
    });

    vlink::Publisher<vlink::zerocopy::RawData> pub("shm://zerocopy_basic/rawdata");
    pub.wait_for_subscribers();

    for (uint32_t i = 1; i <= 3; ++i) {
      vlink::zerocopy::RawData rd;
      rd.create(64 * i);
      rd.header.seq = i;
      rd.get_reserved() = static_cast<uint16_t>(i * 10);
      pub.publish(rd);
    }

    loop.wait_for_idle(1000);
    VLOG_I("  total received: ", received);

    loop.quit();
    loop.wait_for_quit();
  }

  // Section 7: API summary
  {
    VLOG_I("[7] API Summary");
    VLOG_I("  is_support_loan()      -- transport-pool availability");
    VLOG_I("  loan(size)             -- borrow from pool (SHM only)");
    VLOG_I("  return_loan(bytes)     -- return unused loan");
    VLOG_I("  set_manual_unloan(b)   -- subscriber controls buffer lifetime");
    VLOG_I("  RawData: create/header/serialize/shallow_copy/deep_copy/move_copy");
    VLOG_I("  Loan-capable transports: shm://, shm2://");
  }

  VLOG_I("Zerocopy basic example complete.");
  return 0;
}
