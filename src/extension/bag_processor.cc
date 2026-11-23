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

#include "./extension/bag_processor.h"

#include <algorithm>
#include <atomic>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>

#include "./base/condition_variable.h"
#include "./base/logger.h"

namespace vlink {

namespace {

struct BagProcessorState final {
  enum class Request : uint8_t {
    kNone = 0,
    kFlush = 1,
    kReset = 2,
  };

  struct CacheEntry final {
    int64_t data_timestamp{0};
    Frame frame;
    bool data_timestamp_valid{false};
  };

  BagProcessor::Config config;
  BagProcessor::OutputCallback output_callback;
  std::deque<CacheEntry> data_queue;
  std::mutex mtx;
  ConditionVariable cv;
  std::thread thread;

  int64_t current_size{0};
  int64_t last_data_timestamp{0};
  int64_t last_timestamp{0};
  int64_t data_timestamp_anchor{0};
  int64_t timestamp_anchor{0};
  int64_t last_output_timestamp{0};

  std::atomic_bool quit_flag{false};
  bool skip_flush_on_quit{false};
  Request request{Request::kNone};
  bool last_resolved_data_timestamp_valid{false};
  bool timestamp_anchor_valid{false};
  bool output_timestamp_valid{false};

  bool on_check() const {
    if (data_queue.empty()) {
      return false;  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
    }

    if (current_size >= config.max_cache_size) {
      return true;
    }

    const int64_t min_cache_time = config.min_cache_time * 1000;
    const auto& oldest = data_queue.front();
    const auto& newest = data_queue.back();

    if (oldest.data_timestamp_valid != newest.data_timestamp_valid) {
      return true;
    }

    const int64_t oldest_timestamp = oldest.data_timestamp_valid ? oldest.data_timestamp : oldest.frame.timestamp;
    const int64_t newest_timestamp = newest.data_timestamp_valid ? newest.data_timestamp : newest.frame.timestamp;

    return newest_timestamp - oldest_timestamp >= min_cache_time;
  }

  void reset_timeline() {
    last_data_timestamp = 0;
    last_timestamp = 0;
    data_timestamp_anchor = 0;
    timestamp_anchor = 0;
    last_output_timestamp = 0;
    last_resolved_data_timestamp_valid = false;
    timestamp_anchor_valid = false;
    output_timestamp_valid = false;
  }

  void on_output(std::unique_lock<std::mutex>& lock, bool at_end) {
    if (data_queue.empty()) {
      return;
    }

    do {
      if VUNLIKELY (skip_flush_on_quit) {
        data_queue.clear();
        current_size = 0;
        return;
      }

      const int64_t min_cache_time = config.min_cache_time * 1000;
      const bool flush_all = at_end || current_size >= config.max_cache_size;
      bool should_output = flush_all;

      if (!should_output) {
        const auto& oldest = data_queue.front();
        const auto& newest = data_queue.back();

        if (oldest.data_timestamp_valid != newest.data_timestamp_valid) {
          should_output = true;
        } else {
          const int64_t oldest_timestamp = oldest.data_timestamp_valid ? oldest.data_timestamp : oldest.frame.timestamp;
          const int64_t newest_timestamp = newest.data_timestamp_valid ? newest.data_timestamp : newest.frame.timestamp;
          const int64_t timestamp_span = newest_timestamp - oldest_timestamp;

          should_output = timestamp_span >= min_cache_time && oldest_timestamp <= newest_timestamp - min_cache_time;
        }

        if (!should_output) {
          return;  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
        }
      }

      auto entry = std::move(data_queue.front());
      data_queue.pop_front();

      current_size -= entry.frame.data.size();

      Frame frame = std::move(entry.frame);
      int64_t output_timestamp = frame.timestamp;
      bool update_timestamp_anchor = false;

      if (entry.data_timestamp_valid && !timestamp_anchor_valid) {
        data_timestamp_anchor = entry.data_timestamp;
        update_timestamp_anchor = true;
      } else if (entry.data_timestamp_valid) {
        output_timestamp = timestamp_anchor + (entry.data_timestamp - data_timestamp_anchor);
      }

      if (output_timestamp_valid && output_timestamp <= last_output_timestamp) {
        output_timestamp = last_output_timestamp + 1;
      }

      if (update_timestamp_anchor) {
        timestamp_anchor_valid = true;
        timestamp_anchor = output_timestamp;
      }

      output_timestamp_valid = true;
      last_output_timestamp = output_timestamp;
      frame.timestamp = output_timestamp;

      lock.unlock();

      output_callback(frame);

      lock.lock();
    } while (at_end && !data_queue.empty() && !skip_flush_on_quit);
  }

