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

#include <nanobind/stl/string.h>
#include <nanobind/stl/vector.h>
#include <vlink/base/cpu_profiler.h>
#include <vlink/base/cpu_profiler_guard.h>
#include <vlink/base/deadline_timer.h>
#include <vlink/base/elapsed_timer.h>
#include <vlink/base/memory_pool.h>
#include <vlink/base/memory_resource.h>
#include <vlink/base/multi_loop.h>
#include <vlink/base/spin_lock.h>
#include <vlink/base/thread_pool.h>
#include <vlink/base/timer.h>
#include <vlink/base/wheel_timer.h>

#include "callbacks.h"

namespace vlink::python {

using namespace nb::literals;  // NOLINT

struct PythonWheelTimer final {
  PythonWheelTimer(uint32_t slots, uint32_t interval_ms)
      : timer(std::make_unique<vlink::WheelTimer>(slots, interval_ms)) {}

  PythonWheelTimer(const PythonWheelTimer&) = delete;
  PythonWheelTimer& operator=(const PythonWheelTimer&) = delete;

  ~PythonWheelTimer() {
    if (!timer) {
      return;
    }

    if (Py_IsInitialized() && PyGILState_Check()) {
      nb::gil_scoped_release release;
      timer.reset();
    } else {
      timer.reset();
    }
  }

  std::unique_ptr<vlink::WheelTimer> timer;
};

struct PythonTimer final {
  PythonTimer() : activity(std::make_shared<PythonCallbackActivity>()), timer(std::make_unique<vlink::Timer>()) {}

  explicit PythonTimer(vlink::MessageLoop* loop)
      : activity(std::make_shared<PythonCallbackActivity>()), timer(std::make_unique<vlink::Timer>(loop)) {}

  PythonTimer(vlink::MessageLoop* loop, uint32_t interval_ms, int32_t loop_count)
      : activity(std::make_shared<PythonCallbackActivity>()),
        timer(std::make_unique<vlink::Timer>(loop, interval_ms, loop_count)) {}

  PythonTimer(uint32_t interval_ms, int32_t loop_count)
      : activity(std::make_shared<PythonCallbackActivity>()),
        timer(std::make_unique<vlink::Timer>(interval_ms, loop_count)) {}

  PythonTimer(const PythonTimer&) = delete;
  PythonTimer& operator=(const PythonTimer&) = delete;

  ~PythonTimer() { destroy_python_callback_owner(timer, activity); }

