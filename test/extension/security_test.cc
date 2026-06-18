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

#include "./extension/security.h"

#ifdef VLINK_TEST_SUPPORT_SECURITY

#include <doctest/doctest.h>
#include <openssl/bio.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/rsa.h>

#include <atomic>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <type_traits>
#include <vector>

#include "../common_test.h"
#include "./base/bytes.h"
#include "./publisher.h"
#include "./subscriber.h"

namespace {

inline Security::Config make_key_cfg(const std::string& key) {
  Security::Config cfg;
  cfg.key = key;
  return cfg;
}

inline Security::Config make_passphrase_cfg(const std::string& passphrase, const Bytes& salt,
                                            uint32_t iterations = 200000U) {
  Security::Config cfg;
  cfg.passphrase = passphrase;
  cfg.pbkdf2_salt = salt;
  cfg.pbkdf2_iterations = iterations;
  return cfg;
}

inline Security::Config make_callbacks_cfg(Security::Callback encrypt_cb, Security::Callback decrypt_cb) {
  Security::Config cfg;
  cfg.encrypt_callback = std::move(encrypt_cb);
  cfg.decrypt_callback = std::move(decrypt_cb);
  return cfg;
}

struct RsaKeyPair {
  std::string public_pem;
  std::string private_pem;
};

inline RsaKeyPair generate_rsa_keypair(int bits) {
  using EvpPkeyCtxPtr = std::unique_ptr<EVP_PKEY_CTX, decltype(&EVP_PKEY_CTX_free)>;
  using EvpPkeyPtr = std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)>;
  using BioPtr = std::unique_ptr<BIO, decltype(&BIO_free)>;

  RsaKeyPair kp;
  EvpPkeyCtxPtr gctx{EVP_PKEY_CTX_new_id(EVP_PKEY_RSA, nullptr), &EVP_PKEY_CTX_free};
  REQUIRE(gctx.get() != nullptr);
  REQUIRE(EVP_PKEY_keygen_init(gctx.get()) > 0);
  REQUIRE(EVP_PKEY_CTX_set_rsa_keygen_bits(gctx.get(), bits) > 0);

  EVP_PKEY* raw_pkey = nullptr;
  REQUIRE(EVP_PKEY_keygen(gctx.get(), &raw_pkey) > 0);
  EvpPkeyPtr pkey{raw_pkey, &EVP_PKEY_free};

  BioPtr pub_bio{BIO_new(BIO_s_mem()), &BIO_free};
  REQUIRE(pub_bio.get() != nullptr);
  REQUIRE(PEM_write_bio_PUBKEY(pub_bio.get(), pkey.get()) == 1);
  char* pub_buf = nullptr;
  const auto pub_len = BIO_get_mem_data(pub_bio.get(), &pub_buf);  // NOLINT(runtime/int,google-runtime-int)
  REQUIRE(pub_buf != nullptr);
  kp.public_pem.assign(pub_buf, static_cast<size_t>(pub_len));

  BioPtr prv_bio{BIO_new(BIO_s_mem()), &BIO_free};
  REQUIRE(prv_bio.get() != nullptr);
  REQUIRE(PEM_write_bio_PrivateKey(prv_bio.get(), pkey.get(), nullptr, nullptr, 0, nullptr, nullptr) == 1);
  char* prv_buf = nullptr;
  const auto prv_len = BIO_get_mem_data(prv_bio.get(), &prv_buf);  // NOLINT(runtime/int,google-runtime-int)
  REQUIRE(prv_buf != nullptr);
  kp.private_pem.assign(prv_buf, static_cast<size_t>(prv_len));

  return kp;
}

}  // namespace

