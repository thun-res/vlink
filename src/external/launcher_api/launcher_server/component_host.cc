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

#include <vlink/base/elapsed_timer.h>
#include <vlink/base/helpers.h>
#include <vlink/base/logger.h>
#include <vlink/base/memory_resource.h>
#include <vlink/base/timer.h>
//
#include <atomic>
#include <chrono>
#include <deque>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "./log_manager.h"

namespace vlink {

namespace launcher {

class ComponentHost::ComponentContextImpl final : public LauncherContext,
                                                  public std::enable_shared_from_this<ComponentContextImpl> {
 public:
  ComponentContextImpl(std::string name, std::string ns, std::string group, MessageLoop& loop, ThreadPool* pool,
                       CancellationToken cancel_token, std::shared_ptr<const std::atomic_bool> active_flag,
                       uint16_t default_priority, LauncherParamMap params,
                       std::unordered_map<std::string, std::string> urls,
                       std::unordered_map<std::string, std::string> inputs,
                       std::unordered_map<std::string, std::string> outputs,
                       std::vector<std::pair<std::string, std::string>> remaps, std::string record_path,
                       bool record_force_publish, ReadyCallback on_ready, HealthCallback on_health,
                       WatchdogCallback on_watchdog)
      : name_(std::move(name)),
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
        on_watchdog_(std::move(on_watchdog)) {
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
    const std::string key(url);

    {
      std::lock_guard lock(remap_cache_mtx_);
      const auto it = remap_cache_.find(key);

      if VLIKELY (it != remap_cache_.end()) {
        return it->second;
      }
    }

    std::string resolved = resolve_unit_url(url, ns_, remaps_);

    {
      std::lock_guard lock(remap_cache_mtx_);
      remap_cache_.emplace(key, resolved);
    }

    return resolved;
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

  struct RecordHookEntry final {
    uint64_t id{0};
    std::shared_ptr<Function<bool(const std::string&)>> fn;
  };

  void register_record_hook(Function<bool(const std::string&)>&& hook) const override {
    if (!hook) {
      return;
    }

    auto fn = std::make_shared<Function<bool(const std::string&)>>(std::move(hook));
    uint64_t hook_id = 0U;
    std::string cur_path;
    uint64_t seen_epoch = 0U;

    {
      std::lock_guard lock(record_mtx_);
      hook_id = ++next_record_hook_id_;
      cur_path = record_path_;
      seen_epoch = record_epoch_;
      record_hooks_.push_back(RecordHookEntry{hook_id, fn});
    }

    while (true) {
      if (!cur_path.empty() || seen_epoch > 0U) {
        bool alive = false;

        try {
          alive = (*fn)(cur_path);
        } catch (...) {
          std::lock_guard lock(record_mtx_);
          record_hooks_.erase(std::remove_if(record_hooks_.begin(), record_hooks_.end(),
                                             [hook_id](const RecordHookEntry& e) { return e.id == hook_id; }),
                              record_hooks_.end());
          throw;
        }

        if (!alive) {
          std::lock_guard lock(record_mtx_);
          record_hooks_.erase(std::remove_if(record_hooks_.begin(), record_hooks_.end(),
                                             [hook_id](const RecordHookEntry& e) { return e.id == hook_id; }),
                              record_hooks_.end());
          return;
        }
      }

      std::lock_guard lock(record_mtx_);

      if (record_epoch_ == seen_epoch) {
        return;
      }

      seen_epoch = record_epoch_;
      cur_path = record_path_;
    }
  }

  void clear_record_hooks() noexcept {
    std::vector<RecordHookEntry> stale;

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
    std::vector<RecordHookEntry> snapshot;
    std::string prev_path;

    {
      std::lock_guard lock(record_mtx_);
      prev_path = record_path_;
      record_path_ = path;
      ++record_epoch_;
      snapshot = record_hooks_;
    }

    std::unordered_set<uint64_t> dead_ids;
    std::vector<RecordHookEntry> applied;
    applied.reserve(snapshot.size());
    std::exception_ptr first_error;

    for (const auto& entry : snapshot) {
      if (!entry.fn || !(*entry.fn)) {
        dead_ids.insert(entry.id);
        continue;
      }

      try {
        if ((*entry.fn)(path)) {
          applied.push_back(entry);
        } else {
          dead_ids.insert(entry.id);
        }
      } catch (...) {
        first_error = std::current_exception();
        break;
      }
    }

    if (first_error) {
      for (const auto& entry : applied) {
        if (entry.fn && *entry.fn) {
          try {
            if (!(*entry.fn)(prev_path)) {
              dead_ids.insert(entry.id);
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
        ++record_epoch_;

        if (!dead_ids.empty()) {
          record_hooks_.erase(std::remove_if(record_hooks_.begin(), record_hooks_.end(),
                                             [&](const RecordHookEntry& e) { return dead_ids.count(e.id) != 0U; }),
                              record_hooks_.end());
        }
      }

      std::rethrow_exception(first_error);
    }

    if (!dead_ids.empty()) {
      std::lock_guard lock(record_mtx_);
      record_hooks_.erase(std::remove_if(record_hooks_.begin(), record_hooks_.end(),
                                         [&](const RecordHookEntry& e) { return dead_ids.count(e.id) != 0U; }),
                          record_hooks_.end());
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

    if (should_fire && on_ready_) {
      try {
        on_ready_(name_);
      } catch (const std::exception& e) {
        VLOG_W("ComponentHost: Component '", name_, "' mark_ready callback threw: ", e.what(), ".");
      } catch (...) {
        VLOG_W("ComponentHost: Component '", name_, "' mark_ready callback threw unknown exception.");
      }
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

    if (should_fire && on_ready_) {
      try {
        on_ready_(name_);
      } catch (const std::exception& e) {
        VLOG_W("ComponentHost: Component '", name_, "' mark_ready callback threw: ", e.what(), ".");
      } catch (...) {
        VLOG_W("ComponentHost: Component '", name_, "' mark_ready callback threw unknown exception.");
      }
    }
  }

  void report_health(bool healthy, std::string_view detail) noexcept override {
    if (on_health_) {
      try {
        on_health_(name_, healthy, std::string(detail));
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
        on_watchdog_(name_);
      } catch (const std::exception& e) {
        VLOG_W("ComponentHost: Component '", name_, "' watchdog callback threw: ", e.what(), ".");
      } catch (...) {
        VLOG_W("ComponentHost: Component '", name_, "' watchdog callback threw unknown exception.");
      }
    }
  }

  [[nodiscard]] uint64_t last_watchdog_ms() const noexcept { return last_watchdog_ms_.load(std::memory_order_acquire); }

  [[nodiscard]] double get_cpu_usage() const noexcept override {
    std::lock_guard lock(prof_mtx_);
    return profiler_.get();
  }

  [[nodiscard]] uintptr_t enter_callback_scope() const noexcept override {
    ScopedComponentLogTag::push_tag(name_);

    std::lock_guard lock(prof_mtx_);

    if (prof_depth_ == 0U) {
      profiler_.begin();
    }

    ++prof_depth_;
    return 1U;
  }

  void exit_callback_scope(uintptr_t token) const noexcept override {
    if (token != 0U) {
      {
        std::lock_guard lock(prof_mtx_);

        if (prof_depth_ > 0U) {
          --prof_depth_;

          if (prof_depth_ == 0U) {
            profiler_.end();
          }
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
      immediate();
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
    auto self = shared_from_this();
    auto wrapped = [self, guard = std::move(guard), fn = std::move(task)]() mutable {
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
        VLOG_E("ComponentHost: Component '", self->name_, "' task threw exception: ", e.what(), ".");
        throw;
      } catch (...) {
        VLOG_E("ComponentHost: Component '", self->name_, "' task threw unknown exception.");
        throw;
      }
    };

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

    auto self = shared_from_this();
    auto wrapped = [self, guard = std::move(guard), fn = std::move(task)]() mutable {
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
        VLOG_E("ComponentHost: Component '", self->name_, "' compute task threw exception: ", e.what(), ".");
        throw;
      } catch (...) {
        VLOG_E("ComponentHost: Component '", self->name_, "' compute task threw unknown exception.");
        throw;
      }
    };

    return pool_->post_task_handle(std::move(wrapped), effective);
  }

 private:
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
  mutable std::mutex remap_cache_mtx_;
  mutable std::unordered_map<std::string, std::string> remap_cache_;
  mutable std::mutex record_mtx_;
  std::string record_path_;
  std::shared_ptr<std::atomic_bool> record_force_publish_;
  uint64_t record_epoch_{0};
  mutable uint64_t next_record_hook_id_{0};
  mutable std::vector<RecordHookEntry> record_hooks_;
  mutable std::vector<Function<void()>> cleanup_hooks_;
  ReadyCallback on_ready_;
  HealthCallback on_health_;
  WatchdogCallback on_watchdog_;
  std::mutex ready_mtx_;
  bool ready_signalled_{false};
  bool defer_ready_{false};
  bool deferred_ready_pending_{false};
  std::atomic<uint64_t> last_watchdog_ms_{0};
  mutable std::mutex prof_mtx_;
  mutable uint32_t prof_depth_{0};
  mutable CpuProfiler profiler_;
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

bool ComponentHost::start_component(const ComponentSpec& spec, std::string* error_out, bool initial_active) {
  ReadyCallback ready_cb;
  HealthCallback health_cb;
  WatchdogCallback watchdog_cb;
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

    transition_cv_.wait(lock, [&] { return transitioning_.count(spec.name) == 0U; });

    if (instances_.count(spec.name) != 0U) {
      if (error_out != nullptr) {
        *error_out = "Component '" + spec.name + "' is already loaded.";
      }

      return false;
    }

    ready_cb = on_ready_;
    health_cb = on_health_;
    watchdog_cb = on_watchdog_;
    inst = MemoryResource::make_shared<LoadedInstance>();
    inst->spec = spec;

    if (spec.legacy_runnable) {
      const auto search_paths = build_search_paths();
      inst->legacy_plugin = plugin_loader_.load<RunablePluginInterface>(spec.library, 1, 0, "", search_paths);

      if VUNLIKELY (!inst->legacy_plugin) {
        if (error_out != nullptr) {
          *error_out = "Failed to load legacy RunablePluginInterface library '" + spec.library + "'.";
        }

        return false;
      }

      transitioning_.insert(spec.name);
    } else {
      inst->module = load_or_get_module(spec.library);

      if VUNLIKELY (!inst->module) {
        if (error_out != nullptr) {
          *error_out = "Failed to load LauncherModuleInterface library '" + spec.library + "'.";
        }

        return false;
      }

      try {
        inst->component = inst->module->create_component(spec.class_name);
      } catch (const std::exception& e) {
        if (error_out != nullptr) {
          *error_out = "Component class '" + spec.class_name + "' constructor threw: " + e.what();
        }

        return false;
      } catch (...) {
        if (error_out != nullptr) {
          *error_out = "Component class '" + spec.class_name + "' constructor threw unknown exception.";
        }

        return false;
      }

      if VUNLIKELY (!inst->component) {
        if (error_out != nullptr) {
          *error_out = "Component class '" + spec.class_name + "' not found in library '" + spec.library + "'.";
        }

        return false;
      }

      const std::string sched_name = spec.scheduler_group.empty() ? "default" : spec.scheduler_group;
      inst->assigned_loop = scheduler_.acquire_loop(sched_name, spec.name, spec.worker_index);

      if VUNLIKELY (!inst->assigned_loop) {
        inst->module->destroy_component(inst->component);
        inst->component = nullptr;

        if (error_out != nullptr) {
          *error_out = "Failed to acquire scheduler group '" + sched_name + "' for component '" + spec.name + "'.";
        }

        return false;
      }

      const std::string pool_name = spec.compute_pool.empty() ? sched_name : spec.compute_pool;
      inst->compute_pool = scheduler_.acquire_thread_pool(pool_name);

      inst->ctx = MemoryResource::make_shared<ComponentContextImpl>(
          spec.name, spec.ns, sched_name, *inst->assigned_loop, inst->compute_pool, inst->cancel_source.token(),
          inst->active, spec.task_priority, spec.params, spec.urls, spec.inputs, spec.outputs, spec.remaps,
          spec.record_path, spec.record_force_publish.value_or(false), std::move(ready_cb), std::move(health_cb),
          std::move(watchdog_cb));

      transitioning_.insert(spec.name);
    }
  }

  auto finish_transition = [&](bool publish) {
    std::lock_guard lock(mtx_);

    if (publish) {
      instances_.emplace(spec.name, inst);
    }

    transitioning_.erase(spec.name);
    transition_cv_.notify_all();
  };

  if (spec.legacy_runnable) {
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
        std::lock_guard lock(mtx_);
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
        std::lock_guard lock(mtx_);
        (void)plugin_loader_.unload<RunablePluginInterface>(spec.library);
      }
      finish_transition(false);
      return false;
    }

    finish_transition(true);
    return true;
  }

  auto teardown_failed_inst = [&](bool& deferred_out) {
    deferred_out = false;

    if (inst->ctx) {
      inst->ctx->stop_accepting();
    }

    inst->cancel_source.request_cancel();

    if (inst->component && inst->assigned_loop) {
      const bool on_same = inst->assigned_loop->is_in_same_thread();

      auto stop_started_fn = [&]() {
        std::lock_guard life_lock(inst->lifecycle_mtx);

        if (inst->component == nullptr) {
          return;
        }

        if (inst->active->exchange(false, std::memory_order_acq_rel)) {
          try {
            inst->component->on_deactivate();
          } catch (const std::exception& e) {
            VLOG_W("ComponentHost: Failed-start deactivate of '", spec.name, "' threw: ", e.what(), ".");
          } catch (...) {
            VLOG_W("ComponentHost: Failed-start deactivate of '", spec.name, "' threw unknown exception.");
          }
        }

        if (inst->started.exchange(false, std::memory_order_acq_rel)) {
          try {
            inst->component->on_stop();
          } catch (const std::exception& e) {
            VLOG_W("ComponentHost: Failed-start stop of '", spec.name, "' threw: ", e.what(), ".");
          } catch (...) {
            VLOG_W("ComponentHost: Failed-start stop of '", spec.name, "' threw unknown exception.");
          }
        }
      };

      if (inst->assigned_loop->is_running() && !on_same) {
        try {
          inst->assigned_loop->invoke_task(stop_started_fn).get();
        } catch (const std::future_error&) {
          stop_started_fn();
        }
      } else {
        stop_started_fn();
      }

      bool drained = true;

      if (inst->ctx) {
        if (on_same) {
          drained = (inst->ctx->in_flight_count() == 0U);
        } else {
          drained = inst->ctx->wait_drained(5000);
        }
      }

      auto life_state = life_state_;
      auto finalize_failed = [this, life_state, inst, name = spec.name]() {
        auto destroy_fn = [inst, name]() {
          std::lock_guard life_lock(inst->lifecycle_mtx);

          if (inst->component != nullptr) {
            try {
              inst->component->on_cleanup();
            } catch (const std::exception& e) {
              VLOG_W("ComponentHost: Failed-start cleanup of '", name, "' threw: ", e.what(), ".");
            } catch (...) {
              VLOG_W("ComponentHost: Failed-start cleanup of '", name, "' threw unknown exception.");
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

        const bool same = inst->assigned_loop->is_in_same_thread();

        if (inst->assigned_loop->is_running() && !same) {
          try {
            inst->assigned_loop->invoke_task(destroy_fn).get();
          } catch (const std::future_error&) {
            destroy_fn();
          }
        } else {
          destroy_fn();
        }

        schedule_dedicated_release(name, inst->assigned_loop);
      };

      if VUNLIKELY (!drained && inst->ctx) {
        deferred_out = true;
        inst->ctx->set_deferred_cleanup(
            [this, life_state, finalize = std::move(finalize_failed), name = spec.name]() mutable {
              finalize();

              std::shared_lock host_lock(life_state->mtx);

              if VLIKELY (life_state->alive) {
                std::lock_guard lock(mtx_);
                transitioning_.erase(name);
                transition_cv_.notify_all();
              }
            });
      } else {
        finalize_failed();
      }
    }
  };

  std::string invoke_err;
  bool ok = false;

  auto run_init = [&]() -> bool {
    std::lock_guard life_lock(inst->lifecycle_mtx);
    ScopedComponentLogTag tag(spec.name);

    try {
      if (!inst->component->on_configure(*inst->ctx)) {
        invoke_err = "Component '" + spec.name + "' returned false from on_configure().";
        return false;
      }

      if (initial_active) {
        if (!inst->component->on_start(*inst->ctx)) {
          invoke_err = "Component '" + spec.name + "' returned false from on_start().";
          return false;
        }

        inst->started.store(true, std::memory_order_release);
        inst->active->store(true, std::memory_order_release);

        if (!inst->component->on_activate(*inst->ctx)) {
          inst->active->store(false, std::memory_order_release);
          invoke_err = "Component '" + spec.name + "' returned false from on_activate().";
          return false;
        }
      }

      return true;
    } catch (const std::exception& e) {
      inst->active->store(false, std::memory_order_release);
      invoke_err = "Component '" + spec.name + "' threw during startup: " + e.what();
      return false;
    } catch (...) {
      inst->active->store(false, std::memory_order_release);
      invoke_err = "Component '" + spec.name + "' threw unknown exception during startup.";
      return false;
    }
  };

  inst->ctx->set_defer_ready(true);

  if (inst->assigned_loop->is_in_same_thread()) {
    ok = run_init();
  } else {
    try {
      auto fut = inst->assigned_loop->invoke_task(run_init);
      ok = fut.get();
    } catch (const std::future_error& e) {
      invoke_err = "Scheduler loop rejected startup task for '" + spec.name + "': " + e.what();
      ok = false;
    }
  }

  if (!ok) {
    inst->ctx->set_defer_ready(false);

    if (error_out != nullptr) {
      *error_out = invoke_err;
    }

    bool deferred = false;
    teardown_failed_inst(deferred);

    if (!deferred) {
      finish_transition(false);
    }

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

bool ComponentHost::stop_component(const std::string& name) {
  std::shared_ptr<LoadedInstance> inst;

  {
    std::unique_lock lock(mtx_);
    transition_cv_.wait(lock, [&] { return transitioning_.count(name) == 0U; });
    auto it = instances_.find(name);

    if (it == instances_.end()) {
      return false;
    }

    inst = std::move(it->second);
    instances_.erase(it);
    transitioning_.insert(name);
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
    return false;
  }

  if (inst->ctx) {
    inst->ctx->stop_accepting();
  }

  inst->cancel_source.request_cancel();

  if (inst->legacy_plugin) {
    inst->active->store(false, std::memory_order_release);
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
      inst->legacy_plugin->wait_for_quit();
    }

    const std::string lib_name = inst->spec.library;
    inst->legacy_plugin.reset();
    {
      std::lock_guard lock(mtx_);
      (void)plugin_loader_.unload<RunablePluginInterface>(lib_name);
    }
    return true;
  }

  if (inst->component && inst->assigned_loop) {
    auto run_on_stop = [&]() {
      std::lock_guard life_lock(inst->lifecycle_mtx);
      ScopedComponentLogTag tag(name);

      if (inst->active->exchange(false, std::memory_order_acq_rel)) {
        try {
          inst->component->on_deactivate();
        } catch (const std::exception& e) {
          VLOG_E("ComponentHost: Component '", name, "' on_deactivate threw: ", e.what(), ".");
        } catch (...) {
          VLOG_E("ComponentHost: Component '", name, "' on_deactivate threw unknown exception.");
        }
      }

      if (inst->started.exchange(false, std::memory_order_acq_rel)) {
        try {
          inst->component->on_stop();
        } catch (const std::exception& e) {
          VLOG_E("ComponentHost: Component '", name, "' on_stop threw: ", e.what(), ".");
        } catch (...) {
          VLOG_E("ComponentHost: Component '", name, "' on_stop threw unknown exception.");
        }
      }
    };

    auto life_state = life_state_;
    auto finalize_cleanup = [this, life_state, inst, name]() {
      auto run_on_cleanup = [inst, name]() {
        std::lock_guard life_lock(inst->lifecycle_mtx);
        ScopedComponentLogTag tag(name);

        if (inst->component == nullptr) {
          return;
        }

        try {
          inst->component->on_cleanup();
        } catch (const std::exception& e) {
          VLOG_E("ComponentHost: Component '", name, "' on_cleanup threw: ", e.what(), ".");
        } catch (...) {
          VLOG_E("ComponentHost: Component '", name, "' on_cleanup threw unknown exception.");
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

      const bool same = inst->assigned_loop->is_in_same_thread();

      if (inst->assigned_loop->is_running() && !same) {
        try {
          inst->assigned_loop->invoke_task(run_on_cleanup).get();
        } catch (const std::future_error&) {
          run_on_cleanup();
        }
      } else {
        run_on_cleanup();
      }

      schedule_dedicated_release(name, inst->assigned_loop);
    };

    const bool on_same_thread = inst->assigned_loop->is_in_same_thread();

    if (inst->assigned_loop->is_running() && !on_same_thread) {
      try {
        auto fut = inst->assigned_loop->invoke_task(run_on_stop);
        fut.get();
      } catch (const std::future_error&) {
        run_on_stop();
      }
    } else {
      run_on_stop();
    }

    bool drained = true;

    if (inst->ctx) {
      if (on_same_thread) {
        drained = (inst->ctx->in_flight_count() == 0U);
      } else {
        drained = inst->ctx->wait_drained(5000);
      }
    }

    if VUNLIKELY (!drained && inst->ctx) {
      VLOG_W("ComponentHost: Component '", name, "' has active task guards; deferring cleanup.");
      trans_guard.armed = false;
      inst->ctx->set_deferred_cleanup([this, life_state, finalize = std::move(finalize_cleanup), name]() mutable {
        finalize();

        std::shared_lock host_lock(life_state->mtx);

        if VLIKELY (life_state->alive) {
          std::lock_guard lock(mtx_);
          transitioning_.erase(name);
          transition_cv_.notify_all();
        }
      });
    } else {
      finalize_cleanup();
    }
  }

  return true;
}

void ComponentHost::stop_all() {
  std::vector<std::string> names;

  {
    std::lock_guard lock(mtx_);
    names.reserve(instances_.size());

    for (const auto& entry : instances_) {
      names.push_back(entry.first);
    }
  }

  for (const auto& name : names) {
    stop_component(name);
  }

  {
    std::unique_lock lock(mtx_);
    (void)transition_cv_.wait_for(lock, std::chrono::milliseconds(5000), [&] { return transitioning_.empty(); });
    transitioning_.clear();
  }

  transition_cv_.notify_all();

  {
    std::unique_lock host_lock(life_state_->mtx);
    life_state_->alive = false;
  }

  join_reaper_threads();

  std::lock_guard lock(mtx_);
  module_cache_.clear();
  plugin_loader_.clear();
}

bool ComponentHost::activate_component(const std::string& name, std::string* error_out) {
  std::shared_ptr<LoadedInstance> inst;

  {
    std::lock_guard lock(mtx_);
    auto it = instances_.find(name);

    if (it == instances_.end() || !it->second || !it->second->component || !it->second->ctx) {
      if (error_out != nullptr) {
        *error_out = "Component '" + name + "' is not loaded or does not support lifecycle activation.";
      }

      return false;
    }

    inst = it->second;
  }

  auto guard = inst->ctx->acquire_task_guard();

  if (!guard) {
    if (error_out != nullptr) {
      *error_out = "Component '" + name + "' is shutting down.";
    }

    return false;
  }

  std::string invoke_err;

  auto apply = [&]() -> bool {
    std::lock_guard life_lock(inst->lifecycle_mtx);

    if (!inst->component) {
      invoke_err = "Component '" + name + "' is no longer valid.";
      return false;
    }

    if (inst->active->load(std::memory_order_acquire)) {
      return true;
    }

    ScopedComponentLogTag tag(name);
    inst->ctx->reset_ready_signal();

    try {
      if (!inst->started.load(std::memory_order_acquire)) {
        if (!inst->component->on_start(*inst->ctx)) {
          invoke_err = "Component '" + name + "' returned false from on_start().";
          return false;
        }

        inst->started.store(true, std::memory_order_release);
      }

      inst->active->store(true, std::memory_order_release);

      if (!inst->component->on_activate(*inst->ctx)) {
        inst->active->store(false, std::memory_order_release);
        invoke_err = "Component '" + name + "' returned false from on_activate().";
        return false;
      }

      return true;
    } catch (const std::exception& e) {
      inst->active->store(false, std::memory_order_release);
      invoke_err = "Component '" + name + "' on_activate threw: " + e.what();
      return false;
    } catch (...) {
      inst->active->store(false, std::memory_order_release);
      invoke_err = "Component '" + name + "' on_activate threw unknown exception.";
      return false;
    }
  };

  bool ok = false;

  if (inst->assigned_loop && inst->assigned_loop->is_running() && !inst->assigned_loop->is_in_same_thread()) {
    try {
      ok = inst->assigned_loop->invoke_task(apply).get();
    } catch (const std::future_error& e) {
      invoke_err = std::string("Failed to dispatch activate task: ") + e.what();
      ok = false;
    }
  } else {
    ok = apply();
  }

  if (!ok && error_out != nullptr) {
    *error_out = invoke_err.empty() ? ("Component '" + name + "' activation failed.") : invoke_err;
  }

  return ok;
}

bool ComponentHost::deactivate_component(const std::string& name, std::string* error_out) {
  std::shared_ptr<LoadedInstance> inst;

  {
    std::lock_guard lock(mtx_);
    auto it = instances_.find(name);

    if (it == instances_.end() || !it->second || !it->second->component || !it->second->ctx) {
      if (error_out != nullptr) {
        *error_out = "Component '" + name + "' is not loaded or does not support lifecycle deactivation.";
      }

      return false;
    }

    inst = it->second;
  }

  auto guard = inst->ctx->acquire_task_guard();

  if (!guard) {
    if (error_out != nullptr) {
      *error_out = "Component '" + name + "' is shutting down.";
    }

    return false;
  }

  std::string invoke_err;

  auto apply = [&]() -> bool {
    std::lock_guard life_lock(inst->lifecycle_mtx);

    if (!inst->component) {
      return false;
    }

    if (!inst->active->load(std::memory_order_acquire)) {
      return true;
    }

    ScopedComponentLogTag tag(name);

    try {
      inst->active->store(false, std::memory_order_release);
      inst->component->on_deactivate();
      return true;
    } catch (const std::exception& e) {
      inst->active->store(true, std::memory_order_release);
      invoke_err = "Component '" + name + "' on_deactivate threw: " + e.what();
      return false;
    } catch (...) {
      inst->active->store(true, std::memory_order_release);
      invoke_err = "Component '" + name + "' on_deactivate threw unknown exception.";
      return false;
    }
  };

  bool ok = false;

  if (inst->assigned_loop && inst->assigned_loop->is_running() && !inst->assigned_loop->is_in_same_thread()) {
    try {
      ok = inst->assigned_loop->invoke_task(apply).get();
    } catch (const std::future_error& e) {
      invoke_err = std::string("Failed to dispatch deactivate task: ") + e.what();
      ok = false;
    }
  } else {
    ok = apply();
  }

  if (!ok && error_out != nullptr) {
    *error_out = invoke_err.empty() ? ("Component '" + name + "' deactivation failed.") : invoke_err;
  }

  return ok;
}

bool ComponentHost::reload_component(const std::string& name, std::string* error_out) {
  std::shared_ptr<LoadedInstance> inst;

  {
    std::lock_guard lock(mtx_);
    auto it = instances_.find(name);

    if (it == instances_.end() || !it->second || !it->second->component || !it->second->ctx) {
      if (error_out != nullptr) {
        *error_out = "Component '" + name + "' is not active or does not support reload.";
      }

      return false;
    }

    inst = it->second;
  }

  auto guard = inst->ctx->acquire_task_guard();

  if (!guard) {
    if (error_out != nullptr) {
      *error_out = "Component '" + name + "' is shutting down.";
    }

    return false;
  }

  auto apply = [&]() -> bool {
    std::lock_guard life_lock(inst->lifecycle_mtx);

    if (!inst->component || !inst->started.load(std::memory_order_acquire)) {
      return false;
    }

    ScopedComponentLogTag tag(name);

    try {
      return inst->component->on_reload(*inst->ctx);
    } catch (const std::exception& e) {
      if (error_out != nullptr) {
        *error_out = e.what();
      }

      return false;
    } catch (...) {
      if (error_out != nullptr) {
        *error_out = "Unknown exception in on_reload().";
      }

      return false;
    }
  };

  bool ok = false;

  if (inst->assigned_loop && inst->assigned_loop->is_running() && !inst->assigned_loop->is_in_same_thread()) {
    try {
      auto fut = inst->assigned_loop->invoke_task(apply);
      ok = fut.get();
    } catch (const std::future_error& e) {
      if (error_out != nullptr) {
        *error_out = e.what();
      }

      ok = false;
    }
  } else {
    ok = apply();
  }

  if (!ok && error_out != nullptr && error_out->empty()) {
    *error_out = "Component '" + name + "' rejected reload.";
  }

  return ok;
}

bool ComponentHost::has_component(const std::string& name) const {
  std::lock_guard lock(mtx_);
  auto it = instances_.find(name);
  return it != instances_.end() && it->second && it->second->component != nullptr;
}

double ComponentHost::get_cpu_usage(const std::string& name) const {
  std::shared_ptr<LoadedInstance> inst;

  {
    std::lock_guard lock(mtx_);
    auto it = instances_.find(name);

    if (it == instances_.end() || !it->second || !it->second->ctx) {
      return -1.0;
    }

    inst = it->second;
  }

  return inst->ctx->get_cpu_usage();
}

uint64_t ComponentHost::get_last_watchdog_ms(const std::string& name) const {
  std::shared_ptr<LoadedInstance> inst;

  {
    std::lock_guard lock(mtx_);
    auto it = instances_.find(name);

    if (it == instances_.end() || !it->second || !it->second->ctx) {
      return 0U;
    }

    inst = it->second;
  }

  return inst->ctx->last_watchdog_ms();
}

bool ComponentHost::get_params(const std::string& name, LauncherParamMap& out) const {
  std::shared_ptr<LoadedInstance> inst;

  {
    std::lock_guard lock(mtx_);
    auto it = instances_.find(name);

    if (it == instances_.end() || !it->second || !it->second->ctx) {
      return false;
    }

    inst = it->second;
  }

  out = inst->ctx->params();
  return true;
}

bool ComponentHost::set_param(const std::string& name, std::string_view key, const LauncherParamValue& value,
                              std::string* error_out) {
  std::shared_ptr<LoadedInstance> inst;

  {
    std::lock_guard lock(mtx_);
    auto it = instances_.find(name);

    if (it == instances_.end() || !it->second || !it->second->ctx || !it->second->component) {
      if (error_out != nullptr) {
        *error_out = "Component '" + name + "' is not active or does not support dynamic params.";
      }

      return false;
    }

    inst = it->second;
  }

  auto guard = inst->ctx->acquire_task_guard();

  if (!guard) {
    if (error_out != nullptr) {
      *error_out = "Component '" + name + "' is shutting down.";
    }

    return false;
  }

  const std::string key_copy(key);

  auto apply = [&]() -> bool {
    std::lock_guard life_lock(inst->lifecycle_mtx);

    if (!inst->component) {
      return false;
    }

    ScopedComponentLogTag tag(name);

    try {
      if (!inst->component->on_param_changed(key_copy, value)) {
        return false;
      }

      inst->ctx->update_param(key_copy, value);
      return true;
    } catch (const std::exception& e) {
      if (error_out != nullptr) {
        *error_out = e.what();
      }

      return false;
    } catch (...) {
      if (error_out != nullptr) {
        *error_out = "Unknown exception in on_param_changed().";
      }

      return false;
    }
  };

  bool accepted = false;

  if (inst->assigned_loop && inst->assigned_loop->is_running() && !inst->assigned_loop->is_in_same_thread()) {
    try {
      auto fut = inst->assigned_loop->invoke_task(apply);
      accepted = fut.get();
    } catch (const std::future_error& e) {
      if (error_out != nullptr) {
        *error_out = e.what();
      }

      accepted = false;
    }
  } else {
    accepted = apply();
  }

  if (!accepted && error_out != nullptr && error_out->empty()) {
    *error_out = "Component '" + name + "' rejected parameter update for '" + key_copy + "'.";
  }

  return accepted;
}

bool ComponentHost::set_component_record(const std::string& name, const std::string& path, bool force_publish,
                                         std::string* error_out) {
  std::shared_ptr<LoadedInstance> inst;

  {
    std::lock_guard lock(mtx_);
    auto it = instances_.find(name);

    if (it == instances_.end() || !it->second || !it->second->ctx || !it->second->component) {
      if (error_out != nullptr) {
        *error_out = "Component '" + name + "' is not active or does not support dynamic recording.";
      }

      return false;
    }

    inst = it->second;
  }

  auto guard = inst->ctx->acquire_task_guard();

  if (!guard) {
    if (error_out != nullptr) {
      *error_out = "Component '" + name + "' is shutting down.";
    }

    return false;
  }

  auto apply = [&]() -> bool {
    std::lock_guard life_lock(inst->lifecycle_mtx);

    if (!inst->component) {
      return false;
    }

    try {
      ScopedComponentLogTag tag(name);
      inst->ctx->apply_record_config(path, force_publish);
      return true;
    } catch (const std::exception& e) {
      if (error_out != nullptr) {
        *error_out = e.what();
      }

      return false;
    } catch (...) {
      if (error_out != nullptr) {
        *error_out = "Unknown exception while updating recording configuration.";
      }

      return false;
    }
  };

  bool ok = false;

  if (inst->assigned_loop && inst->assigned_loop->is_running() && !inst->assigned_loop->is_in_same_thread()) {
    try {
      ok = inst->assigned_loop->invoke_task(apply).get();
    } catch (const std::exception& e) {
      if (error_out != nullptr) {
        *error_out = e.what();
      }

      ok = false;
    } catch (...) {
      if (error_out != nullptr) {
        *error_out = "Unknown exception while updating recording configuration.";
      }

      ok = false;
    }
  } else {
    ok = apply();
  }

  if (!ok && error_out != nullptr && error_out->empty()) {
    *error_out = "Component '" + name + "' is not running.";
  }

  return ok;
}

std::deque<std::string> ComponentHost::build_search_paths() const {
  auto paths = Plugin::default_search_path();

  for (size_t i = plugin_dirs_.size(); i > 0U; --i) {
    paths.push_front(plugin_dirs_[i - 1U]);
  }

  return paths;
}

std::shared_ptr<LauncherModuleInterface> ComponentHost::load_or_get_module(const std::string& lib_name) {
  auto it = module_cache_.find(lib_name);

  if (it != module_cache_.end()) {
    return it->second;
  }

  const auto search_paths = build_search_paths();
  auto mod = plugin_loader_.load<LauncherModuleInterface>(lib_name, LauncherModuleInterface::kVersionMajor,
                                                          LauncherModuleInterface::kVersionMinor, "", search_paths);

  if (!mod) {
    return nullptr;
  }

  module_cache_.emplace(lib_name, mod);
  return mod;
}

void ComponentHost::schedule_dedicated_release(const std::string& name, MessageLoop* assigned_loop) {
  auto loop = scheduler_.detach_dedicated_loop(name);

  if (!loop) {
    return;
  }

  if (assigned_loop != nullptr && assigned_loop->is_in_same_thread()) {
    std::vector<ReaperEntry> finished;

    {
      std::lock_guard lock(reaper_mtx_);
      size_t write_idx = 0U;

      for (size_t i = 0U; i < reaper_threads_.size(); ++i) {
        if (reaper_threads_[i].done && reaper_threads_[i].done->load(std::memory_order_acquire)) {
          finished.push_back(std::move(reaper_threads_[i]));
        } else {
          if (write_idx != i) {
            reaper_threads_[write_idx] = std::move(reaper_threads_[i]);
          }

          ++write_idx;
        }
      }

      reaper_threads_.resize(write_idx);

      auto done_flag = MemoryResource::make_shared<std::atomic_bool>(false);
      std::thread reaper([owned = std::move(loop), done_flag]() mutable {
        if (owned && owned->is_running()) {
          owned->quit(false);
          owned->wait_for_quit();
        }

        done_flag->store(true, std::memory_order_release);
      });
      reaper_threads_.push_back(ReaperEntry{std::move(done_flag), std::move(reaper)});
    }

    for (auto& entry : finished) {
      if (entry.thread.joinable()) {
        entry.thread.join();
      }
    }

    return;
  }

  if (loop->is_running()) {
    loop->quit(false);
    loop->wait_for_quit();
  }
}

void ComponentHost::join_reaper_threads() {
  std::vector<ReaperEntry> to_join;

  {
    std::lock_guard lock(reaper_mtx_);
    to_join.swap(reaper_threads_);
  }

  for (auto& entry : to_join) {
    if (entry.thread.joinable()) {
      entry.thread.join();
    }
  }
}

}  // namespace launcher

}  // namespace vlink
