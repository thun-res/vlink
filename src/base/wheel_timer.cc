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

#include "./base/wheel_timer.h"

#include <atomic>
#include <limits>
#include <list>
#include <mutex>
#include <optional>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include "./base/condition_variable.h"
#include "./base/logger.h"
#include "./base/memory_pool.h"
#include "./base/memory_resource.h"

namespace vlink {

// WheelTimer::Impl
struct WheelTimer::Impl final {  // NOLINT(clang-analyzer-optin.performance.Padding)
  // Handler
  struct Handler final {
    WheelTimer::Key key{-1};
    uint32_t remaining_rounds{0};
    WheelTimer::Callback callback;
    uint32_t repeat_interval_ms{0};

    Handler(WheelTimer::Key _key, uint32_t _rounds, WheelTimer::Callback&& _callback, uint32_t _repeat_ms = 0)
        : key(_key), remaining_rounds(_rounds), callback(std::move(_callback)), repeat_interval_ms(_repeat_ms) {}

    Handler(const Handler&) = default;

    Handler(Handler&&) = default;

    Handler& operator=(const Handler&) = default;

    Handler& operator=(Handler&&) = default;
  };

  alignas(64) std::atomic_bool stop_flag{false};
  alignas(64) std::atomic_bool paused_flag{false};
  alignas(64) std::atomic_bool is_running{false};

  std::atomic<uint32_t> catchup_limit{0};
  std::atomic<WheelTimer::Key> next_key{1};

  uint32_t slots{0};
  uint32_t interval_ms{5};

#ifdef VLINK_ENABLE_BASE_MEMORY_RESOURCE
  std::optional<std::pmr::vector<std::pmr::list<Handler>>> wheels;
#else
  std::optional<std::vector<std::list<Handler>>> wheels;
#endif

  uint32_t current_slot{0};

  std::thread worker_thread;

  std::mutex mtx;
  std::mutex lifecycle_mtx;
  ConditionVariable cv;

  std::unordered_map<WheelTimer::Key, std::pair<uint32_t, std::list<Handler>::iterator>> timer_index;

