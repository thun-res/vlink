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

#include <vlink/base/graph_task.h>
#include <vlink/base/logger.h>
#include <vlink/base/multi_loop.h>

#include <chrono>
#include <string>
#include <thread>

#include "task_graph_builder.h"

using namespace std::chrono_literals;  // NOLINT(build/namespaces, google-build-using-namespace)

// -----------------------------------------------------------------------------
// GraphTask example
//
// Module:   vlink/base/graph_task.h (+ multi_loop.h for the worker pool)
// Scenario: GraphTask models a DAG of work units. Each node carries a
//           callable; edges (set via precede()) declare ordering. The
//           scheduler dispatches ready nodes to a MultiLoop engine; nodes
//           with no remaining predecessors run in parallel automatically.
//           Policies kPolicyWaitAll / kPolicyWaitAny control how a node
//           with multiple inputs treats them. Conditional nodes return an
//           int that selects which outgoing edge fires.
// Caveats:  - has_cycle() must be false before execute() -- a cycle yields
//             undefined behaviour.
//           - The engine must be running (async_run / run) when execute()
//             is called; wait_for_idle() then drains all reachable nodes.
//           - Status callbacks are dispatched on the worker thread that
//             transitioned the node -- do not perform blocking work in them.
// -----------------------------------------------------------------------------
int main() {
  // Linear DAG (A -> B -> C): sequential pipeline. Drains identically on
  // any engine width because there is never more than one ready node.
  {
    VLOG_I("=== Linear DAG (A -> B -> C) ===");
    vlink::MultiLoop engine(4);
    engine.async_run();

    auto root = task_graph_builder::build_linear_dag();
    MLOG_I("  has_cycle={}", root->has_cycle());
    root->execute(&engine);
    engine.wait_for_idle();

    engine.quit();
    engine.wait_for_quit();
  }

  // Diamond DAG: A -> {B,C} -> D. B and C run concurrently on two workers;
  // D uses kPolicyWaitAll (the default for multi-input nodes) to fire only
  // after BOTH predecessors complete.
  {
    VLOG_I("=== Diamond DAG ===");
    vlink::MultiLoop engine(4);
    engine.async_run();

    auto root = task_graph_builder::build_diamond_dag();
    root->execute(&engine);
    engine.wait_for_idle();

    engine.quit();
    engine.wait_for_quit();
  }

  // Conditional DAG: the Branch node returns an int that picks which
  // outgoing edge fires (0 -> Path0, 1 -> Path1). The unchosen path is
  // skipped entirely -- Finish still runs because at least one of its
  // predecessors completed.
  {
    VLOG_I("=== Conditional branching ===");
    vlink::MultiLoop engine(4);
    engine.async_run();

    auto root = task_graph_builder::build_conditional_dag(1);
    root->execute(&engine);
    engine.wait_for_idle();

    engine.quit();
    engine.wait_for_quit();
  }

  // Status callback: fires every time the node transitions through one of
  // the four states. Useful for live dashboards / progress reporting.
  // Callback runs on the worker that performed the transition; keep it
  // cheap and non-blocking.
  {
    VLOG_I("=== Status callback ===");
    vlink::MultiLoop engine(2);
    engine.async_run();

    auto task = vlink::GraphTask::create("Monitored", []() {
      VLOG_I("  task body running");
      std::this_thread::sleep_for(30ms);
    });

    task->register_status_callback([](const std::string& name, vlink::GraphTask::Status status) {
      const char* s = "?";
      switch (status) {
        case vlink::GraphTask::kStatusInActive:
          s = "InActive";
          break;
        case vlink::GraphTask::kStatusPending:
          s = "Pending";
          break;
        case vlink::GraphTask::kStatusRunning:
          s = "Running";
          break;
        case vlink::GraphTask::kStatusDone:
          s = "Done";
          break;
      }

      MLOG_I("  [status] {} -> {}", name, s);
    });

    task->execute(&engine);
    engine.wait_for_idle();

    engine.quit();
    engine.wait_for_quit();
  }

  // export_to_dot serialises the DAG into Graphviz DOT format -- pipe it to
  // `dot -Tpng` for a visual rendering of complex pipelines.
  {
    VLOG_I("=== DOT export ===");
    auto root = task_graph_builder::build_pipeline_dag();
    MLOG_I("  DOT:\n{}", root->export_to_dot());
  }

  VLOG_I("GraphTask example finished.");
  return 0;
}
