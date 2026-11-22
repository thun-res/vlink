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

#include <vlink/base/condition_variable.h>
#include <vlink/base/macros.h>

#include <atomic>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_set>
#include <utility>

#include "bindings.h"

namespace vlink::python {

[[nodiscard]] inline bool is_python_runtime_available() noexcept {
  if VUNLIKELY (!Py_IsInitialized()) {
    return false;
  }

#if PY_VERSION_HEX >= 0x030D0000
  return Py_IsFinalizing() == 0;
#elif !defined(Py_LIMITED_API)
  return _Py_IsFinalizing() == 0;
#else
  return true;
#endif
}

struct GilSafePyFunction {
  nb::callable fn;

  explicit GilSafePyFunction(nb::callable f) : fn(std::move(f)) {}
  GilSafePyFunction(GilSafePyFunction&&) noexcept = default;

  ~GilSafePyFunction() {
    nb::handle leaked = fn.release();

    if VLIKELY (leaked.is_valid() && is_python_runtime_available()) {
      nb::gil_scoped_acquire gil;
      leaked.dec_ref();
    }
  }
};

struct GilSafePyObject {
  nb::object obj;
  explicit GilSafePyObject(nb::object o) : obj(std::move(o)) {}
  ~GilSafePyObject() {
    nb::handle leaked = obj.release();

    if VLIKELY (leaked.is_valid() && is_python_runtime_available()) {
      nb::gil_scoped_acquire gil;
      leaked.dec_ref();
    }
  }
};

struct PythonCallbackActivity final {
  ~PythonCallbackActivity();

  void enter() {
    std::lock_guard lock(mtx);
    ++active;
  }

  void leave() noexcept {
    std::lock_guard lock(mtx);

    if (--active == 0) {
      cv.notify_all();
    }
  }

  void wait() {
    std::unique_lock lock(mtx);
    cv.wait(lock, [this]() { return active == 0; });
  }

  void finish_cleanup() noexcept {
    std::lock_guard lock(mtx);

    if (--pending_cleanup == 0) {
      cv.notify_all();
    }
  }

  std::mutex mtx;
  vlink::ConditionVariable cv;
  size_t active{0};
  size_t pending_cleanup{0};
  std::atomic_bool stopping{false};
  nb::object loop_owner;
};

struct PythonCallbackScope final {
  explicit PythonCallbackScope(const std::shared_ptr<PythonCallbackActivity>& activity, const void* owner = nullptr,
                               const void* kind = nullptr);

  PythonCallbackScope(const PythonCallbackScope&) = delete;
  PythonCallbackScope& operator=(const PythonCallbackScope&) = delete;

  ~PythonCallbackScope();

