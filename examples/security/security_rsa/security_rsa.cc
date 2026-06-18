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

#include <openssl/bio.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/rsa.h>
#include <vlink/base/logger.h>
#include <vlink/extension/security.h>
#include <vlink/vlink.h>

#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <thread>

using namespace std::chrono_literals;  // NOLINT(build/namespaces, google-build-using-namespace)

// ---------------------------------------------------------------------------
// security_rsa.cc
//
// RSA hybrid encryption (Security::Config 3rd mode). The sender generates
// a fresh AES-128 session key per message, encrypts the payload with
// AES-128-GCM, wraps the session key with the receiver's RSA-OAEP-SHA256
// public key, and concatenates [wrapped-key][AEAD-envelope]. Optional
// RSA-PSS-SHA256 signature over the envelope provides sender
// authentication (defeats key-substitution attacks).
//
// Wire layout:
//   [u16 wrapped_key_len][wrapped_key bytes][AES-GCM envelope][optional PSS sig]
//
// Key roles in Security::Config:
//   * Sender   sets  public_key_pem            (peer's pub, for wrap)
//                    advanced.signing_key_pem  (own priv, optional sign)
//   * Receiver sets  private_key_pem           (own priv, for unwrap)
//                    advanced.verify_key_pem   (peer's pub, optional verify)
//
// Failure modes:
//   * Wrong receiver private key  -> RSA unwrap fails, sample dropped.
//   * Tampered ciphertext         -> GCM tag fails, sample dropped.
//   * Wrong signing key           -> PSS verify fails, sample dropped.
//
// This file generates ephemeral key pairs at startup via OpenSSL; in
// production you'd load PEMs from disk or a secure store.
// ---------------------------------------------------------------------------

struct RsaKeyPair {
  std::string public_pem;
  std::string private_pem;
};

