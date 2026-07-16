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

// BagProcessor::Impl
struct BagProcessor::Impl final {
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
  Request request{Request::kNone};
  bool last_resolved_data_timestamp_valid{false};
  bool timestamp_anchor_valid{false};
  bool output_timestamp_valid{false};
};

// BagProcessor
BagProcessor::BagProcessor(const Config& config) : impl_(std::make_unique<Impl>()) { impl_->config = config; }

BagProcessor::~BagProcessor() {
  {
    std::lock_guard lock(impl_->mtx);

    impl_->quit_flag.store(true, std::memory_order_release);
  }

  impl_->cv.notify_all();

  if VLIKELY (impl_->thread.joinable()) {
    impl_->thread.join();
  }
}

void BagProcessor::register_output_callback(OutputCallback&& output_callback) {
  std::lock_guard lock(impl_->mtx);

  if VUNLIKELY (impl_->output_callback) {
    VLOG_W("BagProcessor output callback has already been registered.");
    return;
  }

  if VUNLIKELY (!output_callback) {
    VLOG_F("BagProcessor output callback is empty.");
  }

  impl_->output_callback = std::move(output_callback);
  impl_->thread = std::thread(&BagProcessor::on_run, this);
}

void BagProcessor::push(int64_t data_timestamp, const Frame& frame) {
  std::unique_lock lock(impl_->mtx);

  if VUNLIKELY (!impl_->output_callback) {
    VLOG_F("BagProcessor output callback has not been registered.");
  }

  if VUNLIKELY (impl_->current_size >= impl_->config.max_cache_size) {
    VLOG_W("BagProcessor: Cache size is full, waiting to consume.");  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
  }

  impl_->cv.wait(lock, [this]() -> bool {
    if (impl_->data_queue.empty()) {
      return true;
    }

    return impl_->current_size < impl_->config.max_cache_size || impl_->quit_flag.load(std::memory_order_acquire);
  });

  if VUNLIKELY (impl_->quit_flag.load(std::memory_order_acquire)) {
    return;  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
  }

  bool data_timestamp_valid = true;

  if (data_timestamp < 0) {
    if (!impl_->last_resolved_data_timestamp_valid) {
      data_timestamp = -1;
      data_timestamp_valid = false;
    } else {
      data_timestamp = impl_->last_data_timestamp + (frame.timestamp - impl_->last_timestamp);
    }
  } else if (impl_->last_resolved_data_timestamp_valid) {
    const int64_t max_jump = impl_->config.max_jump_time * 1000;
    const int64_t jump = data_timestamp - impl_->last_data_timestamp;

    if (max_jump > 0 && (jump > max_jump || jump < -max_jump)) {
      data_timestamp = impl_->last_data_timestamp + (frame.timestamp - impl_->last_timestamp);
    }
  }

  if (data_timestamp_valid) {
    impl_->last_resolved_data_timestamp_valid = true;
    impl_->last_data_timestamp = data_timestamp;
    impl_->last_timestamp = frame.timestamp;
  }

  impl_->current_size += frame.data.size();

  Impl::CacheEntry entry{data_timestamp, frame, data_timestamp_valid};

  auto iter = std::upper_bound(impl_->data_queue.begin(), impl_->data_queue.end(), entry,
                               [](const Impl::CacheEntry& candidate, const Impl::CacheEntry& queued) {
                                 if (candidate.data_timestamp_valid != queued.data_timestamp_valid) {
                                   return !candidate.data_timestamp_valid;
                                 }

                                 if (!candidate.data_timestamp_valid) {
                                   return candidate.frame.timestamp < queued.frame.timestamp;
                                 }

                                 return candidate.data_timestamp < queued.data_timestamp;
                               });
  impl_->data_queue.emplace(iter, std::move(entry));

  impl_->cv.notify_one();
}

void BagProcessor::flush() {
  std::unique_lock lock(impl_->mtx);

  if VUNLIKELY (impl_->quit_flag.load(std::memory_order_acquire) || !impl_->thread.joinable()) {
    return;
  }

  impl_->cv.wait(lock, [this]() -> bool {
    return impl_->request == Impl::Request::kNone || impl_->quit_flag.load(std::memory_order_acquire);
  });

  if VUNLIKELY (impl_->quit_flag.load(std::memory_order_acquire)) {
    return;
  }

  impl_->request = Impl::Request::kFlush;

  impl_->cv.notify_all();

  impl_->cv.wait(lock, [this]() -> bool {
    return impl_->request == Impl::Request::kNone || impl_->quit_flag.load(std::memory_order_acquire);
  });  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
}