  void on_exec(bool at_end) {
    std::unique_lock lock(mtx);

    if VLIKELY (!at_end) {
      cv.wait(lock, [this]() -> bool {
        return quit_flag.load(std::memory_order_acquire) || request != Request::kNone || on_check();
      });

      if VUNLIKELY (quit_flag.load(std::memory_order_acquire)) {
        return;
      }

      if VUNLIKELY (request != Request::kNone) {
        if (request == Request::kFlush) {
          on_output(lock, true);
        } else {
          data_queue.clear();
          current_size = 0;
        }

        reset_timeline();
        request = Request::kNone;

        cv.notify_all();

        return;
      }
    } else if VUNLIKELY (skip_flush_on_quit) {
      data_queue.clear();
      current_size = 0;
      return;
    }

    on_output(lock, at_end);

    if VLIKELY (!at_end) {
      cv.notify_all();
    }
  }

  static void run(std::shared_ptr<BagProcessorState> state) {
    while (!state->quit_flag.load(std::memory_order_acquire)) {
      state->on_exec(false);
    }

    state->on_exec(true);
  }
};

}  // namespace

// BagProcessor::Impl
struct BagProcessor::Impl final {
  std::shared_ptr<BagProcessorState> state{std::make_shared<BagProcessorState>()};
};

// BagProcessor
BagProcessor::BagProcessor(const Config& config) : impl_(std::make_unique<Impl>()) { impl_->state->config = config; }

BagProcessor::~BagProcessor() {
  auto state = impl_->state;
  bool self_thread = false;

  {
    std::lock_guard lock(state->mtx);

    self_thread = state->thread.joinable() && state->thread.get_id() == std::this_thread::get_id();
    state->quit_flag.store(true, std::memory_order_release);

    if VUNLIKELY (self_thread) {
      state->skip_flush_on_quit = true;
    }
  }

  state->cv.notify_all();

  if VLIKELY (state->thread.joinable()) {
    if VUNLIKELY (self_thread) {
      state->thread.detach();
    } else {
      state->thread.join();
    }
  }
}

void BagProcessor::register_output_callback(OutputCallback&& output_callback) {
  auto& state = *impl_->state;
  std::lock_guard lock(state.mtx);

  if VUNLIKELY (state.output_callback) {
    VLOG_W("BagProcessor output callback has already been registered.");
    return;
  }

  if VUNLIKELY (!output_callback) {
    VLOG_F("BagProcessor output callback is empty.");
  }

  state.output_callback = std::move(output_callback);
  state.thread = std::thread(&BagProcessorState::run, impl_->state);
}

void BagProcessor::push(int64_t data_timestamp, const Frame& frame) {
  auto& state = *impl_->state;
  std::unique_lock lock(state.mtx);

  if VUNLIKELY (!state.output_callback) {
    VLOG_F("BagProcessor output callback has not been registered.");
  }

  if VUNLIKELY (state.current_size >= state.config.max_cache_size) {
    VLOG_W("BagProcessor: Cache size is full, waiting to consume.");  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
  }

  state.cv.wait(lock, [&state]() -> bool {
    if (state.data_queue.empty()) {
      return true;
    }

    return state.current_size < state.config.max_cache_size || state.quit_flag.load(std::memory_order_acquire);
  });

  if VUNLIKELY (state.quit_flag.load(std::memory_order_acquire)) {
    return;  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
  }

  bool data_timestamp_valid = true;

  if (data_timestamp < 0) {
    if (!state.last_resolved_data_timestamp_valid) {
      data_timestamp = -1;
      data_timestamp_valid = false;
    } else {
      data_timestamp = state.last_data_timestamp + (frame.timestamp - state.last_timestamp);
    }
  } else if (state.last_resolved_data_timestamp_valid) {
    const int64_t max_jump = state.config.max_jump_time * 1000;
    const int64_t jump = data_timestamp - state.last_data_timestamp;

    if (max_jump > 0 && (jump > max_jump || jump < -max_jump)) {
      data_timestamp = state.last_data_timestamp + (frame.timestamp - state.last_timestamp);
    }
  }

  if (data_timestamp_valid) {
    state.last_resolved_data_timestamp_valid = true;
    state.last_data_timestamp = data_timestamp;
    state.last_timestamp = frame.timestamp;
  }

  state.current_size += frame.data.size();

  BagProcessorState::CacheEntry entry{data_timestamp, frame, data_timestamp_valid};

  auto iter =
      std::upper_bound(state.data_queue.begin(), state.data_queue.end(), entry,
                       [](const BagProcessorState::CacheEntry& candidate, const BagProcessorState::CacheEntry& queued) {
                         if (candidate.data_timestamp_valid != queued.data_timestamp_valid) {
                           return !candidate.data_timestamp_valid;
                         }

                         if (!candidate.data_timestamp_valid) {
                           return candidate.frame.timestamp < queued.frame.timestamp;
                         }

                         return candidate.data_timestamp < queued.data_timestamp;
                       });
  state.data_queue.emplace(iter, std::move(entry));

  state.cv.notify_one();
}

void BagProcessor::flush() {
  auto& state = *impl_->state;
  std::unique_lock lock(state.mtx);

  if VUNLIKELY (state.quit_flag.load(std::memory_order_acquire) || !state.thread.joinable() ||
                state.thread.get_id() == std::this_thread::get_id()) {
    return;
  }

  state.cv.wait(lock, [&state]() -> bool {
    return state.request == BagProcessorState::Request::kNone || state.quit_flag.load(std::memory_order_acquire);
  });

  if VUNLIKELY (state.quit_flag.load(std::memory_order_acquire)) {
    return;
  }

  state.request = BagProcessorState::Request::kFlush;

  state.cv.notify_all();

  state.cv.wait(lock, [&state]() -> bool {
    return state.request == BagProcessorState::Request::kNone || state.quit_flag.load(std::memory_order_acquire);
  });  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
}

void BagProcessor::reset() {
  auto& state = *impl_->state;
  std::unique_lock lock(state.mtx);

  if VUNLIKELY (state.quit_flag.load(std::memory_order_acquire) || !state.thread.joinable()) {
    return;
  }

  state.cv.wait(lock, [&state]() -> bool {
    return state.request == BagProcessorState::Request::kNone || state.quit_flag.load(std::memory_order_acquire);
  });

  if VUNLIKELY (state.quit_flag.load(std::memory_order_acquire)) {
    return;
  }

  state.request = BagProcessorState::Request::kReset;

  state.cv.notify_all();

  state.cv.wait(lock, [&state]() -> bool {
    return state.request == BagProcessorState::Request::kNone || state.quit_flag.load(std::memory_order_acquire);
  });  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
}

bool BagProcessor::on_check() { return impl_->state->on_check(); }

void BagProcessor::on_output(std::unique_lock<std::mutex>& lock, bool at_end) { impl_->state->on_output(lock, at_end); }

void BagProcessor::on_run() { BagProcessorState::run(impl_->state); }

void BagProcessor::on_exec(bool at_end) { impl_->state->on_exec(at_end); }

void BagProcessor::reset_timeline() { impl_->state->reset_timeline(); }

}  // namespace vlink
