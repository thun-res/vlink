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

#include "ownership.h"

#include <vlink/base/timer.h>

#include <thread>
#include <unordered_map>
#include <utility>

namespace vlink::python {

struct PythonInstanceOwner {
  nb::object owner;
  const vlink::Bytes* pinned_bytes{nullptr};

  void clear() noexcept {
    if (pinned_bytes != nullptr) {
      unpin_bytes_storage(*pinned_bytes);
      pinned_bytes = nullptr;
    }

    owner = nb::object();
  }

  ~PythonInstanceOwner() { clear(); }
};

static auto& python_instance_owners() {
  static auto* owners = new std::unordered_map<PyObject*, PythonInstanceOwner>();
  return *owners;
}

static void defer_python_owner_until_loop_idle(nb::object& owner, vlink::MessageLoop& loop) noexcept {
  nb::handle retained = owner.release();

  if (!retained.is_valid()) {
    return;
  }

  try {
    std::thread cleanup([retained, &loop]() {
      loop.wait_for_idle(vlink::Timer::kInfinite, false);

      if VLIKELY (Py_IsInitialized()) {
        nb::gil_scoped_acquire gil;

        if (Py_REFCNT(retained.ptr()) == 1 && loop.is_running()) {
          nb::gil_scoped_release release;
          loop.quit();
          loop.wait_for_quit(vlink::Timer::kInfinite, false);
        }

        retained.dec_ref();
      }
    });
    cleanup.detach();
  } catch (...) {
  }
}

static void delete_python_instance_owner(void* data) noexcept {
  auto& owners = python_instance_owners();
  auto iter = owners.find(static_cast<PyObject*>(data));

  if (iter == owners.end()) {
    return;
  }

  if (iter->second.pinned_bytes == nullptr && iter->second.owner.is_valid()) {
    vlink::MessageLoop* loop = nullptr;

    if (nb::try_cast(iter->second.owner, loop, false) && loop != nullptr && loop->is_in_same_thread()) {
      defer_python_owner_until_loop_idle(iter->second.owner, *loop);
    }
  }

  owners.erase(iter);
}

static PythonInstanceOwner& python_instance_owner(nb::handle instance) {
  auto& owners = python_instance_owners();
  auto [iter, inserted] = owners.try_emplace(instance.ptr());

  if (inserted) {
    nb::detail::keep_alive(instance.ptr(), instance.ptr(), delete_python_instance_owner);
  }

  return iter->second;
}

void bind_python_instance_owner(nb::handle instance, nb::object owner) {
  auto& slot = python_instance_owner(instance);

  if (slot.pinned_bytes == nullptr && slot.owner.ptr() == owner.ptr()) {
    return;
  }

  slot.clear();
  slot.owner = std::move(owner);
}

void bind_python_bytes_owner(nb::handle instance, nb::object input, const vlink::Bytes& bytes) {
  auto iter = python_instance_owners().find(instance.ptr());

  if (iter != python_instance_owners().end() && iter->second.pinned_bytes == &bytes) {
    return;
  }

  pin_bytes_storage(bytes);

  try {
    auto& slot = python_instance_owner(instance);
    slot.clear();
    slot.owner = std::move(input);
    slot.pinned_bytes = &bytes;
  } catch (...) {
    unpin_bytes_storage(bytes);
    throw;
  }
}

void unbind_python_instance_owner(nb::handle instance) {
  auto iter = python_instance_owners().find(instance.ptr());

  if (iter != python_instance_owners().end()) {
    iter->second.clear();
  }
}

void unbind_python_instance_owner_after_loop_idle(nb::handle instance, vlink::MessageLoop& loop) noexcept {
  auto iter = python_instance_owners().find(instance.ptr());

  if (iter == python_instance_owners().end()) {
    return;
  }

  if (iter->second.pinned_bytes != nullptr) {
    iter->second.clear();
    return;
  }

  defer_python_owner_until_loop_idle(iter->second.owner, loop);
}

}  // namespace vlink::python
