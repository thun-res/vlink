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
#include <openssl/ec.h>
#include <openssl/evp.h>
#include <openssl/obj_mac.h>
#include <openssl/pem.h>
#include <openssl/rsa.h>

#include <atomic>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <set>
#include <string>
#include <system_error>
#include <thread>
#include <type_traits>
#include <utility>
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

inline void move_assign_security(Security& lhs, Security& rhs) { lhs = std::move(rhs); }

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

inline RsaKeyPair generate_ec_keypair() {
  using EvpPkeyCtxPtr = std::unique_ptr<EVP_PKEY_CTX, decltype(&EVP_PKEY_CTX_free)>;
  using EvpPkeyPtr = std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)>;
  using BioPtr = std::unique_ptr<BIO, decltype(&BIO_free)>;

  RsaKeyPair kp;
  EvpPkeyCtxPtr param_ctx{EVP_PKEY_CTX_new_id(EVP_PKEY_EC, nullptr), &EVP_PKEY_CTX_free};
  REQUIRE(param_ctx.get() != nullptr);
  REQUIRE(EVP_PKEY_paramgen_init(param_ctx.get()) > 0);
  REQUIRE(EVP_PKEY_CTX_set_ec_paramgen_curve_nid(param_ctx.get(), NID_X9_62_prime256v1) > 0);

  EVP_PKEY* raw_params = nullptr;
  REQUIRE(EVP_PKEY_paramgen(param_ctx.get(), &raw_params) > 0);
  EvpPkeyPtr params{raw_params, &EVP_PKEY_free};

  EvpPkeyCtxPtr key_ctx{EVP_PKEY_CTX_new(params.get(), nullptr), &EVP_PKEY_CTX_free};
  REQUIRE(key_ctx.get() != nullptr);
  REQUIRE(EVP_PKEY_keygen_init(key_ctx.get()) > 0);

  EVP_PKEY* raw_pkey = nullptr;
  REQUIRE(EVP_PKEY_keygen(key_ctx.get(), &raw_pkey) > 0);
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

class ScopedSecurityTmpFile {
 public:
  ScopedSecurityTmpFile(const std::string& name, const std::string& content) {
    path_ = std::filesystem::path(vlink::Utils::get_tmp_dir()) / "vlink-security-tests" /
            (name + "_" + vlink::Utils::get_pid_str() + ".pem");
    std::filesystem::create_directories(path_.parent_path());

    std::ofstream file(path_, std::ios::binary | std::ios::trunc);
    file << content;
  }

  ScopedSecurityTmpFile(const ScopedSecurityTmpFile&) = delete;
  ScopedSecurityTmpFile& operator=(const ScopedSecurityTmpFile&) = delete;

  ScopedSecurityTmpFile(ScopedSecurityTmpFile&& other) noexcept : path_(std::move(other.path_)) { other.path_.clear(); }

  ScopedSecurityTmpFile& operator=(ScopedSecurityTmpFile&& other) noexcept {
    if (this != &other) {
      remove();
      path_ = std::move(other.path_);
      other.path_.clear();
    }
    return *this;
  }

  ~ScopedSecurityTmpFile() { remove(); }

  std::string string() const { return path_.string(); }

 private:
  void remove() {
    if (!path_.empty()) {
      std::error_code ec;
      std::filesystem::remove(path_, ec);
      std::filesystem::remove(path_.parent_path(), ec);
    }
  }

  std::filesystem::path path_;
};

inline ScopedSecurityTmpFile make_security_tmp_file(const std::string& name, const std::string& content) {
  return ScopedSecurityTmpFile(name, content);
}

