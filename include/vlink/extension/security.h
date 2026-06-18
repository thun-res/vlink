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

/**
 * @file security.h
 * @brief Application-layer authenticated encryption with symmetric, hybrid asymmetric, and pluggable backends.
 *
 * @details
 * @c Security sits between VLink serialisation and any transport layer.  Every endpoint
 * (publisher / subscriber / client / server / setter / getter) may own at most one @c Security
 * instance; the instance authenticates and encrypts each outbound message and verifies and
 * decrypts each inbound message before the transport sees the payload.  It is fully orthogonal
 * to the channel-level TLS controlled by @c SslOptions and can be combined with it.
 *
 * Compiling with @c VLINK_ENABLE_SECURITY enables the OpenSSL-backed default crypto suite.
 * Without that macro only the user-supplied callback path is available.
 *
 * @par Encryption algorithms
 *
 * | Algorithm          | Key size           | Mode / construction                      | Used for                    |
 * | ------------------ | ------------------ | ---------------------------------------- | --------------------------- |
 * | AES-GCM            | 128 bit            | AEAD, 12-byte nonce, 16-byte tag         | bulk encrypt / decrypt      |
 * | RSA-OAEP-SHA256    | >= 2048 b          | wrap a fresh per-message AES key         | asymmetric session-key wrap |
 * | RSA-PSS-SHA256     | >= 2048 b          | sender signature over AAD-bound envelope | optional sender identity    |
 * | SHA-256 truncation | 256 -> 128         | derive raw @c key -> AES-128 key         | symmetric key derivation    |
 * | PBKDF2-HMAC-SHA256 | configurable iters | derive passphrase -> AES-128             | symmetric key derivation    |
 *
 * @par Key sources
 *
 * | Source                     | Selector field(s)                         | Notes                                    |
 * | -------------------------- | ----------------------------------------- | ---------------------------------------- |
 * | Built-in default symmetric | every cryptographic field empty           | only with @c VLINK_ENABLE_SECURITY       |
 * | Explicit raw key           | @c Config::key                            | SHA-256 truncated to AES-128             |
 * | Passphrase + salt          | @c Config::passphrase + @c pbkdf2_salt    | PBKDF2-HMAC-SHA256 @ iterations          |
 * | Peer public-key PEM        | @c Config::public_key_pem                 | RSA-OAEP wrap of a fresh session key     |
 * | Local private-key PEM      | @c Config::private_key_pem                | RSA-OAEP unwrap of session key           |
 * | Custom callback pair       | @c encrypt_callback + @c decrypt_callback | bypasses every built-in algorithm        |
 *
 * @par Mode summary diagram
 * @code
 *      sender                                       transport                                       receiver
 *  +------------+                                 +-----------+                                 +-------------+
 *  |  plaintext | ---> [select mode] ---> AEAD/RSA-OAEP envelope ---> bytes on wire --->  [select mode] ---> |
 *  +------------+         ^                                                                  ^   plaintext   |
 *                         |                                                                  |               |
 *           custom > asymmetric > symmetric                       custom > asymmetric > symmetric            |
 *                                                                                                            |
 *  encrypt(in, out)  -->  envelope(version, mode, nonce, AAD)  -->  ciphertext + tag  -->  decrypt(in, out)  v
 * @endcode
 *
 * @par Example (1: custom callback pair)
 * @code
 *   vlink::Security::Config cfg;
 *   cfg.encrypt_callback = [](const vlink::Bytes& in, vlink::Bytes& out) {
 *     out = my_aead_encrypt(in);
 *     return !out.empty();
 *   };
 *   cfg.decrypt_callback = [](const vlink::Bytes& in, vlink::Bytes& out) {
 *     out = my_aead_decrypt(in);
 *     return !out.empty();
 *   };
 *   vlink::Security sec(std::move(cfg));
 * @endcode
 *
 * @par Example (2: symmetric passphrase with PBKDF2)
 * @code
 *   vlink::Security::Config cfg;
 *   cfg.passphrase = "correct horse battery staple";
 *   cfg.pbkdf2_salt = shared_salt;            // >= 16 bytes, shared out of band
 *   cfg.pbkdf2_iterations = 200000U;
 *   vlink::Security sec(cfg);
 * @endcode
 *
 * @par Example (3: asymmetric PEM with optional sender authentication)
 * @code
 *   auto sender_cfg = vlink::Security::from_public_key_path("peer_pub.pem");
 *   sender_cfg.advanced.signing_key_pem = own_priv_pem;   // optional RSA-PSS signature
 *   vlink::Security sender(std::move(sender_cfg));
 *
 *   auto receiver_cfg = vlink::Security::from_private_key_path("own_priv.pem");
 *   receiver_cfg.advanced.verify_key_pem = peer_pub_pem;  // reject unsigned envelopes
 *   vlink::Security receiver(std::move(receiver_cfg));
 * @endcode
 *
 * @note
 * - Every public method is thread-safe; concurrent @c encrypt() / @c decrypt() are serialised
 *   by an internal mutex.
 * - Configuration is immutable after construction; rebuild the @c Security instance to change
 *   keys or callbacks.
 * - When @c VLINK_ENABLE_SECURITY is undefined only the callback path is usable; other fields
 *   are accepted but emit a warning and remain unconfigured.
 */

