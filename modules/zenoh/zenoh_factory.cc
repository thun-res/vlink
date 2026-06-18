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

#include "./zenoh_factory.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <iterator>
#include <limits>
#include <optional>
#include <string>
#include <utility>

#include "./base/elapsed_timer.h"
#include "./base/helpers.h"
#include "./extension/qos_profile.h"
#include "./impl/server_impl.h"
#include "./impl/ssl_options.h"

namespace vlink {

[[maybe_unused]] static void encode_u64be(uint8_t* buf, uint64_t val) {
  buf[0] = static_cast<uint8_t>(val >> 56);
  buf[1] = static_cast<uint8_t>(val >> 48);
  buf[2] = static_cast<uint8_t>(val >> 40);
  buf[3] = static_cast<uint8_t>(val >> 32);
  buf[4] = static_cast<uint8_t>(val >> 24);
  buf[5] = static_cast<uint8_t>(val >> 16);
  buf[6] = static_cast<uint8_t>(val >> 8);
  buf[7] = static_cast<uint8_t>(val >> 0);
}

[[maybe_unused]] static uint64_t decode_u64be(const uint8_t* buf) {
  return (static_cast<uint64_t>(buf[0]) << 56) | (static_cast<uint64_t>(buf[1]) << 48) |
         (static_cast<uint64_t>(buf[2]) << 40) | (static_cast<uint64_t>(buf[3]) << 32) |
         (static_cast<uint64_t>(buf[4]) << 24) | (static_cast<uint64_t>(buf[5]) << 16) |
         (static_cast<uint64_t>(buf[6]) << 8) | (static_cast<uint64_t>(buf[7]) << 0);
}

[[maybe_unused]] static bool has_prefix(const std::string& value, const char* prefix) {
  // NOLINTNEXTLINE(modernize-use-starts-ends-with)
  return value.rfind(prefix, 0) == 0;
}

[[maybe_unused]] static std::string lower_ascii(std::string value) {
  for (auto& ch : value) {
    ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
  }

  return value;
}

[[maybe_unused]] static bool parse_bool_value(const std::string& value, bool fallback) {
  if (value.empty()) {
    return fallback;
  }

  const auto normalized = lower_ascii(value);

  if (normalized == "1" || normalized == "true" || normalized == "yes" || normalized == "on") {
    return true;
  }

  if (normalized == "0" || normalized == "false" || normalized == "no" || normalized == "off") {
    return false;
  }

  return Helpers::to_int(value, fallback ? 1 : 0) != 0;
}

[[maybe_unused]] static bool parse_size_value(const std::string& value, size_t* out, bool allow_zero = false) {
  if (value.empty() || !out) {
    return false;
  }

  size_t number_end = value.size();
  uint64_t multiplier = 1;

  if (number_end > 0 && std::isalpha(static_cast<unsigned char>(value[number_end - 1]))) {
    --number_end;

    if (number_end > 0 && (value[value.size() - 1] == 'b' || value[value.size() - 1] == 'B') &&
        std::isalpha(static_cast<unsigned char>(value[number_end - 1]))) {
      --number_end;
    }

    const char unit = static_cast<char>(std::tolower(static_cast<unsigned char>(value[number_end])));

    if (unit == 'k') {
      multiplier = 1024ULL;
    } else if (unit == 'm') {
      multiplier = 1024ULL * 1024ULL;
    } else if (unit == 'g') {
      multiplier = 1024ULL * 1024ULL * 1024ULL;
    } else if (unit == 'b') {
      multiplier = 1;
    } else {
      return false;
    }
  }

  if (number_end == 0) {
    return false;
  }

  uint64_t parsed = 0;
  const auto* begin = value.data();
  const auto* end = value.data() + number_end;
  auto [ptr, err] = std::from_chars(begin, end, parsed);

  if (err != std::errc() || ptr != end || (!allow_zero && parsed == 0) ||
      parsed > static_cast<uint64_t>(std::numeric_limits<size_t>::max()) / multiplier) {
    return false;
  }

  *out = static_cast<size_t>(parsed * multiplier);
  return true;
}

[[maybe_unused]] static int32_t zenoh_depth_from_qos(int32_t depth, const Qos& qos) {
  if (depth > 0) {
    return depth;
  }

  if (qos.valid && qos.history.depth > 0) {
    return qos.history.depth;
  }

  return 0;
}

[[maybe_unused]] static std::optional<size_t> parse_zenoh_queue_depth(const std::string& value, const char* name) {
  size_t depth = 0;
  auto [ptr, err] = std::from_chars(value.data(), value.data() + value.size(), depth);

  if (err != std::errc() || ptr != value.data() + value.size() || depth == 0) {
    VLOG_W("ZenohFactory: Invalid ", name, "='", value, "', ignoring this override.");
    return std::nullopt;
  }

  if (depth > 16) {
    VLOG_W("ZenohFactory: ", name, "=", depth, " exceeds Zenoh queue limit 16, using 16.");
    return 16;
  }

  return depth;
}

[[maybe_unused]] static Conf::PropertiesMap zenoh_session_properties_from(const Conf::PropertiesMap& properties) {
  Conf::PropertiesMap session_properties = properties;

  session_properties.erase("zenoh.shm_blocking");
  session_properties.erase("zenoh.shm_loan_threshold");

  return session_properties;
}

[[maybe_unused]] static bool zenoh_shm_enabled_from(const std::string& fragment,
                                                    const Conf::PropertiesMap& properties) {
  static bool env_shm = parse_bool_value(Utils::get_env("VLINK_ZENOH_SHM", "0"), false);

  bool enabled = env_shm || fragment == "shm";

  auto iter = properties.find("zenoh.shm");

  if (iter != properties.end()) {
    enabled = parse_bool_value(iter->second, enabled);
  }

  return enabled;
}

[[maybe_unused]] static bool zenoh_shm_blocking_from(const Conf::PropertiesMap& properties) {
  static bool env_blocking = parse_bool_value(Utils::get_env("VLINK_ZENOH_SHM_BLOCKING", "0"), false);

  bool blocking = env_blocking;

  auto iter = properties.find("zenoh.shm_blocking");

  if (iter != properties.end()) {
    blocking = parse_bool_value(iter->second, blocking);
  }

  return blocking;
}

[[maybe_unused]] static size_t zenoh_shm_loan_threshold_from(const Conf::PropertiesMap& properties) {
  static std::string env_threshold =
      Utils::get_env("VLINK_ZENOH_SHM_LOAN_THRESHOLD", std::to_string(kZenohDefaultShmLoanThreshold));

  size_t threshold = kZenohDefaultShmLoanThreshold;
  size_t parsed = 0;

  if (parse_size_value(env_threshold, &parsed, true)) {
    threshold = parsed;
  }

  auto iter = properties.find("zenoh.shm_loan_threshold");

  if (iter != properties.end() && parse_size_value(iter->second, &parsed, true)) {
    threshold = parsed;
  }

  return threshold;
}

struct ZenohReplyContext final {
  std::weak_ptr<ZenohClient> instance;
  uint64_t seq{0};
  std::atomic_bool dropped{false};
};

struct ZenohPayloadView final {
  ZenohPayloadView() = default;

  ~ZenohPayloadView() {
    if (slice_owned) {
      z_drop(z_move(slice));
    }
  }

  ZenohPayloadView(const ZenohPayloadView&) = delete;
  ZenohPayloadView& operator=(const ZenohPayloadView&) = delete;

  bool load(const z_loaned_bytes_t* payload) {
    if VUNLIKELY (!payload) {
      return false;
    }

#if defined(Z_FEATURE_UNSTABLE_API)

    if (z_bytes_get_contiguous_view(payload, &view) == Z_OK) {
      data = z_slice_data(z_view_slice_loan(&view));
      size = z_slice_len(z_view_slice_loan(&view));
      return true;
    }
#endif

    if VUNLIKELY (z_bytes_to_slice(payload, &slice) != Z_OK) {
      return false;
    }

    slice_owned = true;
    data = z_slice_data(z_loan(slice));
    size = z_slice_len(z_loan(slice));
    return true;
  }

