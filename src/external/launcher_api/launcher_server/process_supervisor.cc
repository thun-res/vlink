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

#include "process_supervisor.h"

#include <vlink/base/logger.h>
#include <vlink/base/macros.h>

#include <algorithm>
#include <cstdio>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#if !defined(_WIN32) && !defined(_WIN64)
#include <sys/types.h>

#include <csignal>
#endif

namespace vlink::launcher {

static void signal_process_group(int64_t pid, int sig) noexcept {
#if !defined(_WIN32) && !defined(_WIN64)
  if (pid > 0) {
    (void)::kill(-static_cast<pid_t>(pid), sig);
  }
#else
  (void)pid;
  (void)sig;
#endif
}

ProcessSupervisor::~ProcessSupervisor() { stop_all(); }

static constexpr size_t kMaxPendingStdoutBytes = 65536U;

void ProcessSupervisor::set_callbacks(ExitCallback on_exit, StdoutReadyCallback on_stdout_ready) {
  auto next = std::make_shared<Callbacks>();
  next->on_exit = std::move(on_exit);
  next->on_stdout_ready = std::move(on_stdout_ready);
  std::shared_ptr<Callbacks> old;

  {
    std::lock_guard lock(shared_cbs_->mtx);
    old = std::move(shared_cbs_->callbacks);
    shared_cbs_->callbacks = std::move(next);
  }
}

void ProcessSupervisor::stop_entry(const std::shared_ptr<Entry>& entry, uint32_t timeout_ms) {
  if (!entry || !entry->state) {
    return;
  }

  entry->state->expected_stop.store(true, std::memory_order_release);

  if (!entry->proc) {
    return;
  }

  if (entry->proc->get_state() == Process::kStartingState) {
    (void)entry->proc->wait_for_started(500);
  }

  if (entry->proc->is_running()) {
    const int64_t pid = entry->state->pid.load(std::memory_order_acquire);
#if !defined(_WIN32) && !defined(_WIN64)
    if (pid > 0) {
      signal_process_group(pid, SIGTERM);
    } else {
      entry->proc->terminate();
    }
#else
    (void)pid;
    entry->proc->terminate();
#endif

    const int wait_ms = static_cast<int>(std::clamp<uint32_t>(timeout_ms, 50U, 60000U));

    if (!entry->proc->wait_for_finished(wait_ms)) {
#if !defined(_WIN32) && !defined(_WIN64)
      if (pid > 0) {
        signal_process_group(pid, SIGKILL);
      }
#endif
      entry->proc->kill();
      (void)entry->proc->wait_for_finished(1000);
    } else {
#if !defined(_WIN32) && !defined(_WIN64)
      if (pid > 0) {
        signal_process_group(pid, SIGKILL);
      }
#endif
    }
  }
}

bool ProcessSupervisor::start_process(const ProcessLaunchRequest& req, int64_t* pid_out, std::string* error_out) {
  (void)stop_process(req.name, req.stop_timeout_ms);

  auto state = std::make_shared<EntryState>();
  state->name = req.name;
  state->generation = req.generation;
  state->output = req.output;
  state->ready_pattern = req.ready_stdout_pattern;

  auto entry = std::make_shared<Entry>();
  entry->state = state;
  entry->proc = std::make_unique<Process>();
  Process* const proc_raw = entry->proc.get();

  entry->proc->set_create_process_group(true);
  entry->proc->set_inherit_environment(req.inherit_env);
  entry->proc->set_environment(req.env);

  if (!req.cwd.empty()) {
    entry->proc->set_working_directory(req.cwd);
  }

  const bool capture_stdout = !req.ready_stdout_pattern.empty();

  if (capture_stdout) {
    entry->proc->set_process_mode(Process::kSeparateMode);
  } else if (req.output == OutputMode::kScreen) {
    entry->proc->set_process_mode(Process::kForwardedMode);
  } else {
    entry->proc->set_process_mode(Process::kSeparateMode);
  }

  entry->proc->register_before_exec_callback([state](int64_t child_pid) {
    state->pid.store(child_pid, std::memory_order_release);
    return true;
  });

  std::weak_ptr<SharedCallbacks> weak_cbs = shared_cbs_;

  if (capture_stdout || req.output == OutputMode::kNone) {
    entry->proc->register_ready_read_stdout_callback([weak_cbs, state, proc_raw]() {
      std::string chunk;

      while (proc_raw->read_line_stdout(chunk)) {
        if (state->output == OutputMode::kScreen) {
          (void)std::fwrite(chunk.data(), 1U, chunk.size(), stdout);
          (void)std::fflush(stdout);
        }

        if (!state->ready_pattern.empty() && !state->ready_fired.load(std::memory_order_acquire)) {
          state->pending_stdout.append(chunk);

          if (state->pending_stdout.size() > kMaxPendingStdoutBytes) {
            state->pending_stdout.erase(0U, state->pending_stdout.size() - kMaxPendingStdoutBytes);
          }

          size_t nl = state->pending_stdout.find('\n');

          while (nl != std::string::npos) {
            const std::string_view full_line(state->pending_stdout.data(), nl + 1U);

            if (full_line.find(state->ready_pattern) != std::string_view::npos &&
                !state->ready_fired.exchange(true, std::memory_order_acq_rel)) {
              auto shared = weak_cbs.lock();

              if (shared) {
                std::shared_ptr<Callbacks> cbs;

                {
                  std::lock_guard lock(shared->mtx);
                  cbs = shared->callbacks;
                }

                if (cbs && cbs->on_stdout_ready) {
                  cbs->on_stdout_ready(state->name, state->generation);
                }
              }

              state->pending_stdout.clear();
              break;
            }

            state->pending_stdout.erase(0U, nl + 1U);
            nl = state->pending_stdout.find('\n');
          }

          if (!state->ready_fired.load(std::memory_order_acquire) &&
              state->pending_stdout.find(state->ready_pattern) != std::string::npos &&
              !state->ready_fired.exchange(true, std::memory_order_acq_rel)) {
            auto shared = weak_cbs.lock();

            if (shared) {
              std::shared_ptr<Callbacks> cbs;

              {
                std::lock_guard lock(shared->mtx);
                cbs = shared->callbacks;
              }

              if (cbs && cbs->on_stdout_ready) {
                cbs->on_stdout_ready(state->name, state->generation);
              }
            }

            state->pending_stdout.clear();
          }
        }
      }
    });

    entry->proc->register_ready_read_stderr_callback([state, proc_raw]() {
      std::string line;

      while (proc_raw->read_line_stderr(line)) {
        if (state->output == OutputMode::kScreen) {
          (void)std::fwrite(line.data(), 1U, line.size(), stderr);
          (void)std::fflush(stderr);
        }
      }
    });
  }

  entry->proc->register_finished_callback([weak_cbs, state](int code, Process::ExitStatus status) {
    const bool expected = state->expected_stop.load(std::memory_order_acquire);
    const int64_t pid = state->pid.load(std::memory_order_acquire);

#if !defined(_WIN32) && !defined(_WIN64)
    if (!expected && pid > 0) {
      signal_process_group(pid, SIGKILL);
    }
#else
    (void)pid;
#endif

    auto shared = weak_cbs.lock();

    if (!shared) {
      return;
    }

    std::shared_ptr<Callbacks> cbs;

    {
      std::lock_guard lock(shared->mtx);
      cbs = shared->callbacks;
    }

    if (cbs && cbs->on_exit) {
      cbs->on_exit(state->name, state->generation, static_cast<int32_t>(code), status, expected);
    }
  });

  std::shared_ptr<Entry> replaced;

  {
    std::lock_guard lock(mtx_);
    auto it = entries_.find(req.name);

    if (it != entries_.end()) {
      replaced = std::move(it->second);
    }

    entries_[req.name] = entry;
  }

  if (replaced) {
    stop_entry(replaced, req.stop_timeout_ms);
    replaced.reset();
  }

  entry->proc->start(req.program, req.args);

  if VUNLIKELY (!entry->proc->wait_for_started(3000) || entry->proc->get_error() == Process::kStartError) {
    if (error_out != nullptr) {
      *error_out = "Failed to start process '" + req.program + "'.";
    }

    std::lock_guard lock(mtx_);
    auto it = entries_.find(req.name);

    if (it != entries_.end() && it->second == entry) {
      entries_.erase(it);
    }

    return false;
  }

  const int64_t started_pid = entry->proc->get_process_id();

  if (started_pid > 0) {
    state->pid.store(started_pid, std::memory_order_release);
  }

  if (pid_out != nullptr) {
    *pid_out = state->pid.load(std::memory_order_acquire);
  }

  return true;
}

bool ProcessSupervisor::stop_process(const std::string& name, uint32_t timeout_ms,
                                     std::optional<uint64_t> expected_gen) {
  std::shared_ptr<Entry> entry;

  {
    std::lock_guard lock(mtx_);
    auto it = entries_.find(name);

    if (it == entries_.end()) {
      return true;
    }

    if (expected_gen.has_value() && it->second->state->generation != *expected_gen) {
      return true;
    }

    entry = it->second;
    entries_.erase(it);
  }

  stop_entry(entry, timeout_ms);
  return true;
}

bool ProcessSupervisor::reload_process(const std::string& name) {
#if !defined(_WIN32) && !defined(_WIN64)
  int64_t pid = -1;

  {
    std::lock_guard lock(mtx_);
    auto it = entries_.find(name);

    if (it == entries_.end() || !it->second->proc || !it->second->proc->is_running()) {
      return false;
    }

    pid = it->second->state->pid.load(std::memory_order_acquire);
  }

  return pid > 0 && ::kill(static_cast<pid_t>(pid), SIGHUP) == 0;
#else
  (void)name;
  return false;
#endif
}

void ProcessSupervisor::reap_exited(const std::string& name, uint64_t generation) {
  std::shared_ptr<Entry> stale;

  {
    std::lock_guard lock(mtx_);
    auto it = entries_.find(name);

    if (it != entries_.end() && it->second->state->generation == generation) {
      stale = std::move(it->second);
      entries_.erase(it);
    }
  }
}

bool ProcessSupervisor::is_running(const std::string& name) const {
  std::lock_guard lock(mtx_);
  auto it = entries_.find(name);
  return it != entries_.end() && it->second->proc && it->second->proc->is_running();
}

void ProcessSupervisor::stop_all() {
  std::vector<std::shared_ptr<Entry>> snapshot;

  {
    std::lock_guard lock(mtx_);
    snapshot.reserve(entries_.size());

    for (auto& [k, v] : entries_) {
      (void)k;
      snapshot.push_back(std::move(v));
    }

    entries_.clear();
  }

  for (auto& entry : snapshot) {
    stop_entry(entry, 1500U);
  }
}

}  // namespace vlink::launcher
