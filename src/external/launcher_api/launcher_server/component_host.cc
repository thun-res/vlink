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

#include "component_host.h"

#include <vlink/base/condition_variable.h>
#include <vlink/base/elapsed_timer.h>
#include <vlink/base/helpers.h>
#include <vlink/base/logger.h>
#include <vlink/base/macros.h>
#include <vlink/base/multi_loop.h>

#include <chrono>
#include <filesystem>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace vlink::launcher {

static bool run_on_loop_sync(MessageLoop* loop, MoveFunction<bool()>&& fn, uint32_t timeout_ms, bool& completed_out) {
  completed_out = false;

  if VUNLIKELY (loop == nullptr || !loop->is_running()) {
    return false;
  }

  if (loop->is_in_same_thread()) {
    completed_out = true;

    try {
      return fn();
    } catch (...) {
      return false;
    }
  }

  struct SyncState final {
    std::mutex mtx;
    ConditionVariable cv;
    bool finished{false};
    bool executed{false};
    bool ok{false};
  };

  struct FinishGuard final {
    explicit FinishGuard(std::shared_ptr<SyncState> s) : state(std::move(s)) {}
    FinishGuard(const FinishGuard&) = delete;
    FinishGuard& operator=(const FinishGuard&) = delete;
    ~FinishGuard() {
      if (state) {
        {
          std::lock_guard lock(state->mtx);
          state->finished = true;
        }

        state->cv.notify_all();
      }
    }
    std::shared_ptr<SyncState> state;
  };

  auto state = std::make_shared<SyncState>();
  auto guard = std::make_shared<FinishGuard>(state);

  auto cb = [state, guard = std::move(guard), task = std::move(fn)]() mutable {
    bool res = false;

    try {
      res = task();
    } catch (...) {
      res = false;
    }

    {
      std::lock_guard lock(state->mtx);
      state->ok = res;
      state->executed = true;
      state->finished = true;
    }

    state->cv.notify_all();
    guard->state.reset();
  };

  PostTaskOptions opts;
  opts.drop_policy = TaskDropPolicy::kProtected;
  const TaskHandle handle =
      (loop->get_type() == MessageLoop::kPriorityType)
          ? loop->post_task_with_priority_handle(std::move(cb), MessageLoop::kHighestPriority, opts)
          : loop->post_task_handle(std::move(cb), opts);
  const auto st = handle.state();

  if VUNLIKELY (st == TaskExecutionState::kInvalid || st == TaskExecutionState::kRejected) {
    return false;
  }

  std::unique_lock lock(state->mtx);
  const bool woke =
      state->cv.wait_for(lock, std::chrono::milliseconds(timeout_ms), [&state]() { return state->finished; });
  completed_out = woke && state->executed;
  return completed_out && state->ok;
}

ComponentHost::~ComponentHost() { stop_all(); }

void ComponentHost::set_plugin_dirs(const std::vector<std::string>& dirs) {
  std::lock_guard lock(mtx_);
  plugin_dirs_ = dirs;
}

void ComponentHost::set_callbacks(ReadyCallback on_ready, HealthCallback on_health, StopRequestCallback on_stop_req) {
  auto next = std::make_shared<Callbacks>();
  next->on_ready = std::move(on_ready);
  next->on_health = std::move(on_health);
  next->on_stop_req = std::move(on_stop_req);
  std::shared_ptr<Callbacks> old;

  {
    std::lock_guard lock(shared_cbs_->mtx);
    old = std::move(shared_cbs_->callbacks);
    shared_cbs_->callbacks = std::move(next);
  }
}

MessageLoop* ComponentHost::ensure_shared_loop_locked(MessageLoop::Type qtype, MessageLoop::Strategy qstrat) {
  const auto key = static_cast<uint16_t>((static_cast<uint16_t>(qtype) << 8U) | static_cast<uint16_t>(qstrat));
  auto& loop = shared_loops_[key];

  if (!loop) {
    auto candidate = std::make_unique<MessageLoop>(qtype);
    candidate->set_strategy(qstrat);
    candidate->set_name("vl_comp_shared");

    if (!candidate->async_run()) {
      return nullptr;
    }

    loop = std::move(candidate);
  } else if (!loop->is_running() && !loop->async_run()) {
    return nullptr;
  }

  return loop.get();
}

