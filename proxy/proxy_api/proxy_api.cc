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

#include <vlink/external/proxy_api.h>
//
#include <vlink/base/elapsed_timer.h>
#include <vlink/base/helpers.h>
#include <vlink/base/timer.h>
#include <vlink/base/utils.h>
#include <vlink/version.h>
#include <vlink/vlink.h>
#include <vlink/zerocopy/proxy_data.h>
//
#include <memory>
#include <shared_mutex>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#ifdef _WIN32
#include <Windows.h>
#undef min
#undef max
#undef GetMessage
#endif

#if __has_include(<unistd.h>)
#include <unistd.h>
#endif

#include "../proxy_common.h"

namespace vlink {

[[maybe_unused]] static constexpr size_t kMaxTaskSize{5000U};
[[maybe_unused]] static constexpr uint32_t kMaxElapsedTime{5000U};

using DataSub = Subscriber<zerocopy::ProxyData>;
using DataPub = Publisher<zerocopy::ProxyData>;

using RawPub = Publisher<Bytes>;
using RawSub = Subscriber<Bytes>;

using TimeSub = SecuritySubscriber<proxy::TimePacket>;
using InfoSub = SecuritySubscriber<proxy::InfoListPacket>;
using ControlPub = SecurityPublisher<proxy::ControlPacket>;

#if VLINK_PROXY_ENABLE_HANDSHAKE
using HandshakeCli = SecurityClient<proxy::HandshakeReqPacket, proxy::HandshakeRespPacket>;
#endif

// ProxyAPI::Impl
struct ProxyAPI::Impl final {  // NOLINT(clang-analyzer-optin.performance.Padding)
  std::atomic<uint32_t> control_error_count{0};
  std::atomic<ProxyAPI::Mode> mode{ProxyAPI::kOffline};
  std::atomic<ProxyAPI::Error> error{ProxyAPI::kNoError};
  std::atomic<uint64_t> sys_time{0};
  std::atomic<uint64_t> boot_time{0};
  std::atomic<double> cpu_usage{0};
  std::atomic<double> memory_usage{0};
  std::atomic_bool is_connected{false};
  std::atomic_bool control_ret{false};
  std::atomic_bool resetting{false};

  ProxyAPI::Config config;
  uint32_t control_id{0};
#if VLINK_PROXY_ENABLE_HANDSHAKE
  std::string token;
  std::shared_mutex token_mtx;
#endif
  ProxyAPI::Control last_control;
  Timer heartbeat_timer;

  ElapsedTimer record_elapsed_timer{ElapsedTimer::kMicro};
  ElapsedTimer sys_elapsed_timer{ElapsedTimer::kMicro};
  ElapsedTimer boot_elapsed_timer{ElapsedTimer::kMicro};
  ElapsedTimer main_elapsed_timer;
  ElapsedTimer connect_elapsed_timer;
  ElapsedTimer error_elapsed_timer;

  std::shared_mutex control_mtx;
  std::shared_mutex connect_mtx;
  std::shared_mutex error_mtx;
  // Guards top-level transport handle pointers while they are rebuilt.
  std::shared_mutex handle_mtx;
  std::shared_mutex time_mtx;
  std::shared_mutex info_mtx;
  std::shared_mutex data_mtx;
  // Guards proxy_version, hostname/machine_id, and their discovered value sets.
  std::shared_mutex version_mtx;
  std::shared_mutex direct_mtx;

  ProxyAPI::ConnectCallback connect_callback;
  ProxyAPI::ErrorCallback error_callback;
  ProxyAPI::TimeCallback time_callback;
  ProxyAPI::InfoCallback info_callback;
  ProxyAPI::DataCallback data_callback;

  std::shared_ptr<DataSub> data_sub;
  std::shared_ptr<DataPub> data_pub;
  std::shared_ptr<TimeSub> time_sub;
  std::shared_ptr<InfoSub> info_sub;
  std::shared_ptr<ControlPub> control_pub;
#if VLINK_PROXY_ENABLE_HANDSHAKE
  std::shared_ptr<HandshakeCli> handshake_cli;
#endif

  std::vector<ProxyAPI::Info> direct_info_list;

  std::unordered_map<std::string, std::shared_ptr<RawPub>> pub_map;
  std::unordered_map<std::string, std::shared_ptr<RawSub>> sub_map;
  std::unordered_set<std::string> getter_sub_urls;