void BagProcessor::reset() {
  std::unique_lock lock(impl_->mtx);

  if VUNLIKELY (impl_->quit_flag.load(std::memory_order_acquire) || !impl_->thread.joinable()) {
    return;
  }

  impl_->cv.wait(lock, [this]() -> bool {
    return impl_->request == Impl::Request::kNone || impl_->quit_flag.load(std::memory_order_acquire);
  });

  if VUNLIKELY (impl_->quit_flag.load(std::memory_order_acquire)) {
    return;
  }

  impl_->request = Impl::Request::kReset;

  impl_->cv.notify_all();

  impl_->cv.wait(lock, [this]() -> bool {
    return impl_->request == Impl::Request::kNone || impl_->quit_flag.load(std::memory_order_acquire);
  });  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
}

void BagProcessor::reset_timeline() {
  impl_->last_data_timestamp = 0;
  impl_->last_timestamp = 0;
  impl_->data_timestamp_anchor = 0;
  impl_->timestamp_anchor = 0;
  impl_->last_output_timestamp = 0;
  impl_->last_resolved_data_timestamp_valid = false;
  impl_->timestamp_anchor_valid = false;
  impl_->output_timestamp_valid = false;
}

bool BagProcessor::on_check() {
  if (impl_->data_queue.empty()) {
    return false;  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
  }

  if (impl_->current_size >= impl_->config.max_cache_size) {
    return true;
  }

  const int64_t min_cache_time = impl_->config.min_cache_time * 1000;
  const auto& oldest = impl_->data_queue.front();
  const auto& newest = impl_->data_queue.back();

  if (oldest.data_timestamp_valid != newest.data_timestamp_valid) {
    return true;
  }

  const int64_t oldest_timestamp = oldest.data_timestamp_valid ? oldest.data_timestamp : oldest.frame.timestamp;
  const int64_t newest_timestamp = newest.data_timestamp_valid ? newest.data_timestamp : newest.frame.timestamp;

  return newest_timestamp - oldest_timestamp >= min_cache_time;
}

void BagProcessor::on_output(std::unique_lock<std::mutex>& lock, bool at_end) {
  if (impl_->data_queue.empty()) {
    return;
  }

  do {
    const int64_t min_cache_time = impl_->config.min_cache_time * 1000;
    const bool flush_all = at_end || impl_->current_size >= impl_->config.max_cache_size;
    bool should_output = flush_all;

    if (!should_output) {
      const auto& oldest = impl_->data_queue.front();
      const auto& newest = impl_->data_queue.back();

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

    auto entry = std::move(impl_->data_queue.front());
    impl_->data_queue.pop_front();

    impl_->current_size -= entry.frame.data.size();

    Frame frame = std::move(entry.frame);
    int64_t output_timestamp = frame.timestamp;
    bool update_timestamp_anchor = false;

    if (entry.data_timestamp_valid && !impl_->timestamp_anchor_valid) {
      impl_->data_timestamp_anchor = entry.data_timestamp;
      update_timestamp_anchor = true;
    } else if (entry.data_timestamp_valid) {
      output_timestamp = impl_->timestamp_anchor + (entry.data_timestamp - impl_->data_timestamp_anchor);
    }

    if (impl_->output_timestamp_valid && output_timestamp <= impl_->last_output_timestamp) {
      output_timestamp = impl_->last_output_timestamp + 1;
    }

    if (update_timestamp_anchor) {
      impl_->timestamp_anchor_valid = true;
      impl_->timestamp_anchor = output_timestamp;
    }

    impl_->output_timestamp_valid = true;
    impl_->last_output_timestamp = output_timestamp;
    frame.timestamp = output_timestamp;

    lock.unlock();

    impl_->output_callback(frame);

    lock.lock();
  } while (at_end && !impl_->data_queue.empty());
}

void BagProcessor::on_run() {
  while (!impl_->quit_flag.load(std::memory_order_acquire)) {
    on_exec(false);
  }

  on_exec(true);
}

void BagProcessor::on_exec(bool at_end) {
  std::unique_lock lock(impl_->mtx);

  if VLIKELY (!at_end) {
    impl_->cv.wait(lock, [this]() -> bool {
      return impl_->quit_flag.load(std::memory_order_acquire) || impl_->request != Impl::Request::kNone || on_check();
    });

    if VUNLIKELY (impl_->quit_flag.load(std::memory_order_acquire)) {
      return;
    }

    if VUNLIKELY (impl_->request != Impl::Request::kNone) {
      if (impl_->request == Impl::Request::kFlush) {
        on_output(lock, true);
      } else {
        impl_->data_queue.clear();
        impl_->current_size = 0;
      }

      reset_timeline();
      impl_->request = Impl::Request::kNone;

      impl_->cv.notify_all();

      return;
    }
  }

  on_output(lock, at_end);

  if VLIKELY (!at_end) {
    impl_->cv.notify_all();
  }
}

}  // namespace vlink
