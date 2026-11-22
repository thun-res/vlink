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
#include <exception>
#include <mutex>
#include <stdexcept>

#include "./base/condition_variable.h"
#include "./base/logger.h"

namespace vlink {

struct BagPluginInterface::Binding final {
  Binding(Callback&& callback, const std::shared_ptr<Gate>& gate) : callback(std::move(callback)), gate(gate) {}

  void close() {
    if (!gate) {
      return;
    }

    std::unique_lock lock(gate->mtx);
    closed = true;

    for (auto* call = Gate::Call::current_; call; call = call->previous) {
      if (call->binding.gate == gate) {
        return;
      }
    }

    gate->cv.wait(lock, [this] { return active == 0; });
  }

  Callback callback;
  std::shared_ptr<Gate> gate;
  size_t active{0};
  bool closed{false};
  std::atomic_bool flushing{false};
};

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

    if VUNLIKELY (!snapshot) {
      return;
    }

    if (!snapshot->gate) {
      snapshot->callback(frame);
      return;
    }

    Gate::Call call(*snapshot);

    if VLIKELY (call.entered) {
      snapshot->callback(frame);
    }
  }
};

BagPluginInterface::BagPluginInterface() : callback_(Forwarder{}) {}

thread_local BagPluginInterface::Gate::Call* BagPluginInterface::Gate::Call::current_ = nullptr;

BagPluginInterface::Gate::Call::Call(Binding& value) : binding(value) {
  auto& gate = *binding.gate;
  std::lock_guard lock(gate.mtx);

  entered = !gate.closed && !binding.closed;

  if VLIKELY (entered) {
    ++gate.active;
    ++binding.active;

    previous = current_;
    current_ = this;
  }
}

BagPluginInterface::Gate::Call::~Call() {
  if VLIKELY (entered) {
    current_ = previous;

    auto& gate = *binding.gate;
    std::lock_guard lock(gate.mtx);

    --gate.active;
    --binding.active;

    if VUNLIKELY (gate.closed || binding.closed) {
      gate.cv.notify_all();
    }
  }
}

void BagPluginInterface::Gate::Call::enter_output_callback() { current_->in_output_callback = true; }

std::unique_lock<std::recursive_mutex> BagPluginInterface::Gate::lock_binding() {
  std::unique_lock lock(binding_mtx, std::try_to_lock);

  if (!lock) {
    throw std::runtime_error("Bag plugin binding is being changed by another thread.");
  }

  return lock;
}

void BagPluginInterface::Gate::close() {
  size_t local_outputs = 0;

  for (auto* call = Call::current_; call; call = call->previous) {
    if (call->binding.gate.get() == this && call->in_output_callback) {
      ++local_outputs;
    }
  }

  std::unique_lock lock(mtx);
  closed = true;
  cv.wait(lock, [this, local_outputs] { return active == local_outputs; });
}

BagPluginInterface::BindingPtr BagPluginInterface::replace_callback(Callback&& callback, BindingPtr& previous,
                                                                    const std::shared_ptr<Gate>& gate) {
  auto* forwarder = callback_.target<Forwarder>();

  if VUNLIKELY (forwarder == nullptr) {
    callback_ = Forwarder{};
    forwarder = callback_.target<Forwarder>();
  }

  auto next = callback ? std::make_shared<Binding>(std::move(callback), gate) : nullptr;
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

  binding->close();
}

void BagPluginInterface::register_callback(Callback&& callback) {
  BindingPtr previous;
  replace_callback(std::move(callback), previous);
}

void BagPluginInterface::flush_binding(const BindingPtr& binding) {
  if (binding->flushing.exchange(true, std::memory_order_acq_rel)) {
    return;
  }

  try {
    flush();
  } catch (...) {
    binding->flushing.store(false, std::memory_order_release);
    throw;
  }

  binding->flushing.store(false, std::memory_order_release);
}

void BagPluginInterface::flush_before_destroy(const BindingPtr& binding) {
  try {
    flush_binding(binding);
  } catch (const std::exception& e) {
    CLOG_E("BagPluginInterface: Failed to flush during teardown: %s.", e.what());
  } catch (...) {
    VLOG_E("BagPluginInterface: Failed to flush during teardown.");
  }
}

}  // namespace vlink