  const uint8_t* data{nullptr};
  size_t size{0};
#if defined(Z_FEATURE_UNSTABLE_API)
  z_view_slice_t view;
#endif
  z_owned_slice_t slice;
  bool slice_owned{false};
};

static bool keep_query_for_deferred_reply(z_owned_query_t* dst, z_loaned_query_t* src) {
  if VUNLIKELY (!dst || !src) {
    return false;
  }

#ifdef VLINK_ENABLE_ZENOH_PICO
  return z_clone(dst, src) == Z_OK;
#else
  z_clone(dst, src);
  return z_internal_check(*dst);
#endif
}

#if VLINK_ZENOH_SHM_AVAILABLE
// ZenohShmSupport
ZenohShmSupport::ZenohShmSupport() { z_internal_null(&provider_); }

ZenohShmSupport::~ZenohShmSupport() {
  std::lock_guard lock(mtx_);
  clear_locked();
}

void ZenohShmSupport::configure(bool enabled, bool blocking, size_t loan_threshold) {
  std::lock_guard lock(mtx_);

  enabled_ = enabled;
  blocking_ = blocking;
  loan_threshold_ = loan_threshold;
  loan_map_.reserve(8);

  if (!enabled_) {
    clear_locked();
  }
}

bool ZenohShmSupport::is_support_loan(const ZenohSessionPtr& session) {
  std::lock_guard lock(mtx_);
  return enabled_ && init_locked(session);
}

bool ZenohShmSupport::init_locked(const ZenohSessionPtr& session) {
  if (!enabled_) {
    return false;
  }

  if (ready_ && z_internal_check(provider_)) {
    return true;
  }

  if VUNLIKELY (!session || !z_internal_check(*session)) {
    return false;
  }

  z_owned_shared_shm_provider_t provider;
  z_internal_null(&provider);

  z_shm_provider_state state = Z_SHM_PROVIDER_STATE_DISABLED;
  z_result_t ret = z_obtain_shm_provider(z_loan(*session), &provider, &state);

  if (ret == Z_OK && z_internal_check(provider)) {
    if (z_internal_check(provider_)) {
      z_drop(z_move(provider_));
    }

    z_take(&provider_, z_move(provider));
    ready_ = true;
    return true;
  }

  if (z_internal_check(provider)) {
    z_drop(z_move(provider));
  }

  ready_ = false;

  if (state == Z_SHM_PROVIDER_STATE_INITIALIZING) {
    return false;
  }

  VLOG_W("ZenohFactory: Zenoh SHM provider is not ready, state=", static_cast<int>(state), ", ret=", +ret, ".");
  return false;
}

void ZenohShmSupport::clear_locked() {
  for (auto& [data, shm] : loan_map_) {
    (void)data;

    if (z_internal_check(shm)) {
      z_drop(z_move(shm));
    }
  }

  loan_map_.clear();

  if (z_internal_check(provider_)) {
    z_drop(z_move(provider_));
  }

  ready_ = false;
}

Bytes ZenohShmSupport::loan(const ZenohSessionPtr& session, int64_t size) {
  if VUNLIKELY (size <= 0) {
    return Bytes();
  }

  const auto request_size = static_cast<size_t>(size);

  if (request_size < loan_threshold_) {
    return Bytes::create(request_size);
  }

  z_owned_shared_shm_provider_t provider;
  z_internal_null(&provider);
  bool blocking = false;

  {
    std::lock_guard lock(mtx_);

    if VUNLIKELY (!init_locked(session)) {
      return Bytes();
    }

    z_shared_shm_provider_clone(&provider, z_shared_shm_provider_loan(&provider_));
    blocking = blocking_;
  }

  if VUNLIKELY (!z_internal_check(provider)) {
    return Bytes();
  }

  z_buf_layout_alloc_result_t result{};
  result.status = ZC_BUF_LAYOUT_ALLOC_STATUS_ALLOC_ERROR;
  z_internal_null(&result.buf);

  if (blocking) {
    z_shm_provider_alloc_gc_defrag_blocking(&result, z_shared_shm_provider_loan_as(z_loan(provider)), request_size);
  } else {
    z_shm_provider_alloc_gc_defrag(&result, z_shared_shm_provider_loan_as(z_loan(provider)), request_size);
  }

  z_drop(z_move(provider));

  if VUNLIKELY (result.status != ZC_BUF_LAYOUT_ALLOC_STATUS_OK || !z_internal_check(result.buf)) {
    if (z_internal_check(result.buf)) {
      z_drop(z_move(result.buf));
    }

    VLOG_E("ZenohFactory: Failed to loan SHM buffer, size=", request_size, ", status=", static_cast<int>(result.status),
           ".");
    return Bytes();
  }

  auto* data = z_shm_mut_data_mut(z_loan_mut(result.buf));
  const size_t actual_size = z_shm_mut_len(z_loan(result.buf));

  if VUNLIKELY (!data || actual_size < request_size) {
    z_drop(z_move(result.buf));
    VLOG_E("ZenohFactory: Invalid SHM loan buffer, requested=", request_size, ", actual=", actual_size, ".");
    return Bytes();
  }

  {
    std::lock_guard lock(mtx_);

    auto iter =
        std::find_if(loan_map_.begin(), loan_map_.end(), [data](const auto& item) { return item.first == data; });
    const bool inserted = iter == loan_map_.end();

    if (inserted) {
      loan_map_.emplace_back(data, z_owned_shm_mut_t{});
      iter = std::prev(loan_map_.end());
      z_internal_null(&iter->second);
    } else if (z_internal_check(iter->second)) {
      VLOG_W("ZenohFactory: Replacing an unfinished Zenoh SHM loan.");
      z_drop(z_move(iter->second));
      z_internal_null(&iter->second);
    }

    z_take(&iter->second, z_move(result.buf));
  }

  return Bytes::loan_internal(data, request_size);
}

bool ZenohShmSupport::release(const Bytes& bytes) {
  if VUNLIKELY (!bytes.is_loaned()) {
    return false;
  }

  std::lock_guard lock(mtx_);
  auto iter = std::find_if(loan_map_.begin(), loan_map_.end(),
                           [&bytes](const auto& item) { return item.first == bytes.data(); });

  if (iter == loan_map_.end()) {
    return false;
  }

  z_drop(z_move(iter->second));
  loan_map_.erase(iter);
  return true;
}

bool ZenohShmSupport::build_payload(z_owned_bytes_t* payload, const Bytes& bytes) {
  if VUNLIKELY (!payload) {
    return false;
  }

  if (bytes.is_loaned()) {
    z_owned_shm_mut_t shm;
    z_internal_null(&shm);

    {
      std::lock_guard lock(mtx_);
      auto iter = std::find_if(loan_map_.begin(), loan_map_.end(),
                               [&bytes](const auto& item) { return item.first == bytes.data(); });

      if (iter != loan_map_.end()) {
        z_take(&shm, z_move(iter->second));
        loan_map_.erase(iter);
      }
    }

    if (z_internal_check(shm)) {
      const z_result_t ret = z_bytes_from_shm_mut(payload, z_move(shm));

      if VUNLIKELY (ret != Z_OK) {
        if (z_internal_check(shm)) {
          z_drop(z_move(shm));
        }

        VLOG_E("ZenohFactory: Failed to build payload from SHM loan, ret=", +ret, ".");
        return false;
      }

      return true;
    }

    static std::atomic_bool warned_foreign_loan{false};

    if (!warned_foreign_loan.exchange(true, std::memory_order_relaxed)) {
      VLOG_W("ZenohFactory: Copying a loaned Bytes buffer that was not created by this Zenoh endpoint.");
    }
  }

  return z_bytes_copy_from_buf(payload, bytes.data(), bytes.size()) == Z_OK;
}
#endif

// ZenohFactory
ZenohFactory::ZenohFactory() {
  Bytes::init_memory_pool();

  if VUNLIKELY (ZenohConf::get_thread_count() != 1) {
    VLOG_W("ZenohFactory: Zenoh does not support setting thread count.");
  }

  for (const auto& [name, qos] : QosProfile::get_available_qos_map()) {
    ZenohConf::register_qos_internal(name, qos);
  }

  init();
}

ZenohFactory::~ZenohFactory() { deinit(); }

void ZenohFactory::init() {
  bool expected = false;

  if VUNLIKELY (!has_inited_.compare_exchange_strong(expected, true)) {
    return;
  }

  std::string default_event_qos_str = Utils::get_env("VLINK_ZENOH_EVENT_QOS");
  std::string default_method_qos_str = Utils::get_env("VLINK_ZENOH_METHOD_QOS");
  std::string default_field_qos_str = Utils::get_env("VLINK_ZENOH_FIELD_QOS");

  if (default_event_qos_str.empty()) {
    default_event_qos_ = QosProfile::kEvent;
  } else {
    default_event_qos_ = ZenohConf::find_qos(default_event_qos_str);
  }

  if (default_method_qos_str.empty()) {
    default_method_qos_ = QosProfile::kMethod;
  } else {
    default_method_qos_ = ZenohConf::find_qos(default_method_qos_str);
  }

  if (default_field_qos_str.empty()) {
    default_field_qos_ = QosProfile::kField;
  } else {
    default_field_qos_ = ZenohConf::find_qos(default_field_qos_str);
  }

  z_result_t ret = Z_EINVAL;

#ifdef VLINK_ENABLE_ZENOH_PICO
  ret = z_config_default(&global_config_);
#else
  ret = zc_init_log_from_env_or("error");

  if VUNLIKELY (ret != Z_OK) {
    VLOG_F("ZenohFactory: Failed to invoke [zc_init_log_from_env_or].");
    return;
  }

  ret = z_config_default(&global_config_);
#endif

  if VUNLIKELY (ret != Z_OK) {
    VLOG_F("ZenohFactory: Failed to invoke [z_config_default].");
    return;
  }

  has_config_.store(true, std::memory_order_release);

#ifndef VLINK_ENABLE_ZENOH_PICO
  std::string zenoh_config_path = Utils::get_env("VLINK_ZENOH_CONFIG");

  if (!zenoh_config_path.empty()) {
    z_owned_config_t file_config;
    ret = zc_config_from_file(&file_config, zenoh_config_path.c_str());

    if VUNLIKELY (ret != Z_OK) {
      if (has_config_.exchange(false, std::memory_order_acq_rel)) {
        z_drop(z_move(global_config_));
      }

      VLOG_F("ZenohFactory: Failed to invoke [zc_config_from_file]. path=", zenoh_config_path, " error=", +ret, ".");
      return;
    }

    z_drop(z_move(global_config_));
    z_take(&global_config_, z_move(file_config));
  }
#endif

  message_loop_.async_run();
}

void ZenohFactory::deinit() {
  bool expected = true;

  if VUNLIKELY (!has_inited_.compare_exchange_strong(expected, false)) {
    return;
  }

  const bool in_loop = message_loop_.is_in_same_thread();

  if (in_loop) {
    cleanup();
  } else if VLIKELY (message_loop_.post_task([this]() { cleanup(); })) {
    message_loop_.wait_for_idle();
  } else {
    cleanup();
  }

  message_loop_.quit();

  if VLIKELY (!in_loop) {
    message_loop_.wait_for_quit();
  }
}

void ZenohFactory::cleanup() {
  std::lock_guard lock(session_mtx_);

  session_map_.clear();

  if (has_config_.exchange(false, std::memory_order_acq_rel)) {
    z_drop(z_move(global_config_));
  }
}

bool ZenohFactory::write_header(const ZenohHeader& header, z_owned_bytes_t* attachment) {
  if VUNLIKELY (!attachment) {
    return false;
  }

  uint8_t buf[sizeof(ZenohHeader)];

  encode_u64be(buf + 0, header.guid);
  encode_u64be(buf + 8, header.timestamp);
  encode_u64be(buf + 16, header.channel);
  encode_u64be(buf + 24, header.seq);

  return z_bytes_copy_from_buf(attachment, buf, sizeof(buf)) == Z_OK;
}

bool ZenohFactory::read_header(ZenohHeader& header, const z_loaned_bytes_t* attachment) {
  if VUNLIKELY (!attachment) {
    return false;
  }

  if VUNLIKELY (z_bytes_len(attachment) != sizeof(ZenohHeader)) {
    return false;
  }

  uint8_t buf[sizeof(ZenohHeader)];
  z_bytes_reader_t reader = z_bytes_get_reader(attachment);

  if VUNLIKELY (z_bytes_reader_read(&reader, buf, sizeof(buf)) != sizeof(buf)) {
    return false;
  }

  header.guid = decode_u64be(buf + 0);
  header.timestamp = decode_u64be(buf + 8);
  header.channel = decode_u64be(buf + 16);
  header.seq = decode_u64be(buf + 24);

  return true;
}

int ZenohFactory::get_default_domain_id() {
  const std::string& domain_str = Utils::get_env("VLINK_ZENOH_DOMAIN");
  return Helpers::to_int(domain_str, 0);
}

z_priority_t ZenohFactory::convert_priority(Qos::Additions::Priority priority) {
  switch (priority) {
    case Qos::Additions::kPriorityRealTime:
      return Z_PRIORITY_REAL_TIME;
    case Qos::Additions::kPriorityHigh:
      return Z_PRIORITY_INTERACTIVE_HIGH;
    case Qos::Additions::kPriorityNormal:
      return Z_PRIORITY_DATA;
    case Qos::Additions::kPriorityLow:
      return Z_PRIORITY_DATA_LOW;
    case Qos::Additions::kPriorityBackground:
      return Z_PRIORITY_BACKGROUND;
    default:
      return Z_PRIORITY_DATA;
  }
}

#if defined(Z_FEATURE_UNSTABLE_API)
z_reliability_t ZenohFactory::convert_reliability(Qos::Reliability::Kind reliability) {
  return reliability == Qos::Reliability::kBestEffort ? Z_RELIABILITY_BEST_EFFORT : Z_RELIABILITY_RELIABLE;
}
#endif

uint64_t ZenohFactory::ntp64_to_ns(uint64_t ntp64) {
  uint64_t sec = ntp64 >> 32;
  uint64_t frac = ntp64 & 0xFFFFFFFF;
  return sec * 1000000000ULL + ((frac * 1000000000ULL) >> 32);
}

ZenohSessionPtr ZenohFactory::get_session(int32_t domain, int32_t depth, const std::string& fragment,
                                          const Conf::PropertiesMap& properties) {
  SessionID session_id = SessionID{domain, depth, fragment, zenoh_session_properties_from(properties)};

  std::lock_guard lock(session_mtx_);

  auto [iter, inserted] = session_map_.emplace(session_id, ZenohSessionPtr());

  if (!inserted) {
    return iter->second;
  }

  auto session = ZenohSessionPtr(new z_owned_session_t(), [](z_owned_session_t* ptr) {
    if (ptr) {
      if (z_internal_check(*ptr)) {
        z_drop(z_move(*ptr));
      }

      delete ptr;
    }
  });
  z_internal_null(session.get());
  iter->second = session;

  static std::string env_mode = Utils::get_env("VLINK_ZENOH_MODE", "peer");
  static std::string env_ip = Utils::get_env("VLINK_ZENOH_IP", "");
  static std::string env_peer = Utils::get_env("VLINK_ZENOH_PEER", "");
  static std::string env_listen = Utils::get_env("VLINK_ZENOH_LISTEN", "");
  static std::string env_multicast = Utils::get_env("VLINK_ZENOH_MULTICAST", "239.255.0.100");
  static std::string env_mcast_if = Utils::get_env("VLINK_ZENOH_MULTICAST_IF", "");
  static bool env_gossip = parse_bool_value(Utils::get_env("VLINK_ZENOH_GOSSIP", "1"), true);
  static std::string env_rx_buf_str = Utils::get_env("VLINK_ZENOH_RX_BUF", "");
  static std::string env_max_msg_str = Utils::get_env("VLINK_ZENOH_MAX_MSG", "");
  static std::string env_mcast_ttl_str = Utils::get_env("VLINK_ZENOH_MULTICAST_TTL", "");
  static std::string env_tx_queue_data_str = Utils::get_env("VLINK_ZENOH_TX_QUEUE_DATA", "");
  static std::string env_tx_queue_rt_str = Utils::get_env("VLINK_ZENOH_TX_QUEUE_RT", "");
  static std::string env_shm_mode = Utils::get_env("VLINK_ZENOH_SHM_MODE", "init");
  static std::string env_shm_size_str = Utils::get_env("VLINK_ZENOH_SHM_SIZE", "");
  static std::string env_shm_threshold_str = Utils::get_env("VLINK_ZENOH_SHM_THRESHOLD", "");
  static bool env_lowlatency = parse_bool_value(Utils::get_env("VLINK_ZENOH_LOWLATENCY", "0"), false);
  static bool env_qos = parse_bool_value(Utils::get_env("VLINK_ZENOH_QOS", "1"), true);
  static bool env_compression = parse_bool_value(Utils::get_env("VLINK_ZENOH_COMPRESSION", "0"), false);
  static bool env_timestamps = parse_bool_value(Utils::get_env("VLINK_ZENOH_TIMESTAMPS", "0"), false);

  [[maybe_unused]] std::string prop_mode = env_mode;
  [[maybe_unused]] std::string prop_ip = env_ip;
  [[maybe_unused]] std::string prop_peer = env_peer;
  [[maybe_unused]] std::string prop_listen = env_listen;
  [[maybe_unused]] std::string prop_rx_buf_str = env_rx_buf_str;
  [[maybe_unused]] std::string prop_max_msg_str = env_max_msg_str;
  [[maybe_unused]] std::string prop_mcast_ttl_str = env_mcast_ttl_str;
  [[maybe_unused]] std::string prop_tx_queue_data_str = env_tx_queue_data_str;
  [[maybe_unused]] std::string prop_tx_queue_rt_str = env_tx_queue_rt_str;

  if (depth > 0) {
    const std::string depth_str = std::to_string(depth);
    if (prop_tx_queue_data_str.empty()) {
      prop_tx_queue_data_str = depth_str;
    }
    if (prop_tx_queue_rt_str.empty()) {
      prop_tx_queue_rt_str = depth_str;
    }
  }

  [[maybe_unused]] bool prop_shm = zenoh_shm_enabled_from(fragment, {});
  [[maybe_unused]] std::string prop_shm_mode = env_shm_mode;
  [[maybe_unused]] std::string prop_shm_size_str = env_shm_size_str;
  [[maybe_unused]] std::string prop_shm_threshold_str = env_shm_threshold_str;
  [[maybe_unused]] bool prop_lowlatency = env_lowlatency;
  [[maybe_unused]] bool prop_qos = env_qos;
  [[maybe_unused]] bool prop_compression = env_compression;
  [[maybe_unused]] bool prop_timestamps = env_timestamps;
  [[maybe_unused]] bool prop_gossip = env_gossip;

  for (const auto& [prop, value] : properties) {
    if (!Helpers::has_startwith(prop, "zenoh.")) {
      continue;
    }

    if (prop == "zenoh.mode") {
      prop_mode = value;
    } else if (prop == "zenoh.ip") {
      prop_ip = value;
    } else if (prop == "zenoh.peer") {
      prop_peer = value;
    } else if (prop == "zenoh.listen") {
      prop_listen = value;
    } else if (prop == "zenoh.rx_buf") {
      prop_rx_buf_str = value;
    } else if (prop == "zenoh.max_msg") {
      prop_max_msg_str = value;
    } else if (prop == "zenoh.multicast_ttl") {
      prop_mcast_ttl_str = value;
    } else if (prop == "zenoh.tx_queue_data") {
      prop_tx_queue_data_str = value;
    } else if (prop == "zenoh.tx_queue_rt") {
      prop_tx_queue_rt_str = value;
    } else if (prop == "zenoh.shm") {
      prop_shm = parse_bool_value(value, prop_shm);
    } else if (prop == "zenoh.shm_mode") {
      prop_shm_mode = value;
    } else if (prop == "zenoh.shm_size") {
      prop_shm_size_str = value;
    } else if (prop == "zenoh.shm_threshold") {
      prop_shm_threshold_str = value;
    } else if (prop == "zenoh.lowlatency") {
      prop_lowlatency = parse_bool_value(value, prop_lowlatency);
    } else if (prop == "zenoh.qos") {
      prop_qos = parse_bool_value(value, prop_qos);
    } else if (prop == "zenoh.compression") {
      prop_compression = parse_bool_value(value, prop_compression);
    } else if (prop == "zenoh.timestamps") {
      prop_timestamps = parse_bool_value(value, prop_timestamps);
    } else if (prop == "zenoh.gossip") {
      prop_gossip = parse_bool_value(value, prop_gossip);
    }
  }

  auto ssl_cfg = SslOptions::parse_from(properties);

  bool ssl_cfg_valid = ssl_cfg.is_valid();
  const bool fragment_is_tcp = (fragment == "tcp" || has_prefix(fragment, "tcp/") || has_prefix(fragment, "tls/"));
  const bool fragment_is_udp = (fragment == "udp" || has_prefix(fragment, "udp/"));
  const bool fragment_is_unix = (fragment == "unix" || has_prefix(fragment, "unixsock-stream/"));

  if (!fragment.empty() && fragment != "tcp" && fragment != "udp" && fragment != "unix" && fragment != "shm") {
    if (fragment_is_tcp || fragment_is_udp) {
      prop_peer = prop_peer.empty() ? fragment : prop_peer;
    } else if (fragment_is_unix) {
      prop_peer = prop_peer.empty() ? fragment : prop_peer;
    }
  }

  if (fragment == "udp") {
    prop_peer = prop_peer.empty() ? "udp/239.255.0.100:7447" : prop_peer;
  } else if (fragment == "tcp") {
    if (prop_listen.empty()) {
      prop_listen = ssl_cfg_valid ? "tls/0.0.0.0:0" : "tcp/0.0.0.0:0";
    }
  } else if (fragment == "unix") {
    if (prop_listen.empty()) {
      const std::string& base = ("/tmp/vlink_" + std::to_string(domain) + ".sock");
      prop_listen = "unixsock-stream/" + base;
    }
  }

  z_owned_config_t config;
  z_config_clone(&config, z_loan(global_config_));

#ifdef VLINK_ENABLE_ZENOH_PICO

  if (ssl_cfg_valid) {
    VLOG_W("ZenohFactory: zenoh-pico does not support TLS, ssl.* properties will be ignored.");
  }

  if (prop_shm) {
    VLOG_W("ZenohFactory: Zenoh SHM requested but zenoh-pico does not support Zenoh SHM APIs.");
  }

  zp_config_insert(z_loan_mut(config), Z_CONFIG_MODE_KEY, prop_mode.c_str());

  if (!prop_ip.empty()) {
    std::string endpoint = fragment_is_tcp ? ("tcp/" + prop_ip + ":7447") : ("udp/" + prop_ip + ":7447");
    zp_config_insert(z_loan_mut(config), Z_CONFIG_CONNECT_KEY, endpoint.c_str());
  }

  if (!prop_peer.empty()) {
    auto peers = Helpers::get_split_string(prop_peer, ';');
    if (!peers.empty()) {
      zp_config_insert(z_loan_mut(config), Z_CONFIG_CONNECT_KEY, peers[0].c_str());
    }
  }

  bool enable_multicast = !(fragment_is_udp || fragment_is_tcp || fragment_is_unix);
  zp_config_insert(z_loan_mut(config), Z_CONFIG_MULTICAST_SCOUTING_KEY, enable_multicast ? "true" : "false");

  if (enable_multicast) {
    std::string mcast_key = "udp/" + env_multicast + ":7446";
    zp_config_insert(z_loan_mut(config), Z_CONFIG_MULTICAST_LOCATOR_KEY, mcast_key.c_str());
  }

  zp_config_insert(z_loan_mut(config), Z_CONFIG_SCOUTING_WHAT_KEY, "2");
#else
  {
    std::string mode_json = "\"" + prop_mode + "\"";
    zc_config_insert_json5(z_loan_mut(config), "mode", mode_json.c_str());
  }

  if (prop_shm) {
#if VLINK_ZENOH_SHM_AVAILABLE
    auto log_failure = [](const char* key, z_result_t rc) {
      if (rc != Z_OK) {
        VLOG_W("ZenohFactory: Failed to set '", key, "', rc=", +rc, ".");
      }
    };

    log_failure("transport/shared_memory/enabled",
                zc_config_insert_json5(z_loan_mut(config), "transport/shared_memory/enabled", "true"));

    if (prop_shm_mode != "lazy" && prop_shm_mode != "init") {
      VLOG_W("ZenohFactory: Invalid zenoh.shm_mode='", prop_shm_mode, "', fallback to 'init'.");
      prop_shm_mode = "init";
    }

    {
      std::string mode_json = "\"" + prop_shm_mode + "\"";
      log_failure("transport/shared_memory/mode",
                  zc_config_insert_json5(z_loan_mut(config), "transport/shared_memory/mode", mode_json.c_str()));
    }

    log_failure(
        "transport/shared_memory/transport_optimization/enabled",
        zc_config_insert_json5(z_loan_mut(config), "transport/shared_memory/transport_optimization/enabled", "true"));

    if (!prop_shm_size_str.empty()) {
      size_t shm_size = 0;

      if (parse_size_value(prop_shm_size_str, &shm_size)) {
        log_failure(
            "transport/shared_memory/transport_optimization/pool_size",
            zc_config_insert_json5(z_loan_mut(config), "transport/shared_memory/transport_optimization/pool_size",
                                   std::to_string(shm_size).c_str()));
      } else {
        VLOG_W("ZenohFactory: Invalid zenoh.shm_size='", prop_shm_size_str, "', using Zenoh default.");
      }
    }

    if (!prop_shm_threshold_str.empty()) {
      size_t threshold = 0;

      if (parse_size_value(prop_shm_threshold_str, &threshold, true)) {
        log_failure("transport/shared_memory/transport_optimization/message_size_threshold",
                    zc_config_insert_json5(z_loan_mut(config),
                                           "transport/shared_memory/transport_optimization/message_size_threshold",
                                           std::to_string(threshold).c_str()));
      } else {
        VLOG_W("ZenohFactory: Invalid zenoh.shm_threshold='", prop_shm_threshold_str, "', using Zenoh default.");
      }
    }
#else
    VLOG_W("ZenohFactory: Zenoh SHM requested but zenoh-c was not built with shared-memory unstable APIs.");
#endif
  }

  if (prop_lowlatency) {
    zc_config_insert_json5(z_loan_mut(config), "transport/unicast/lowlatency", "true");
    zc_config_insert_json5(z_loan_mut(config), "transport/unicast/qos/enabled", "false");
  } else {
    zc_config_insert_json5(z_loan_mut(config), "transport/unicast/qos/enabled", prop_qos ? "true" : "false");
  }

  zc_config_insert_json5(z_loan_mut(config), "transport/unicast/compression/enabled",
                         prop_compression ? "true" : "false");

  if (prop_timestamps) {
    zc_config_insert_json5(z_loan_mut(config), "timestamping/enabled", "{router:true,peer:true,client:true}");
  }

  if (!prop_rx_buf_str.empty()) {
    size_t buf_val = 0;
    std::from_chars(prop_rx_buf_str.data(), prop_rx_buf_str.data() + prop_rx_buf_str.size(), buf_val);
    if (buf_val > 0) {
      std::string buf_json = std::to_string(buf_val);
      zc_config_insert_json5(z_loan_mut(config), "transport/link/rx/buffer_size", buf_json.c_str());
    }
  }

  if (!prop_max_msg_str.empty()) {
    size_t max_msg_val = 0;
    std::from_chars(prop_max_msg_str.data(), prop_max_msg_str.data() + prop_max_msg_str.size(), max_msg_val);
    if (max_msg_val > 0) {
      std::string max_msg_json = std::to_string(max_msg_val);
      zc_config_insert_json5(z_loan_mut(config), "transport/link/rx/max_message_size", max_msg_json.c_str());
    }
  }

  if (!prop_tx_queue_data_str.empty()) {
    if (const auto val = parse_zenoh_queue_depth(prop_tx_queue_data_str, "zenoh.tx_queue_data")) {
      zc_config_insert_json5(z_loan_mut(config), "transport/link/tx/queue/size/data", std::to_string(*val).c_str());
    }
  }

  if (!prop_tx_queue_rt_str.empty()) {
    if (const auto val = parse_zenoh_queue_depth(prop_tx_queue_rt_str, "zenoh.tx_queue_rt")) {
      zc_config_insert_json5(z_loan_mut(config), "transport/link/tx/queue/size/real_time",
                             std::to_string(*val).c_str());
    }
  }

  bool enable_multicast = !(fragment_is_udp || fragment_is_tcp || fragment_is_unix);
  zc_config_insert_json5(z_loan_mut(config), "scouting/multicast/enabled", enable_multicast ? "true" : "false");

  if (enable_multicast) {
    std::string mcast_addr = "\"" + env_multicast + ":7446\"";
    zc_config_insert_json5(z_loan_mut(config), "scouting/multicast/address", mcast_addr.c_str());

    if (!prop_mcast_ttl_str.empty()) {
      size_t ttl_val = 0;

      std::from_chars(prop_mcast_ttl_str.data(), prop_mcast_ttl_str.data() + prop_mcast_ttl_str.size(), ttl_val);

      if (ttl_val > 0) {
        zc_config_insert_json5(z_loan_mut(config), "scouting/multicast/ttl", std::to_string(ttl_val).c_str());
      }
    }

    std::string mcast_if = env_mcast_if;

    if (mcast_if.empty() && !prop_ip.empty()) {
      auto ips = Helpers::get_split_string(prop_ip, ';');

      if (!ips.empty()) {
        const auto& first_ip = ips[0];

        if (first_ip.find(':') != std::string::npos) {
          mcast_if = Utils::get_interface_name_by_ipv6(first_ip);
        } else {
          mcast_if = Utils::get_interface_name_by_ipv4(first_ip);
        }
      }
    }

    if (!mcast_if.empty()) {
      std::string mcast_if_json = "\"" + mcast_if + "\"";
      zc_config_insert_json5(z_loan_mut(config), "scouting/multicast/interface", mcast_if_json.c_str());
    }
  }

  zc_config_insert_json5(z_loan_mut(config), "scouting/gossip/enabled", prop_gossip ? "true" : "false");

  if (!prop_peer.empty()) {
    auto peers = Helpers::get_split_string(prop_peer, ';');
    std::string peers_json = "[";

    for (size_t i = 0; i < peers.size(); ++i) {
      if (i > 0) {
        peers_json += ",";
      }

      peers_json += "\"" + peers[i] + "\"";
    }

    peers_json += "]";
    zc_config_insert_json5(z_loan_mut(config), "connect/endpoints", peers_json.c_str());
  } else if (!prop_ip.empty()) {
    auto ips = Helpers::get_split_string(prop_ip, ';');
    std::string peers_json = "[";

    for (size_t i = 0; i < ips.size(); ++i) {
      if (i > 0) {
        peers_json += ",";
      }

      // NOLINTNEXTLINE(readability-avoid-nested-conditional-operator)
      std::string proto = fragment_is_tcp ? (ssl_cfg_valid ? "tls" : "tcp") : "udp";
      peers_json += "\"" + proto + "/" + ips[i] + ":7447\"";
    }

    peers_json += "]";
    zc_config_insert_json5(z_loan_mut(config), "connect/endpoints", peers_json.c_str());
  }

  if (!prop_listen.empty()) {
    auto listens = Helpers::get_split_string(prop_listen, ';');
    std::string listen_json = "[";

    for (size_t i = 0; i < listens.size(); ++i) {
      if (i > 0) {
        listen_json += ",";
      }

      listen_json += "\"" + listens[i] + "\"";
    }

    listen_json += "]";
    zc_config_insert_json5(z_loan_mut(config), "listen/endpoints", listen_json.c_str());
  }

  {
    static auto batch_time_env = Utils::get_env("VLINK_ZENOH_BATCH_TIME_LIMIT_MS", "1");
    static auto batch_enabled_env = Utils::get_env("VLINK_ZENOH_BATCH_ENABLED", "true");

    auto log_failure = [](const char* key, z_result_t rc) {
      if (rc != Z_OK) {
        VLOG_W("ZenohFactory: Failed to set '", key, "', rc=", rc, ".");
      }
    };

    log_failure("transport/link/tx/queue/batching/time_limit",
                zc_config_insert_json5(z_loan_mut(config), "transport/link/tx/queue/batching/time_limit",
                                       batch_time_env.c_str()));
    log_failure("transport/link/tx/queue/batching/enabled",
                zc_config_insert_json5(z_loan_mut(config), "transport/link/tx/queue/batching/enabled",
                                       batch_enabled_env.c_str()));
  }

  if (fragment_is_unix) {
    zc_config_insert_json5(z_loan_mut(config), "scouting/gossip/enabled", "false");
  }

  if (ssl_cfg_valid) {
    if (!ssl_cfg.ca_file.empty()) {
      std::string ca_json = "\"" + ssl_cfg.ca_file + "\"";
      zc_config_insert_json5(z_loan_mut(config), "transport/link/tls/root_ca_certificate", ca_json.c_str());
    }

    if (!ssl_cfg.cert_file.empty()) {
      std::string cert_json = "\"" + ssl_cfg.cert_file + "\"";
      zc_config_insert_json5(z_loan_mut(config), "transport/link/tls/client_certificate", cert_json.c_str());
    }

    if (!ssl_cfg.key_file.empty()) {
      std::string key_json = "\"" + ssl_cfg.key_file + "\"";
      zc_config_insert_json5(z_loan_mut(config), "transport/link/tls/client_private_key", key_json.c_str());
    }

    if (!ssl_cfg.server_name.empty()) {
      std::string sni_json = "\"" + ssl_cfg.server_name + "\"";
      zc_config_insert_json5(z_loan_mut(config), "transport/link/tls/server_name_verification", sni_json.c_str());
    }
  }

#endif

  z_result_t ret = z_open(session.get(), z_move(config), nullptr);

  if VUNLIKELY (ret != Z_OK) {
    session_map_.erase(iter);

    VLOG_E(
        "ZenohFactory: Failed to invoke [z_open] session,"
        " domain=",
        domain, " fragment=", fragment, " error=", +ret, ".");

    return nullptr;
  }

  return session;
}

const Qos& ZenohFactory::find_qos(uint8_t impl_type, const std::string& name) {
  if (name.empty()) {
    if ((impl_type & kPublisher) || (impl_type & kSubscriber)) {
      return default_event_qos_;
    } else if ((impl_type & kServer) || (impl_type & kClient)) {
      return default_method_qos_;
    } else if ((impl_type & kSetter) || (impl_type & kGetter)) {
      return default_field_qos_;
    } else {
      VLOG_E("ZenohFactory: Found invalid qos.");
      static Qos invalid_qos;
      return invalid_qos;
    }
  } else {
    return ZenohConf::find_qos(name);
  }
}

MessageLoop& ZenohFactory::get_message_loop() { return message_loop_; }

// ZenohServer
ZenohServer::ZenohServer(const ZenohID& id) {
  z_internal_null(&liveliness_token_);
  z_internal_null(&server_);

  static auto& factory = ZenohFactory::get();

  const auto& [impl_type, address, domain, depth, qos, fragment, properties] = id;

#if VLINK_ZENOH_SHM_AVAILABLE
  shm_.configure(zenoh_shm_enabled_from(fragment, properties), zenoh_shm_blocking_from(properties),
                 zenoh_shm_loan_threshold_from(properties));
#endif

  topic_ = address;

  if (domain != 0) {
    topic_ += ("_" + std::to_string(domain));
  }

  guid_ = std::hash<std::string>{}(topic_) ^ (reinterpret_cast<uint64_t>(this) << 32);

  z_queryable_options_default(&options_);

  const auto& target_qos = factory.find_qos(impl_type, qos);

  options_.complete = true;
  congestion_control_ = target_qos.reliability.kind == Qos::Reliability::kReliable ? Z_CONGESTION_CONTROL_BLOCK
                                                                                   : Z_CONGESTION_CONTROL_DROP;
  priority_ = ZenohFactory::convert_priority(target_qos.additions.priority);
  is_express_ = target_qos.additions.is_express;

  session_ = factory.get_session(domain, zenoh_depth_from_qos(depth, target_qos), fragment, properties);

  if VUNLIKELY (!session_ || !z_internal_check(*session_)) {
    VLOG_E("ZenohFactory: Server session is invalid.");
    return;
  }

  z_result_t ret = Z_EINVAL;

  ret = z_view_keyexpr_from_str(&keyexpr_, topic_.c_str());

  if VUNLIKELY (ret != Z_OK) {
    VLOG_F("ZenohFactory: Failed to invoke server [z_view_keyexpr_from_str].");
    return;
  }

  z_owned_closure_query_t closure;
  z_closure_query(&closure, ZenohServer::on_data_callback, nullptr, this);

  ret = z_declare_queryable(z_loan(*session_), &server_, z_loan(keyexpr_), z_move(closure), &options_);

  if VUNLIKELY (ret != Z_OK) {
    VLOG_F("ZenohFactory: Failed to invoke [z_declare_queryable].");
    return;
  }

  {
    liveliness_key_ = topic_ + "@alive";

    ret = z_view_keyexpr_from_str(&liveliness_keyexpr_, liveliness_key_.c_str());

    if VUNLIKELY (ret != Z_OK) {
      VLOG_E("ZenohFactory: Failed to invoke server liveliness [z_view_keyexpr_from_str].");
      return;
    }

    z_liveliness_token_options_t liveliness_opts;
    z_liveliness_token_options_default(&liveliness_opts);

    ret = z_liveliness_declare_token(z_loan(*session_), &liveliness_token_, z_loan(liveliness_keyexpr_),
                                     &liveliness_opts);

    if VUNLIKELY (ret != Z_OK) {
      VLOG_E("ZenohFactory: Failed to invoke [z_liveliness_declare_token].");
      return;
    }
  }
}

ZenohServer::~ZenohServer() {
  is_suspend_ = true;
  z_drop(z_move(liveliness_token_));
  z_drop(z_move(server_));

  {
    std::lock_guard lock(query_mtx_);

    for (auto& [req_id, query] : query_map_) {
      z_drop(z_move(query));
    }

    query_map_.clear();
  }
}

std::any ZenohServer::get_native_handle() const { return this; }

bool ZenohServer::suspend() {
  is_suspend_ = true;
  return true;
}

bool ZenohServer::resume() {
  is_suspend_ = false;
  return true;
}

bool ZenohServer::is_suspend() const { return is_suspend_; }

// NOLINTNEXTLINE(readability-convert-member-functions-to-static)
bool ZenohServer::is_support_loan() const {
#if VLINK_ZENOH_SHM_AVAILABLE
  return shm_.is_support_loan(session_);
#else
  return false;
#endif
}

// NOLINTNEXTLINE(readability-convert-member-functions-to-static)
Bytes ZenohServer::loan(uint64_t channel, int64_t size) {
  (void)channel;

#if VLINK_ZENOH_SHM_AVAILABLE
  return shm_.loan(session_, size);
#else
  (void)size;
  return Bytes();
#endif
}

// NOLINTNEXTLINE(readability-convert-member-functions-to-static)
bool ZenohServer::release(const Bytes& bytes) {
#if VLINK_ZENOH_SHM_AVAILABLE
  return shm_.release(bytes);
#else
  (void)bytes;
  return false;
#endif
}

// NOLINTNEXTLINE(readability-convert-member-functions-to-static)
bool ZenohServer::build_payload(z_owned_bytes_t* payload, const Bytes& bytes) {
#if VLINK_ZENOH_SHM_AVAILABLE
  return shm_.build_payload(payload, bytes);
#else
  return z_bytes_copy_from_buf(payload, bytes.data(), bytes.size()) == Z_OK;
#endif
}

void ZenohServer::drop_query(uint64_t req_id) {
  std::lock_guard lock(query_mtx_);
  auto iter = query_map_.find(req_id);

  if VLIKELY (iter != query_map_.end()) {
    z_drop(z_move(iter->second));
    query_map_.erase(iter);
  }
}

bool ZenohServer::reply(uint64_t channel, uint64_t req_id, const Bytes& resp_data) {
  ZenohHeader header{guid_, 0, channel, req_id};
  z_owned_bytes_t attachment;

  if VUNLIKELY (!ZenohFactory::write_header(header, &attachment)) {
    VLOG_E("ZenohFactory: Failed to build reply attachment header.");
    drop_query(req_id);
    return false;
  }

  z_owned_bytes_t payload;

  if VUNLIKELY (!build_payload(&payload, resp_data)) {
    VLOG_E("ZenohFactory: Failed to build reply payload.");
    z_drop(z_move(attachment));
    drop_query(req_id);
    return false;
  }

  z_owned_query_t query;
  z_internal_null(&query);

  {
    std::lock_guard lock(query_mtx_);
    auto iter = query_map_.find(req_id);

    if VUNLIKELY (iter == query_map_.end()) {
      VLOG_E("ZenohFactory: Cannot find query.");
      z_drop(z_move(attachment));
      z_drop(z_move(payload));
      return false;
    }

    z_take(&query, z_move(iter->second));
    query_map_.erase(iter);
  }

  z_query_reply_options_t reply_options;
  z_query_reply_options_default(&reply_options);
  reply_options.congestion_control = congestion_control_;
  reply_options.priority = priority_;
  reply_options.is_express = is_express_;
  reply_options.attachment = z_move(attachment);

  z_result_t ret = z_query_reply(z_loan(query), z_loan(keyexpr_), z_move(payload), &reply_options);
  z_drop(z_move(query));

  return ret == Z_OK;
}

void ZenohServer::process_message(uint64_t channel, uint64_t seq, MessageLoop* message_loop, Bytes&& req_bytes) {
  if (message_loop) {
    auto weak_self = weak_from_this();
    auto task = [weak_self, channel, seq, req_bytes = std::move(req_bytes)]() mutable {
      auto self = weak_self.lock();

      if VUNLIKELY (!self || !ZenohFactory::get().has_object(self.get())) {
        return;
      }

      auto* first_impl = self->get_first_impl();

      if VUNLIKELY (!first_impl || !first_impl->get_message_loop()) {
        self->drop_query(seq);
        return;
      }

      bool is_deferred = false;

      self->traverse_req_resp_callback(
          [self, channel, seq, &req_bytes, &is_deferred](NodeImpl* impl, const auto& callback) {
            const auto* conf_ptr = impl->get_target_conf<ZenohConf>();

            if (static_cast<uint64_t>(conf_ptr->hash_code) != channel) {
              self->ignore_called();
              return;
            }

            if VUNLIKELY (self->has_called()) {
              VLOG_F(*conf_ptr, "Two identical service requests.");
              return;
            }

            if (static_cast<ServerImpl*>(impl)->is_resp_type) {
              Bytes resp_bytes;

              callback(seq, req_bytes, &resp_bytes);
              is_deferred = !static_cast<ServerImpl*>(impl)->is_sync_type;
            } else {
              callback(seq, req_bytes, nullptr);
              is_deferred = false;
            }
          });

      if VLIKELY (!is_deferred) {
        self->drop_query(seq);
      }
    };

    if VUNLIKELY (!message_loop->post_task(std::move(task))) {
      drop_query(seq);
    }
  } else {
    bool is_deferred = false;

    traverse_req_resp_callback([this, channel, seq, &req_bytes, &is_deferred](NodeImpl* impl, const auto& callback) {
      const auto* conf_ptr = impl->get_target_conf<ZenohConf>();

      if (static_cast<uint64_t>(conf_ptr->hash_code) != channel) {
        ignore_called();
        return;
      }

      if VUNLIKELY (has_called()) {
        VLOG_F(*conf_ptr, "Two identical service requests.");
        return;
      }

      std::lock_guard lock(mtx_);

      if (static_cast<ServerImpl*>(impl)->is_resp_type) {
        Bytes resp_bytes;

        callback(seq, req_bytes, &resp_bytes);
        is_deferred = !static_cast<ServerImpl*>(impl)->is_sync_type;
      } else {
        callback(seq, req_bytes, nullptr);
        is_deferred = false;
      }
    });

    if VLIKELY (!is_deferred) {
      drop_query(seq);
    }
  }
}

void ZenohServer::on_data_callback(z_loaned_query_t* query, void* context) {
  auto* instance = static_cast<ZenohServer*>(context);

  if VUNLIKELY (!ZenohFactory::get().has_object(instance)) {
    return;
  }

  if VUNLIKELY (instance->is_suspend_) {
    return;
  }

  ZenohHeader header;

  if VUNLIKELY (!ZenohFactory::read_header(header, z_query_attachment(query))) {
    VLOG_E("ZenohFactory: Failed to read server header.");
    return;
  }

  ZenohPayloadView payload;

  if VUNLIKELY (!payload.load(z_query_payload(query))) {
    VLOG_E("ZenohFactory: Failed to read server payload.");
    return;
  }

  auto* first_impl = instance->get_first_impl();
  auto* message_loop = first_impl ? first_impl->get_message_loop() : nullptr;
  Bytes req_bytes = Bytes::deep_copy(payload.data, payload.size);
  z_owned_query_t kept_query;

  z_internal_null(&kept_query);

  if VUNLIKELY (!keep_query_for_deferred_reply(&kept_query, query)) {
    VLOG_E("ZenohFactory: Failed to keep query for deferred reply.");

    return;
  }

  {
    std::lock_guard lock(instance->query_mtx_);
    auto [iter, inserted] = instance->query_map_.emplace(header.seq, z_owned_query_t());

    if VUNLIKELY (!inserted) {
      VLOG_W("ZenohFactory: Duplicate query sequence received, replacing old query. seq=", header.seq, ".");
      z_drop(z_move(iter->second));
    }

    z_take(&iter->second, z_move(kept_query));
  }

  instance->process_message(header.channel, header.seq, message_loop, std::move(req_bytes));
}

// ZenohClient
ZenohClient::ZenohClient(const ZenohID& id) {
  z_internal_null(&liveliness_sub_);

  static auto& factory = ZenohFactory::get();

  const auto& [impl_type, address, domain, depth, qos, fragment, properties] = id;

#if VLINK_ZENOH_SHM_AVAILABLE
  shm_.configure(zenoh_shm_enabled_from(fragment, properties), zenoh_shm_blocking_from(properties),
                 zenoh_shm_loan_threshold_from(properties));
#endif

  topic_ = address;

  if (domain != 0) {
    topic_ += ("_" + std::to_string(domain));
  }

  guid_ = std::hash<std::string>{}(topic_) ^ (reinterpret_cast<uint64_t>(this) << 32);

  const auto& target_qos = factory.find_qos(impl_type, qos);

  session_ = factory.get_session(domain, zenoh_depth_from_qos(depth, target_qos), fragment, properties);

  target_ = Z_QUERY_TARGET_DEFAULT;
  congestion_control_ = target_qos.reliability.kind == Qos::Reliability::kReliable ? Z_CONGESTION_CONTROL_BLOCK
                                                                                   : Z_CONGESTION_CONTROL_DROP;
  priority_ = ZenohFactory::convert_priority(target_qos.additions.priority);
  is_express_ = target_qos.additions.is_express;

  z_result_t ret = Z_EINVAL;

  ret = z_view_keyexpr_from_str(&keyexpr_, topic_.c_str());

  if VUNLIKELY (ret != Z_OK) {
    VLOG_F("ZenohFactory: Failed to invoke client [z_view_keyexpr_from_str].");
    return;
  }

  {
    if VUNLIKELY (!session_ || !z_internal_check(*session_)) {
      VLOG_E("ZenohFactory: Client session is invalid.");
      return;
    }

    liveliness_key_ = topic_ + "@alive";

    ret = z_view_keyexpr_from_str(&liveliness_keyexpr_, liveliness_key_.c_str());

    if VUNLIKELY (ret != Z_OK) {
      VLOG_E("ZenohFactory: Failed to invoke client liveliness [z_view_keyexpr_from_str].");
      return;
    }

    z_owned_closure_sample_t liveliness_closure;

    z_closure(&liveliness_closure, ZenohClient::on_liveliness_change, nullptr, this);

    z_liveliness_subscriber_options_t liveliness_options;

    z_liveliness_subscriber_options_default(&liveliness_options);

    z_result_t live_ret =
        z_liveliness_declare_subscriber(z_loan(*session_), &liveliness_sub_, z_loan(liveliness_keyexpr_),
                                        z_move(liveliness_closure), &liveliness_options);

    if VUNLIKELY (live_ret != Z_OK) {
      VLOG_E("ZenohFactory: Failed to invoke [z_liveliness_declare_subscriber].");
      return;
    }

    check_online();
  }
}

ZenohClient::~ZenohClient() { z_drop(z_move(liveliness_sub_)); }

std::any ZenohClient::get_native_handle() const { return this; }

bool ZenohClient::is_connected() const { return has_connected_.load(std::memory_order_acquire); }

// NOLINTNEXTLINE(readability-convert-member-functions-to-static)
bool ZenohClient::is_support_loan() const {
#if VLINK_ZENOH_SHM_AVAILABLE
  return shm_.is_support_loan(session_);
#else
  return false;
#endif
}

// NOLINTNEXTLINE(readability-convert-member-functions-to-static)
Bytes ZenohClient::loan(uint64_t channel, int64_t size) {
  (void)channel;

#if VLINK_ZENOH_SHM_AVAILABLE
  return shm_.loan(session_, size);
#else
  (void)size;
  return Bytes();
#endif
}

// NOLINTNEXTLINE(readability-convert-member-functions-to-static)
bool ZenohClient::release(const Bytes& bytes) {
#if VLINK_ZENOH_SHM_AVAILABLE
  return shm_.release(bytes);
#else
  (void)bytes;
  return false;
#endif
}

// NOLINTNEXTLINE(readability-convert-member-functions-to-static)
bool ZenohClient::build_payload(z_owned_bytes_t* payload, const Bytes& bytes) {
#if VLINK_ZENOH_SHM_AVAILABLE
  return shm_.build_payload(payload, bytes);
#else
  return z_bytes_copy_from_buf(payload, bytes.data(), bytes.size()) == Z_OK;
#endif
}

bool ZenohClient::call(NodeImpl* owner, uint64_t channel, const Bytes& req_data, NodeImpl::MsgCallback&& callback,
                       int timeout_ms) {
  if VUNLIKELY (!is_connected()) {
    return false;
  }

  z_owned_bytes_t payload;

  uint64_t seq_guid = Helpers::hash_combine(guid_, ++seq_);

  ZenohHeader header{guid_, 0, channel, seq_guid};

  z_owned_bytes_t attachment;

  if VUNLIKELY (!ZenohFactory::write_header(header, &attachment)) {
    VLOG_E("ZenohFactory: Failed to build client attachment header.");
    return false;
  }

  if VUNLIKELY (!build_payload(&payload, req_data)) {
    VLOG_E("ZenohFactory: Failed to build client payload.");
    z_drop(z_move(attachment));
    return false;
  }

  z_get_options_t opts;
  z_get_options_default(&opts);
  opts.target = target_;
  opts.congestion_control = congestion_control_;
  opts.priority = priority_;
  opts.is_express = is_express_;
  opts.attachment = z_move(attachment);
  opts.payload = z_move(payload);

  if (timeout_ms < 0) {
    opts.timeout_ms = std::numeric_limits<uint64_t>::max();
  } else {
    opts.timeout_ms = static_cast<uint64_t>(timeout_ms);
  }

  z_owned_closure_reply_t reply_closure;
  std::shared_ptr<ZenohReplyContext> reply_context;
  std::shared_ptr<ZenohReplyContext>* reply_context_holder = nullptr;
  const bool has_callback = static_cast<bool>(callback);

  if VLIKELY (has_callback) {
    reply_context = std::make_shared<ZenohReplyContext>();
    reply_context->instance = weak_from_this();
    reply_context->seq = seq_guid;
    reply_context_holder = new std::shared_ptr<ZenohReplyContext>(reply_context);

    z_closure_reply(&reply_closure, ZenohClient::on_data_callback, ZenohClient::on_reply_drop, reply_context_holder);

    std::lock_guard lock(mtx_);

    callbacks_[seq_guid] =
        ResponseCallback{owner, [callback = std::move(callback), channel](uint64_t target_channel, const Bytes& bytes) {
                           if (channel != target_channel) {
                             return;
                           }

                           callback(bytes);
                         }};
  } else {
    z_closure_reply(&reply_closure, nullptr, nullptr, nullptr);
  }

  z_result_t ret = z_get(z_loan(*session_), z_loan(keyexpr_), "", z_move(reply_closure), &opts);

  if VUNLIKELY (ret != Z_OK) {
    if VLIKELY (has_callback) {
      std::lock_guard lock(mtx_);
      callbacks_.erase(seq_guid);
    }

    if (reply_context && !reply_context->dropped.exchange(true, std::memory_order_acq_rel)) {
      delete reply_context_holder;
    }

    return false;
  }

  return true;
}

void ZenohClient::cancel_calls(NodeImpl* owner) {
  std::lock_guard lock(mtx_);

  for (auto iter = callbacks_.begin(); iter != callbacks_.end();) {
    if (iter->second.owner == owner) {
      iter = callbacks_.erase(iter);
    } else {
      ++iter;
    }
  }
}

void ZenohClient::check_online() {
  if VUNLIKELY (!session_ || !z_internal_check(*session_)) {
    return;
  }

  z_result_t ret = Z_EINVAL;

  z_owned_fifo_handler_reply_t liveliness_get_handler;

  z_owned_closure_reply_t liveliness_get_closure;

  z_fifo_channel_reply_new(&liveliness_get_closure, &liveliness_get_handler, 4);

  z_liveliness_get_options_t liveliness_get_options;

  z_liveliness_get_options_default(&liveliness_get_options);

  ret = z_liveliness_get(z_loan(*session_), z_loan(liveliness_keyexpr_), z_move(liveliness_get_closure),
                         &liveliness_get_options);

  if (ret == Z_OK) {
    z_owned_reply_t reply;

    if (z_try_recv(z_loan(liveliness_get_handler), &reply) == Z_OK) {
      if (z_reply_is_ok(z_loan(reply))) {
        has_connected_.store(true, std::memory_order_release);
      }

      z_drop(z_move(reply));
    }
  }

  z_drop(z_move(liveliness_get_handler));
}

void ZenohClient::on_data_callback(z_loaned_reply_t* reply, void* context) {
  auto* context_holder = static_cast<std::shared_ptr<ZenohReplyContext>*>(context);

  if VUNLIKELY (!context_holder || !*context_holder) {
    return;
  }

  auto reply_context = *context_holder;
  auto instance = reply_context->instance.lock();

  if VUNLIKELY (!instance || !ZenohFactory::get().has_object(instance.get())) {
    return;
  }

  if VUNLIKELY (!z_reply_is_ok(reply)) {
    VLOG_E("ZenohFactory: Client received an error reply.");
    return;
  }

  const z_loaned_sample_t* sample = z_reply_ok(reply);

  ZenohHeader header;

  if VUNLIKELY (!ZenohFactory::read_header(header, z_sample_attachment(sample))) {
    VLOG_E("ZenohFactory: Failed to read client header.");
    return;
  }

  ZenohPayloadView payload;

  if VUNLIKELY (!payload.load(z_sample_payload(sample))) {
    VLOG_E("ZenohFactory: Failed to read client payload.");
    return;
  }

  Bytes resp_bytes = Bytes::shallow_copy(payload.data, payload.size);

  Function<void(uint64_t, const Bytes&)> callback;
  NodeImpl* owner = nullptr;

  {
    std::lock_guard lock(instance->mtx_);

    auto iter = instance->callbacks_.find(header.seq);

    if (iter != instance->callbacks_.end()) {
      owner = iter->second.owner;
      callback = std::move(iter->second.callback);
      instance->callbacks_.erase(iter);
    }
  }

  if (callback && instance->is_contains_impl(owner)) {
    callback(header.channel, resp_bytes);
  }
}

void ZenohClient::on_reply_drop(void* context) {
  auto* context_holder = static_cast<std::shared_ptr<ZenohReplyContext>*>(context);

  if VUNLIKELY (!context_holder || !*context_holder) {
    delete context_holder;
    return;
  }

  auto reply_context = *context_holder;

  if (!reply_context->dropped.exchange(true, std::memory_order_acq_rel)) {
    auto instance = reply_context->instance.lock();

    if (instance && ZenohFactory::get().has_object(instance.get())) {
      std::lock_guard lock(instance->mtx_);
      instance->callbacks_.erase(reply_context->seq);
    }
  }

  delete context_holder;
}

void ZenohClient::on_liveliness_change(z_loaned_sample_t* sample, void* context) {
  auto* instance = static_cast<ZenohClient*>(context);

  if VUNLIKELY (!ZenohFactory::get().has_object(instance)) {
    return;
  }

  z_sample_kind_t kind = z_sample_kind(sample);

  bool connected = (kind == Z_SAMPLE_KIND_PUT);

  instance->has_connected_.store(connected, std::memory_order_release);
  instance->traverse_server_connect_callback([connected](NodeImpl*, const auto& callback) { callback(connected); });
}

// ZenohPublisher
ZenohPublisher::ZenohPublisher(const ZenohID& id) {
  z_internal_null(&pub_);
  z_internal_null(&matching_listener_);

  static auto& factory = ZenohFactory::get();

  const auto& [impl_type, address, domain, depth, qos, fragment, properties] = id;

#if VLINK_ZENOH_SHM_AVAILABLE
  shm_.configure(zenoh_shm_enabled_from(fragment, properties), zenoh_shm_blocking_from(properties),
                 zenoh_shm_loan_threshold_from(properties));
#endif

  topic_ = address;

  if (domain != 0) {
    topic_ += ("_" + std::to_string(domain));
  }

  guid_ = std::hash<std::string>{}(topic_) ^ (reinterpret_cast<uint64_t>(this) << 32);

  z_publisher_options_default(&options_);

  const auto& target_qos = factory.find_qos(impl_type, qos);

  if (target_qos.reliability.kind == Qos::Reliability::kReliable) {
    options_.congestion_control = Z_CONGESTION_CONTROL_BLOCK;
  } else {
    options_.congestion_control = Z_CONGESTION_CONTROL_DROP;
  }

  options_.priority = ZenohFactory::convert_priority(target_qos.additions.priority);
  options_.is_express = target_qos.additions.is_express;
#if defined(Z_FEATURE_UNSTABLE_API)
  options_.reliability = ZenohFactory::convert_reliability(target_qos.reliability.kind);
#endif

  session_ = factory.get_session(domain, zenoh_depth_from_qos(depth, target_qos), fragment, properties);

  if VUNLIKELY (!session_ || !z_internal_check(*session_)) {
    VLOG_E("ZenohFactory: Publisher session is invalid.");
    return;
  }

  z_result_t ret = Z_EINVAL;

  ret = z_view_keyexpr_from_str(&keyexpr_, topic_.c_str());

  if VUNLIKELY (ret != Z_OK) {
    VLOG_F("ZenohFactory: Failed to invoke publisher [z_view_keyexpr_from_str].");
    return;
  }

  ret = z_declare_publisher(z_loan(*session_), &pub_, z_loan(keyexpr_), &options_);

  if VUNLIKELY (ret != Z_OK) {
    VLOG_F("ZenohFactory: Failed to invoke [z_declare_publisher].");
    return;
  }

  z_owned_closure_matching_status_t closure;
  z_closure(&closure, on_matching_status, nullptr, this);

  z_publisher_declare_matching_listener(z_loan(pub_), &matching_listener_, z_move(closure));
}

ZenohPublisher::~ZenohPublisher() {
  quit_flag_ = true;

  z_drop(z_move(matching_listener_));
  z_drop(z_move(pub_));
}

std::any ZenohPublisher::get_native_handle() const { return this; }

void ZenohPublisher::check_matching() {
  if VUNLIKELY (!z_internal_check(pub_)) {
    return;
  }

  z_matching_status_t status;

  if (z_publisher_get_matching_status(z_loan(pub_), &status) == Z_OK && status.matching) {
    has_subscribers_.store(true, std::memory_order_release);
  }
}

bool ZenohPublisher::has_subscribers() const { return has_subscribers_.load(std::memory_order_acquire); }

// NOLINTNEXTLINE(readability-convert-member-functions-to-static)
bool ZenohPublisher::is_support_loan() const {
#if VLINK_ZENOH_SHM_AVAILABLE
  return shm_.is_support_loan(session_);
#else
  return false;
#endif
}

// NOLINTNEXTLINE(readability-convert-member-functions-to-static)
Bytes ZenohPublisher::loan(uint64_t channel, int64_t size) {
  (void)channel;

#if VLINK_ZENOH_SHM_AVAILABLE
  return shm_.loan(session_, size);
#else
  (void)size;
  return Bytes();
#endif
}

// NOLINTNEXTLINE(readability-convert-member-functions-to-static)
bool ZenohPublisher::release(const Bytes& bytes) {
#if VLINK_ZENOH_SHM_AVAILABLE
  return shm_.release(bytes);
#else
  (void)bytes;
  return false;
#endif
}

// NOLINTNEXTLINE(readability-convert-member-functions-to-static)
bool ZenohPublisher::build_payload(z_owned_bytes_t* payload, const Bytes& bytes) {
#if VLINK_ZENOH_SHM_AVAILABLE
  return shm_.build_payload(payload, bytes);
#else
  return z_bytes_copy_from_buf(payload, bytes.data(), bytes.size()) == Z_OK;
#endif
}

bool ZenohPublisher::publish(uint64_t channel, const Bytes& bytes) {
  if VUNLIKELY (!z_internal_check(pub_)) {
    return false;
  }

  z_result_t ret = Z_EINVAL;

  z_publisher_put_options_t options;
  z_publisher_put_options_default(&options);

  ZenohHeader header{guid_, ElapsedTimer::get_sys_timestamp(ElapsedTimer::kNano, false), channel, ++seq_};

  z_owned_bytes_t attachment;

  if VUNLIKELY (!ZenohFactory::write_header(header, &attachment)) {
    VLOG_E("ZenohFactory: Failed to build publisher attachment header.");
    return false;
  }

  z_owned_bytes_t payload;

  if VUNLIKELY (!build_payload(&payload, bytes)) {
    VLOG_E("ZenohFactory: Failed to build publisher payload.");
    z_drop(z_move(attachment));
    return false;
  }

  options.attachment = z_move(attachment);

  ret = z_publisher_put(z_loan(pub_), z_move(payload), &options);

  return ret == Z_OK;
}

void ZenohPublisher::on_matching_status(const z_matching_status_t* status, void* context) {
  auto* instance = static_cast<ZenohPublisher*>(context);

  if VUNLIKELY (!ZenohFactory::get().has_object(instance)) {
    return;
  }

  if VUNLIKELY (instance->quit_flag_) {
    return;
  }

  bool connected = status->matching;

  instance->has_subscribers_.store(connected, std::memory_order_release);
  instance->traverse_sub_connect_callback([connected](NodeImpl*, const auto& callback) { callback(connected); });
}

// ZenohSubscriber
ZenohSubscriber::ZenohSubscriber(const ZenohID& id) {
  z_internal_null(&sub_);

  static auto& factory = ZenohFactory::get();

  const auto& [impl_type, address, domain, depth, qos, fragment, properties] = id;

  topic_ = address;

  if (domain != 0) {
    topic_ += ("_" + std::to_string(domain));
  }

  guid_ = std::hash<std::string>{}(topic_) ^ (reinterpret_cast<uint64_t>(this) << 32);

  z_subscriber_options_default(&options_);

#if defined(Z_FEATURE_UNSTABLE_API) && !defined(VLINK_ENABLE_ZENOH_PICO)
  static auto locality_env = Utils::get_env("VLINK_ZENOH_ALLOWED_LOCALITY", "any");

  if (locality_env == "local") {
    options_.allowed_origin = Z_LOCALITY_SESSION_LOCAL;
  } else if (locality_env == "remote") {
    options_.allowed_origin = Z_LOCALITY_REMOTE;
  } else {
    options_.allowed_origin = Z_LOCALITY_ANY;
  }
#endif

  const auto& target_qos = factory.find_qos(impl_type, qos);

  session_ = factory.get_session(domain, zenoh_depth_from_qos(depth, target_qos), fragment, properties);

  if VUNLIKELY (!session_ || !z_internal_check(*session_)) {
    VLOG_E("ZenohFactory: Subscriber session is invalid.");
    return;
  }

  z_result_t ret = Z_EINVAL;

  ret = z_view_keyexpr_from_str(&keyexpr_, topic_.c_str());

  if VUNLIKELY (ret != Z_OK) {
    VLOG_F("ZenohFactory: Failed to invoke subscriber [z_view_keyexpr_from_str].");
    return;
  }
}

ZenohSubscriber::~ZenohSubscriber() { unsubscribe(); }

std::any ZenohSubscriber::get_native_handle() const { return this; }

bool ZenohSubscriber::suspend() {
  is_suspend_ = true;
  return true;
}

bool ZenohSubscriber::resume() {
  is_suspend_ = false;
  return true;
}

bool ZenohSubscriber::is_suspend() const { return is_suspend_; }

void ZenohSubscriber::subscribe() {
  if VUNLIKELY (!session_ || !z_internal_check(*session_)) {
    return;
  }

  bool expected = false;

  if VUNLIKELY (!has_subscribe_.compare_exchange_strong(expected, true)) {
    return;
  }

  z_result_t ret = Z_EINVAL;

  z_owned_closure_sample_t closure;
  z_closure_sample(&closure, ZenohSubscriber::on_data_callback, nullptr, this);

  ret = z_declare_subscriber(z_loan(*session_), &sub_, z_loan(keyexpr_), z_move(closure), &options_);

  if VUNLIKELY (ret != Z_OK) {
    has_subscribe_.store(false, std::memory_order_release);
    VLOG_F("ZenohFactory: Failed to invoke [z_declare_subscriber].");
    return;
  }
}

void ZenohSubscriber::unsubscribe() {
  bool expected = true;

  if VUNLIKELY (!has_subscribe_.compare_exchange_strong(expected, false)) {
    return;
  }

  z_drop(z_move(sub_));
}

void ZenohSubscriber::set_latency_and_lost_enabled(bool enable) {
  is_latency_and_lost_enabled_.store(enable, std::memory_order_release);
}

bool ZenohSubscriber::is_latency_and_lost_enabled() const {
  return is_latency_and_lost_enabled_.load(std::memory_order_acquire);
}

int64_t ZenohSubscriber::get_latency() const {
  if (!is_latency_and_lost_enabled_.load(std::memory_order_acquire)) {
    return 0;
  }

  return last_latency_.load(std::memory_order_relaxed);
}

const CalculateSample& ZenohSubscriber::get_calculate_sample() const { return calc_sample_; }

void ZenohSubscriber::process_message(uint64_t channel, uint64_t seq, uint64_t guid, uint64_t timestamp,
                                      MessageLoop* message_loop, Bytes&& bytes) {
  if VUNLIKELY (is_latency_and_lost_enabled_.load(std::memory_order_acquire)) {
    calc_sample_.update(seq, guid);
    last_latency_.store(ElapsedTimer::get_sys_timestamp(ElapsedTimer::kNano, false) - timestamp,
                        std::memory_order_relaxed);
  }

  if (message_loop) {
    auto weak_self = weak_from_this();
    auto task = [weak_self, channel, bytes = std::move(bytes)]() mutable {
      auto self = weak_self.lock();

      if VUNLIKELY (!self || !ZenohFactory::get().has_object(self.get())) {
        return;
      }

      auto* first_impl = self->get_first_impl();

      if VUNLIKELY (!first_impl || !first_impl->get_message_loop()) {
        return;
      }

      self->traverse_msg_callback([channel, &bytes](NodeImpl* impl, const auto& callback) {
        const auto* conf_ptr = impl->get_target_conf<ZenohConf>();

        if (static_cast<uint64_t>(conf_ptr->hash_code) != channel) {
          return;
        }

        callback(bytes);
      });
    };

    if VUNLIKELY (!message_loop->post_task(std::move(task))) {
      VLOG_W("ZenohFactory: Failed to post subscriber callback.");
    }
  } else {
    traverse_msg_callback([channel, &bytes](NodeImpl* impl, const auto& callback) {
      const auto* conf_ptr = impl->get_target_conf<ZenohConf>();

      if (static_cast<uint64_t>(conf_ptr->hash_code) != channel) {
        return;
      }

      callback(bytes);
    });
  }
}

void ZenohSubscriber::on_data_callback(z_loaned_sample_t* sample, void* context) {
  auto* instance = static_cast<ZenohSubscriber*>(context);

  if VUNLIKELY (!ZenohFactory::get().has_object(instance)) {
    return;
  }

  if VUNLIKELY (instance->is_suspend_) {
    return;
  }

  ZenohHeader header;

  if VUNLIKELY (!ZenohFactory::read_header(header, z_sample_attachment(sample))) {
    VLOG_E("ZenohFactory: Failed to read subscriber header.");
    return;
  }

  ZenohPayloadView payload;

  if VUNLIKELY (!payload.load(z_sample_payload(sample))) {
    VLOG_E("ZenohFactory: Failed to read subscriber payload.");
    return;
  }

  auto* first_impl = instance->get_first_impl();
  auto* message_loop = first_impl ? first_impl->get_message_loop() : nullptr;

  Bytes msg_bytes =
      message_loop ? Bytes::deep_copy(payload.data, payload.size) : Bytes::shallow_copy(payload.data, payload.size);

  instance->process_message(header.channel, header.seq, header.guid, header.timestamp, message_loop,
                            std::move(msg_bytes));
}

}  // namespace vlink