#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include "../base/bytes.h"
#include "../base/functional.h"
#include "../base/macros.h"

namespace vlink {

/**
 * @class Security
 * @brief Thread-safe authenticated encryption with symmetric, asymmetric, and pluggable modes.
 *
 * @details
 * One @c Security instance per endpoint.  Configuration is supplied through @c Config at
 * construction time; copy and assignment are deleted, move is supported.  Each call to
 * @c encrypt() / @c decrypt() selects a mode using the precedence order described in the
 * file-level @c mode summary diagram.
 */
class VLINK_EXPORT Security final {
 public:
  /**
   * @brief Callable signature for user-supplied encrypt / decrypt callbacks.
   *
   * @details
   * Implementations must populate @c out and return @c true on success; failure leaves
   * @c out empty and the surrounding @c encrypt() / @c decrypt() call returns @c false.
   * @c Function is copyable, so callbacks may be passed in a const-reference @c Config.
   */
  using Callback = Function<bool(const Bytes& in, Bytes& out)>;

  /**
   * @struct Config
   * @brief Aggregate of every parameter accepted by the @c Security constructor.
   *
   * @details
   * Fields are processed independently.  Empty strings, empty @c Bytes, and null callbacks
   * mean "leave this slot blank"; non-empty values are validated by the constructor and
   * installed on success or logged-and-ignored on failure.  When every cryptographic field
   * is empty and @c VLINK_ENABLE_SECURITY is defined, the constructor falls back to the
   * built-in default symmetric slot, which is intended for development only.
   *
   * @par Precedence rules
   * - When both callbacks are present, the built-in path is bypassed for both directions.
   * - Outbound: @c public_key_pem (if installed) -> @c key / @c passphrase -> default slot.
   * - Inbound : @c private_key_pem (if installed) -> @c key / @c passphrase -> default slot.
   */
  struct Config final {
    /**
     * @struct Advanced
     * @brief Low-frequency policy knobs and sender-authentication keys.
     */
    struct Advanced final {
      std::string aad_context;        ///< Application or channel tag (<= 65535 bytes) bound into AEAD AAD.
      uint32_t replay_window{4096U};  ///< Sliding replay-window size in messages; @c 0 disables anti-replay.
      std::string signing_key_pem;    ///< Local RSA private key (PEM) used to sign with RSA-PSS-SHA256.
      std::string verify_key_pem;     ///< Peer's RSA public key (PEM) required for RSA-PSS verification.
    };

    std::string key;                      ///< Raw symmetric seed; SHA-256 truncated to 16 bytes.
    std::string passphrase;               ///< Low-entropy passphrase consumed by PBKDF2-HMAC-SHA256.
    Bytes pbkdf2_salt;                    ///< Per-deployment salt (>=16 bytes) shared out of band.
    uint32_t pbkdf2_iterations{200000U};  ///< PBKDF2 iteration count, tune for target hardware.
    std::string public_key_pem;           ///< Peer's RSA public key (PEM) for RSA-OAEP outbound wrap.
    std::string private_key_pem;          ///< Local RSA private key (PEM) for RSA-OAEP inbound unwrap.
    Callback encrypt_callback;            ///< Custom encrypt; bypasses the built-in AEAD pipeline.
    Callback decrypt_callback;            ///< Custom decrypt; must accompany @c encrypt_callback.
    Advanced advanced;                    ///< AAD, replay window, and signing / verifying PEM material.

    Config() = default;
  };

  /**
   * @brief Loads a private-key PEM file into a fresh @c Config.
   *
   * @details
   * Reads the file at construction time and populates @c Config::private_key_pem.  RSA
   * validation is deferred until the @c Security constructor consumes the config.
   *
   * @param private_key_path  Filesystem path of the PEM-encoded private key.
   * @return Pre-populated @c Config; @c private_key_pem is empty when the file is unreadable.
   */
  [[nodiscard]] static Config from_private_key_path(const std::string& private_key_path);