  std::string proxy_version;
  std::string hostname;
  std::unordered_set<std::string> hostname_set;
  std::string machine_id;
  std::unordered_set<std::string> machine_id_set;
};

// ProxyAPI
ProxyAPI::ProxyAPI(const Config& config) : impl_(std::make_unique<Impl>()) {
  set_name("ProxyAPI");

  impl_->config = config;
  impl_->control_id = static_cast<uint32_t>(ElapsedTimer::get_cpu_timestamp());

  if VUNLIKELY (impl_->control_id == 0) {
    impl_->control_id = 1;
  }

  impl_->heartbeat_timer.attach(this);
  impl_->heartbeat_timer.set_loop_count(Timer::kInfinite);
  impl_->heartbeat_timer.set_interval(1000);
  impl_->heartbeat_timer.start([this]() {
    if VUNLIKELY (is_ready_to_quit()) {
      return;
    }

    if VUNLIKELY (impl_->main_elapsed_timer.is_active() && impl_->main_elapsed_timer.restart() > 5000) {
      VLOG_I("ProxyApi: Wakeup.");
      reset_handle();

      if (impl_->config.role == kController) {
        std::shared_lock lock(impl_->control_mtx);
        send_control_sync(impl_->last_control);
      }

      return;
    }

#if VLINK_PROXY_ENABLE_HANDSHAKE
    bool token_empty = false;

    {
      std::shared_lock token_lock(impl_->token_mtx);
      token_empty = impl_->token.empty();
    }

    if VUNLIKELY (token_empty) {
      Error handshake_err = kNoError;
      bool handshake_ok = false;

      if (do_handshake(handshake_err)) {
        handshake_ok = true;

        if (impl_->error.load(std::memory_order_relaxed) != kMultiProxyError) {
          process_error(kNoError);
        }

        if (impl_->config.role == kController) {
          std::shared_lock lock(impl_->control_mtx);
          send_control_sync(impl_->last_control);
        }
      } else if (handshake_err != kNoError && impl_->error.load(std::memory_order_relaxed) != handshake_err) {
        process_error(handshake_err);
      }

      if (!handshake_ok && impl_->connect_elapsed_timer.get() > 5000) {
        process_connected(false);
      }

      return;
    }
#endif

    if VUNLIKELY (impl_->connect_elapsed_timer.get() > 5000) {
      process_connected(false);
    }
  });

  impl_->main_elapsed_timer.start();

  if (impl_->config.role == kController) {
    Control control;
    control.mode = kAuto;
    send_control(control, true);
  }
}

ProxyAPI::~ProxyAPI() {
  this->quit(true);

  this->wait_for_quit();

  std::unique_lock handle_lock(impl_->handle_mtx);
#if VLINK_PROXY_ENABLE_HANDSHAKE
  impl_->handshake_cli.reset();
#endif
  impl_->data_sub.reset();
  impl_->data_pub.reset();
  impl_->time_sub.reset();
  impl_->info_sub.reset();
  impl_->control_pub.reset();
}

void ProxyAPI::register_connect_callback(ConnectCallback&& callback) {
  bool fire_now = false;

  if (callback) {
    std::lock_guard lock(impl_->connect_mtx);
    fire_now = impl_->is_connected.load(std::memory_order_relaxed);
  }

  if (fire_now) {
    callback(true);
  }

  std::lock_guard lock(impl_->connect_mtx);
  impl_->connect_callback = std::move(callback);
}

void ProxyAPI::register_error_callback(ErrorCallback&& callback) {
  Error fire_error = kNoError;

  if (callback) {
    std::lock_guard lock(impl_->error_mtx);
    fire_error = impl_->error.load(std::memory_order_relaxed);
  }

  if (fire_error != kNoError) {
    callback(fire_error);
  }

  std::lock_guard lock(impl_->error_mtx);
  impl_->error_callback = std::move(callback);
}

void ProxyAPI::register_time_callback(TimeCallback&& callback) {
  bool fire_now = false;

  if (callback) {
    std::lock_guard lock(impl_->time_mtx);
    fire_now = impl_->sys_elapsed_timer.is_active() && impl_->boot_elapsed_timer.is_active();
  }

  if (fire_now) {
    callback(get_current_sys_time(), get_current_boot_time());
  }

  std::lock_guard lock(impl_->time_mtx);
  impl_->time_callback = std::move(callback);
}

void ProxyAPI::register_info_callback(InfoCallback&& callback) {
  std::lock_guard lock(impl_->info_mtx);
  impl_->info_callback = std::move(callback);
}

void ProxyAPI::register_data_callback(DataCallback&& callback) {
  std::lock_guard lock(impl_->data_mtx);
  impl_->data_callback = std::move(callback);
}

bool ProxyAPI::send_control(const Control& control, bool async) {
  if VUNLIKELY (impl_->config.role != kController) {
    VLOG_E("ProxyApi: Non-controller nodes cannot send control.");
    return false;
  }

  {
    std::lock_guard lock(impl_->control_mtx);
    impl_->last_control = control;
  }

  impl_->mode.store(control.mode, std::memory_order_relaxed);

  if (control.mode == kRecord) {
    impl_->record_elapsed_timer.restart();
  }

  impl_->control_ret.store(false, std::memory_order_release);

  bool send_ok = false;

  if (async) {
    send_ok = post_task(
        [this, control]() { impl_->control_ret.store(send_control_sync(control), std::memory_order_release); });
  } else {
    impl_->control_ret.store(send_control_sync(control), std::memory_order_release);
    send_ok = impl_->control_ret.load(std::memory_order_acquire);
  }

  if (impl_->config.direct) {
    send_ok = post_task([this, control]() { sync_direct_maps(control); }) && send_ok;
  }

  return send_ok;
}

bool ProxyAPI::send_data(const Data& data) {
  if VUNLIKELY (impl_->config.role != kController) {
    VLOG_E("ProxyApi: Non-controller nodes cannot send data.");
    return false;
  }

  const auto schema_type = SchemaData::is_valid_type(data.schema) ? data.schema : SchemaType::kUnknown;

  if VUNLIKELY (schema_type == SchemaType::kUnknown) {
    VLOG_W("ProxyApi: send_data with unknown schema (url=", data.url, ").");
  }

  if VUNLIKELY (data.url.empty() || data.ser.empty()) {
    VLOG_E("ProxyApi: send_data requires url and ser.");
    return false;
  }

  if (impl_->config.direct) {
    std::shared_lock lock(impl_->direct_mtx);

    auto pub_iter = impl_->pub_map.find(data.url);

    if VUNLIKELY (pub_iter == impl_->pub_map.end()) {
      return false;
    }

    if VUNLIKELY (!pub_iter->second->has_subscribers()) {
      return false;
    }

    const auto direct_schema_type = SchemaData::is_valid_type(pub_iter->second->get_schema_type())
                                        ? pub_iter->second->get_schema_type()
                                        : SchemaType::kUnknown;

    if VUNLIKELY (pub_iter->second->get_ser_type() != data.ser || direct_schema_type != schema_type) {
      VLOG_E("ProxyApi: send_data metadata does not match direct publisher.");
      return false;
    }

    pub_iter->second->publish(data.raw, true);

    return true;
  }

  std::shared_lock handle_lock(impl_->handle_mtx);

  if VUNLIKELY (this->is_ready_to_quit() || !impl_->data_pub) {
    return false;
  }

  if VUNLIKELY (!impl_->data_pub->has_subscribers()) {
    return false;
  }

  zerocopy::ProxyData t_data;
  t_data.create(data.raw, data.url, data.ser, static_cast<uint32_t>(schema_type));

  t_data.set_control_id(impl_->control_id);
  t_data.set_mode(impl_->mode.load(std::memory_order_relaxed));
  t_data.set_timestamp(data.timestamp);
  t_data.set_seq(data.seq);

  return impl_->data_pub->publish(t_data, true);
}

const ProxyAPI::Config& ProxyAPI::get_current_config() const { return impl_->config; }

ProxyAPI::Mode ProxyAPI::get_current_mode() const { return impl_->mode.load(std::memory_order_relaxed); }

ProxyAPI::Error ProxyAPI::get_current_error() const { return impl_->error.load(std::memory_order_relaxed); }

std::string ProxyAPI::get_current_hostname() const {
  std::shared_lock lock(impl_->version_mtx);
  return impl_->hostname;
}

std::string ProxyAPI::get_current_machine_id() const {
  std::shared_lock lock(impl_->version_mtx);
  return impl_->machine_id;
}

uint64_t ProxyAPI::get_current_sys_time() const {
  if VUNLIKELY (!impl_->sys_elapsed_timer.is_active()) {
    return impl_->sys_time.load(std::memory_order_relaxed);
  }

  return impl_->sys_time.load(std::memory_order_relaxed) + impl_->sys_elapsed_timer.get();
}

uint64_t ProxyAPI::get_current_boot_time() const {
  if VUNLIKELY (!impl_->boot_elapsed_timer.is_active()) {
    return impl_->boot_time.load(std::memory_order_relaxed);
  }

  return impl_->boot_time.load(std::memory_order_relaxed) + impl_->boot_elapsed_timer.get();
}

double ProxyAPI::get_current_cpu_usage() const { return impl_->cpu_usage.load(std::memory_order_relaxed); }

double ProxyAPI::get_current_memory_usage() const { return impl_->memory_usage.load(std::memory_order_relaxed); }

int64_t ProxyAPI::get_latency() const {
  if (impl_->config.direct) {
    return 0;
  }

  std::shared_lock handle_lock(impl_->handle_mtx);

  if VUNLIKELY (!impl_->data_sub || !impl_->data_sub->has_inited()) {
    return 0;
  }

  return impl_->data_sub->get_latency();
}

SampleLostInfo ProxyAPI::get_lost() const {
  if (impl_->config.direct) {
    return SampleLostInfo();
  }

  std::shared_lock handle_lock(impl_->handle_mtx);

  if VUNLIKELY (!impl_->data_sub || !impl_->data_sub->has_inited()) {
    return SampleLostInfo();
  }

  return impl_->data_sub->get_lost();
}

bool ProxyAPI::is_connected() const { return impl_->is_connected.load(std::memory_order_relaxed); }

std::string ProxyAPI::get_proxy_version() const {
  std::shared_lock lock(impl_->version_mtx);
  return impl_->proxy_version;
}

std::unordered_set<std::string> ProxyAPI::get_proxy_hostnames() const {
  std::shared_lock lock(impl_->version_mtx);
  return impl_->hostname_set;
}

std::unordered_set<std::string> ProxyAPI::get_proxy_machine_ids() const {
  std::shared_lock lock(impl_->version_mtx);
  return impl_->machine_id_set;
}

bool ProxyAPI::is_support_shm() {
#ifdef VLINK_SUPPORT_SHM
  return true;
#else
  return false;
#endif
}

bool ProxyAPI::is_enable_filter() { return VLINK_PROXY_ENABLE_FILTER; }

std::string ProxyAPI::get_format_sys_time(uint64_t time, bool enable_utc) {
  auto seconds_part = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::microseconds(time));
  std::chrono::microseconds microseconds_part =
      std::chrono::microseconds(time) - std::chrono::duration_cast<std::chrono::microseconds>(seconds_part);

