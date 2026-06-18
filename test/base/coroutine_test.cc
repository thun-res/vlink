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

#include "./base/coroutine.h"

#ifdef VLINK_ENABLE_COROUTINE

#include <doctest/doctest.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <future>
#include <memory>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

#include "../common_test.h"
#include "./base/graph_task.h"
#include "./base/message_loop.h"
#include "./base/schedule.h"
#include "./base/task_handle.h"
#include "./base/thread_pool.h"

using vlink::MessageLoop;
using vlink::Co::await_future;
using vlink::Co::await_graph;
using vlink::Co::co_spawn;
using vlink::Co::co_spawn_with_priority;
using vlink::Co::delay_ms;
using vlink::Co::exec;
using vlink::Co::schedule;
using vlink::Co::sequence;
using vlink::Co::Task;
using vlink::Co::when_all;
using vlink::Co::when_any;
using vlink::Co::yield;

static_assert(!std::is_copy_constructible_v<vlink::Co::ScheduleAwaiter>);
static_assert(!std::is_copy_constructible_v<vlink::Co::YieldAwaiter>);
static_assert(!std::is_copy_constructible_v<vlink::Co::DelayAwaiter>);
static_assert(std::is_move_constructible_v<vlink::Co::ScheduleAwaiter>);
static_assert(std::is_move_constructible_v<vlink::Co::YieldAwaiter>);
static_assert(std::is_move_constructible_v<vlink::Co::DelayAwaiter>);

namespace {

class SmallQueueMessageLoop final : public MessageLoop {
 public:
  using MessageLoop::MessageLoop;

  [[nodiscard]] size_t get_max_task_count() const override { return 1U; }
};

void sleep_ms(int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }

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

Task<> body_basic(std::atomic<int>* counter, std::promise<void>* done) {
  counter->fetch_add(1, std::memory_order_release);
  done->set_value();
  co_return;
}

Task<int> body_return_42() { co_return 42; }

Task<int> body_square(MessageLoop* loop, int key) {
  co_await delay_ms(*loop, 5);
  co_return key* key;
}

Task<> body_compose(MessageLoop* loop, std::promise<int>* out) {
  int a = co_await body_square(loop, 6);
  int b = co_await body_square(loop, 7);
  out->set_value(a + b);
  co_return;
}

Task<> body_yield(MessageLoop* loop, std::atomic<int>* seq, std::promise<void>* done) {
  int v1 = seq->fetch_add(1, std::memory_order_acq_rel);
  CHECK(v1 == 0);

  loop->post_task([seq] { seq->fetch_add(1, std::memory_order_acq_rel); });

  co_await yield(*loop);

  int v3 = seq->fetch_add(1, std::memory_order_acq_rel);
  CHECK(v3 == 2);

  done->set_value();
  co_return;
}

Task<> body_delay_measured(MessageLoop* loop, std::chrono::steady_clock::time_point start,
                           std::promise<int64_t>* done) {
  co_await delay_ms(*loop, 80);
  auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start);
  done->set_value(elapsed.count());
  co_return;
}

Task<> body_delay_zero(MessageLoop* loop, std::promise<std::thread::id>* done) {
  co_await delay_ms(*loop, 0);
  done->set_value(std::this_thread::get_id());
  co_return;
}

Task<> body_schedule(MessageLoop* loop, std::promise<std::thread::id>* done) {
  co_await schedule(*loop);
  done->set_value(std::this_thread::get_id());
  co_return;
}

Task<> body_await_future(MessageLoop* loop, std::future<int> input, std::promise<int>* output) {
  int value = co_await await_future(*loop, std::move(input));
  output->set_value(value + 1);
  co_return;
}

Task<> body_signal_then_await_future(MessageLoop* loop, std::future<int> input, std::promise<void>* awaiting,
                                     std::promise<int>* output) {
  awaiting->set_value();
  int value = co_await await_future(*loop, std::move(input));
  output->set_value(value);
  co_return;
}

Task<> body_signal_then_await_future_cancel(MessageLoop* loop, std::future<int> input, std::promise<void>* awaiting,
                                            std::atomic<bool>* caught, std::promise<void>* done) {
  awaiting->set_value();

  try {
    (void)co_await await_future(*loop, std::move(input));
  } catch (const Exception::OperationCancelled&) {
    caught->store(true, std::memory_order_release);
  }

  done->set_value();
  co_return;
}

Task<> body_schedule_then_done(MessageLoop* target, std::promise<void>* awaiting, std::promise<void>* done) {
  awaiting->set_value();
  co_await schedule(*target);
  done->set_value();
  co_return;
}

Task<> body_yield_after_filling_queue(MessageLoop* loop, std::promise<void>* done) {
  vlink::PostTaskOptions options;
  options.drop_policy = vlink::TaskDropPolicy::kProtected;
  auto filler = loop->post_task_handle([] {}, options);
  CHECK(filler.state() == vlink::TaskExecutionState::kQueued);

  co_await yield(*loop);
  done->set_value();
  co_return;
}

Task<> body_throws(std::atomic<bool>* ran, std::promise<void>* done) {
  ran->store(true, std::memory_order_release);
  done->set_value();
  throw std::runtime_error("boom");
  co_return;
}

Task<int> body_throws_int() {
  throw std::runtime_error("inner");
  co_return 0;
}

Task<> body_catches(std::atomic<bool>* caught, std::promise<void>* done) {
  try {
    (void)co_await body_throws_int();
  } catch (const std::runtime_error&) {
    caught->store(true, std::memory_order_release);
  }

  done->set_value();
  co_return;
}

Task<> body_set_ran(std::atomic<bool>* ran) {
  ran->store(true, std::memory_order_release);
  co_return;
}

Task<int> body_return_1() { co_return 1; }

Task<> body_inc_steps(MessageLoop* loop, std::atomic<int>* steps, int n, std::promise<void>* done) {
  for (int i = 0; i < n; ++i) {
    steps->fetch_add(1, std::memory_order_acq_rel);
    co_await yield(*loop);
  }

  done->set_value();
  co_return;
}

Task<> body_exec_sets(MessageLoop* loop, std::atomic<bool>* ran, std::promise<void>* done) {
  vlink::Schedule::Config cfg(0);
  co_await exec(*loop, cfg, [ran] { ran->store(true, std::memory_order_release); });
  done->set_value();
  co_return;
}

Task<> body_exec_throws(MessageLoop* loop, std::atomic<bool>* caught, std::promise<void>* done) {
  vlink::Schedule::Config cfg(0);

  try {
    co_await exec(*loop, cfg, [] { throw std::runtime_error("boom"); });
  } catch (const std::runtime_error&) {
    caught->store(true, std::memory_order_release);
  }

  done->set_value();
  co_return;
}

Task<int> body_int_const(int v) { co_return v; }

Task<int> body_delay_then(MessageLoop* loop, uint32_t ms, int v) {
  co_await delay_ms(*loop, ms);
  co_return v;
}

Task<> body_delay_then_void(MessageLoop* loop, uint32_t ms) {
  co_await delay_ms(*loop, ms);
  co_return;
}

Task<> body_when_all_int(MessageLoop* loop, std::promise<std::vector<int>>* done) {
  std::vector<Task<int>> tasks;
  tasks.emplace_back(body_int_const(1));
  tasks.emplace_back(body_int_const(2));
  tasks.emplace_back(body_int_const(3));

  auto results = co_await when_all<int>(*loop, std::move(tasks));
  done->set_value(std::move(results));
  co_return;
}

