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

#include "./base/task_handle.h"

#include <chrono>
#include <memory>
#include <mutex>
#include <utility>

#include "./base/condition_variable.h"
#include "./base/memory_resource.h"

namespace vlink {

[[maybe_unused]] static bool is_terminal(TaskExecutionState state) noexcept {
  return state == TaskExecutionState::kCompleted || state == TaskExecutionState::kCancelled ||
         state == TaskExecutionState::kDropped || state == TaskExecutionState::kRejected ||
         state == TaskExecutionState::kFailed;
}

template <typename StateT>
[[maybe_unused]] static void release_parent_registration(StateT& state,
                                                         CancellationRegistration& registration) noexcept {
  registration = std::move(state.parent_registration);
}

// TaskHandle::State
struct TaskHandle::State final {
  mutable std::mutex mtx;
  ConditionVariable cv;
  TaskExecutionState state{TaskExecutionState::kInvalid};
  CancellationSource cancellation_source;
  CancellationRegistration parent_registration;
};

// TrackedTask
class TrackedTask final {
 public:
  TrackedTask(TaskHandle handle, MoveFunction<void()>&& callback) noexcept
      : handle_(std::move(handle)), callback_(std::move(callback)) {}

  TrackedTask(TrackedTask&& other) noexcept
      : handle_(std::move(other.handle_)), callback_(std::move(other.callback_)), armed_(other.armed_) {
    other.armed_ = false;
  }

  TrackedTask& operator=(TrackedTask&& other) noexcept {
    if VUNLIKELY (this == &other) {
      return *this;
    }

    if VUNLIKELY (armed_) {
      TaskHandle::drop_task_if_queued(handle_);
    }

    handle_ = std::move(other.handle_);
    callback_ = std::move(other.callback_);
    armed_ = other.armed_;

    other.armed_ = false;

    return *this;
  }

  ~TrackedTask() {
    if VUNLIKELY (armed_) {
      TaskHandle::drop_task_if_queued(handle_);
    }
  }

  void operator()() {
    armed_ = false;

    if VUNLIKELY (!TaskHandle::begin_task_execution(handle_)) {
      return;
    }

    try {
      if VLIKELY (callback_) {
        callback_();
      }

      TaskHandle::complete_task_execution(handle_);
    } catch (...) {
      TaskHandle::fail_task_execution(handle_);
    }
  }

 private:
  TaskHandle handle_;
  MoveFunction<void()> callback_;
  bool armed_{true};

