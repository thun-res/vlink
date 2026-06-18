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

#pragma once

#include <vlink/base/graph_task.h>
#include <vlink/base/logger.h>

#include <thread>

// -----------------------------------------------------------------------------
// task_graph_builder: small library of canned DAGs shared by the GraphTask
// example. Kept in a header so we can also reuse the shapes from tests or
// other demos without dragging a separate translation unit.
// -----------------------------------------------------------------------------
namespace task_graph_builder {

// A -> B -> C linear chain. The operator overload `a-- > b` is the project's
// shorthand for `a->precede(b)` -- the chained form returns the right-hand
// node so a second `-- >` cascades naturally.
inline vlink::GraphTaskPtr build_linear_dag() {
  auto a = vlink::GraphTask::create("A", []() { VLOG_I("  A: load"); });
  auto b = vlink::GraphTask::create("B", []() { VLOG_I("  B: process"); });
  auto c = vlink::GraphTask::create("C", []() { VLOG_I("  C: save"); });
  a-- > b-- > c;
  return a;
}

// Diamond: A fans out to B and C in parallel, both feed D. D is marked
// kPolicyWaitAll so it only fires after BOTH B and C complete. The sleeps
// make the parallel section observable in the log.
inline vlink::GraphTaskPtr build_diamond_dag() {
  auto a = vlink::GraphTask::create("A", []() { VLOG_I("  A: init"); });
  auto b = vlink::GraphTask::create("B", []() {
    VLOG_I("  B: parallel #1");
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  });
  auto c = vlink::GraphTask::create("C", []() {
    VLOG_I("  C: parallel #2");
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
  });
  auto d = vlink::GraphTask::create("D", []() { VLOG_I("  D: merge"); });

  a->precede(b);
  a->precede(c);
  b->precede(d);
  c->precede(d);

  d->set_policy(vlink::GraphTask::kPolicyWaitAll);
  return a;
}

// Conditional branch: the Branch node is created with create_condition; its
// return value selects which outgoing edge fires. Path0 and Path1 are the
// candidates; Finish always runs because it has at least one upstream branch.
inline vlink::GraphTaskPtr build_conditional_dag(int condition_value) {
  auto start = vlink::GraphTask::create("Start", []() { VLOG_I("  Start"); });
  auto branch = vlink::GraphTask::create_condition("Branch", [condition_value]() -> int {
    MLOG_I("  Branch -> path {}", condition_value);
    return condition_value;
  });
  auto path_0 = vlink::GraphTask::create("Path0", []() { VLOG_I("  Path 0 (branch returned 0)"); });
  auto path_1 = vlink::GraphTask::create("Path1", []() { VLOG_I("  Path 1 (branch returned 1)"); });
  auto finish = vlink::GraphTask::create("Finish", []() { VLOG_I("  Finish"); });

  start->precede(branch);
  branch->precede(path_0);
  branch->precede(path_1);
  path_0->precede(finish);
  path_1->precede(finish);
  return start;
}

// Pipeline shape used by the DOT export demo -- a slightly richer graph so
// the export shows both fan-out (b -> c, b -> d) and fan-in (c, d -> e).
inline vlink::GraphTaskPtr build_pipeline_dag() {
  auto a = vlink::GraphTask::create("Load", []() {});
  auto b = vlink::GraphTask::create("Parse", []() {});
  auto c = vlink::GraphTask::create("Validate", []() {});
  auto d = vlink::GraphTask::create("Transform", []() {});
  auto e = vlink::GraphTask::create("Save", []() {});

  a-- > b-- > c;
  b-- > d;
  c-- > e;
  d-- > e;
  return a;
}

}  // namespace task_graph_builder