  std::time_t time_t_part = seconds_part.count();

  std::tm tm_result{};
  std::tm* tm_ptr = nullptr;

#if defined(_WIN32) || defined(_WIN64)

  if (enable_utc) {
    if VLIKELY (gmtime_s(&tm_result, &time_t_part) == 0) {
      tm_ptr = &tm_result;
    }
  } else {
    if VLIKELY (localtime_s(&tm_result, &time_t_part) == 0) {
      tm_ptr = &tm_result;
    }
  }
#else

  if (enable_utc) {
    if VLIKELY (gmtime_r(&time_t_part, &tm_result) != nullptr) {
      tm_ptr = &tm_result;
    }
  } else {
    if VLIKELY (localtime_r(&time_t_part, &tm_result) != nullptr) {
      tm_ptr = &tm_result;
    }
  }
#endif

  if VUNLIKELY (!tm_ptr) {
    return {};
  }

  char buffer[100];
  std::strftime(buffer, sizeof(buffer), "%Y/%m/%d %H:%M:%S", tm_ptr);

  thread_local std::ostringstream oss;

  oss.clear();
  oss.str("");

  oss << buffer << ":" << std::setw(3) << std::setfill('0') << microseconds_part.count() / 1000;

  return oss.str();
}

std::string ProxyAPI::get_format_boot_time(uint64_t time) { return Helpers::format_milliseconds(time / 1000U, true); }

size_t ProxyAPI::get_max_task_count() const { return kMaxTaskSize; }

uint32_t ProxyAPI::get_max_elapsed_time() const { return kMaxElapsedTime; }

void ProxyAPI::on_begin() {
  reset_handle();

  MessageLoop::on_begin();
}

void ProxyAPI::on_end() {
  if (impl_->config.role == kController) {
    Control control;
    control.mode = kOffline;
    send_control(control, false);
  }

  MessageLoop::on_end();
}