Task<> body_when_any_int(MessageLoop* loop, std::promise<std::pair<size_t, int>>* done) {
  std::vector<Task<int>> tasks;
  tasks.emplace_back(body_delay_then(loop, 80, 0));
  tasks.emplace_back(body_delay_then(loop, 10, 1));
  tasks.emplace_back(body_delay_then(loop, 40, 2));

  auto winner = co_await when_any<int>(*loop, std::move(tasks));
  done->set_value(winner);
  co_return;
}

Task<> body_when_all_void(MessageLoop* loop, std::atomic<int>* counter, std::promise<void>* done) {
  std::vector<Task<void>> tasks;

  for (int i = 0; i < 5; ++i) {
    tasks.emplace_back(body_delay_then_void(loop, 10));
  }

  co_await when_all(*loop, std::move(tasks));
  counter->store(5, std::memory_order_release);
  done->set_value();
  co_return;
}

Task<> body_when_all_empty(MessageLoop* loop, std::promise<void>* done) {
  std::vector<Task<void>> empty;
  co_await when_all(*loop, std::move(empty));
  done->set_value();
  co_return;
}

Task<> body_when_any_void(MessageLoop* loop, std::promise<size_t>* done) {
  std::vector<Task<void>> tasks;
  tasks.emplace_back(body_delay_then_void(loop, 80));
  tasks.emplace_back(body_delay_then_void(loop, 10));

  size_t idx = co_await when_any(*loop, std::move(tasks));
  done->set_value(idx);
  co_return;
}

Task<> body_push_order(std::vector<int>* order, int v) {
  order->push_back(v);
  co_return;
}

Task<> body_push_order_done(std::vector<int>* order, int v, std::promise<void>* done) {
  order->push_back(v);
  done->set_value();
  co_return;
}

Task<> body_delay_push(MessageLoop* loop, uint32_t ms, std::vector<int>* order, int v) {
  co_await delay_ms(*loop, ms);
  order->push_back(v);
  co_return;
}

Task<> body_sequence(MessageLoop* loop, std::vector<int>* order, std::promise<void>* done) {
  std::vector<Task<void>> tasks;
  tasks.emplace_back(body_delay_push(loop, 30, order, 0));
  tasks.emplace_back(body_delay_push(loop, 5, order, 1));
  tasks.emplace_back(body_push_order(order, 2));

  co_await sequence(*loop, std::move(tasks));
  done->set_value();
  co_return;
}

Task<std::unique_ptr<int>> body_produce_unique() { co_return std::make_unique<int>(99); }

Task<> body_consume_unique(std::promise<int>* done) {
  auto value_ptr = co_await body_produce_unique();
  done->set_value(*value_ptr);
  co_return;
}

Task<> body_record_first(std::atomic<int>* first, int my_value, std::promise<void>* done) {
  int expected = 0;
  first->compare_exchange_strong(expected, my_value);
  done->set_value();
  co_return;
}

Task<> body_await_graph(MessageLoop* loop, vlink::GraphTaskPtr root, vlink::GraphTaskPtr leaf, std::atomic<int>* step,
                        std::promise<int>* done) {
  root->execute(loop);
  co_await await_graph(*loop, leaf);
  done->set_value(step->load(std::memory_order_acquire));
  co_return;
}

Task<> body_await_done_graph(MessageLoop* loop, vlink::GraphTaskPtr leaf, std::atomic<int>* step,
                             std::promise<int>* done) {
  co_await await_graph(*loop, leaf);
  done->set_value(step->load(std::memory_order_acquire));
  co_return;
}

Task<> body_signal_then_await_graph(MessageLoop* loop, vlink::GraphTaskPtr graph, std::promise<void>* awaiting,
                                    std::promise<void>* done) {
  awaiting->set_value();
  co_await await_graph(*loop, std::move(graph));
  done->set_value();
  co_return;
}

Task<> body_signal_then_await_graph_cancel(MessageLoop* loop, vlink::GraphTaskPtr graph, std::promise<void>* awaiting,
                                           std::atomic<bool>* caught, std::promise<void>* done) {
  awaiting->set_value();

  try {
    co_await await_graph(*loop, std::move(graph));
  } catch (const Exception::OperationCancelled&) {
    caught->store(true, std::memory_order_release);
  }

  done->set_value();
  co_return;
}

Task<> body_yield_high_prio(MessageLoop* loop, std::vector<int>* order, std::promise<void>* low_done,
                            std::promise<void>* high_done) {
  loop->post_task_with_priority(
      [order_ptr = order, low_ptr = low_done] {
        order_ptr->push_back(0);
        low_ptr->set_value();
      },
      MessageLoop::kLowestPriority);

  co_await yield(*loop, MessageLoop::kHighestPriority);
  order->push_back(1);
  high_done->set_value();
  co_return;
}

Task<> body_inc_shared(std::shared_ptr<std::atomic<int>> counter) {
  counter->fetch_add(1, std::memory_order_acq_rel);
  co_return;
}

Task<> body_empty_sequence(MessageLoop* loop, std::promise<void>* done) {
  std::vector<Task<void>> empty;
  co_await sequence(*loop, std::move(empty));
  done->set_value();
  co_return;
}

Task<> body_await_void_future(MessageLoop* loop, std::future<void> input, std::promise<void>* output) {
  co_await await_future(*loop, std::move(input));
  output->set_value();
  co_return;
}

Task<> body_await_future_catch(MessageLoop* loop, std::future<int> fut, std::atomic<bool>* caught,
                               std::promise<void>* done) {
  try {
    (void)co_await await_future(*loop, std::move(fut));
  } catch (const std::runtime_error&) {
    caught->store(true, std::memory_order_release);
  }

  done->set_value();
  co_return;
}

Task<> body_delay_set(MessageLoop* loop, uint32_t ms, std::atomic<bool>* ran) {
  co_await delay_ms(*loop, ms);
  ran->store(true, std::memory_order_release);
  co_return;
}

Task<> body_consumer_doubles(MessageLoop* loop, std::future<int> in, std::promise<int>* out) {
  int v = co_await await_future(*loop, std::move(in));
  out->set_value(v * 2);
  co_return;
}

Task<> body_when_any_instant(MessageLoop* loop, std::promise<std::pair<size_t, int>>* done) {
  std::vector<Task<int>> tasks;
  tasks.emplace_back(body_int_const(99));
  tasks.emplace_back(body_delay_then(loop, 200, 0));

  auto w = co_await when_any<int>(*loop, std::move(tasks));
  done->set_value(w);
  co_return;
}

Task<int> body_throws_runtime_error() {
  throw std::runtime_error("forced");
  co_return 0;
}

Task<> body_when_all_with_throwing(MessageLoop* loop, std::atomic<bool>* caught_runtime_error,
                                   std::atomic<bool>* caught_future_error, std::atomic<bool>* what_matches,
                                   std::atomic<bool>* caught_other, std::promise<void>* done) {
  std::vector<Task<int>> tasks;
  tasks.emplace_back(body_throws_runtime_error());
  tasks.emplace_back(body_int_const(1));

  try {
    (void)co_await when_all<int>(*loop, std::move(tasks));
  } catch (const std::future_error&) {
    caught_future_error->store(true, std::memory_order_release);
  } catch (const std::runtime_error& e) {
    caught_runtime_error->store(true, std::memory_order_release);

    if (std::strcmp(e.what(), "forced") == 0) {
      what_matches->store(true, std::memory_order_release);
    }
  } catch (...) {
    caught_other->store(true, std::memory_order_release);
  }

  done->set_value();
  co_return;
}