std::shared_ptr<LauncherModuleInterface> ComponentHost::load_module_locked(const std::string& library,
                                                                           std::string* error_out) {
  auto cached = modules_.find(library);

  if (cached != modules_.end()) {
    return cached->second;
  }

  auto try_load = [&](const std::string& candidate) {
    auto it = modules_.find(candidate);

    if (it != modules_.end()) {
      return it->second;
    }

    return plugin_loader_.load<LauncherModuleInterface>(candidate, LauncherModuleInterface::kVersionMajor,
                                                        LauncherModuleInterface::kVersionMinor);
  };

  std::string resolved_path = library;
  auto mod = try_load(library);

  if (!mod) {
    for (const auto& dir : plugin_dirs_) {
      const std::filesystem::path base(dir);
      const std::vector<std::string> names = {
          library,
#if defined(_WIN32) || defined(_WIN64)
          library + ".dll",
#elif defined(__APPLE__)
          "lib" + library + ".dylib",
#else
          "lib" + library + ".so",
#endif
      };

      for (const auto& fname : names) {
        const std::filesystem::path p = base / fname;
        std::error_code ec;

        if (std::filesystem::exists(p, ec)) {
          const std::string full = Helpers::path_to_string(p);
          mod = try_load(full);

          if (mod) {
            resolved_path = full;
            break;
          }
        }
      }

      if (mod) {
        break;
      }
    }
  }

  if (!mod) {
    if (error_out != nullptr) {
      *error_out = "Failed to load component library '" + library + "'.";
    }

    return nullptr;
  }

  modules_[library] = mod;

  if (resolved_path != library) {
    modules_[resolved_path] = mod;
  }

  return mod;
}

void ComponentHost::cleanup_entry(const std::shared_ptr<Entry>& entry, bool invoke_on_stop, uint32_t timeout_ms,
                                  bool start_completed) {
  std::lock_guard op_lock(entry->op_mtx);
  entry->stopped.store(true, std::memory_order_release);

  if (entry->context) {
    entry->context->set_stopping(true);
  }

  bool safe_to_destroy = start_completed;

  if (invoke_on_stop && entry->started && entry->instance != nullptr && entry->context &&
      entry->active_loop != nullptr) {
    bool completed = false;
    (void)run_on_loop_sync(
        entry->active_loop,
        [entry]() {
          if (entry->instance != nullptr && entry->context) {
            entry->instance->on_stop(*entry->context);
          }

          return true;
        },
        timeout_ms, completed);
    safe_to_destroy = safe_to_destroy && completed;
  }

  if (safe_to_destroy && entry->instance != nullptr) {
    bool drained = entry->instance->on_drain_stop(timeout_ms);

    if (!drained && entry->dedicated_loop) {
      entry->dedicated_loop->quit(false);
      (void)entry->dedicated_loop->wait_for_quit(static_cast<int>(std::min<uint32_t>(timeout_ms, 3000U)));
      drained = entry->instance->on_drain_stop(500U);
    }

    safe_to_destroy = drained;
  }

  if (safe_to_destroy && entry->dedicated_loop && entry->dedicated_loop->is_running()) {
    entry->dedicated_loop->quit(false);
    safe_to_destroy = entry->dedicated_loop->wait_for_quit(static_cast<int>(std::min<uint32_t>(timeout_ms, 5000U)));
  }

  if (!safe_to_destroy || entry->in_flight_ops.load(std::memory_order_acquire) != 0U) {
    if (entry->dedicated_loop) {
      entry->dedicated_loop->quit(false);
    }

    std::lock_guard lock(mtx_);
    orphans_.push_back(entry);
    return;
  }

  // Destroy component instance BEFORE tearing down dedicated_loop and context so destructors never observe freed loop.
  if (entry->module && entry->instance != nullptr) {
    entry->module->destroy_component(entry->instance);
    entry->instance = nullptr;
  }

  if (entry->dedicated_loop) {
    entry->dedicated_loop.reset();
    entry->active_loop = nullptr;
  }

  entry->context.reset();
}