void ProxyAPI::sync_direct_maps(const Control& control) {
  std::unique_lock direct_lock(impl_->direct_mtx);

  std::unordered_set<std::string> pub_url_set;

  if (impl_->config.role == kController) {
    pub_url_set.reserve(control.url_meta_list.size());

    for (const auto& meta : control.url_meta_list) {
      if (meta.type != kSubscriber) {
        pub_url_set.emplace(meta.url);
      }
    }

    for (auto pub_iter = impl_->pub_map.begin(); pub_iter != impl_->pub_map.end();) {
      if (pub_url_set.count(pub_iter->first) == 0) {
        pub_iter = impl_->pub_map.erase(pub_iter);
      } else {
        ++pub_iter;
      }
    }
  }

  std::unordered_map<std::string, UrlMeta> desired_sub_meta_map;
  desired_sub_meta_map.reserve(impl_->direct_info_list.size() + control.url_meta_list.size());

  std::unordered_set<std::string> getter_url_set;
  getter_url_set.reserve(impl_->direct_info_list.size());

  if (control.mode == kObserveAll || control.mode == kAutoAndObserveAll) {
    for (const auto& info : impl_->direct_info_list) {
      if (info.status == kInvalid || info.url.empty() || info.ser.empty()) {
        continue;
      }

      desired_sub_meta_map[info.url] = UrlMeta{info.url, info.ser, info.schema, kSubscriber};

      if ((info.type & kSetter) != 0U) {
        getter_url_set.emplace(info.url);
      }
    }
  } else if (control.mode == kObserveOne || control.mode == kRecord || control.mode == kAuto) {
    std::unordered_map<std::string, uint32_t> info_type_map;
    info_type_map.reserve(impl_->direct_info_list.size());

    for (const auto& info : impl_->direct_info_list) {
      if (info.status != kInvalid && !info.url.empty()) {
        info_type_map[info.url] = info.type;
      }
    }

    for (const auto& meta : control.url_meta_list) {
      if (meta.type != kSubscriber || meta.url.empty() || meta.ser.empty()) {
        continue;
      }

      desired_sub_meta_map[meta.url] = meta;

      auto type_iter = info_type_map.find(meta.url);

      if (type_iter != info_type_map.end() && (type_iter->second & kSetter) != 0U) {
        getter_url_set.emplace(meta.url);
      }
    }
  }

  for (auto sub_iter = impl_->sub_map.begin(); sub_iter != impl_->sub_map.end();) {
    if (desired_sub_meta_map.find(sub_iter->first) == desired_sub_meta_map.end()) {
      impl_->getter_sub_urls.erase(sub_iter->first);
      sub_iter = impl_->sub_map.erase(sub_iter);
    } else {
      ++sub_iter;
    }
  }

#ifdef _WIN32
  std::this_thread::sleep_for(std::chrono::milliseconds(20));  // DDS BUG?
#endif

  for (const auto& meta : control.url_meta_list) {
    if (meta.type != kPublisher || meta.url.empty() || meta.ser.empty()) {
      continue;
    }

    if VUNLIKELY (impl_->config.role != kController) {
      continue;
    }

    auto pub_iter = impl_->pub_map.find(meta.url);

    if (pub_iter != impl_->pub_map.end()) {
      auto* pub = pub_iter->second.get();
      const auto schema_type = SchemaData::is_valid_type(meta.schema) ? meta.schema : SchemaType::kUnknown;

      if (pub && pub->get_ser_type() == meta.ser && pub->get_schema_type() == schema_type) {
        continue;
      }

      impl_->pub_map.erase(pub_iter);
    }

    try {
      auto pub = std::make_shared<RawPub>(meta.url, InitType::kWithoutInit);

      if (impl_->config.native) {
        pub->set_property("dds.ip", "127.0.0.1");
      }

      pub->set_ser_type(meta.ser, meta.schema);
      pub->init();
      impl_->pub_map.emplace(meta.url, std::move(pub));
    } catch (const Exception::RuntimeError&) {
    }
  }

  for (const auto& [url, meta] : desired_sub_meta_map) {
    const bool want_getter = getter_url_set.count(url) != 0U;
    auto sub_iter = impl_->sub_map.find(url);

    if (sub_iter != impl_->sub_map.end()) {
      auto* sub = sub_iter->second.get();
      const bool is_getter = impl_->getter_sub_urls.count(url) != 0U;

      if (is_getter != want_getter) {
        impl_->sub_map.erase(sub_iter);
        impl_->getter_sub_urls.erase(url);
      } else if (sub && sub->get_ser_type() == meta.ser && sub->get_schema_type() == meta.schema) {
        if (want_getter) {
          impl_->getter_sub_urls.emplace(url);
        }
        continue;
      } else {
        impl_->sub_map.erase(sub_iter);
        impl_->getter_sub_urls.erase(url);
      }
    }

    try {
      auto sub = std::make_shared<RawSub>(url, InitType::kWithoutInit);

      if (want_getter) {
        sub->mark_as_getter();
      }

      if (impl_->config.native) {
        sub->set_property("dds.ip", "127.0.0.1");
      }

      sub->set_discovery_enabled(false);
      sub->set_ser_type(meta.ser, meta.schema);
      sub->init();

      Data data;
      data.url = sub->get_url();
      data.ser = meta.ser;
      data.schema = meta.schema;

      sub->listen([this, sub_ptr = sub.get(), data](const auto& bytes) mutable {
        std::shared_lock data_lock(impl_->data_mtx);

        if VUNLIKELY (!impl_->data_callback) {
          return;
        }

        data.raw.shallow_copy(bytes);
        data.timestamp = impl_->record_elapsed_timer.get();
        data.seq = sub_ptr->get_lost().total - 1;

        impl_->data_callback(data);
      });

      impl_->sub_map.emplace(url, std::move(sub));

      if (want_getter) {
        impl_->getter_sub_urls.emplace(url);
      }
    } catch (const Exception::RuntimeError&) {
    }
  }
}

