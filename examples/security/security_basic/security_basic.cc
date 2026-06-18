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

#include <atomic>
#include <chrono>
#include <thread>

using namespace std::chrono_literals;  // NOLINT(build/namespaces, google-build-using-namespace)

// ---------------------------------------------------------------------------
// security_basic.cc
//
// Symmetric encryption walkthrough for SecurityPublisher / SecuritySubscriber
// (the SecurityType::kWithSecurity template specialisation of the base
// Publisher/Subscriber). VLink ships AES-128-GCM as the AEAD primitive and
// PBKDF2-HMAC-SHA256 as the KDF. Wire envelope per sample:
//
//   [u8 version][u8 mode][u16 reserved][u64 seq][u96 nonce][ciphertext][u128 tag]
//
//   version -- envelope schema version (lets future upgrades coexist).
//   mode    -- 0=raw-key, 1=passphrase, 2=RSA-wrap, 3=callback.
//   seq     -- monotonically increasing sequence number; subscriber drops
//              any sample older than (latest - replay_window).
//   nonce   -- 96-bit AES-GCM nonce, derived from seq + a per-session salt.
//   tag     -- 128-bit GCM authentication tag covering AAD + ciphertext.
//
// AAD ("associated data") covers the topic URL + sequence so a sample
// can't be replayed across topics. Any tampering (or wrong key) makes
// GCM tag verification fail and the sample is silently dropped --
// subscribers receive *nothing*, which is the correct fail-closed mode.
//
// Four configuration modes (Security::Config field-driven):
//   1. key        -- raw 16-byte AES key (caller manages key material).
//   2. passphrase -- human string + PBKDF2 -> 16-byte AES key.
//   3. RSA        -- per-session AES key wrapped with peer's RSA public.
//   4. callbacks  -- caller-supplied encrypt/decrypt functions (HSM/SE).
// Modes 1+2 are demoed here; 3 and 4 live in security_rsa / security_custom.
// ---------------------------------------------------------------------------