bool ComponentHost::start_component(const ComponentSpec& spec, uint64_t generation, std::string* error_out) {
  (void)stop_component(spec.name, spec.stop_timeout_ms);

  auto entry = std::make_shared<Entry>();
  entry->name = spec.name;
  entry->generation = generation;
  std::shared_ptr<Entry> replaced;

  {
    std::lock_guard lock(mtx_);
    entry->module = load_module_locked(spec.library, error_out);

    if (!entry->module) {
      return false;
    }

    entry->instance = entry->module->create_component(spec.class_name);

    if (entry->instance == nullptr) {
      if (error_out != nullptr) {
        *error_out = "Component class '" + spec.class_name + "' not found in '" + spec.library + "'.";
      }

      return false;
    }

    uint32_t active_threads = 1U;

    if (spec.scheduler == SchedulerMode::kMulti) {
      active_threads = std::max<uint32_t>(2U, spec.threads);
      entry->dedicated_loop = std::make_unique<MultiLoop>(active_threads, spec.queue_type);
      entry->dedicated_loop->set_strategy(spec.queue_strategy);
      entry->dedicated_loop->set_name(spec.name.substr(0U, 15U));

      if (!entry->dedicated_loop->async_run()) {
        entry->module->destroy_component(entry->instance);
        entry->instance = nullptr;

        if (error_out != nullptr) {
          *error_out = "Failed to start multi loop for '" + spec.name + "'.";
        }

        return false;
      }

      entry->active_loop = entry->dedicated_loop.get();
    } else if (spec.scheduler == SchedulerMode::kDedicated || spec.dedicated) {
      entry->dedicated_loop = std::make_unique<MessageLoop>(spec.queue_type);
      entry->dedicated_loop->set_strategy(spec.queue_strategy);
      entry->dedicated_loop->set_name(spec.name.substr(0U, 15U));

      if (!entry->dedicated_loop->async_run()) {
        entry->module->destroy_component(entry->instance);
        entry->instance = nullptr;

        if (error_out != nullptr) {
          *error_out = "Failed to start dedicated loop for '" + spec.name + "'.";
        }

        return false;
      }

      entry->active_loop = entry->dedicated_loop.get();
    } else {
      entry->active_loop = ensure_shared_loop_locked(spec.queue_type, spec.queue_strategy);

      if (entry->active_loop == nullptr) {
        entry->module->destroy_component(entry->instance);
        entry->instance = nullptr;

        if (error_out != nullptr) {
          *error_out = "Failed to start shared loop for '" + spec.name + "'.";
        }

        return false;
      }
    }

    std::weak_ptr<Entry> weak_entry = entry;
    std::weak_ptr<SharedCallbacks> weak_cbs = shared_cbs_;
    LauncherContext::Hooks hooks;
    hooks.name = spec.name;
    hooks.ns = spec.ns;
    hooks.loop = entry->active_loop;
    hooks.params = spec.params;
    hooks.remaps = spec.remaps;
    hooks.watchdog_ms = spec.watchdog_ms;
    hooks.worker_threads = active_threads;
    hooks.standalone = false;
    hooks.on_ready = [weak_cbs, weak_entry](std::string_view detail) {
      auto e = weak_entry.lock();
      auto shared = weak_cbs.lock();

      if (!e || !shared || e->stopped.load(std::memory_order_acquire)) {
        return;
      }

      std::shared_ptr<Callbacks> cbs;

      {
        std::lock_guard cb_lock(shared->mtx);
        cbs = shared->callbacks;
      }

      if (cbs && cbs->on_ready) {
        cbs->on_ready(e->name, e->generation, std::string(detail));
      }
    };
    hooks.on_health = [weak_cbs, weak_entry](bool healthy, std::string_view detail) {
      auto e = weak_entry.lock();
      auto shared = weak_cbs.lock();

      if (!e || !shared || e->stopped.load(std::memory_order_acquire)) {
        return;
      }

      std::shared_ptr<Callbacks> cbs;

      {
        std::lock_guard cb_lock(shared->mtx);
        cbs = shared->callbacks;
      }

      if (cbs && cbs->on_health) {
        cbs->on_health(e->name, e->generation, healthy, std::string(detail));
      }
    };
    hooks.on_watchdog = [weak_entry]() {
      auto e = weak_entry.lock();

      if (e && !e->stopped.load(std::memory_order_acquire)) {
        e->last_watchdog_ms.store(ElapsedTimer::get_cpu_timestamp(ElapsedTimer::kMilli), std::memory_order_release);
      }
    };
    hooks.on_request_stop = [weak_cbs, weak_entry]() {
      auto e = weak_entry.lock();
      auto shared = weak_cbs.lock();

      if (!e || !shared || e->stopped.load(std::memory_order_acquire)) {
        return;
      }

      std::shared_ptr<Callbacks> cbs;

      {
        std::lock_guard cb_lock(shared->mtx);
        cbs = shared->callbacks;
      }

      if (cbs && cbs->on_stop_req) {
        cbs->on_stop_req(e->name, e->generation);
      }
    };

    entry->context = std::make_unique<LauncherContext>(std::move(hooks));
    auto old_it = entries_.find(spec.name);

    if (old_it != entries_.end()) {
      replaced = std::move(old_it->second);
    }

    entries_[spec.name] = entry;
  }

  if (replaced) {
    cleanup_entry(replaced, true, spec.stop_timeout_ms);
    replaced.reset();
  }

  bool completed = false;
  bool ok = false;

  {
    std::lock_guard op_lock(entry->op_mtx);

    if (!entry->stopped.load(std::memory_order_acquire)) {
      ok = run_on_loop_sync(
          entry->active_loop, [entry]() { return entry->instance->on_start(*entry->context); }, 5000U, completed);

      if (ok) {
        entry->started = true;
      }
    }
  }

  if (!ok) {
    if (error_out != nullptr) {
      *error_out = "Component '" + spec.name + "' on_start() failed.";
    }

    {
      std::lock_guard lock(mtx_);
      auto it = entries_.find(spec.name);

      if (it != entries_.end() && it->second == entry) {
        entries_.erase(it);
      }
    }

    cleanup_entry(entry, false, spec.stop_timeout_ms, completed);
    return false;
  }

  return true;
}