bool ProxyAPI::send_control_sync(const Control& control) {
  std::shared_lock handle_lock(impl_->handle_mtx);

  if VUNLIKELY (!impl_->control_pub) {
    return false;
  }

#if VLINK_PROXY_ENABLE_HANDSHAKE
  std::string token_copy;

  {
    std::shared_lock token_lock(impl_->token_mtx);
    token_copy = impl_->token;
  }

  if VUNLIKELY (token_copy.empty()) {
    return false;
  }
#endif

  if VUNLIKELY (!impl_->control_pub->has_subscribers()) {
    return false;
  }

  proxy::ControlPacket packet;
  packet.control_id = impl_->control_id;
#if VLINK_PROXY_ENABLE_HANDSHAKE
  packet.token = std::move(token_copy);
#endif
  packet.body = control;

  for (auto& url_meta : packet.body.url_meta_list) {
    if VUNLIKELY (!SchemaData::is_valid_type(url_meta.schema)) {
      url_meta.schema = SchemaType::kUnknown;
    }
  }

  impl_->connect_elapsed_timer.restart();

  return impl_->control_pub->publish(packet, true);
}

bool ProxyAPI::do_handshake(Error& out_err) {
  out_err = kNoError;

#if VLINK_PROXY_ENABLE_HANDSHAKE
  if VUNLIKELY (!impl_->handshake_cli || !impl_->handshake_cli->has_inited()) {
    return false;
  }

  const auto wait_timeout = std::chrono::milliseconds(proxy::kHandshakeWaitMs);
  const auto invoke_timeout = std::chrono::milliseconds(proxy::kHandshakeInvokeMs);

  if VUNLIKELY (!impl_->handshake_cli->wait_for_connected(wait_timeout)) {
    return false;
  }

  proxy::HandshakeReqPacket req;
  req.control_id = impl_->control_id;
  if (impl_->config.match_version) {
    req.version = VLINK_VERSION;
  }
  req.hostname = Utils::get_host_name();
  req.role = impl_->config.role == kController ? "controller" : "listener";

  proxy::HandshakeRespPacket resp;

  if VUNLIKELY (!impl_->handshake_cli->invoke(req, resp, invoke_timeout)) {
    return false;
  }

  if VUNLIKELY (resp.result == proxy::kHandshakeVersionMismatch) {
    VLOG_E("ProxyApi: Handshake version mismatch (server=", resp.version, ", client=" VLINK_VERSION ").");
    out_err = kVersionCompError;
    return false;
  }

  if VUNLIKELY (resp.result != proxy::kHandshakeOk || resp.token.empty()) {
    VLOG_E("ProxyApi: Handshake refused by server (result=", static_cast<int>(resp.result), ").");
    out_err = kTokenError;
    return false;
  }

  {
    std::lock_guard version_lock(impl_->version_mtx);
    impl_->proxy_version = std::move(resp.version);

    if (!resp.hostname.empty()) {
      impl_->hostname_set.emplace(resp.hostname);
    }

    if (!resp.machine_id.empty()) {
      impl_->machine_id_set.emplace(resp.machine_id);
    }

    impl_->hostname = std::move(resp.hostname);
    impl_->machine_id = std::move(resp.machine_id);
  }

  {
    std::lock_guard token_lock(impl_->token_mtx);
    impl_->token = std::move(resp.token);
  }
#endif

  return true;
}

