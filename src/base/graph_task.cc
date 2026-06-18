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

#include "./base/graph_task.h"

#include <algorithm>
#include <atomic>
#include <limits>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <sstream>
#include <stack>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "./base/condition_variable.h"
#include "./base/helpers.h"
#include "./base/logger.h"
#include "./base/memory_pool.h"
#include "./base/memory_resource.h"

namespace vlink {

static std::atomic<uint32_t> global_graph_task_count = 0;

static std::recursive_mutex& topology_mutex() {
  static std::recursive_mutex mtx;
  return mtx;
}

// GraphTask::Impl
struct GraphTask::Impl final {  // NOLINT(clang-analyzer-optin.performance.Padding)
  alignas(64) std::atomic<size_t> pending_index{0};
  alignas(64) std::atomic<size_t> active_index{0};
  alignas(64) std::atomic<bool> is_ready{false};
  alignas(64) std::atomic<bool> is_enable{false};
  alignas(64) std::atomic<GraphTask::Status> status{GraphTask::kStatusInActive};

  std::atomic<uint32_t> max_recursion_depth{10'000};
  std::atomic<GraphTask::Policy> policy{GraphTask::kPolicyOnce};
  std::atomic<uint16_t> priority{100};
  std::atomic<int> condition_number{0};

  std::vector<std::weak_ptr<GraphTask>> precede_task_list;
  std::vector<std::weak_ptr<GraphTask>> succeed_task_list;

  mutable std::mutex mtx;
  vlink::ConditionVariable cv;

  std::shared_mutex shared_mtx;

  std::string name;
  std::string group_name;

  GraphTask::Callback callback;
  GraphTask::ConditionCallback condition_callback;

  std::mutex status_callbacks_mtx;
  std::atomic<uint32_t> next_status_callback_id{1};
  std::unordered_map<uint32_t, std::shared_ptr<GraphTask::StatusCallback>> status_callbacks;

  bool is_condition_task{false};
};

// GraphTask
std::shared_ptr<GraphTask> GraphTask::create(Callback&& callback, int condition_number) {
  auto& pool = MemoryPool::global_instance();

  void* mem = pool.allocate(sizeof(GraphTask), alignof(GraphTask));

  if VUNLIKELY (mem == nullptr) {
    throw std::bad_alloc{};
  }

  GraphTask* raw = nullptr;

  try {
    raw = new (mem) GraphTask(std::move(callback), condition_number);
  } catch (...) {
    pool.deallocate(mem, sizeof(GraphTask), alignof(GraphTask));
    throw;
  }

  return std::shared_ptr<GraphTask>(raw, [](GraphTask* task) {
    if VUNLIKELY (task == nullptr) {
      return;
    }

    task->~GraphTask();

    MemoryPool::global_instance().deallocate(task, sizeof(GraphTask), alignof(GraphTask));
  });
}

std::shared_ptr<GraphTask> GraphTask::create(const std::string& name, Callback&& callback, int condition_number) {
  auto& pool = MemoryPool::global_instance();

  void* mem = pool.allocate(sizeof(GraphTask), alignof(GraphTask));

  if VUNLIKELY (mem == nullptr) {
    throw std::bad_alloc{};
  }

  GraphTask* raw = nullptr;

  try {
    raw = new (mem) GraphTask(name, std::move(callback), condition_number);
  } catch (...) {
    pool.deallocate(mem, sizeof(GraphTask), alignof(GraphTask));
    throw;
  }

  return std::shared_ptr<GraphTask>(raw, [](GraphTask* task) {
    if VUNLIKELY (task == nullptr) {
      return;
    }

    task->~GraphTask();

    MemoryPool::global_instance().deallocate(task, sizeof(GraphTask), alignof(GraphTask));
  });
}

std::shared_ptr<GraphTask> GraphTask::create_condition(ConditionCallback&& callback, int condition_number) {
  auto& pool = MemoryPool::global_instance();

  void* mem = pool.allocate(sizeof(GraphTask), alignof(GraphTask));

  if VUNLIKELY (mem == nullptr) {
    throw std::bad_alloc{};
  }

  GraphTask* raw = nullptr;

  try {
    raw = new (mem) GraphTask(std::move(callback), condition_number);
  } catch (...) {
    pool.deallocate(mem, sizeof(GraphTask), alignof(GraphTask));
    throw;
  }

  return std::shared_ptr<GraphTask>(raw, [](GraphTask* task) {
    if VUNLIKELY (task == nullptr) {
      return;
    }

    task->~GraphTask();

    MemoryPool::global_instance().deallocate(task, sizeof(GraphTask), alignof(GraphTask));
  });
}

std::shared_ptr<GraphTask> GraphTask::create_condition(const std::string& name, ConditionCallback&& callback,
                                                       int condition_number) {
  auto& pool = MemoryPool::global_instance();

  void* mem = pool.allocate(sizeof(GraphTask), alignof(GraphTask));

  if VUNLIKELY (mem == nullptr) {
    throw std::bad_alloc{};
  }

  GraphTask* raw = nullptr;

  try {
    raw = new (mem) GraphTask(name, std::move(callback), condition_number);
  } catch (...) {
    pool.deallocate(mem, sizeof(GraphTask), alignof(GraphTask));
    throw;
  }

  return std::shared_ptr<GraphTask>(raw, [](GraphTask* task) {
    if VUNLIKELY (task == nullptr) {
      return;
    }

    task->~GraphTask();

    MemoryPool::global_instance().deallocate(task, sizeof(GraphTask), alignof(GraphTask));
  });
}

void GraphTask::cancel() {
  std::lock_guard topology_lock(topology_mutex());

  std::stack<std::shared_ptr<GraphTask>> task_stack;
  task_stack.emplace(shared_from_this());

  while (!task_stack.empty()) {
    auto current_task = task_stack.top();
    task_stack.pop();

    std::vector<std::weak_ptr<GraphTask>> succ_snapshot;

    {
      std::lock_guard lock(current_task->impl_->mtx);

      if (current_task->impl_->status == kStatusInActive) {
        continue;
      }

      succ_snapshot = current_task->impl_->succeed_task_list;
    }

    current_task->update_status(kStatusInActive);
    current_task->impl_->cv.notify_all();

    for (const auto& weak_task : succ_snapshot) {
      if (auto task_ptr = weak_task.lock()) {
        task_stack.emplace(task_ptr);
      }
    }
  }
}

void GraphTask::precede(const std::shared_ptr<GraphTask>& task) {
  if VUNLIKELY (!task || task.get() == this) {
    VLOG_F("GraphTask: Invalid task for precede.");
    return;
  }

  std::lock_guard topology_lock(topology_mutex());

  std::unique_lock lock1(this->impl_->mtx, std::defer_lock);
  std::unique_lock lock2(task->impl_->mtx, std::defer_lock);
  std::lock(lock1, lock2);

  if (std::find_if(impl_->succeed_task_list.begin(), impl_->succeed_task_list.end(), [&task](const auto& weak_task) {
        return weak_task.lock() == task;
      }) != impl_->succeed_task_list.end()) {
    VLOG_F("GraphTask: Task already added.");
    return;
  }

  if VUNLIKELY (reaches_via_successors(task.get(), task->impl_->succeed_task_list, this)) {
    VLOG_E("GraphTask: precede would create a cycle; edge rejected.");
    return;
  }

  impl_->succeed_task_list.emplace_back(task);
  task->impl_->precede_task_list.emplace_back(shared_from_this());
}

void GraphTask::succeed(const std::shared_ptr<GraphTask>& task) {
  if VUNLIKELY (!task || task.get() == this) {
    VLOG_F("GraphTask: Invalid task for succeed.");
    return;
  }

  std::lock_guard topology_lock(topology_mutex());

  std::unique_lock lock1(this->impl_->mtx, std::defer_lock);
  std::unique_lock lock2(task->impl_->mtx, std::defer_lock);
  std::lock(lock1, lock2);

  if (std::find_if(impl_->precede_task_list.begin(), impl_->precede_task_list.end(), [&task](const auto& weak_task) {
        return weak_task.lock() == task;
      }) != impl_->precede_task_list.end()) {
    VLOG_F("GraphTask: Task already added.");
    return;
  }

  if VUNLIKELY (reaches_via_successors(this, impl_->succeed_task_list, task.get())) {
    VLOG_E("GraphTask: succeed would create a cycle; edge rejected.");
    return;
  }

  impl_->precede_task_list.emplace_back(task);
  task->impl_->succeed_task_list.emplace_back(shared_from_this());
}

uint32_t GraphTask::register_status_callback(StatusCallback&& callback) {
  if VUNLIKELY (!callback) {
    return 0;
  }

  std::lock_guard lock(impl_->status_callbacks_mtx);

  uint32_t id = 0;

  for (uint32_t attempts = 0; attempts < std::numeric_limits<uint32_t>::max(); ++attempts) {
    id = impl_->next_status_callback_id.fetch_add(1, std::memory_order_relaxed);

    if VUNLIKELY (id == 0) {
      continue;
    }

    if VLIKELY (impl_->status_callbacks.find(id) == impl_->status_callbacks.end()) {
      break;
    }
  }

  if VUNLIKELY (id == 0) {
    VLOG_E("GraphTask: status_callback id space exhausted.");
    return 0;
  }

  impl_->status_callbacks.emplace(id, MemoryResource::make_shared<StatusCallback>(std::move(callback)));

  return id;
}

bool GraphTask::unregister_status_callback(uint32_t id) {
  std::lock_guard lock(impl_->status_callbacks_mtx);
  return impl_->status_callbacks.erase(id) > 0;
}

void GraphTask::clear_status_callbacks() {
  std::lock_guard lock(impl_->status_callbacks_mtx);
  impl_->status_callbacks.clear();
}

void GraphTask::set_name(const std::string& name) {
  std::lock_guard lock(impl_->shared_mtx);
  impl_->name = name;
}

void GraphTask::set_group_name(const std::string& name) {
  std::lock_guard lock(impl_->shared_mtx);
  impl_->group_name = name;
}

void GraphTask::set_condition_number(int condition_number) { impl_->condition_number = condition_number; }

void GraphTask::set_priority(uint16_t priority) { impl_->priority = priority; }

void GraphTask::set_max_recursion_depth(uint32_t depth) { impl_->max_recursion_depth = depth; }

void GraphTask::set_policy(Policy policy) { impl_->policy = policy; }

std::string GraphTask::get_name() const {
  std::shared_lock lock(impl_->shared_mtx);
  return impl_->name;
}

std::string GraphTask::get_group_name() const {
  std::shared_lock lock(impl_->shared_mtx);
  return impl_->group_name;
}

int GraphTask::get_condition_number() const { return impl_->condition_number; }

uint16_t GraphTask::get_priority() const { return impl_->priority; }

uint32_t GraphTask::get_max_recursion_depth() const { return impl_->max_recursion_depth; }

GraphTask::Policy GraphTask::get_policy() const { return impl_->policy; }

GraphTask::Status GraphTask::get_status() const { return impl_->status; }

void GraphTask::remove_precede_task(const std::shared_ptr<GraphTask>& task) {
  if VUNLIKELY (!task) {
    VLOG_F("GraphTask: Invalid task provided to remove_precede_task.");
    return;
  }

  std::lock_guard topology_lock(topology_mutex());

  std::unique_lock lock1(this->impl_->mtx, std::defer_lock);
  std::unique_lock lock2(task->impl_->mtx, std::defer_lock);
  std::lock(lock1, lock2);

  auto iter_succeed = std::remove_if(impl_->succeed_task_list.begin(), impl_->succeed_task_list.end(),
                                     [&task](const std::weak_ptr<GraphTask>& weak_task) {
                                       auto locked_task = weak_task.lock();
                                       return locked_task == task;
                                     });

  if VLIKELY (iter_succeed != impl_->succeed_task_list.end()) {
    impl_->succeed_task_list.erase(iter_succeed, impl_->succeed_task_list.end());
  } else {
    VLOG_F("GraphTask: Task not found in succeed_task_list.");
  }

  auto iter_precede = std::remove_if(task->impl_->precede_task_list.begin(), task->impl_->precede_task_list.end(),
                                     [this](const std::weak_ptr<GraphTask>& weak_task) {
                                       auto locked_task = weak_task.lock();
                                       return locked_task.get() == this;
                                     });

  if VLIKELY (iter_precede != task->impl_->precede_task_list.end()) {
    task->impl_->precede_task_list.erase(iter_precede, task->impl_->precede_task_list.end());
  } else {
    VLOG_F("GraphTask: Current task not found in task's precede_task_list.");
  }
}

void GraphTask::remove_succeed_task(const std::shared_ptr<GraphTask>& task) {
  if VUNLIKELY (!task) {
    VLOG_F("GraphTask: Invalid task provided to remove_succeed_task.");
    return;
  }

  std::lock_guard topology_lock(topology_mutex());

  std::unique_lock lock1(this->impl_->mtx, std::defer_lock);
  std::unique_lock lock2(task->impl_->mtx, std::defer_lock);
  std::lock(lock1, lock2);

  auto iter_precede = std::remove_if(impl_->precede_task_list.begin(), impl_->precede_task_list.end(),
                                     [&task](const auto& weak_task) { return weak_task.lock() == task; });

  if (iter_precede != impl_->precede_task_list.end()) {
    impl_->precede_task_list.erase(iter_precede, impl_->precede_task_list.end());
  }

  auto iter_succeed = std::remove_if(task->impl_->succeed_task_list.begin(), task->impl_->succeed_task_list.end(),
                                     [this](const auto& weak_task) { return weak_task.lock().get() == this; });

  if (iter_succeed != task->impl_->succeed_task_list.end()) {
    task->impl_->succeed_task_list.erase(iter_succeed, task->impl_->succeed_task_list.end());
  }
}

std::vector<std::weak_ptr<GraphTask>> GraphTask::get_precede_task_list() const {
  std::lock_guard lock(impl_->mtx);
  return impl_->precede_task_list;
}

std::vector<std::weak_ptr<GraphTask>> GraphTask::get_succeed_task_list() const {
  std::lock_guard lock(impl_->mtx);
  return impl_->succeed_task_list;
}

bool GraphTask::is_condition_task() const { return impl_->is_condition_task; }

GraphTask::GraphTask(Callback&& callback, int condition_number) : impl_(std::make_unique<Impl>()) {
  impl_->name = "Task_" + std::to_string(global_graph_task_count.fetch_add(1));
  impl_->condition_number = condition_number;
  impl_->is_condition_task = false;
  impl_->callback = std::move(callback);
}

GraphTask::GraphTask(const std::string& name, Callback&& callback, int condition_number)
    : impl_(std::make_unique<Impl>()) {
  impl_->name = name;
  impl_->condition_number = condition_number;
  impl_->is_condition_task = false;
  impl_->callback = std::move(callback);
}

GraphTask::GraphTask(ConditionCallback&& callback, int condition_number) : impl_(std::make_unique<Impl>()) {
  impl_->name = "Task_" + std::to_string(global_graph_task_count.fetch_add(1));
  impl_->condition_number = condition_number;
  impl_->is_condition_task = true;
  impl_->condition_callback = std::move(callback);
}

GraphTask::GraphTask(const std::string& name, ConditionCallback&& callback, int condition_number)
    : impl_(std::make_unique<Impl>()) {
  impl_->name = name;
  impl_->condition_number = condition_number;
  impl_->is_condition_task = true;
  impl_->condition_callback = std::move(callback);
}

GraphTask::~GraphTask() = default;

void GraphTask::process_and_traverse(FindTaskCallback&& callback) {
  uint32_t recursion_count = 0;

  std::stack<std::shared_ptr<GraphTask>> task_stack;

  task_stack.emplace(shared_from_this());

  std::unordered_map<GraphTask*, int> pending_count_map;
  std::unordered_set<GraphTask*> processed;

  std::vector<std::shared_ptr<GraphTask>> top_task_list;

  while (!task_stack.empty()) {
    auto current_task = task_stack.top();
    task_stack.pop();

    if (!processed.insert(current_task.get()).second) {
      continue;
    }

    {
      std::lock_guard lock(current_task->impl_->mtx);

      clear_invalid_task(current_task);

      if (recursion_count == 0) {
        current_task->impl_->is_ready = true;
        current_task->impl_->is_enable = true;
        current_task->impl_->active_index = 0U;

        auto& sub_pending_count = pending_count_map[current_task.get()];
        current_task->impl_->pending_index = ++sub_pending_count;

        top_task_list.emplace_back(current_task);
      }

      for (const auto& task : current_task->impl_->succeed_task_list) {
        auto task_ptr = task.lock();

        if VUNLIKELY (!task_ptr) {
          continue;
        }

        bool first_seen = (pending_count_map.find(task_ptr.get()) == pending_count_map.end());

        auto& sub_pending_count = pending_count_map[task_ptr.get()];
        task_ptr->impl_->pending_index = ++sub_pending_count;

        if (first_seen) {
          task_ptr->impl_->is_ready = false;
          task_ptr->impl_->is_enable = false;
          task_ptr->impl_->active_index = 0U;

          top_task_list.emplace_back(task_ptr);
          task_stack.emplace(task_ptr);
        }

        if VUNLIKELY (recursion_count++ >= impl_->max_recursion_depth) {
          CLOG_F("GraphTask: Recursion detection exceeds the upper limit (%d).", impl_->max_recursion_depth.load());
          return;
        }
      }
    }
  }

  for (const auto& top_task : top_task_list) {
    top_task->update_status(kStatusPending);
  }

  for (const auto& top_task : top_task_list) {
    callback(top_task);
  }
}

bool GraphTask::has_cycle() const {
  std::unordered_set<const GraphTask*> visited;
  std::unordered_set<const GraphTask*> recursion_stack;
  uint32_t depth = 0;
  const uint32_t max_depth = impl_->max_recursion_depth.load(std::memory_order_relaxed);

  return detect_cycle(this, visited, recursion_stack, depth, max_depth);
}

bool GraphTask::reaches_via_successors(const GraphTask* start_node,
                                       const std::vector<std::weak_ptr<GraphTask>>& start_successors,
                                       const GraphTask* target) const {
  std::unordered_set<const GraphTask*> visited;
  visited.insert(start_node);
  visited.insert(target);

  std::stack<std::shared_ptr<GraphTask>> stack;

  for (const auto& w : start_successors) {
    auto p = w.lock();

    if VUNLIKELY (!p) {
      continue;
    }

    if (p.get() == target) {
      return true;
    }

    if (visited.insert(p.get()).second) {
      stack.push(std::move(p));
    }
  }

  const uint32_t max_depth = impl_->max_recursion_depth.load(std::memory_order_relaxed);
  uint32_t visit_count = 0;

  while (!stack.empty()) {
    auto cur = std::move(stack.top());
    stack.pop();

    if VUNLIKELY (++visit_count > max_depth) {
      CLOG_F("GraphTask: reaches() exceeded max_recursion_depth (%u).", max_depth);
      return true;
    }

    std::vector<std::weak_ptr<GraphTask>> succ_copy;
    {
      std::lock_guard lock(cur->impl_->mtx);
      succ_copy = cur->impl_->succeed_task_list;
    }

    for (const auto& w : succ_copy) {
      auto p = w.lock();

      if VUNLIKELY (!p) {
        continue;
      }

      if (p.get() == target) {
        return true;
      }

      if (visited.insert(p.get()).second) {
        stack.push(std::move(p));
      }
    }
  }

  return false;
}

std::string GraphTask::export_to_dot() const {
  std::ostringstream dot_stream;

  dot_stream << "digraph TaskGraph {\n";

  dot_stream << "  node [fontname=\"Arial\"];\n";

  std::unordered_map<std::string, std::vector<const GraphTask*>> groups;

  std::unordered_set<const GraphTask*> visited;

  Function<void(const GraphTask*)> traverse = [&](const GraphTask* task) {
    if (visited.count(task)) {
      return;
    }

    visited.insert(task);

    {
      groups[task->get_group_name()].emplace_back(task);
    }

    {
      std::lock_guard lock(task->impl_->mtx);

      for (const auto& succeed_task_weak : task->impl_->succeed_task_list) {
        auto succeed_task = succeed_task_weak.lock();

        if VUNLIKELY (!succeed_task) {
          continue;
        }

        if (task->is_condition_task()) {
          dot_stream << "  \"" << task->impl_->name << "\" -> \"" << succeed_task->impl_->name
                     << "\" [style=dashed, arrowhead=vee];\n";
        } else {
          dot_stream << "  \"" << task->impl_->name << "\" -> \"" << succeed_task->impl_->name << "\";\n";
        }

        traverse(succeed_task.get());
      }
    }
  };

  traverse(this);

  std::string title_label;
  std::string extra_label;
  std::string color_label;

  for (const auto& [group_name, tasks] : groups) {
    if (!group_name.empty()) {
      dot_stream << "  subgraph cluster_" << group_name << " {\n";
      dot_stream << "    label = \"" << group_name << "\";\n";
      dot_stream << "    style = filled;\n";
      dot_stream << "    color = lightgray;\n";
    }

    for (const auto* task : tasks) {
      const std::string& name = task->get_name();

      if (task->impl_->policy == kPolicyOnce) {
        extra_label.clear();
        color_label = "lightgray";
      } else if (task->impl_->policy == kPolicyMultiple) {
        extra_label = "\\n[Multiple]";
        color_label = "lightgreen";
      } else if (task->impl_->policy == kPolicyWaitAll) {
        extra_label = "\\n[Waitfor]";
        color_label = "lightblue";
      } else {
        extra_label.clear();
        color_label = "lightgray";
      }

      if (task->is_condition_task()) {
        title_label = "style=dashed, shape=diamond, style=filled";
        Helpers::replace_string(color_label, "light", "");
      } else {
        title_label = "style=ellipse, style=filled";
      }

      dot_stream << "    \"";
      dot_stream << name;
      dot_stream << "\" [";
      dot_stream << title_label;
      dot_stream << ", color=";
      dot_stream << color_label;
      dot_stream << ", label=\"";
      dot_stream << name;
      dot_stream << extra_label;
      dot_stream << "\"];\n";
    }

    if (!group_name.empty()) {
      dot_stream << "  }\n";
    }
  }

  dot_stream << "}\n";

  return dot_stream.str();
}

int GraphTask::invoke(bool once) {
  if (once) {
    if (!impl_->is_enable || impl_->status != kStatusPending) {
      return -1;
    }
  }

  update_status(kStatusRunning);

  if (!impl_->is_condition_task && impl_->callback) {
    try {
      impl_->callback();
    } catch (const std::exception& e) {
      CLOG_E("GraphTask: callback (%s) threw an exception: %s.", impl_->name.c_str(), e.what());
    } catch (...) {
      CLOG_E("GraphTask: callback (%s) threw a non-std exception.", impl_->name.c_str());
    }

    update_status(kStatusDone);

    return 0;
  }

  if (impl_->is_condition_task && impl_->condition_callback) {
    int ret = 0;
    bool failed = false;

    try {
      ret = impl_->condition_callback();
    } catch (const std::exception& e) {
      failed = true;
      CLOG_E("GraphTask: condition_callback (%s) threw an exception: %s.", impl_->name.c_str(), e.what());
    } catch (...) {
      failed = true;
      CLOG_E("GraphTask: condition_callback (%s) threw a non-std exception.", impl_->name.c_str());
    }

    update_status(kStatusDone);

    if (failed) {
      return std::numeric_limits<int>::max();
    }

    return ret;
  }

  update_status(kStatusDone);

  return -1;
}

void GraphTask::wait() {
  std::unique_lock lock(impl_->mtx);

  impl_->cv.wait(lock, [this]() -> bool { return impl_->is_ready.load() || impl_->status == kStatusInActive; });
}

void GraphTask::notify(int condition_number) {
  std::vector<std::weak_ptr<GraphTask>> invoke_list;
  std::vector<std::shared_ptr<GraphTask>> skip_list;

  {
    std::lock_guard lock(impl_->mtx);

    for (const auto& task : impl_->succeed_task_list) {
      auto task_ptr = task.lock();

      if VUNLIKELY (!task_ptr) {
        continue;
      }

      if (condition_number != task_ptr->impl_->condition_number) {
        if (impl_->is_condition_task) {
          bool has_active = false;
          const bool ready = task_ptr->mark_predecessor_satisfied(false, &has_active);

          if (ready && !has_active) {
            task_ptr->impl_->is_ready = true;
            task_ptr->impl_->is_enable = false;
            task_ptr->impl_->cv.notify_all();
            skip_list.emplace_back(task_ptr);
          } else if (ready && task_ptr->impl_->policy == kPolicyWaitAll) {
            task_ptr->impl_->is_ready = true;
            task_ptr->impl_->is_enable = true;
            task_ptr->impl_->cv.notify_all();
          }
        }

        continue;
      }

      if (task_ptr->impl_->policy == kPolicyOnce) {
        task_ptr->mark_predecessor_satisfied(true, nullptr);
        task_ptr->impl_->is_ready = true;
        task_ptr->impl_->is_enable = true;
        task_ptr->impl_->cv.notify_all();
      } else if (task_ptr->impl_->policy == kPolicyMultiple) {
        task_ptr->mark_predecessor_satisfied(true, nullptr);
        task_ptr->impl_->is_ready = true;
        task_ptr->impl_->is_enable = false;
        task_ptr->impl_->cv.notify_all();

        invoke_list.emplace_back(task);
      } else if (task_ptr->impl_->policy == kPolicyWaitAll) {
        bool has_active = false;

        if (!task_ptr->mark_predecessor_satisfied(true, &has_active) || !has_active) {
          continue;
        }

        task_ptr->impl_->is_ready = true;
        task_ptr->impl_->is_enable = true;
        task_ptr->impl_->cv.notify_all();
      }
    }
  }

  for (const auto& task_ptr : skip_list) {
    task_ptr->update_status(kStatusInActive);
    task_ptr->notify_skip();
  }

  for (const auto& task : invoke_list) {
    auto task_ptr = task.lock();

    if VUNLIKELY (!task_ptr) {
      continue;
    }

    task_ptr->invoke(false);
  }
}

void GraphTask::notify_skip() {
  std::vector<std::weak_ptr<GraphTask>> succ_snapshot;
  std::vector<std::shared_ptr<GraphTask>> skip_list;

  {
    std::lock_guard lock(impl_->mtx);
    succ_snapshot = impl_->succeed_task_list;
  }

  for (const auto& task : succ_snapshot) {
    auto task_ptr = task.lock();

    if VUNLIKELY (!task_ptr) {
      continue;
    }

    bool has_active = false;
    const bool ready = task_ptr->mark_predecessor_satisfied(false, &has_active);

    if (!ready) {
      continue;
    }

    if (!has_active) {
      task_ptr->impl_->is_ready = true;
      task_ptr->impl_->is_enable = false;
      task_ptr->impl_->cv.notify_all();
      skip_list.emplace_back(task_ptr);
    } else if (task_ptr->impl_->policy == kPolicyWaitAll) {
      task_ptr->impl_->is_ready = true;
      task_ptr->impl_->is_enable = true;
      task_ptr->impl_->cv.notify_all();
    }
  }

  for (const auto& task_ptr : skip_list) {
    task_ptr->update_status(kStatusInActive);
    task_ptr->notify_skip();
  }
}

bool GraphTask::mark_predecessor_satisfied(bool active, bool* has_active) {
  if (active) {
    impl_->active_index.fetch_add(1U, std::memory_order_acq_rel);
  }

  size_t expected = impl_->pending_index.load(std::memory_order_acquire);

  while (expected > 0U) {
    const size_t desired = expected - 1U;

    if (impl_->pending_index.compare_exchange_weak(expected, desired, std::memory_order_acq_rel,
                                                   std::memory_order_acquire)) {
      if (has_active != nullptr) {
        *has_active = impl_->active_index.load(std::memory_order_acquire) > 0U;
      }

      return desired == 0U;
    }
  }

  if (has_active != nullptr) {
    *has_active = impl_->active_index.load(std::memory_order_acquire) > 0U;
  }

  return false;
}

void GraphTask::update_status(Status status) {
  if VUNLIKELY (impl_->status == status) {
    return;
  }

  impl_->status = status;

  std::string name_copy;

  {
    std::shared_lock lock(impl_->shared_mtx);

    name_copy = impl_->name;
  }

#ifdef VLINK_ENABLE_BASE_MEMORY_RESOURCE
  std::pmr::vector<std::shared_ptr<StatusCallback>> callbacks(&MemoryResource::global_instance());
#else
  std::vector<std::shared_ptr<StatusCallback>> callbacks;
#endif

  {
    std::lock_guard lock(impl_->status_callbacks_mtx);

    callbacks.reserve(impl_->status_callbacks.size());

    for (auto& [id, cb] : impl_->status_callbacks) {
      (void)id;

      callbacks.emplace_back(cb);
    }
  }

  for (auto& cb : callbacks) {
    if VUNLIKELY (!cb || !(*cb)) {
      continue;
    }

    try {
      (*cb)(name_copy, status);
    } catch (const std::exception& e) {
      CLOG_E("GraphTask: status_callback (%s) threw an exception: %s.", name_copy.c_str(), e.what());
    } catch (...) {
      CLOG_E("GraphTask: status_callback (%s) threw a non-std exception.", name_copy.c_str());
    }
  }
}

bool GraphTask::detect_cycle(const GraphTask* task, std::unordered_set<const GraphTask*>& visited,
                             std::unordered_set<const GraphTask*>& recursion_stack, uint32_t& depth,
                             uint32_t max_depth) const {
  if VUNLIKELY (recursion_stack.count(task)) {
    return true;
  }

  if (visited.count(task)) {
    return false;
  }

  if VUNLIKELY (++depth > max_depth) {
    CLOG_F("GraphTask: detect_cycle exceeded max_recursion_depth (%u).", max_depth);
    return true;
  }

  visited.insert(task);
  recursion_stack.insert(task);

  std::vector<std::weak_ptr<GraphTask>> succ_copy;
  {
    std::lock_guard lock(task->impl_->mtx);
    succ_copy = task->impl_->succeed_task_list;
  }

  for (const auto& weak_task : succ_copy) {
    auto next_task = weak_task.lock();

    if VUNLIKELY (!next_task) {
      continue;
    }

    if VUNLIKELY (detect_cycle(next_task.get(), visited, recursion_stack, depth, max_depth)) {
      return true;
    }
  }

  recursion_stack.erase(task);
  --depth;

  return false;
}

void GraphTask::clear_invalid_task(const std::shared_ptr<GraphTask>& task) {
  {
    task->impl_->precede_task_list.erase(
        std::remove_if(task->impl_->precede_task_list.begin(), task->impl_->precede_task_list.end(),
                       [](const std::weak_ptr<GraphTask>& weak_task) { return weak_task.expired(); }),
        task->impl_->precede_task_list.end());

    task->impl_->succeed_task_list.erase(
        std::remove_if(task->impl_->succeed_task_list.begin(), task->impl_->succeed_task_list.end(),
                       [](const std::weak_ptr<GraphTask>& weak_task) { return weak_task.expired(); }),
        task->impl_->succeed_task_list.end());
  }
}

}  // namespace vlink
