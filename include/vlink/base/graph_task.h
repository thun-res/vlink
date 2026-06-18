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

/**
 * @file graph_task.h
 * @brief Directed acyclic task graph with condition branching, cycle guard and DOT export.
 *
 * @details
 * @c GraphTask is VLink's data-flow primitive: every node carries a callback, a list of
 * predecessors and a list of successors, and is submitted to any engine that exposes
 * @c post_task / @c post_task_with_priority -- @c MessageLoop, @c MultiLoop or @c ThreadPool.
 * Edges are declared with @c precede / @c succeed (Taskflow convention) and a process-wide
 * topology mutex serialises all mutators across every graph instance.
 *
 * @par DAG diagram
 *
 * @verbatim
 *               +---+   precede   +---+   precede   +---+
 *               | A | ----------> | B | ----------> | C |
 *               +-+-+             +-+-+             +-+-+
 *                 |     condition   |                 |
 *                 v                 v                 v
 *              +-----+           +-----+           +-----+
 *              | D0  |           | E   |           | F   |
 *              +-----+           +-----+           +-----+
 *                 ^
 *                 |  D0/D1/...   condition task selects which successor branch fires
 *              +-----+
 *              | D1  |
 *              +-----+
 * @endverbatim
 *
 * @par Task factories
 *
 * | Factory                       | Callback signature  | Use case                              |
 * | ----------------------------- | ------------------- | ------------------------------------- |
 * | @c create(callback)           | @c void()           | Regular work node                     |
 * | @c create_condition(callback) | @c int()            | Branch selector (return value picks)  |
 *
 * @par Execution policies
 *
 * | Policy             | Behaviour                                                          |
 * | ------------------ | ------------------------------------------------------------------ |
 * | @c kPolicyOnce     | Runs exactly once per @c execute pass (default)                    |
 * | @c kPolicyMultiple | Runs multiple times within one @c execute pass                     |
 * | @c kPolicyWaitAll  | Waits for every predecessor before running                         |
 *
 * @par Example
 * @code
 *   vlink::MultiLoop engine(4);
 *   engine.async_run();
 *
 *   auto load = vlink::GraphTask::create("load", [] { load_data(); });
 *   auto proc = vlink::GraphTask::create("proc", [] { process();   });
 *   auto save = vlink::GraphTask::create("save", [] { save_data(); });
 *
 *   // load -- > proc means load->precede(proc); produces execution order load, proc, save.
 *   load-- > proc-- > save;
 *
 *   assert(!load->has_cycle());
 *   load->execute(&engine);
 * @endcode
 */

#pragma once

#include <memory>
#include <string>
#include <type_traits>
#include <unordered_set>
#include <utility>
#include <vector>

#include "./functional.h"
#include "./macros.h"
#include "./traits.h"

namespace vlink {

/**
 * @class GraphTask
 * @brief DAG node carrying a callback, predecessor and successor links, and run-time state.
 *
 * @details
 * Must be created through the @c create / @c create_condition factories so callbacks share
 * @c std::enable_shared_from_this safely.  @c execute traverses the reachable sub-graph and
 * submits ready tasks to a user-supplied engine.
 */
class VLINK_EXPORT GraphTask final : public std::enable_shared_from_this<GraphTask> {
 public:
  /**
   * @brief Run-time execution state of a node within an @c execute pass.
   */
  enum Status : uint8_t {
    kStatusInActive = 0,  ///< Not yet submitted or cancelled.
    kStatusPending = 1,   ///< Waiting for predecessors to complete.
    kStatusRunning = 2,   ///< Currently executing.
    kStatusDone = 3,      ///< Execution finished.
  };

  /**
   * @brief Policy controlling how many times the task may run within a pass.
   */
  enum Policy : uint8_t {
    kPolicyOnce = 0,      ///< Run exactly once per @c execute pass (default).
    kPolicyMultiple = 1,  ///< Allow multiple invocations within the pass.
    kPolicyWaitAll = 2,   ///< Wait for every predecessor before running.
  };

  /**
   * @brief Callback type for void-returning work nodes.
   */
  using Callback = MoveFunction<void()>;

  /**
   * @brief Callback type for condition nodes; the return value selects an outgoing branch.
   */
  using ConditionCallback = MoveFunction<int()>;