inline Bytes clone_bytes(const Bytes& source) {
  Bytes copy = Bytes::create(source.size());
  if (!source.empty() && source.data() != nullptr && copy.data() != nullptr) {
    std::memcpy(copy.data(), source.data(), source.size());
  }
  return copy;
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

  TEST_CASE("replay window can be disabled explicitly") {
    auto cfg = make_key_cfg("replay_disabled_seed");
    cfg.advanced.replay_window = 0U;
    Security sender(cfg);
    Security receiver(cfg);

    Bytes plain = Bytes::create(24);
    std::memset(plain.data(), 0x63, plain.size());

    Bytes cipher;
    REQUIRE(sender.encrypt(plain, cipher));

    Bytes recovered;
    REQUIRE(receiver.decrypt(cipher, recovered));
    REQUIRE_EQ(recovered.size(), plain.size());
    CHECK_EQ(std::memcmp(plain.data(), recovered.data(), plain.size()), 0);

    Bytes recovered_again;
    REQUIRE(receiver.decrypt(cipher, recovered_again));
    REQUIRE_EQ(recovered_again.size(), plain.size());
    CHECK_EQ(std::memcmp(plain.data(), recovered_again.data(), plain.size()), 0);
  }

  TEST_CASE("replay peer tracking is bounded without limiting disabled replay mode") {
    static constexpr size_t kExpectedReplayPeerLimit = 1024U;

    for (const uint32_t replay_window : {64U, 0U}) {
      auto cfg = make_key_cfg("replay_peer_limit_seed");
      cfg.advanced.replay_window = replay_window;
      Security receiver(cfg);
      std::set<uint64_t> sender_ids;
      size_t attempts = 0;

      while (sender_ids.size() <= kExpectedReplayPeerLimit && attempts < kExpectedReplayPeerLimit * 2U) {
        ++attempts;
        Security sender(cfg);
        Bytes plain = Bytes::create(1);
        plain[0] = 0x65U;

        Bytes cipher;
        REQUIRE(sender.encrypt(plain, cipher));
        REQUIRE_GE(cipher.size(), 14U);

        uint64_t sender_id = 0;
        for (size_t index = 0; index < sizeof(sender_id); ++index) {
          sender_id |= static_cast<uint64_t>(cipher[6U + index]) << (index * 8U);
        }

        if (!sender_ids.insert(sender_id).second) {
          continue;
        }

        Bytes recovered;
        const bool accepted = receiver.decrypt(cipher, recovered);

        if (replay_window == 0U || sender_ids.size() <= kExpectedReplayPeerLimit) {
          REQUIRE(accepted);
          CHECK_EQ(recovered.size(), 1U);
          CHECK_EQ(recovered[0], 0x65U);
        } else {
          CHECK_FALSE(accepted);
          CHECK(recovered.empty());
        }
      }

      REQUIRE_EQ(sender_ids.size(), kExpectedReplayPeerLimit + 1U);
    }
  }

  TEST_CASE("replay window accepts out of order ciphertext inside the configured range") {
    Security sender(make_key_cfg("replay_window_seed"));
    Security receiver(make_key_cfg("replay_window_seed"));

    Bytes plain = Bytes::create(20);
    std::memset(plain.data(), 0x64, plain.size());

    Bytes first;
    Bytes second;
    Bytes third;
    REQUIRE(sender.encrypt(plain, first));
    REQUIRE(sender.encrypt(plain, second));
    REQUIRE(sender.encrypt(plain, third));

    Bytes recovered;
    REQUIRE(receiver.decrypt(third, recovered));
    CHECK(receiver.decrypt(second, recovered));
    CHECK(receiver.decrypt(first, recovered));
  }

  TEST_CASE("small replay window rejects ciphertext older than the configured range") {
    auto cfg = make_key_cfg("small_replay_window_seed");
    cfg.advanced.replay_window = 1U;
    Security sender(cfg);
    Security receiver(cfg);

    Bytes plain = Bytes::create(20);
    std::memset(plain.data(), 0x65, plain.size());

    Bytes first;
    Bytes second;
    Bytes third;
    REQUIRE(sender.encrypt(plain, first));
    REQUIRE(sender.encrypt(plain, second));
    REQUIRE(sender.encrypt(plain, third));

    Bytes recovered;
    REQUIRE(receiver.decrypt(third, recovered));
    CHECK_FALSE(receiver.decrypt(first, recovered));
  }

  TEST_CASE("large sequence gaps reset replay tracking without breaking decryption") {
    auto cfg = make_key_cfg("large_replay_gap_seed");
    cfg.advanced.replay_window = 1U;
    Security sender(cfg);
    Security receiver(cfg);

    Bytes plain = Bytes::create(20);
    std::memset(plain.data(), 0x66, plain.size());

    Bytes latest;
    for (size_t i = 0; i < 70U; ++i) {
      REQUIRE(sender.encrypt(plain, latest));
    }

    Bytes recovered;
    REQUIRE(receiver.decrypt(latest, recovered));
    REQUIRE_EQ(recovered.size(), plain.size());
    CHECK_EQ(std::memcmp(plain.data(), recovered.data(), plain.size()), 0);
  }

  TEST_CASE("oversized replay window is clamped while preserving round trip") {
    auto cfg = make_key_cfg("clamped_replay_window_seed");
    cfg.advanced.replay_window = std::numeric_limits<uint32_t>::max();
    Security sender(cfg);
    Security receiver(cfg);

    Bytes plain = Bytes::create(16);
    std::memset(plain.data(), 0x6F, plain.size());

    Bytes cipher;
    REQUIRE(sender.encrypt(plain, cipher));

    Bytes recovered;
    REQUIRE(receiver.decrypt(cipher, recovered));
    REQUIRE_EQ(recovered.size(), plain.size());
    CHECK_EQ(std::memcmp(plain.data(), recovered.data(), plain.size()), 0);
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

  TEST_CASE("empty input is rejected before any built in operation") {
    Security sec(make_key_cfg("empty_input_seed"));

    Bytes out;
    CHECK_FALSE(sec.encrypt(Bytes{}, out));
    CHECK(out.empty());
    CHECK_FALSE(sec.decrypt(Bytes{}, out));
    CHECK(out.empty());
  }

  TEST_CASE("oversized logical input is rejected before reading payload memory") {
    Security sec(make_key_cfg("oversized_input_seed"));

    uint8_t one_byte = 0x5AU;
    Bytes huge = Bytes::shallow_copy(&one_byte, static_cast<size_t>(std::numeric_limits<int>::max()) + 1U);

    Bytes out;
    CHECK_FALSE(sec.encrypt(huge, out));
    CHECK(out.empty());
    CHECK_FALSE(sec.decrypt(huge, out));
    CHECK(out.empty());
  }

  TEST_CASE("malformed envelope header fields are rejected before authentication") {
    Security sender(make_key_cfg("malformed_header_seed"));
    Security receiver(make_key_cfg("malformed_header_seed"));

    Bytes plain = Bytes::create(16);
    std::memset(plain.data(), 0x67, plain.size());

    Bytes cipher;
    REQUIRE(sender.encrypt(plain, cipher));
    REQUIRE_GT(cipher.size(), 34U);

    auto reject = [&receiver](const Bytes& payload) {
      Bytes recovered;
      CHECK_FALSE(receiver.decrypt(payload, recovered));
      CHECK(recovered.empty());
    };

    Bytes short_header = Bytes::create(33U);
    std::memcpy(short_header.data(), cipher.data(), short_header.size());
    reject(short_header);

    Bytes bad_mode = clone_bytes(cipher);
    bad_mode.data()[3] = 0x7FU;
    reject(bad_mode);

    Bytes bad_magic = clone_bytes(cipher);
    bad_magic.data()[0] ^= 0x01U;
    reject(bad_magic);

    Bytes bad_version = clone_bytes(cipher);
    bad_version.data()[2] = 0xFFU;
    reject(bad_version);

    Bytes bad_flags = clone_bytes(cipher);
    bad_flags.data()[4] = 0x01U;
    reject(bad_flags);

    Bytes zero_sender = clone_bytes(cipher);
    std::memset(zero_sender.data() + 6U, 0, 8U);
    reject(zero_sender);
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

  TEST_CASE("PBKDF2 passphrase takes precedence over raw key when both are configured") {
    Bytes salt = Bytes::create(16);
    std::memset(salt.data(), 0x78, salt.size());

    auto sender_cfg = make_passphrase_cfg("priority_passphrase", salt, 1000U);
    sender_cfg.key = "ignored_raw_key";
    Security sender(sender_cfg);

    Security receiver(make_passphrase_cfg("priority_passphrase", salt, 1000U));
    Security raw_key_receiver(make_key_cfg("ignored_raw_key"));

    Bytes plain = Bytes::create(24);
    std::memset(plain.data(), 0x89, plain.size());

    Bytes cipher;
    REQUIRE(sender.encrypt(plain, cipher));

    Bytes recovered;
    REQUIRE(receiver.decrypt(cipher, recovered));
    REQUIRE_EQ(recovered.size(), plain.size());
    CHECK_EQ(std::memcmp(plain.data(), recovered.data(), plain.size()), 0);

    Bytes wrong;
    CHECK_FALSE(raw_key_receiver.decrypt(cipher, wrong));
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

  TEST_CASE("RSA-OAEP hybrid binds non-empty aad context") {
    const auto kp = generate_rsa_keypair(2048);

    Security::Config sender_cfg;
    sender_cfg.public_key_pem = kp.public_pem;
    sender_cfg.advanced.aad_context = "rsa://secure/context";
    Security sender(sender_cfg);

    Security::Config receiver_cfg;
    receiver_cfg.private_key_pem = kp.private_pem;
    receiver_cfg.advanced.aad_context = "rsa://secure/context";
    Security receiver(receiver_cfg);

    Security::Config wrong_cfg;
    wrong_cfg.private_key_pem = kp.private_pem;
    wrong_cfg.advanced.aad_context = "rsa://wrong/context";
    Security wrong_receiver(wrong_cfg);

    Bytes plain = Bytes::create(32);
    std::memset(plain.data(), 0x8A, plain.size());

    Bytes cipher;
    REQUIRE(sender.encrypt(plain, cipher));

    Bytes wrong_out;
    CHECK_FALSE(wrong_receiver.decrypt(cipher, wrong_out));

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

  TEST_CASE("asymmetric envelope is rejected when no private key is installed") {
    const auto kp = generate_rsa_keypair(2048);

    Security::Config sender_cfg;
    sender_cfg.public_key_pem = kp.public_pem;
    Security sender(sender_cfg);
    Security receiver;

    Bytes plain = Bytes::create(32);
    std::memset(plain.data(), 0x68, plain.size());

    Bytes cipher;
    REQUIRE(sender.encrypt(plain, cipher));

    Bytes recovered;
    CHECK_FALSE(receiver.decrypt(cipher, recovered));
  }

  TEST_CASE("malformed asymmetric envelope metadata is rejected before unwrap") {
    const auto kp = generate_rsa_keypair(2048);

    Security::Config sender_cfg;
    sender_cfg.public_key_pem = kp.public_pem;
    Security sender(sender_cfg);

    Security::Config receiver_cfg;
    receiver_cfg.private_key_pem = kp.private_pem;
    Security receiver(receiver_cfg);

    Bytes plain = Bytes::create(32);
    std::memset(plain.data(), 0x6B, plain.size());

    Bytes cipher;
    REQUIRE(sender.encrypt(plain, cipher));
    REQUIRE_GT(cipher.size(), 34U + 4U + 16U);

    auto reject = [&receiver](const Bytes& payload) {
      Bytes recovered;
      CHECK_FALSE(receiver.decrypt(payload, recovered));
      CHECK(recovered.empty());
    };

    Bytes too_short = Bytes::create(34U + 4U + 16U);
    std::memcpy(too_short.data(), cipher.data(), too_short.size());
    reject(too_short);

    Bytes zero_wrap = clone_bytes(cipher);
    zero_wrap.data()[34] = 0U;
    zero_wrap.data()[35] = 0U;
    reject(zero_wrap);

    Bytes oversized_wrap = clone_bytes(cipher);
    oversized_wrap.data()[34] = 0xFFU;
    oversized_wrap.data()[35] = 0xFFU;
    reject(oversized_wrap);

    Bytes oversized_sig = clone_bytes(cipher);
    oversized_sig.data()[36] = 0xFFU;
    oversized_sig.data()[37] = 0xFFU;
    reject(oversized_sig);
  }

  TEST_CASE("tampered RSA-PSS signature field is rejected before decrypting payload") {
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

    Bytes plain = Bytes::create(40);
    std::memset(plain.data(), 0x70, plain.size());

    Bytes cipher;
    REQUIRE(sender.encrypt(plain, cipher));
    REQUIRE_GT(cipher.size(), 34U + 4U + 256U);

    const uint16_t wrap_len =
        static_cast<uint16_t>(cipher.data()[34] | (static_cast<uint16_t>(cipher.data()[35]) << 8U));
    const uint16_t sig_len =
        static_cast<uint16_t>(cipher.data()[36] | (static_cast<uint16_t>(cipher.data()[37]) << 8U));
    REQUIRE_GT(wrap_len, 0U);
    REQUIRE_GT(sig_len, 0U);

    Bytes tampered = clone_bytes(cipher);
    tampered.data()[34U + 4U + wrap_len] ^= 0x01U;

    Bytes recovered;
    CHECK_FALSE(receiver.decrypt(tampered, recovered));
    CHECK(recovered.empty());
  }

  TEST_CASE("asymmetric replay protection rejects duplicate ciphertext") {
    const auto kp = generate_rsa_keypair(2048);

    Security::Config sender_cfg;
    sender_cfg.public_key_pem = kp.public_pem;
    Security sender(sender_cfg);

    Security::Config receiver_cfg;
    receiver_cfg.private_key_pem = kp.private_pem;
    Security receiver(receiver_cfg);

    Bytes plain = Bytes::create(24);
    std::memset(plain.data(), 0x6C, plain.size());

    Bytes cipher;
    REQUIRE(sender.encrypt(plain, cipher));

    Bytes recovered;
    REQUIRE(receiver.decrypt(cipher, recovered));
    CHECK_FALSE(receiver.decrypt(cipher, recovered));
  }

  TEST_CASE("symmetric envelope is rejected when only a private key is installed") {
    const auto kp = generate_rsa_keypair(2048);

    Security sender(make_key_cfg("symmetric_without_slot_seed"));

    Security::Config receiver_cfg;
    receiver_cfg.private_key_pem = kp.private_pem;
    Security receiver(receiver_cfg);

    Bytes plain = Bytes::create(32);
    std::memset(plain.data(), 0x69, plain.size());

    Bytes cipher;
    REQUIRE(sender.encrypt(plain, cipher));

    Bytes recovered;
    CHECK_FALSE(receiver.decrypt(cipher, recovered));
  }

  TEST_CASE("symmetric envelope is rejected when only a public key is installed") {
    const auto kp = generate_rsa_keypair(2048);

    Security sender(make_key_cfg("symmetric_without_public_slot_seed"));

    Security::Config receiver_cfg;
    receiver_cfg.public_key_pem = kp.public_pem;
    Security receiver(receiver_cfg);

    Bytes plain = Bytes::create(32);
    std::memset(plain.data(), 0x6A, plain.size());

    Bytes cipher;
    REQUIRE(sender.encrypt(plain, cipher));

    Bytes recovered;
    CHECK_FALSE(receiver.decrypt(cipher, recovered));
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

  TEST_CASE("invalid signing and verify keys are ignored after logging") {
    const auto recv_kp = generate_rsa_keypair(2048);

    Security::Config sender_cfg;
    sender_cfg.public_key_pem = recv_kp.public_pem;
    sender_cfg.advanced.signing_key_pem = "not a private pem";
    Security sender(sender_cfg);
    CHECK(sender.can_encrypt());

    Security::Config receiver_cfg;
    receiver_cfg.private_key_pem = recv_kp.private_pem;
    receiver_cfg.advanced.verify_key_pem = "not a public pem";
    Security receiver(receiver_cfg);
    CHECK(receiver.can_decrypt());

    Bytes plain = Bytes::create(28);
    std::memset(plain.data(), 0x6E, plain.size());

    Bytes cipher;
    REQUIRE(sender.encrypt(plain, cipher));

    Bytes recovered;
    REQUIRE(receiver.decrypt(cipher, recovered));
    REQUIRE_EQ(recovered.size(), plain.size());
    CHECK_EQ(std::memcmp(plain.data(), recovered.data(), plain.size()), 0);
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

  TEST_CASE("key path helpers load pem files and support asymmetric round trip") {
    const auto kp = generate_rsa_keypair(2048);
    const auto public_path = make_security_tmp_file("public", kp.public_pem);
    const auto private_path = make_security_tmp_file("private", kp.private_pem);

    auto sender_cfg = Security::from_public_key_path(public_path.string());
    auto receiver_cfg = Security::from_private_key_path(private_path.string());
    auto both_cfg = Security::from_key_paths(public_path.string(), private_path.string());

    CHECK_EQ(sender_cfg.public_key_pem, kp.public_pem);
    CHECK(sender_cfg.private_key_pem.empty());
    CHECK_EQ(receiver_cfg.private_key_pem, kp.private_pem);
    CHECK(receiver_cfg.public_key_pem.empty());
    CHECK_EQ(both_cfg.public_key_pem, kp.public_pem);
    CHECK_EQ(both_cfg.private_key_pem, kp.private_pem);

    Security sender(std::move(sender_cfg));
    Security receiver(std::move(receiver_cfg));

    Bytes plain = Bytes::create(48);
    std::memset(plain.data(), 0x6D, plain.size());

    Bytes cipher;
    REQUIRE(sender.encrypt(plain, cipher));

    Bytes recovered;
    REQUIRE(receiver.decrypt(cipher, recovered));
    REQUIRE_EQ(recovered.size(), plain.size());
    CHECK_EQ(std::memcmp(plain.data(), recovered.data(), plain.size()), 0);
  }

  TEST_CASE("key path helpers leave only missing fields empty") {
    const auto kp = generate_rsa_keypair(2048);
    const auto public_path = make_security_tmp_file("public_missing_pair", kp.public_pem);
    const std::filesystem::path missing_path = std::filesystem::path(Utils::get_tmp_dir()) / "vlink-security-tests" /
                                               ("missing_" + Utils::get_pid_str() + ".pem");

    auto public_only = Security::from_key_paths(public_path.string(), missing_path.string());
    CHECK_EQ(public_only.public_key_pem, kp.public_pem);
    CHECK(public_only.private_key_pem.empty());

    auto missing_public = Security::from_key_paths(missing_path.string(), public_path.string());
    CHECK(missing_public.public_key_pem.empty());
    CHECK_FALSE(missing_public.private_key_pem.empty());

    auto missing_private = Security::from_private_key_path(missing_path.string());
    auto missing_pub = Security::from_public_key_path(missing_path.string());
    CHECK(missing_private.private_key_pem.empty());
    CHECK(missing_pub.public_key_pem.empty());
  }

  TEST_CASE("move construction and assignment preserve usable security state") {
    Security original(make_key_cfg("move_seed"));
    Security moved(std::move(original));

    Bytes plain = Bytes::create(24);
    std::memset(plain.data(), 0x24, plain.size());

    Bytes cipher;
    REQUIRE(moved.encrypt(plain, cipher));

    Security assigned(make_key_cfg("other_seed"));
    assigned = std::move(moved);

    Bytes recovered;
    REQUIRE(assigned.decrypt(cipher, recovered));
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

    Bytes plain = Bytes::create(16);
    std::memset(plain.data(), 0x35, plain.size());
    Bytes cipher;
    CHECK_FALSE(sec.encrypt(plain, cipher));
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

  TEST_CASE("lone decrypt callback without encrypt partner is ignored") {
    int dec_calls = 0;
    Security::Config cfg;
    cfg.decrypt_callback = [&dec_calls](const Bytes& in, Bytes& out) -> bool {
      ++dec_calls;
      out = in;
      return true;
    };
    Security sec(cfg);

    CHECK_FALSE(sec.is_configured());
    CHECK_FALSE(sec.can_encrypt());
    CHECK_FALSE(sec.can_decrypt());

    Bytes payload = Bytes::create(16);
    std::memset(payload.data(), 0x6A, payload.size());
    Bytes recovered;
    CHECK_FALSE(sec.decrypt(payload, recovered));
    CHECK_EQ(dec_calls, 0);
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

  TEST_CASE("PBKDF2 iterations above INT_MAX are rejected before derivation") {
    Bytes salt = Bytes::create(16);
    std::memset(salt.data(), 0x56, salt.size());

    Security::Config cfg;
    cfg.passphrase = "iteration_overflow";
    cfg.pbkdf2_salt = salt;
    cfg.pbkdf2_iterations = static_cast<uint32_t>(std::numeric_limits<int>::max()) + 1U;
    Security sec(cfg);

    CHECK_FALSE(sec.is_configured());
    CHECK_FALSE(sec.can_encrypt());
    CHECK_FALSE(sec.can_decrypt());

    Bytes plain = Bytes::create(16);
    std::memset(plain.data(), 0x12, plain.size());
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

    Bytes data = Bytes::create(16);
    std::memset(data.data(), 0x72, data.size());
    Bytes out;
    CHECK_FALSE(sec.encrypt(data, out));
    CHECK_FALSE(sec.decrypt(data, out));
  }

  TEST_CASE("callback pair remains usable when built-in aad_context is oversized") {
    auto identity = [](const Bytes& in, Bytes& out) -> bool {
      out = clone_bytes(in);
      return true;
    };

    Security::Config cfg;
    cfg.advanced.aad_context.assign(65536U, 'b');
    cfg.encrypt_callback = identity;
    cfg.decrypt_callback = identity;
    Security sec(cfg);

    CHECK(sec.is_configured());
    CHECK(sec.can_encrypt());
    CHECK(sec.can_decrypt());

    Bytes plain = Bytes::create(12);
    std::memset(plain.data(), 0x8B, plain.size());

    Bytes cipher;
    REQUIRE(sec.encrypt(plain, cipher));
    Bytes recovered;
    REQUIRE(sec.decrypt(cipher, recovered));
    REQUIRE_EQ(recovered.size(), plain.size());
    CHECK_EQ(std::memcmp(plain.data(), recovered.data(), plain.size()), 0);
  }

  TEST_CASE("symmetric envelope with no payload is rejected before authentication") {
    Security sec(make_key_cfg("no_payload_seed"));

    Bytes plain = Bytes::create(16);
    std::memset(plain.data(), 0x81, plain.size());

    Bytes cipher;
    REQUIRE(sec.encrypt(plain, cipher));
    REQUIRE_GE(cipher.size(), 50U);

    Bytes no_payload = Bytes::create(50U);
    std::memcpy(no_payload.data(), cipher.data(), no_payload.size());

    Bytes recovered;
    CHECK_FALSE(sec.decrypt(no_payload, recovered));
  }

  TEST_CASE("security move self-assignment is a no-op") {
    Security sec(make_key_cfg("self_move_seed"));
    Security& same = sec;
    move_assign_security(sec, same);

    Bytes plain = Bytes::create(16);
    std::memset(plain.data(), 0x82, plain.size());

    Bytes cipher;
    REQUIRE(sec.encrypt(plain, cipher));

    Bytes recovered;
    REQUIRE(sec.decrypt(cipher, recovered));
    REQUIRE_EQ(recovered.size(), plain.size());
    CHECK_EQ(std::memcmp(plain.data(), recovered.data(), plain.size()), 0);
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

  TEST_CASE("valid non-RSA PEM keys are rejected by built-in RSA path") {
    const auto kp = generate_ec_keypair();

    Security::Config public_cfg;
    public_cfg.public_key_pem = kp.public_pem;
    Security public_sec(public_cfg);
    CHECK_FALSE(public_sec.is_configured());
    CHECK_FALSE(public_sec.can_encrypt());

    Security::Config private_cfg;
    private_cfg.private_key_pem = kp.private_pem;
    Security private_sec(private_cfg);
    CHECK_FALSE(private_sec.is_configured());
    CHECK_FALSE(private_sec.can_decrypt());

    Bytes plain = Bytes::create(16);
    std::memset(plain.data(), 0x91, plain.size());
    Bytes out;
    CHECK_FALSE(public_sec.encrypt(plain, out));
    CHECK_FALSE(private_sec.decrypt(plain, out));
  }

  TEST_CASE("tampered RSA wrapped key fails unwrap before plaintext is exposed") {
    const auto kp = generate_rsa_keypair(2048);

    Security::Config sender_cfg;
    sender_cfg.public_key_pem = kp.public_pem;
    Security sender(sender_cfg);

    Security::Config receiver_cfg;
    receiver_cfg.private_key_pem = kp.private_pem;
    Security receiver(receiver_cfg);

    Bytes plain = Bytes::create(32);
    std::memset(plain.data(), 0x92, plain.size());

    Bytes cipher;
    REQUIRE(sender.encrypt(plain, cipher));
    REQUIRE_GT(cipher.size(), 34U + 4U);

    Bytes tampered = clone_bytes(cipher);
    tampered.data()[34U + 4U] ^= 0x01U;

    Bytes recovered;
    CHECK_FALSE(receiver.decrypt(tampered, recovered));
    CHECK(recovered.empty());
  }

  TEST_CASE("asymmetric AAD context mismatch rejects otherwise valid envelope") {
    const auto kp = generate_rsa_keypair(2048);

    Security::Config sender_cfg;
    sender_cfg.public_key_pem = kp.public_pem;
    sender_cfg.advanced.aad_context = "asym-context-a";
    Security sender(sender_cfg);

    Security::Config receiver_cfg;
    receiver_cfg.private_key_pem = kp.private_pem;
    receiver_cfg.advanced.aad_context = "asym-context-b";
    Security receiver(receiver_cfg);

    Bytes plain = Bytes::create(24);
    std::memset(plain.data(), 0x93, plain.size());

    Bytes cipher;
    REQUIRE(sender.encrypt(plain, cipher));

    Bytes recovered;
    CHECK_FALSE(receiver.decrypt(cipher, recovered));
    CHECK(recovered.empty());
  }
}

#endif  // VLINK_TEST_SUPPORT_SECURITY

// NOLINTEND