  std::shared_ptr<PythonCallbackActivity> activity;
  const void* owner;
  const void* kind;
  PythonCallbackScope* previous;
};

void report_current_exception(const char* context) noexcept;
void initialize_python_finalizers();
void close_python_finalizers();
void require_python_finalizers();
void release_python_loop_owner(nb::handle retained) noexcept;
void set_python_timer_loop(const std::shared_ptr<PythonCallbackActivity>& activity, nb::object loop);
[[nodiscard]] bool is_in_python_callback(const PythonCallbackActivity* activity) noexcept;
[[nodiscard]] std::shared_ptr<PythonCallbackActivity> current_python_callback_activity() noexcept;
[[nodiscard]] bool is_in_python_owner_callback(const void* owner) noexcept;
[[nodiscard]] bool is_in_python_owner_callback(const void* owner, const void* kind) noexcept;
void defer_last_python_callback_owner(nb::object& owner,
                                      const std::shared_ptr<PythonCallbackActivity>& activity) noexcept;
std::unordered_set<PyObject*>& python_pre_destroy_hooks();
std::unordered_set<const void*>& python_native_finalizing();

template <typename T>
void destroy_python_callback_owner(std::unique_ptr<T>& owner,
                                   const std::shared_ptr<PythonCallbackActivity>& activity) noexcept {
  if (!owner) {
    return;
  }

  if (is_in_python_callback(activity.get())) {
    T* deferred = owner.release();

    try {
      std::thread cleanup([deferred, activity = activity]() {
        activity->wait();
        delete deferred;
      });
      cleanup.detach();
    } catch (...) {
      return;
    }

    return;
  }

  if (is_python_runtime_available() && PyGILState_Check()) {
    nb::gil_scoped_release release;
    owner.reset();
  } else {
    owner.reset();
  }
}

template <typename CleanupT>
void ensure_python_finalizer(nb::handle instance, CleanupT cleanup, bool only_at_exit) {
  require_python_finalizers();
  auto& hooks = python_pre_destroy_hooks();
  PyObject* key = instance.ptr();

  if (!hooks.insert(key).second) {
    return;
  }

  try {
    auto weakref = nb::module_::import_("weakref");
    weakref.attr("finalize")(instance, nb::cpp_function([key, ref = weakref.attr("ref")(instance),
                                                         cleanup = std::move(cleanup), only_at_exit]() noexcept {
                               python_pre_destroy_hooks().erase(key);

                               if VUNLIKELY (!is_python_runtime_available()) {
                                 return;
                               }

                               try {
                                 nb::object owner = ref();

                                 if (!owner.is_none()) {
                                   close_python_finalizers();
                                 } else if (only_at_exit) {
                                   return;
                                 }

                                 cleanup();
                               } catch (...) {
                                 // Destruction hooks cannot propagate into weakref finalization.
                               }
                             }));
  } catch (...) {
    hooks.erase(key);
    throw;
  }
}

template <typename NativeT, typename CleanupT>
void ensure_python_pre_destroy_hook(nb::handle instance, NativeT* native, CleanupT cleanup) {
  ensure_python_finalizer(
      instance,
      [native, cleanup = std::move(cleanup)] {
        python_native_finalizing().insert(native);
        try {
          nb::gil_scoped_release release;
          cleanup(*native);
        } catch (...) {
          // Destruction hooks cannot propagate into weakref finalization.
        }
        python_native_finalizing().erase(native);
      },
      false);
}

template <typename NativeT>
auto make_owned_void_callback(NativeT* native, nb::callable py_cb, const char* context) {
  return [native, activity = std::make_shared<PythonCallbackActivity>(), cb = GilSafePyFunction(std::move(py_cb)),
          context]() {
    if VUNLIKELY (!is_python_runtime_available()) {
      return;
    }

    PythonCallbackScope active(activity, native);
    nb::gil_scoped_acquire gil;

    if VUNLIKELY (python_native_finalizing().find(native) != python_native_finalizing().end()) {
      return;
    }

    nb::object owner = nb::find(native);

    try {
      cb.fn();
    } catch (std::exception&) {
      report_current_exception(context);
    }

    defer_last_python_callback_owner(owner, activity);
  };
}

template <typename NativeT, typename InvokeT>
void invoke_owned_python_callback(NativeT* native, const std::shared_ptr<PythonCallbackActivity>& activity,
                                  const char* context, InvokeT&& invoke, const void* kind = nullptr) {
  if VUNLIKELY (!is_python_runtime_available()) {
    return;
  }

  PythonCallbackScope active(activity, native, kind);

  if VUNLIKELY (activity->stopping.load(std::memory_order_acquire)) {
    return;
  }

  nb::gil_scoped_acquire gil;

  if VUNLIKELY (python_native_finalizing().find(native) != python_native_finalizing().end()) {
    return;
  }

  nb::object owner = nb::find(native);

  try {
    std::forward<InvokeT>(invoke)();
  } catch (std::exception&) {
    report_current_exception(context);
  }

  defer_last_python_callback_owner(owner, activity);
}

inline auto make_stateful_void_callback(const std::shared_ptr<PythonCallbackActivity>& activity,
                                        std::shared_ptr<GilSafePyFunction> cb, const char* context) {
  return [activity = activity, cb = std::move(cb), context]() {
    if VUNLIKELY (!is_python_runtime_available()) {
      return;
    }

    PythonCallbackScope active(activity);

    if VUNLIKELY (activity->stopping.load(std::memory_order_acquire)) {
      return;
    }

    nb::gil_scoped_acquire gil;

    try {
      cb->fn();
    } catch (std::exception&) {
      report_current_exception(context);
    }
  };
}

inline auto make_stateful_void_callback(const std::shared_ptr<PythonCallbackActivity>& activity, nb::callable py_cb,
                                        const char* context) {
  return make_stateful_void_callback(activity, std::make_shared<GilSafePyFunction>(std::move(py_cb)), context);
}

}  // namespace vlink::python