  /**
   * @brief Status-change notification callback.
   *
   * @details
   * Invoked with the task name and the new status whenever the node transitions.
   */
  using StatusCallback = MoveFunction<void(const std::string&, Status)>;

  /**
   * @brief Creates a regular work node.
   *
   * @param callback          Work function.
   * @param condition_number  Number of outgoing branches (@c 0 disables branching).
   * @return Shared pointer to the new task.
   */
  [[nodiscard]] static std::shared_ptr<GraphTask> create(Callback&& callback, int condition_number = 0);

  /**
   * @brief Creates a named regular work node.
   *
   * @param name              Node name used in DOT output and status callbacks.
   * @param callback          Work function.
   * @param condition_number  Number of outgoing branches.
   * @return Shared pointer to the new task.
   */
  [[nodiscard]] static std::shared_ptr<GraphTask> create(const std::string& name, Callback&& callback,
                                                         int condition_number = 0);

  /**
   * @brief Creates a condition node whose return value selects a successor branch.
   *
   * @param callback          Predicate returning the branch index.
   * @param condition_number  Number of branches accepted; out-of-range returns skip all successors.
   * @return Shared pointer to the new condition task.
   */
  [[nodiscard]] static std::shared_ptr<GraphTask> create_condition(ConditionCallback&& callback,
                                                                   int condition_number = 0);

  /**
   * @brief Creates a named condition node.
   *
   * @param name              Node name.
   * @param callback          Predicate returning the branch index.
   * @param condition_number  Branch count.
   * @return Shared pointer to the new condition task.
   */
  [[nodiscard]] static std::shared_ptr<GraphTask> create_condition(const std::string& name,
                                                                   ConditionCallback&& callback,
                                                                   int condition_number = 0);

  /**
   * @brief Submits the reachable sub-graph to @p graph_engine.
   *
   * @details
   * Traverses the sub-graph, identifies ready nodes and posts them via @c post_task or
   * @c post_task_with_priority when available.  Compatible with @c MessageLoop, @c MultiLoop and
   * @c ThreadPool.
   *
   * @tparam GraphEngineT  Engine type exposing @c post_task and optionally @c post_task_with_priority.
   * @param graph_engine   Target engine instance.
   */
  template <class GraphEngineT>
  void execute(GraphEngineT* graph_engine);

  /**
   * @brief Cancels this node and propagates the cancellation downstream.
   *
   * @details
   * Sets this node's status to @c kStatusInActive and walks the successor list forward marking
   * every reachable node inactive.  Predecessors are unaffected.  Inactive nodes are skipped
   * by the engine and by downstream @c kPolicyWaitAll counters.
   */
  void cancel();

  /**
   * @brief Declares that this node must complete before @p task starts.
   *
   * @details
   * Runs a reachability pre-check on @p task 's downstream cone before mutating any list; the
   * edge is rejected (with an error log) if it would form a cycle.  On success @p task is
   * appended to this node's successor list and this node is appended to @p task 's predecessor
   * list.  Topology mutation is serialised by a process-wide recursive mutex shared across
   * every @c GraphTask instance, so concurrent writers are safe; read paths (@c execute,
   * @c has_cycle, @c export_to_dot) read per-node snapshots without taking that mutex.
   *
   * @param task  Successor node.
   */
  void precede(const std::shared_ptr<GraphTask>& task);

  /**
   * @brief Declares that @p task must complete before this node starts.
   *
   * @details
   * Mirror of @c precede; rejects edges that would form a cycle.  Shares the same single-writer
   * topology mutex as @c precede.
   *
   * @param task  Predecessor node.
   */
  void succeed(const std::shared_ptr<GraphTask>& task);

  /**
   * @brief Subscribes a callback to status transitions on this node.
   *
   * @details
   * Each call appends a new subscriber.  On every status change the node snapshots the current
   * subscriber set under @c status_callbacks_mtx, releases the lock, then invokes the snapshot
   * in unspecified order.  Because callbacks fire without the mutex held they may freely
   * register / unregister callbacks on the same node; such mutations apply on the next
   * transition.  Exceptions are caught and logged; remaining subscribers still fire.
   *
   * @param callback  Status change callback.
   * @return Subscription id (>0).  Returns @c 0 when @p callback is empty.
   */
  uint32_t register_status_callback(StatusCallback&& callback);