void ProxyAPI::reset_handle() {
  std::unique_lock handle_lock(impl_->handle_mtx);

  impl_->resetting.store(true, std::memory_order_relaxed);

  impl_->control_ret.store(false, std::memory_order_release);

  std::string domain_id_str = std::to_string(impl_->config.domain_id);

  if (impl_->config.direct) {
    impl_->data_sub =
        std::make_shared<DataSub>(proxy::make_url("shm", proxy::kDataShmUrlCtx, domain_id_str), InitType::kWithoutInit);
    impl_->data_pub = std::make_shared<DataPub>(proxy::make_url("shm", proxy::kViewerDataShmUrlCtx, domain_id_str),
                                                InitType::kWithoutInit);
  } else {
    if (impl_->config.reliable) {
      impl_->data_sub = std::make_shared<DataSub>(
          proxy::make_url(impl_->config.dds_impl, proxy::kDataReliableUrlCtx, domain_id_str), InitType::kWithoutInit);
      impl_->data_pub = std::make_shared<DataPub>(
          proxy::make_url(impl_->config.dds_impl, proxy::kViewerDataReliableUrlCtx, domain_id_str),
          InitType::kWithoutInit);
    } else {
      impl_->data_sub = std::make_shared<DataSub>(
          proxy::make_url(impl_->config.dds_impl, proxy::kDataUrlCtx, domain_id_str), InitType::kWithoutInit);
      impl_->data_pub = std::make_shared<DataPub>(
          proxy::make_url(impl_->config.dds_impl, proxy::kViewerDataUrlCtx, domain_id_str), InitType::kWithoutInit);
    }
  }

  impl_->data_sub->set_latency_and_lost_enabled(true);

  Security::Config sec_cfg;

  if (!impl_->config.security_key.empty()) {
    sec_cfg.key = impl_->config.security_key;
  }

  impl_->time_sub = std::make_shared<TimeSub>(
      proxy::make_url(impl_->config.dds_impl, proxy::kTimeUrlCtx, domain_id_str), sec_cfg, InitType::kWithoutInit);
  impl_->info_sub = std::make_shared<InfoSub>(
      proxy::make_url(impl_->config.dds_impl, proxy::kInfoListUrlCtx, domain_id_str), sec_cfg, InitType::kWithoutInit);
  impl_->control_pub = std::make_shared<ControlPub>(
      proxy::make_url(impl_->config.dds_impl, proxy::kControlUrlCtx, domain_id_str), sec_cfg, InitType::kWithoutInit);

#if VLINK_PROXY_ENABLE_HANDSHAKE
  impl_->handshake_cli = std::make_shared<HandshakeCli>(
      proxy::make_url(impl_->config.dds_impl, proxy::kHandshakeUrlCtx, domain_id_str), sec_cfg, InitType::kWithoutInit);
#endif

  impl_->data_sub->set_discovery_enabled(false);
  impl_->data_pub->set_discovery_enabled(false);
  impl_->time_sub->set_discovery_enabled(false);
  impl_->info_sub->set_discovery_enabled(false);
  impl_->control_pub->set_discovery_enabled(false);
#if VLINK_PROXY_ENABLE_HANDSHAKE
  impl_->handshake_cli->set_discovery_enabled(false);
#endif

  if (impl_->config.native) {
    impl_->data_sub->set_property("dds.ip", "127.0.0.1");
    impl_->data_pub->set_property("dds.ip", "127.0.0.1");
    impl_->time_sub->set_property("dds.ip", "127.0.0.1");
    impl_->info_sub->set_property("dds.ip", "127.0.0.1");
    impl_->control_pub->set_property("dds.ip", "127.0.0.1");
#if VLINK_PROXY_ENABLE_HANDSHAKE
    impl_->handshake_cli->set_property("dds.ip", "127.0.0.1");
#endif
  } else {
    if (!impl_->config.allow_ip.empty()) {
      impl_->data_sub->set_property("dds.ip", impl_->config.allow_ip);
      impl_->data_pub->set_property("dds.ip", impl_->config.allow_ip);
      impl_->time_sub->set_property("dds.ip", impl_->config.allow_ip);
      impl_->info_sub->set_property("dds.ip", impl_->config.allow_ip);
      impl_->control_pub->set_property("dds.ip", impl_->config.allow_ip);
#if VLINK_PROXY_ENABLE_HANDSHAKE
      impl_->handshake_cli->set_property("dds.ip", impl_->config.allow_ip);
#endif
    }

    if (!impl_->config.peer_ip.empty()) {
      impl_->data_sub->set_property("dds.peer", impl_->config.peer_ip);
      impl_->data_pub->set_property("dds.peer", impl_->config.peer_ip);
      impl_->time_sub->set_property("dds.peer", impl_->config.peer_ip);
      impl_->info_sub->set_property("dds.peer", impl_->config.peer_ip);
      impl_->control_pub->set_property("dds.peer", impl_->config.peer_ip);
#if VLINK_PROXY_ENABLE_HANDSHAKE
      impl_->handshake_cli->set_property("dds.peer", impl_->config.peer_ip);
#endif
    }
  }

  if (impl_->config.buf_size > 0) {
    std::string buf_str = std::to_string(impl_->config.buf_size);
    impl_->data_sub->set_property("dds.buf", buf_str);
    impl_->data_pub->set_property("dds.buf", buf_str);
    impl_->time_sub->set_property("dds.buf", buf_str);
    impl_->info_sub->set_property("dds.buf", buf_str);
    impl_->control_pub->set_property("dds.buf", buf_str);
#if VLINK_PROXY_ENABLE_HANDSHAKE
    impl_->handshake_cli->set_property("dds.buf", buf_str);
#endif
  } else {
    impl_->data_sub->set_property("dds.buf", std::string(proxy::kSocketBufStr));
    impl_->data_pub->set_property("dds.buf", std::string(proxy::kSocketBufStr));
    impl_->time_sub->set_property("dds.buf", std::string(proxy::kSocketBufStr));
    impl_->info_sub->set_property("dds.buf", std::string(proxy::kSocketBufStr));
    impl_->control_pub->set_property("dds.buf", std::string(proxy::kSocketBufStr));
#if VLINK_PROXY_ENABLE_HANDSHAKE
    impl_->handshake_cli->set_property("dds.buf", std::string(proxy::kSocketBufStr));
#endif
  }

  if (impl_->config.mtu_size > 0) {
    std::string mtu_str = std::to_string(impl_->config.mtu_size);
    impl_->data_sub->set_property("dds.mtu", mtu_str);
    impl_->data_pub->set_property("dds.mtu", mtu_str);
    impl_->time_sub->set_property("dds.mtu", mtu_str);
    impl_->info_sub->set_property("dds.mtu", mtu_str);
    impl_->control_pub->set_property("dds.mtu", mtu_str);
#if VLINK_PROXY_ENABLE_HANDSHAKE
    impl_->handshake_cli->set_property("dds.mtu", mtu_str);
#endif
  } else {
    impl_->data_sub->set_property("dds.mtu", std::string(proxy::kSocketMtuStr));
    impl_->data_pub->set_property("dds.mtu", std::string(proxy::kSocketMtuStr));
    impl_->time_sub->set_property("dds.mtu", std::string(proxy::kSocketMtuStr));
    impl_->info_sub->set_property("dds.mtu", std::string(proxy::kSocketMtuStr));
    impl_->control_pub->set_property("dds.mtu", std::string(proxy::kSocketMtuStr));
#if VLINK_PROXY_ENABLE_HANDSHAKE
    impl_->handshake_cli->set_property("dds.mtu", std::string(proxy::kSocketMtuStr));
#endif
  }

  if (impl_->config.enable_tcp) {
    impl_->data_sub->set_property("dds.tcp", "1");
    impl_->data_pub->set_property("dds.tcp", "1");
  } else {
    impl_->data_sub->set_property("dds.tcp", "0");
    impl_->data_pub->set_property("dds.tcp", "0");
  }

  {
    std::lock_guard version_lock(impl_->version_mtx);
    impl_->hostname.clear();
    impl_->hostname_set.clear();

    impl_->machine_id.clear();
    impl_->machine_id_set.clear();

    impl_->proxy_version.clear();
  }

#if VLINK_PROXY_ENABLE_HANDSHAKE
  {
    std::lock_guard token_lock(impl_->token_mtx);
    impl_->token.clear();
  }

  impl_->handshake_cli->init();

  Error handshake_err = kNoError;

  if VUNLIKELY (!do_handshake(handshake_err)) {
    if (handshake_err != kNoError) {
      VLOG_E("ProxyApi: Handshake failed during reset_handle (err=", static_cast<int>(handshake_err), ").");

      const Error prev_err = impl_->error.exchange(handshake_err, std::memory_order_acq_rel);

      if (prev_err != handshake_err) {
        post_task([this, handshake_err]() {
          std::shared_lock lock(impl_->error_mtx);

          if VLIKELY (impl_->error_callback) {
            impl_->error_callback(handshake_err);
          }
        });
      }
    }
  }
#endif

  if (!impl_->config.direct) {
    impl_->data_sub->init();
    impl_->data_pub->init();
  }

  impl_->time_sub->init();
  impl_->info_sub->init();
  impl_->control_pub->init();

  if (impl_->data_sub->has_inited()) {
    impl_->data_sub->listen([this](const zerocopy::ProxyData& t_data) {
      if VUNLIKELY (this->is_ready_to_quit()) {
        return;
      }

      if VUNLIKELY (t_data.control_id() == 0) {
        return;
      }

      if VUNLIKELY (!t_data.hostname().empty()) {
        std::shared_lock version_lock(impl_->version_mtx);
        if VUNLIKELY (t_data.hostname() != impl_->hostname) {
          return;
        }
      }

      if (impl_->config.role == kController) {
        if VUNLIKELY (t_data.control_id() != impl_->control_id) {
          return;
        }

        if VUNLIKELY (t_data.mode() != static_cast<uint32_t>(impl_->mode.load(std::memory_order_relaxed))) {
          return;
        }
      }

      std::shared_lock lock(impl_->data_mtx);

      if VLIKELY (impl_->data_callback) {
        Data data;
        data.url = t_data.url();
        data.ser = t_data.ser();
        data.schema = SchemaData::is_valid_type(static_cast<SchemaType>(t_data.schema()))
                          ? static_cast<SchemaType>(t_data.schema())
                          : SchemaType::kUnknown;

        if VUNLIKELY (data.ser.empty()) {
          return;
        }

        data.raw.shallow_copy(t_data.raw());
        data.timestamp = t_data.timestamp();
        data.seq = t_data.seq();

        impl_->data_callback(data);
      }
    });
  }

  impl_->time_sub->listen([this](const proxy::TimePacket& time) {
    if VUNLIKELY (this->is_ready_to_quit()) {
      return;
    }

#if VLINK_PROXY_ENABLE_HANDSHAKE
    bool token_mismatch = false;
    bool token_present = false;
    std::string expected_token;

    {
      std::shared_lock token_lock(impl_->token_mtx);
      expected_token = impl_->token;
      token_present = !expected_token.empty();
      token_mismatch = token_present && (time.token != expected_token);
    }

    if VUNLIKELY (!token_present) {
      return;
    }

    const auto clear_expected_token = [this, &expected_token]() {
      std::lock_guard token_lock(impl_->token_mtx);
      if (impl_->token != expected_token) {
        return false;
      }

      impl_->token.clear();
      return true;
    };
#else
    if VUNLIKELY (time.control_id == 0) {
      return;
    }
#endif

    {
      std::unique_lock lock(impl_->version_mtx);

      if VLIKELY (!time.hostname.empty()) {
        impl_->hostname_set.emplace(time.hostname);
      }

      if (impl_->hostname.empty()) {
        impl_->hostname = time.hostname;
      } else if (impl_->hostname != time.hostname) {
        lock.unlock();

#if VLINK_PROXY_ENABLE_HANDSHAKE
        if (token_mismatch) {
          if (clear_expected_token()) {
            process_error(kMultiProxyError);
          }

          return;
        }
#endif

        if VUNLIKELY (impl_->control_error_count.fetch_add(1, std::memory_order_relaxed) + 1 >= 2 &&
                      impl_->error_elapsed_timer.restart() >= 200) {
          process_error(kMultiProxyError);
        }

        return;
      }

      if VLIKELY (!time.machine_id.empty()) {
        impl_->machine_id_set.emplace(time.machine_id);
      }

      if (impl_->machine_id.empty()) {
        impl_->machine_id = time.machine_id;
      } else if (impl_->machine_id != time.machine_id) {
        lock.unlock();

#if VLINK_PROXY_ENABLE_HANDSHAKE
        if (token_mismatch) {
          if (clear_expected_token()) {
            process_error(kMultiProxyError);
          }

          return;
        }
#endif

        if VUNLIKELY (impl_->control_error_count.fetch_add(1, std::memory_order_relaxed) + 1 >= 2 &&
                      impl_->error_elapsed_timer.restart() >= 200) {
          process_error(kMultiProxyError);
        }

        return;
      }
    }

#if VLINK_PROXY_ENABLE_HANDSHAKE
    if VUNLIKELY (token_mismatch) {
      if (clear_expected_token()) {
        process_error(kTokenError);
      }

      return;
    }

    if VUNLIKELY (time.control_id == 0) {
      return;
    }
#endif

    if (impl_->config.role == kController) {
      if VUNLIKELY (time.control_id != impl_->control_id) {
        if VUNLIKELY (impl_->control_error_count.fetch_add(1, std::memory_order_relaxed) + 1 >= 2 &&
                      impl_->error_elapsed_timer.restart() >= 200) {
          process_error(kControlError);
        }

        return;
      }

      if VUNLIKELY (time.mode != impl_->mode.load(std::memory_order_relaxed)) {
        return;
      }
    }

    {
      std::lock_guard lock(impl_->version_mtx);
      impl_->proxy_version = time.version;
    }

    if (impl_->config.match_version) {
      if VUNLIKELY (time.version != VLINK_VERSION) {
        if VUNLIKELY (impl_->control_error_count.fetch_add(1, std::memory_order_relaxed) + 1 >= 2 &&
                      impl_->error_elapsed_timer.restart() >= 200) {
          process_error(kVersionCompError);
        }
        return;
      }
    }

    if VUNLIKELY (time.reliable_mode != impl_->config.reliable) {
      if VUNLIKELY (impl_->control_error_count.fetch_add(1, std::memory_order_relaxed) + 1 >= 2 &&
                    impl_->error_elapsed_timer.restart() >= 200) {
        process_error(kReliableCompError);
      }
      return;
    }

    if VUNLIKELY (time.tcp_mode != impl_->config.enable_tcp) {
      if VUNLIKELY (impl_->control_error_count.fetch_add(1, std::memory_order_relaxed) + 1 >= 2 &&
                    impl_->error_elapsed_timer.restart() >= 200) {
        process_error(kTcpCompError);
      }
      return;
    }

    if VUNLIKELY (time.direct_mode != impl_->config.direct) {
      if VUNLIKELY (impl_->control_error_count.fetch_add(1, std::memory_order_relaxed) + 1 >= 2 &&
                    impl_->error_elapsed_timer.restart() >= 200) {
        process_error(kDirectCompError);
      }
      return;
    }

    impl_->cpu_usage.store(time.cpu_usage, std::memory_order_relaxed);
    impl_->memory_usage.store(time.memory_usage, std::memory_order_relaxed);

    impl_->connect_elapsed_timer.restart();

    process_error(kNoError);
    process_time(time.sys_time, time.boot_time);
    process_connected(true);
  });

  impl_->info_sub->listen([this](const proxy::InfoListPacket& packet) {
    if VUNLIKELY (this->is_ready_to_quit()) {
      return;
    }

    if VUNLIKELY (packet.control_id == 0) {
      return;
    }

#if VLINK_PROXY_ENABLE_HANDSHAKE
    {
      std::shared_lock token_lock(impl_->token_mtx);

      if VUNLIKELY (impl_->token.empty()) {
        return;
      }
    }
#endif

    if VUNLIKELY (!packet.hostname.empty()) {
      std::shared_lock version_lock(impl_->version_mtx);
      if VUNLIKELY (packet.hostname != impl_->hostname) {
        return;
      }
    }

    if (impl_->config.role == kController) {
      if VUNLIKELY (packet.control_id != impl_->control_id) {
        return;
      }

      if (impl_->mode.load(std::memory_order_relaxed) != kObserveOne &&
          impl_->mode.load(std::memory_order_relaxed) != kObserveAll &&
          impl_->mode.load(std::memory_order_relaxed) != kAuto &&
          impl_->mode.load(std::memory_order_relaxed) != kAutoAndObserveAll) {
        return;
      }
    }

    if VUNLIKELY (impl_->error.load(std::memory_order_relaxed) != kNoError) {
      return;
    }

    impl_->connect_elapsed_timer.restart();

    std::vector<Info> info_list = packet.info_list;

    for (auto& info : info_list) {
      if VUNLIKELY (!SchemaData::is_valid_type(info.schema)) {
        info.schema = SchemaType::kUnknown;
      }
    }

    if (impl_->config.direct) {
      Control control;

      {
        std::shared_lock control_lock(impl_->control_mtx);
        control = impl_->last_control;
      }

      {
        std::unique_lock direct_lock(impl_->direct_mtx);
        impl_->direct_info_list = info_list;
      }

      sync_direct_maps(control);
    }

    std::shared_lock lock(impl_->info_mtx);

    if VLIKELY (impl_->info_callback) {
      impl_->info_callback(info_list);
    }
  });

  impl_->control_pub->detect_subscribers([this](bool connected) {
    // VLOG_I("Proxy Connected: ", connected);

    {
      std::shared_lock lock(impl_->control_mtx);

      if (!is_ready_to_quit() && connected && impl_->last_control.mode != kOffline &&
          impl_->config.role == kController) {
        post_task([this, control = impl_->last_control]() {
          if VUNLIKELY (!impl_->control_ret.load(std::memory_order_acquire)) {
            send_control_sync(control);
          }
        });
      }
    }

    if VUNLIKELY (!connected) {
      process_connected(false);
    }
  });

  impl_->resetting.store(false, std::memory_order_relaxed);
}

