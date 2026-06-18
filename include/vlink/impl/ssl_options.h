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
 * @file ssl_options.h
 * @brief Backend-neutral SSL / TLS settings shared by all VLink transports that support encryption.
 *
 * @details
 * This is an internal implementation header used by @c NodeImpl and by every
 * transport backend that supports TLS; user code typically interacts with
 * @c SslOptions through @c Node::set_ssl_options().  The struct is a thin
 * aggregate that the transport factory translates into the @c ssl.* property
 * convention used during connection setup.
 *
 * @par Native mechanism per backend
 * | Backend     | Underlying TLS mechanism                                                |
 * | ----------- | ----------------------------------------------------------------------- |
 * | MQTT        | @c MQTTClient_SSLOptions (Paho C); auto promotes @c tcp:// to @c ssl:// |
 * | DDS         | @c TCPv4TransportDescriptor::tls_config (Fast-DDS)                      |
 * | CycloneDDS  | @c ddsi_config SSL fields (requires @c DDS_HAS_SSL)                     |
 * | Zenoh       | @c transport/link/tls config keys (zenoh-c, not zenoh-pico)             |
 *
 * @par SSL options table
 * | Field            | Property key         | Description                                  |
 * | ---------------- | -------------------- | -------------------------------------------- |
 * | @c ca_file       | @c ssl.ca            | CA certificate file path (PEM).              |
 * | @c cert_file     | @c ssl.cert          | Client certificate file path (PEM).          |
 * | @c key_file      | @c ssl.key           | Client private key file path (PEM).          |
 * | @c key_password  | @c ssl.key_password  | Passphrase for an encrypted private key.     |
 * | @c verify_peer   | @c ssl.verify        | @c "0" to skip server verification.          |
 * | @c server_name   | @c ssl.server_name   | SNI override.                                |
 * | @c ciphers       | @c ssl.ciphers       | Cipher suite string (OpenSSL format).        |
 *
 * @par Environment variable defaults
 * The factory consults these variables when the matching property is not set;
 * explicit properties always win.
 *
 * | Environment variable    | Property key         |
 * | ----------------------- | -------------------- |
 * | @c VLINK_SSL_CA         | @c ssl.ca            |
 * | @c VLINK_SSL_CERT       | @c ssl.cert          |
 * | @c VLINK_SSL_KEY        | @c ssl.key           |
 * | @c VLINK_SSL_KEY_PASS   | @c ssl.key_password  |
 * | @c VLINK_SSL_VERIFY     | @c ssl.verify        |
 * | @c VLINK_SSL_SNI        | @c ssl.server_name   |
 * | @c VLINK_SSL_CIPHERS    | @c ssl.ciphers       |
 *
 * @par Auto-detection
 * TLS is considered active when @c ca_file or @c cert_file is non-empty; there
 * is no separate enable flag.  On DDS / CycloneDDS, activating TLS also forces
 * the TCP transport because TLS rides over TCP.
 *
 * @par Example
 * @code
 * // --- Through the public Node API ---
 * vlink::Publisher<MyMsg> pub("mqtt://sensor/data");
 * vlink::SslOptions ssl;
 * ssl.ca_file   = "/etc/certs/ca.pem";
 * ssl.cert_file = "/etc/certs/client.pem";
 * ssl.key_file  = "/etc/certs/client-key.pem";
 * pub.set_ssl_options(ssl);
 *
 * // --- Through set_property ---
 * pub.set_property("ssl.ca", "/etc/certs/ca.pem");
 *
 * // --- Through global property ---
 * vlink::MqttConf::set_global_property("ssl.ca", "/etc/certs/ca.pem");
 *
 * // --- Through environment ---
 * // export VLINK_SSL_CA=/etc/certs/ca.pem
 * @endcode
 *
 * @note
 * - Zenoh-pico (@c VLINK_ENABLE_ZENOH_PICO) does not support TLS; SSL
 *   properties trigger a warning.
 * - CycloneDDS requires @c DDS_HAS_SSL at compile time; if the feature was
 *   not compiled in, SSL properties also trigger a warning.
 */