  /**
   * @brief Removes a previously registered status callback by id.
   *
   * @param id  Subscription id returned by @c register_status_callback.
   * @return @c true when the subscription was found and removed.
   */
  bool unregister_status_callback(uint32_t id);

  /**
   * @brief Removes every status callback subscription on this node.
   */
  void clear_status_callbacks();

  /**
   * @brief Sets the node name used in DOT output and status callbacks.
   *
   * @param name  Node name.
   */
  void set_name(const std::string& name);

  /**
   * @brief Sets a group name used to visually cluster nodes in DOT output.
   *
   * @param name  Group name.
   */
  void set_group_name(const std::string& name);

  /**
   * @brief Sets the number of outgoing condition branches.
   *
   * @param condition_number  Branch count.
   */
  void set_condition_number(int condition_number);

  /**
   * @brief Sets the dispatch priority used by priority-aware engines.
   *
   * @param priority  Priority value.
   */
  void set_priority(uint16_t priority);

  /**
   * @brief Sets the maximum recursion depth that bounds DFS traversals.
   *
   * @details
   * Traversals exceeding this depth treat the graph as conservatively cyclic.  Default: @c 10000.
   *
   * @param depth  Maximum recursion depth.
   */
  void set_max_recursion_depth(uint32_t depth);

  /**
   * @brief Sets the execution policy for this node.
   *
   * @param policy  Policy enumerator.
   */
  void set_policy(Policy policy);

  /**
   * @brief Returns the node name.
   *
   * @return Node name.
   */
  [[nodiscard]] std::string get_name() const;

  /**
   * @brief Returns the group name.
   *
   * @return Group name.
   */
  [[nodiscard]] std::string get_group_name() const;

  /**
   * @brief Returns the configured branch count.
   *
   * @return Branch count.
   */
  [[nodiscard]] int get_condition_number() const;

  /**
   * @brief Returns the configured dispatch priority.
   *
   * @return Priority value.
   */
  [[nodiscard]] uint16_t get_priority() const;

  /**
   * @brief Returns the maximum recursion depth used for cycle detection.
   *
   * @return Recursion depth bound.
   */
  [[nodiscard]] uint32_t get_max_recursion_depth() const;

  /**
   * @brief Returns the configured execution policy.
   *
   * @return Policy enumerator.
   */
  [[nodiscard]] Policy get_policy() const;

  /**
   * @brief Returns the current execution status of this node.
   *
   * @return Status enumerator.
   */
  [[nodiscard]] Status get_status() const;

  /**
   * @brief Removes an outgoing edge created by @c precede.
   *
   * @details
   * Removes @p task from the successor list and removes this node from @p task 's predecessor
   * list.  Logs an error when the edge does not exist.
   *
   * @param task  Previously attached successor.
   */
  void remove_precede_task(const std::shared_ptr<GraphTask>& task);

  /**
   * @brief Removes an incoming edge created by @c succeed.
   *
   * @param task  Previously attached predecessor.
   */
  void remove_succeed_task(const std::shared_ptr<GraphTask>& task);

  /**
   * @brief Returns the current predecessor list as weak pointers.
   *
   * @return Vector of weak predecessor pointers.
   */
  [[nodiscard]] std::vector<std::weak_ptr<GraphTask>> get_precede_task_list() const;

  /**
   * @brief Returns the current successor list as weak pointers.
   *
   * @return Vector of weak successor pointers.
   */
  [[nodiscard]] std::vector<std::weak_ptr<GraphTask>> get_succeed_task_list() const;

  /**
   * @brief Reports whether this node was created via @c create_condition.
   *
   * @return @c true for condition nodes.
   */
  [[nodiscard]] bool is_condition_task() const;

  /**
   * @brief Detects whether the reachable sub-graph contains a cycle.
   *
   * @details
   * Iterative DFS with a recursion stack.  Acquires per-node mutexes one at a time and does
   * not take the global topology mutex; safe to call from status callbacks fired by @c invoke
   * or @c cancel.
   *
   * @return @c true when a cycle is found or the recursion bound is exceeded.
   */
  [[nodiscard]] bool has_cycle() const;

  /**
   * @brief Exports the reachable sub-graph as a Graphviz DOT document.
   *
   * @return DOT source string.
   */
  [[nodiscard]] std::string export_to_dot() const;

 protected:
  using FindTaskCallback = MoveFunction<void(const std::shared_ptr<GraphTask>&)>;

  explicit GraphTask(Callback&& callback, int condition_number);

