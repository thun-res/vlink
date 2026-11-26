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

#include <vlink/base/condition_variable.h>
#include <vlink/base/elapsed_timer.h>
#include <vlink/base/logger.h>
#include <vlink/base/logger_plugin_interface.h>
#include <vlink/base/memory_resource.h>
#include <vlink/base/utils.h>
#include <vlink/extension/security.h>
#include <vlink/publisher.h>
//
#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <exception>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

#include "../launcher_common.h"

namespace vlink {

namespace launcher {

static constexpr size_t kDefaultLogdQueueDepth = 2048U;
static constexpr size_t kMinLogdQueueDepth = 64U;
static constexpr size_t kMaxLogdQueueDepth = 65536U;
static constexpr uint32_t kInitialConnectWaitMs = 500U;
static constexpr uint32_t kFlushTimeoutMs = 2000U;
static thread_local bool g_in_logd_sender = false;  // NOLINT(cppcoreguidelines-avoid-non-const-global-variables)

struct LogdRecursionGuard final {
  bool prev{false};

  LogdRecursionGuard() noexcept : prev(g_in_logd_sender) { g_in_logd_sender = true; }

  ~LogdRecursionGuard() noexcept { g_in_logd_sender = prev; }
};

static std::string extract_scheme(std::string_view url) {
  const size_t pos = url.find("://");

  if (pos == std::string_view::npos || pos == 0U) {
    return {};
  }

  return std::string(url.substr(0U, pos));
}

class LauncherLogdPlugin final : public LoggerPluginInterface {
  VLINK_PLUGIN_REGISTER(LoggerPluginInterface)

 public:
  LauncherLogdPlugin() = default;

  ~LauncherLogdPlugin() override { shutdown(); }

  bool init(std::string_view app_name) noexcept override {
    LogdRecursionGuard guard;
    (void)guard;

    try {
      {
        std::lock_guard lock(mtx_);

        if (running_) {
          return true;
        }
      }

      const std::string url = Utils::get_env("VLINK_LOGD_URL");

      if (url.empty()) {
        return false;
      }

      const std::string unit_env = Utils::get_env("VLINK_LOGD_UNIT");
      std::string resolved_unit;

      if (!unit_env.empty()) {
        resolved_unit = unit_env;
      } else if (!app_name.empty()) {
        resolved_unit = std::string(app_name);
      } else {
        resolved_unit = "vlink";
      }

      if (resolved_unit.size() > kMaxShortStr) {
        resolved_unit.resize(kMaxShortStr);
      }

      std::string scheme = Utils::get_env("VLINK_LOGD_SCHEME");

      if (scheme.empty()) {
        scheme = extract_scheme(url);
      }

      const std::string bind_ip = Utils::get_env("VLINK_LOGD_BIND_IP");
      const bool native_mode = (Utils::get_env("VLINK_LOGD_NATIVE") == "1");
      std::string sec_key = Utils::get_env("VLINK_LOGD_KEY");
      const std::string queue_env = Utils::get_env("VLINK_LOGD_QUEUE");
      size_t resolved_queue = kDefaultLogdQueueDepth;

      if (!queue_env.empty()) {
        size_t parsed = kDefaultLogdQueueDepth;
        const auto res = std::from_chars(queue_env.data(), queue_env.data() + queue_env.size(), parsed);

        if (res.ec == std::errc() && res.ptr == queue_env.data() + queue_env.size()) {
          resolved_queue = std::clamp(parsed, kMinLogdQueueDepth, kMaxLogdQueueDepth);
        }
      }

      std::shared_ptr<PlainLogPub> new_plain;
#ifdef VLINK_ENABLE_SECURITY
      std::shared_ptr<SecLogPub> new_sec;
#endif
      bool pub_ok = false;

      if (!sec_key.empty()) {
#ifdef VLINK_ENABLE_SECURITY
        Security::Config sec_cfg;
        sec_cfg.key = sec_key;
        sec_cfg.advanced.aad_context = "launcher.logd";
        wipe_secret_string(sec_key);
        new_sec = MemoryResource::make_shared<SecLogPub>(url, sec_cfg, InitType::kWithoutInit);
        wipe_secret_string(sec_cfg.key);
        new_sec->set_discovery_enabled(false);
        new_sec->set_safety_quit(true);
        apply_control_node_properties(*new_sec, scheme, bind_ip, native_mode);
        pub_ok = new_sec->init();
#else
        wipe_secret_string(sec_key);
        return false;
#endif
      } else {
        new_plain = MemoryResource::make_shared<PlainLogPub>(url, InitType::kWithoutInit);
        new_plain->set_discovery_enabled(false);
        new_plain->set_safety_quit(true);
        apply_control_node_properties(*new_plain, scheme, bind_ip, native_mode);
        pub_ok = new_plain->init();
      }

      if VUNLIKELY (!pub_ok) {
        new_plain.reset();
#ifdef VLINK_ENABLE_SECURITY
        new_sec.reset();
#endif
        return false;
      }

      {
        std::lock_guard lock(mtx_);

        if (running_) {
          return true;
        }

        unit_name_ = std::move(resolved_unit);
        max_queue_ = resolved_queue;
        connected_once_ = false;
        plain_pub_ = std::move(new_plain);
#ifdef VLINK_ENABLE_SECURITY
        sec_pub_ = std::move(new_sec);
#endif
        stop_requested_ = false;
        flush_requested_ = false;
        sender_thread_ = std::thread([this] { run_sender(); });
        running_ = true;
      }

      return true;
    } catch (const std::exception& e) {
      std::fprintf(stderr, "LauncherLogdPlugin: init failed: %s\n", e.what());
      shutdown();
      return false;
    } catch (...) {
      std::fprintf(stderr, "LauncherLogdPlugin: init failed with unknown exception.\n");
      shutdown();
      return false;
    }
  }

