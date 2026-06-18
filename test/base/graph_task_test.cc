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

#include "./base/graph_task.h"

#include <doctest/doctest.h>

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

#include "../common_test.h"
#include "./base/thread_pool.h"

namespace {

ThreadPool& shared_pool() {
  static auto* pool = new ThreadPool(1);
  return *pool;
}

void run_graph(const GraphTaskPtr& entry, int timeout_ms = 50) {
  entry->execute(&shared_pool());
  std::this_thread::sleep_for(std::chrono::milliseconds(timeout_ms));
}

struct RejectEngine {
  bool post_task(GraphTask::Callback&&) { return false; }
};

}  // namespace

TEST_SUITE("base-GraphTask") {
  TEST_CASE("get_name and set_name") {
    auto task = GraphTask::create("my_task", [] {});

    CHECK_EQ(task->get_name(), "my_task");

    task->set_name("renamed");
    CHECK_EQ(task->get_name(), "renamed");
  }

  TEST_CASE("unnamed task gets auto-generated non-empty name") {
    auto task = GraphTask::create([] {});
    CHECK_FALSE(task->get_name().empty());
  }

  TEST_CASE("initial status is kStatusInActive") {
    auto task = GraphTask::create("t", [] {});
    CHECK_EQ(task->get_status(), GraphTask::kStatusInActive);
  }

  TEST_CASE("set_policy and get_policy round-trip all values") {
    auto task = GraphTask::create([] {});
    CHECK_EQ(task->get_policy(), GraphTask::kPolicyOnce);

    task->set_policy(GraphTask::kPolicyMultiple);
    CHECK_EQ(task->get_policy(), GraphTask::kPolicyMultiple);

    task->set_policy(GraphTask::kPolicyWaitAll);
    CHECK_EQ(task->get_policy(), GraphTask::kPolicyWaitAll);
  }

  TEST_CASE("is_condition_task is false for regular task") {
    auto task = GraphTask::create([] {});
    CHECK_FALSE(task->is_condition_task());
  }

  TEST_CASE("is_condition_task is true for condition task") {
    auto task = GraphTask::create_condition([] { return 0; });
    CHECK(task->is_condition_task());
  }

  TEST_CASE("named condition factory sets name and condition number") {
    auto task = GraphTask::create_condition("my_cond", [] { return 0; }, 2);
    CHECK(task->is_condition_task());
    CHECK_EQ(task->get_name(), "my_cond");
    CHECK_EQ(task->get_condition_number(), 2);
  }

  TEST_CASE("unnamed condition task has non-empty name") {
    auto task = GraphTask::create_condition([] { return 0; }, 3);
    CHECK(task->is_condition_task());
    CHECK_EQ(task->get_condition_number(), 3);
    CHECK_FALSE(task->get_name().empty());
  }

  TEST_CASE("get_condition_number and set_condition_number") {
    auto task = GraphTask::create("c", [] {}, 3);
    CHECK_EQ(task->get_condition_number(), 3);

    task->set_condition_number(5);
    CHECK_EQ(task->get_condition_number(), 5);
  }

  TEST_CASE("get_priority and set_priority") {
    auto task = GraphTask::create([] {});
    task->set_priority(42u);
    CHECK_EQ(task->get_priority(), 42u);
  }

  TEST_CASE("get_max_recursion_depth and set_max_recursion_depth") {
    auto task = GraphTask::create([] {});
    task->set_max_recursion_depth(128u);
    CHECK_EQ(task->get_max_recursion_depth(), 128u);
  }

  TEST_CASE("get_group_name and set_group_name") {
    auto task = GraphTask::create("t", [] {});
    task->set_group_name("my_group");
    CHECK_EQ(task->get_group_name(), "my_group");
  }

  TEST_CASE("has_cycle false for single node") {
    auto task = GraphTask::create("solo", [] {});
    CHECK_FALSE(task->has_cycle());
  }

  TEST_CASE("has_cycle false for linear chain a->b->c") {
    auto a = GraphTask::create("A", [] {});
    auto b = GraphTask::create("B", [] {});
    auto c = GraphTask::create("C", [] {});

    a->succeed(b);
    b->succeed(c);

    CHECK_FALSE(a->has_cycle());
  }

  TEST_CASE("has_cycle false for diamond DAG") {
    auto a = GraphTask::create("A", [] {});
    auto b = GraphTask::create("B", [] {});
    auto c = GraphTask::create("C", [] {});
    auto d = GraphTask::create("D", [] {});

    a->succeed(b);
    a->succeed(c);
    b->succeed(d);
    c->succeed(d);

    CHECK_FALSE(a->has_cycle());
  }

  TEST_CASE("back-edge creating cycle is rejected") {
    auto a = GraphTask::create("A", [] {});
    auto b = GraphTask::create("B", [] {});
    auto c = GraphTask::create("C", [] {});

    a->precede(b);
    b->precede(c);

    REQUIRE_FALSE(a->has_cycle());

    c->precede(a);

    CHECK_FALSE(a->has_cycle());
  }

  TEST_CASE("self-loop precede throws and leaves graph unchanged") {
    auto a = GraphTask::create("A", [] {});
    CHECK_THROWS(a->precede(a));
    CHECK(a->get_succeed_task_list().empty());
    CHECK(a->get_precede_task_list().empty());
  }

  TEST_CASE("precede cycle rejection leaves lists intact") {
    auto a = GraphTask::create("a", [] {});
    auto b = GraphTask::create("b", [] {});
    auto c = GraphTask::create("c", [] {});

    a->precede(b);
    b->precede(c);

    REQUIRE_EQ(a->get_succeed_task_list().size(), 1u);
    REQUIRE_EQ(b->get_succeed_task_list().size(), 1u);
    REQUIRE(c->get_succeed_task_list().empty());

    c->precede(a);

    CHECK_EQ(a->get_succeed_task_list().size(), 1u);
    CHECK_EQ(b->get_succeed_task_list().size(), 1u);
    CHECK(c->get_succeed_task_list().empty());
    CHECK(a->get_precede_task_list().empty());
  }

  TEST_CASE("succeed cycle rejection leaves lists intact") {
    auto a = GraphTask::create("a", [] {});
    auto b = GraphTask::create("b", [] {});
    auto c = GraphTask::create("c", [] {});

    a->precede(b);
    b->precede(c);

    a->succeed(c);

    CHECK_EQ(a->get_succeed_task_list().size(), 1u);
    CHECK_EQ(b->get_succeed_task_list().size(), 1u);
    CHECK(c->get_succeed_task_list().empty());
    CHECK(a->get_precede_task_list().empty());
  }

  TEST_CASE("precede and succeed list sizes after edge building") {
    auto a = GraphTask::create("A", [] {});
    auto b = GraphTask::create("B", [] {});
    auto c = GraphTask::create("C", [] {});

    a->succeed(b);
    a->succeed(c);

    CHECK_EQ(a->get_precede_task_list().size(), 2u);
    CHECK_EQ(b->get_succeed_task_list().size(), 1u);
    CHECK_EQ(c->get_succeed_task_list().size(), 1u);
  }

  TEST_CASE("remove_succeed_task removes edge from both sides") {
    auto a = GraphTask::create("A", [] {});
    auto b = GraphTask::create("B", [] {});

    a->succeed(b);
    REQUIRE_EQ(b->get_succeed_task_list().size(), 1u);

    a->remove_succeed_task(b);
    CHECK(b->get_succeed_task_list().empty());
    CHECK(a->get_precede_task_list().empty());
  }

  TEST_CASE("remove_precede_task removes edge from both sides") {
    auto a = GraphTask::create("A", [] {});
    auto b = GraphTask::create("B", [] {});

    b->precede(a);
    REQUIRE_EQ(b->get_succeed_task_list().size(), 1u);

    b->remove_precede_task(a);
    CHECK(b->get_succeed_task_list().empty());
  }

  TEST_CASE("cancel sets status to kStatusInActive without executing callback") {
    std::atomic<int> executed{0};
    auto task = GraphTask::create("cancelable", [&] { executed.store(1); });

    task->cancel();
    CHECK_EQ(task->get_status(), GraphTask::kStatusInActive);
    CHECK_EQ(executed.load(), 0);
  }

  TEST_CASE("export_to_dot is non-empty and contains digraph keyword") {
    auto a = GraphTask::create("node_alpha", [] {});
    auto b = GraphTask::create("node_beta", [] {});
    a->succeed(b);

    std::string dot = b->export_to_dot();
    CHECK_FALSE(dot.empty());
    CHECK(dot.find("digraph") != std::string::npos);
    CHECK(dot.find("node_alpha") != std::string::npos);
    CHECK(dot.find("node_beta") != std::string::npos);
  }

  TEST_CASE("export_to_dot for single node is non-empty") {
    auto task = GraphTask::create("standalone", [] {});
    CHECK_FALSE(task->export_to_dot().empty());
  }

  TEST_CASE("single task executes and transitions to done") {
    auto ran = std::make_shared<std::atomic<int>>(0);
    auto task = GraphTask::create("solo", [ran] { ran->store(1); });

    run_graph(task);

    CHECK_EQ(ran->load(), 1);
  }

  TEST_CASE("linear chain a->b->c executes in topological order") {
    auto order = std::make_shared<std::vector<int>>();
    auto mtx = std::make_shared<std::mutex>();

    auto ta = GraphTask::create("A", [order, mtx] {
      std::lock_guard lock(*mtx);
      order->push_back(0);
    });
    auto tb = GraphTask::create("B", [order, mtx] {
      std::lock_guard lock(*mtx);
      order->push_back(1);
    });
    auto tc = GraphTask::create("C", [order, mtx] {
      std::lock_guard lock(*mtx);
      order->push_back(2);
    });

    ta->precede(tb);
    tb->precede(tc);

    run_graph(ta);

    REQUIRE_EQ(order->size(), 3u);
    CHECK_EQ((*order)[0], 0);
    CHECK_EQ((*order)[1], 1);
    CHECK_EQ((*order)[2], 2);
  }

  TEST_CASE("fan-out a->b and a->c executes both children after a") {
    auto a_done = std::make_shared<std::atomic<int>>(0);
    auto b_done = std::make_shared<std::atomic<int>>(0);
    auto c_done = std::make_shared<std::atomic<int>>(0);

    auto ta = GraphTask::create("A", [a_done] { a_done->store(1); });
    auto tb = GraphTask::create("B", [a_done, b_done] {
      CHECK_EQ(a_done->load(), 1);
      b_done->store(1);
    });
    auto tc = GraphTask::create("C", [a_done, c_done] {
      CHECK_EQ(a_done->load(), 1);
      c_done->store(1);
    });

    ta->precede(tb);
    ta->precede(tc);

    run_graph(ta);

    CHECK_EQ(a_done->load(), 1);
    CHECK_EQ(b_done->load(), 1);
    CHECK_EQ(c_done->load(), 1);
  }

  TEST_CASE("kPolicyWaitAll join fires exactly once after all predecessors") {
    auto fired = std::make_shared<std::atomic<int>>(0);

    auto root = GraphTask::create("root", [] {});
    auto p1 = GraphTask::create("p1", [] {});
    auto p2 = GraphTask::create("p2", [] {});
    auto p3 = GraphTask::create("p3", [] {});
    auto sink = GraphTask::create("sink", [fired] { fired->fetch_add(1); });

    sink->set_policy(GraphTask::kPolicyWaitAll);
    root->precede(p1);
    root->precede(p2);
    root->precede(p3);
    p1->precede(sink);
    p2->precede(sink);
    p3->precede(sink);

    run_graph(root, 100);

    CHECK_EQ(sink->get_status(), GraphTask::kStatusDone);
    CHECK_EQ(fired->load(), 1);
  }

  TEST_CASE("diamond DAG executes all four tasks") {
    auto a_ran = std::make_shared<std::atomic<int>>(0);
    auto b_ran = std::make_shared<std::atomic<int>>(0);
    auto c_ran = std::make_shared<std::atomic<int>>(0);
    auto d_ran = std::make_shared<std::atomic<int>>(0);

    auto ta = GraphTask::create("A", [a_ran] { a_ran->store(1); });
    auto tb = GraphTask::create("B", [b_ran] { b_ran->store(1); });
    auto tc = GraphTask::create("C", [c_ran] { c_ran->store(1); });
    auto td = GraphTask::create("D", [d_ran] { d_ran->store(1); });

    td->set_policy(GraphTask::kPolicyWaitAll);
    ta->precede(tb);
    ta->precede(tc);
    tb->precede(td);
    tc->precede(td);

    run_graph(ta, 100);

    CHECK_EQ(a_ran->load(), 1);
    CHECK_EQ(b_ran->load(), 1);
    CHECK_EQ(c_ran->load(), 1);
    CHECK_EQ(d_ran->load(), 1);
  }

  TEST_CASE("condition branch 0 selected when callback returns 0") {
    auto branch0 = std::make_shared<std::atomic<int>>(0);
    auto branch1 = std::make_shared<std::atomic<int>>(0);

    auto cond = GraphTask::create_condition("cond", [] { return 0; }, 2);
    auto b0 = GraphTask::create("b0", [branch0] { branch0->store(1); });
    auto b1 = GraphTask::create("b1", [branch1] { branch1->store(1); });

    b1->set_condition_number(1);
    cond->precede(b0);
    cond->precede(b1);

    run_graph(cond);

    CHECK_EQ(branch0->load(), 1);
    CHECK_EQ(branch1->load(), 0);
  }

  TEST_CASE("condition branch 1 selected when callback returns 1") {
    auto branch0 = std::make_shared<std::atomic<int>>(0);
    auto branch1 = std::make_shared<std::atomic<int>>(0);

    auto cond = GraphTask::create_condition("cond", [] { return 1; }, 2);
    auto b0 = GraphTask::create("b0", [branch0] { branch0->store(1); });
    auto b1 = GraphTask::create("b1", [branch1] { branch1->store(1); });

    b1->set_condition_number(1);
    cond->precede(b0);
    cond->precede(b1);

    run_graph(cond);

    CHECK_EQ(branch0->load(), 0);
    CHECK_EQ(branch1->load(), 1);
  }

  TEST_CASE("condition out-of-range index skips all branches") {
    auto branch0 = std::make_shared<std::atomic<int>>(0);
    auto branch1 = std::make_shared<std::atomic<int>>(0);

    auto cond = GraphTask::create_condition("cond", [] { return 99; }, 2);
    auto b0 = GraphTask::create("b0", [branch0] { branch0->store(1); });
    auto b1 = GraphTask::create("b1", [branch1] { branch1->store(1); });

    b1->set_condition_number(1);
    cond->precede(b0);
    cond->precede(b1);

    run_graph(cond);

    CHECK_EQ(branch0->load(), 0);
    CHECK_EQ(branch1->load(), 0);
  }

  TEST_CASE("throwing condition callback gracefully disables all branches") {
    auto b0_ran = std::make_shared<std::atomic<int>>(0);
    auto b1_ran = std::make_shared<std::atomic<int>>(0);

    auto cond = GraphTask::create_condition("cond", []() -> int { throw std::runtime_error("boom"); }, 2);
    auto b0 = GraphTask::create("b0", [b0_ran] { b0_ran->store(1); });
    auto b1 = GraphTask::create("b1", [b1_ran] { b1_ran->store(1); });

    b1->set_condition_number(1);
    cond->precede(b0);
    cond->precede(b1);

    run_graph(cond);

    CHECK_EQ(cond->get_status(), GraphTask::kStatusDone);
    CHECK_EQ(b0_ran->load(), 0);
    CHECK_EQ(b1_ran->load(), 0);
  }

  TEST_CASE("operator --> builds chain with correct execution order") {
    auto order = std::make_shared<std::vector<int>>();
    auto mtx = std::make_shared<std::mutex>();

    auto ta = GraphTask::create("A", [order, mtx] {
      std::lock_guard lock(*mtx);
      order->push_back(0);
    });
    auto tb = GraphTask::create("B", [order, mtx] {
      std::lock_guard lock(*mtx);
      order->push_back(1);
    });
    auto tc = GraphTask::create("C", [order, mtx] {
      std::lock_guard lock(*mtx);
      order->push_back(2);
    });

    ta-- > tb-- > tc;  // NOLINT(bugprone-chained-comparison)

    run_graph(ta);

    REQUIRE_EQ(order->size(), 3u);
    CHECK_EQ((*order)[0], 0);
    CHECK_EQ((*order)[1], 1);
    CHECK_EQ((*order)[2], 2);
  }

  TEST_CASE("status transitions pending->running->done are observed") {
    auto statuses = std::make_shared<std::vector<GraphTask::Status>>();
    auto mtx = std::make_shared<std::mutex>();
    auto inside = std::make_shared<std::atomic<bool>>(false);
    auto release = std::make_shared<std::atomic<bool>>(false);

    auto task = GraphTask::create("status_track", [inside, release] {
      inside->store(true);
      while (!release->load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }
    });

    task->register_status_callback([statuses, mtx](const std::string&, GraphTask::Status s) {
      std::lock_guard lock(*mtx);
      statuses->push_back(s);
    });

    task->execute(&shared_pool());

    while (!inside->load()) {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    GraphTask::Status mid = task->get_status();
    release->store(true);

    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (task->get_status() != GraphTask::kStatusDone && std::chrono::steady_clock::now() < deadline) {
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }

    CHECK_EQ(mid, GraphTask::kStatusRunning);
    CHECK_EQ(task->get_status(), GraphTask::kStatusDone);

    std::lock_guard lock(*mtx);
    bool has_running = false;
    bool has_done = false;
    for (auto s : *statuses) {
      if (s == GraphTask::kStatusRunning) {
        has_running = true;
      }
      if (s == GraphTask::kStatusDone) {
        has_done = true;
      }
    }
    CHECK(has_running);
    CHECK(has_done);
  }

  TEST_CASE("register_status_callback returns unique non-zero ids") {
    auto task = GraphTask::create("uniq", [] {});
    std::unordered_set<uint32_t> ids;

    for (int i = 0; i < 256; ++i) {
      uint32_t id = task->register_status_callback([](const std::string&, GraphTask::Status) {});
      REQUIRE_NE(id, 0u);
      CHECK(ids.insert(id).second);
    }

    for (uint32_t id : ids) {
      CHECK(task->unregister_status_callback(id));
    }
  }

  TEST_CASE("register_status_callback returns 0 for empty callback") {
    auto task = GraphTask::create("empty_cb", [] {});
    GraphTask::StatusCallback empty;
    uint32_t id = task->register_status_callback(std::move(empty));
    CHECK_EQ(id, 0u);
  }

  TEST_CASE("unregister_status_callback removes specific subscriber") {
    auto subA = std::make_shared<std::atomic<int>>(0);
    auto subB = std::make_shared<std::atomic<int>>(0);
    auto task = GraphTask::create("unreg", [] {});

    uint32_t id_a =
        task->register_status_callback([subA](const std::string&, GraphTask::Status) { subA->fetch_add(1); });
    task->register_status_callback([subB](const std::string&, GraphTask::Status) { subB->fetch_add(1); });

    CHECK(task->unregister_status_callback(id_a));
    CHECK_FALSE(task->unregister_status_callback(id_a));

    run_graph(task);

    CHECK_EQ(subA->load(), 0);
    CHECK(subB->load() >= 1);
  }

  TEST_CASE("multiple status subscribers all fire") {
    auto subA = std::make_shared<std::atomic<int>>(0);
    auto subB = std::make_shared<std::atomic<int>>(0);
    auto task = GraphTask::create("multi_sub", [] {});

    task->register_status_callback([subA](const std::string&, GraphTask::Status) { subA->fetch_add(1); });
    task->register_status_callback([subB](const std::string&, GraphTask::Status) { subB->fetch_add(1); });

    run_graph(task);

    CHECK(subA->load() >= 1);
    CHECK_EQ(subA->load(), subB->load());
  }

  TEST_CASE("throwing task callback does not block downstream task") {
    auto downstream_ran = std::make_shared<std::atomic<int>>(0);
    auto a = GraphTask::create("A", [] { throw std::runtime_error("boom"); });
    auto b = GraphTask::create("B", [downstream_ran] { downstream_ran->store(1); });

    a->precede(b);
    run_graph(a);

    CHECK_EQ(a->get_status(), GraphTask::kStatusDone);
    CHECK_EQ(downstream_ran->load(), 1);
  }

  TEST_CASE("throwing status callback does not prevent remaining subscribers") {
    auto throwing_calls = std::make_shared<std::atomic<int>>(0);
    auto safe_calls = std::make_shared<std::atomic<int>>(0);
    auto task = GraphTask::create("throw_sub", [] {});

    task->register_status_callback([throwing_calls](const std::string&, GraphTask::Status) {
      throwing_calls->fetch_add(1);
      throw std::runtime_error("boom");
    });
    task->register_status_callback([safe_calls](const std::string&, GraphTask::Status) { safe_calls->fetch_add(1); });

    run_graph(task);

    CHECK_EQ(task->get_status(), GraphTask::kStatusDone);
    CHECK(throwing_calls->load() >= 1);
    CHECK_EQ(safe_calls->load(), throwing_calls->load());
  }

  TEST_CASE("execute cancels task when engine rejects post") {
    auto task = GraphTask::create("reject_post", [] {});
    RejectEngine engine;

    task->execute(&engine);

    CHECK_EQ(task->get_status(), GraphTask::kStatusInActive);
  }

  TEST_CASE("cancel cascades to reachable successors") {
    auto a = GraphTask::create("a", [] {});
    auto b = GraphTask::create("b", [] {});
    auto c = GraphTask::create("c", [] {});

    a->precede(b);
    b->precede(c);

    a->cancel();

    CHECK_EQ(a->get_status(), GraphTask::kStatusInActive);
    CHECK_EQ(b->get_status(), GraphTask::kStatusInActive);
    CHECK_EQ(c->get_status(), GraphTask::kStatusInActive);
  }
}

// NOLINTEND