Task<> body_await_moved_from_task(std::atomic<bool>* caught, std::promise<void>* done) {
  Task<int> empty;

  try {
    (void)co_await std::move(empty);
  } catch (const std::logic_error&) {
    caught->store(true, std::memory_order_release);
  }

  done->set_value();
  co_return;
}

Task<> body_await_null_graph(MessageLoop* loop, std::atomic<bool>* caught, std::promise<void>* done) {
  try {
    co_await await_graph(*loop, vlink::GraphTaskPtr{});
  } catch (const std::invalid_argument&) {
    caught->store(true, std::memory_order_release);
  }

  done->set_value();
  co_return;
}

Task<> body_when_any_empty_throws(MessageLoop* loop, std::atomic<bool>* caught, std::promise<void>* done) {
  try {
    std::vector<Task<int>> empty;
    (void)co_await when_any<int>(*loop, std::move(empty));
  } catch (const std::invalid_argument&) {
    caught->store(true, std::memory_order_release);
  }

  done->set_value();
  co_return;
}

Task<> body_when_any_void_empty_throws(MessageLoop* loop, std::atomic<bool>* caught, std::promise<void>* done) {
  try {
    std::vector<Task<void>> empty;
    (void)co_await when_any(*loop, std::move(empty));
  } catch (const std::invalid_argument&) {
    caught->store(true, std::memory_order_release);
  }

  done->set_value();
  co_return;
}

Task<int> body_throws_runtime_error_with(const char* msg) {
  throw std::runtime_error(msg);
  co_return 0;
}

Task<> body_when_any_all_throw(MessageLoop* loop, std::atomic<bool>* caught_runtime_error,
                               std::atomic<bool>* caught_future_error, std::atomic<bool>* what_in_set,
                               std::atomic<bool>* caught_other, std::promise<void>* done) {
  std::vector<Task<int>> tasks;
  tasks.emplace_back(body_throws_runtime_error_with("first"));
  tasks.emplace_back(body_throws_runtime_error_with("second"));

  try {
    (void)co_await when_any<int>(*loop, std::move(tasks));
  } catch (const std::future_error&) {
    caught_future_error->store(true, std::memory_order_release);
  } catch (const std::runtime_error& e) {
    caught_runtime_error->store(true, std::memory_order_release);

    if (std::strcmp(e.what(), "first") == 0 || std::strcmp(e.what(), "second") == 0) {
      what_in_set->store(true, std::memory_order_release);
    }
  } catch (...) {
    caught_other->store(true, std::memory_order_release);
  }

  done->set_value();
  co_return;
}

Task<> body_when_any_partial_fail(MessageLoop* loop, std::promise<std::pair<size_t, int>>* done) {
  std::vector<Task<int>> tasks;
  tasks.emplace_back(body_throws_runtime_error());
  tasks.emplace_back(body_int_const(42));

  auto winner = co_await when_any<int>(*loop, std::move(tasks));
  done->set_value(winner);
  co_return;
}

Task<> body_schedule_throws_on_post_fail(MessageLoop* target, std::atomic<bool>* caught, std::promise<void>* done) {
  try {
    co_await vlink::Coroutine::schedule(*target);
  } catch (const std::runtime_error&) {
    caught->store(true, std::memory_order_release);
  }

  done->set_value();
  co_return;
}

Task<> body_three_priority_awaits(MessageLoop* loop, std::promise<void>* done) {
  co_await schedule(*loop, MessageLoop::kNormalPriority);
  co_await yield(*loop, MessageLoop::kHighestPriority);
  co_await delay_ms(*loop, 5, MessageLoop::kLowestPriority);
  done->set_value();
  co_return;
}

Task<> body_nested_when_all(MessageLoop* loop, std::atomic<int>* counter, std::promise<void>* done) {
  std::vector<Task<void>> first_batch;
  first_batch.emplace_back(body_delay_then_void(loop, 5));
  first_batch.emplace_back(body_delay_then_void(loop, 10));
  co_await when_all(*loop, std::move(first_batch));

  std::vector<Task<void>> second_batch;
  second_batch.emplace_back(body_delay_then_void(loop, 5));
  second_batch.emplace_back(body_delay_then_void(loop, 10));
  co_await when_all(*loop, std::move(second_batch));

  counter->fetch_add(1, std::memory_order_release);
  done->set_value();
  co_return;
}

}  // namespace