  explicit GraphTask(const std::string& name, Callback&& callback, int condition_number);

  explicit GraphTask(ConditionCallback&& callback, int condition_number);

  explicit GraphTask(const std::string& name, ConditionCallback&& callback, int condition_number);

  ~GraphTask();

  void process_and_traverse(FindTaskCallback&& callback);

 private:
  int invoke(bool once);

  void wait();

  void notify(int condition_number);

  void notify_skip();

  bool mark_predecessor_satisfied(bool active, bool* has_active);

  void update_status(Status status);

  bool detect_cycle(const GraphTask* task, std::unordered_set<const GraphTask*>& visited,
                    std::unordered_set<const GraphTask*>& recursion_stack, uint32_t& depth, uint32_t max_depth) const;

  bool reaches_via_successors(const GraphTask* start_node,
                              const std::vector<std::weak_ptr<GraphTask>>& start_successors,
                              const GraphTask* target) const;

  static void clear_invalid_task(const std::shared_ptr<GraphTask>& task);

  struct Impl;
  std::unique_ptr<Impl> impl_;

  VLINK_DISALLOW_COPY_AND_ASSIGN(GraphTask)
};

/**
 * @typedef GraphTaskPtr
 * @brief Convenience alias for the canonical @c shared_ptr<GraphTask> handle.
 */
using GraphTaskPtr = std::shared_ptr<GraphTask>;

////////////////////////////////////////////////////////////////
/// Details
////////////////////////////////////////////////////////////////

template <class GraphEngineT>
inline void GraphTask::execute(GraphEngineT* graph_engine) {
  auto self = shared_from_this();

  process_and_traverse([self, graph_engine](const std::shared_ptr<GraphTask>& task) {
    constexpr bool kHaspriority = VLINK_HAS_MEMBER(GraphEngineT, post_task_with_priority);
    [[maybe_unused]] constexpr uint8_t kPriorityType = 2;

    if VUNLIKELY (task->get_status() == kStatusInActive) {
      return;
    }

    auto task_func = [self, task]() {
      if VLIKELY (task.get() != self.get()) {
        task->wait();
      }

      int ret = task->invoke(true);

      if VLIKELY (ret >= 0) {
        task->notify(ret);
      }
    };

    auto post_task = [graph_engine](auto&& func) -> bool {
      using Ret = decltype(graph_engine->post_task(std::forward<decltype(func)>(func)));

      if constexpr (std::is_same_v<Ret, bool>) {
        return graph_engine->post_task(std::forward<decltype(func)>(func));
      } else {
        graph_engine->post_task(std::forward<decltype(func)>(func));
        return true;
      }
    };

    bool posted = false;

    if constexpr (kHaspriority) {
      auto post_task_with_priority = [graph_engine, task](auto&& func) -> bool {
        using Ret =
            decltype(graph_engine->post_task_with_priority(std::forward<decltype(func)>(func), task->get_priority()));

        if constexpr (std::is_same_v<Ret, bool>) {
          return graph_engine->post_task_with_priority(std::forward<decltype(func)>(func), task->get_priority());
        } else {
          graph_engine->post_task_with_priority(std::forward<decltype(func)>(func), task->get_priority());
          return true;
        }
      };

      if constexpr (VLINK_HAS_MEMBER(GraphEngineT, get_type)) {
        if (graph_engine->get_type() == kPriorityType) {
          posted = post_task_with_priority(std::move(task_func));
        } else {
          posted = post_task(std::move(task_func));
        }
      } else {
        posted = post_task(std::move(task_func));
      }
    } else {
      posted = post_task(std::move(task_func));
    }

    if VUNLIKELY (!posted) {
      task->cancel();
    }
  });
}

[[maybe_unused]] static inline GraphTaskPtr& operator--(GraphTaskPtr& task, int) { return task; }

[[maybe_unused]] static inline GraphTaskPtr& operator>(GraphTaskPtr& task, GraphTaskPtr& target_task) {
  task->precede(target_task);
  return target_task;
}

[[maybe_unused]] static inline GraphTaskPtr& operator<(GraphTaskPtr& task, GraphTaskPtr& target_task) {
  task->succeed(target_task);
  return target_task;
}

[[maybe_unused]] static inline GraphTaskPtr& operator--(GraphTaskPtr& task) { return task; }

}  // namespace vlink
