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
#include <chrono>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>

#include "./base/condition_variable.h"
#include "./base/elapsed_timer.h"
#include "./base/logger.h"

namespace vlink {

// BagProcessor::Impl
struct BagProcessor::Impl final {
  struct CacheEntry final {
    int64_t data_timestamp{0};
    Frame frame;
    int64_t enqueue_time{0};
  };

  std::atomic_bool quit_flag{false};
  bool flush_request{false};

  BagProcessor::Config config;
  std::shared_ptr<BagProcessor::OutputCallback> output_callback;

  std::deque<CacheEntry> data_queue;

  int64_t current_size{0};

  std::mutex mtx;
  ConditionVariable cv;
  std::thread thread;
};

// BagProcessor
BagProcessor::BagProcessor(const Config& config) : impl_(std::make_unique<Impl>()) {
  impl_->config = config;

  impl_->thread = std::thread(&BagProcessor::on_run, this);
}

BagProcessor::~BagProcessor() {
  impl_->quit_flag = true;

  impl_->cv.notify_all();

  if VLIKELY (impl_->thread.joinable()) {
    impl_->thread.join();
  }
}

void BagProcessor::register_output_callback(OutputCallback&& output_callback) {
  std::lock_guard lock(impl_->mtx);
  impl_->output_callback = std::make_shared<OutputCallback>(std::move(output_callback));
}

void BagProcessor::push(int64_t data_timestamp, const Frame& frame) {
  std::unique_lock lock(impl_->mtx);

  if VUNLIKELY (impl_->current_size >= impl_->config.max_cache_size) {
    VLOG_W("BagProcessor: Cache size is full, waiting to consume.");
  }

  impl_->cv.wait(lock, [this]() -> bool {
    if (impl_->data_queue.empty()) {
      return true;
    }

    return impl_->current_size < impl_->config.max_cache_size || impl_->quit_flag.load();
  });

  if VUNLIKELY (impl_->quit_flag) {
    return;
  }

  const int64_t enqueue_time = ElapsedTimer::get_sys_timestamp(ElapsedTimer::kMicro);

  impl_->current_size += frame.data.size();

  Impl::CacheEntry entry{data_timestamp, frame, enqueue_time};

  auto iter = std::upper_bound(impl_->data_queue.begin(), impl_->data_queue.end(), entry.data_timestamp,
                               [](int64_t target_timestamp, const Impl::CacheEntry& target) {
                                 return target_timestamp < target.data_timestamp;
                               });
  impl_->data_queue.emplace(iter, std::move(entry));

  impl_->cv.notify_one();
}

void BagProcessor::flush() {
  std::unique_lock lock(impl_->mtx);

  if VUNLIKELY (impl_->quit_flag) {
    return;
  }

  impl_->flush_request = true;

  impl_->cv.notify_all();

  impl_->cv.wait(lock, [this]() -> bool { return !impl_->flush_request || impl_->quit_flag.load(); });
}

bool BagProcessor::on_check() {
  if (impl_->data_queue.empty()) {
    return false;
  }

  if (impl_->current_size >= impl_->config.max_cache_size) {
    return true;
  }

  const int64_t min_cache_time = impl_->config.min_cache_time * 1000;

  if (impl_->data_queue.back().data_timestamp - impl_->data_queue.front().data_timestamp >= min_cache_time) {
    return true;
  }

  const int64_t cache_elapsed = static_cast<int64_t>(ElapsedTimer::get_sys_timestamp(ElapsedTimer::kMicro)) -
                                impl_->data_queue.front().enqueue_time;

  return cache_elapsed >= min_cache_time;
}

void BagProcessor::on_output(std::unique_lock<std::mutex>& lock, bool at_end) {
  if VUNLIKELY (!impl_->output_callback || !*impl_->output_callback) {
    impl_->data_queue.clear();
    impl_->current_size = 0;
    return;
  }

  if (impl_->data_queue.empty()) {
    return;
  }

  do {
    const int64_t min_cache_time = impl_->config.min_cache_time * 1000;
    const bool flush_all = at_end || impl_->current_size >= impl_->config.max_cache_size;
    bool should_output = flush_all;

    if (!should_output) {
      const int64_t timestamp_span = impl_->data_queue.back().data_timestamp - impl_->data_queue.front().data_timestamp;

      if (timestamp_span >= min_cache_time) {
        should_output =
            impl_->data_queue.front().data_timestamp <= impl_->data_queue.back().data_timestamp - min_cache_time;
      } else {
        const int64_t cache_elapsed = static_cast<int64_t>(ElapsedTimer::get_sys_timestamp(ElapsedTimer::kMicro)) -
                                      impl_->data_queue.front().enqueue_time;

        should_output = cache_elapsed >= min_cache_time;
      }

      if (!should_output) {
        return;
      }
    }

    auto output_callback = impl_->output_callback;
    auto entry = std::move(impl_->data_queue.front());
    impl_->data_queue.pop_front();

    impl_->current_size -= entry.frame.data.size();

    lock.unlock();

    (*output_callback)(entry.frame);

    lock.lock();
  } while (at_end && !impl_->data_queue.empty());
}

void BagProcessor::on_run() {
  while (!impl_->quit_flag) {
    on_exec(false);
  }

  on_exec(true);
}

void BagProcessor::on_exec(bool at_end) {
  std::unique_lock lock(impl_->mtx);

  if VLIKELY (!at_end) {
    impl_->cv.wait(lock,
                   [this]() -> bool { return !impl_->data_queue.empty() || impl_->quit_flag || impl_->flush_request; });

    if VUNLIKELY (impl_->quit_flag) {
      return;
    }

    if VUNLIKELY (impl_->flush_request) {
      on_output(lock, true);

      impl_->flush_request = false;

      impl_->cv.notify_all();

      return;
    }
  }

  if VLIKELY (!at_end) {
    if (!on_check()) {
      const int64_t min_cache_time = impl_->config.min_cache_time * 1000;
      const int64_t cache_elapsed = static_cast<int64_t>(ElapsedTimer::get_sys_timestamp(ElapsedTimer::kMicro)) -
                                    impl_->data_queue.front().enqueue_time;
      const int64_t wait_time = min_cache_time - cache_elapsed;

      if (wait_time > 0) {
        impl_->cv.wait_for(lock, std::chrono::microseconds(wait_time),
                           [this]() -> bool { return impl_->quit_flag.load() || impl_->flush_request || on_check(); });
      }

      if VUNLIKELY (impl_->quit_flag) {
        return;
      }

      if VUNLIKELY (impl_->flush_request) {
        on_output(lock, true);

        impl_->flush_request = false;

        impl_->cv.notify_all();

        return;
      }

      if (!on_check()) {
        return;
      }
    }
  }

  on_output(lock, at_end);

  if VLIKELY (!at_end) {
    impl_->cv.notify_all();
  }
}

}  // namespace vlink
