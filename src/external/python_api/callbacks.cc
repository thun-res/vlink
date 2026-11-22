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

#include <vlink/base/message_loop.h>

#include <stdexcept>

namespace vlink::python {

static thread_local PythonCallbackScope* current_python_callback_scope{nullptr};
static bool python_finalizers_closed{false};

void close_python_finalizers() { python_finalizers_closed = true; }

void initialize_python_finalizers() {
  nb::object finalize = nb::module_::import_("weakref").attr("finalize");
  python_finalizers_closed = nb::cast<bool>(finalize.attr("_shutdown"));

  if (!python_finalizers_closed) {
    finalize(finalize, nb::cpp_function(&close_python_finalizers));
  }
}

void require_python_finalizers() {
  if VUNLIKELY (python_finalizers_closed || !is_python_runtime_available()) {
    throw std::runtime_error("Cannot start or register a VLink resource during interpreter shutdown");
  }
}

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

void release_python_loop_owner(nb::handle retained) noexcept {
  if (!retained.is_valid() || !is_python_runtime_available()) {
    return;
  }

  nb::gil_scoped_acquire gil;
  nb::object owner = nb::steal(retained);

  if (Py_REFCNT(owner.ptr()) != 1 || !nb::inst_state(owner).second) {
    return;
  }

  auto& loop = nb::cast<vlink::MessageLoop&>(owner);

  if (!loop.is_running()) {
    return;
  }

  if (loop.is_in_same_thread()) {
    try {
      std::thread cleanup(release_python_loop_owner, owner.release());
      cleanup.detach();
    } catch (const std::exception&) {
      report_current_exception("vlink::Timer releasing its MessageLoop on another thread");
    }

    return;
  }

  if (python_pre_destroy_hooks().count(owner.ptr()) != 0) {
    return;
  }

  try {
    nb::weakref on_dealloc(owner, nb::cpp_function([&loop](nb::handle) {
                             nb::gil_scoped_release release;
                             loop.quit();
                             loop.wait_for_quit(vlink::Timer::kInfinite, false);
                           }));
    owner.reset();
  } catch (const std::exception&) {
    (void)owner.release();
    report_current_exception("vlink::Timer preparing to release its MessageLoop");
  }
}

PythonCallbackActivity::~PythonCallbackActivity() { release_python_loop_owner(loop_owner.release()); }

void set_python_timer_loop(const std::shared_ptr<PythonCallbackActivity>& activity, nb::object loop) {
  auto previous = std::exchange(activity->loop_owner, std::move(loop));

  if (!previous.is_valid()) {
    return;
  }

  {
    std::unique_lock lock(activity->mtx);

    if (activity->active == 0) {
      lock.unlock();
      release_python_loop_owner(previous.release());
      return;
    }

    ++activity->pending_cleanup;
  }

  try {
    std::thread cleanup([activity = activity, retained = previous.release()]() {
      activity->wait();
      release_python_loop_owner(retained);
      activity->finish_cleanup();
    });
    cleanup.detach();
  } catch (const std::exception&) {
    activity->finish_cleanup();
    report_current_exception("vlink::Timer releasing its MessageLoop after a callback");
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

std::shared_ptr<PythonCallbackActivity> current_python_callback_activity() noexcept {
  return current_python_callback_scope ? current_python_callback_scope->activity : nullptr;
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
    std::thread cleanup([activity = activity, retained]() {
      activity->wait();

      if VLIKELY (is_python_runtime_available()) {
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