bool ComponentHost::stop_component(const std::string& name, uint32_t timeout_ms, std::optional<uint64_t> expected_gen) {
  std::shared_ptr<Entry> entry;

  {
    std::lock_guard lock(mtx_);
    auto it = entries_.find(name);

    if (it == entries_.end()) {
      return true;
    }

    if (expected_gen.has_value() && it->second->generation != *expected_gen) {
      return true;
    }

    entry = std::move(it->second);
    entries_.erase(it);
  }

  cleanup_entry(entry, true, timeout_ms);
  return true;
}

bool ComponentHost::reload_component(const std::string& name, std::string* error_out) {
  std::shared_ptr<Entry> entry;

  {
    std::lock_guard lock(mtx_);
    auto it = entries_.find(name);

    if (it == entries_.end()) {
      if (error_out != nullptr) {
        *error_out = "Component '" + name + "' is not running.";
      }

      return false;
    }

    entry = it->second;
  }

  std::lock_guard op_lock(entry->op_mtx);

  if (entry->stopped.load(std::memory_order_acquire) || !entry->started || entry->instance == nullptr ||
      entry->active_loop == nullptr) {
    if (error_out != nullptr) {
      *error_out = "Component '" + name + "' is not running.";
    }

    return false;
  }

  struct InFlightGuard final {
    std::shared_ptr<Entry> e;
    explicit InFlightGuard(std::shared_ptr<Entry> ent) : e(std::move(ent)) {
      if (e) {
        e->in_flight_ops.fetch_add(1U, std::memory_order_acq_rel);
      }
    }
    InFlightGuard(const InFlightGuard&) = delete;
    InFlightGuard& operator=(const InFlightGuard&) = delete;
    ~InFlightGuard() {
      if (e) {
        e->in_flight_ops.fetch_sub(1U, std::memory_order_acq_rel);
      }
    }
  };

  bool completed = false;
  auto guard = std::make_shared<InFlightGuard>(entry);
  const bool ok = run_on_loop_sync(
      entry->active_loop,
      [entry, guard = std::move(guard)]() {
        (void)guard;

        if (entry->stopped.load(std::memory_order_acquire) || entry->instance == nullptr || !entry->context) {
          return false;
        }

        return entry->instance->on_reload(*entry->context);
      },
      5000U, completed);

  if (!ok && error_out != nullptr) {
    *error_out = "Component '" + name + "' on_reload() returned false.";
  }

  return ok;
}

