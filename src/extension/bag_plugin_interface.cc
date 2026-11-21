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

#include "./extension/bag_plugin_interface.h"

#include <atomic>
#include <mutex>

#include "./base/condition_variable.h"

namespace vlink {

struct BagPluginInterface::Forwarder final {
#ifdef __cpp_lib_atomic_shared_ptr
  std::atomic<BindingPtr> binding;

  Forwarder() = default;
  Forwarder(Forwarder&& other) noexcept : binding(other.binding.exchange(nullptr)) {}
#else
  BindingPtr binding;
#endif

  void operator()(const Frame& frame) const {
    auto snapshot = std::atomic_load_explicit(&binding, std::memory_order_acquire);

    if VLIKELY (snapshot) {
      (*snapshot)(frame);
    }
  }
};

BagPluginInterface::BagPluginInterface() : callback_(Forwarder{}) {}

thread_local BagPluginInterface::Gate::Call* BagPluginInterface::Gate::Call::current_output_ = nullptr;

BagPluginInterface::Gate::Call::Call(Gate& value) : gate(value) {
  std::lock_guard lock(gate.mtx);
  entered = !gate.closed;

  if VLIKELY (entered) {
    ++gate.active;
  }
}

BagPluginInterface::Gate::Call::~Call() {
  if (current_output_ == this) {
    current_output_ = previous_output;
  }

  if VLIKELY (entered) {
    std::lock_guard lock(gate.mtx);

    --gate.active;

    if VUNLIKELY (gate.closed) {
      gate.cv.notify_all();
    }
  }
}

void BagPluginInterface::Gate::Call::enter_output_callback() {
  previous_output = current_output_;
  current_output_ = this;
}

void BagPluginInterface::Gate::close() {
  size_t local_outputs = 0;

  for (auto* call = Call::current_output_; call; call = call->previous_output) {
    if (&call->gate == this) {
      ++local_outputs;
    }
  }

  std::unique_lock lock(mtx);
  closed = true;
  cv.wait(lock, [this, local_outputs] { return active == local_outputs; });
}

BagPluginInterface::BindingPtr BagPluginInterface::replace_callback(Callback&& callback, BindingPtr& previous) {
  auto* forwarder = callback_.target<Forwarder>();

  if VUNLIKELY (forwarder == nullptr) {
    callback_ = Forwarder{};
    forwarder = callback_.target<Forwarder>();
  }

  auto next = callback ? std::make_shared<Callback>(std::move(callback)) : nullptr;
  previous = std::atomic_exchange_explicit(&forwarder->binding, next, std::memory_order_acq_rel);
  return next;
}

void BagPluginInterface::retire_callback(const BindingPtr& binding) {
  if (!binding) {
    return;
  }

  if (auto* forwarder = callback_.target<Forwarder>()) {
    auto expected = binding;
    std::atomic_compare_exchange_strong(&forwarder->binding, &expected, BindingPtr{});
  }
}

void BagPluginInterface::register_callback(Callback&& callback) {
  BindingPtr previous;
  replace_callback(std::move(callback), previous);
}

void BagPluginInterface::flush_and_retire_callback(const BindingPtr& binding) {
  try {
    flush();
  } catch (...) {
    retire_callback(binding);
    throw;
  }

  retire_callback(binding);
}

}  // namespace vlink