void ProxyAPI::process_connected(bool connected) {
  if (impl_->is_connected.load(std::memory_order_relaxed) != connected) {
    impl_->is_connected.store(connected, std::memory_order_relaxed);

    impl_->cpu_usage.store(0, std::memory_order_relaxed);
    impl_->memory_usage.store(0, std::memory_order_relaxed);

    if (!impl_->is_connected.load(std::memory_order_relaxed)) {
      std::lock_guard version_lock(impl_->version_mtx);
      impl_->hostname.clear();
      // impl_->hostname_set.clear();

      impl_->machine_id.clear();
      // impl_->machine_id_set.clear();
    }

    {
      std::shared_lock lock(impl_->connect_mtx);

      if VLIKELY (impl_->connect_callback) {
        impl_->connect_callback(connected);
      }
    }

    if (!impl_->is_connected.load(std::memory_order_relaxed) && impl_->config.role == kController &&
        !impl_->resetting.load(std::memory_order_relaxed) &&
        (impl_->error.load(std::memory_order_relaxed) == kNoError ||
         impl_->error.load(std::memory_order_relaxed) == kTokenError ||
         impl_->error.load(std::memory_order_relaxed) == kVersionCompError)) {
      post_task([this]() { reset_handle(); });
    }
  }
}

void ProxyAPI::process_time(uint64_t sys_time, uint64_t boot_time) {
  impl_->sys_time.store(sys_time, std::memory_order_relaxed);
  impl_->boot_time.store(boot_time, std::memory_order_relaxed);
  impl_->sys_elapsed_timer.restart();
  impl_->boot_elapsed_timer.restart();

  std::shared_lock lock(impl_->time_mtx);

  if VLIKELY (impl_->time_callback) {
    impl_->time_callback(sys_time, boot_time);
  }
}

void ProxyAPI::process_error(Error error) {
  const Error prev_error = impl_->error.exchange(error, std::memory_order_acq_rel);

  if (prev_error != error) {
    if (error == kNoError) {
      impl_->control_error_count.store(0, std::memory_order_relaxed);
      impl_->error_elapsed_timer.stop();
    }

    std::shared_lock lock(impl_->error_mtx);

    if VLIKELY (impl_->error_callback) {
      impl_->error_callback(error);
    }
  }
}

}  // namespace vlink
