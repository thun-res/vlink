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
#include <vlink/impl/intra_data.h>
#include <vlink/vlink.h>

#include <chrono>
#include <cstring>
#include <string>

using namespace std::chrono_literals;  // NOLINT(build/namespaces, google-build-using-namespace)

// ---------------------------------------------------------------------------
// intra_data.cc
//
// IntraData is VLink's zero-copy in-process message wrapper. When publisher
// and subscriber both live in the same process and share the same loop (or
// any pair of loops over intra://), the underlying shared_ptr is passed by
// reference -- no serialization, no allocation per delivery, no memcpy.
//
// The VLINK_INTRA_DATA_DECLARE(T, Alias) macro generates two types:
//   * AliasType  -- IntraDataType<T> subclass that holds a T value member.
//                   Owns the Serializer routing tag and serialize helpers.
//   * Alias      -- std::shared_ptr<AliasType> with a create() factory.
//                   This is what user code passes around in publish/listen.
//
// The wrapped T can be any Serializer-supported type (POD, Custom, etc.).
// When the topic crosses a process boundary (shm/dds), the framework falls
// back to operator>>/<< on the inner T to produce a wire buffer; on intra
// it bypasses that path entirely (`is_owner` semantics described below).
// ---------------------------------------------------------------------------

// MyStruct provides operator>>/<<, so it is detected as kCustomType.
// IntraData wrappers may target any supported Serializer type.
struct MyStruct {
  int32_t id;
  float temperature;
  char label[32];

  void operator>>(vlink::Bytes& out) const {  // NOLINT(google-runtime-operator)
    out = vlink::Bytes::create(sizeof(MyStruct));
    std::memcpy(out.data(), this, sizeof(MyStruct));
  }

  void operator<<(const vlink::Bytes& in) {
    if (in.size() >= sizeof(MyStruct)) {
      std::memcpy(this, in.data(), sizeof(MyStruct));
    }
  }
};

// Macro expansion (conceptually):
//   class MyIntraType : public vlink::IntraDataType<MyStruct> { ... };
//   using MyIntra = std::shared_ptr<MyIntraType>;
//   inline MyIntra MyIntra_create() { return std::make_shared<MyIntraType>(); }
// In practice MyIntra exposes ::create() as a static factory.
// MyIntraType -- concrete IntraDataType subclass holding MyStruct.
// MyIntra     -- shared_ptr<MyIntraType> wrapper exposing create().
VLINK_INTRA_DATA_DECLARE(MyStruct, MyIntra)

int main() {
  // ---- Construction ----
  // create() allocates one MyIntraType on the heap; the user fills its
  // .value (the inner MyStruct) in place. No serialization runs yet --
  // the wrapper just holds a typed shared_ptr to mutable storage.
  auto data = MyIntra::create();
  data->value.id = 42;
  data->value.temperature = 36.6F;
  std::strncpy(data->value.label, "sensor_A", sizeof(data->value.label) - 1);
  VLOG_I("[Create] id=", data->value.id, " temp=", data->value.temperature, " label=", data->value.label);
  VLOG_I("[Create] kValueType=", static_cast<int>(MyIntraType::kValueType),
         " type=", MyIntraType::get_serialized_type());

  // ---- Manual serialize / deserialize for cross-transport fallback ----
  // operator>>(Bytes&) and operator<<(const Bytes&) delegate to the inner
  // MyStruct's operators. This path activates automatically when the topic
  // backend cannot share memory (e.g. dds:// across hosts). Return value
  // indicates success so caller can detect malformed input.
  auto original = MyIntra::create();
  original->value.id = 100;
  original->value.temperature = 25.0F;
  std::strncpy(original->value.label, "motor_B", sizeof(original->value.label) - 1);

  vlink::Bytes wire;
  bool ok = (*original) >> wire;
  VLOG_I("[Serialize] ok=", ok, " size=", wire.size(), " expected=", original->get_serialized_size());

  auto restored = MyIntra::create();
  ok = (*restored) << wire;
  VLOG_I("[Deserialize] ok=", ok, " id=", restored->value.id, " temp=", restored->value.temperature,
         " label=", restored->value.label);

  // ---- Zero-copy pub/sub on intra:// ----
  // The `#direct` fragment is the intra mode hint. With it, publish() hands
  // the SAME shared_ptr to the subscriber: the subscriber's `typed` and the
  // publisher's `frame` point at one heap object. The `is_owner` flag in
  // IntraDataType is what flips between "I own this storage" (subscriber-
  // side allocation when crossing a process boundary, post-deserialize) and
  // "I am sharing the producer's storage" (intra direct mode). Mutating
  // through a subscriber-side handle in direct mode would be visible to the
  // publisher -- treat received samples as read-only.
  int received_count = 0;
  const std::string topic_url = "intra://example/intra_data/struct#direct";

  vlink::Subscriber<MyIntra> sub(topic_url);
  // Listener fires inline on the publisher's thread in intra `#direct` mode
  // (no queueing). Mind the implications: heavy work in the callback blocks
  // publish().
  sub.listen([&](const MyIntra& typed) {
    received_count++;

    if (typed) {
      VLOG_I("[Sub] #", received_count, " id=", typed->value.id, " temp=", typed->value.temperature,
             " label=", typed->value.label);
    }
  });

  vlink::Publisher<MyIntra> pub(topic_url);
  // Block until at least one subscriber has matched; without this, the loop
  // below could fire publishes before the listener is wired in.
  pub.wait_for_subscribers();

  for (int i = 1; i <= 5; ++i) {
    auto frame = MyIntra::create();
    frame->value.id = i;
    frame->value.temperature = 20.0F + static_cast<float>(i);
    std::snprintf(frame->value.label, sizeof(frame->value.label), "reading_%d", i);
    pub.publish(frame);
  }

  VLOG_I("[Sub] total=", received_count);

  // ---- Shared ownership semantics ----
  // MyIntra is std::shared_ptr<MyIntraType>, so copy = atomic refcount++.
  // Both handles point at one MyStruct; mutations through either are seen
  // by the other. This is identical to how subscribers receive samples in
  // direct mode -- understand it before mutating received messages.
  auto shared = MyIntra::create();
  shared->value.id = 999;
  MyIntra alias = shared;  // NOLINT(performance-unnecessary-copy-initialization)
  VLOG_I("[Share] use_count=", shared.use_count(), " same_object=", shared.get() == alias.get());

  VLOG_I("IntraData example complete.");
  return 0;
}