TEST_SUITE("base-Coroutine") {
  TEST_CASE("Task<void> spawn runs body once") {
    MessageLoop loop;
    loop.async_run();

    std::atomic<int> counter{0};
    std::promise<void> done;
    auto fut = done.get_future();

    co_spawn(loop, body_basic(&counter, &done));

    fut.get();
    CHECK(counter.load(std::memory_order_acquire) == 1);

    loop.quit();
    loop.wait_for_quit();
  }

  TEST_CASE("Task<int> spawn invokes completion callback with value") {
    MessageLoop loop;
    loop.async_run();

    std::promise<int> done;
    auto fut = done.get_future();

    co_spawn(loop, body_return_42(), [done_ptr = &done](int value) { done_ptr->set_value(value); });

    CHECK(fut.get() == 42);

    loop.quit();
    loop.wait_for_quit();
  }

  TEST_CASE("Task can await another Task and forward the value") {
    MessageLoop loop;
    loop.async_run();

    std::promise<int> done;
    auto fut = done.get_future();

    co_spawn(loop, body_compose(&loop, &done));

    CHECK(fut.get() == 85);

    loop.quit();
    loop.wait_for_quit();
  }

  TEST_CASE("yield re-posts the coroutine and runs after sibling tasks") {
    MessageLoop loop;
    loop.async_run();

    std::atomic<int> seq{0};
    std::promise<void> done;
    auto fut = done.get_future();

    co_spawn(loop, body_yield(&loop, &seq, &done));

    fut.get();
    loop.quit();
    loop.wait_for_quit();
  }

  TEST_CASE("delay_ms suspends for at least the specified duration") {
    MessageLoop loop;
    loop.async_run();

    std::promise<int64_t> done;
    auto fut = done.get_future();

    auto start = std::chrono::steady_clock::now();
    co_spawn(loop, body_delay_measured(&loop, start, &done));

    int64_t elapsed_ms = fut.get();
    CHECK(elapsed_ms >= 70);
    CHECK(elapsed_ms < 500);

    loop.quit();
    loop.wait_for_quit();
  }

  TEST_CASE("delay_ms with zero ms still routes through the loop thread") {
    MessageLoop loop;
    loop.set_name("Coroutine-zero-delay-loop");
    loop.async_run();

    auto caller_id = std::this_thread::get_id();
    std::promise<std::thread::id> done;
    auto fut = done.get_future();

    co_spawn(loop, body_delay_zero(&loop, &done));

    std::thread::id ran_on = fut.get();
    CHECK(ran_on != caller_id);

    loop.quit();
    loop.wait_for_quit();
  }

  TEST_CASE("schedule hops execution onto the loop thread") {
    MessageLoop loop;
    loop.set_name("Coroutine-loop");
    loop.async_run();

    auto caller_id = std::this_thread::get_id();
    std::promise<std::thread::id> done;
    auto fut = done.get_future();

    co_spawn(loop, body_schedule(&loop, &done));

    std::thread::id ran_on = fut.get();
    CHECK(ran_on != caller_id);

    loop.quit();
    loop.wait_for_quit();
  }

  TEST_CASE("await_future resolves and forwards value") {
    MessageLoop loop;
    loop.async_run();

    std::promise<int> input;
    auto input_fut = input.get_future();

    std::promise<int> output;
    auto output_fut = output.get_future();

    co_spawn(loop, body_await_future(&loop, std::move(input_fut), &output));

    sleep_ms(20);
    input.set_value(100);

    CHECK(output_fut.get() == 101);

    loop.quit();
    loop.wait_for_quit();
  }

  TEST_CASE("await_future retries resume while target queue is temporarily full") {
    SmallQueueMessageLoop loop(MessageLoop::kNormalType);
    loop.set_strategy(MessageLoop::kPopStrategy);
    loop.async_run();

    std::promise<int> input;
    auto input_fut = input.get_future();
    std::promise<void> awaiting;
    auto awaiting_fut = awaiting.get_future();
    std::promise<int> output;
    auto output_fut = output.get_future();

    co_spawn(loop, body_signal_then_await_future(&loop, std::move(input_fut), &awaiting, &output));
    awaiting_fut.get();

    std::atomic<bool> release_loop{false};
    std::atomic<bool> loop_blocked{false};

    CHECK(loop.post_task([&] {
      loop_blocked.store(true, std::memory_order_release);

      while (!release_loop.load(std::memory_order_acquire)) {
        std::this_thread::yield();
      }
    }));

    CHECK(wait_until([&loop_blocked] { return loop_blocked.load(std::memory_order_acquire); }));

    vlink::PostTaskOptions options;
    options.drop_policy = vlink::TaskDropPolicy::kProtected;
    auto filler = loop.post_task_handle([] {}, options);
    CHECK(filler.state() == vlink::TaskExecutionState::kQueued);

    input.set_value(7);
    CHECK(output_fut.wait_for(std::chrono::milliseconds(30)) == std::future_status::timeout);

    release_loop.store(true, std::memory_order_release);
    CHECK(output_fut.get() == 7);
    CHECK(filler.wait(1000));

    loop.quit();
    loop.wait_for_quit();
  }

  TEST_CASE("await_future reports cancellation when target loop closes before resume") {
    MessageLoop loop;
    loop.async_run();

    std::promise<int> input;
    auto input_fut = input.get_future();
    std::promise<void> awaiting;
    auto awaiting_fut = awaiting.get_future();
    std::atomic<bool> caught{false};
    std::promise<void> done;
    auto done_fut = done.get_future();

    co_spawn(loop, body_signal_then_await_future_cancel(&loop, std::move(input_fut), &awaiting, &caught, &done));
    awaiting_fut.get();

    loop.quit();
    loop.wait_for_quit();
    input.set_value(11);

    CHECK(done_fut.wait_for(std::chrono::milliseconds(1000)) == std::future_status::ready);
    CHECK(caught.load(std::memory_order_acquire));
  }

  TEST_CASE("await_future reports cancellation when queued resume is dropped by quit") {
    MessageLoop loop;
    loop.async_run();

    std::promise<int> input;
    auto input_fut = input.get_future();
    std::promise<void> awaiting;
    auto awaiting_fut = awaiting.get_future();
    std::atomic<bool> caught{false};
    std::promise<void> done;
    auto done_fut = done.get_future();

    co_spawn(loop, body_signal_then_await_future_cancel(&loop, std::move(input_fut), &awaiting, &caught, &done));
    awaiting_fut.get();

    std::atomic<bool> release_loop{false};
    std::atomic<bool> loop_blocked{false};

    CHECK(loop.post_task([&] {
      loop_blocked.store(true, std::memory_order_release);

      while (!release_loop.load(std::memory_order_acquire)) {
        std::this_thread::yield();
      }
    }));

    CHECK(wait_until([&loop_blocked] { return loop_blocked.load(std::memory_order_acquire); }));

    input.set_value(11);
    CHECK(wait_until([&loop] { return loop.get_task_count() != 0; }));

    loop.quit();
    CHECK(done_fut.wait_for(std::chrono::milliseconds(1000)) == std::future_status::ready);
    CHECK(caught.load(std::memory_order_acquire));

    release_loop.store(true, std::memory_order_release);
    loop.wait_for_quit();
  }

  TEST_CASE("await_future with already-ready future short-circuits") {
    MessageLoop loop;
    loop.async_run();

    std::promise<int> input;
    input.set_value(9);

    std::promise<int> output;
    auto output_fut = output.get_future();

    co_spawn(loop, body_await_future(&loop, input.get_future(), &output));

    CHECK(output_fut.get() == 10);

    loop.quit();
    loop.wait_for_quit();
  }

  TEST_CASE("exception in coroutine body is swallowed by detached spawn") {
    MessageLoop loop;
    loop.async_run();

    std::atomic<bool> ran{false};
    std::promise<void> done;
    auto fut = done.get_future();

    co_spawn(loop, body_throws(&ran, &done));

    fut.get();
    sleep_ms(50);
    CHECK(ran.load(std::memory_order_acquire));
    CHECK(loop.is_running());

    std::promise<void> after;
    auto after_fut = after.get_future();
    CHECK(loop.post_task([&after] { after.set_value(); }));
    CHECK(after_fut.wait_for(std::chrono::milliseconds(1000)) == std::future_status::ready);

    loop.quit();
    loop.wait_for_quit();
  }

  TEST_CASE("exception propagates through co_await to outer Task") {
    MessageLoop loop;
    loop.async_run();

    std::atomic<bool> caught{false};
    std::promise<void> done;
    auto fut = done.get_future();

    co_spawn(loop, body_catches(&caught, &done));

    fut.get();
    CHECK(caught.load(std::memory_order_acquire));

    loop.quit();
    loop.wait_for_quit();
  }

  TEST_CASE("co_spawn fails-safe when loop is quitting") {
    MessageLoop loop;
    loop.async_run();
    loop.quit();
    loop.wait_for_quit();

    std::atomic<bool> ran{false};
    co_spawn(loop, body_set_ran(&ran));

    sleep_ms(50);
    CHECK_FALSE(ran.load(std::memory_order_acquire));
  }

  TEST_CASE("co_spawn retries initial resume while target queue is temporarily full") {
    SmallQueueMessageLoop loop(MessageLoop::kNormalType);
    loop.set_strategy(MessageLoop::kPopStrategy);

    vlink::PostTaskOptions options;
    options.drop_policy = vlink::TaskDropPolicy::kProtected;
    auto filler = loop.post_task_handle([] {}, options);
    CHECK(filler.state() == vlink::TaskExecutionState::kQueued);

    std::atomic<int> counter{0};
    std::promise<void> done;
    auto fut = done.get_future();

    co_spawn(loop, body_basic(&counter, &done));
    CHECK(fut.wait_for(std::chrono::milliseconds(30)) == std::future_status::timeout);

    loop.async_run();

    CHECK(fut.wait_for(std::chrono::milliseconds(1000)) == std::future_status::ready);
    CHECK(counter.load(std::memory_order_acquire) == 1);
    CHECK(filler.wait(1000));

    loop.quit();
    loop.wait_for_quit();
  }

  TEST_CASE("schedule retries while target queue is temporarily full") {
    MessageLoop source;
    SmallQueueMessageLoop target(MessageLoop::kNormalType);
    target.set_strategy(MessageLoop::kPopStrategy);
    source.async_run();
    target.async_run();

    std::atomic<bool> release_target{false};
    std::atomic<bool> target_blocked{false};

    CHECK(target.post_task([&] {
      target_blocked.store(true, std::memory_order_release);

      while (!release_target.load(std::memory_order_acquire)) {
        std::this_thread::yield();
      }
    }));

    CHECK(wait_until([&target_blocked] { return target_blocked.load(std::memory_order_acquire); }));

    vlink::PostTaskOptions options;
    options.drop_policy = vlink::TaskDropPolicy::kProtected;
    auto filler = target.post_task_handle([] {}, options);
    CHECK(filler.state() == vlink::TaskExecutionState::kQueued);

    std::promise<void> awaiting;
    auto awaiting_fut = awaiting.get_future();
    std::promise<void> done;
    auto done_fut = done.get_future();

    co_spawn(source, body_schedule_then_done(&target, &awaiting, &done));
    CHECK(awaiting_fut.wait_for(std::chrono::milliseconds(1000)) == std::future_status::ready);
    CHECK(done_fut.wait_for(std::chrono::milliseconds(30)) == std::future_status::timeout);

    release_target.store(true, std::memory_order_release);
    CHECK(done_fut.wait_for(std::chrono::milliseconds(1000)) == std::future_status::ready);
    CHECK(filler.wait(1000));

    source.quit();
    target.quit();
    source.wait_for_quit();
    target.wait_for_quit();
  }

  TEST_CASE("yield retries while target queue is temporarily full") {
    SmallQueueMessageLoop loop(MessageLoop::kNormalType);
    loop.set_strategy(MessageLoop::kPopStrategy);
    loop.async_run();

    std::promise<void> done;
    auto done_fut = done.get_future();

    co_spawn(loop, body_yield_after_filling_queue(&loop, &done));

    CHECK(done_fut.wait_for(std::chrono::milliseconds(1000)) == std::future_status::ready);

    loop.quit();
    loop.wait_for_quit();
  }

  TEST_CASE("Task default-constructs to an invalid state") {
    Task<int> empty;
    CHECK_FALSE(empty.valid());
    CHECK_FALSE(empty.done());
  }

  TEST_CASE("Task is move-constructible and move-assignable") {
    Task<int> a = body_return_1();
    CHECK(a.valid());

    Task<int> b = std::move(a);
    CHECK(b.valid());
    CHECK_FALSE(a.valid());  // NOLINT(bugprone-use-after-move)

    Task<int> c;
    c = std::move(b);
    CHECK(c.valid());
    CHECK_FALSE(b.valid());  // NOLINT(bugprone-use-after-move)
  }

  TEST_CASE("multiple coroutines on same loop interleave via yield") {
    MessageLoop loop;
    loop.async_run();

    std::atomic<int> a_steps{0};
    std::atomic<int> b_steps{0};
    std::promise<void> done_a;
    std::promise<void> done_b;
    auto fa = done_a.get_future();
    auto fb = done_b.get_future();

    co_spawn(loop, body_inc_steps(&loop, &a_steps, 5, &done_a));
    co_spawn(loop, body_inc_steps(&loop, &b_steps, 5, &done_b));

    fa.get();
    fb.get();
    CHECK(a_steps.load(std::memory_order_acquire) == 5);
    CHECK(b_steps.load(std::memory_order_acquire) == 5);

    loop.quit();
    loop.wait_for_quit();
  }

  TEST_CASE("exec wraps Schedule::Config and resumes after the callback") {
    MessageLoop loop;
    loop.async_run();

    std::atomic<bool> ran{false};
    std::promise<void> done;
    auto fut = done.get_future();

    co_spawn(loop, body_exec_sets(&loop, &ran, &done));

    fut.get();
    CHECK(ran.load(std::memory_order_acquire));

    loop.quit();
    loop.wait_for_quit();
  }

  TEST_CASE("exec propagates callback exception through co_await") {
    MessageLoop loop;
    loop.async_run();

    std::atomic<bool> caught{false};
    std::promise<void> done;
    auto fut = done.get_future();

    co_spawn(loop, body_exec_throws(&loop, &caught, &done));

    fut.get();
    CHECK(caught.load(std::memory_order_acquire));

    loop.quit();
    loop.wait_for_quit();
  }

  TEST_CASE("when_all<int> collects results from all sub-tasks") {
    MessageLoop loop;
    loop.async_run();

    std::promise<std::vector<int>> done;
    auto fut = done.get_future();

    co_spawn(loop, body_when_all_int(&loop, &done));

    auto results = fut.get();
    REQUIRE(results.size() == 3);
    CHECK(results[0] == 1);
    CHECK(results[1] == 2);
    CHECK(results[2] == 3);

    loop.quit();
    loop.wait_for_quit();
  }

  TEST_CASE("when_any<int> records the first completed sub-task as winner") {
    MessageLoop loop;
    loop.async_run();

    std::promise<std::pair<size_t, int>> done;
    auto fut = done.get_future();

    co_spawn(loop, body_when_any_int(&loop, &done));

    auto winner = fut.get();
    CHECK(winner.first == 1);
    CHECK(winner.second == 1);

    loop.quit();
    loop.wait_for_quit();
  }

  TEST_CASE("when_all<void> awaits all sub-tasks") {
    MessageLoop loop;
    loop.async_run();

    std::atomic<int> counter{0};
    std::promise<void> done;
    auto fut = done.get_future();

    co_spawn(loop, body_when_all_void(&loop, &counter, &done));

    fut.get();
    CHECK(counter.load(std::memory_order_acquire) == 5);

    loop.quit();
    loop.wait_for_quit();
  }

  TEST_CASE("when_all on empty vector resolves immediately") {
    MessageLoop loop;
    loop.async_run();

    std::promise<void> done;
    auto fut = done.get_future();

    co_spawn(loop, body_when_all_empty(&loop, &done));

    auto status = fut.wait_for(std::chrono::seconds(1));
    CHECK(status == std::future_status::ready);

    loop.quit();
    loop.wait_for_quit();
  }

  TEST_CASE("when_any<void> records the first index to complete as winner") {
    MessageLoop loop;
    loop.async_run();

    std::promise<size_t> done;
    auto fut = done.get_future();

    co_spawn(loop, body_when_any_void(&loop, &done));

    CHECK(fut.get() == 1);

    loop.quit();
    loop.wait_for_quit();
  }

  TEST_CASE("sequence runs Task<void> entries strictly in order") {
    MessageLoop loop;
    loop.async_run();

    std::vector<int> order;
    std::promise<void> done;
    auto fut = done.get_future();

    co_spawn(loop, body_sequence(&loop, &order, &done));

    fut.get();
    REQUIRE(order.size() == 3);
    CHECK(order[0] == 0);
    CHECK(order[1] == 1);
    CHECK(order[2] == 2);

    loop.quit();
    loop.wait_for_quit();
  }

  TEST_CASE("Task with move-only TypeT (unique_ptr<int>) round-trip") {
    MessageLoop loop;
    loop.async_run();

    std::promise<int> done;
    auto fut = done.get_future();

    co_spawn(loop, body_consume_unique(&done));

    CHECK(fut.get() == 99);

    loop.quit();
    loop.wait_for_quit();
  }

  TEST_CASE("await_graph resumes after GraphTask DAG completes") {
    MessageLoop loop;
    loop.async_run();

    std::atomic<int> step{0};
    auto a = vlink::GraphTask::create("A", [step_ptr = &step] { step_ptr->fetch_add(1, std::memory_order_release); });
    auto b = vlink::GraphTask::create("B", [step_ptr = &step] { step_ptr->fetch_add(10, std::memory_order_release); });
    a->precede(b);

    std::promise<int> done;
    auto fut = done.get_future();

    co_spawn(loop, body_await_graph(&loop, a, b, &step, &done));

    CHECK(fut.get() == 11);

    loop.quit();
    loop.wait_for_quit();
  }

  TEST_CASE("await_graph retries resume while target queue is temporarily full") {
    SmallQueueMessageLoop loop(MessageLoop::kNormalType);
    loop.set_strategy(MessageLoop::kPopStrategy);
    loop.async_run();

    vlink::ThreadPool pool(1);
    auto graph = vlink::GraphTask::create([] {});

    std::promise<void> awaiting;
    auto awaiting_fut = awaiting.get_future();
    std::promise<void> done;
    auto done_fut = done.get_future();

    co_spawn(loop, body_signal_then_await_graph(&loop, graph, &awaiting, &done));
    awaiting_fut.get();

    std::atomic<bool> release_loop{false};
    std::atomic<bool> loop_blocked{false};

    CHECK(loop.post_task([&] {
      loop_blocked.store(true, std::memory_order_release);

      while (!release_loop.load(std::memory_order_acquire)) {
        std::this_thread::yield();
      }
    }));

    CHECK(wait_until([&loop_blocked] { return loop_blocked.load(std::memory_order_acquire); }));

    vlink::PostTaskOptions options;
    options.drop_policy = vlink::TaskDropPolicy::kProtected;
    auto filler = loop.post_task_handle([] {}, options);
    CHECK(filler.state() == vlink::TaskExecutionState::kQueued);

    graph->execute(&pool);
    CHECK(done_fut.wait_for(std::chrono::milliseconds(30)) == std::future_status::timeout);

    release_loop.store(true, std::memory_order_release);
    CHECK(done_fut.wait_for(std::chrono::milliseconds(1000)) == std::future_status::ready);
    CHECK(filler.wait(1000));

    pool.shutdown();
    loop.quit();
    loop.wait_for_quit();
  }

  TEST_CASE("await_graph reports cancellation when target loop closes before resume") {
    MessageLoop loop;
    loop.async_run();
    ThreadPool pool(1);

    auto graph = GraphTask::create([] {});
    std::promise<void> awaiting;
    auto awaiting_fut = awaiting.get_future();
    std::atomic<bool> caught{false};
    std::promise<void> done;
    auto done_fut = done.get_future();

    co_spawn(loop, body_signal_then_await_graph_cancel(&loop, graph, &awaiting, &caught, &done));
    awaiting_fut.get();

    loop.quit();
    loop.wait_for_quit();
    graph->execute(&pool);

    CHECK(done_fut.wait_for(std::chrono::milliseconds(1000)) == std::future_status::ready);
    CHECK(caught.load(std::memory_order_acquire));

    pool.shutdown();
  }

  TEST_CASE("await_graph reports cancellation when queued resume is dropped by quit") {
    MessageLoop loop;
    loop.async_run();
    ThreadPool pool(1);

    auto graph = GraphTask::create([] {});
    std::promise<void> awaiting;
    auto awaiting_fut = awaiting.get_future();
    std::atomic<bool> caught{false};
    std::promise<void> done;
    auto done_fut = done.get_future();

    co_spawn(loop, body_signal_then_await_graph_cancel(&loop, graph, &awaiting, &caught, &done));
    awaiting_fut.get();

    std::atomic<bool> release_loop{false};
    std::atomic<bool> loop_blocked{false};

    CHECK(loop.post_task([&] {
      loop_blocked.store(true, std::memory_order_release);

      while (!release_loop.load(std::memory_order_acquire)) {
        std::this_thread::yield();
      }
    }));

    CHECK(wait_until([&loop_blocked] { return loop_blocked.load(std::memory_order_acquire); }));

    graph->execute(&pool);
    CHECK(wait_until([&loop] { return loop.get_task_count() != 0; }));

    loop.quit();
    CHECK(done_fut.wait_for(std::chrono::milliseconds(1000)) == std::future_status::ready);
    CHECK(caught.load(std::memory_order_acquire));

    release_loop.store(true, std::memory_order_release);
    loop.wait_for_quit();
    pool.shutdown();
  }

  TEST_CASE("co_spawn_with_priority dispatches at the requested priority level") {
    MessageLoop loop(MessageLoop::kPriorityType);
    loop.async_run();

    std::promise<void> release;
    std::promise<void> barrier_started;
    auto release_future = release.get_future();
    auto barrier_fut = barrier_started.get_future();

    loop.post_task_with_priority(
        [release_future = std::move(release_future), &barrier_started]() mutable {
          barrier_started.set_value();
          release_future.wait();
        },
        MessageLoop::kHighestPriority);

    REQUIRE(barrier_fut.wait_for(std::chrono::milliseconds(1000)) == std::future_status::ready);

    std::atomic<int> first_executed{0};
    std::promise<void> low_done;
    std::promise<void> high_done;
    auto low_fut = low_done.get_future();
    auto high_fut = high_done.get_future();

    co_spawn_with_priority(loop, body_record_first(&first_executed, 1, &low_done), MessageLoop::kLowestPriority);
    co_spawn_with_priority(loop, body_record_first(&first_executed, 2, &high_done), MessageLoop::kHighestPriority);

    release.set_value();

    REQUIRE(high_fut.wait_for(std::chrono::milliseconds(1000)) == std::future_status::ready);
    REQUIRE(low_fut.wait_for(std::chrono::milliseconds(1000)) == std::future_status::ready);

    CHECK(first_executed.load(std::memory_order_acquire) == 2);

    loop.quit();
    loop.wait_for_quit();
  }

  TEST_CASE("co_spawn default priority is normal on priority loop") {
    MessageLoop loop(MessageLoop::kPriorityType);
    loop.async_run();

    std::promise<void> release;
    std::promise<void> barrier_started;
    auto release_future = release.get_future();
    auto barrier_fut = barrier_started.get_future();

    loop.post_task_with_priority(
        [release_future = std::move(release_future), &barrier_started]() mutable {
          barrier_started.set_value();
          release_future.wait();
        },
        MessageLoop::kHighestPriority);

    REQUIRE(barrier_fut.wait_for(std::chrono::milliseconds(1000)) == std::future_status::ready);

    std::vector<int> order;
    std::promise<void> default_done;
    std::promise<void> low_done;
    std::promise<void> high_done;
    auto default_fut = default_done.get_future();
    auto low_fut = low_done.get_future();
    auto high_fut = high_done.get_future();

    co_spawn(loop, body_push_order_done(&order, 0, &default_done));
    co_spawn_with_priority(loop, body_push_order_done(&order, 1, &low_done), MessageLoop::kLowestPriority);
    co_spawn_with_priority(loop, body_push_order_done(&order, 2, &high_done), MessageLoop::kHighestPriority);

    release.set_value();

    REQUIRE(high_fut.wait_for(std::chrono::milliseconds(1000)) == std::future_status::ready);
    REQUIRE(default_fut.wait_for(std::chrono::milliseconds(1000)) == std::future_status::ready);
    REQUIRE(low_fut.wait_for(std::chrono::milliseconds(1000)) == std::future_status::ready);

    REQUIRE(order.size() == 3U);
    CHECK(order[0] == 2);
    CHECK(order[1] == 0);
    CHECK(order[2] == 1);

    loop.quit();
    loop.wait_for_quit();
  }

  TEST_CASE("yield with priority pre-empts lower-priority pending tasks") {
    MessageLoop loop(MessageLoop::kPriorityType);
    loop.async_run();

    std::vector<int> order;
    std::promise<void> low_done;
    std::promise<void> high_done;
    auto low_fut = low_done.get_future();
    auto high_fut = high_done.get_future();

    co_spawn_with_priority(loop, body_yield_high_prio(&loop, &order, &low_done, &high_done),
                           MessageLoop::kHighestPriority);

    REQUIRE(high_fut.wait_for(std::chrono::milliseconds(1000)) == std::future_status::ready);
    REQUIRE(low_fut.wait_for(std::chrono::milliseconds(1000)) == std::future_status::ready);

    REQUIRE(order.size() == 2);
    CHECK(order[0] == 1);
    CHECK(order[1] == 0);

    loop.quit();
    loop.wait_for_quit();
  }

  TEST_CASE("await_graph short-circuits if graph is already terminal") {
    MessageLoop loop;
    loop.async_run();

    std::atomic<int> step{0};
    auto a = vlink::GraphTask::create("X", [step_ptr = &step] { step_ptr->fetch_add(1, std::memory_order_release); });
    a->execute(&loop);

    while (a->get_status() != vlink::GraphTask::kStatusDone) {
      sleep_ms(2);
    }

    std::promise<int> done;
    auto fut = done.get_future();
    co_spawn(loop, body_await_done_graph(&loop, a, &step, &done));

    auto status = fut.wait_for(std::chrono::seconds(1));
    CHECK(status == std::future_status::ready);

    loop.quit();
    loop.wait_for_quit();
  }

  TEST_CASE("sequence on empty vector resolves immediately") {
    MessageLoop loop;
    loop.async_run();

    std::promise<void> done;
    auto fut = done.get_future();

    co_spawn(loop, body_empty_sequence(&loop, &done));

    auto status = fut.wait_for(std::chrono::seconds(1));
    CHECK(status == std::future_status::ready);

    loop.quit();
    loop.wait_for_quit();
  }

  TEST_CASE("Task::release transfers ownership and leaves Task invalid") {
    auto t = body_return_1();
    CHECK(t.valid());

    auto handle = t.release();
    CHECK_FALSE(t.valid());
    CHECK(handle != nullptr);

    handle.destroy();
  }

  TEST_CASE("Task::native_handle reflects internal state") {
    Task<int> empty;
    CHECK(empty.native_handle() == nullptr);

    auto t = body_return_1();
    CHECK(t.native_handle() != nullptr);
  }

  TEST_CASE("await_future<void> resolves to no value") {
    MessageLoop loop;
    loop.async_run();

    std::promise<void> input;
    auto input_fut = input.get_future();

    std::promise<void> output;
    auto output_fut = output.get_future();

    co_spawn(loop, body_await_void_future(&loop, std::move(input_fut), &output));

    sleep_ms(20);
    input.set_value();

    auto status = output_fut.wait_for(std::chrono::seconds(1));
    CHECK(status == std::future_status::ready);

    loop.quit();
    loop.wait_for_quit();
  }

  TEST_CASE("co_spawn_with_priority<int> with callback dispatches correctly") {
    MessageLoop loop(MessageLoop::kPriorityType);
    loop.async_run();

    std::promise<int> done;
    auto fut = done.get_future();

    co_spawn_with_priority(
        loop, body_return_42(), [done_ptr = &done](int v) { done_ptr->set_value(v); }, MessageLoop::kHighestPriority);

    CHECK(fut.get() == 42);

    loop.quit();
    loop.wait_for_quit();
  }

  TEST_CASE("stress: many short coroutines via MemoryPool-backed frames") {
    MessageLoop loop;
    loop.async_run();

    static constexpr int kCount = 500;
    auto finished = std::make_shared<std::atomic<int>>(0);

    for (int i = 0; i < kCount; ++i) {
      co_spawn(loop, body_inc_shared(finished));
    }

    int deadline_ms = 2000;

    while (finished->load(std::memory_order_acquire) < kCount && deadline_ms > 0) {
      sleep_ms(5);
      deadline_ms -= 5;
    }

    CHECK(finished->load(std::memory_order_acquire) == kCount);

    loop.quit();
    loop.wait_for_quit();
  }

  TEST_CASE("nested when_all<void> inside another coroutine") {
    MessageLoop loop;
    loop.async_run();

    std::atomic<int> counter{0};
    std::promise<void> done;
    auto fut = done.get_future();

    co_spawn(loop, body_nested_when_all(&loop, &counter, &done));

    fut.get();
    CHECK(counter.load(std::memory_order_acquire) == 1);

    loop.quit();
    loop.wait_for_quit();
  }

  TEST_CASE("await_future rethrows when promise was set_exception") {
    MessageLoop loop;
    loop.async_run();

    std::promise<int> input;
    input.set_exception(std::make_exception_ptr(std::runtime_error("forced")));

    std::atomic<bool> caught{false};
    std::promise<void> done;
    auto done_fut = done.get_future();

    co_spawn(loop, body_await_future_catch(&loop, input.get_future(), &caught, &done));

    done_fut.get();
    CHECK(caught.load(std::memory_order_acquire));

    loop.quit();
    loop.wait_for_quit();
  }

  TEST_CASE("delay_ms after loop.quit destroys frame without resume") {
    MessageLoop loop;
    loop.async_run();
    loop.quit();
    loop.wait_for_quit();

    std::atomic<bool> ran{false};
    co_spawn(loop, body_delay_set(&loop, 10, &ran));

    sleep_ms(50);
    CHECK_FALSE(ran.load(std::memory_order_acquire));
  }

  TEST_CASE("co_spawn on kLockfreeType MessageLoop") {
    MessageLoop loop(MessageLoop::kLockfreeType);
    loop.async_run();

    std::atomic<int> counter{0};
    std::promise<void> done;
    auto fut = done.get_future();

    co_spawn(loop, body_basic(&counter, &done));

    fut.get();
    CHECK(counter.load(std::memory_order_acquire) == 1);

    loop.quit();
    loop.wait_for_quit();
  }

  TEST_CASE("co_spawn_with_priority on kNormalType runs without honoring priority") {
    MessageLoop loop;
    loop.async_run();

    std::atomic<int> a_done{0};
    std::atomic<int> b_done{0};
    std::promise<void> done_a;
    std::promise<void> done_b;

    co_spawn_with_priority(loop, body_record_first(&a_done, 1, &done_a), MessageLoop::kHighestPriority);
    co_spawn_with_priority(loop, body_record_first(&b_done, 1, &done_b), MessageLoop::kLowestPriority);

    done_a.get_future().get();
    done_b.get_future().get();

    CHECK(a_done.load(std::memory_order_acquire) == 1);
    CHECK(b_done.load(std::memory_order_acquire) == 1);

    loop.quit();
    loop.wait_for_quit();
  }

  TEST_CASE("cross-loop: coroutine on loopA awaits future fulfilled from loopB") {
    MessageLoop loop_a;
    MessageLoop loop_b;
    loop_a.async_run();
    loop_b.async_run();

    std::promise<int> bridge;

    std::promise<int> result;
    auto result_fut = result.get_future();

    co_spawn(loop_a, body_consumer_doubles(&loop_a, bridge.get_future(), &result));

    loop_b.post_task([&bridge]() { bridge.set_value(7); });

    CHECK(result_fut.get() == 14);

    loop_a.quit();
    loop_b.quit();
    loop_a.wait_for_quit();
    loop_b.wait_for_quit();
  }

  TEST_CASE("when_any picks the already-completed sub-task as winner") {
    MessageLoop loop;
    loop.async_run();

    std::promise<std::pair<size_t, int>> done;
    auto fut = done.get_future();

    co_spawn(loop, body_when_any_instant(&loop, &done));

    auto w = fut.get();
    CHECK(w.first == 0);
    CHECK(w.second == 99);

    loop.quit();
    loop.wait_for_quit();
  }

  TEST_CASE("when_all<int> with a throwing sub-task rethrows the original exception type") {
    MessageLoop loop;
    loop.async_run();

    std::atomic<bool> caught_runtime_error{false};
    std::atomic<bool> caught_future_error{false};
    std::atomic<bool> what_matches{false};
    std::atomic<bool> caught_other{false};
    std::promise<void> done;
    auto fut = done.get_future();

    co_spawn(loop, body_when_all_with_throwing(&loop, &caught_runtime_error, &caught_future_error, &what_matches,
                                               &caught_other, &done));

    auto status = fut.wait_for(std::chrono::seconds(2));
    CHECK(status == std::future_status::ready);
    CHECK(caught_runtime_error.load(std::memory_order_acquire));
    CHECK(what_matches.load(std::memory_order_acquire));
    CHECK_FALSE(caught_future_error.load(std::memory_order_acquire));
    CHECK_FALSE(caught_other.load(std::memory_order_acquire));

    loop.quit();
    loop.wait_for_quit();
  }

  TEST_CASE("when_any<int> with all sub-tasks throwing rethrows the first observed exception") {
    MessageLoop loop;
    loop.async_run();

    std::atomic<bool> caught_runtime_error{false};
    std::atomic<bool> caught_future_error{false};
    std::atomic<bool> what_in_set{false};
    std::atomic<bool> caught_other{false};
    std::promise<void> done;
    auto fut = done.get_future();

    co_spawn(loop, body_when_any_all_throw(&loop, &caught_runtime_error, &caught_future_error, &what_in_set,
                                           &caught_other, &done));

    auto status = fut.wait_for(std::chrono::seconds(2));
    CHECK(status == std::future_status::ready);
    CHECK(caught_runtime_error.load(std::memory_order_acquire));
    CHECK(what_in_set.load(std::memory_order_acquire));
    CHECK_FALSE(caught_future_error.load(std::memory_order_acquire));
    CHECK_FALSE(caught_other.load(std::memory_order_acquire));

    loop.quit();
    loop.wait_for_quit();
  }

  TEST_CASE("when_any<int> with a failing and a succeeding sub-task returns the successful winner") {
    MessageLoop loop;
    loop.async_run();

    std::promise<std::pair<size_t, int>> done;
    auto fut = done.get_future();

    co_spawn(loop, body_when_any_partial_fail(&loop, &done));

    auto status = fut.wait_for(std::chrono::seconds(2));
    CHECK(status == std::future_status::ready);
    auto winner = fut.get();
    CHECK(winner.first == 1);
    CHECK(winner.second == 42);

    loop.quit();
    loop.wait_for_quit();
  }

  TEST_CASE("schedule + yield + delay_ms accept MessageLoop::Priority values on kPriorityType") {
    MessageLoop loop(MessageLoop::kPriorityType);
    loop.async_run();

    std::promise<void> done;
    auto fut = done.get_future();

    co_spawn(loop, body_three_priority_awaits(&loop, &done));

    fut.get();

    loop.quit();
    loop.wait_for_quit();
  }

  TEST_CASE("co_await on moved-from Task throws std::logic_error") {
    MessageLoop loop;
    loop.async_run();

    std::atomic<bool> caught{false};
    std::promise<void> done;
    auto fut = done.get_future();

    co_spawn(loop, body_await_moved_from_task(&caught, &done));

    fut.get();
    CHECK(caught.load(std::memory_order_acquire));

    loop.quit();
    loop.wait_for_quit();
  }

  TEST_CASE("await_graph with null GraphTaskPtr throws std::invalid_argument") {
    MessageLoop loop;
    loop.async_run();

    std::atomic<bool> caught{false};
    std::promise<void> done;
    auto fut = done.get_future();

    co_spawn(loop, body_await_null_graph(&loop, &caught, &done));

    fut.get();
    CHECK(caught.load(std::memory_order_acquire));

    loop.quit();
    loop.wait_for_quit();
  }

  TEST_CASE("when_any<int> with empty vector throws std::invalid_argument") {
    MessageLoop loop;
    loop.async_run();

    std::atomic<bool> caught{false};
    std::promise<void> done;
    auto fut = done.get_future();

    co_spawn(loop, body_when_any_empty_throws(&loop, &caught, &done));

    fut.get();
    CHECK(caught.load(std::memory_order_acquire));

    loop.quit();
    loop.wait_for_quit();
  }

  TEST_CASE("when_any<void> with empty vector throws std::invalid_argument") {
    MessageLoop loop;
    loop.async_run();

    std::atomic<bool> caught{false};
    std::promise<void> done;
    auto fut = done.get_future();

    co_spawn(loop, body_when_any_void_empty_throws(&loop, &caught, &done));

    fut.get();
    CHECK(caught.load(std::memory_order_acquire));

    loop.quit();
    loop.wait_for_quit();
  }

  TEST_CASE("schedule awaiter throws std::runtime_error when loop is quitting") {
    MessageLoop loop;
    loop.async_run();
    loop.quit();
    loop.wait_for_quit();

    MessageLoop driver;
    driver.async_run();

    std::atomic<bool> caught_runtime{false};
    std::promise<void> done;
    auto fut = done.get_future();

    co_spawn(driver, body_schedule_throws_on_post_fail(&loop, &caught_runtime, &done));

    fut.get();
    CHECK(caught_runtime.load(std::memory_order_acquire));

    driver.quit();
    driver.wait_for_quit();
  }

  TEST_CASE("Co alias namespace exposes the same types as Coroutine") {
    static_assert(std::is_same_v<vlink::Co::Task<int>, vlink::Coroutine::Task<int>>);

    MessageLoop loop;
    loop.async_run();

    std::promise<int> done;
    auto fut = done.get_future();

    co_spawn(loop, body_return_42(), [done_ptr = &done](int v) { done_ptr->set_value(v + 1); });

    CHECK(fut.get() == 43);

    loop.quit();
    loop.wait_for_quit();
  }
}

#endif  // VLINK_ENABLE_COROUTINE

// NOLINTEND
