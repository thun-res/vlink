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

#include "./impl/ssl_options.h"

#include <string>

#include "./base/helpers.h"
#include "./base/utils.h"
#include "./impl/conf.h"

namespace vlink {

// SslOptions
bool SslOptions::is_valid() const noexcept { return !ca_file.empty() || !cert_file.empty(); }

SslOptions SslOptions::parse_from(const Conf::PropertiesMap& properties) noexcept {
  SslOptions options;

  static const std::string& env_verify = Utils::get_env("VLINK_SSL_VERIFY");
  static const std::string& env_ca = Utils::get_env("VLINK_SSL_CA");
  static const std::string& env_cert = Utils::get_env("VLINK_SSL_CERT");
  static const std::string& env_key = Utils::get_env("VLINK_SSL_KEY");
  static const std::string& env_key_pass = Utils::get_env("VLINK_SSL_KEY_PASS");
  static const std::string& env_sni = Utils::get_env("VLINK_SSL_SNI");
  static const std::string& env_ciphers = Utils::get_env("VLINK_SSL_CIPHERS");

  options.verify_peer = (env_verify != "0");

  options.ca_file = env_ca;
  options.cert_file = env_cert;
  options.key_file = env_key;
  options.key_password = env_key_pass;
  options.server_name = env_sni;
  options.ciphers = env_ciphers;

  for (const auto& [prop, value] : properties) {
    if VLIKELY (!Helpers::has_startwith(prop, "ssl.")) {
      continue;
    }

    if (prop == "ssl.ca") {
      options.ca_file = value;
    } else if (prop == "ssl.cert") {
      options.cert_file = value;
    } else if (prop == "ssl.key") {
      options.key_file = value;
    } else if (prop == "ssl.key_password") {
      options.key_password = value;
    } else if (prop == "ssl.verify") {
      options.verify_peer = (value != "0");
    } else if (prop == "ssl.server_name") {
      options.server_name = value;
    } else if (prop == "ssl.ciphers") {
      options.ciphers = value;
    }
  }

  return options;
}

void SslOptions::parse_to(Conf::PropertiesMap& properties) const noexcept {
  if (!verify_peer) {
    properties["ssl.verify"] = "0";
  }

  if (!ca_file.empty()) {
    properties["ssl.ca"] = ca_file;
  }

  if (!cert_file.empty()) {
    properties["ssl.cert"] = cert_file;
  }

  if (!key_file.empty()) {
    properties["ssl.key"] = key_file;
  }

  if (!key_password.empty()) {
    properties["ssl.key_password"] = key_password;
  }

  if (!server_name.empty()) {
    properties["ssl.server_name"] = server_name;
  }

  if (!ciphers.empty()) {
    properties["ssl.ciphers"] = ciphers;
  }
}

}  // namespace vlink
