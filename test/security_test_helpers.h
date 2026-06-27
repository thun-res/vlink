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

#pragma once

// NOLINTBEGIN

#ifdef VLINK_TEST_SUPPORT_SECURITY

#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/rsa.h>

#include <memory>
#include <string>

namespace vlink_test_sec {

struct RsaKeyPair {
  std::string public_pem;
  std::string private_pem;
};

inline RsaKeyPair generate_rsa_keypair(int bits = 2048) {
  using EvpPkeyCtxPtr = std::unique_ptr<EVP_PKEY_CTX, decltype(&EVP_PKEY_CTX_free)>;
  using EvpPkeyPtr = std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)>;
  using BioPtr = std::unique_ptr<BIO, decltype(&BIO_free)>;

  RsaKeyPair kp;
  EvpPkeyCtxPtr gctx{EVP_PKEY_CTX_new_id(EVP_PKEY_RSA, nullptr), &EVP_PKEY_CTX_free};

  if (!gctx || EVP_PKEY_keygen_init(gctx.get()) <= 0 || EVP_PKEY_CTX_set_rsa_keygen_bits(gctx.get(), bits) <= 0) {
    return kp;
  }

  EVP_PKEY* raw_pkey = nullptr;

  if (EVP_PKEY_keygen(gctx.get(), &raw_pkey) <= 0 || raw_pkey == nullptr) {
    return kp;
  }

  EvpPkeyPtr pkey{raw_pkey, &EVP_PKEY_free};

  BioPtr pub_bio{BIO_new(BIO_s_mem()), &BIO_free};

  if (!pub_bio || PEM_write_bio_PUBKEY(pub_bio.get(), pkey.get()) != 1) {
    return kp;
  }

  char* pub_buf = nullptr;
  const auto pub_len = BIO_get_mem_data(pub_bio.get(), &pub_buf);

  if (pub_buf == nullptr || pub_len <= 0) {
    return kp;
  }

  kp.public_pem.assign(pub_buf, static_cast<size_t>(pub_len));

  BioPtr prv_bio{BIO_new(BIO_s_mem()), &BIO_free};

  if (!prv_bio || PEM_write_bio_PrivateKey(prv_bio.get(), pkey.get(), nullptr, nullptr, 0, nullptr, nullptr) != 1) {
    return kp;
  }

  char* prv_buf = nullptr;
  const auto prv_len = BIO_get_mem_data(prv_bio.get(), &prv_buf);

  if (prv_buf == nullptr || prv_len <= 0) {
    return kp;
  }

  kp.private_pem.assign(prv_buf, static_cast<size_t>(prv_len));

  return kp;
}

}  // namespace vlink_test_sec

#endif  // VLINK_TEST_SUPPORT_SECURITY

// NOLINTEND
