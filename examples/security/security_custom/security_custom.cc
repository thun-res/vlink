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
#include <vlink/extension/security.h>
#include <vlink/vlink.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <thread>

using namespace std::chrono_literals;  // NOLINT(build/namespaces, google-build-using-namespace)

// ---------------------------------------------------------------------------
// security_custom.cc
//
// Demonstrates the 4th Security::Config mode: caller-supplied encrypt /
// decrypt callbacks. Used to integrate hardware security modules, secure
// enclaves, smart cards, or any cipher VLink doesn't ship natively.
//
// Callback signature:
//   bool(const Bytes& plaintext_or_cipher_in, Bytes& cipher_or_plain_out);
//   return true on success; false signals failure and the framework drops
//   the sample (fail-closed, same contract as a GCM tag mismatch).
//
// Setting encrypt_callback + decrypt_callback fully bypasses VLink's AES-
// 128-GCM path -- no envelope, no AAD, no replay window. The caller is
// solely responsible for confidentiality, integrity, and replay defence.
// Config is consumed at construction; rebuild the publisher/subscriber to
// rotate keys or change cipher.
//
// The XOR / ROT-N demos below are illustrative only; do NOT ship anything
// like them in production.
// ---------------------------------------------------------------------------

// Toy XOR cipher for demo only -- production code should use a real AEAD.
static constexpr uint8_t kXorKey[] = {0xDE, 0xAD, 0xBE, 0xEF, 0xCA, 0xFE, 0xBA, 0xBE};

static bool xor_transform(const vlink::Bytes& in, vlink::Bytes& out) {
  if (in.empty()) {
    return false;
  }

  out = vlink::Bytes::create(in.size());
  for (size_t i = 0; i < in.size(); ++i) {
    out[i] = in.data()[i] ^ kXorKey[i % sizeof(kXorKey)];
  }

  return true;
}
int main() {
  // ---- Section 1: custom XOR via Security::Config callbacks ----
  // XOR is self-inverse, so the same function serves as both encrypt and
  // decrypt. Useful for confirming the callback wiring works in isolation
  // before swapping in a real cipher.
  {
    VLOG_I("[1] Custom XOR Cipher via Security::Config callbacks");

    std::atomic<int> received{0};

    vlink::Security::Config sub_cfg;
    sub_cfg.encrypt_callback = xor_transform;
    sub_cfg.decrypt_callback = xor_transform;

    vlink::SecuritySubscriber<std::string> sub("dds://security_custom/xor", sub_cfg);
    // Listener fires on the DDS dispatch thread *only* when the decrypt
    // callback returns true. A `return false` from decrypt drops the sample.
    sub.listen([&received](const std::string& msg) {
      received++;
      VLOG_I("[XOR] Received: ", msg);
    });

    vlink::Security::Config pub_cfg;
    pub_cfg.encrypt_callback = xor_transform;
    pub_cfg.decrypt_callback = xor_transform;

    vlink::SecurityPublisher<std::string> pub("dds://security_custom/xor", pub_cfg);
    pub.wait_for_subscribers();

    pub.publish("Hello with XOR cipher!");
    pub.publish("Custom encryption bypasses AES");

    std::this_thread::sleep_for(100ms);
    VLOG_I("XOR cipher: received ", received.load(), " messages");
  }

  // ---- Section 2: lambda-based callbacks (ROT-N) ----
  // Two distinct callbacks (encrypt != decrypt) prove the framework
  // routes the right direction. Captures-by-value lambdas convert to the
  // Function wrapper Security::Config holds.
  {
    VLOG_I("[2] Lambda-based Custom Cipher (ROT-N)");

    static constexpr uint8_t kRotation = 13;

    auto rot_encrypt = [](const vlink::Bytes& in, vlink::Bytes& out) -> bool {
      if (in.empty()) {
        return false;
      }

      out = vlink::Bytes::create(in.size());
      for (size_t i = 0; i < in.size(); ++i) {
        out[i] = in.data()[i] + kRotation;
      }

      return true;
    };

    auto rot_decrypt = [](const vlink::Bytes& in, vlink::Bytes& out) -> bool {
      if (in.empty()) {
        return false;
      }

      out = vlink::Bytes::create(in.size());
      for (size_t i = 0; i < in.size(); ++i) {
        out[i] = in.data()[i] - kRotation;
      }

      return true;
    };

    std::atomic<int> received{0};

    vlink::Security::Config sub_cfg;
    sub_cfg.encrypt_callback = rot_encrypt;
    sub_cfg.decrypt_callback = rot_decrypt;

    vlink::SecuritySubscriber<std::string> sub("dds://security_custom/lambda", sub_cfg);
    sub.listen([&received](const std::string& msg) {
      received++;
      VLOG_I("[ROT-N] Received: ", msg);
    });

    vlink::Security::Config pub_cfg;
    pub_cfg.encrypt_callback = rot_encrypt;
    pub_cfg.decrypt_callback = rot_decrypt;

    vlink::SecurityPublisher<std::string> pub("dds://security_custom/lambda", pub_cfg);
    pub.wait_for_subscribers();

    pub.publish("Lambda ROT-13 encrypted");

    std::this_thread::sleep_for(100ms);
    VLOG_I("Lambda cipher: received ", received.load(), " messages");
  }

  // ---- Section 3: standalone Security class roundtrip ----
  // The Security class itself is constructible from a Config and exposes
  // encrypt()/decrypt() directly. Useful for testing custom callbacks
  // outside the pub/sub stack, or for offline encryption tooling.
  {
    VLOG_I("[3] Direct Security Class Usage");

    vlink::Security::Config sec_cfg;
    sec_cfg.encrypt_callback = xor_transform;
    sec_cfg.decrypt_callback = xor_transform;
    vlink::Security security(sec_cfg);

    vlink::Bytes plaintext = vlink::Bytes::from_string("Hello, Security!");
    vlink::Bytes ciphertext;
    const bool enc_ok = security.encrypt(plaintext, ciphertext);
    VLOG_I("Encrypt success: ", enc_ok, " plaintext_size: ", plaintext.size(), " cipher_size: ", ciphertext.size());

    vlink::Bytes recovered;
    const bool dec_ok = security.decrypt(ciphertext, recovered);
    VLOG_I("Decrypt success: ", dec_ok, " recovered: ", recovered.to_string());

    if (plaintext == recovered) {
      VLOG_I("Roundtrip verification: PASS");
    } else {
      VLOG_W("Roundtrip verification: FAIL");
    }
  }

  // ---- Section 4: callback signature reference ----
  {
    VLOG_I("[4] Callback Signature Reference");
    VLOG_I("  Signature: bool(const Bytes& in, Bytes& out)");
    VLOG_I("  Set both encrypt_callback and decrypt_callback to bypass AES-128-GCM.");
    VLOG_I("  Security::Config is one-shot at construction; rebuild to change settings.");
  }

  VLOG_I("Security custom cipher example complete.");
  return 0;
}