  VLINK_DISALLOW_COPY_AND_ASSIGN(TrackedTask)
};

// TaskHandle
TaskHandle::TaskHandle() noexcept = default;

TaskHandle::~TaskHandle() = default;

TaskHandle::TaskHandle(const TaskHandle&) noexcept = default;

TaskHandle& TaskHandle::operator=(const TaskHandle&) noexcept = default;

TaskHandle::TaskHandle(TaskHandle&&) noexcept = default;

TaskHandle& TaskHandle::operator=(TaskHandle&&) noexcept = default;

bool TaskHandle::valid() const noexcept { return state_ != nullptr; }

TaskExecutionState TaskHandle::state() const noexcept {
  if VUNLIKELY (!state_) {
    return TaskExecutionState::kInvalid;
  }

  std::lock_guard lock(state_->mtx);

  return state_->state;
}

bool TaskHandle::is_done() const noexcept { return is_terminal(state()); }

CancellationToken TaskHandle::cancellation_token() const noexcept {
  if VUNLIKELY (!state_) {
    return {};
  }

  return state_->cancellation_source.token();
}

bool TaskHandle::cancel() const { return request_cancel(state_); }

bool TaskHandle::wait(int timeout_ms) const {
  if VUNLIKELY (!state_) {
    return false;
  }

  std::unique_lock lock(state_->mtx);

  auto predicate = [this] { return is_terminal(state_->state); };

  if (timeout_ms < 0) {
    state_->cv.wait(lock, predicate);

    return true;
  }

  return state_->cv.wait_for(lock, std::chrono::milliseconds(timeout_ms), predicate);
}

TaskHandle::TaskHandle(std::shared_ptr<State> state) noexcept : state_(std::move(state)) {}

TaskHandle TaskHandle::make_task_handle(const CancellationToken& parent_token) {
  auto state = MemoryResource::make_shared<TaskHandle::State>();

  if VLIKELY (parent_token.valid()) {
    std::weak_ptr<TaskHandle::State> weak_state = state;

    auto registration = parent_token.register_callback([weak_state]() {
      auto locked = weak_state.lock();

      if VLIKELY (locked) {
        (void)TaskHandle::request_cancel(std::move(locked));
      }
    });

    {
      std::lock_guard lock(state->mtx);

      state->parent_registration = std::move(registration);
    }

    if VUNLIKELY (parent_token.is_cancellation_requested()) {
      (void)TaskHandle::request_cancel(state);
    }
  }

  return TaskHandle{std::move(state)};
}

MoveFunction<void()> TaskHandle::make_tracked_task(TaskHandle handle, MoveFunction<void()>&& callback) {
  return TrackedTask{std::move(handle), std::move(callback)};
}

void TaskHandle::mark_task_queued(const TaskHandle& handle) {
  if VUNLIKELY (!handle.state_) {
    return;
  }

  CancellationRegistration parent_registration;

  {
    std::lock_guard lock(handle.state_->mtx);

    if VUNLIKELY (handle.state_->state != TaskExecutionState::kInvalid) {
      return;
    }

    if VUNLIKELY (handle.state_->cancellation_source.is_cancellation_requested()) {
      handle.state_->state = TaskExecutionState::kCancelled;

      release_parent_registration(*handle.state_, parent_registration);
    } else {
      handle.state_->state = TaskExecutionState::kQueued;
    }
  }

  handle.state_->cv.notify_all();
}

void TaskHandle::mark_task_rejected(const TaskHandle& handle) {
  if VUNLIKELY (!handle.state_) {
    return;
  }

  CancellationRegistration parent_registration;

  {
    std::lock_guard lock(handle.state_->mtx);

    if VLIKELY (!is_terminal(handle.state_->state)) {
      handle.state_->state = TaskExecutionState::kRejected;

      release_parent_registration(*handle.state_, parent_registration);
    }
  }

  handle.state_->cv.notify_all();
}

bool TaskHandle::begin_task_execution(const TaskHandle& handle) {
  if VUNLIKELY (!handle.state_) {
    return false;
  }

  std::lock_guard lock(handle.state_->mtx);

  if VUNLIKELY (handle.state_->state != TaskExecutionState::kQueued) {
    return false;
  }

  if VUNLIKELY (handle.state_->cancellation_source.is_cancellation_requested()) {
    handle.state_->state = TaskExecutionState::kCancelled;

    handle.state_->cv.notify_all();

    return false;
  }

  handle.state_->state = TaskExecutionState::kRunning;

  handle.state_->cv.notify_all();

  return true;
}

void TaskHandle::complete_task_execution(const TaskHandle& handle) {
  if VUNLIKELY (!handle.state_) {
    return;
  }

  CancellationRegistration parent_registration;

  {
    std::lock_guard lock(handle.state_->mtx);

    if VLIKELY (handle.state_->state == TaskExecutionState::kRunning) {
      handle.state_->state = TaskExecutionState::kCompleted;

      release_parent_registration(*handle.state_, parent_registration);
    }
  }

  handle.state_->cv.notify_all();
}

void TaskHandle::fail_task_execution(const TaskHandle& handle) {
  if VUNLIKELY (!handle.state_) {
    return;
  }

  CancellationRegistration parent_registration;

  {
    std::lock_guard lock(handle.state_->mtx);

    if VLIKELY (handle.state_->state == TaskExecutionState::kRunning) {
      handle.state_->state = TaskExecutionState::kFailed;

      release_parent_registration(*handle.state_, parent_registration);
    }
  }

  handle.state_->cv.notify_all();
}

void TaskHandle::drop_task_if_queued(const TaskHandle& handle) {
  if VUNLIKELY (!handle.state_) {
    return;
  }

  CancellationRegistration parent_registration;

  {
    std::lock_guard lock(handle.state_->mtx);

    if VLIKELY (handle.state_->state == TaskExecutionState::kQueued) {
      handle.state_->state = TaskExecutionState::kDropped;

      release_parent_registration(*handle.state_, parent_registration);
    }
  }

  handle.state_->cv.notify_all();
}

bool TaskHandle::request_cancel(std::shared_ptr<State> state) {
  if VUNLIKELY (!state) {
    return false;
  }

  bool changed = false;

  CancellationRegistration parent_registration;

  {
    std::lock_guard lock(state->mtx);

    if VUNLIKELY (is_terminal(state->state)) {
      return false;
    }

    if VLIKELY (state->state == TaskExecutionState::kInvalid || state->state == TaskExecutionState::kQueued) {
      state->state = TaskExecutionState::kCancelled;

      release_parent_registration(*state, parent_registration);

      changed = true;
    }
  }

  if VLIKELY (changed) {
    state->cv.notify_all();
  }

  changed = state->cancellation_source.request_cancel() || changed;

  return changed;
}

}  // namespace vlink