#pragma once

#include <string>

#include "../base/macros.h"
#include "./conf.h"

namespace vlink {

/**
 * @struct SslOptions
 * @brief Aggregate of SSL / TLS settings for transport-layer encryption.
 *
 * @details
 * The struct can be either populated by hand and passed to
 * @c Node::set_ssl_options(), or constructed from a @c Conf::PropertiesMap via
 * @c parse_from() / written back via @c parse_to().  An instance is considered
 * to enable TLS when @c ca_file or @c cert_file is non-empty.
 */
struct VLINK_EXPORT SslOptions final {
  /**
   * @brief Whether the server certificate must be validated.
   *
   * @details
   * Defaults to @c true.  Setting it to @c false maps to @c ssl.verify = @c "0"
   * and disables peer verification, which is convenient for development with
   * self-signed certificates but should never ship to production.
   */
  bool verify_peer{true};

  /**
   * @brief Path to the CA certificate (PEM) used to validate the peer.
   *
   * @details
   * Setting this field (or its corresponding @c ssl.ca property) is one of the
   * two conditions that makes @c is_valid() report @c true.
   */
  std::string ca_file;

  /**
   * @brief Path to the client certificate (PEM) used for mutual TLS.
   *
   * @details
   * Setting this field is the second condition that makes @c is_valid() report
   * @c true.  Pair with @c key_file for true mTLS.
   */
  std::string cert_file;

  /**
   * @brief Path to the client private key (PEM) accompanying @c cert_file.
   *
   * @details
   * If the key is encrypted, provide its passphrase via @c key_password.
   */
  std::string key_file;

  /**
   * @brief Passphrase for an encrypted private key.
   *
   * @details
   * Mirrored to the @c ssl.key_password property and the
   * @c VLINK_SSL_KEY_PASS environment variable.
   */
  std::string key_password;

  /**
   * @brief Server Name Indication (SNI) override.
   *
   * @details
   * When non-empty the handshake announces this name to the server instead of
   * the host derived from the URL.  Honoured by MQTT, DDS and Zenoh.
   */
  std::string server_name;

  /**
   * @brief Cipher suite string passed verbatim to the TLS implementation.
   *
   * @details
   * Format depends on the underlying TLS library (OpenSSL by default).  Leave
   * empty to inherit the backend's default cipher choice.
   */
  std::string ciphers;

  /**
   * @brief Default-constructs an empty options aggregate.
   */
  SslOptions() noexcept = default;

  /**
   * @brief Default destructor.
   */
  ~SslOptions() noexcept = default;

  /**
   * @brief Reports whether the configuration is sufficient to enable TLS.
   *
   * @details
   * Returns @c true as soon as @c ca_file or @c cert_file is non-empty.  An
   * otherwise empty @c SslOptions yields @c false and the transport backend
   * skips the TLS handshake entirely.
   *
   * @return @c true when TLS should be activated.
   */
  bool is_valid() const noexcept;

  /**
   * @brief Reads @c ssl.* entries from @p properties (and the environment) into a new instance.
   *
   * @details
   * Resolution order, highest priority first:
   * -# Explicit @c ssl.* entries in @p properties.
   * -# Matching @c VLINK_SSL_* environment variables.
   *
   * Fields absent from both sources keep their default values.
   *
   * @param properties  Property map to read.
   * @return Fully-resolved @c SslOptions aggregate.
   */
  static SslOptions parse_from(const Conf::PropertiesMap& properties) noexcept;

  /**
   * @brief Writes non-default fields back into @p properties as @c ssl.* entries.
   *
   * @details
   * Only non-empty string fields and a @c false @c verify_peer are emitted.
   * Used internally by @c Node::set_ssl_options() to merge the aggregate into
   * the node property map.
   *
   * @param properties  Property map updated in place.
   */
  void parse_to(Conf::PropertiesMap& properties) const noexcept;
};

}  // namespace vlink