  void run();
};

// WheelTimer
WheelTimer::WheelTimer(uint32_t slots, uint32_t interval_ms) : impl_(MemoryResource::make_shared<Impl>()) {
  if VUNLIKELY (slots == 0 || interval_ms == 0) {
    VLOG_F("WheelTimer: Slots and interval_ms must be greater than 0.");
  }

  MemoryPool::global_instance();

#ifdef VLINK_ENABLE_BASE_MEMORY_RESOURCE
  impl_->wheels.emplace(&MemoryResource::global_instance());
#else
  impl_->wheels.emplace();
#endif

  impl_->slots = (slots == 0) ? 1U : slots;
  impl_->interval_ms = (interval_ms == 0) ? 1U : interval_ms;
  impl_->wheels->resize(impl_->slots);
}

WheelTimer::~WheelTimer() {
  stop();
  impl_->wheels.reset();
}

void WheelTimer::start() {
  std::lock_guard lifecycle_lock(impl_->lifecycle_mtx);

  {
    std::lock_guard lock(impl_->mtx);

    if VUNLIKELY (impl_->is_running) {
      VLOG_W("WheelTimer: Timer is already running.");
      return;
    }
  }

  if (impl_->worker_thread.joinable()) {
    impl_->worker_thread.join();
  }

  {
    std::lock_guard lock(impl_->mtx);
    impl_->stop_flag = false;
    impl_->is_running = true;
  }

  try {
    auto impl_copy = impl_;
    impl_->worker_thread = std::thread([impl_copy]() { impl_copy->run(); });
  } catch (std::exception&) {
    {
      std::lock_guard lock(impl_->mtx);
      impl_->is_running = false;
    }

    throw;
  }
}

void WheelTimer::stop() {
  {
    std::lock_guard lock(impl_->mtx);
    impl_->stop_flag = true;
  }

  wakeup();

  const bool called_from_worker =
      impl_->worker_thread.joinable() && impl_->worker_thread.get_id() == std::this_thread::get_id();

  if (called_from_worker) {
    if (impl_->lifecycle_mtx.try_lock()) {
      if (impl_->worker_thread.joinable()) {
        impl_->worker_thread.detach();
      }

      impl_->lifecycle_mtx.unlock();
    }

    return;
  }

  std::lock_guard lifecycle_lock(impl_->lifecycle_mtx);

  if (impl_->worker_thread.joinable()) {
    impl_->worker_thread.join();
  } else {
    std::unique_lock lock(impl_->mtx);
    impl_->cv.wait(lock, [this]() { return !impl_->is_running.load(std::memory_order_acquire); });
  }
}

void WheelTimer::pause() {
  std::lock_guard lock(impl_->mtx);
  impl_->paused_flag = true;
}

void WheelTimer::resume() {
  std::unique_lock lock(impl_->mtx);
  impl_->paused_flag = false;

  lock.unlock();

  wakeup();
}

void WheelTimer::wakeup() { impl_->cv.notify_one(); }

bool WheelTimer::is_running() const { return impl_->is_running; }

WheelTimer::Key WheelTimer::add(uint32_t timeout_ms, Callback&& callback, uint32_t repeat_ms) {
  if VUNLIKELY (timeout_ms == 0) {
    VLOG_E("WheelTimer: Timeout must be greater than 0.");
    return -1;
  }

  if VUNLIKELY (!callback) {
    VLOG_E("WheelTimer: Callback must be non-empty.");
    return -1;
  }

  std::lock_guard lock(impl_->mtx);

  uint32_t interval = impl_->interval_ms;
  uint32_t slots = impl_->slots;
  uint32_t current_slot = impl_->current_slot;

  uint64_t ticks = (static_cast<uint64_t>(timeout_ms) + interval - 1) / interval;

  if VUNLIKELY (ticks == 0) {
    ticks = 1;
  }

  uint64_t max_rounds = std::numeric_limits<uint32_t>::max();
  uint64_t rounds64 = ticks / slots;

  if VUNLIKELY (rounds64 > max_rounds) {
    VLOG_E("WheelTimer: Timeout too large (rounds overflow).");
    return -1;
  }

  auto ticks_mod = static_cast<uint32_t>(ticks % slots);
  auto rounds = static_cast<uint32_t>(rounds64);
  uint32_t slot = (current_slot + ticks_mod) % slots;

  WheelTimer::Key key = impl_->next_key++;

  if VUNLIKELY (key <= 0) {
    impl_->next_key = 1;
    key = impl_->next_key++;
  }

  int probe = 0;

  while (impl_->timer_index.find(key) != impl_->timer_index.end() && probe < 8) {
    key = impl_->next_key++;

    if (key <= 0) {
      impl_->next_key = 1;
      key = impl_->next_key++;
    }

    ++probe;
  }

  if VUNLIKELY (impl_->timer_index.find(key) != impl_->timer_index.end()) {
    VLOG_E("WheelTimer: Failed to allocate a unique key.");
    return -1;
  }

  auto& slot_list = (*impl_->wheels)[slot];
  slot_list.emplace_back(key, rounds, std::move(callback), repeat_ms);

  auto it = std::prev(slot_list.end());
  impl_->timer_index[key] = {slot, it};

  wakeup();

  return key;
}

bool WheelTimer::remove(WheelTimer::Key key) {
  {
    std::lock_guard lock(impl_->mtx);

    auto it = impl_->timer_index.find(key);

    if VUNLIKELY (it == impl_->timer_index.end()) {
      return false;
    }

    auto& slot_list = (*impl_->wheels)[it->second.first];
    slot_list.erase(it->second.second);
    impl_->timer_index.erase(it);
  }

  wakeup();

  return true;
}

uint32_t WheelTimer::get_remaining_time(Key key) const {
  std::lock_guard lock(impl_->mtx);

  auto it = impl_->timer_index.find(key);

  if (it == impl_->timer_index.end()) {
    return 0;
  }

  uint32_t slot = it->second.first;
  uint32_t current_slot = impl_->current_slot;
  uint32_t delta_slot = (slot + impl_->slots - current_slot) % impl_->slots;
  uint32_t rounds = it->second.second->remaining_rounds;

  uint64_t total_ticks = static_cast<uint64_t>(rounds) * impl_->slots + delta_slot;
  uint64_t total_ms = total_ticks * impl_->interval_ms;

  return (total_ms > std::numeric_limits<uint32_t>::max()) ? std::numeric_limits<uint32_t>::max()
                                                           : static_cast<uint32_t>(total_ms);
}

void WheelTimer::set_catchup_limit(uint32_t max_slots_to_catch_up) { impl_->catchup_limit = max_slots_to_catch_up; }

void WheelTimer::Impl::run() {
#ifdef VLINK_ENABLE_BASE_MEMORY_RESOURCE
  std::pmr::vector<std::pair<WheelTimer::Key, WheelTimer::Callback>> callbacks_to_execute(
      &MemoryResource::global_instance());
#else
  std::vector<std::pair<WheelTimer::Key, WheelTimer::Callback>> callbacks_to_execute;
#endif

  auto interval = std::chrono::milliseconds(interval_ms);

  auto next_tick = std::chrono::steady_clock::now();

  for (;;) {
    std::unique_lock lock(mtx);

    if VUNLIKELY (stop_flag) {
      break;
    }

    while (paused_flag && !stop_flag) {
      cv.wait(lock);
    }

    if VUNLIKELY (stop_flag) {
      break;
    }

    auto now = std::chrono::steady_clock::now();

    if (now < next_tick) {
      cv.wait_until(lock, next_tick, [this]() -> bool { return stop_flag || paused_flag; });

      if VUNLIKELY (stop_flag) {
        break;
      }

      now = std::chrono::steady_clock::now();
    }

    constexpr int64_t kStaleTickResetIntervals = 10;

    if VUNLIKELY (next_tick + interval * kStaleTickResetIntervals < now) {
      next_tick = now + interval;
    }

    uint32_t advanced = 0;
    uint32_t catchup_limit_snapshot = catchup_limit.load();

    while (now >= next_tick && !stop_flag && !paused_flag) {
      auto& timers = (*wheels)[current_slot];

      for (auto it = timers.begin(); it != timers.end();) {
        if VLIKELY (it->remaining_rounds > 0) {
          --(it->remaining_rounds);
          ++it;
        } else {
          if (it->repeat_interval_ms > 0) {
            callbacks_to_execute.emplace_back(it->key, it->callback);

            uint64_t repeat_ticks = (static_cast<uint64_t>(it->repeat_interval_ms) + interval_ms - 1) / interval_ms;

            if VUNLIKELY (repeat_ticks == 0) {
              repeat_ticks = 1;
            }

            uint64_t rounds64 = repeat_ticks / slots;

            if VUNLIKELY (rounds64 > std::numeric_limits<uint32_t>::max()) {
              VLOG_E("WheelTimer: Repeat interval too large.");

              timer_index.erase(it->key);
              it = timers.erase(it);

              continue;
            }

            auto repeat_ticks_mod = static_cast<uint32_t>(repeat_ticks % slots);
            auto new_rounds = static_cast<uint32_t>(rounds64);
            auto new_slot = (current_slot + repeat_ticks_mod) % slots;

            Handler new_handler(it->key, new_rounds, std::move(it->callback), it->repeat_interval_ms);
            auto& new_list = (*wheels)[new_slot];

            new_list.emplace_back(std::move(new_handler));

            auto new_it = std::prev(new_list.end());

            timer_index[it->key] = {new_slot, new_it};

            it = timers.erase(it);
          } else {
            callbacks_to_execute.emplace_back(it->key, std::move(it->callback));
            timer_index.erase(it->key);
            it = timers.erase(it);
          }
        }
      }

      current_slot = (current_slot + 1) % slots;

      next_tick += interval;

      if (catchup_limit_snapshot > 0) {
        if (++advanced >= catchup_limit_snapshot) {
          break;
        }
      }
    }

    now = std::chrono::steady_clock::now();

    if VUNLIKELY (next_tick + interval * kStaleTickResetIntervals < now) {
      next_tick = now + interval;
    }

#ifdef VLINK_ENABLE_BASE_MEMORY_RESOURCE
    decltype(callbacks_to_execute) pending_callbacks(&MemoryResource::global_instance());
#else
    decltype(callbacks_to_execute) pending_callbacks;
#endif

    pending_callbacks.swap(callbacks_to_execute);

    lock.unlock();

    for (const auto& [key, callback] : pending_callbacks) {
      callback(key);
    }
  }

  {
    std::lock_guard lock(mtx);
    is_running = false;
    paused_flag = false;
    current_slot = 0;

#ifdef VLINK_ENABLE_BASE_MEMORY_RESOURCE
    wheels.emplace(&MemoryResource::global_instance());
#else
    wheels.emplace();
#endif

    wheels->resize(slots);

    timer_index.clear();
  }

  cv.notify_all();
}

}  // namespace vlink
