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

// Shared-memory transport with AEAD encryption sample.
//
// Exercises VLink's six Security* node variants over shm:// (iceoryx-backed):
//   SecurityServer / SecurityClient     -- encrypted RPC (Method model)
//   SecurityPublisher / SecuritySubscriber -- encrypted pub/sub (Event model)
//   SecuritySetter / SecurityGetter     -- encrypted state field (Field model)
// Each pair is the security-aware drop-in for its plain counterpart and is
// configured through vlink::Security::Config. Typical engineering scenario:
// IPC between sandboxed processes on the same host where confidentiality is
// required even though no bytes leave the machine.
//
// Prerequisite: the iceoryx RouDi daemon must be running ("iox-roudi &") so
// that shm:// endpoints can be created.

#include <vlink/base/logger.h>
#include <vlink/vlink.h>

#include <chrono>
#include <thread>

using namespace std::chrono_literals;  // NOLINT(build/namespaces, google-build-using-namespace)

int main() {
  // Security::Config supports four credential modes:
  //   (1) .key         -- raw symmetric key string (AES-128-GCM keying material).
  //   (2) .passphrase  -- KDF-derived symmetric key (passphrase + salt).
  //   (3) RSA keypair  -- asymmetric: peers exchange a session key under RSA.
  //   (4) callbacks    -- caller-supplied encrypt/decrypt functors for full
  //                       control (HSM, custom cipher, vendor SDK, ...).
  // Below we only use mode (1) for brevity; each node may pick any mode
  // independently as long as both ends agree.
  vlink::Security::Config method_sec_cfg;
  method_sec_cfg.key = "rpc-shared-key";

  // ======== Method model (RPC + encryption) ========
  // SecurityServer is the secure variant of Server; it automatically
  // encrypts/decrypts requests and responses using AES-128-GCM (AEAD).
  // Security::Config is passed as the second constructor argument.
  vlink::SecurityServer<vlink::Bytes, vlink::Bytes> server("shm://example_raw/method", method_sec_cfg);

  // Register a synchronous callback. listen() fires on the transport worker
  // thread once decryption succeeds; the response is encrypted before sending.
  server.listen([](const vlink::Bytes& req, vlink::Bytes& resp) {
    if (req == vlink::Bytes{0x1, 0x2, 0x3}) {
      // Allocate a 1MB response buffer in one shot to avoid reallocations.
      resp = vlink::Bytes::create(1024 * 1024);
      resp[0] = 0xA;                  // First byte marker.
      resp[(1024 * 1024) - 1] = 0xB;  // Last byte marker -- proves full payload integrity.
    }
  });

  // SecurityClient automatically encrypts requests and decrypts responses.
  vlink::SecurityClient<vlink::Bytes, vlink::Bytes> client("shm://example_raw/method", method_sec_cfg);

  // Synchronous call: returns std::optional<Bytes>; empty on timeout or auth fail.
  auto resp = client.invoke(vlink::Bytes{0x1, 0x2, 0x3});

  if (resp.has_value()) {
    VLOG_I("Client invoke size:", resp.value().size());
    VLOG_I("Client invoke first:", +resp.value().data()[0]);
    VLOG_I("Client invoke last:", +resp.value().data()[(1024 * 1024) - 1]);
  } else {
    VLOG_W("Client invoke failed.");
  }

  // ======== Event model (Pub/Sub + custom key) ========
  // Both ends share the exact same 16-byte key string -- AES-128 requires this
  // length; mismatched lengths trigger decryption failure on the subscriber.
  vlink::Security::Config sub_sec_cfg;
  sub_sec_cfg.key = "custom-key-16b!!";

  vlink::SecuritySubscriber<vlink::Bytes> sub("shm://example_raw/event", sub_sec_cfg);
  // Receive callback: decrypted Bytes are converted to string for display.
  sub.listen([](const vlink::Bytes& msg) { VLOG_I("sub:", msg.to_string()); });

  vlink::Security::Config pub_sec_cfg;
  pub_sec_cfg.key = "custom-key-16b!!";

  vlink::SecurityPublisher<vlink::Bytes> pub("shm://example_raw/event", pub_sec_cfg);

  // Wait for at least one matched subscriber before publishing, otherwise the
  // first events would be dropped (shm:// has no late-join history by default).
  pub.wait_for_subscribers();
  // Publish three encrypted messages back-to-back.
  pub.publish(vlink::Bytes::from_string("hello1"));
  pub.publish(vlink::Bytes::from_string("hello2"));
  pub.publish(vlink::Bytes::from_string("hello3"));

  vlink::Security::Config field_sec_cfg;
  field_sec_cfg.key = "field-shared-key";

  // ======== Field model (Getter/Setter + encryption) ========
  // SecuritySetter writes encrypted field values (latest-value cache).
  vlink::SecuritySetter<vlink::Bytes> setter("shm://example_raw/field", field_sec_cfg);

  // Write the field value {0x0A, 0x0B, 0x0C}.
  setter.set(vlink::Bytes{0xA, 0XB, 0XC});

  // SecurityGetter reads encrypted field values.
  vlink::SecurityGetter<vlink::Bytes> getter("shm://example_raw/field", field_sec_cfg);

  // Brief wait so the latest-value propagation completes before we read.
  std::this_thread::sleep_for(100ms);

  // Read the latest field value; returns std::optional, empty if not yet set.
  auto ret = getter.get();

  if (ret.has_value()) {
    VLOG_I("Getter value:", ret.value());
  } else {
    VLOG_W("Getter get failed.");
  }

  return 0;
}
