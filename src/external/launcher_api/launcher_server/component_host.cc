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

#include "./component_host.h"

#include <vlink/base/deadline_timer.h>
#include <vlink/base/elapsed_timer.h>
#include <vlink/base/helpers.h>
#include <vlink/base/logger.h>
#include <vlink/base/memory_resource.h>
#include <vlink/base/timer.h>
#include <vlink/external/launcher/context.h>
//
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <ctime>
#include <deque>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include "../launcher_common.h"
#include "./log_manager.h"

#if defined(__APPLE__)
#include <mach/mach_init.h>
#include <mach/thread_act.h>
#include <mach/thread_info.h>
#elif defined(_WIN32) || defined(_WIN64)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace vlink {

namespace launcher {

static uint64_t current_thread_cpu_ns() noexcept {
#if defined(__linux__) || defined(__QNX__) || defined(__QNXNTO__)
  struct timespec ts{};

  if VLIKELY (::clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts) == 0) {
    return static_cast<uint64_t>(ts.tv_sec) * 1000000000ULL + static_cast<uint64_t>(ts.tv_nsec);
  }

  return 0ULL;
#elif defined(__APPLE__)
  thread_basic_info_data_t info{};
  mach_msg_type_number_t count = THREAD_BASIC_INFO_COUNT;
  const mach_port_t self_thread = ::mach_thread_self();
  const kern_return_t kr =
      ::thread_info(self_thread, THREAD_BASIC_INFO, reinterpret_cast<thread_info_t>(&info), &count);
  (void)::mach_port_deallocate(::mach_task_self(), self_thread);

  if VLIKELY (kr == KERN_SUCCESS) {
    const uint64_t user_us =
        static_cast<uint64_t>(info.user_time.seconds) * 1000000ULL + static_cast<uint64_t>(info.user_time.microseconds);
    const uint64_t sys_us = static_cast<uint64_t>(info.system_time.seconds) * 1000000ULL +
                            static_cast<uint64_t>(info.system_time.microseconds);
    return (user_us + sys_us) * 1000ULL;
  }

  return 0ULL;
#elif defined(_WIN32) || defined(_WIN64)
  FILETIME creation_ft{};
  FILETIME exit_ft{};
  FILETIME kernel_ft{};
  FILETIME user_ft{};

  if VLIKELY (::GetThreadTimes(::GetCurrentThread(), &creation_ft, &exit_ft, &kernel_ft, &user_ft) != FALSE) {
    auto ft_to_u64 = [](const FILETIME& ft) noexcept -> uint64_t {
      return (static_cast<uint64_t>(ft.dwHighDateTime) << 32U) | static_cast<uint64_t>(ft.dwLowDateTime);
    };
    return (ft_to_u64(kernel_ft) + ft_to_u64(user_ft)) * 100ULL;
  }

  return 0ULL;
#else
  return 0ULL;
#endif
}

struct ThreadCpuFrame final {
  std::atomic<uint64_t>* counter{nullptr};
  uint64_t start_cpu_ns{0};
  uint32_t depth{0};
};

static constexpr size_t kMaxThreadCpuFrames = 16U;
static thread_local std::array<ThreadCpuFrame, kMaxThreadCpuFrames>
    g_thread_cpu_frames{};                           // NOLINT(cppcoreguidelines-avoid-non-const-global-variables)
static thread_local size_t g_thread_cpu_depth = 0U;  // NOLINT(cppcoreguidelines-avoid-non-const-global-variables)

enum class DispatchStatus : uint8_t {
  kCompleted = 0,
  kRejected = 1,
  kTimedOut = 2,
};

struct DispatchState final {
  std::mutex mtx;
  ConditionVariable cv;
  bool done{false};
  MoveFunction<void()> fn;
};