// Generate an in-memory RSA key pair using OpenSSL EVP_PKEY APIs. Returns
// the keys in PEM format, which is what Security::Config consumes. Returns
// an empty struct on any failure (subsequent .empty() check in main()).
static RsaKeyPair generate_rsa_keypair(int bits) {
  RsaKeyPair kp;

  EVP_PKEY_CTX* gctx = EVP_PKEY_CTX_new_id(EVP_PKEY_RSA, nullptr);

  if (gctx == nullptr) {
    VLOG_W("EVP_PKEY_CTX_new_id failed");
    return kp;
  }

  std::unique_ptr<EVP_PKEY_CTX, decltype(&EVP_PKEY_CTX_free)> gctx_guard(gctx, &EVP_PKEY_CTX_free);

  if (EVP_PKEY_keygen_init(gctx) <= 0 || EVP_PKEY_CTX_set_rsa_keygen_bits(gctx, bits) <= 0) {
    VLOG_W("RSA keygen init failed");
    return kp;
  }

  EVP_PKEY* pkey = nullptr;

  if (EVP_PKEY_keygen(gctx, &pkey) <= 0 || pkey == nullptr) {
    VLOG_W("RSA keygen failed");
    return kp;
  }

  std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)> pkey_guard(pkey, &EVP_PKEY_free);

  {
    BIO* bio = BIO_new(BIO_s_mem());

    if (bio == nullptr) {
      return RsaKeyPair{};
    }

    std::unique_ptr<BIO, decltype(&BIO_free)> bio_guard(bio, &BIO_free);

    if (PEM_write_bio_PUBKEY(bio, pkey) != 1) {
      return RsaKeyPair{};
    }

    char* buf = nullptr;
    const auto len = BIO_get_mem_data(bio, &buf);  // NOLINT(runtime/int, google-runtime-int)

    if (buf == nullptr || len <= 0) {
      return RsaKeyPair{};
    }

    kp.public_pem.assign(buf, static_cast<size_t>(len));
  }
  {
    BIO* bio = BIO_new(BIO_s_mem());

    if (bio == nullptr) {
      return RsaKeyPair{};
    }

    std::unique_ptr<BIO, decltype(&BIO_free)> bio_guard(bio, &BIO_free);

    if (PEM_write_bio_PrivateKey(bio, pkey, nullptr, nullptr, 0, nullptr, nullptr) != 1) {
      return RsaKeyPair{};
    }

    char* buf = nullptr;
    const auto len = BIO_get_mem_data(bio, &buf);  // NOLINT(runtime/int, google-runtime-int)

    if (buf == nullptr || len <= 0) {
      return RsaKeyPair{};
    }

    kp.private_pem.assign(buf, static_cast<size_t>(len));
  }

  return kp;
}
int main() {
  VLOG_I("Generating ephemeral RSA-2048 key pairs (receiver + signing)");
  const RsaKeyPair recv_kp = generate_rsa_keypair(2048);
  const RsaKeyPair sign_kp = generate_rsa_keypair(2048);

  if (recv_kp.public_pem.empty() || sign_kp.public_pem.empty()) {
    VLOG_W("RSA key generation failed -- aborting example.");
    return 1;
  }

  // ---- Section 1: RSA-OAEP hybrid (encryption only, no signing) ----
  // Receiver holds the private half; publisher only knows the public half.
  // No verify_key_pem on the subscriber -> no sender authentication, any
  // party with the receiver's public key could publish valid samples.
  {
    VLOG_I("[1] RSA-OAEP Hybrid (AES-128-GCM payload)");

    std::atomic<int> received{0};

    vlink::Security::Config sub_cfg;
    sub_cfg.private_key_pem = recv_kp.private_pem;

    vlink::SecuritySubscriber<std::string> sub("dds://security_rsa/hybrid", sub_cfg);
    // Listener runs on the DDS dispatch thread iff RSA unwrap + GCM tag
    // both succeed.
    sub.listen([&received](const std::string& msg) {
      received++;
      VLOG_I("[RSA-Hybrid] Received: ", msg);
    });

    vlink::Security::Config pub_cfg;
    pub_cfg.public_key_pem = recv_kp.public_pem;

    vlink::SecurityPublisher<std::string> pub("dds://security_rsa/hybrid", pub_cfg);
    pub.wait_for_subscribers();

    pub.publish("RSA-OAEP wraps an AES-128 session key");
    pub.publish("Each message uses a fresh session key");
    pub.publish("AES-128-GCM provides the authenticated payload");

    std::this_thread::sleep_for(100ms);
    VLOG_I("RSA hybrid: received ", received.load(), " messages");
  }

  // ---- Section 2: RSA hybrid + RSA-PSS signing (full authentication) ----
  // Publisher signs envelope with sign_kp.private; subscriber verifies with
  // sign_kp.public. Now an attacker who only knows the receiver's public
  // key still cannot forge samples -- the signature check would fail.
  {
    VLOG_I("[2] RSA Hybrid + RSA-PSS Signed (sender authenticated)");

    std::atomic<int> received{0};

    vlink::Security::Config sub_cfg;
    sub_cfg.private_key_pem = recv_kp.private_pem;
    sub_cfg.advanced.verify_key_pem = sign_kp.public_pem;

    vlink::SecuritySubscriber<std::string> sub("dds://security_rsa/signed", sub_cfg);
    sub.listen([&received](const std::string& msg) {
      received++;
      VLOG_I("[RSA-Signed] Received: ", msg);
    });

    vlink::Security::Config pub_cfg;
    pub_cfg.public_key_pem = recv_kp.public_pem;
    pub_cfg.advanced.signing_key_pem = sign_kp.private_pem;

    vlink::SecurityPublisher<std::string> pub("dds://security_rsa/signed", pub_cfg);
    pub.wait_for_subscribers();

    pub.publish("Signed with RSA-PSS-SHA256");
    pub.publish("Receiver verifies before decrypting");

    std::this_thread::sleep_for(100ms);
    VLOG_I("RSA signed: received ", received.load(), " messages");
  }

  // ---- Section 3: wrong signing key rejection ----
  // Publisher signs with an impostor key; subscriber's verify_key_pem
  // expects sign_kp.public, so RSA-PSS verification fails on every sample
  // and the listener never fires. Expected count is 0.
  {
    VLOG_I("[3] Wrong Signing Key -- Verification Failure");

    const RsaKeyPair impostor_kp = generate_rsa_keypair(2048);
    std::atomic<int> received{0};

    vlink::Security::Config sub_cfg;
    sub_cfg.private_key_pem = recv_kp.private_pem;
    sub_cfg.advanced.verify_key_pem = sign_kp.public_pem;

    vlink::SecuritySubscriber<std::string> sub("dds://security_rsa/impostor", sub_cfg);
    sub.listen([&received](const std::string& msg) {
      received++;
      VLOG_I("[Impostor] Received (unexpected): ", msg);
    });

    vlink::Security::Config pub_cfg;
    pub_cfg.public_key_pem = recv_kp.public_pem;
    pub_cfg.advanced.signing_key_pem = impostor_kp.private_pem;

    vlink::SecurityPublisher<std::string> pub("dds://security_rsa/impostor", pub_cfg);
    pub.wait_for_subscribers();

    pub.publish("This message should be rejected on verification");
    pub.publish("Impostor signing key does not match advanced.verify_key_pem");

    std::this_thread::sleep_for(200ms);
    VLOG_I("Impostor: received ", received.load(), " messages (expected 0 due to RSA-PSS failure)");
  }

  // ---- Section 4: standalone Security class roundtrip ----
  // Exercises the same RSA-hybrid + PSS-sign path outside pub/sub. Used by
  // offline tooling that needs to encrypt/decrypt bag samples.
  {
    VLOG_I("[4] Standalone vlink::Security with RSA Hybrid");

    vlink::Security::Config sender_cfg;
    sender_cfg.public_key_pem = recv_kp.public_pem;
    sender_cfg.advanced.signing_key_pem = sign_kp.private_pem;
    vlink::Security sender(sender_cfg);

    vlink::Security::Config receiver_cfg;
    receiver_cfg.private_key_pem = recv_kp.private_pem;
    receiver_cfg.advanced.verify_key_pem = sign_kp.public_pem;
    vlink::Security receiver(receiver_cfg);

    vlink::Bytes plaintext = vlink::Bytes::from_string("Standalone RSA hybrid payload");
    vlink::Bytes ciphertext;
    const bool enc_ok = sender.encrypt(plaintext, ciphertext);
    VLOG_I("Encrypt success: ", enc_ok, " plaintext_size: ", plaintext.size(), " cipher_size: ", ciphertext.size());

    vlink::Bytes recovered;
    const bool dec_ok = receiver.decrypt(ciphertext, recovered);
    VLOG_I("Decrypt success: ", dec_ok, " recovered: ", recovered.to_string());

    if (plaintext == recovered) {
      VLOG_I("Roundtrip verification: PASS");
    } else {
      VLOG_W("Roundtrip verification: FAIL");
    }
  }

  // ---- Section 5: configuration reference ----
  {
    VLOG_I("[5] RSA Configuration Reference");
    VLOG_I("  Sender:   public_key_pem (peer pub), advanced.signing_key_pem (own priv, optional)");
    VLOG_I("  Receiver: private_key_pem (own priv), advanced.verify_key_pem (peer pub, optional)");
    VLOG_I("  Wrap:      RSA-OAEP-SHA256 (per-message 16-byte AES session key)");
    VLOG_I("  Payload:   AES-128-GCM (envelope AAD, sequence nonce, 16-byte tag)");
    VLOG_I("  Signature: RSA-PSS-SHA256");
  }

  VLOG_I("Security RSA hybrid example complete.");
  return 0;
}
