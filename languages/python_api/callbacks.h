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

#include <memory>
#include <mutex>
#include <thread>
#include <unordered_set>
#include <utility>

#include "bindings.h"

namespace vlink::python {

struct GilSafePyFunction {
  nb::callable fn;
  explicit GilSafePyFunction(nb::callable f) : fn(std::move(f)) {}
  ~GilSafePyFunction() {
    nb::handle leaked = fn.release();

    if VLIKELY (leaked.is_valid() && Py_IsInitialized()) {
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

    if VLIKELY (leaked.is_valid() && Py_IsInitialized()) {
      nb::gil_scoped_acquire gil;
      leaked.dec_ref();
    }
  }
};

struct PythonCallbackActivity final {
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

  std::mutex mtx;
  vlink::ConditionVariable cv;
  size_t active{0};
  std::shared_ptr<GilSafePyObject> lifetime_owner;
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
void set_python_callback_lifetime_owner(const std::shared_ptr<PythonCallbackActivity>& activity,
                                        std::shared_ptr<GilSafePyObject> owner);
[[nodiscard]] bool is_in_python_callback(const PythonCallbackActivity* activity) noexcept;
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
      std::thread cleanup([deferred, activity]() {
        activity->wait();
        delete deferred;
      });
      cleanup.detach();
    } catch (...) {
      return;
    }

    return;
  }

  if (Py_IsInitialized() && PyGILState_Check()) {
    nb::gil_scoped_release release;
    owner.reset();
  } else {
    owner.reset();
  }
}

template <typename NativeT, typename Cleanup>
void ensure_python_pre_destroy_hook(nb::handle instance, NativeT* native, Cleanup cleanup) {
  auto& hooks = python_pre_destroy_hooks();
  auto [iter, inserted] = hooks.insert(instance.ptr());

  if (!inserted) {
    return;
  }

  PyObject* key = instance.ptr();

  try {
    nb::module_::import_("weakref").attr("finalize")(
        instance, nb::cpp_function([key, native, cleanup = std::move(cleanup)]() noexcept {
          python_pre_destroy_hooks().erase(key);

          if VUNLIKELY (!Py_IsInitialized()) {
            return;
          }

          python_native_finalizing().insert(native);
          try {
            nb::gil_scoped_release release;
            cleanup(*native);
          } catch (...) {
            // Destruction hooks cannot propagate into weakref finalization.
          }
          python_native_finalizing().erase(native);
        }));
  } catch (...) {
    hooks.erase(iter);
    throw;
  }
}

template <typename NativeT>
auto make_owned_void_callback(NativeT* native, nb::callable py_cb, const char* context) {
  auto activity = std::make_shared<PythonCallbackActivity>();
  auto cb = std::make_shared<GilSafePyFunction>(std::move(py_cb));

  return [native, activity, cb = std::move(cb), context]() {
    PythonCallbackScope active(activity, native);

    if VUNLIKELY (!Py_IsInitialized()) {
      return;
    }

    nb::gil_scoped_acquire gil;

    if VUNLIKELY (python_native_finalizing().find(native) != python_native_finalizing().end()) {
      return;
    }

    nb::object owner = nb::find(native);

    try {
      cb->fn();
    } catch (std::exception&) {
      report_current_exception(context);
    }

    defer_last_python_callback_owner(owner, activity);
  };
}

template <typename NativeT, typename Invoke>
void invoke_owned_python_callback(NativeT* native, const std::shared_ptr<PythonCallbackActivity>& activity,
                                  const char* context, Invoke&& invoke, const void* kind = nullptr) {
  PythonCallbackScope active(activity, native, kind);

  if VUNLIKELY (!Py_IsInitialized()) {
    return;
  }

  nb::gil_scoped_acquire gil;

  if VUNLIKELY (python_native_finalizing().find(native) != python_native_finalizing().end()) {
    return;
  }

  nb::object owner = nb::find(native);

  try {
    std::forward<Invoke>(invoke)();
  } catch (std::exception&) {
    report_current_exception(context);
  }

  defer_last_python_callback_owner(owner, activity);
}

inline auto make_stateful_void_callback(const std::shared_ptr<PythonCallbackActivity>& activity, nb::callable py_cb,
                                        const char* context) {
  auto cb = std::make_shared<GilSafePyFunction>(std::move(py_cb));

  return [activity, cb = std::move(cb), context]() {
    PythonCallbackScope active(activity);

    if VUNLIKELY (!Py_IsInitialized()) {
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

}  // namespace vlink::python