bool ComponentHost::set_param(const std::string& name, const std::string& key, const LauncherParamValue& value,
                              std::string* error_out) {
  std::shared_ptr<Entry> entry;

  {
    std::lock_guard lock(mtx_);
    auto it = entries_.find(name);

    if (it == entries_.end()) {
      if (error_out != nullptr) {
        *error_out = "Component '" + name + "' is not running.";
      }

      return false;
    }

    entry = it->second;
  }

  std::lock_guard op_lock(entry->op_mtx);

  if (entry->stopped.load(std::memory_order_acquire) || !entry->started || entry->instance == nullptr ||
      entry->active_loop == nullptr) {
    if (error_out != nullptr) {
      *error_out = "Component '" + name + "' is not running.";
    }

    return false;
  }

  struct InFlightGuard final {
    std::shared_ptr<Entry> e;
    explicit InFlightGuard(std::shared_ptr<Entry> ent) : e(std::move(ent)) {
      if (e) {
        e->in_flight_ops.fetch_add(1U, std::memory_order_acq_rel);
      }
    }
    InFlightGuard(const InFlightGuard&) = delete;
    InFlightGuard& operator=(const InFlightGuard&) = delete;
    ~InFlightGuard() {
      if (e) {
        e->in_flight_ops.fetch_sub(1U, std::memory_order_acq_rel);
      }
    }
  };

  bool completed = false;
  auto guard = std::make_shared<InFlightGuard>(entry);
  const bool ok = run_on_loop_sync(
      entry->active_loop,
      [entry, guard = std::move(guard), key, val = value]() mutable {
        (void)guard;

        if (entry->stopped.load(std::memory_order_acquire) || entry->instance == nullptr || !entry->context) {
          return false;
        }

        if (!entry->instance->on_param_changed(*entry->context, key, val)) {
          return false;
        }

        entry->context->update_param_internal(key, std::move(val));
        return true;
      },
      5000U, completed);

  if (!ok && error_out != nullptr) {
    *error_out = "Component '" + name + "' rejected parameter update '" + key + "'.";
  }

  return ok;
}

std::optional<LauncherParamMap> ComponentHost::get_params(const std::string& name) const {
  std::shared_ptr<Entry> entry;

  {
    std::lock_guard lock(mtx_);
    auto it = entries_.find(name);

    if (it == entries_.end() || !it->second->context) {
      return std::nullopt;
    }

    entry = it->second;
  }

  return entry->context->params();
}

uint64_t ComponentHost::get_last_watchdog_ms(const std::string& name) const noexcept {
  std::lock_guard lock(mtx_);
  auto it = entries_.find(name);

  if (it == entries_.end()) {
    return 0U;
  }

  return it->second->last_watchdog_ms.load(std::memory_order_acquire);
}

void ComponentHost::stop_all() {
  std::vector<std::string> names;

  {
    std::lock_guard lock(mtx_);
    names.reserve(entries_.size());

    for (const auto& [k, v] : entries_) {
      (void)v;
      names.push_back(k);
    }
  }

  for (const auto& n : names) {
    (void)stop_component(n, 3000U);
  }

  std::unordered_map<uint16_t, std::unique_ptr<MessageLoop>> shared_loops;

  {
    std::lock_guard lock(mtx_);
    shared_loops.swap(shared_loops_);
  }

  for (auto& [k, loop] : shared_loops) {
    (void)k;

    if (loop) {
      loop->quit(false);
      loop->wait_for_quit();
      loop.reset();
    }
  }

  std::vector<std::shared_ptr<Entry>> orphans;

  {
    std::lock_guard lock(mtx_);
    orphans.swap(orphans_);
  }

  for (auto& orphan : orphans) {
    bool loop_stopped = true;

    if (orphan->dedicated_loop) {
      orphan->dedicated_loop->quit(false);
      loop_stopped = orphan->dedicated_loop->wait_for_quit(2000);
    }

    if (loop_stopped && orphan->in_flight_ops.load(std::memory_order_acquire) == 0U &&
        (orphan->instance == nullptr || orphan->instance->on_drain_stop(1000U))) {
      if (orphan->module && orphan->instance != nullptr) {
        orphan->module->destroy_component(orphan->instance);
        orphan->instance = nullptr;
      }

      orphan->dedicated_loop.reset();
      orphan->active_loop = nullptr;
      orphan->context.reset();
    } else {
      VLOG_W("ComponentHost: Orphan component '", orphan->name,
             "' did not finish within shutdown timeout; skipping destroy.");
    }
  }

  orphans.clear();

  std::lock_guard lock(mtx_);
  modules_.clear();
  plugin_loader_.clear();
}

}  // namespace vlink::launcher