  bool log(int level, std::string_view str) noexcept override {
    if VUNLIKELY (g_in_logd_sender) {
      return false;
    }

    try {
      while (!str.empty() && (str.back() == '\n' || str.back() == '\r')) {
        str.remove_suffix(1U);
      }

      if VUNLIKELY (str.empty()) {
        return true;
      }

      LogRecord rec;
      rec.timestamp_ms = ElapsedTimer::get_sys_timestamp(ElapsedTimer::kMilli);
      rec.level =
          static_cast<uint8_t>(std::clamp(level, static_cast<int>(Logger::kTrace), static_cast<int>(Logger::kFatal)));
      rec.is_stderr = (level >= static_cast<int>(Logger::kWarn));

      if VUNLIKELY (str.size() > kMaxLogStr) {
        rec.text.assign(str.data(), kMaxLogStr - 16U);
        rec.text.append("...[truncated]");
      } else {
        rec.text.assign(str.data(), str.size());
      }

      {
        std::lock_guard lock(mtx_);

        if VUNLIKELY (!running_ || stop_requested_) {
          return false;
        }

        rec.unit_name = unit_name_;

        if VUNLIKELY (queue_.size() >= max_queue_) {
          queue_.pop_front();
        }

        rec.seq = ++enqueued_seq_;
        queue_.push_back(std::move(rec));
      }

      cv_.notify_one();
      return true;
    } catch (const std::exception& e) {
      std::fprintf(stderr, "LauncherLogdPlugin: log failed: %s\n", e.what());
      return false;
    } catch (...) {
      std::fprintf(stderr, "LauncherLogdPlugin: log failed with unknown exception.\n");
      return false;
    }
  }

  void flush() noexcept override {
    if VUNLIKELY (g_in_logd_sender) {
      return;
    }

    try {
      std::unique_lock lock(mtx_);

      if VUNLIKELY (!running_) {
        return;
      }

      const uint64_t target = enqueued_seq_;

      if (drained_seq_ >= target) {
        return;
      }

      flush_requested_ = true;
      cv_.notify_one();
      (void)flush_cv_.wait_for(lock, std::chrono::milliseconds(kFlushTimeoutMs),
                               [&] { return drained_seq_ >= target || !running_; });
    } catch (const std::exception& e) {
      std::fprintf(stderr, "LauncherLogdPlugin: flush failed: %s\n", e.what());
    } catch (...) {
      std::fprintf(stderr, "LauncherLogdPlugin: flush failed with unknown exception.\n");
    }
  }

 private:
  using PlainLogPub = Publisher<LogRecord>;
#ifdef VLINK_ENABLE_SECURITY
  using SecLogPub = SecurityPublisher<LogRecord>;
#endif

  [[nodiscard]] bool has_subscribers() const noexcept {
    if (plain_pub_) {
      return plain_pub_->has_subscribers();
    }

#ifdef VLINK_ENABLE_SECURITY
    if (sec_pub_) {
      return sec_pub_->has_subscribers();
    }
#endif

    return false;
  }