  std::shared_ptr<PythonCallbackActivity> activity;
  std::unique_ptr<vlink::Timer> timer;
};

static void acquire_spin_lock(vlink::SpinLock& lock) {
  if (lock.try_lock()) {
    return;
  }

  nb::gil_scoped_release release;
  lock.lock();
}

void bind_runtime(nb::module_& m) {
  nb::enum_<vlink::ElapsedTimer::Method>(m, "TimerMethod")
      .value("CpuTimestamp", vlink::ElapsedTimer::kCpuTimestamp)
      .value("CpuActiveTime", vlink::ElapsedTimer::kCpuActiveTime);
  nb::enum_<vlink::ElapsedTimer::Accuracy>(m, "TimerAccuracy")
      .value("Milli", vlink::ElapsedTimer::kMilli)
      .value("Micro", vlink::ElapsedTimer::kMicro)
      .value("Nano", vlink::ElapsedTimer::kNano);

  nb::class_<vlink::ElapsedTimer>(m, "ElapsedTimer")
      .def(nb::init<>())
      .def(nb::init<vlink::ElapsedTimer::Method>(), "method"_a)
      .def(nb::init<vlink::ElapsedTimer::Accuracy>(), "accuracy"_a)
      .def(nb::init<vlink::ElapsedTimer::Method, vlink::ElapsedTimer::Accuracy>(), "method"_a, "accuracy"_a)
      .def_static("get_sys_timestamp", &vlink::ElapsedTimer::get_sys_timestamp,
                  "accuracy"_a = vlink::ElapsedTimer::kMilli, "high_resolution"_a = true)
      .def_static("get_cpu_timestamp", &vlink::ElapsedTimer::get_cpu_timestamp,
                  "accuracy"_a = vlink::ElapsedTimer::kMilli, "high_resolution"_a = true)
      .def_static("get_cpu_active_time", &vlink::ElapsedTimer::get_cpu_active_time,
                  "accuracy"_a = vlink::ElapsedTimer::kMilli)
      .def("get_method", &vlink::ElapsedTimer::get_method)
      .def("get_accuracy", &vlink::ElapsedTimer::get_accuracy)
      .def("start", &vlink::ElapsedTimer::start)
      .def("stop", &vlink::ElapsedTimer::stop)
      .def("restart", &vlink::ElapsedTimer::restart)
      .def("is_active", &vlink::ElapsedTimer::is_active)
      .def("get", &vlink::ElapsedTimer::get);

  nb::class_<vlink::DeadlineTimer>(m, "DeadlineTimer")
      .def(nb::init<>())
      .def(nb::init<int64_t>(), "interval_ms"_a)
      .def(nb::init<int64_t, vlink::ElapsedTimer::Accuracy>(), "interval"_a, "accuracy"_a)
      .def("set_deadline", &vlink::DeadlineTimer::set_deadline, "interval"_a)
      .def("set_deadline_abs", &vlink::DeadlineTimer::set_deadline_abs, "abs_deadline"_a)
      .def("reset", &vlink::DeadlineTimer::reset)
      .def("deadline", &vlink::DeadlineTimer::deadline)
      .def("remaining_time", &vlink::DeadlineTimer::remaining_time)
      .def("has_expired", &vlink::DeadlineTimer::has_expired)
      .def("is_valid", &vlink::DeadlineTimer::is_valid)
      .def("get_accuracy", &vlink::DeadlineTimer::get_accuracy);

  nb::enum_<vlink::MessageLoop::Type>(m, "MessageLoopType")
      .value("Normal", vlink::MessageLoop::kNormalType)
      .value("Lockfree", vlink::MessageLoop::kLockfreeType)
      .value("Priority", vlink::MessageLoop::kPriorityType);
  nb::enum_<vlink::MessageLoop::Strategy>(m, "MessageLoopStrategy")
      .value("Optimization", vlink::MessageLoop::kOptimizationStrategy)
      .value("Pop", vlink::MessageLoop::kPopStrategy)
      .value("Block", vlink::MessageLoop::kBlockStrategy);
  nb::enum_<vlink::MessageLoop::Priority>(m, "TaskPriority")
      .value("No", vlink::MessageLoop::kNoPriority)
      .value("Lowest", vlink::MessageLoop::kLowestPriority)
      .value("Timer", vlink::MessageLoop::kTimerPriority)
      .value("Normal", vlink::MessageLoop::kNormalPriority)
      .value("Highest", vlink::MessageLoop::kHighestPriority);

  nb::enum_<vlink::ThreadPool::Type>(m, "ThreadPoolType")
      .value("Normal", vlink::ThreadPool::kNormalType)
      .value("Lockfree", vlink::ThreadPool::kLockfreeType);
  nb::enum_<vlink::ThreadPool::Strategy>(m, "ThreadPoolStrategy")
      .value("Optimization", vlink::ThreadPool::kOptimizationStrategy)
      .value("Pop", vlink::ThreadPool::kPopStrategy)
      .value("Block", vlink::ThreadPool::kBlockStrategy);

  nb::class_<vlink::MessageLoop>(m, "MessageLoop", "Single-threaded event loop", nb::is_weak_referenceable())
      .def(nb::init<>())
      .def(nb::init<vlink::MessageLoop::Type>(), "type"_a)
      .def("set_name", &vlink::MessageLoop::set_name, "name"_a)
      .def("get_name", &vlink::MessageLoop::get_name)
      .def("get_type", &vlink::MessageLoop::get_type)
      .def("get_strategy", &vlink::MessageLoop::get_strategy)
      .def("set_strategy", &vlink::MessageLoop::set_strategy, "strategy"_a)
      .def(
          "post_task",
          [](nb::object instance, nb::callable callback) {
            auto& self = nb::cast<vlink::MessageLoop&>(instance);
            ensure_python_pre_destroy_hook(instance, &self, [](vlink::MessageLoop& loop) {
              loop.quit(true);
              loop.wait_for_quit(vlink::Timer::kInfinite, false);
            });
            auto cb = make_owned_void_callback(&self, std::move(callback), "vlink::MessageLoop.post_task");
            nb::gil_scoped_release release;
            return self.post_task(std::move(cb));
          },
          "callback"_a)
      .def(
          "post_task_with_priority",
          [](nb::object instance, nb::callable callback, vlink::MessageLoop::Priority priority) {
            auto& self = nb::cast<vlink::MessageLoop&>(instance);
            ensure_python_pre_destroy_hook(instance, &self, [](vlink::MessageLoop& loop) {
              loop.quit(true);
              loop.wait_for_quit(vlink::Timer::kInfinite, false);
            });
            auto cb =
                make_owned_void_callback(&self, std::move(callback), "vlink::MessageLoop.post_task_with_priority");
            nb::gil_scoped_release release;
            return self.post_task_with_priority(std::move(cb), static_cast<uint16_t>(priority));
          },
          "callback"_a, "priority"_a)
      .def(
          "register_begin_handler",
          [](nb::object instance, nb::callable callback) {
            auto& self = nb::cast<vlink::MessageLoop&>(instance);
            ensure_python_pre_destroy_hook(instance, &self, [](vlink::MessageLoop& loop) {
              loop.quit(true);
              loop.wait_for_quit(vlink::Timer::kInfinite, false);
            });
            self.register_begin_handler(
                make_owned_void_callback(&self, std::move(callback), "vlink::MessageLoop.register_begin_handler"));
          },
          "callback"_a)
      .def(
          "register_end_handler",
          [](nb::object instance, nb::callable callback) {
            auto& self = nb::cast<vlink::MessageLoop&>(instance);
            ensure_python_pre_destroy_hook(instance, &self, [](vlink::MessageLoop& loop) {
              loop.quit(true);
              loop.wait_for_quit(vlink::Timer::kInfinite, false);
            });
            self.register_end_handler(
                make_owned_void_callback(&self, std::move(callback), "vlink::MessageLoop.register_end_handler"));
          },
          "callback"_a)
      .def(
          "register_idle_handler",
          [](nb::object instance, nb::callable callback) {
            auto& self = nb::cast<vlink::MessageLoop&>(instance);
            ensure_python_pre_destroy_hook(instance, &self, [](vlink::MessageLoop& loop) {
              loop.quit(true);
              loop.wait_for_quit(vlink::Timer::kInfinite, false);
            });
            self.register_idle_handler(
                make_owned_void_callback(&self, std::move(callback), "vlink::MessageLoop.register_idle_handler"));
          },
          "callback"_a)
      .def("run",
           [](vlink::MessageLoop& self) {
             nb::gil_scoped_release release;
             return self.run();
           })
      .def("async_run",
           [](vlink::MessageLoop& self) {
             nb::gil_scoped_release release;
             return self.async_run();
           })
      .def("spin",
           [](vlink::MessageLoop& self) {
             nb::gil_scoped_release release;
             return self.spin();
           })
      .def(
          "spin_once",
          [](vlink::MessageLoop& self, bool block) {
            nb::gil_scoped_release release;
            return self.spin_once(block);
          },
          "block"_a = true)
      .def(
          "exec_task",
          [](nb::object instance, uint32_t delay_ms, nb::callable callback, uint16_t priority,
             uint32_t schedule_timeout_ms, uint32_t execution_timeout_ms) {
            auto& self = nb::cast<vlink::MessageLoop&>(instance);
            ensure_python_pre_destroy_hook(instance, &self, [](vlink::MessageLoop& loop) {
              loop.quit(true);
              loop.wait_for_quit(vlink::Timer::kInfinite, false);
            });
            vlink::Schedule::Config cfg(delay_ms, priority, schedule_timeout_ms, execution_timeout_ms);
            auto status = self.exec_task(
                cfg, make_owned_void_callback(&self, std::move(callback), "vlink::MessageLoop.exec_task"));
            nb::gil_scoped_release release;
            return status.dispatch();
          },
          "delay_ms"_a, "callback"_a, "priority"_a = 0, "schedule_timeout_ms"_a = 0, "execution_timeout_ms"_a = 0,
          "Post a delayed/scheduled task. Returns True if posted successfully")
      .def(
          "quit",
          [](vlink::MessageLoop& self, bool force) {
            nb::gil_scoped_release release;
            return self.quit(force);
          },
          "force"_a = false)
      .def(
          "wait_for_quit",
          [](vlink::MessageLoop& self, int timeout_ms, bool check) {
            nb::gil_scoped_release release;
            return self.wait_for_quit(timeout_ms, check);
          },
          "timeout_ms"_a = -1, "check"_a = true)
      .def(
          "wait_for_idle",
          [](vlink::MessageLoop& self, int timeout_ms, bool check) {
            nb::gil_scoped_release release;
            return self.wait_for_idle(timeout_ms, check);
          },
          "timeout_ms"_a = -1, "check"_a = true)
      .def("wakeup", &vlink::MessageLoop::wakeup)
      .def("reset_lockfree_capacity", &vlink::MessageLoop::reset_lockfree_capacity)
      .def("is_running", &vlink::MessageLoop::is_running)
      .def("is_busy", &vlink::MessageLoop::is_busy)
      .def("is_ready_to_quit", &vlink::MessageLoop::is_ready_to_quit)
      .def("is_in_same_thread", &vlink::MessageLoop::is_in_same_thread)
      .def("get_task_count", &vlink::MessageLoop::get_task_count)
      .def("get_max_task_count", &vlink::MessageLoop::get_max_task_count)
      .def("get_max_timer_count", &vlink::MessageLoop::get_max_timer_count)
      .def("get_max_elapsed_time", &vlink::MessageLoop::get_max_elapsed_time);

  nb::class_<PythonTimer>(m, "Timer", "Event-loop-driven periodic timer")
      .def(nb::init<>())
      .def(
          "__init__",
          [](nb::pointer_and_handle<PythonTimer> v, vlink::MessageLoop* loop_ptr) {
            new (static_cast<PythonTimer*>(v.p)) PythonTimer(loop_ptr);

            if (v.p->timer->get_message_loop() == loop_ptr) {
              set_python_callback_lifetime_owner(v.p->activity, std::make_shared<GilSafePyObject>(nb::find(loop_ptr)));
            }
          },
          "loop"_a)
      .def(
          "__init__",
          [](nb::pointer_and_handle<PythonTimer> v, vlink::MessageLoop* loop_ptr, uint32_t interval_ms,
             int32_t loop_count) {
            new (static_cast<PythonTimer*>(v.p)) PythonTimer(loop_ptr, interval_ms, loop_count);

            if (v.p->timer->get_message_loop() == loop_ptr) {
              set_python_callback_lifetime_owner(v.p->activity, std::make_shared<GilSafePyObject>(nb::find(loop_ptr)));
            }
          },
          "loop"_a, "interval_ms"_a, "loop_count"_a = vlink::Timer::kInfinite)
      .def(
          "__init__",
          [](nb::pointer_and_handle<PythonTimer> v, vlink::MessageLoop* loop_ptr, uint32_t interval_ms,
             int32_t loop_count, nb::callable callback) {
            new (static_cast<PythonTimer*>(v.p)) PythonTimer(loop_ptr, interval_ms, loop_count);
            v.p->timer->set_callback(
                make_stateful_void_callback(v.p->activity, std::move(callback), "vlink::Timer.__init__"));

            if (v.p->timer->get_message_loop() == loop_ptr) {
              set_python_callback_lifetime_owner(v.p->activity, std::make_shared<GilSafePyObject>(nb::find(loop_ptr)));
            }
          },
          "loop"_a, "interval_ms"_a, "loop_count"_a, "callback"_a)
      .def(
          "__init__",
          [](nb::pointer_and_handle<PythonTimer> v, uint32_t interval_ms, int32_t loop_count) {
            new (static_cast<PythonTimer*>(v.p)) PythonTimer(interval_ms, loop_count);
          },
          "interval_ms"_a, "loop_count"_a = vlink::Timer::kInfinite)
      .def(
          "__init__",
          [](nb::pointer_and_handle<PythonTimer> v, uint32_t interval_ms, nb::callable callback) {
            new (static_cast<PythonTimer*>(v.p)) PythonTimer(interval_ms, vlink::Timer::kInfinite);
            v.p->timer->set_callback(
                make_stateful_void_callback(v.p->activity, std::move(callback), "vlink::Timer.__init__"));
          },
          "interval_ms"_a, "callback"_a)
      .def(
          "__init__",
          [](nb::pointer_and_handle<PythonTimer> v, uint32_t interval_ms, int32_t loop_count, nb::callable callback) {
            new (static_cast<PythonTimer*>(v.p)) PythonTimer(interval_ms, loop_count);
            v.p->timer->set_callback(
                make_stateful_void_callback(v.p->activity, std::move(callback), "vlink::Timer.__init__"));
          },
          "interval_ms"_a, "loop_count"_a, "callback"_a)
      .def(
          "attach",
          [](nb::object instance, nb::object loop) {
            auto& self = nb::cast<PythonTimer&>(instance);
            auto* loop_ptr = nb::cast<vlink::MessageLoop*>(loop);
            const bool result = self.timer->attach(loop_ptr);

            if (self.timer->get_message_loop() == loop_ptr) {
              set_python_callback_lifetime_owner(self.activity, std::make_shared<GilSafePyObject>(std::move(loop)));
            } else if (self.timer->get_message_loop() == nullptr) {
              set_python_callback_lifetime_owner(self.activity, nullptr);
            }

            return result;
          },
          "loop"_a)
      .def("detach",
           [](nb::object instance) {
             auto& self = nb::cast<PythonTimer&>(instance);
             const bool result = self.timer->detach();

             if (self.timer->get_message_loop() == nullptr) {
               set_python_callback_lifetime_owner(self.activity, nullptr);
             }

             return result;
           })
      .def(
          "start",
          [](PythonTimer& self, nb::object callback) {
            if (callback.is_none()) {
              self.timer->start();
              return;
            }

            auto cb =
                make_stateful_void_callback(self.activity, nb::cast<nb::callable>(callback), "vlink::Timer.start");
            nb::gil_scoped_release release;
            self.timer->start(std::move(cb));
          },
          "callback"_a = nb::none())
      .def(
          "set_callback",
          [](PythonTimer& self, nb::callable callback) {
            auto cb = make_stateful_void_callback(self.activity, std::move(callback), "vlink::Timer.set_callback");
            nb::gil_scoped_release release;
            self.timer->set_callback(std::move(cb));
          },
          "callback"_a)
      .def("restart", [](PythonTimer& self) { self.timer->restart(); })
      .def("stop", [](PythonTimer& self) { self.timer->stop(); })
      .def("is_active", [](const PythonTimer& self) { return self.timer->is_active(); })
      .def("is_strict", [](const PythonTimer& self) { return self.timer->is_strict(); })
      .def("get_interval", [](const PythonTimer& self) { return self.timer->get_interval(); })
      .def("get_loop_count", [](const PythonTimer& self) { return self.timer->get_loop_count(); })
      .def("get_remain_loop_count", [](const PythonTimer& self) { return self.timer->get_remain_loop_count(); })
      .def("get_invoke_count", [](const PythonTimer& self) { return self.timer->get_invoke_count(); })
      .def("get_priority", [](const PythonTimer& self) { return self.timer->get_priority(); })
      .def(
          "set_interval", [](PythonTimer& self, uint32_t interval_ms) { self.timer->set_interval(interval_ms); },
          "interval_ms"_a)
      .def(
          "set_loop_count", [](PythonTimer& self, int32_t count) { self.timer->set_loop_count(count); }, "count"_a)
      .def(
          "set_strict", [](PythonTimer& self, bool strict) { self.timer->set_strict(strict); }, "strict"_a)
      .def(
          "set_priority", [](PythonTimer& self, uint16_t priority) { self.timer->set_priority(priority); },
          "priority"_a)
      .def(
          "get_message_loop", [](const PythonTimer& self) { return self.timer->get_message_loop(); },
          nb::rv_policy::reference)
      .def_static(
          "call_once",
          [](vlink::MessageLoop* loop, uint32_t interval_ms, nb::callable callback, uint16_t priority) {
            nb::object instance = nb::find(loop);
            ensure_python_pre_destroy_hook(instance, loop, [](vlink::MessageLoop& owner) {
              owner.quit(true);
              owner.wait_for_quit(vlink::Timer::kInfinite, false);
            });
            return vlink::Timer::call_once(
                loop, interval_ms, make_owned_void_callback(loop, std::move(callback), "vlink::Timer.call_once"),
                priority);
          },
          "loop"_a, "interval_ms"_a, "callback"_a, "priority"_a = 0);
  m.attr("TIMER_INFINITE") = vlink::Timer::kInfinite;

  nb::class_<vlink::ThreadPool>(m, "ThreadPool", nb::is_weak_referenceable())
      .def(nb::init<size_t>(), "thread_count"_a = static_cast<size_t>(4))
      .def(nb::init<size_t, vlink::ThreadPool::Type>(), "thread_count"_a, "type"_a)
      .def("set_name", &vlink::ThreadPool::set_name, "name"_a)
      .def("get_name", &vlink::ThreadPool::get_name)
      .def("get_type", &vlink::ThreadPool::get_type)
      .def("get_strategy", &vlink::ThreadPool::get_strategy)
      .def("set_strategy", &vlink::ThreadPool::set_strategy, "strategy"_a)
      .def(
          "post_task",
          [](nb::object instance, nb::callable callback) {
            auto& self = nb::cast<vlink::ThreadPool&>(instance);
            ensure_python_pre_destroy_hook(instance, &self, [](vlink::ThreadPool& pool) { pool.shutdown(); });
            auto cb = make_owned_void_callback(&self, std::move(callback), "vlink::ThreadPool.post_task");
            nb::gil_scoped_release release;
            return self.post_task(std::move(cb));
          },
          "callback"_a)
      .def("shutdown",
           [](vlink::ThreadPool& self) {
             nb::gil_scoped_release release;
             return self.shutdown();
           })
      .def("get_task_count", &vlink::ThreadPool::get_task_count)
      .def("get_max_task_count", &vlink::ThreadPool::get_max_task_count)
      .def("is_in_work_thread", &vlink::ThreadPool::is_in_work_thread);

  nb::class_<vlink::SpinLock>(m, "SpinLock")
      .def(nb::init<>())
      .def("lock", &acquire_spin_lock)
      .def("try_lock", &vlink::SpinLock::try_lock)
      .def("unlock", &vlink::SpinLock::unlock)
      .def("__enter__",
           [](nb::object self) -> nb::object {
             acquire_spin_lock(nb::cast<vlink::SpinLock&>(self));
             return self;
           })
      .def("__exit__", [](vlink::SpinLock& self, nb::args, nb::kwargs) { self.unlock(); });

  nb::class_<vlink::CpuProfiler>(m, "CpuProfiler", "CPU active-time profiler")
      .def(nb::init<>())
      .def_static("is_global_enabled", &vlink::CpuProfiler::is_global_enabled)
      .def("begin", &vlink::CpuProfiler::begin)
      .def("end", &vlink::CpuProfiler::end)
      .def("get", &vlink::CpuProfiler::get)
      .def("restart", &vlink::CpuProfiler::restart)
      .def("__enter__",
           [](nb::object self) -> nb::object {
             nb::cast<vlink::CpuProfiler&>(self).begin();
             return self;
           })
      .def("__exit__", [](vlink::CpuProfiler& self, nb::args, nb::kwargs) { self.end(); });

  nb::class_<vlink::CpuProfilerGuard>(m, "CpuProfilerGuard", "RAII guard that brackets CpuProfiler.begin/end")
      .def(nb::init<vlink::CpuProfiler*>(), "profiler"_a, nb::keep_alive<1, 2>());

  nb::class_<vlink::MultiLoop, vlink::MessageLoop>(m, "MultiLoop", "Thread-pool backed MessageLoop")
      .def(nb::init<size_t>(), "thread_num"_a = static_cast<size_t>(4))
      .def(nb::init<size_t, vlink::MessageLoop::Type>(), "thread_num"_a, "type"_a);

  nb::class_<PythonWheelTimer>(m, "WheelTimer", "Hierarchical timing wheel")
      .def(nb::init<uint32_t, uint32_t>(), "slots"_a, "interval_ms"_a)
      .def("start",
           [](PythonWheelTimer& self) {
             nb::gil_scoped_release release;
             self.timer->start();
           })
      .def("stop",
           [](PythonWheelTimer& self) {
             nb::gil_scoped_release release;
             self.timer->stop();
           })
      .def("pause", [](PythonWheelTimer& self) { self.timer->pause(); })
      .def("resume", [](PythonWheelTimer& self) { self.timer->resume(); })
      .def("wakeup", [](PythonWheelTimer& self) { self.timer->wakeup(); })
      .def("is_running", [](const PythonWheelTimer& self) { return self.timer->is_running(); })
      .def(
          "add",
          [](PythonWheelTimer& self, uint32_t timeout_ms, nb::callable callback,
             uint32_t repeat_ms) -> vlink::WheelTimer::Key {
            auto cb = std::make_shared<GilSafePyFunction>(std::move(callback));
            return self.timer->add(
                timeout_ms,
                [cb](vlink::WheelTimer::Key key) {
                  if VUNLIKELY (!Py_IsInitialized()) {
                    return;
                  }

                  nb::gil_scoped_acquire gil;
                  try {
                    cb->fn(key);
                  } catch (std::exception&) {
                    report_current_exception("vlink::WheelTimer.add");
                  }
                },
                repeat_ms);
          },
          "timeout_ms"_a, "callback"_a, "repeat_ms"_a = 0,
          "Schedule callback(key) once after timeout_ms; if repeat_ms>0 reschedule that interval. Returns Key.")
      .def(
          "remove", [](PythonWheelTimer& self, vlink::WheelTimer::Key key) { return self.timer->remove(key); }, "key"_a)
      .def(
          "get_remaining_time",
          [](const PythonWheelTimer& self, vlink::WheelTimer::Key key) { return self.timer->get_remaining_time(key); },
          "key"_a)
      .def(
          "set_catchup_limit",
          [](PythonWheelTimer& self, uint32_t max_slots_to_catch_up) {
            self.timer->set_catchup_limit(max_slots_to_catch_up);
          },
          "max_slots_to_catch_up"_a);

  auto mp_cls = nb::class_<vlink::MemoryPool>(m, "MemoryPool", "Tiered (pyramid) memory pool with per-tier statistics");

  nb::class_<vlink::MemoryPool::Tier>(mp_cls, "Tier")
      .def(nb::init<>())
      .def(
          "__init__",
          [](vlink::MemoryPool::Tier* self, size_t max_size, size_t blocks_per_chunk) {
            new (self) vlink::MemoryPool::Tier{max_size, blocks_per_chunk};
          },
          "max_size"_a, "blocks_per_chunk"_a)
      .def_rw("max_size", &vlink::MemoryPool::Tier::max_size)
      .def_rw("blocks_per_chunk", &vlink::MemoryPool::Tier::blocks_per_chunk);

  nb::class_<vlink::MemoryPool::Config>(mp_cls, "Config",
                                        "Memory-pool tiers, preallocation, and cross-shard transfer batch size")
      .def(nb::init<>())
      .def_rw("tiers", &vlink::MemoryPool::Config::tiers)
      .def_rw("prealloc", &vlink::MemoryPool::Config::prealloc)
      .def_rw("batch_size", &vlink::MemoryPool::Config::batch_size,
              "Maximum free-list nodes moved by one cross-shard steal; 0 falls back to 16");

  nb::class_<vlink::MemoryPool::TierStats>(mp_cls, "TierStats")
      .def_ro("max_size", &vlink::MemoryPool::TierStats::max_size)
      .def_ro("blocks_per_chunk", &vlink::MemoryPool::TierStats::blocks_per_chunk)
      .def_ro("block_size", &vlink::MemoryPool::TierStats::block_size)
      .def_ro("hit_count", &vlink::MemoryPool::TierStats::hit_count)
      .def_ro("deallocate_count", &vlink::MemoryPool::TierStats::deallocate_count)
      .def_ro("in_use_blocks", &vlink::MemoryPool::TierStats::in_use_blocks)
      .def_ro("chunk_count", &vlink::MemoryPool::TierStats::chunk_count)
      .def_ro("upstream_alloc_count", &vlink::MemoryPool::TierStats::upstream_alloc_count)
      .def_ro("upstream_alloc_bytes", &vlink::MemoryPool::TierStats::upstream_alloc_bytes);

  nb::class_<vlink::MemoryPool::OversizedStats>(mp_cls, "OversizedStats")
      .def_ro("alloc_count", &vlink::MemoryPool::OversizedStats::alloc_count)
      .def_ro("alloc_bytes", &vlink::MemoryPool::OversizedStats::alloc_bytes)
      .def_ro("dealloc_count", &vlink::MemoryPool::OversizedStats::dealloc_count);

  mp_cls.def(nb::init<>())
      .def(nb::init<int, bool>(), "level"_a, "prealloc"_a = false)
      .def(nb::init<const vlink::MemoryPool::Config&>(), "config"_a)
      .def("get_tier_count", &vlink::MemoryPool::get_tier_count)
      .def("get_stats", &vlink::MemoryPool::get_stats)
      .def("get_oversized_stats", &vlink::MemoryPool::get_oversized_stats)
      .def("reset_stats", &vlink::MemoryPool::reset_stats)
      .def("clear", &vlink::MemoryPool::clear)
      .def("trim", &vlink::MemoryPool::trim)
      .def_static("get_default_config", &vlink::MemoryPool::get_default_config)
      .def_static("global_instance", &vlink::MemoryPool::global_instance, "use_env_level"_a = true,
                  nb::rv_policy::reference);
  mp_cls.attr("kBlockAlignment") = vlink::MemoryPool::kBlockAlignment;

#ifdef VLINK_ENABLE_BASE_MEMORY_RESOURCE
  nb::class_<vlink::MemoryResource>(m, "MemoryResource",
                                    "std::pmr::memory_resource adapter delegating to a vlink::MemoryPool")
      .def(nb::init<>())
      .def(nb::init<int, bool>(), "level"_a, "prealloc"_a = false)
      .def(nb::init<const vlink::MemoryPool::Config&>(), "config"_a)
      .def("get_memory_pool", &vlink::MemoryResource::get_memory_pool, nb::rv_policy::reference_internal)
      .def("trim", &vlink::MemoryResource::trim)
      .def_static("global_instance", &vlink::MemoryResource::global_instance, "use_env_level"_a = true,
                  nb::rv_policy::reference);
#endif
}

}  // namespace vlink::python
