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

#include "./base/schedule.h"

#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <utility>
#include <vector>

#include "./base/logger.h"
#include "./base/memory_resource.h"

namespace vlink {

// Schedule::Config
Schedule::Config::Config() = default;

Schedule::Config::Config(uint32_t _delay_ms, uint16_t _priority, uint32_t _schedule_timeout_ms,
                         uint32_t _execution_timeout_ms)
    : delay_ms(_delay_ms),
      priority(_priority),
      schedule_timeout_ms(_schedule_timeout_ms),
      execution_timeout_ms(_execution_timeout_ms) {}

// Schedule::Status
Schedule::Status::Status() : impl_(MemoryResource::make_shared<Schedule::Status::Impl>()) { impl_->is_valid = true; }

Schedule::Status::~Status() = default;

Schedule::Status::Status(Status&& status) noexcept {
  if VUNLIKELY (this == &status) {
    return;
  }

  impl_ = std::move(status.impl_);
}

Schedule::Status& Schedule::Status::operator=(Status&& status) noexcept {
  if VUNLIKELY (this == &status) {
    return *this;
  }

  impl_ = std::move(status.impl_);

  return *this;
}

void Schedule::Status::set_valid(bool valid) {
  if VLIKELY (impl_) {
    impl_->is_valid = valid;
  }
}

bool Schedule::Status::is_valid() const { return impl_ && impl_->is_valid; }

Schedule::Status& Schedule::Status::on_execution_timeout(Callback&& callback) {
  if VLIKELY (is_valid()) {
    std::lock_guard lock(impl_->mtx);

    if VUNLIKELY (impl_->dispatched.load(std::memory_order_acquire)) {
      VLOG_E("Schedule: on_execution_timeout registered after dispatch; ignored.");
      return *this;
    }

    if VUNLIKELY (impl_->execution_timeout_callback) {
      VLOG_E("Schedule: Execution timeout callback is already set.");
      return *this;
    }

    impl_->execution_timeout_callback = std::move(callback);
  }

  return *this;
}

Schedule::Status& Schedule::Status::on_schedule_timeout(Callback&& callback) {
  if VLIKELY (is_valid()) {
    std::lock_guard lock(impl_->mtx);

    if VUNLIKELY (impl_->dispatched.load(std::memory_order_acquire)) {
      VLOG_E("Schedule: on_schedule_timeout registered after dispatch; ignored.");
      return *this;
    }

    if VUNLIKELY (impl_->schedule_timeout_callback) {
      VLOG_E("Schedule: Schedule timeout callback is already set.");
      return *this;
    }

    impl_->schedule_timeout_callback = std::move(callback);
  }

  return *this;
}

Schedule::Status& Schedule::Status::on_catch(CatchCallback&& callback) {
  if VLIKELY (is_valid()) {
    std::lock_guard lock(impl_->mtx);

    if VUNLIKELY (impl_->dispatched.load(std::memory_order_acquire)) {
      VLOG_E("Schedule: on_catch registered after dispatch; ignored.");
      return *this;
    }

    if VUNLIKELY (impl_->catch_callback) {
      VLOG_E("Schedule: Catch callback is already set.");
      return *this;
    }

    impl_->catch_callback = std::move(callback);
  }

  return *this;
}

// Schedule::RetStatus
Schedule::Status& Schedule::RetStatus::on_else(Callback&& callback) {
  if VLIKELY (is_valid()) {
    std::lock_guard lock(impl_->mtx);

    if VUNLIKELY (impl_->dispatched.load(std::memory_order_acquire)) {
      VLOG_E("Schedule: on_else registered after dispatch; ignored.");
      return *this;
    }

    if VUNLIKELY (impl_->else_callback) {
      VLOG_E("Schedule: Else callback is already set.");
      return *this;
    }

    impl_->else_callback = std::move(callback);
  }

  return *this;
}

Schedule::RetStatus& Schedule::RetStatus::on_then(RetCallback&& callback) {
  if VLIKELY (is_valid()) {
    std::lock_guard lock(impl_->mtx);

    if VUNLIKELY (impl_->dispatched.load(std::memory_order_acquire)) {
      VLOG_E("Schedule: on_then registered after dispatch; ignored.");
      return *this;
    }

    impl_->then_callback_list.emplace_back(std::move(callback));
  }

  return *this;
}

// Schedule
Schedule::Status Schedule::process(const Config& config, Callback&& callback, Callback& wrapper_callback) {
  RetCallback adapted_callback = [callback = std::move(callback)]() mutable -> bool {
    if VLIKELY (callback) {
      callback();
    }

    return true;
  };

  return internal_process_with_ret(config, std::move(adapted_callback), wrapper_callback);
}

Schedule::RetStatus Schedule::process_with_ret(const Config& config, RetCallback&& callback,
                                               Callback& wrapper_callback) {
  return internal_process_with_ret(config, std::move(callback), wrapper_callback);
}

Schedule::RetStatus Schedule::internal_process_with_ret(const Config& config, RetCallback&& callback,
                                                        Callback& wrapper_callback) {
  Schedule::RetStatus status;

  auto submit_time = std::chrono::steady_clock::now();

  wrapper_callback = [callback = std::move(callback), config, submit_time, impl = status.impl_]() mutable {
    Schedule::Callback schedule_timeout_cb;
    Schedule::Callback execution_timeout_cb;
    Schedule::CatchCallback catch_cb;
    Schedule::Callback else_cb;
    std::vector<Schedule::RetCallback> then_callbacks;

    {
      std::lock_guard lock(impl->mtx);

      if VUNLIKELY (!impl->is_valid) {
        return;
      }

      schedule_timeout_cb = std::move(impl->schedule_timeout_callback);
      execution_timeout_cb = std::move(impl->execution_timeout_callback);
      catch_cb = std::move(impl->catch_callback);
      else_cb = std::move(impl->else_callback);
      then_callbacks = std::move(impl->then_callback_list);

      impl->dispatched.store(true, std::memory_order_release);
    }

    auto now = std::chrono::steady_clock::now();

    if (config.schedule_timeout_ms > 0) {
      auto wait_ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - submit_time).count();

      if (static_cast<uint32_t>(wait_ms) > config.schedule_timeout_ms + config.delay_ms) {
        if (schedule_timeout_cb) {
          schedule_timeout_cb();
        }

        return;
      }
    }

    auto run_with_timeout = [&catch_cb, &config,
                             &execution_timeout_cb](Schedule::RetCallback& exe_callback) -> std::optional<bool> {
      if VUNLIKELY (!exe_callback) {
        return std::nullopt;
      }

      auto exec_start = std::chrono::steady_clock::now();

      bool result = false;

      try {
        result = exe_callback();
      } catch (std::exception& e) {
        if (catch_cb) {
          catch_cb(e);

          return std::nullopt;
        }
      }

      auto exec_end = std::chrono::steady_clock::now();

      if (config.execution_timeout_ms > 0) {
        auto exec_ms = std::chrono::duration_cast<std::chrono::milliseconds>(exec_end - exec_start).count();

        if (static_cast<uint32_t>(exec_ms) > config.execution_timeout_ms) {
          if (execution_timeout_cb) {
            execution_timeout_cb();
          }

          return std::nullopt;
        }
      }

      return result;
    };

    auto main_ret = run_with_timeout(callback);

    if (!main_ret.has_value()) {
      return;
    }

    if (!main_ret.value()) {
      if (else_cb) {
        else_cb();
      }

      return;
    }

    for (auto& then_callback : then_callbacks) {
      auto then_ret = run_with_timeout(then_callback);

      if (!then_ret.has_value()) {
        return;
      }

      if (!then_ret.value()) {
        if (else_cb) {
          else_cb();
        }

        return;
      }
    }
  };

  return status;
}

}  // namespace vlink
