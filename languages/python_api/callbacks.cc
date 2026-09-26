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

#include "callbacks.h"

namespace vlink::python {

static thread_local PythonCallbackScope* current_python_callback_scope{nullptr};

PythonCallbackScope::PythonCallbackScope(const std::shared_ptr<PythonCallbackActivity>& activity, const void* owner,
                                         const void* kind)
    : activity(activity), owner(owner), kind(kind), previous(current_python_callback_scope) {
  activity->enter();
  current_python_callback_scope = this;
}

PythonCallbackScope::~PythonCallbackScope() {
  current_python_callback_scope = previous;
  activity->leave();
}

void report_current_exception(const char* context) noexcept {
  try {
    throw;
  } catch (nb::python_error& e) {
    e.discard_as_unraisable(context);
  } catch (const std::exception& e) {
    PyErr_SetString(PyExc_RuntimeError, e.what());
    PyErr_WriteUnraisable(nb::str(context).ptr());
  } catch (...) {
    PyErr_SetString(PyExc_RuntimeError, "Unknown C++ exception");
    PyErr_WriteUnraisable(nb::str(context).ptr());
  }
}

void set_python_callback_lifetime_owner(const std::shared_ptr<PythonCallbackActivity>& activity,
                                        std::shared_ptr<GilSafePyObject> owner) {
  auto previous = std::exchange(activity->lifetime_owner, std::move(owner));

  if (!previous) {
    return;
  }

  {
    std::lock_guard lock(activity->mtx);

    if (activity->active == 0) {
      return;
    }
  }

  nb::handle retained = previous->obj.release();
  previous.reset();

  try {
    std::thread cleanup([activity, retained]() {
      activity->wait();

      if VLIKELY (retained.is_valid() && Py_IsInitialized()) {
        nb::gil_scoped_acquire gil;
        retained.dec_ref();
      }
    });
    cleanup.detach();
  } catch (...) {
    return;
  }
}

bool is_in_python_callback(const PythonCallbackActivity* activity) noexcept {
  for (auto* scope = current_python_callback_scope; scope != nullptr; scope = scope->previous) {
    if (scope->activity.get() == activity) {
      return true;
    }
  }

  return false;
}

bool is_in_python_owner_callback(const void* owner) noexcept {
  for (auto* scope = current_python_callback_scope; scope != nullptr; scope = scope->previous) {
    if (scope->owner == owner) {
      return true;
    }
  }

  return false;
}

bool is_in_python_owner_callback(const void* owner, const void* kind) noexcept {
  for (auto* scope = current_python_callback_scope; scope != nullptr; scope = scope->previous) {
    if (scope->owner == owner && scope->kind == kind) {
      return true;
    }
  }

  return false;
}

void defer_last_python_callback_owner(nb::object& owner,
                                      const std::shared_ptr<PythonCallbackActivity>& activity) noexcept {
  if (!owner.is_valid() || Py_REFCNT(owner.ptr()) != 1) {
    return;
  }

  nb::handle retained = owner.release();

  try {
    std::thread cleanup([activity, retained]() {
      activity->wait();

      if VLIKELY (Py_IsInitialized()) {
        nb::gil_scoped_acquire gil;
        retained.dec_ref();
      }
    });
    cleanup.detach();
  } catch (...) {
  }
}

std::unordered_set<PyObject*>& python_pre_destroy_hooks() {
  static auto* hooks = new std::unordered_set<PyObject*>();
  return *hooks;
}

std::unordered_set<const void*>& python_native_finalizing() {
  static auto* owners = new std::unordered_set<const void*>();
  return *owners;
}

}  // namespace vlink::python
