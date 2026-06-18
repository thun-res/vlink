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

// NOLINTBEGIN

#include "./base/task_handle.h"

#include <doctest/doctest.h>

#include <atomic>
#include <chrono>
#include <future>
#include <stdexcept>
#include <vector>

#include "../common_test.h"
#include "./base/message_loop.h"
#include "./base/thread_pool.h"

namespace {

class SmallQueueMessageLoop final : public MessageLoop {
 public:
  using MessageLoop::MessageLoop;

  [[nodiscard]] size_t get_max_task_count() const override { return 1U; }
};

class TwoTaskMessageLoop final : public MessageLoop {
 public:
  using MessageLoop::MessageLoop;

  [[nodiscard]] size_t get_max_task_count() const override { return 2U; }
};

class SmallQueueThreadPool final : public ThreadPool {
 public:
  using ThreadPool::ThreadPool;

  [[nodiscard]] size_t get_max_task_count() const override { return 1U; }
};

class TwoTaskThreadPool final : public ThreadPool {
 public:
  using ThreadPool::ThreadPool;

  [[nodiscard]] size_t get_max_task_count() const override { return 2U; }
};

template <typename PredicateT>
bool wait_until(PredicateT&& predicate, std::chrono::milliseconds timeout = std::chrono::milliseconds(1000)) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;

  while (std::chrono::steady_clock::now() < deadline) {
    if (predicate()) {
      return true;
    }

    std::this_thread::yield();
  }

  return predicate();
}

}  // namespace