TEST_SUITE("extension-Security") {
  TEST_CASE("custom xor round-trip succeeds") {
    auto xor_fn = [](const Bytes& in, Bytes& out) -> bool {
      out = Bytes::create(in.size());
      for (size_t i = 0; i < in.size(); ++i) {
        out[i] = static_cast<uint8_t>(in[i] ^ 0xAAu);
      }
      return true;
    };

    Security sec(make_callbacks_cfg(xor_fn, xor_fn));

    const std::string plain_str = "hello world test message";
    Bytes plain = Bytes::create(plain_str.size());
    std::memcpy(plain.data(), plain_str.data(), plain_str.size());

    Bytes cipher;
    REQUIRE(sec.encrypt(plain, cipher));
    CHECK_FALSE(cipher.empty());

    Bytes recovered;
    REQUIRE(sec.decrypt(cipher, recovered));
    REQUIRE_EQ(recovered.size(), plain.size());
    CHECK_EQ(std::memcmp(plain.data(), recovered.data(), plain.size()), 0);
  }

  TEST_CASE("failing custom callback propagates as encrypt/decrypt failure") {
    auto fail_fn = [](const Bytes&, Bytes&) -> bool { return false; };
    Security sec(make_callbacks_cfg(fail_fn, fail_fn));

    Bytes data = Bytes::create(16);
    data[0] = 0xFF;
    Bytes out;
    CHECK_FALSE(sec.encrypt(data, out));
    CHECK_FALSE(sec.decrypt(data, out));
  }

  TEST_CASE("custom callbacks are each invoked once per call") {
    int enc_calls = 0;
    int dec_calls = 0;
    Security sec(make_callbacks_cfg(
        [&enc_calls](const Bytes& in, Bytes& out) -> bool {
          ++enc_calls;
          out = in;
          return true;
        },
        [&dec_calls](const Bytes& in, Bytes& out) -> bool {
          ++dec_calls;
          out = in;
          return true;
        }));

    Bytes data = Bytes::create(8);
    Bytes out;
    REQUIRE(sec.encrypt(data, out));
    REQUIRE(sec.decrypt(data, out));
    CHECK_EQ(enc_calls, 1);
    CHECK_EQ(dec_calls, 1);
  }

  TEST_CASE("AES-GCM round-trip with matching key") {
    Security sender(make_key_cfg("test_key_seed"));
    Security receiver(make_key_cfg("test_key_seed"));

    const std::string plain_str = "AES-GCM authenticated payload";
    Bytes plain = Bytes::create(plain_str.size());
    std::memcpy(plain.data(), plain_str.data(), plain_str.size());

    Bytes cipher;
    REQUIRE(sender.encrypt(plain, cipher));
    CHECK_EQ(cipher.size(), plain.size() + 50U);

    Bytes recovered;
    REQUIRE(receiver.decrypt(cipher, recovered));
    REQUIRE_EQ(recovered.size(), plain.size());
    CHECK_EQ(std::memcmp(plain.data(), recovered.data(), plain.size()), 0);
  }

  TEST_CASE("AES-GCM 1-byte plaintext is the smallest valid payload") {
    Security sender(make_key_cfg("one_byte_seed"));
    Security receiver(make_key_cfg("one_byte_seed"));

    Bytes plain = Bytes::create(1U);
    plain.data()[0] = 0xA5U;

    Bytes cipher;
    REQUIRE(sender.encrypt(plain, cipher));
    CHECK_EQ(cipher.size(), 1U + 50U);

    Bytes recovered;
    REQUIRE(receiver.decrypt(cipher, recovered));
    REQUIRE_EQ(recovered.size(), 1U);
    CHECK_EQ(recovered.data()[0], 0xA5U);
  }

  TEST_CASE("AAD context mismatch causes authentication failure") {
    auto sender_cfg = make_key_cfg("aad_seed");
    sender_cfg.advanced.aad_context = "shm://secure/topic|demo.Msg|3";
    Security sender(sender_cfg);

    auto wrong_cfg = make_key_cfg("aad_seed");
    wrong_cfg.advanced.aad_context = "shm://other/topic|demo.Msg|3";
    Security wrong_receiver(wrong_cfg);

    auto right_cfg = make_key_cfg("aad_seed");
    right_cfg.advanced.aad_context = "shm://secure/topic|demo.Msg|3";
    Security right_receiver(right_cfg);

    Bytes plain = Bytes::create(24);
    std::memset(plain.data(), 0x31, 24);

    Bytes cipher;
    REQUIRE(sender.encrypt(plain, cipher));

    Bytes recovered;
    CHECK_FALSE(wrong_receiver.decrypt(cipher, recovered));
    REQUIRE(right_receiver.decrypt(cipher, recovered));
    REQUIRE_EQ(recovered.size(), plain.size());
    CHECK_EQ(std::memcmp(plain.data(), recovered.data(), plain.size()), 0);
  }

  TEST_CASE("replay protection rejects duplicate ciphertext") {
    Security sender(make_key_cfg("replay_seed"));
    Security receiver(make_key_cfg("replay_seed"));

    Bytes plain = Bytes::create(32);
    std::memset(plain.data(), 0x62, 32);

    Bytes cipher;
    REQUIRE(sender.encrypt(plain, cipher));

    Bytes recovered;
    REQUIRE(receiver.decrypt(cipher, recovered));
    CHECK_FALSE(receiver.decrypt(cipher, recovered));
  }

  TEST_CASE("ciphertext differs across calls due to unique nonce") {
    Security sec(make_key_cfg("same_seed"));

    const std::string plain_str = "deterministic plaintext payload";
    Bytes plain = Bytes::create(plain_str.size());
    std::memcpy(plain.data(), plain_str.data(), plain_str.size());

    Bytes c1;
    Bytes c2;
    REQUIRE(sec.encrypt(plain, c1));
    REQUIRE(sec.encrypt(plain, c2));

    REQUIRE_EQ(c1.size(), c2.size());
    CHECK_NE(std::memcmp(c1.data(), c2.data(), c1.size()), 0);
  }

  TEST_CASE("round-trip succeeds for a range of key seed lengths") {
    const std::vector<std::string> seeds = {
        "short",
        "exactly_16_bytes",
        "a much longer passphrase than aes block size",
    };

    for (const auto& seed : seeds) {
      Security sender(make_key_cfg(seed));
      Security receiver(make_key_cfg(seed));

      Bytes plain = Bytes::create(64);
      std::memset(plain.data(), 0x5A, 64);

      Bytes cipher;
      Bytes recovered;
      REQUIRE(sender.encrypt(plain, cipher));
      REQUIRE(receiver.decrypt(cipher, recovered));
      REQUIRE_EQ(recovered.size(), plain.size());
      CHECK_EQ(std::memcmp(plain.data(), recovered.data(), plain.size()), 0);
    }
  }

  TEST_CASE("decrypt with mismatched key fails authentication") {
    Security alice(make_key_cfg("alice_seed"));
    Security bob(make_key_cfg("bob_seed"));

    Bytes plain = Bytes::create(48);
    std::memset(plain.data(), 0x11, 48);

    Bytes cipher;
    REQUIRE(alice.encrypt(plain, cipher));

    Bytes recovered;
    CHECK_FALSE(bob.decrypt(cipher, recovered));
  }

  TEST_CASE("tampered ciphertext fails AES-GCM authentication") {
    Security sec(make_key_cfg("tamper_seed"));

    Bytes plain = Bytes::create(64);
    for (size_t i = 0; i < plain.size(); ++i) {
      plain.data()[i] = static_cast<uint8_t>(i);
    }

    Bytes cipher;
    REQUIRE(sec.encrypt(plain, cipher));
    REQUIRE_GT(cipher.size(), 0U);

    for (size_t pos : {size_t{0}, cipher.size() / 2U, cipher.size() - 1U}) {
      Bytes tampered = Bytes::create(cipher.size());
      std::memcpy(tampered.data(), cipher.data(), cipher.size());
      tampered.data()[pos] ^= 0x01U;

      Bytes recovered;
      CHECK_FALSE(sec.decrypt(tampered, recovered));
    }
  }

  TEST_CASE("truncated ciphertext fails decryption") {
    Security sec(make_key_cfg("trunc_seed"));

    Bytes plain = Bytes::create(32);
    std::memset(plain.data(), 0xAB, 32);

    Bytes cipher;
    REQUIRE(sec.encrypt(plain, cipher));

    Bytes truncated = Bytes::create(cipher.size() - 1U);
    std::memcpy(truncated.data(), cipher.data(), cipher.size() - 1U);

    Bytes recovered;
    CHECK_FALSE(sec.decrypt(truncated, recovered));
  }

  TEST_CASE("PBKDF2 round-trip with shared salt and passphrase") {
    Bytes salt = Bytes::create(16);
    std::memset(salt.data(), 0x42, 16);

    Security sender(make_passphrase_cfg("correct horse battery staple", salt));
    Security receiver(make_passphrase_cfg("correct horse battery staple", salt));

    Bytes plain = Bytes::create(40);
    std::memset(plain.data(), 0xC9, 40);

    Bytes cipher;
    REQUIRE(sender.encrypt(plain, cipher));

    Bytes recovered;
    REQUIRE(receiver.decrypt(cipher, recovered));
    REQUIRE_EQ(recovered.size(), plain.size());
    CHECK_EQ(std::memcmp(plain.data(), recovered.data(), plain.size()), 0);
  }

  TEST_CASE("PBKDF2 different salts produce different keys") {
    Bytes salt_a = Bytes::create(16);
    Bytes salt_b = Bytes::create(16);
    std::memset(salt_a.data(), 0x01, 16);
    std::memset(salt_b.data(), 0x02, 16);

    Security alice(make_passphrase_cfg("shared_pass", salt_a));
    Security bob(make_passphrase_cfg("shared_pass", salt_b));

    Bytes plain = Bytes::create(32);
    std::memset(plain.data(), 0xAA, 32);

    Bytes cipher;
    REQUIRE(alice.encrypt(plain, cipher));

    Bytes recovered;
    CHECK_FALSE(bob.decrypt(cipher, recovered));
  }

  TEST_CASE("PBKDF2 short salt is rejected") {
    Bytes short_salt = Bytes::create(8);
    std::memset(short_salt.data(), 0x33, 8);

    Security sec(make_passphrase_cfg("pass", short_salt));

    Bytes plain = Bytes::create(16);
    std::memset(plain.data(), 0x11, 16);

    Bytes cipher;
    CHECK_FALSE(sec.encrypt(plain, cipher));
  }

  TEST_CASE("PBKDF2 different iteration counts produce incompatible keys") {
    Bytes salt = Bytes::create(16);
    std::memset(salt.data(), 0x77, 16);

    Security a(make_passphrase_cfg("pw", salt, 1000U));
    Security b(make_passphrase_cfg("pw", salt, 2000U));

    Bytes plain = Bytes::create(16);
    std::memset(plain.data(), 0x88, 16);

    Bytes cipher;
    REQUIRE(a.encrypt(plain, cipher));

    Bytes recovered;
    CHECK_FALSE(b.decrypt(cipher, recovered));
  }

  TEST_CASE("default-constructed Security uses built-in slot and round-trips") {
    Security sender;
    Security receiver;

    Bytes plain = Bytes::create(16);
    std::memset(plain.data(), 0x55, 16);

    Bytes cipher;
    REQUIRE(sender.encrypt(plain, cipher));

    Bytes recovered;
    REQUIRE(receiver.decrypt(cipher, recovered));
    REQUIRE_EQ(recovered.size(), plain.size());
    CHECK_EQ(std::memcmp(recovered.data(), plain.data(), plain.size()), 0);

    Bytes garbage_recovered;
    CHECK_FALSE(receiver.decrypt(plain, garbage_recovered));
  }

  TEST_CASE("concurrent encrypt and decrypt is thread-safe") {
    Security sec(make_key_cfg("concurrent_seed"));

    static constexpr int kThreads = 8;
    static constexpr int kIters = 64;
    std::atomic<int> failures{0};
    std::vector<std::thread> workers;
    workers.reserve(kThreads);

    for (int t = 0; t < kThreads; ++t) {
      workers.emplace_back([&sec, &failures, t]() {
        for (int i = 0; i < kIters; ++i) {
          Bytes plain = Bytes::create(48U);
          for (size_t k = 0; k < plain.size(); ++k) {
            plain.data()[k] = static_cast<uint8_t>((t * 31 + i + static_cast<int>(k)) & 0xFFU);
          }

          Bytes cipher;
          if (!sec.encrypt(plain, cipher)) {
            ++failures;
            continue;
          }

          Bytes recovered;
          if (!sec.decrypt(cipher, recovered) || recovered.size() != plain.size() ||
              std::memcmp(recovered.data(), plain.data(), plain.size()) != 0) {
            ++failures;
          }
        }
      });
    }

    for (auto& w : workers) {
      w.join();
    }

    CHECK_EQ(failures.load(), 0);
  }

  TEST_CASE("RSA-OAEP hybrid round-trip") {
    const auto kp = generate_rsa_keypair(2048);

    Security::Config sender_cfg;
    sender_cfg.public_key_pem = kp.public_pem;
    Security sender(sender_cfg);

    Security::Config receiver_cfg;
    receiver_cfg.private_key_pem = kp.private_pem;
    Security receiver(receiver_cfg);

    const std::string plain_str = "RSA-OAEP + AES-128-GCM hybrid payload";
    Bytes plain = Bytes::create(plain_str.size());
    std::memcpy(plain.data(), plain_str.data(), plain_str.size());

    Bytes cipher;
    REQUIRE(sender.encrypt(plain, cipher));
    CHECK_GT(cipher.size(), plain.size() + 256U);

    Bytes recovered;
    REQUIRE(receiver.decrypt(cipher, recovered));
    REQUIRE_EQ(recovered.size(), plain.size());
    CHECK_EQ(std::memcmp(plain.data(), recovered.data(), plain.size()), 0);
  }

  TEST_CASE("RSA hybrid ciphertext is randomised across calls") {
    const auto kp = generate_rsa_keypair(2048);

    Security::Config cfg;
    cfg.public_key_pem = kp.public_pem;
    Security sender(cfg);

    Bytes plain = Bytes::create(64);
    std::memset(plain.data(), 0x77, 64);

    Bytes c1;
    Bytes c2;
    REQUIRE(sender.encrypt(plain, c1));
    REQUIRE(sender.encrypt(plain, c2));

    REQUIRE_EQ(c1.size(), c2.size());
    CHECK_NE(std::memcmp(c1.data(), c2.data(), c1.size()), 0);
  }

  TEST_CASE("RSA decrypt with wrong private key fails") {
    const auto kp1 = generate_rsa_keypair(2048);
    const auto kp2 = generate_rsa_keypair(2048);

    Security::Config sender_cfg;
    sender_cfg.public_key_pem = kp1.public_pem;
    Security sender(sender_cfg);

    Security::Config wrong_cfg;
    wrong_cfg.private_key_pem = kp2.private_pem;
    Security wrong(wrong_cfg);

    Bytes plain = Bytes::create(32);
    std::memset(plain.data(), 0x33, 32);

    Bytes cipher;
    REQUIRE(sender.encrypt(plain, cipher));

    Bytes recovered;
    CHECK_FALSE(wrong.decrypt(cipher, recovered));
  }

  TEST_CASE("tampered RSA hybrid ciphertext fails authentication") {
    const auto kp = generate_rsa_keypair(2048);

    Security::Config sender_cfg;
    sender_cfg.public_key_pem = kp.public_pem;
    Security sender(sender_cfg);

    Security::Config receiver_cfg;
    receiver_cfg.private_key_pem = kp.private_pem;
    Security receiver(receiver_cfg);

    Bytes plain = Bytes::create(80);
    std::memset(plain.data(), 0x42, 80);

    Bytes cipher;
    REQUIRE(sender.encrypt(plain, cipher));
    REQUIRE_GT(cipher.size(), 0U);

    const size_t body_offset = cipher.size() - 40U;
    Bytes tampered = Bytes::create(cipher.size());
    std::memcpy(tampered.data(), cipher.data(), cipher.size());
    tampered.data()[body_offset] ^= 0x80U;

    Bytes recovered;
    CHECK_FALSE(receiver.decrypt(tampered, recovered));
  }

  TEST_CASE("RSA 1024-bit key is rejected as too weak") {
    const auto kp = generate_rsa_keypair(1024);

    Security::Config cfg;
    cfg.public_key_pem = kp.public_pem;
    Security sec(cfg);

    Bytes plain = Bytes::create(16);
    std::memset(plain.data(), 0x01, 16);

    Bytes cipher;
    CHECK_FALSE(sec.encrypt(plain, cipher));
  }

  TEST_CASE("invalid PEM string is rejected without crash") {
    Security::Config cfg;
    cfg.public_key_pem = "not a valid pem";
    Security sec(cfg);

    Bytes plain = Bytes::create(16);
    std::memset(plain.data(), 0x03, 16);

    Bytes cipher;
    CHECK_FALSE(sec.encrypt(plain, cipher));
  }

  TEST_CASE("malformed PEM edge cases are all rejected cleanly") {
    const auto kp = generate_rsa_keypair(2048);

    SUBCASE("BEGIN line only without body or END") {
      Security::Config cfg;
      cfg.public_key_pem = "-----BEGIN PUBLIC KEY-----\n";
      Security sec(cfg);
      Bytes plain = Bytes::create(16);
      Bytes cipher;
      CHECK_FALSE(sec.encrypt(plain, cipher));
    }

    SUBCASE("body truncated before END marker") {
      std::string truncated = kp.public_pem;
      const auto end_pos = truncated.find("-----END");
      REQUIRE_NE(end_pos, std::string::npos);
      truncated.resize(end_pos / 2U);
      Security::Config cfg;
      cfg.public_key_pem = truncated;
      Security sec(cfg);
      Bytes plain = Bytes::create(16);
      Bytes cipher;
      CHECK_FALSE(sec.encrypt(plain, cipher));
    }

    SUBCASE("END marker removed") {
      std::string no_end = kp.public_pem;
      const auto end_pos = no_end.find("-----END");
      REQUIRE_NE(end_pos, std::string::npos);
      no_end.resize(end_pos);
      Security::Config cfg;
      cfg.public_key_pem = no_end;
      Security sec(cfg);
      Bytes plain = Bytes::create(16);
      Bytes cipher;
      CHECK_FALSE(sec.encrypt(plain, cipher));
    }

    SUBCASE("private PEM supplied in public_key_pem slot") {
      Security::Config cfg;
      cfg.public_key_pem = kp.private_pem;
      Security sec(cfg);
      Bytes plain = Bytes::create(16);
      Bytes cipher;
      CHECK_FALSE(sec.encrypt(plain, cipher));
    }

    SUBCASE("public PEM supplied in private_key_pem slot") {
      Security::Config cfg;
      cfg.private_key_pem = kp.public_pem;
      Security sec(cfg);
      Bytes payload = Bytes::create(64);
      std::memset(payload.data(), 0x10, 64);
      Bytes recovered;
      CHECK_FALSE(sec.decrypt(payload, recovered));
    }
  }

  TEST_CASE("RSA-PSS signed message verifies under matching verify key") {
    const auto recv_kp = generate_rsa_keypair(2048);
    const auto sign_kp = generate_rsa_keypair(2048);

    Security::Config sender_cfg;
    sender_cfg.public_key_pem = recv_kp.public_pem;
    sender_cfg.advanced.signing_key_pem = sign_kp.private_pem;
    Security sender(sender_cfg);

    Security::Config receiver_cfg;
    receiver_cfg.private_key_pem = recv_kp.private_pem;
    receiver_cfg.advanced.verify_key_pem = sign_kp.public_pem;
    Security receiver(receiver_cfg);

    Bytes plain = Bytes::create(72);
    std::memset(plain.data(), 0x5C, 72);

    Bytes cipher;
    REQUIRE(sender.encrypt(plain, cipher));

    Bytes recovered;
    REQUIRE(receiver.decrypt(cipher, recovered));
    REQUIRE_EQ(recovered.size(), plain.size());
    CHECK_EQ(std::memcmp(plain.data(), recovered.data(), plain.size()), 0);
  }

  TEST_CASE("RSA-PSS signature from wrong key causes verification failure") {
    const auto recv_kp = generate_rsa_keypair(2048);
    const auto real_sign_kp = generate_rsa_keypair(2048);
    const auto other_sign_kp = generate_rsa_keypair(2048);

    Security::Config sender_cfg;
    sender_cfg.public_key_pem = recv_kp.public_pem;
    sender_cfg.advanced.signing_key_pem = real_sign_kp.private_pem;
    Security sender(sender_cfg);

    Security::Config receiver_cfg;
    receiver_cfg.private_key_pem = recv_kp.private_pem;
    receiver_cfg.advanced.verify_key_pem = other_sign_kp.public_pem;
    Security receiver(receiver_cfg);

    Bytes plain = Bytes::create(32);
    std::memset(plain.data(), 0x7F, 32);

    Bytes cipher;
    REQUIRE(sender.encrypt(plain, cipher));

    Bytes recovered;
    CHECK_FALSE(receiver.decrypt(cipher, recovered));
  }

  TEST_CASE("unsigned message is rejected when verify key is configured") {
    const auto recv_kp = generate_rsa_keypair(2048);
    const auto sign_kp = generate_rsa_keypair(2048);

    Security::Config sender_cfg;
    sender_cfg.public_key_pem = recv_kp.public_pem;
    Security unsigned_sender(sender_cfg);

    Security::Config receiver_cfg;
    receiver_cfg.private_key_pem = recv_kp.private_pem;
    receiver_cfg.advanced.verify_key_pem = sign_kp.public_pem;
    Security receiver(receiver_cfg);

    Bytes plain = Bytes::create(16);
    std::memset(plain.data(), 0x09, 16);

    Bytes cipher;
    REQUIRE(unsigned_sender.encrypt(plain, cipher));

    Bytes recovered;
    CHECK_FALSE(receiver.decrypt(cipher, recovered));
  }

  TEST_CASE("signed message is accepted by receiver without verify key") {
    const auto recv_kp = generate_rsa_keypair(2048);
    const auto sign_kp = generate_rsa_keypair(2048);

    Security::Config sender_cfg;
    sender_cfg.public_key_pem = recv_kp.public_pem;
    sender_cfg.advanced.signing_key_pem = sign_kp.private_pem;
    Security sender(sender_cfg);

    Security::Config receiver_cfg;
    receiver_cfg.private_key_pem = recv_kp.private_pem;
    Security receiver(receiver_cfg);

    Bytes plain = Bytes::create(24);
    std::memset(plain.data(), 0xC3, 24);

    Bytes cipher;
    REQUIRE(sender.encrypt(plain, cipher));

    Bytes recovered;
    REQUIRE(receiver.decrypt(cipher, recovered));
    REQUIRE_EQ(recovered.size(), plain.size());
    CHECK_EQ(std::memcmp(plain.data(), recovered.data(), plain.size()), 0);
  }

  TEST_CASE("large payload via RSA hybrid round-trips correctly") {
    const auto kp = generate_rsa_keypair(2048);

    Security::Config sender_cfg;
    sender_cfg.public_key_pem = kp.public_pem;
    Security sender(sender_cfg);

    Security::Config receiver_cfg;
    receiver_cfg.private_key_pem = kp.private_pem;
    Security receiver(receiver_cfg);

    Bytes plain = Bytes::create(8192);
    for (size_t i = 0; i < plain.size(); ++i) {
      plain.data()[i] = static_cast<uint8_t>(i & 0xFFU);
    }

    Bytes cipher;
    REQUIRE(sender.encrypt(plain, cipher));

    Bytes recovered;
    REQUIRE(receiver.decrypt(cipher, recovered));
    REQUIRE_EQ(recovered.size(), plain.size());
    CHECK_EQ(std::memcmp(plain.data(), recovered.data(), plain.size()), 0);
  }

  TEST_CASE("custom callback overrides asymmetric path when both are present") {
    const auto kp = generate_rsa_keypair(2048);
    auto xor_fn = [](const Bytes& in, Bytes& out) -> bool {
      out = Bytes::create(in.size());
      for (size_t i = 0; i < in.size(); ++i) {
        out.data()[i] = static_cast<uint8_t>(in.data()[i] ^ 0x5AU);
      }
      return true;
    };

    Security::Config cfg;
    cfg.public_key_pem = kp.public_pem;
    cfg.private_key_pem = kp.private_pem;
    cfg.encrypt_callback = xor_fn;
    cfg.decrypt_callback = xor_fn;
    Security sec(cfg);

    Bytes plain = Bytes::create(20);
    std::memset(plain.data(), 0x99, 20);

    Bytes cipher;
    REQUIRE(sec.encrypt(plain, cipher));
    REQUIRE_EQ(cipher.size(), plain.size());

    Bytes recovered;
    REQUIRE(sec.decrypt(cipher, recovered));
    REQUIRE_EQ(recovered.size(), plain.size());
    CHECK_EQ(std::memcmp(plain.data(), recovered.data(), plain.size()), 0);
  }

  TEST_CASE("default-constructed Security is fully configured") {
    Security sec;
    CHECK(sec.is_configured());
    CHECK(sec.can_encrypt());
    CHECK(sec.can_decrypt());
  }

  TEST_CASE("symmetric key populates all capability slots") {
    Security sec(make_key_cfg("cap_key_seed"));
    CHECK(sec.is_configured());
    CHECK(sec.can_encrypt());
    CHECK(sec.can_decrypt());
  }

  TEST_CASE("public_key_pem enables encrypt but not decrypt") {
    const auto kp = generate_rsa_keypair(2048);
    Security::Config cfg;
    cfg.public_key_pem = kp.public_pem;
    Security sec(cfg);
    CHECK(sec.is_configured());
    CHECK(sec.can_encrypt());
    CHECK_FALSE(sec.can_decrypt());
  }

  TEST_CASE("private_key_pem enables decrypt but not encrypt") {
    const auto kp = generate_rsa_keypair(2048);
    Security::Config cfg;
    cfg.private_key_pem = kp.private_pem;
    Security sec(cfg);
    CHECK(sec.is_configured());
    CHECK_FALSE(sec.can_encrypt());
    CHECK(sec.can_decrypt());
  }

  TEST_CASE("matched callback pair enables both directions") {
    auto identity = [](const Bytes& in, Bytes& out) -> bool {
      out = in;
      return true;
    };
    Security sec(make_callbacks_cfg(identity, identity));
    CHECK(sec.is_configured());
    CHECK(sec.can_encrypt());
    CHECK(sec.can_decrypt());
  }

  TEST_CASE("lone encrypt callback without decrypt partner is ignored") {
    int enc_calls = 0;
    Security::Config cfg;
    cfg.encrypt_callback = [&enc_calls](const Bytes& in, Bytes& out) -> bool {
      ++enc_calls;
      out = Bytes::create(in.size());
      for (size_t i = 0; i < in.size(); ++i) {
        out.data()[i] = static_cast<uint8_t>(in.data()[i] ^ 0x33U);
      }
      return true;
    };
    Security sec(cfg);

    Bytes plain = Bytes::create(16);
    std::memset(plain.data(), 0x44, 16);
    Bytes cipher;
    CHECK_FALSE(sec.encrypt(plain, cipher));
    CHECK_EQ(enc_calls, 0);
  }

  TEST_CASE("PBKDF2 iterations=0 is rejected") {
    Bytes salt = Bytes::create(16);
    std::memset(salt.data(), 0x55, 16);

    Security::Config cfg;
    cfg.passphrase = "x";
    cfg.pbkdf2_salt = salt;
    cfg.pbkdf2_iterations = 0U;
    Security sec(cfg);

    Bytes plain = Bytes::create(16);
    std::memset(plain.data(), 0x11, 16);
    Bytes cipher;
    CHECK_FALSE(sec.encrypt(plain, cipher));
  }

  TEST_CASE("oversized aad_context disables the instance") {
    Security::Config cfg;
    cfg.key = "oversized_aad_seed";
    cfg.advanced.aad_context.assign(65536U, 'a');
    Security sec(cfg);
    CHECK_FALSE(sec.is_configured());
    CHECK_FALSE(sec.can_encrypt());
    CHECK_FALSE(sec.can_decrypt());
  }

  TEST_CASE("pbkdf2_salt mutation after construction does not affect derived key") {
    Bytes salt = Bytes::create(16);
    std::memset(salt.data(), 0x42, 16);

    Security::Config cfg;
    cfg.passphrase = "stable_passphrase";
    cfg.pbkdf2_salt = salt;
    cfg.pbkdf2_iterations = 1000U;
    Security sec(cfg);

    Bytes plain = Bytes::create(32);
    std::memset(plain.data(), 0xC0, 32);

    Bytes cipher_before;
    REQUIRE(sec.encrypt(plain, cipher_before));

    std::memset(salt.data(), 0xFF, 16);

    Bytes cipher_after;
    REQUIRE(sec.encrypt(plain, cipher_after));

    Bytes recovered;
    REQUIRE(sec.decrypt(cipher_before, recovered));
    REQUIRE_EQ(recovered.size(), plain.size());
    CHECK_EQ(std::memcmp(plain.data(), recovered.data(), plain.size()), 0);

    Bytes recovered2;
    REQUIRE(sec.decrypt(cipher_after, recovered2));
    REQUIRE_EQ(recovered2.size(), plain.size());
    CHECK_EQ(std::memcmp(plain.data(), recovered2.data(), plain.size()), 0);
  }

  TEST_CASE("cross-instance key mismatch always fails") {
    Security first(make_key_cfg("first_key_seed"));
    Security second(make_key_cfg("second_key_seed"));

    const std::string plain_str = "different keys must not interop";
    Bytes plain = Bytes::create(plain_str.size());
    std::memcpy(plain.data(), plain_str.data(), plain_str.size());

    Bytes cipher;
    REQUIRE(second.encrypt(plain, cipher));

    Bytes recovered;
    CHECK_FALSE(first.decrypt(cipher, recovered));
  }
}

#endif  // VLINK_TEST_SUPPORT_SECURITY

// NOLINTEND