int main() {
  // ---- Section 1: symmetric raw key (AES-128-GCM) ----
  // Simplest mode -- both endpoints carry the same 16-byte secret string.
  // Suitable when key distribution is solved out-of-band (provisioning).
  {
    VLOG_I("[1] Symmetric Raw Key (AES-128-GCM)");

    std::atomic<int> received{0};

    vlink::Security::Config sub_cfg;
    sub_cfg.key = "my-secret-key-16";

    // SecuritySubscriber takes Config as its 2nd ctor arg -- the field
    // determines mode (key vs passphrase vs callbacks). Listener fires
    // on the DDS dispatch thread *only* if GCM tag verifies.
    vlink::SecuritySubscriber<std::string> sub("dds://security_basic/raw_key", sub_cfg);
    sub.listen([&received](const std::string& msg) {
      received++;
      VLOG_I("[Raw Key] Received: ", msg);
    });

    vlink::Security::Config pub_cfg;
    pub_cfg.key = "my-secret-key-16";

    vlink::SecurityPublisher<std::string> pub("dds://security_basic/raw_key", pub_cfg);
    pub.wait_for_subscribers();

    pub.publish("Hello with AES-128-GCM!");
    pub.publish("Authenticated encryption is automatic");
    pub.publish("Both endpoints derive the same 16-byte AES key");

    std::this_thread::sleep_for(100ms);
    VLOG_I("Raw key: received ", received.load(), " messages");
  }

  // ---- Section 2: PBKDF2 passphrase ----
  // Use when keys must be human-typeable (CLI, config). PBKDF2 stretches
  // the low-entropy input into a 16-byte AES key. Both endpoints MUST
  // agree on passphrase + salt + iteration count, otherwise they derive
  // different keys and GCM tag verification fails silently.
  {
    VLOG_I("[2] PBKDF2 Passphrase (AES-128-GCM)");

    std::atomic<int> received{0};
    const vlink::Bytes shared_salt = vlink::Bytes::from_string("vlink-example-salt-v1");

    vlink::Security::Config sub_cfg;
    sub_cfg.passphrase = "correct horse battery staple";
    sub_cfg.pbkdf2_salt = shared_salt;
    sub_cfg.pbkdf2_iterations = 200000U;

    vlink::SecuritySubscriber<std::string> sub("dds://security_basic/passphrase", sub_cfg);
    sub.listen([&received](const std::string& msg) {
      received++;
      VLOG_I("[Passphrase] Received: ", msg);
    });

    vlink::Security::Config pub_cfg;
    pub_cfg.passphrase = "correct horse battery staple";
    pub_cfg.pbkdf2_salt = shared_salt;
    pub_cfg.pbkdf2_iterations = 200000U;

    vlink::SecurityPublisher<std::string> pub("dds://security_basic/passphrase", pub_cfg);
    pub.wait_for_subscribers();

    pub.publish("Passphrase-derived AES key in use");
    pub.publish("PBKDF2 normalises low-entropy inputs");

    std::this_thread::sleep_for(100ms);
    VLOG_I("Passphrase: received ", received.load(), " messages");
  }

  // ---- Section 3: key mismatch (fail-closed demo) ----
  // Subscriber and publisher hold different keys -- GCM tag verification
  // fails on every sample and the listener never fires. Expected count
  // is exactly 0, which is the security contract.
  {
    VLOG_I("[3] Key Mismatch Failure Demo");

    std::atomic<int> received{0};

    vlink::Security::Config sub_cfg;
    sub_cfg.key = "beta--key-16byte";

    vlink::SecuritySubscriber<std::string> sub("dds://security_basic/mismatch", sub_cfg);
    sub.listen([&received](const std::string& msg) {
      received++;
      VLOG_I("[Mismatch] Received (unexpected): ", msg);
    });

    vlink::Security::Config pub_cfg;
    pub_cfg.key = "alpha-key-16byte";

    vlink::SecurityPublisher<std::string> pub("dds://security_basic/mismatch", pub_cfg);
    pub.wait_for_subscribers();

    pub.publish("This message should fail GCM authentication");
    pub.publish("Key mismatch prevents decryption");

    std::this_thread::sleep_for(200ms);
    VLOG_I("Key mismatch: received ", received.load(), " messages (expected 0)");
  }

  // ---- Section 4: security with Bytes payload ----
  // Encryption is type-agnostic: ciphering happens after Serializer turns
  // the value into Bytes. Demonstrated here with a binary 256-byte buffer.
  {
    VLOG_I("[4] Security with Bytes Type");

    std::atomic<int> received{0};

    vlink::Security::Config sub_cfg;
    sub_cfg.key = "bytes--key-16b!!";

    vlink::SecuritySubscriber<vlink::Bytes> sub("dds://security_basic/bytes", sub_cfg);
    sub.listen([&received](const vlink::Bytes& msg) {
      received++;

      // NOLINTNEXTLINE(readability-container-size-empty)
      VLOG_I("[Bytes] Received size: ", msg.size(), " first_byte: ", msg.size() > 0 ? +msg.data()[0] : -1);
    });

    vlink::Security::Config pub_cfg;
    pub_cfg.key = "bytes--key-16b!!";

    vlink::SecurityPublisher<vlink::Bytes> pub("dds://security_basic/bytes", pub_cfg);
    pub.wait_for_subscribers();

    vlink::Bytes data = vlink::Bytes::create(256);
    for (size_t i = 0; i < 256; ++i) {
      data[i] = static_cast<uint8_t>(i);
    }

    pub.publish(data);

    std::this_thread::sleep_for(100ms);
    VLOG_I("Bytes security: received ", received.load(), " messages");
  }

  // ---- Section 5: transport limitations ----
  // Some backends bake their own framing and can't carry the security
  // envelope; intra:// is in-process so encryption is meaningless. CDR-
  // mode dds:// is excluded because CDR rewrites the payload bytes.
  {
    VLOG_I("[5] Security Limitations");
    VLOG_I("  Not supported: intra://, dds:// with CDR serialization");
    VLOG_I("  Supported:     shm://, shm2://, zenoh://, mqtt://, fdbus://, etc.");
    VLOG_I("  AEAD:          AES-128-GCM (envelope AAD, sequence nonce, 16-byte tag)");
    VLOG_I("  KDF:           PBKDF2-HMAC-SHA256 (passphrase -> 16-byte AES key)");
  }

  VLOG_I("Security basic example complete.");
  return 0;
}