TEST_SUITE("base-TaskHandle") {
  TEST_CASE("default handle is invalid with no-op cancel and wait") {
    TaskHandle handle;

    CHECK_FALSE(handle.valid());
    CHECK_EQ(handle.state(), TaskExecutionState::kInvalid);
    CHECK_FALSE(handle.is_done());
    CHECK_FALSE(handle.cancel());
    CHECK_FALSE(handle.wait(0));
    CHECK_FALSE(handle.wait(TaskHandle::kInfinite));
    CHECK_FALSE(handle.cancellation_token().valid());
  }

  TEST_CASE("kInfinite sentinel equals -1") { CHECK_EQ(TaskHandle::kInfinite, -1); }

  TEST_CASE("default PostTaskOptions has droppable kUseDispatcherStrategy and no token") {
    PostTaskOptions opts;

    CHECK_EQ(opts.overflow_policy, TaskOverflowPolicy::kUseDispatcherStrategy);
    CHECK_EQ(opts.drop_policy, TaskDropPolicy::kDroppable);
    CHECK_FALSE(opts.cancellation_token.valid());
  }

  TEST_CASE("copy-constructed handle shares state and reaches terminal together") {
    MessageLoop loop;
    auto original = loop.post_task_handle([] {});
    TaskHandle copy = original;

    CHECK(copy.valid());
    CHECK(original.valid());
    CHECK_EQ(copy.state(), original.state());

    loop.async_run();

    CHECK(original.wait(1000));
    CHECK(copy.is_done());
    CHECK_EQ(copy.state(), TaskExecutionState::kCompleted);
    CHECK_EQ(original.state(), copy.state());

    loop.quit();
    loop.wait_for_quit();
  }

  TEST_CASE("copy-assigned handle observes shared completion state") {
    MessageLoop loop;
    auto handle = loop.post_task_handle([] {});
    TaskHandle assigned;

    CHECK_FALSE(assigned.valid());

    assigned = handle;

    CHECK(assigned.valid());
    CHECK_EQ(assigned.state(), handle.state());

    loop.async_run();

    CHECK(handle.wait(1000));
    CHECK_EQ(assigned.state(), TaskExecutionState::kCompleted);

    loop.quit();
    loop.wait_for_quit();
  }

  TEST_CASE("move-constructed handle leaves source invalid") {
    MessageLoop loop;
    auto original = loop.post_task_handle([] {});

    CHECK(original.valid());

    TaskHandle moved(std::move(original));

    CHECK(moved.valid());
    CHECK_FALSE(original.valid());  // NOLINT(bugprone-use-after-move)
    CHECK_EQ(original.state(), TaskExecutionState::kInvalid);
    CHECK_FALSE(original.cancel());
    CHECK_FALSE(original.wait(0));

    loop.async_run();

    CHECK(moved.wait(1000));
    CHECK_EQ(moved.state(), TaskExecutionState::kCompleted);

    loop.quit();
    loop.wait_for_quit();
  }

  TEST_CASE("move-assigned handle transfers state and leaves source invalid") {
    MessageLoop loop;
    auto original = loop.post_task_handle([] {});
    TaskHandle target;

    target = std::move(original);

    CHECK(target.valid());
    CHECK_FALSE(original.valid());  // NOLINT(bugprone-use-after-move)
    CHECK_EQ(original.state(), TaskExecutionState::kInvalid);

    loop.async_run();

    CHECK(target.wait(1000));
    CHECK_EQ(target.state(), TaskExecutionState::kCompleted);

    loop.quit();
    loop.wait_for_quit();
  }

  TEST_CASE("wait on already-completed handle returns true for any timeout") {
    MessageLoop loop;
    loop.async_run();

    auto handle = loop.post_task_handle([] {});

    CHECK(handle.wait(1000));
    CHECK_EQ(handle.state(), TaskExecutionState::kCompleted);

    CHECK(handle.wait(0));
    CHECK(handle.wait(1));
    CHECK(handle.wait(TaskHandle::kInfinite));
    CHECK(handle.is_done());

    loop.quit();
    loop.wait_for_quit();
  }

  TEST_CASE("is_done reflects terminal vs non-terminal states") {
    MessageLoop loop;

    auto queued = loop.post_task_handle([] {});

    CHECK_EQ(queued.state(), TaskExecutionState::kQueued);
    CHECK_FALSE(queued.is_done());

    auto cancelled = loop.post_task_handle([] {});

    CHECK(cancelled.cancel());
    CHECK_EQ(cancelled.state(), TaskExecutionState::kCancelled);
    CHECK(cancelled.is_done());

    TaskHandle invalid;

    CHECK_EQ(invalid.state(), TaskExecutionState::kInvalid);
    CHECK_FALSE(invalid.is_done());

    loop.async_run();

    auto completed = loop.post_task_handle([] {});

    CHECK(completed.wait(1000));
    CHECK_EQ(completed.state(), TaskExecutionState::kCompleted);
    CHECK(completed.is_done());

    auto failed = loop.post_task_handle([] { throw std::runtime_error("boom"); });

    CHECK(failed.wait(1000));
    CHECK_EQ(failed.state(), TaskExecutionState::kFailed);
    CHECK(failed.is_done());

    loop.quit();
    loop.wait_for_quit();
  }

  TEST_CASE("queued handle can be cancelled before loop runs") {
    MessageLoop loop;
    std::atomic<bool> ran{false};
    auto handle = loop.post_task_handle([&ran] { ran.store(true, std::memory_order_release); });

    CHECK(handle.cancel());
    CHECK(handle.wait(100));
    CHECK_EQ(handle.state(), TaskExecutionState::kCancelled);

    loop.async_run();
    loop.wait_for_idle();

    CHECK_FALSE(ran.load(std::memory_order_acquire));

    loop.quit();
    loop.wait_for_quit();
  }

  TEST_CASE("multiple copies share cancellation visibility") {
    MessageLoop loop;
    std::atomic<bool> ran{false};
    auto first = loop.post_task_handle([&ran] { ran.store(true, std::memory_order_release); });
    TaskHandle second = first;
    TaskHandle third;
    third = first;

    CHECK(second.cancel());
    CHECK_EQ(first.state(), TaskExecutionState::kCancelled);
    CHECK_EQ(third.state(), TaskExecutionState::kCancelled);
    CHECK(first.is_done());
    CHECK(second.is_done());
    CHECK(third.is_done());

    loop.async_run();
    loop.wait_for_idle();

    CHECK_FALSE(ran.load(std::memory_order_acquire));

    loop.quit();
    loop.wait_for_quit();
  }

  TEST_CASE("tracked failure is observable and loop continues") {
    MessageLoop loop;
    loop.async_run();

    auto failed = loop.post_task_handle([] { throw std::runtime_error("tracked failure"); });

    CHECK(failed.wait(1000));
    CHECK_EQ(failed.state(), TaskExecutionState::kFailed);

    std::atomic<bool> ran{false};
    auto next = loop.post_task_handle([&ran] { ran.store(true, std::memory_order_release); });

    CHECK(next.wait(1000));
    CHECK_EQ(next.state(), TaskExecutionState::kCompleted);
    CHECK(ran.load(std::memory_order_acquire));

    loop.quit();
    loop.wait_for_quit();
  }

  TEST_CASE("kReject overflow policy rejects when queue is full") {
    SmallQueueMessageLoop loop(MessageLoop::kNormalType);

    CHECK(loop.post_task([] {}));

    PostTaskOptions options;
    options.overflow_policy = TaskOverflowPolicy::kReject;

    std::atomic<bool> ran{false};
    auto rejected = loop.post_task_handle([&ran] { ran.store(true, std::memory_order_release); }, options);

    CHECK(rejected.wait(100));
    CHECK_EQ(rejected.state(), TaskExecutionState::kRejected);
    CHECK_FALSE(ran.load(std::memory_order_acquire));
  }

  TEST_CASE("kPopStrategy drops oldest queued task and makes it observable as kDropped") {
    SmallQueueMessageLoop loop(MessageLoop::kNormalType);
    loop.set_strategy(MessageLoop::kPopStrategy);

    auto first = loop.post_task_handle([] {});
    auto second = loop.post_task_handle([] {});

    CHECK(first.wait(100));
    CHECK_EQ(first.state(), TaskExecutionState::kDropped);
    CHECK_EQ(second.state(), TaskExecutionState::kQueued);
  }

  TEST_CASE("protected task is not dropped by overflow") {
    SmallQueueMessageLoop loop(MessageLoop::kNormalType);
    loop.set_strategy(MessageLoop::kPopStrategy);

    PostTaskOptions options;
    options.drop_policy = TaskDropPolicy::kProtected;

    auto protected_task = loop.post_task_handle([] {}, options);

    CHECK_EQ(protected_task.state(), TaskExecutionState::kQueued);

    CHECK_FALSE(loop.post_task([] {}));
    CHECK_EQ(protected_task.state(), TaskExecutionState::kQueued);

    auto rejected = loop.post_task_handle([] {});

    CHECK(rejected.wait(100));
    CHECK_EQ(rejected.state(), TaskExecutionState::kRejected);
  }

  TEST_CASE("already-cancelled parent token cancels handle immediately") {
    MessageLoop loop;
    CancellationSource source;

    CHECK(source.request_cancel());

    PostTaskOptions options;
    options.cancellation_token = source.token();

    std::atomic<bool> ran{false};
    auto handle = loop.post_task_handle([&ran] { ran.store(true, std::memory_order_release); }, options);

    CHECK(handle.wait(100));
    CHECK_EQ(handle.state(), TaskExecutionState::kCancelled);
    CHECK_EQ(loop.get_task_count(), 0U);
    CHECK_FALSE(ran.load(std::memory_order_acquire));
  }

  TEST_CASE("parent token cancelled after enqueue cancels queued task") {
    MessageLoop loop;
    CancellationSource source;
    PostTaskOptions options;
    options.cancellation_token = source.token();

    std::atomic<bool> ran{false};
    auto handle = loop.post_task_handle([&ran] { ran.store(true, std::memory_order_release); }, options);

    CHECK_EQ(handle.state(), TaskExecutionState::kQueued);
    CHECK(source.request_cancel());
    CHECK(handle.wait(100));
    CHECK_EQ(handle.state(), TaskExecutionState::kCancelled);

    loop.async_run();
    loop.wait_for_idle();

    CHECK_FALSE(ran.load(std::memory_order_acquire));

    loop.quit();
    loop.wait_for_quit();
  }

  TEST_CASE("kBlock overflow policy interrupted by parent cancellation") {
    SmallQueueMessageLoop loop(MessageLoop::kNormalType);

    CHECK(loop.post_task([] {}));

    CancellationSource source;
    PostTaskOptions options;
    options.overflow_policy = TaskOverflowPolicy::kBlock;
    options.cancellation_token = source.token();

    auto fut =
        std::async(std::launch::async, [&loop, &options] { return loop.post_task_handle([] {}, options).state(); });

    CHECK_EQ(fut.wait_for(30ms), std::future_status::timeout);
    CHECK(source.request_cancel());
    CHECK_EQ(fut.wait_for(1000ms), std::future_status::ready);
    CHECK_EQ(fut.get(), TaskExecutionState::kCancelled);
  }

  TEST_CASE("priority loop dispatches by priority order") {
    MessageLoop loop(MessageLoop::kPriorityType);
    std::vector<int> order;

    PostTaskOptions protected_options;
    protected_options.drop_policy = TaskDropPolicy::kProtected;

    auto low = loop.post_task_with_priority_handle([&order] { order.push_back(1); }, MessageLoop::kLowestPriority);
    auto high = loop.post_task_with_priority_handle([&order] { order.push_back(2); }, MessageLoop::kHighestPriority,
                                                    protected_options);
    auto normal = loop.post_task_with_priority_handle([&order] { order.push_back(3); }, MessageLoop::kNormalPriority);

    loop.async_run();

    CHECK(low.wait(1000));
    CHECK(high.wait(1000));
    CHECK(normal.wait(1000));
    CHECK_EQ(order, std::vector<int>{2, 3, 1});

    loop.quit();
    loop.wait_for_quit();
  }

  TEST_CASE("post_task_with_priority_handle rejects kNoPriority on priority loop") {
    MessageLoop loop(MessageLoop::kPriorityType);
    std::atomic<bool> ran{false};
    auto handle = loop.post_task_with_priority_handle([&ran] { ran.store(true, std::memory_order_release); },
                                                      MessageLoop::kNoPriority);

    CHECK(handle.wait(100));
    CHECK_EQ(handle.state(), TaskExecutionState::kRejected);
    CHECK_FALSE(ran.load(std::memory_order_acquire));
  }

  TEST_CASE("post_task_with_priority_handle rejects on non-priority loop") {
    MessageLoop loop(MessageLoop::kNormalType);
    std::atomic<bool> ran{false};
    auto handle = loop.post_task_with_priority_handle([&ran] { ran.store(true, std::memory_order_release); },
                                                      MessageLoop::kHighestPriority);

    CHECK(handle.wait(100));
    CHECK_EQ(handle.state(), TaskExecutionState::kRejected);
    CHECK_FALSE(ran.load(std::memory_order_acquire));
  }

  TEST_CASE("ThreadPool post_task_handle completes and task executes") {
    ThreadPool pool(2);
    std::atomic<bool> ran{false};
    auto handle = pool.post_task_handle([&ran] { ran.store(true, std::memory_order_release); });

    CHECK(handle.wait(1000));
    CHECK_EQ(handle.state(), TaskExecutionState::kCompleted);
    CHECK(ran.load(std::memory_order_acquire));
    CHECK_FALSE(handle.cancel());

    pool.shutdown();
  }

  TEST_CASE("ThreadPool tracked failure is observable and worker continues") {
    ThreadPool pool(1);
    auto failed = pool.post_task_handle([] { throw std::runtime_error("tracked failure"); });

    CHECK(failed.wait(1000));
    CHECK_EQ(failed.state(), TaskExecutionState::kFailed);

    std::atomic<bool> ran{false};
    auto next = pool.post_task_handle([&ran] { ran.store(true, std::memory_order_release); });

    CHECK(next.wait(1000));
    CHECK_EQ(next.state(), TaskExecutionState::kCompleted);
    CHECK(ran.load(std::memory_order_acquire));

    pool.shutdown();
  }

  TEST_CASE("ThreadPool dropped queued task is observable as kDropped") {
    SmallQueueThreadPool pool(1);
    pool.set_strategy(ThreadPool::kPopStrategy);

    std::atomic<bool> release_worker{false};
    std::atomic<bool> worker_started{false};

    CHECK(pool.post_task([&] {
      worker_started.store(true, std::memory_order_release);

      while (!release_worker.load(std::memory_order_acquire)) {
        std::this_thread::yield();
      }
    }));

    CHECK(wait_until([&worker_started] { return worker_started.load(std::memory_order_acquire); }));

    auto first = pool.post_task_handle([] {});
    auto second = pool.post_task_handle([] {});

    CHECK(first.wait(100));
    CHECK_EQ(first.state(), TaskExecutionState::kDropped);
    CHECK_EQ(second.state(), TaskExecutionState::kQueued);

    release_worker.store(true, std::memory_order_release);
    CHECK(second.wait(1000));

    pool.shutdown();
  }

  TEST_CASE("ThreadPool running task observes cancellation token cooperatively") {
    ThreadPool pool(1);
    std::atomic<bool> task_can_read_handle{false};
    std::atomic<bool> observed_cancel{false};
    std::atomic<bool> started{false};
    TaskHandle handle;

    handle = pool.post_task_handle([&] {
      started.store(true, std::memory_order_release);

      while (!task_can_read_handle.load(std::memory_order_acquire)) {
        std::this_thread::yield();
      }

      while (!handle.cancellation_token().is_cancellation_requested()) {
        std::this_thread::yield();
      }

      observed_cancel.store(true, std::memory_order_release);
    });

    CHECK(wait_until([&started] { return started.load(std::memory_order_acquire); }));

    task_can_read_handle.store(true, std::memory_order_release);

    CHECK(handle.cancel());
    CHECK(handle.wait(1000));
    CHECK_EQ(handle.state(), TaskExecutionState::kCompleted);
    CHECK(observed_cancel.load(std::memory_order_acquire));
    CHECK_FALSE(handle.cancel());

    pool.shutdown();
  }

  TEST_CASE("ThreadPool already-cancelled parent token cancels handle") {
    ThreadPool pool(1);
    CancellationSource source;

    CHECK(source.request_cancel());

    PostTaskOptions options;
    options.cancellation_token = source.token();

    std::atomic<bool> ran{false};
    auto handle = pool.post_task_handle([&ran] { ran.store(true, std::memory_order_release); }, options);

    CHECK(handle.wait(1000));
    CHECK_EQ(handle.state(), TaskExecutionState::kCancelled);
    CHECK_FALSE(ran.load(std::memory_order_acquire));

    pool.shutdown();
  }

  TEST_CASE("ThreadPool kBlock interrupted by parent cancellation") {
    SmallQueueThreadPool pool(1);

    std::atomic<bool> release_worker{false};
    std::atomic<bool> worker_started{false};

    CHECK(pool.post_task([&] {
      worker_started.store(true, std::memory_order_release);

      while (!release_worker.load(std::memory_order_acquire)) {
        std::this_thread::yield();
      }
    }));

    CHECK(wait_until([&worker_started] { return worker_started.load(std::memory_order_acquire); }));
    CHECK(pool.post_task([] {}));

    CancellationSource source;
    PostTaskOptions options;
    options.overflow_policy = TaskOverflowPolicy::kBlock;
    options.cancellation_token = source.token();

    auto fut =
        std::async(std::launch::async, [&pool, &options] { return pool.post_task_handle([] {}, options).state(); });

    CHECK_EQ(fut.wait_for(30ms), std::future_status::timeout);
    CHECK(source.request_cancel());
    CHECK_EQ(fut.wait_for(1000ms), std::future_status::ready);
    CHECK_EQ(fut.get(), TaskExecutionState::kCancelled);

    release_worker.store(true, std::memory_order_release);
    pool.shutdown();
  }

  TEST_CASE("ThreadPool moved-from handle becomes invalid") {
    ThreadPool pool(1);
    auto original = pool.post_task_handle([] {});
    TaskHandle moved(std::move(original));

    CHECK(moved.valid());
    CHECK_FALSE(original.valid());  // NOLINT(bugprone-use-after-move)
    CHECK_EQ(original.state(), TaskExecutionState::kInvalid);
    CHECK_FALSE(original.cancel());
    CHECK_FALSE(original.wait(0));

    CHECK(moved.wait(1000));
    CHECK_EQ(moved.state(), TaskExecutionState::kCompleted);

    pool.shutdown();
  }

  TEST_CASE("ThreadPool wait on completed handle returns true immediately") {
    ThreadPool pool(1);
    auto handle = pool.post_task_handle([] {});

    CHECK(handle.wait(1000));
    CHECK_EQ(handle.state(), TaskExecutionState::kCompleted);

    CHECK(handle.wait(0));
    CHECK(handle.wait(1));
    CHECK(handle.wait(TaskHandle::kInfinite));

    pool.shutdown();
  }
}

// NOLINTEND