  void wait_initial_subscriber() noexcept {
    if (connected_once_ || has_subscribers()) {
      connected_once_ = true;
      return;
    }

    const uint64_t deadline = ElapsedTimer::get_cpu_timestamp(ElapsedTimer::kMilli) + kInitialConnectWaitMs;

    while (!has_subscribers()) {
      const uint64_t now = ElapsedTimer::get_cpu_timestamp(ElapsedTimer::kMilli);

      if (now >= deadline) {
        break;
      }

      const uint64_t slice = std::min<uint64_t>(20U, deadline - now);
      std::unique_lock lock(mtx_);

      if (stop_requested_ && !flush_requested_) {
        break;
      }

      (void)cv_.wait_for(lock, std::chrono::milliseconds(slice), [&] { return stop_requested_ && !flush_requested_; });
    }

    connected_once_ = true;
  }

  void publish_record(const LogRecord& rec) noexcept {
    try {
      if (plain_pub_) {
        (void)plain_pub_->publish(rec);
        return;
      }

#ifdef VLINK_ENABLE_SECURITY
      if (sec_pub_) {
        (void)sec_pub_->publish(rec);
      }
#endif
    } catch (const std::exception& e) {
      std::fprintf(stderr, "LauncherLogdPlugin: publish failed: %s\n", e.what());
    } catch (...) {
      std::fprintf(stderr, "LauncherLogdPlugin: publish failed with unknown exception.\n");
    }
  }

  void run_sender() noexcept {
    LogdRecursionGuard guard;
    (void)guard;

    while (true) {
      std::deque<LogRecord> batch;

      {
        std::unique_lock lock(mtx_);
        cv_.wait(lock, [&] { return !queue_.empty() || stop_requested_; });

        if (queue_.empty() && stop_requested_) {
          running_ = false;
          flush_cv_.notify_all();
          break;
        }

        batch.swap(queue_);
      }

      if (!connected_once_) {
        wait_initial_subscriber();
      }

      uint64_t last_seq = 0U;

      for (const auto& rec : batch) {
        publish_record(rec);
        last_seq = rec.seq;
      }

      {
        std::lock_guard lock(mtx_);

        if (last_seq > drained_seq_) {
          drained_seq_ = last_seq;
        }

        const bool notify_flush = flush_requested_;

        if (queue_.empty()) {
          flush_requested_ = false;
        }

        if (notify_flush) {
          flush_cv_.notify_all();
        }
      }
    }
  }

  void shutdown() noexcept {
    LogdRecursionGuard guard;
    (void)guard;

    try {
      {
        std::unique_lock lock(mtx_);

        if (running_ && drained_seq_ < enqueued_seq_) {
          const uint64_t target = enqueued_seq_;
          flush_requested_ = true;
          cv_.notify_one();
          (void)flush_cv_.wait_for(lock, std::chrono::milliseconds(kFlushTimeoutMs),
                                   [&] { return drained_seq_ >= target || !running_; });
        }

        stop_requested_ = true;
      }

      cv_.notify_all();

      if (sender_thread_.joinable()) {
        sender_thread_.join();
      }

      std::shared_ptr<PlainLogPub> old_plain;
#ifdef VLINK_ENABLE_SECURITY
      std::shared_ptr<SecLogPub> old_sec;
#endif

      {
        std::lock_guard lock(mtx_);
        running_ = false;
        queue_.clear();
        old_plain.swap(plain_pub_);
#ifdef VLINK_ENABLE_SECURITY
        old_sec.swap(sec_pub_);
#endif
      }

      flush_cv_.notify_all();
      old_plain.reset();
#ifdef VLINK_ENABLE_SECURITY
      old_sec.reset();
#endif
    } catch (const std::exception& e) {
      std::fprintf(stderr, "LauncherLogdPlugin: shutdown failed: %s\n", e.what());
    } catch (...) {
      std::fprintf(stderr, "LauncherLogdPlugin: shutdown failed with unknown exception.\n");
    }
  }

  std::string unit_name_;
  size_t max_queue_{kDefaultLogdQueueDepth};
  bool connected_once_{false};

  std::shared_ptr<PlainLogPub> plain_pub_;
#ifdef VLINK_ENABLE_SECURITY
  std::shared_ptr<SecLogPub> sec_pub_;
#endif

  std::mutex mtx_;
  ConditionVariable cv_;
  ConditionVariable flush_cv_;
  std::deque<LogRecord> queue_;
  uint64_t enqueued_seq_{0};
  uint64_t drained_seq_{0};
  bool running_{false};
  bool stop_requested_{false};
  bool flush_requested_{false};
  std::thread sender_thread_;
};

}  // namespace launcher

}  // namespace vlink

VLINK_PLUGIN_DECLARE(vlink::launcher::LauncherLogdPlugin, 1, 0)