static DispatchStatus dispatch_on_loop_bounded(MessageLoop* loop, uint32_t timeout_ms, MoveFunction<void()>&& fn) {
  if (loop == nullptr || !loop->is_running() || loop->is_in_same_thread()) {
    if (fn) {
      fn();
    }

    return DispatchStatus::kCompleted;
  }

  const uint32_t budget_ms = (timeout_ms == 0U) ? 1U : timeout_ms;
  DeadlineTimer deadline(static_cast<int64_t>(budget_ms), ElapsedTimer::kMilli);
  auto state = MemoryResource::make_shared<DispatchState>();
  state->fn = std::move(fn);

  PostTaskOptions opts;
  opts.overflow_policy = TaskOverflowPolicy::kReject;

  if (loop->get_type() != MessageLoop::kLockfreeType) {
    opts.drop_policy = TaskDropPolicy::kProtected;
  }

  while (true) {
    if (!loop->is_running() || loop->is_ready_to_quit()) {
      return DispatchStatus::kRejected;
    }

    auto handle = loop->post_task_handle(
        [state]() mutable {
          struct FinishGuard final {
            std::shared_ptr<DispatchState> st;

            ~FinishGuard() noexcept {
              {
                std::lock_guard lk(st->mtx);
                st->done = true;
              }

              st->cv.notify_all();
            }
          } guard{state};
          (void)guard;

          if (state->fn) {
            state->fn();
          }
        },
        opts);

    const auto hstate = handle.state();

    if (hstate != TaskExecutionState::kRejected && hstate != TaskExecutionState::kCancelled &&
        hstate != TaskExecutionState::kInvalid) {
      break;
    }

    if (deadline.has_expired()) {
      return DispatchStatus::kTimedOut;
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }

  const int64_t rem_ms = std::max<int64_t>(1, deadline.remaining_time());
  std::unique_lock lk(state->mtx);
  const bool finished = state->cv.wait_for(lk, std::chrono::milliseconds(rem_ms), [&] { return state->done; });
  return finished ? DispatchStatus::kCompleted : DispatchStatus::kTimedOut;
}

class ComponentHost::ComponentContextImpl final : public LauncherContext,
                                                  public std::enable_shared_from_this<ComponentContextImpl> {
 public:
  using DeferredDispatchFn = Function<void(Function<void()>&&)>;

  ComponentContextImpl(std::string name, uint64_t generation, std::string ns, std::string group, MessageLoop& loop,
                       ThreadPool* pool, CancellationToken cancel_token,
                       std::shared_ptr<const std::atomic_bool> active_flag, uint16_t default_priority,
                       LauncherParamMap params, std::unordered_map<std::string, std::string> urls,
                       std::unordered_map<std::string, std::string> inputs,
                       std::unordered_map<std::string, std::string> outputs,
                       std::vector<std::pair<std::string, std::string>> remaps, std::string record_path,
                       bool record_force_publish, ReadyCallback on_ready, HealthCallback on_health,
                       WatchdogCallback on_watchdog, DeferredDispatchFn deferred_dispatch)
      : name_(std::move(name)),
        generation_(generation),
        ns_(std::move(ns)),
        group_(std::move(group)),
        loop_(loop),
        pool_(pool),
        cancel_token_(std::move(cancel_token)),
        active_flag_(std::move(active_flag)),
        default_priority_(default_priority),
        params_(std::move(params)),
        urls_(std::move(urls)),
        inputs_(std::move(inputs)),
        outputs_(std::move(outputs)),
        remaps_(std::move(remaps)),
        record_path_(std::move(record_path)),
        record_force_publish_(MemoryResource::make_shared<std::atomic_bool>(record_force_publish)),
        on_ready_(std::move(on_ready)),
        on_health_(std::move(on_health)),
        on_watchdog_(std::move(on_watchdog)),
        deferred_dispatch_(std::move(deferred_dispatch)),
        last_sample_wall_ns_(ElapsedTimer::get_cpu_timestamp(ElapsedTimer::kNano)) {
#ifdef VLINK_ENABLE_COROUTINE
    coro_ops_.spawn = &ComponentContextImpl::coro_spawn_entry;
    coro_ops_.sleep = &ComponentContextImpl::coro_sleep_entry;
    coro_ops_.await_stop = &ComponentContextImpl::coro_await_stop_entry;
    coro_ops_.host_ctx = this;
#endif
  }

  [[nodiscard]] const std::string& name() const noexcept override { return name_; }

  [[nodiscard]] const std::string& ns() const noexcept override { return ns_; }

  [[nodiscard]] const std::string& scheduler_group() const noexcept override { return group_; }

  [[nodiscard]] MessageLoop& loop() noexcept override { return loop_; }

  [[nodiscard]] ThreadPool* thread_pool() noexcept override { return pool_; }

  [[nodiscard]] CancellationToken cancellation_token() const noexcept override { return cancel_token_; }

  [[nodiscard]] bool is_active() const noexcept override {
    return active_flag_ != nullptr && active_flag_->load(std::memory_order_acquire);
  }

  [[nodiscard]] std::shared_ptr<const std::atomic_bool> active_flag() const noexcept override { return active_flag_; }

  [[nodiscard]] std::string resolve_url(std::string_view url) const override {
    return resolve_unit_url(url, ns_, remaps_);
  }

  [[nodiscard]] std::unordered_map<std::string, std::string> urls() const override {
    std::unordered_map<std::string, std::string> out;

    for (const auto& [k, v] : outputs_) {
      out[k] = resolve_url(v);
    }

    for (const auto& [k, v] : inputs_) {
      out[k] = resolve_url(v);
    }

    for (const auto& [k, v] : urls_) {
      out[k] = resolve_url(v);
    }

    return out;
  }

  [[nodiscard]] std::unordered_map<std::string, std::string> input_urls() const override {
    std::unordered_map<std::string, std::string> out;
    out.reserve(inputs_.size());

    for (const auto& [k, v] : inputs_) {
      out[k] = resolve_url(v);
    }

    return out;
  }

  [[nodiscard]] std::unordered_map<std::string, std::string> output_urls() const override {
    std::unordered_map<std::string, std::string> out;
    out.reserve(outputs_.size());

    for (const auto& [k, v] : outputs_) {
      out[k] = resolve_url(v);
    }

    return out;
  }

  [[nodiscard]] std::string get_url(std::string_view key, std::string_view default_url = "") const override {
    const std::string k(key);
    const auto url_it = urls_.find(k);

    if (url_it != urls_.end()) {
      return resolve_url(url_it->second);
    }

    const auto in_it = inputs_.find(k);

    if (in_it != inputs_.end()) {
      return resolve_url(in_it->second);
    }

    const auto out_it = outputs_.find(k);

    if (out_it != outputs_.end()) {
      return resolve_url(out_it->second);
    }

    return default_url.empty() ? std::string() : resolve_url(default_url);
  }

  [[nodiscard]] std::string get_input_url(std::string_view key, std::string_view default_url = "") const override {
    const std::string k(key);
    const auto in_it = inputs_.find(k);

    if VLIKELY (in_it != inputs_.end()) {
      return resolve_url(in_it->second);
    }

    const auto url_it = urls_.find(k);

    if (url_it != urls_.end()) {
      return resolve_url(url_it->second);
    }

    return default_url.empty() ? std::string() : resolve_url(default_url);
  }

  [[nodiscard]] std::string get_output_url(std::string_view key, std::string_view default_url = "") const override {
    const std::string k(key);
    const auto out_it = outputs_.find(k);

    if VLIKELY (out_it != outputs_.end()) {
      return resolve_url(out_it->second);
    }

    const auto url_it = urls_.find(k);

    if (url_it != urls_.end()) {
      return resolve_url(url_it->second);
    }

    return default_url.empty() ? std::string() : resolve_url(default_url);
  }

  [[nodiscard]] LauncherParamMap params() const override {
    std::lock_guard lock(param_mtx_);
    return params_;
  }

  [[nodiscard]] bool has_param(std::string_view key) const override {
    std::lock_guard lock(param_mtx_);
    return params_.count(std::string(key)) != 0U;
  }

  [[nodiscard]] bool get_param(std::string_view key, LauncherParamValue& out) const override {
    std::lock_guard lock(param_mtx_);
    auto it = params_.find(std::string(key));

    if (it == params_.end()) {
      return false;
    }

    out = it->second;
    return true;
  }

  void update_param(std::string_view key, const LauncherParamValue& value) {
    std::lock_guard lock(param_mtx_);
    params_[std::string(key)] = value;
  }

  [[nodiscard]] std::string record_path() const override {
    std::lock_guard lock(record_mtx_);
    return record_path_;
  }

  [[nodiscard]] bool record_force_publish() const noexcept override {
    return record_force_publish_ != nullptr && record_force_publish_->load(std::memory_order_acquire);
  }

  [[nodiscard]] std::shared_ptr<const std::atomic_bool> record_force_publish_flag() const noexcept override {
    return record_force_publish_;
  }

  using RecordHookFn = Function<bool(const std::string&)>;

  void register_record_hook(RecordHookFn&& hook) const override {
    if (!hook) {
      return;
    }

    auto fn = std::make_shared<RecordHookFn>(std::move(hook));
    std::string cur_path;

    {
      std::lock_guard lock(record_mtx_);
      cur_path = record_path_;

      if (cur_path.empty()) {
        record_hooks_.push_back(std::move(fn));
        return;
      }
    }

    while (true) {
      if (!(*fn)(cur_path)) {
        return;
      }

      std::lock_guard lock(record_mtx_);

      if (record_path_ == cur_path) {
        record_hooks_.push_back(std::move(fn));
        return;
      }

      cur_path = record_path_;
    }
  }

  void clear_record_hooks() noexcept {
    std::vector<std::shared_ptr<RecordHookFn>> stale;

    {
      std::lock_guard lock(record_mtx_);
      stale.swap(record_hooks_);
    }

    stale.clear();
  }

  void register_cleanup_hook(Function<void()>&& hook) const override {
    if (!hook) {
      return;
    }

    std::lock_guard lock(record_mtx_);
    cleanup_hooks_.push_back(std::move(hook));
  }

  void run_cleanup_hooks() noexcept {
    std::vector<Function<void()>> hooks;

    {
      std::lock_guard lock(record_mtx_);
      hooks.swap(cleanup_hooks_);
    }

    for (auto& hook : hooks) {
      if (hook) {
        try {
          hook();
        } catch (const std::exception& e) {
          VLOG_W("ComponentHost: Component '", name_, "' cleanup hook threw: ", e.what(), ".");
        } catch (...) {
          VLOG_W("ComponentHost: Component '", name_, "' cleanup hook threw unknown exception.");
        }
      }
    }
  }

  void apply_record_config(const std::string& path, bool force_publish) {
    std::vector<std::shared_ptr<RecordHookFn>> snapshot;
    std::string prev_path;

    {
      std::lock_guard lock(record_mtx_);
      prev_path = record_path_;
      record_path_ = path;
      snapshot = record_hooks_;
    }

    std::vector<const RecordHookFn*> dead;
    size_t applied_count = 0U;
    std::exception_ptr first_error;

    for (size_t i = 0U; i < snapshot.size(); ++i) {
      const auto& fn = snapshot[i];

      if (!fn || !(*fn)) {
        dead.push_back(fn.get());
        continue;
      }

      try {
        if ((*fn)(path)) {
          applied_count = i + 1U;
        } else {
          dead.push_back(fn.get());
        }
      } catch (...) {
        first_error = std::current_exception();
        break;
      }
    }

    auto prune_dead = [&]() {
      if (!dead.empty()) {
        record_hooks_.erase(std::remove_if(record_hooks_.begin(), record_hooks_.end(),
                                           [&](const std::shared_ptr<RecordHookFn>& fn) {
                                             return std::find(dead.begin(), dead.end(), fn.get()) != dead.end();
                                           }),
                            record_hooks_.end());
      }
    };

    if (first_error) {
      for (size_t i = 0U; i < applied_count; ++i) {
        const auto& fn = snapshot[i];

        if (fn && *fn && std::find(dead.begin(), dead.end(), fn.get()) == dead.end()) {
          try {
            if (!(*fn)(prev_path)) {
              dead.push_back(fn.get());
            }
          } catch (const std::exception& e) {
            VLOG_W("ComponentHost: Component '", name_, "' record rollback hook threw: ", e.what(), ".");
          } catch (...) {
            VLOG_W("ComponentHost: Component '", name_, "' record rollback hook threw unknown exception.");
          }
        }
      }

      {
        std::lock_guard lock(record_mtx_);
        record_path_ = prev_path;
        prune_dead();
      }

      std::rethrow_exception(first_error);
    }

    if (!dead.empty()) {
      std::lock_guard lock(record_mtx_);
      prune_dead();
    }

    if (record_force_publish_ != nullptr) {
      record_force_publish_->store(force_publish, std::memory_order_release);
    }
  }

  void reset_ready_signal() noexcept {
    std::lock_guard lock(ready_mtx_);
    ready_signalled_ = false;
    deferred_ready_pending_ = false;
  }

  void set_defer_ready(bool defer) noexcept {
    std::lock_guard lock(ready_mtx_);
    defer_ready_ = defer;
  }

  void flush_deferred_ready() noexcept {
    bool should_fire = false;

    {
      std::lock_guard lock(ready_mtx_);
      defer_ready_ = false;

      if (deferred_ready_pending_ && !ready_signalled_) {
        deferred_ready_pending_ = false;
        ready_signalled_ = true;
        should_fire = true;
      }
    }

    if (should_fire) {
      invoke_ready_callback();
    }
  }

  void mark_ready() noexcept override {
    bool should_fire = false;

    {
      std::lock_guard lock(ready_mtx_);

      if (ready_signalled_) {
        return;
      }

      if (defer_ready_) {
        deferred_ready_pending_ = true;
        return;
      }

      ready_signalled_ = true;
      should_fire = true;
    }

    if (should_fire) {
      invoke_ready_callback();
    }
  }

  void report_health(bool healthy, std::string_view detail) noexcept override {
    if (on_health_) {
      try {
        on_health_(name_, generation_, healthy, std::string(detail));
      } catch (const std::exception& e) {
        VLOG_W("ComponentHost: Component '", name_, "' report_health callback threw: ", e.what(), ".");
      } catch (...) {
        VLOG_W("ComponentHost: Component '", name_, "' report_health callback threw unknown exception.");
      }
    }
  }

  void watchdog_ping() noexcept override {
    last_watchdog_ms_.store(ElapsedTimer::get_cpu_timestamp(ElapsedTimer::kMilli), std::memory_order_release);

    if (on_watchdog_) {
      try {
        on_watchdog_(name_, generation_);
      } catch (const std::exception& e) {
        VLOG_W("ComponentHost: Component '", name_, "' watchdog callback threw: ", e.what(), ".");
      } catch (...) {
        VLOG_W("ComponentHost: Component '", name_, "' watchdog callback threw unknown exception.");
      }
    }
  }

  [[nodiscard]] uint64_t last_watchdog_ms() const noexcept { return last_watchdog_ms_.load(std::memory_order_acquire); }

  [[nodiscard]] double get_cpu_usage() const noexcept override {
    const uint64_t now_wall_ns = ElapsedTimer::get_cpu_timestamp(ElapsedTimer::kNano);
    const uint64_t total_cpu_ns = active_cpu_ns_.load(std::memory_order_relaxed);
    std::lock_guard lock(prof_mtx_);

    if (now_wall_ns > last_sample_wall_ns_) {
      const uint64_t d_wall = now_wall_ns - last_sample_wall_ns_;

      if (d_wall >= 10000000ULL) {
        const uint64_t d_cpu = (total_cpu_ns >= last_sample_cpu_ns_) ? (total_cpu_ns - last_sample_cpu_ns_) : 0ULL;
        last_cpu_percent_ = (static_cast<double>(d_cpu) * 100.0) / static_cast<double>(d_wall);
        last_sample_cpu_ns_ = total_cpu_ns;
        last_sample_wall_ns_ = now_wall_ns;
      }
    }

    return last_cpu_percent_;
  }

  [[nodiscard]] uintptr_t enter_callback_scope() const noexcept override {
    ScopedComponentLogTag::push_tag(name_);
    const uint64_t now_cpu = current_thread_cpu_ns();

    if (g_thread_cpu_depth > 0U && g_thread_cpu_depth <= kMaxThreadCpuFrames) {
      auto& top = g_thread_cpu_frames[g_thread_cpu_depth - 1U];

      if (top.counter == &active_cpu_ns_) {
        ++top.depth;
        return 1U;
      }

      if (now_cpu > top.start_cpu_ns && top.counter != nullptr) {
        top.counter->fetch_add(now_cpu - top.start_cpu_ns, std::memory_order_relaxed);
      }

      top.start_cpu_ns = now_cpu;
    }

    if VLIKELY (g_thread_cpu_depth < kMaxThreadCpuFrames) {
      g_thread_cpu_frames[g_thread_cpu_depth] = ThreadCpuFrame{&active_cpu_ns_, now_cpu, 1U};
    }

    ++g_thread_cpu_depth;
    return 1U;
  }

  void exit_callback_scope(uintptr_t token) const noexcept override {
    if (token != 0U) {
      const uint64_t now_cpu = current_thread_cpu_ns();

      if VLIKELY (g_thread_cpu_depth > 0U) {
        if (g_thread_cpu_depth <= kMaxThreadCpuFrames) {
          auto& top = g_thread_cpu_frames[g_thread_cpu_depth - 1U];

          if (top.depth > 1U) {
            --top.depth;
            ScopedComponentLogTag::pop_tag();
            return;
          }

          if (now_cpu > top.start_cpu_ns && top.counter != nullptr) {
            top.counter->fetch_add(now_cpu - top.start_cpu_ns, std::memory_order_relaxed);
          }

          top = ThreadCpuFrame{};
        }

        --g_thread_cpu_depth;

        if (g_thread_cpu_depth > 0U && g_thread_cpu_depth <= kMaxThreadCpuFrames) {
          g_thread_cpu_frames[g_thread_cpu_depth - 1U].start_cpu_ns = now_cpu;
        }
      }

      ScopedComponentLogTag::pop_tag();
    }
  }

  [[nodiscard]] const void* query_extension(std::string_view extension_id) const noexcept override {
#ifdef VLINK_ENABLE_COROUTINE
    if (extension_id == LauncherCoroutineOps::kExtensionId) {
      return &coro_ops_;
    }
#else
    (void)extension_id;
#endif
    return nullptr;
  }

  std::shared_ptr<void> acquire_task_guard() noexcept override {
    std::lock_guard lock(inflight_mtx_);

    if VUNLIKELY (!accepting_tasks_) {
      return nullptr;
    }

    ++in_flight_tasks_;
    auto self = shared_from_this();
    return std::shared_ptr<void>(self.get(), [self](void* ptr) noexcept {
      (void)ptr;
      self->release_task_guard();
    });
  }

  void stop_accepting() noexcept {
    std::lock_guard lock(inflight_mtx_);
    accepting_tasks_ = false;
  }

  [[nodiscard]] size_t in_flight_count() const noexcept {
    std::lock_guard lock(inflight_mtx_);
    return in_flight_tasks_;
  }

  [[nodiscard]] bool wait_drained(int timeout_ms = 5000) {
    std::unique_lock lock(inflight_mtx_);
    accepting_tasks_ = false;

    if (in_flight_tasks_ == 0U) {
      return true;
    }

    if (timeout_ms <= 0) {
      return false;
    }

    return inflight_cv_.wait_for(lock, std::chrono::milliseconds(timeout_ms),
                                 [this] { return in_flight_tasks_ == 0U; });
  }

  void set_deferred_cleanup(Function<void()> cleanup_fn) {
    Function<void()> immediate;

    {
      std::lock_guard lock(inflight_mtx_);

      if (in_flight_tasks_ == 0U) {
        immediate = std::move(cleanup_fn);
      } else {
        deferred_cleanup_ = std::move(cleanup_fn);
      }
    }

    if (immediate) {
      if (deferred_dispatch_) {
        deferred_dispatch_(std::move(immediate));
      } else {
        immediate();
      }
    }
  }

  bool post_task(MessageLoop::Callback&& task, uint16_t priority) override {
    const auto handle = post_task_handle(std::move(task), priority, {});
    const auto st = handle.state();
    return st != TaskExecutionState::kRejected && st != TaskExecutionState::kCancelled &&
           st != TaskExecutionState::kInvalid;
  }

  TaskHandle post_task_handle(MessageLoop::Callback&& task, uint16_t priority,
                              const PostTaskOptions& options) override {
    auto guard = acquire_task_guard();

    if VUNLIKELY (!guard) {
      return TaskHandle{};
    }

    PostTaskOptions effective = options;

    if (!effective.cancellation_token.valid()) {
      effective.cancellation_token = cancel_token_;
    }

    const uint16_t eff_prio = (priority == MessageLoop::kNoPriority) ? default_priority_ : priority;
    auto wrapped = wrap_guarded_task(std::move(guard), std::move(task), "task");

    if (loop_.get_type() == MessageLoop::kPriorityType && eff_prio != MessageLoop::kNoPriority) {
      return loop_.post_task_with_priority_handle(std::move(wrapped), eff_prio, effective);
    }

    return loop_.post_task_handle(std::move(wrapped), effective);
  }

  TaskHandle post_compute_task(ThreadPool::Callback&& task, const PostTaskOptions& options) override {
    if (pool_ == nullptr) {
      return post_task_handle(std::move(task), default_priority_, options);
    }

    auto guard = acquire_task_guard();

    if VUNLIKELY (!guard) {
      return TaskHandle{};
    }

    PostTaskOptions effective = options;

    if (!effective.cancellation_token.valid()) {
      effective.cancellation_token = cancel_token_;
    }

    return pool_->post_task_handle(wrap_guarded_task(std::move(guard), std::move(task), "compute task"), effective);
  }

 private:
  void invoke_ready_callback() const noexcept {
    if (on_ready_) {
      try {
        on_ready_(name_, generation_);
      } catch (const std::exception& e) {
        VLOG_W("ComponentHost: Component '", name_, "' mark_ready callback threw: ", e.what(), ".");
      } catch (...) {
        VLOG_W("ComponentHost: Component '", name_, "' mark_ready callback threw unknown exception.");
      }
    }
  }

  MoveFunction<void()> wrap_guarded_task(std::shared_ptr<void> guard, MoveFunction<void()>&& task, const char* kind) {
    auto self = shared_from_this();
    return [self, guard = std::move(guard), fn = std::move(task), kind]() mutable {
      (void)guard;
      const uintptr_t scope_tok = self->enter_callback_scope();

      struct ScopeExit final {
        const ComponentContextImpl* ctx;
        uintptr_t tok;

        ~ScopeExit() noexcept { ctx->exit_callback_scope(tok); }
      } scope_exit{self.get(), scope_tok};
      (void)scope_exit;

      try {
        if VLIKELY (fn) {
          fn();
        }
      } catch (const std::exception& e) {
        VLOG_E("ComponentHost: Component '", self->name_, "' ", kind, " threw exception: ", e.what(), ".");
        throw;
      } catch (...) {
        VLOG_E("ComponentHost: Component '", self->name_, "' ", kind, " threw unknown exception.");
        throw;
      }
    };
  }

#ifdef VLINK_ENABLE_COROUTINE
  static bool coro_spawn_entry(void* host_ctx, Co::Task<void>&& task, uint16_t priority) {
    auto* self = static_cast<ComponentContextImpl*>(host_ctx);

    if VUNLIKELY (self == nullptr || !self->loop_.is_running() || self->loop_.is_ready_to_quit()) {
      return false;
    }

    auto guard = self->acquire_task_guard();

    if VUNLIKELY (!guard) {
      return false;
    }

    Co::co_spawn_with_priority(self->loop_, run_guarded_coro_resident(self->name_, std::move(guard), std::move(task)),
                               priority);
    return true;
  }

  static Co::Task<bool> coro_sleep_entry(void* host_ctx, uint32_t ms, uint16_t priority) {
    auto* self = static_cast<ComponentContextImpl*>(host_ctx);
    return run_co_sleep(&self->loop_, self->cancel_token_, ms, priority);
  }

  static Co::Task<void> coro_await_stop_entry(void* host_ctx, uint16_t priority) {
    auto* self = static_cast<ComponentContextImpl*>(host_ctx);
    return run_co_await_stop(&self->loop_, self->cancel_token_, priority);
  }

  static Co::Task<void> run_guarded_coro_resident(std::string comp_name, std::shared_ptr<void> guard,
                                                  Co::Task<void> task) {
    auto g = std::move(guard);
    auto t = std::move(task);

    try {
      co_await t;
    } catch (const std::exception& e) {
      ScopedComponentLogTag log_tag(comp_name);
      VLOG_E("ComponentHost: Component '", comp_name, "' coroutine threw exception: ", e.what(), ".");
    } catch (...) {
      ScopedComponentLogTag log_tag(comp_name);
      VLOG_E("ComponentHost: Component '", comp_name, "' coroutine threw unknown exception.");
    }

    t = {};
    g.reset();
  }

  static Co::Task<bool> run_co_sleep(MessageLoop* target_loop, CancellationToken token, uint32_t ms,
                                     uint16_t priority) {
    if (token.is_cancellation_requested()) {
      co_return false;
    }

    auto prom = MemoryResource::make_shared<std::promise<bool>>();
    auto done = MemoryResource::make_shared<std::atomic_bool>(false);
    auto fut = prom->get_future();

    auto reg = token.register_callback([prom, done]() mutable {
      if (!done->exchange(true, std::memory_order_acq_rel)) {
        prom->set_value(false);
      }
    });

    Timer timer(ms, 1, [prom, done]() mutable {
      if (!done->exchange(true, std::memory_order_acq_rel)) {
        prom->set_value(true);
      }
    });

    if (priority > 0U) {
      timer.set_priority(priority);
    }

    if (!done->load(std::memory_order_acquire)) {
      if (timer.attach(target_loop)) {
        timer.start();
      } else if (!done->exchange(true, std::memory_order_acq_rel)) {
        prom->set_value(false);
      }
    }

    const bool elapsed = co_await Co::await_future(*target_loop, std::move(fut));
    timer.detach();
    reg.reset();
    co_return elapsed && !token.is_cancellation_requested();
  }

  static Co::Task<void> run_co_await_stop(MessageLoop* target_loop, CancellationToken token, uint16_t priority) {
    (void)priority;

    if (token.is_cancellation_requested()) {
      co_return;
    }

    auto prom = MemoryResource::make_shared<std::promise<void>>();
    auto done = MemoryResource::make_shared<std::atomic_bool>(false);
    auto fut = prom->get_future();

    auto reg = token.register_callback([prom, done]() mutable {
      if (!done->exchange(true, std::memory_order_acq_rel)) {
        prom->set_value();
      }
    });

    co_await Co::await_future(*target_loop, std::move(fut));
    reg.reset();
  }
#endif

  void release_task_guard() noexcept {
    Function<void()> cleanup_fn;

    {
      std::lock_guard lock(inflight_mtx_);

      if (in_flight_tasks_ > 0U) {
        --in_flight_tasks_;

        if (in_flight_tasks_ == 0U && !accepting_tasks_) {
          cleanup_fn = std::move(deferred_cleanup_);
          inflight_cv_.notify_all();
        }
      }
    }

    if (cleanup_fn) {
      if (deferred_dispatch_) {
        try {
          deferred_dispatch_(std::move(cleanup_fn));
          return;
        } catch (const std::exception& e) {
          VLOG_W("ComponentHost: Component '", name_, "' deferred dispatch threw: ", e.what(), ".");
        } catch (...) {
          VLOG_W("ComponentHost: Component '", name_, "' deferred dispatch threw unknown exception.");
        }
      }

      try {
        cleanup_fn();
      } catch (const std::exception& e) {
        VLOG_W("ComponentHost: Component '", name_, "' deferred cleanup threw: ", e.what(), ".");
      } catch (...) {
        VLOG_W("ComponentHost: Component '", name_, "' deferred cleanup threw unknown exception.");
      }
    }
  }

  std::string name_;
  uint64_t generation_{0};
  std::string ns_;
  std::string group_;
  MessageLoop& loop_;
  ThreadPool* pool_{nullptr};
  CancellationToken cancel_token_;
  std::shared_ptr<const std::atomic_bool> active_flag_;
  uint16_t default_priority_{MessageLoop::kNoPriority};
  mutable std::mutex param_mtx_;
  LauncherParamMap params_;
  std::unordered_map<std::string, std::string> urls_;
  std::unordered_map<std::string, std::string> inputs_;
  std::unordered_map<std::string, std::string> outputs_;
  std::vector<std::pair<std::string, std::string>> remaps_;
  mutable std::mutex record_mtx_;
  std::string record_path_;
  std::shared_ptr<std::atomic_bool> record_force_publish_;
  mutable std::vector<std::shared_ptr<RecordHookFn>> record_hooks_;
  mutable std::vector<Function<void()>> cleanup_hooks_;
  ReadyCallback on_ready_;
  HealthCallback on_health_;
  WatchdogCallback on_watchdog_;
  DeferredDispatchFn deferred_dispatch_;
  std::mutex ready_mtx_;
  bool ready_signalled_{false};
  bool defer_ready_{false};
  bool deferred_ready_pending_{false};
  std::atomic<uint64_t> last_watchdog_ms_{0};
  mutable std::atomic<uint64_t> active_cpu_ns_{0};
  mutable std::mutex prof_mtx_;
  mutable uint64_t last_sample_cpu_ns_{0};
  mutable uint64_t last_sample_wall_ns_{0};
  mutable double last_cpu_percent_{0.0};
#ifdef VLINK_ENABLE_COROUTINE
  LauncherCoroutineOps coro_ops_{};
#endif

  mutable std::mutex inflight_mtx_;
  ConditionVariable inflight_cv_;
  bool accepting_tasks_{true};
  size_t in_flight_tasks_{0};
  Function<void()> deferred_cleanup_;
};

ComponentHost::ComponentHost(SchedulerManager& scheduler)
    : life_state_(MemoryResource::make_shared<HostLifeState>()), scheduler_(scheduler) {
  plugin_loader_.set_log_level(Logger::kWarn);
}

ComponentHost::~ComponentHost() { stop_all(); }

void ComponentHost::set_plugin_dirs(const std::vector<std::string>& dirs) {
  std::lock_guard lock(mtx_);
  plugin_dirs_ = dirs;
}

void ComponentHost::set_callbacks(ReadyCallback&& on_ready, HealthCallback&& on_health,
                                  WatchdogCallback&& on_watchdog) {
  std::lock_guard lock(mtx_);
  on_ready_ = std::move(on_ready);
  on_health_ = std::move(on_health);
  on_watchdog_ = std::move(on_watchdog);
}

bool ComponentHost::try_post_deferred_cleanup(const Function<void()>& fn) {
  if (!fn) {
    return true;
  }

  std::lock_guard lock(cleanup_loop_mtx_);

  if (!cleanup_loop_) {
    cleanup_loop_ = std::make_unique<MessageLoop>(MessageLoop::kNormalType);
    cleanup_loop_->set_name("vl_comp_clean");
    cleanup_loop_->async_run();
  }

  if (cleanup_loop_->is_running() && !cleanup_loop_->is_in_same_thread()) {
    return cleanup_loop_->post_task(Function<void()>(fn));
  }

  return false;
}

bool ComponentHost::start_component(const ComponentSpec& spec, uint64_t generation, std::string* error_out,
                                    bool initial_active, uint32_t timeout_ms) {
  ReadyCallback ready_cb;
  HealthCallback health_cb;
  WatchdogCallback watchdog_cb;
  std::shared_ptr<HostLifeState> life_state;
  std::shared_ptr<LoadedInstance> inst;

  {
    std::unique_lock lock(mtx_);

    if (!life_state_) {
      life_state_ = MemoryResource::make_shared<HostLifeState>();
    } else {
      bool was_alive = false;

      {
        std::shared_lock life_lock(life_state_->mtx);
        was_alive = life_state_->alive;
      }

      if (!was_alive) {
        life_state_ = MemoryResource::make_shared<HostLifeState>();
      }
    }

    life_state = life_state_;
    const uint32_t wait_budget = (timeout_ms == 0U) ? 5000U : timeout_ms;
    const bool cleared = transition_cv_.wait_for(lock, std::chrono::milliseconds(wait_budget),
                                                 [&] { return transitioning_.count(spec.name) == 0U; });

    if (!cleared) {
      if (error_out != nullptr) {
        *error_out = "Timed out waiting for previous transition of component '" + spec.name + "' to finish.";
      }

      return false;
    }

    if (instances_.count(spec.name) != 0U) {
      if (error_out != nullptr) {
        *error_out = "Component '" + spec.name + "' is already loaded.";
      }

      return false;
    }

    ready_cb = on_ready_;
    health_cb = on_health_;
    watchdog_cb = on_watchdog_;
    transitioning_.insert(spec.name);
  }

  auto finish_transition = [&](bool publish) {
    std::lock_guard lock(mtx_);

    if (publish && inst) {
      instances_.emplace(spec.name, inst);
    }

    transitioning_.erase(spec.name);
    transition_cv_.notify_all();
  };

  inst = std::make_shared<LoadedInstance>();
  inst->spec = spec;
  inst->generation = generation;

  if (spec.legacy_runnable) {
    const auto search_paths = build_search_paths();

    {
      std::lock_guard mod_lock(module_mtx_);
      inst->legacy_plugin = plugin_loader_.load<RunablePluginInterface>(spec.library, 1, 0, "", search_paths);
    }

    if VUNLIKELY (!inst->legacy_plugin) {
      if (error_out != nullptr) {
        *error_out = "Failed to load legacy RunablePluginInterface library '" + spec.library + "'.";
      }

      finish_transition(false);
      return false;
    }

    try {
      ScopedComponentLogTag tag(spec.name);
      inst->legacy_plugin->on_init();
      inst->legacy_plugin->async_run();
      inst->started.store(true, std::memory_order_release);
      inst->active->store(true, std::memory_order_release);
    } catch (const std::exception& e) {
      if (error_out != nullptr) {
        *error_out = "Legacy plugin '" + spec.name + "' threw during start: " + e.what();
      }

      inst->legacy_plugin.reset();

      {
        std::lock_guard mod_lock(module_mtx_);
        (void)plugin_loader_.unload<RunablePluginInterface>(spec.library);
      }

      finish_transition(false);
      return false;
    } catch (...) {
      if (error_out != nullptr) {
        *error_out = "Legacy plugin '" + spec.name + "' threw unknown exception during start.";
      }

      inst->legacy_plugin.reset();

      {
        std::lock_guard mod_lock(module_mtx_);
        (void)plugin_loader_.unload<RunablePluginInterface>(spec.library);
      }

      finish_transition(false);
      return false;
    }

    finish_transition(true);
    return true;
  }

  inst->module = load_or_get_module(spec.library);

  if VUNLIKELY (!inst->module) {
    if (error_out != nullptr) {
      *error_out = "Failed to load LauncherModuleInterface library '" + spec.library + "'.";
    }

    finish_transition(false);
    return false;
  }

  try {
    inst->component = inst->module->create_component(spec.class_name);
  } catch (const std::exception& e) {
    if (error_out != nullptr) {
      *error_out = "Component class '" + spec.class_name + "' constructor threw: " + e.what();
    }

    finish_transition(false);
    return false;
  } catch (...) {
    if (error_out != nullptr) {
      *error_out = "Component class '" + spec.class_name + "' constructor threw unknown exception.";
    }

    finish_transition(false);
    return false;
  }

  if VUNLIKELY (!inst->component) {
    if (error_out != nullptr) {
      *error_out = "Component class '" + spec.class_name + "' not found in library '" + spec.library + "'.";
    }

    finish_transition(false);
    return false;
  }

  const std::string sched_name = spec.scheduler_group.empty() ? "default" : spec.scheduler_group;
  inst->assigned_loop = scheduler_.acquire_loop(sched_name, spec.name, spec.worker_index);

  if VUNLIKELY (!inst->assigned_loop) {
    inst->module->destroy_component(inst->component);
    inst->component = nullptr;

    if (error_out != nullptr) {
      *error_out = "Failed to acquire healthy scheduler group '" + sched_name + "' for component '" + spec.name + "'.";
    }

    finish_transition(false);
    return false;
  }

  const std::string pool_name = spec.compute_pool.empty() ? sched_name : spec.compute_pool;
  inst->compute_pool = scheduler_.acquire_thread_pool(pool_name);

  auto deferred_dispatch = [this, life_state](Function<void()>&& fn) {
    if (!fn) {
      return;
    }

    bool dispatched = false;

    {
      std::shared_lock host_lock(life_state->mtx);

      if VLIKELY (life_state->alive) {
        dispatched = try_post_deferred_cleanup(fn);
      }
    }

    if (!dispatched && fn) {
      fn();
    }
  };

  inst->ctx = std::make_shared<ComponentContextImpl>(
      spec.name, generation, spec.ns, sched_name, *inst->assigned_loop, inst->compute_pool, inst->cancel_source.token(),
      inst->active, spec.task_priority, spec.params, spec.urls, spec.inputs, spec.outputs, spec.remaps,
      spec.record_path, spec.record_force_publish.value_or(false), std::move(ready_cb), std::move(health_cb),
      std::move(watchdog_cb), std::move(deferred_dispatch));

  struct InitOutcome final {
    bool ok{false};
    std::string error;
  };

  auto outcome = MemoryResource::make_shared<InitOutcome>();
  inst->ctx->set_defer_ready(true);

  const uint32_t init_budget = (timeout_ms == 0U) ? 5000U : timeout_ms;
  auto init_aborted = MemoryResource::make_shared<std::atomic_bool>(false);
  const DispatchStatus disp = dispatch_on_loop_bounded(
      inst->assigned_loop, init_budget, [inst, outcome, init_aborted, comp_name = spec.name, initial_active]() {
        std::lock_guard life_lock(inst->lifecycle_mtx);

        if (init_aborted->load(std::memory_order_acquire)) {
          return;
        }

        ScopedComponentLogTag tag(comp_name);

        try {
          if (!inst->component->on_configure(*inst->ctx)) {
            outcome->error = "Component '" + comp_name + "' returned false from on_configure().";
            return;
          }

          if (init_aborted->load(std::memory_order_acquire)) {
            return;
          }

          if (initial_active) {
            if (!inst->component->on_start(*inst->ctx)) {
              outcome->error = "Component '" + comp_name + "' returned false from on_start().";
              return;
            }

            inst->started.store(true, std::memory_order_release);

            if (init_aborted->load(std::memory_order_acquire)) {
              return;
            }

            inst->active->store(true, std::memory_order_release);

            if (!inst->component->on_activate(*inst->ctx)) {
              inst->active->store(false, std::memory_order_release);
              outcome->error = "Component '" + comp_name + "' returned false from on_activate().";
              return;
            }
          }

          outcome->ok = true;
        } catch (const std::exception& e) {
          inst->active->store(false, std::memory_order_release);
          outcome->error = "Component '" + comp_name + "' threw during startup: " + e.what();
        } catch (...) {
          inst->active->store(false, std::memory_order_release);
          outcome->error = "Component '" + comp_name + "' threw unknown exception during startup.";
        }
      });

  if (disp == DispatchStatus::kTimedOut) {
    init_aborted->store(true, std::memory_order_release);
    inst->ctx->set_defer_ready(false);
    inst->ctx->stop_accepting();
    inst->active->store(false, std::memory_order_release);
    inst->cancel_source.request_cancel();

    auto wedged_loop = scheduler_.detach_dedicated_loop(spec.name, inst->assigned_loop);

    if (!wedged_loop) {
      scheduler_.mark_loop_poisoned(inst->assigned_loop);
    }

    park_leaked_object(inst);
    park_leaked_object(std::shared_ptr<MessageLoop>(wedged_loop.release()));

    if (error_out != nullptr) {
      *error_out = "Component '" + spec.name + "' startup timed out after " + std::to_string(init_budget) +
                   "ms; executor quarantined.";
    }

    finish_transition(false);
    return false;
  }

  const bool ok = (disp == DispatchStatus::kCompleted && outcome->ok);

  if (!ok) {
    inst->ctx->set_defer_ready(false);

    if (error_out != nullptr) {
      if (disp == DispatchStatus::kRejected) {
        *error_out = "Scheduler loop rejected startup task for '" + spec.name + "'.";
      } else {
        *error_out = outcome->error;
      }
    }

    inst->ctx->stop_accepting();
    const bool was_active = inst->active->exchange(false, std::memory_order_acq_rel);
    inst->cancel_source.request_cancel();

    if (was_active || inst->started.load(std::memory_order_acquire)) {
      const DispatchStatus stop_disp =
          dispatch_on_loop_bounded(inst->assigned_loop, 2000U, [inst, comp_name = spec.name, was_active]() {
            std::lock_guard life_lock(inst->lifecycle_mtx);

            if (inst->component == nullptr) {
              return;
            }

            if (was_active) {
              try {
                inst->component->on_deactivate();
              } catch (const std::exception& e) {
                VLOG_W("ComponentHost: Failed-start deactivate of '", comp_name, "' threw: ", e.what(), ".");
              } catch (...) {
                VLOG_W("ComponentHost: Failed-start deactivate of '", comp_name, "' threw unknown exception.");
              }
            }

            if (inst->started.exchange(false, std::memory_order_acq_rel)) {
              try {
                inst->component->on_stop();
              } catch (const std::exception& e) {
                VLOG_W("ComponentHost: Failed-start stop of '", comp_name, "' threw: ", e.what(), ".");
              } catch (...) {
                VLOG_W("ComponentHost: Failed-start stop of '", comp_name, "' threw unknown exception.");
              }
            }
          });

      if (stop_disp == DispatchStatus::kTimedOut) {
        auto wedged_loop = scheduler_.detach_dedicated_loop(spec.name, inst->assigned_loop);

        if (!wedged_loop) {
          scheduler_.mark_loop_poisoned(inst->assigned_loop);
        }

        park_leaked_object(inst);
        park_leaked_object(std::shared_ptr<MessageLoop>(wedged_loop.release()));
        finish_transition(false);
        return false;
      }
    }

    const bool on_same = inst->assigned_loop->is_in_same_thread();
    const bool drained = on_same ? (inst->ctx->in_flight_count() == 0U) : inst->ctx->wait_drained(2000);

    auto finalize_failed = [this, life_state, inst, comp_name = spec.name]() {
      auto destroy_fn = [inst, comp_name]() {
        std::lock_guard life_lock(inst->lifecycle_mtx);

        if (inst->component != nullptr) {
          try {
            inst->component->on_cleanup();
          } catch (const std::exception& e) {
            VLOG_W("ComponentHost: Failed-start cleanup of '", comp_name, "' threw: ", e.what(), ".");
          } catch (...) {
            VLOG_W("ComponentHost: Failed-start cleanup of '", comp_name, "' threw unknown exception.");
          }

          if (inst->module) {
            inst->module->destroy_component(inst->component);
            inst->component = nullptr;
          }
        }

        if (inst->ctx) {
          inst->ctx->clear_record_hooks();
          inst->ctx->run_cleanup_hooks();
        }
      };

      std::shared_lock host_lock(life_state->mtx);

      if VUNLIKELY (!life_state->alive) {
        destroy_fn();
        return;
      }

      if (dispatch_on_loop_bounded(inst->assigned_loop, 2000U, std::move(destroy_fn)) == DispatchStatus::kTimedOut) {
        auto wedged_loop = scheduler_.detach_dedicated_loop(comp_name, inst->assigned_loop);

        if (!wedged_loop) {
          scheduler_.mark_loop_poisoned(inst->assigned_loop);
        }

        park_leaked_object(inst);
        park_leaked_object(std::shared_ptr<MessageLoop>(wedged_loop.release()));
        return;
      }

      schedule_dedicated_release(comp_name, inst->assigned_loop, 2000U);
    };

    if VUNLIKELY (!drained) {
      inst->ctx->set_deferred_cleanup(
          [this, life_state, finalize = std::move(finalize_failed), comp_name = spec.name]() mutable {
            finalize();

            std::shared_lock host_lock(life_state->mtx);

            if VLIKELY (life_state->alive) {
              std::lock_guard lock(mtx_);
              transitioning_.erase(comp_name);
              transition_cv_.notify_all();
            }
          });
      return false;
    }

    finalize_failed();
    finish_transition(false);
    return false;
  }

  if (!initial_active) {
    inst->ctx->set_defer_ready(false);
    inst->ctx->reset_ready_signal();
  }

  finish_transition(true);

  if (initial_active) {
    inst->ctx->flush_deferred_ready();

    if (spec.ready.type == ReadyProbeType::kNone) {
      inst->ctx->mark_ready();
    }
  }

  return true;
}

ComponentStopResult ComponentHost::stop_component(const std::string& name, uint32_t timeout_ms,
                                                  uint64_t expected_generation) {
  const uint32_t total_budget = (timeout_ms == 0U) ? 5000U : timeout_ms;
  DeadlineTimer deadline(static_cast<int64_t>(total_budget), ElapsedTimer::kMilli);
  std::shared_ptr<LoadedInstance> inst;
  std::shared_ptr<HostLifeState> life_state;

  {
    std::unique_lock lock(mtx_);
    const int64_t rem = std::max<int64_t>(1, deadline.remaining_time());
    const bool clear =
        transition_cv_.wait_for(lock, std::chrono::milliseconds(rem), [&] { return transitioning_.count(name) == 0U; });

    if (!clear) {
      return ComponentStopResult::kDeferredDrain;
    }

    auto it = instances_.find(name);

    if (it == instances_.end()) {
      return ComponentStopResult::kNotFound;
    }

    if (expected_generation != 0U && it->second && it->second->generation != expected_generation) {
      return ComponentStopResult::kNotFound;
    }

    inst = std::move(it->second);
    instances_.erase(it);
    transitioning_.insert(name);
    life_state = life_state_;
  }

  struct TransitionGuard final {
    std::mutex& mtx;
    ConditionVariable& cv;
    std::unordered_set<std::string>& set;
    const std::string& key;
    bool armed{true};

    ~TransitionGuard() {
      if (!armed) {
        return;
      }

      std::lock_guard lock(mtx);
      set.erase(key);
      cv.notify_all();
    }
  };

  TransitionGuard trans_guard{mtx_, transition_cv_, transitioning_, name, true};

  if (!inst) {
    return ComponentStopResult::kNotFound;
  }

  if (inst->ctx) {
    inst->ctx->stop_accepting();
  }

  const bool was_active = inst->active->exchange(false, std::memory_order_acq_rel);
  inst->cancel_source.request_cancel();

  if (inst->legacy_plugin) {
    inst->started.store(false, std::memory_order_release);

    try {
      ScopedComponentLogTag tag(name);
      inst->legacy_plugin->on_deinit();
    } catch (const std::exception& e) {
      VLOG_E("ComponentHost: Legacy plugin '", name, "' on_deinit threw: ", e.what(), ".");
    } catch (...) {
      VLOG_E("ComponentHost: Legacy plugin '", name, "' on_deinit threw unknown exception.");
    }

    if (inst->legacy_plugin->is_running()) {
      inst->legacy_plugin->quit(false);
      const int rem_ms = static_cast<int>(std::max<int64_t>(100, deadline.remaining_time()));

      if (!inst->legacy_plugin->wait_for_quit(rem_ms)) {
        VLOG_E("ComponentHost: Legacy plugin '", name, "' did not exit within ", rem_ms, "ms; quarantining.");
        park_leaked_object(inst);
        return ComponentStopResult::kTimedOutQuarantined;
      }
    }

    const std::string lib_name = inst->spec.library;
    inst->legacy_plugin.reset();

    {
      std::lock_guard mod_lock(module_mtx_);
      (void)plugin_loader_.unload<RunablePluginInterface>(lib_name);
    }

    return ComponentStopResult::kStoppedClean;
  }

  if (inst->component && inst->assigned_loop) {
    const uint32_t stop_budget = static_cast<uint32_t>(std::max<int64_t>(50, deadline.remaining_time()));

    const DispatchStatus disp =
        dispatch_on_loop_bounded(inst->assigned_loop, stop_budget, [inst, comp_name = name, was_active]() {
          std::lock_guard life_lock(inst->lifecycle_mtx);
          ScopedComponentLogTag tag(comp_name);

          if (inst->component == nullptr) {
            return;
          }

          if (was_active || inst->active->exchange(false, std::memory_order_acq_rel)) {
            try {
              inst->component->on_deactivate();
            } catch (const std::exception& e) {
              VLOG_E("ComponentHost: Component '", comp_name, "' on_deactivate threw: ", e.what(), ".");
            } catch (...) {
              VLOG_E("ComponentHost: Component '", comp_name, "' on_deactivate threw unknown exception.");
            }
          }

          if (inst->started.exchange(false, std::memory_order_acq_rel)) {
            try {
              inst->component->on_stop();
            } catch (const std::exception& e) {
              VLOG_E("ComponentHost: Component '", comp_name, "' on_stop threw: ", e.what(), ".");
            } catch (...) {
              VLOG_E("ComponentHost: Component '", comp_name, "' on_stop threw unknown exception.");
            }
          }
        });

    if (disp == DispatchStatus::kTimedOut) {
      auto wedged_loop = scheduler_.detach_dedicated_loop(name, inst->assigned_loop);

      if (!wedged_loop) {
        scheduler_.mark_loop_poisoned(inst->assigned_loop);
      }

      VLOG_E("ComponentHost: Component '", name, "' executor hung during stop (", stop_budget,
             "ms budget exceeded); quarantining instance (use [[container]] for hard-killable isolation).");
      park_leaked_object(inst);
      park_leaked_object(std::shared_ptr<MessageLoop>(wedged_loop.release()));
      return ComponentStopResult::kTimedOutQuarantined;
    }

    auto finalize_cleanup = [this, life_state, inst, comp_name = name, total_budget]() {
      auto run_on_cleanup = [inst, comp_name]() {
        std::lock_guard life_lock(inst->lifecycle_mtx);
        ScopedComponentLogTag tag(comp_name);

        if (inst->component == nullptr) {
          return;
        }

        try {
          inst->component->on_cleanup();
        } catch (const std::exception& e) {
          VLOG_E("ComponentHost: Component '", comp_name, "' on_cleanup threw: ", e.what(), ".");
        } catch (...) {
          VLOG_E("ComponentHost: Component '", comp_name, "' on_cleanup threw unknown exception.");
        }

        if (inst->component != nullptr && inst->module) {
          inst->module->destroy_component(inst->component);
          inst->component = nullptr;
        }

        if (inst->ctx) {
          inst->ctx->clear_record_hooks();
          inst->ctx->run_cleanup_hooks();
        }
      };

      std::shared_lock host_lock(life_state->mtx);

      if VUNLIKELY (!life_state->alive) {
        run_on_cleanup();
        return;
      }

      const DispatchStatus clean_disp = dispatch_on_loop_bounded(
          inst->assigned_loop, std::min<uint32_t>(total_budget, 2000U), std::move(run_on_cleanup));

      if (clean_disp == DispatchStatus::kTimedOut) {
        auto wedged_loop = scheduler_.detach_dedicated_loop(comp_name, inst->assigned_loop);

        if (!wedged_loop) {
          scheduler_.mark_loop_poisoned(inst->assigned_loop);
        }

        park_leaked_object(inst);
        park_leaked_object(std::shared_ptr<MessageLoop>(wedged_loop.release()));
        return;
      }

      schedule_dedicated_release(comp_name, inst->assigned_loop, std::min<uint32_t>(total_budget, 2000U));
    };

    const bool on_same_thread = inst->assigned_loop->is_in_same_thread();
    bool drained = true;

    if (inst->ctx) {
      if (on_same_thread) {
        drained = (inst->ctx->in_flight_count() == 0U);
      } else {
        const int drain_ms = static_cast<int>(std::max<int64_t>(50, deadline.remaining_time()));
        drained = inst->ctx->wait_drained(drain_ms);
      }
    }

    if VUNLIKELY (!drained && inst->ctx) {
      VLOG_W("ComponentHost: Component '", name, "' has active task guards; deferring cleanup.");
      trans_guard.armed = false;
      inst->ctx->set_deferred_cleanup(
          [this, life_state, finalize = std::move(finalize_cleanup), comp_name = name]() mutable {
            finalize();

            std::shared_lock host_lock(life_state->mtx);

            if VLIKELY (life_state->alive) {
              std::lock_guard lock(mtx_);
              transitioning_.erase(comp_name);
              transition_cv_.notify_all();
            }
          });
      return ComponentStopResult::kDeferredDrain;
    }

    finalize_cleanup();
  }

  return ComponentStopResult::kStoppedClean;
}

void ComponentHost::stop_all(uint32_t timeout_ms) {
  const uint32_t total_budget = (timeout_ms == 0U) ? 5000U : timeout_ms;
  DeadlineTimer deadline(static_cast<int64_t>(total_budget), ElapsedTimer::kMilli);

  while (true) {
    std::vector<std::string> names;

    {
      std::unique_lock lock(mtx_);

      if (instances_.empty()) {
        if (transitioning_.empty() || deadline.has_expired()) {
          transitioning_.clear();
          break;
        }

        const int64_t rem = std::max<int64_t>(10, deadline.remaining_time());
        (void)transition_cv_.wait_for(lock, std::chrono::milliseconds(rem),
                                      [&] { return !instances_.empty() || transitioning_.empty(); });

        if (instances_.empty()) {
          if (transitioning_.empty() || deadline.has_expired()) {
            transitioning_.clear();
            break;
          }

          continue;
        }
      }

      names.reserve(instances_.size());

      for (const auto& entry : instances_) {
        names.push_back(entry.first);
      }
    }

    for (const auto& name : names) {
      const uint32_t rem = static_cast<uint32_t>(std::max<int64_t>(100, deadline.remaining_time()));
      (void)stop_component(name, rem);
    }
  }

  transition_cv_.notify_all();

  std::shared_ptr<HostLifeState> life_state;

  {
    std::lock_guard lock(mtx_);
    life_state = life_state_;
  }

  if (life_state) {
    std::unique_lock host_lock(life_state->mtx);
    life_state->alive = false;
  }

  std::unique_ptr<MessageLoop> old_cleanup_loop;

  {
    std::lock_guard lock(cleanup_loop_mtx_);
    old_cleanup_loop.swap(cleanup_loop_);
  }

  if (old_cleanup_loop && old_cleanup_loop->is_running()) {
    old_cleanup_loop->quit(false);
    const int rem_ms = static_cast<int>(std::max<int64_t>(100, deadline.remaining_time()));

    if (!old_cleanup_loop->wait_for_quit(rem_ms)) {
      park_leaked_object(std::shared_ptr<MessageLoop>(old_cleanup_loop.release()));
    }
  }

  {
    std::lock_guard mod_lock(module_mtx_);
    module_cache_.clear();
    plugin_loader_.clear();
  }
}

std::shared_ptr<ComponentHost::LoadedInstance> ComponentHost::find_instance(const std::string& name) const {
  std::lock_guard lock(mtx_);
  const auto it = instances_.find(name);
  return (it == instances_.end()) ? nullptr : it->second;
}

template <typename OpFnT>
bool ComponentHost::dispatch_component_op(const std::string& name, uint32_t timeout_ms, const char* op_label,
                                          const char* not_found_msg, std::string* error_out, OpFnT&& op_fn) {
  auto inst = find_instance(name);

  if (!inst || !inst->component || !inst->ctx) {
    if (error_out != nullptr) {
      *error_out = "Component '" + name + "' " + not_found_msg;
    }

    return false;
  }

  auto guard = inst->ctx->acquire_task_guard();

  if (!guard) {
    if (error_out != nullptr) {
      *error_out = "Component '" + name + "' is shutting down.";
    }

    return false;
  }

  struct ResultBox final {
    bool ok{false};
    std::string error;
  };

  auto res = MemoryResource::make_shared<ResultBox>();
  auto aborted = MemoryResource::make_shared<std::atomic_bool>(false);
  const uint32_t budget = (timeout_ms == 0U) ? 5000U : timeout_ms;

  const DispatchStatus disp = dispatch_on_loop_bounded(
      inst->assigned_loop, budget,
      [inst, res, aborted, comp_name = name, guard, fn = std::forward<OpFnT>(op_fn)]() mutable {
        (void)guard;
        std::lock_guard life_lock(inst->lifecycle_mtx);

        if (aborted->load(std::memory_order_acquire) || !inst->component) {
          return;
        }

        ScopedComponentLogTag tag(comp_name);
        res->ok = fn(*inst, *aborted, res->error);
      });

  if (disp != DispatchStatus::kCompleted || !res->ok) {
    if (disp == DispatchStatus::kTimedOut) {
      aborted->store(true, std::memory_order_release);
    }

    if (error_out != nullptr) {
      if (disp == DispatchStatus::kTimedOut) {
        *error_out = "Component '" + name + "' " + op_label + " timed out.";
      } else if (disp == DispatchStatus::kRejected) {
        *error_out = "Failed to dispatch " + std::string(op_label) + " for '" + name + "'.";
      } else if (!res->error.empty()) {
        *error_out = res->error;
      } else {
        *error_out = "Component '" + name + "' " + op_label + " failed.";
      }
    }

    return false;
  }

  return true;
}

bool ComponentHost::activate_component(const std::string& name, std::string* error_out, uint32_t timeout_ms) {
  auto already_active = MemoryResource::make_shared<std::atomic_bool>(false);

  const bool ok = dispatch_component_op(
      name, timeout_ms, "activation", "is not loaded or does not support lifecycle activation.", error_out,
      [already_active, comp_name = name](LoadedInstance& inst, const std::atomic_bool& aborted,
                                         std::string& err) -> bool {
        if (inst.active->load(std::memory_order_acquire)) {
          already_active->store(true, std::memory_order_release);
          return true;
        }

        inst.ctx->reset_ready_signal();
        inst.ctx->set_defer_ready(true);

        try {
          if (!inst.started.load(std::memory_order_acquire)) {
            if (!inst.component->on_start(*inst.ctx)) {
              inst.ctx->set_defer_ready(false);
              err = "Component '" + comp_name + "' returned false from on_start().";
              return false;
            }

            inst.started.store(true, std::memory_order_release);
          }

          if (aborted.load(std::memory_order_acquire)) {
            inst.ctx->set_defer_ready(false);
            return false;
          }

          inst.active->store(true, std::memory_order_release);

          if (!inst.component->on_activate(*inst.ctx)) {
            inst.active->store(false, std::memory_order_release);
            inst.ctx->set_defer_ready(false);
            err = "Component '" + comp_name + "' returned false from on_activate().";
            return false;
          }

          if (aborted.load(std::memory_order_acquire)) {
            inst.active->store(false, std::memory_order_release);
            inst.ctx->set_defer_ready(false);

            try {
              inst.component->on_deactivate();
            } catch (const std::exception& e) {
              VLOG_W("ComponentHost: Component '", comp_name, "' rollback on_deactivate() threw: ", e.what());
            } catch (...) {
              VLOG_W("ComponentHost: Component '", comp_name, "' rollback on_deactivate() threw unknown exception.");
            }

            return false;
          }

          return true;
        } catch (const std::exception& e) {
          inst.active->store(false, std::memory_order_release);
          inst.ctx->set_defer_ready(false);
          err = "Component '" + comp_name + "' on_activate threw: " + e.what();
          return false;
        } catch (...) {
          inst.active->store(false, std::memory_order_release);
          inst.ctx->set_defer_ready(false);
          err = "Component '" + comp_name + "' on_activate threw unknown exception.";
          return false;
        }
      });

  if (!ok) {
    return false;
  }

  auto activated_inst = find_instance(name);

  if (activated_inst && activated_inst->ctx && !already_active->load(std::memory_order_acquire)) {
    activated_inst->ctx->flush_deferred_ready();

    if (activated_inst->spec.ready.type == ReadyProbeType::kNone) {
      activated_inst->ctx->mark_ready();
    }
  }

  return true;
}

bool ComponentHost::deactivate_component(const std::string& name, std::string* error_out, uint32_t timeout_ms) {
  return dispatch_component_op(
      name, timeout_ms, "deactivation", "is not loaded or does not support lifecycle deactivation.", error_out,
      [comp_name = name](LoadedInstance& inst, const std::atomic_bool& aborted, std::string& err) -> bool {
        if (!inst.active->load(std::memory_order_acquire)) {
          return true;
        }

        if (aborted.load(std::memory_order_acquire)) {
          return false;
        }

        try {
          inst.active->store(false, std::memory_order_release);
          inst.component->on_deactivate();

          if (aborted.load(std::memory_order_acquire)) {
            if (!inst.cancel_source.is_cancellation_requested()) {
              inst.active->store(true, std::memory_order_release);

              try {
                if (!inst.component->on_activate(*inst.ctx)) {
                  inst.active->store(false, std::memory_order_release);
                }
              } catch (const std::exception& e) {
                inst.active->store(false, std::memory_order_release);
                VLOG_W("ComponentHost: Component '", comp_name, "' rollback on_activate() threw: ", e.what());
              } catch (...) {
                inst.active->store(false, std::memory_order_release);
                VLOG_W("ComponentHost: Component '", comp_name, "' rollback on_activate() threw unknown exception.");
              }
            }

            return false;
          }

          return true;
        } catch (const std::exception& e) {
          inst.active->store(true, std::memory_order_release);
          err = "Component '" + comp_name + "' on_deactivate threw: " + e.what();
          return false;
        } catch (...) {
          inst.active->store(true, std::memory_order_release);
          err = "Component '" + comp_name + "' on_deactivate threw unknown exception.";
          return false;
        }
      });
}

bool ComponentHost::reload_component(const std::string& name, std::string* error_out, uint32_t timeout_ms) {
  return dispatch_component_op(name, timeout_ms, "reload", "is not active or does not support reload.", error_out,
                               [](LoadedInstance& inst, const std::atomic_bool&, std::string& err) -> bool {
                                 if (!inst.started.load(std::memory_order_acquire)) {
                                   return false;
                                 }

                                 try {
                                   if (!inst.component->on_reload(*inst.ctx)) {
                                     err = "Component rejected reload.";
                                     return false;
                                   }

                                   return true;
                                 } catch (const std::exception& e) {
                                   err = e.what();
                                   return false;
                                 } catch (...) {
                                   err = "Unknown exception in on_reload().";
                                   return false;
                                 }
                               });
}

bool ComponentHost::has_component(const std::string& name) const {
  auto inst = find_instance(name);
  return inst && (inst->component != nullptr || inst->legacy_plugin != nullptr);
}

double ComponentHost::get_cpu_usage(const std::string& name) const {
  auto inst = find_instance(name);
  return (inst && inst->ctx) ? inst->ctx->get_cpu_usage() : -1.0;
}

uint64_t ComponentHost::get_last_watchdog_ms(const std::string& name) const {
  auto inst = find_instance(name);
  return (inst && inst->ctx) ? inst->ctx->last_watchdog_ms() : 0U;
}

bool ComponentHost::get_params(const std::string& name, LauncherParamMap& out) const {
  auto inst = find_instance(name);

  if (!inst || !inst->ctx) {
    return false;
  }

  out = inst->ctx->params();
  return true;
}

bool ComponentHost::set_param(const std::string& name, std::string_view key, const LauncherParamValue& value,
                              std::string* error_out, uint32_t timeout_ms) {
  return dispatch_component_op(
      name, timeout_ms, "parameter update", "is not active or does not support dynamic params.", error_out,
      [comp_name = name, key_copy = std::string(key), val_copy = value](LoadedInstance& inst, const std::atomic_bool&,
                                                                        std::string& err) -> bool {
        try {
          if (!inst.component->on_param_changed(key_copy, val_copy)) {
            err = "Component '" + comp_name + "' rejected parameter update for '" + key_copy + "'.";
            return false;
          }

          inst.ctx->update_param(key_copy, val_copy);
          return true;
        } catch (const std::exception& e) {
          err = e.what();
          return false;
        } catch (...) {
          err = "Unknown exception in on_param_changed().";
          return false;
        }
      });
}

bool ComponentHost::set_component_record(const std::string& name, const std::string& path, bool force_publish,
                                         std::string* error_out, uint32_t timeout_ms) {
  return dispatch_component_op(
      name, timeout_ms, "recording update", "is not active or does not support dynamic recording.", error_out,
      [path, force_publish](LoadedInstance& inst, const std::atomic_bool&, std::string& err) -> bool {
        try {
          inst.ctx->apply_record_config(path, force_publish);
          return true;
        } catch (const std::exception& e) {
          err = e.what();
          return false;
        } catch (...) {
          err = "Unknown exception while updating recording configuration.";
          return false;
        }
      });
}

std::deque<std::string> ComponentHost::build_search_paths() const {
  std::vector<std::string> dirs_copy;

  {
    std::lock_guard lock(mtx_);
    dirs_copy = plugin_dirs_;
  }

  auto paths = Plugin::default_search_path();

  for (size_t i = dirs_copy.size(); i > 0U; --i) {
    paths.push_front(dirs_copy[i - 1U]);
  }

  return paths;
}

std::shared_ptr<LauncherModuleInterface> ComponentHost::load_or_get_module(const std::string& lib_name) {
  {
    std::lock_guard lock(module_mtx_);
    auto it = module_cache_.find(lib_name);

    if (it != module_cache_.end()) {
      return it->second;
    }
  }

  const auto search_paths = build_search_paths();
  std::lock_guard lock(module_mtx_);
  auto it = module_cache_.find(lib_name);

  if (it != module_cache_.end()) {
    return it->second;
  }

  auto mod = plugin_loader_.load<LauncherModuleInterface>(lib_name, LauncherModuleInterface::kVersionMajor,
                                                          LauncherModuleInterface::kVersionMinor, "", search_paths);

  if (!mod) {
    return nullptr;
  }

  module_cache_.emplace(lib_name, mod);
  return mod;
}

void ComponentHost::schedule_dedicated_release(const std::string& name, MessageLoop* assigned_loop,
                                               uint32_t timeout_ms) {
  auto loop = scheduler_.detach_dedicated_loop(name, assigned_loop);

  if (!loop) {
    return;
  }

  auto stop_and_park = [timeout_ms](std::shared_ptr<MessageLoop> owned) {
    if (owned && owned->is_running()) {
      owned->quit(false);

      if (!owned->wait_for_quit(static_cast<int>(timeout_ms == 0U ? 2000U : timeout_ms))) {
        park_leaked_object(std::move(owned));
      }
    }
  };

  std::shared_ptr<MessageLoop> owned(loop.release());

  if (assigned_loop != nullptr && assigned_loop->is_in_same_thread()) {
    if (!try_post_deferred_cleanup([stop_and_park, owned]() mutable { stop_and_park(std::move(owned)); })) {
      stop_and_park(std::move(owned));
    }

    return;
  }

  stop_and_park(std::move(owned));
}

}  // namespace launcher

}  // namespace vlink
