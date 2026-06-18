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

#include "./modules/shm2_conf.h"

#include <cerrno>
#include <memory>
#include <string>

#include "./base/helpers.h"
#include "./base/logger.h"
#include "./impl/conf_plugin_interface.h"
#include "./impl/url.h"
#include "./shm2_client_impl.h"
#include "./shm2_getter_impl.h"
#include "./shm2_publisher_impl.h"
#include "./shm2_server_impl.h"
#include "./shm2_setter_impl.h"
#include "./shm2_subscriber_impl.h"

namespace vlink {

VLINK_DEFINE_GLOBAL_PROPERTY(Shm2Conf)

static constexpr size_t kMaxStringSize = 80U;

// Shm2Conf
void Shm2Conf::global_init() { Shm2Factory::get(); }

bool Shm2Conf::parse_protocol(struct Protocol* protocol) {
  if VUNLIKELY (get_impl_type() == kUnknownImplType) {
    VLOG_F("Shm2Conf: Unknown node type, cannot parse protocol.");

    return false;
  }

  if VUNLIKELY (protocol->host.empty()) {
    return false;
  }

  address = protocol->host + (protocol->path.empty() ? "" : "/" + protocol->path);
  event = protocol->dictionary["event"];
  domain = Helpers::to_int(protocol->dictionary["domain"], 0);
  depth = Helpers::to_int(protocol->dictionary["depth"], 0);

  const auto& history_str = protocol->dictionary["history"];

  if (history_str.empty()) {
    if (get_impl_type() == kSetter || get_impl_type() == kGetter) {
      history = 1;
    } else {
      history = 0;
    }
  } else {
    history = Helpers::to_int(history_str, 0);
  }

  wait = Helpers::to_int(protocol->dictionary["wait"], 0);

  if (wait > 0 && (get_impl_type() == kClient || get_impl_type() == kServer || get_impl_type() == kSetter ||
                   get_impl_type() == kGetter)) {
    VLOG_W("Shm2Conf: Wait mode must be Event(Pub/Sub).");

    return false;
  }

  if (protocol->fragment.empty()) {
    size = kDefaultMemSize;
  } else {
    const char* str_start = protocol->fragment.c_str();

    if VUNLIKELY (std::isspace(static_cast<unsigned char>(str_start[0]))) {
      VLOG_E("Shm2Conf: Leading spaces are not allowed in fragment: ", protocol->fragment, ".");
      return false;
    }

    char* end_ptr = nullptr;

    errno = 0;

    double val = std::strtod(str_start, &end_ptr);

    if VUNLIKELY (str_start == end_ptr || errno == ERANGE) {
      VLOG_E("Shm2Conf: Failed to parse size from fragment: ", protocol->fragment, ".");
      return false;
    }

    if VUNLIKELY (val < 0) {
      VLOG_E("Shm2Conf: Negative size in fragment: ", protocol->fragment, ".");
      return false;
    }

    std::string_view unit(end_ptr, protocol->fragment.c_str() + protocol->fragment.size() - end_ptr);
    uint64_t multiplier = 1;

    if (unit.empty()) {
      multiplier = 1;
    } else if (unit.size() == 1) {
      char c0 = static_cast<char>(std::tolower(static_cast<unsigned char>(unit[0])));

      if (c0 == 'b') {
        multiplier = 1;
      } else if (c0 == 'k') {
        multiplier = 1024ULL;
      } else if (c0 == 'm') {
        multiplier = 1024ULL * 1024ULL;
      } else if (c0 == 'g') {
        multiplier = 1024ULL * 1024ULL * 1024ULL;
      } else {
        VLOG_E("Shm2Conf: Unknown size unit in fragment: ", protocol->fragment, ".");
        return false;
      }
    } else if (unit.size() == 2) {
      char c0 = static_cast<char>(std::tolower(static_cast<unsigned char>(unit[0])));
      char c1 = static_cast<char>(std::tolower(static_cast<unsigned char>(unit[1])));

      if (c1 != 'b') {
        VLOG_E("Shm2Conf: Unknown size unit in fragment: ", protocol->fragment, ".");
        return false;
      }

      if (c0 == 'k') {
        multiplier = 1024ULL;
      } else if (c0 == 'm') {
        multiplier = 1024ULL * 1024ULL;
      } else if (c0 == 'g') {
        multiplier = 1024ULL * 1024ULL * 1024ULL;
      } else {
        VLOG_E("Shm2Conf: Unknown size unit in fragment: ", protocol->fragment, ".");
        return false;
      }
    } else {
      VLOG_E("Shm2Conf: Unknown size unit in fragment: ", protocol->fragment, ".");
      return false;
    }

    size = static_cast<uint64_t>(val * static_cast<double>(multiplier));
  }

  return true;
}

bool Shm2Conf::is_valid() const {
  if VUNLIKELY (address.empty()) {
    return false;
  }

  if VUNLIKELY (address.size() > kMaxStringSize || event.size() > kMaxStringSize) {
    return false;
  }

  if VUNLIKELY (size == 0 || size > kMaxMemSize) {
    return false;
  }

  if VUNLIKELY (domain < 0 || depth < 0 || history < 0) {
    return false;
  }

  return true;
}

std::unique_ptr<ServerImpl> Shm2Conf::create_server() const { return std::make_unique<Shm2ServerImpl>(*this); }

std::unique_ptr<ClientImpl> Shm2Conf::create_client() const { return std::make_unique<Shm2ClientImpl>(*this); }

std::unique_ptr<PublisherImpl> Shm2Conf::create_publisher() const { return std::make_unique<Shm2PublisherImpl>(*this); }

std::unique_ptr<SubscriberImpl> Shm2Conf::create_subscriber() const {
  return std::make_unique<Shm2SubscriberImpl>(*this);
}

std::unique_ptr<SetterImpl> Shm2Conf::create_setter() const { return std::make_unique<Shm2SetterImpl>(*this); }

std::unique_ptr<GetterImpl> Shm2Conf::create_getter() const { return std::make_unique<Shm2GetterImpl>(*this); }

std::ostream& operator<<(std::ostream& ostream, const Shm2Conf& conf) noexcept {
  std::ios_base::fmtflags f = ostream.flags();

  ostream << "Shm2Conf:"
          << "[type]" << +conf.get_impl_type() << "[address]" << conf.address << "[event]" << conf.event << "[domain]"
          << conf.domain << "[depth]" << conf.depth << "[history]" << conf.history << "[wait]" << conf.wait << "[size]"
          << conf.size;

  ostream.flags(f);

  return ostream;
}

class ConfPluginShm2 : public ConfPluginInterface {
 protected:
  TransportType get_transport_type() const override { return TransportType::kShm2; }

  std::unique_ptr<Conf> create() const override { return std::make_unique<Shm2Conf>(); }
};

#ifdef VLINK_LIBRARY
VLINK_PLUGIN_DECLARE(ConfPluginShm2, 1, 0)
#endif

}  // namespace vlink