  /**
   * @brief Loads a public-key PEM file into a fresh @c Config.
   *
   * @param public_key_path  Filesystem path of the PEM-encoded public key.
   * @return Pre-populated @c Config; @c public_key_pem is empty when the file is unreadable.
   */
  [[nodiscard]] static Config from_public_key_path(const std::string& public_key_path);

  /**
   * @brief Loads both a public-key and a private-key PEM file into a fresh @c Config.
   *
   * @param public_key_path   Filesystem path of the PEM-encoded peer public key.
   * @param private_key_path  Filesystem path of the PEM-encoded local private key.
   * @return Pre-populated @c Config; per-file misses leave only the affected field empty.
   */
  [[nodiscard]] static Config from_key_paths(const std::string& public_key_path, const std::string& private_key_path);

  /**
   * @brief Constructs an empty @c Security; equivalent to @c Security(Config{}).
   *
   * @details
   * With built-in algorithms compiled in, this installs the default symmetric slot; otherwise
   * the instance reports @c is_configured() == @c false and refuses to encrypt or decrypt.
   */
  Security();

  /**
   * @brief Constructs from a configuration aggregate, copying caller state.
   *
   * @param cfg  Configuration aggregate; every non-empty field is validated and installed.
   */
  explicit Security(const Config& cfg);

  /**
   * @brief Constructs from a configuration aggregate, moving caller state in.
   *
   * @param cfg  Configuration aggregate consumed during construction.
   */
  explicit Security(Config&& cfg);

  /**
   * @brief Destroys the instance and zeroises any held symmetric key material in place.
   */
  ~Security();

  /**
   * @brief Move-constructs from another @c Security; the source becomes default-constructed.
   */
  Security(Security&&) noexcept;

  /**
   * @brief Move-assigns from another @c Security; the source becomes default-constructed.
   */
  Security& operator=(Security&&) noexcept;

  /**
   * @brief Encrypts @p in into @p out using the highest-precedence active mode.
   *
   * @details
   * The selected mode follows the precedence summarised in the file-level diagram:
   * @c custom > @c asymmetric (public key present) > @c symmetric.  Empty @p in fails;
   * AEAD requires at least one byte of authenticated material.  Inputs exceeding
   * @c INT_MAX bytes are rejected.
   *
   * @param in   Plaintext payload.
   * @param out  Output buffer overwritten on success and emptied on failure.
   * @return @c true on success; @c false otherwise.
   */
  bool encrypt(const Bytes& in, Bytes& out);

  /**
   * @brief Decrypts @p in into @p out using the highest-precedence active mode.
   *
   * @details
   * Mode selection mirrors @c encrypt() but uses the inbound direction
   * (@c custom > @c asymmetric private key > @c symmetric).  Tampered ciphertext,
   * mismatched AAD, replayed sequence numbers, and missing or invalid RSA-PSS
   * signatures (when @c verify_key_pem is set) cause failure.
   *
   * @param in   Ciphertext produced by a peer's @c encrypt().
   * @param out  Output buffer overwritten on success and emptied on failure.
   * @return @c true on success; @c false otherwise.
   */
  bool decrypt(const Bytes& in, Bytes& out);

  /**
   * @brief Reports whether at least one usable cryptographic slot is installed.
   *
   * @details
   * A @c true result merely means the instance can call @c encrypt() or @c decrypt() in
   * @b some direction; senders should additionally verify @c can_encrypt() and receivers
   * @c can_decrypt() to catch direction-specific RSA misconfiguration.
   *
   * @return @c true when any slot is usable in either direction.
   */
  [[nodiscard]] bool is_configured() const noexcept;

  /**
   * @brief Reports whether @c encrypt() will succeed.
   *
   * @details
   * Requires at least one of: a custom callback pair, a derived AES-128 key, or a peer
   * @c public_key_pem.  A bare @c private_key_pem alone is insufficient.
   *
   * @return @c true when an encryption capability is installed.
   */
  [[nodiscard]] bool can_encrypt() const noexcept;

  /**
   * @brief Reports whether @c decrypt() will succeed.
   *
   * @details
   * Requires at least one of: a custom callback pair, a derived AES-128 key, or a local
   * @c private_key_pem.  A bare @c public_key_pem alone is insufficient.
   *
   * @return @c true when a decryption capability is installed.
   */
  [[nodiscard]] bool can_decrypt() const noexcept;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;

  VLINK_DISALLOW_COPY_AND_ASSIGN(Security)
};

}  // namespace vlink
