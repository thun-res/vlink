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
 * @file vlink_python.cc
 * @brief Comprehensive nanobind bindings for VLink communication middleware.
 *
 * @details
 * This file exposes the full VLink Python surface while factoring the six
 * template communication primitives through reusable nanobind registration
 * helpers.
 */

#include <nanobind/nanobind.h>
#include <nanobind/stl/chrono.h>
#include <nanobind/stl/function.h>
#include <nanobind/stl/map.h>
#include <nanobind/stl/optional.h>
#include <nanobind/stl/pair.h>
#include <nanobind/stl/shared_ptr.h>
#include <nanobind/stl/string.h>
#include <nanobind/stl/string_view.h>
#include <nanobind/stl/tuple.h>
#include <nanobind/stl/unordered_map.h>
#include <nanobind/stl/unordered_set.h>
#include <nanobind/stl/vector.h>
#include <vlink/base/condition_variable.h>
#include <vlink/base/cpu_profiler.h>
#include <vlink/base/cpu_profiler_guard.h>
#include <vlink/base/deadline_timer.h>
#include <vlink/base/elapsed_timer.h>
#include <vlink/base/helpers.h>
#include <vlink/base/memory_pool.h>
#include <vlink/base/memory_resource.h>
#include <vlink/base/multi_loop.h>
#include <vlink/base/plugin.h>
#include <vlink/base/process.h>
#include <vlink/base/quantize.h>
#include <vlink/base/spin_lock.h>
#include <vlink/base/thread_pool.h>
#include <vlink/base/timer.h>
#include <vlink/base/uuid.h>
#include <vlink/base/wheel_timer.h>
#include <vlink/extension/bag_plugin_interface.h>
#include <vlink/extension/bag_reader.h>
#include <vlink/extension/bag_writer.h>
#include <vlink/extension/discovery_viewer.h>
#include <vlink/extension/qos_profile.h>
#include <vlink/extension/status_detail.h>
#include <vlink/extension/trigger_plugin_interface.h>
#include <vlink/extension/trigger_recorder.h>
#include <vlink/extension/url_remap.h>
#include <vlink/vlink.h>
#include <vlink/zerocopy/audio_frame.h>
#include <vlink/zerocopy/camera_frame.h>
#include <vlink/zerocopy/header.h>
#include <vlink/zerocopy/message_parser.h>
#include <vlink/zerocopy/object_array.h>
#include <vlink/zerocopy/occupancy_grid.h>
#include <vlink/zerocopy/point_cloud.h>
#include <vlink/zerocopy/proxy_data.h>
#include <vlink/zerocopy/raw_data.h>
#include <vlink/zerocopy/tensor.h>

#include <algorithm>
#include <csignal>
#include <cstring>
#include <filesystem>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace nb = nanobind;
using namespace nb::literals;  // NOLINT

namespace {

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

std::shared_ptr<GilSafePyFunction>& logger_console_callback_owner() {
  static auto* owner = new std::shared_ptr<GilSafePyFunction>();
  return *owner;
}

std::shared_ptr<GilSafePyFunction>& logger_file_callback_owner() {
  static auto* owner = new std::shared_ptr<GilSafePyFunction>();
  return *owner;
}

std::unordered_map<const vlink::Bytes*, size_t> bytes_export_counts;
size_t bytes_export_total{0};

void pin_bytes_storage(const vlink::Bytes& bytes) {
  ++bytes_export_counts[&bytes];
  ++bytes_export_total;
}

void unpin_bytes_storage(const vlink::Bytes& bytes) noexcept {
  auto iter = bytes_export_counts.find(&bytes);

  if VUNLIKELY (iter == bytes_export_counts.end()) {
    return;
  }

  if (--iter->second == 0) {
    bytes_export_counts.erase(iter);
  }

  --bytes_export_total;
}

[[nodiscard]] bool bytes_has_active_exports(const vlink::Bytes& bytes) {
  if VLIKELY (bytes_export_total == 0) {
    return false;
  }

  const auto iter = bytes_export_counts.find(&bytes);
  return iter != bytes_export_counts.end() && iter->second != 0;
}

void ensure_bytes_not_exported(const vlink::Bytes& bytes) {
  if VUNLIKELY (bytes_has_active_exports(bytes)) {
    PyErr_SetString(PyExc_BufferError, "Existing exports of data: Bytes object cannot be resized");
    throw nb::python_error();
  }
}

std::string_view utf8_prefix(std::string_view value, size_t max_size) noexcept {
  if (value.size() <= max_size) {
    return value;
  }

  size_t size = max_size;
  while (size != 0 && (static_cast<unsigned char>(value[size]) & 0xc0U) == 0x80U) {
    --size;
  }

  return value.substr(0, size);
}

inline void report_current_exception(const char* context) noexcept {
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

inline nb::str wide_string_to_python_str(const std::wstring& value) {
  PyObject* obj = PyUnicode_FromWideChar(value.c_str(), static_cast<Py_ssize_t>(value.size()));

  if VUNLIKELY (!obj) {
    throw nb::python_error();
  }

  return nb::steal<nb::str>(obj);
}

struct PyWideStringDeleter {
  void operator()(wchar_t* ptr) const noexcept {
    if VLIKELY (ptr) {
      PyMem_Free(ptr);
    }
  }
};

inline std::wstring python_str_to_wide_string(nb::str input) {
  Py_ssize_t size = 0;
  wchar_t* raw = PyUnicode_AsWideCharString(input.ptr(), &size);

  if VUNLIKELY (!raw) {
    throw nb::python_error();
  }

  std::unique_ptr<wchar_t, PyWideStringDeleter> wide(raw);
  return std::wstring(wide.get(), static_cast<size_t>(size));
}

class PythonBufferView {
 public:
  explicit PythonBufferView(nb::handle handle, int flags = PyBUF_SIMPLE) {
    if VUNLIKELY (PyObject_GetBuffer(handle.ptr(), &view_, flags) != 0) {
      throw nb::python_error();
    }

    valid_ = true;
  }

  PythonBufferView(const PythonBufferView&) = delete;
  PythonBufferView& operator=(const PythonBufferView&) = delete;

  ~PythonBufferView() {
    if VLIKELY (valid_) {
      PyBuffer_Release(&view_);
    }
  }

  [[nodiscard]] const uint8_t* data() const { return static_cast<const uint8_t*>(view_.buf); }
  [[nodiscard]] size_t size() const { return static_cast<size_t>(view_.len); }

 private:
  Py_buffer view_{};
  bool valid_ = false;
};

template <typename T, typename Enable = void>
struct PythonCodec {
  static T from_python(nb::handle handle) { return nb::cast<T>(handle); }
  static T from_python_owned(nb::handle handle) { return nb::cast<T>(handle); }
  static nb::object to_python(const T& value) { return nb::cast(value); }
};

template <>
struct PythonCodec<vlink::Bytes> {
  static vlink::Bytes from_python(nb::handle handle) {
    PythonBufferView view(handle);
    return vlink::Bytes::shallow_copy(view.data(), view.size());
  }

  static vlink::Bytes from_python_owned(nb::handle handle) {
    PythonBufferView view(handle);
    return vlink::Bytes::deep_copy(view.data(), view.size());
  }

  static nb::object to_python(const vlink::Bytes& value) { return nb::bytes(value.data(), value.size()); }
};

vlink::Frame frame_from_python(const vlink::Frame& frame) { return frame; }

struct PythonZerocopyMessageParser final {
  using Parser = vlink::zerocopy::MessageParser;

  bool parse(std::string_view serialized_type, nb::object input) {
    return this->parse_input(input, [&serialized_type](Parser& parser, const vlink::Bytes& bytes) {
      return parser.parse(serialized_type, bytes);
    });
  }

  bool parse_type(Parser::Type type, nb::object input) {
    return this->parse_input(input,
                             [type](Parser& parser, const vlink::Bytes& bytes) { return parser.parse(type, bytes); });
  }

  void clear() {
    parser.clear();
    input_owner = nb::object();
    input_data = nullptr;
    input_size = 0;
  }

  [[nodiscard]] bool backing_valid() const {
    if (!input_owner.is_valid()) {
      return false;
    }

    const auto& bytes = nb::cast<const vlink::Bytes&>(input_owner);

    if (bytes.data() != input_data || bytes.size() != input_size) {
      return false;
    }

    return parser.valid();
  }

  template <typename ParseFunction>
  bool parse_input(const nb::object& input, ParseFunction&& parse_function) {
    const auto& bytes = nb::cast<const vlink::Bytes&>(input);

    if VUNLIKELY (!parse_function(parser, bytes)) {
      input_owner = nb::object();
      input_data = nullptr;
      input_size = 0;
      return false;
    }

    input_owner = input;
    input_data = bytes.data();
    input_size = bytes.size();
    return true;
  }

  Parser parser;
  nb::object input_owner;
  const uint8_t* input_data{nullptr};
  size_t input_size{0};
};

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

auto& python_instance_owners() {
  static auto* owners = new std::unordered_map<PyObject*, PythonInstanceOwner>();
  return *owners;
}

void defer_python_owner_until_loop_idle(nb::object& owner, vlink::MessageLoop& loop) noexcept {
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
    // Retaining the reference is safer than destroying a running loop on its
    // own thread when a cleanup thread cannot be created.
  }
}

void delete_python_instance_owner(void* data) noexcept {
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

PythonInstanceOwner& python_instance_owner(nb::handle instance) {
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

template <typename MessageT, typename = void>
struct HasInternalData : std::false_type {};

template <typename MessageT>
struct HasInternalData<MessageT, std::void_t<decltype(std::declval<const MessageT&>().get_internal_data())>>
    : std::true_type {};

template <typename MessageT, typename = void>
struct HasData : std::false_type {};

template <typename MessageT>
struct HasData<MessageT, std::void_t<decltype(std::declval<const MessageT&>().data())>> : std::true_type {};

template <typename MessageT, typename = void>
struct HasFullClear : std::false_type {};

template <typename MessageT>
struct HasFullClear<MessageT, std::void_t<decltype(std::declval<MessageT&>().clear(true))>> : std::true_type {};

template <typename MessageT>
const uint8_t* zerocopy_payload_address(const MessageT& message) noexcept {
  if constexpr (HasInternalData<MessageT>::value) {
    return message.get_internal_data();
  } else if constexpr (HasData<MessageT>::value) {
    return message.data();
  } else {
    const vlink::Bytes raw = message.raw();

    if (raw.data() != nullptr) {
      return raw.data();
    }

    for (const std::string_view field : {message.url(), message.ser(), message.hostname()}) {
      if (field.data() != nullptr) {
        return reinterpret_cast<const uint8_t*>(field.data());
      }
    }

    return nullptr;
  }
}

template <typename MessageT>
void zerocopy_full_clear(MessageT& target) noexcept {
  if constexpr (HasFullClear<MessageT>::value) {
    target.clear(true);
  } else {
    target.clear();
  }
}

template <typename MessageT>
bool zerocopy_create(nb::object instance, size_t size) {
  const bool result = nb::cast<MessageT&>(instance).create(size);

  if (result) {
    unbind_python_instance_owner(instance);
  }

  return result;
}

template <typename MessageT>
void zerocopy_clear(nb::object instance) {
  nb::cast<MessageT&>(instance).clear();
  unbind_python_instance_owner(instance);
}

template <typename MessageT>
bool zerocopy_fill_data(nb::object instance, nb::handle data) {
  PythonBufferView view(data);
  const bool result = nb::cast<MessageT&>(instance).fill_data(const_cast<uint8_t*>(view.data()), view.size());

  if (result) {
    unbind_python_instance_owner(instance);
  }

  return result;
}

template <typename MessageT>
bool zerocopy_from_bytes(MessageT& target, const vlink::Bytes& bytes) {
  const uint8_t* previous_payload = target.is_owner() ? nullptr : zerocopy_payload_address(target);
  const uint8_t* input_data = bytes.data();
  const auto input_begin = reinterpret_cast<uintptr_t>(input_data);
  const auto uses_input_storage = [&](const uint8_t* payload) {
    const auto payload_address = reinterpret_cast<uintptr_t>(payload);
    return payload != nullptr && input_data != nullptr && payload_address >= input_begin &&
           payload_address - input_begin < bytes.size();
  };
  const bool reuses_input_storage = uses_input_storage(previous_payload);

  const auto bind_input = [&]() {
    try {
      nb::object instance = nb::find(&target);
      nb::object input = nb::find(const_cast<vlink::Bytes*>(&bytes));
      bind_python_bytes_owner(instance, std::move(input), bytes);
    } catch (...) {
      zerocopy_full_clear(target);
      throw;
    }
  };

  if VUNLIKELY (!(target << bytes)) {
    const uint8_t* current_payload = target.is_owner() ? nullptr : zerocopy_payload_address(target);

    if (target.is_owner() || current_payload == nullptr) {
      nb::object instance = nb::find(&target);
      unbind_python_instance_owner(instance);
    } else if (uses_input_storage(current_payload)) {
      if (!reuses_input_storage) {
        bind_input();
      }
    } else if (current_payload != previous_payload) {
      nb::object instance = nb::find(&target);
      unbind_python_instance_owner(instance);
    }

    return false;
  }

  if (target.is_owner()) {
    nb::object instance = nb::find(&target);
    unbind_python_instance_owner(instance);
  } else if (!reuses_input_storage) {
    bind_input();
  }

  return true;
}

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

struct PythonCallbackScope;
thread_local PythonCallbackScope* current_python_callback_scope{nullptr};

struct PythonCallbackScope final {
  explicit PythonCallbackScope(const std::shared_ptr<PythonCallbackActivity>& activity, const void* owner = nullptr,
                               const void* kind = nullptr)
      : activity(activity), owner(owner), kind(kind), previous(current_python_callback_scope) {
    activity->enter();
    current_python_callback_scope = this;
  }

  PythonCallbackScope(const PythonCallbackScope&) = delete;
  PythonCallbackScope& operator=(const PythonCallbackScope&) = delete;

  ~PythonCallbackScope() {
    current_python_callback_scope = previous;
    activity->leave();
  }

  std::shared_ptr<PythonCallbackActivity> activity;
  const void* owner;
  const void* kind;
  PythonCallbackScope* previous;
};

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

const void* python_keyboard_callback_owner() noexcept {
  static const char kOwner{};
  return &kOwner;
}

const void* python_logger_callback_owner() noexcept {
  static const char kOwner{};
  return &kOwner;
}

const void* python_node_status_callback_kind() noexcept {
  static const char kKind{};
  return &kKind;
}

const void* python_bag_writer_schema_callback_kind() noexcept {
  static const char kKind{};
  return &kKind;
}

const void* python_bag_writer_split_callback_kind() noexcept {
  static const char kKind{};
  return &kKind;
}

const void* python_bag_reader_output_callback_kind() noexcept {
  static const char kKind{};
  return &kKind;
}

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
    // Retaining the reference is safer than destroying its native owner from
    // inside the callback that owner is currently running.
  }
}

auto& python_pre_destroy_hooks() {
  static auto* hooks = new std::unordered_set<PyObject*>();
  return *hooks;
}

auto& python_native_finalizing() {
  static auto* owners = new std::unordered_set<const void*>();
  return *owners;
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

template <typename LoopT, typename Cleanup>
nb::object cast_shared_message_loop(std::shared_ptr<LoopT> owner, Cleanup cleanup) {
  if (!owner) {
    return nb::none();
  }

  nb::object instance = nb::cast(owner);
  std::weak_ptr<LoopT> weak_owner = owner;
  auto& hooks = python_pre_destroy_hooks();
  auto [iter, inserted] = hooks.insert(instance.ptr());

  if (!inserted) {
    return instance;
  }

  PyObject* key = instance.ptr();
  LoopT* native = owner.get();

  try {
    nb::module_::import_("weakref").attr("finalize")(
        instance, nb::cpp_function([key, native, weak_owner, cleanup = std::move(cleanup)]() noexcept {
          python_pre_destroy_hooks().erase(key);
          auto retained = weak_owner.lock();

          // The nanobind holder and this temporary reference are the only
          // owners when destroying the native object is imminent.
          if (!retained || retained.use_count() != 2 || !Py_IsInitialized()) {
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

  return instance;
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

template <typename NativeT, typename MsgT, typename Codec = PythonCodec<MsgT>>
auto make_owned_value_callback(NativeT* native, nb::callable py_cb, const char* context) {
  auto activity = std::make_shared<PythonCallbackActivity>();
  auto cb = std::make_shared<GilSafePyFunction>(std::move(py_cb));

  return [native, activity, cb = std::move(cb), context](const MsgT& value) {
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
      cb->fn(Codec::to_python(value));
    } catch (std::exception&) {
      report_current_exception(context);
    }

    defer_last_python_callback_owner(owner, activity);
  };
}

template <typename NativeT>
auto make_owned_connect_callback(NativeT* native, nb::callable py_cb, const char* context) {
  auto activity = std::make_shared<PythonCallbackActivity>();
  auto cb = std::make_shared<GilSafePyFunction>(std::move(py_cb));

  return [native, activity, cb = std::move(cb), context](bool connected) {
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
      cb->fn(connected);
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

struct PythonProcess final {
  PythonProcess() : activity(std::make_shared<PythonCallbackActivity>()), process(std::make_unique<vlink::Process>()) {}

  PythonProcess(const PythonProcess&) = delete;
  PythonProcess& operator=(const PythonProcess&) = delete;

  ~PythonProcess() { destroy_python_callback_owner(process, activity); }

  vlink::Process* operator->() const { return process.get(); }

  std::shared_ptr<PythonCallbackActivity> activity;
  std::unique_ptr<vlink::Process> process;
};

void acquire_spin_lock(vlink::SpinLock& lock) {
  if (lock.try_lock()) {
    return;
  }

  nb::gil_scoped_release release;
  lock.lock();
}

inline auto make_security_callback(nb::callable py_cb, const char* context) {
  auto cb = std::make_shared<GilSafePyFunction>(std::move(py_cb));

  return [cb = std::move(cb), context](const vlink::Bytes& in, vlink::Bytes& out) -> bool {
    if VUNLIKELY (!Py_IsInitialized()) {
      return false;
    }

    nb::gil_scoped_acquire gil;

    try {
      nb::object result = cb->fn(PythonCodec<vlink::Bytes>::to_python(in));

      if VUNLIKELY (result.is_none()) {
        return false;
      }

      out = PythonCodec<vlink::Bytes>::from_python_owned(result);
      return true;
    } catch (std::exception&) {
      report_current_exception(context);
      return false;
    }
  };
}

inline nb::dict status_to_dict(const vlink::Status::BasePtr& status) {
  nb::dict d;

  if VUNLIKELY (!status) {
    return d;
  }

  const auto type = status->get_type();
  d["type"] = static_cast<int>(type);
  d["status_type"] = type;
  d["description"] = status->get_string();

  auto put_handle = [&d](const char* key, vlink::Status::InstanceHandle handle) {
    if VUNLIKELY (handle == nullptr) {
      d[key] = nb::none();
    } else {
      d[key] = reinterpret_cast<uintptr_t>(handle);
    }
  };

  switch (type) {
    case vlink::Status::kPublicationMatched: {
      const auto publication_matched = std::static_pointer_cast<vlink::Status::PublicationMatched>(status);
      d["total_count"] = publication_matched->total_count;
      d["total_count_change"] = publication_matched->total_count_change;
      d["current_count"] = publication_matched->current_count;
      d["current_count_change"] = publication_matched->current_count_change;
      put_handle("last_subscription_handle", publication_matched->last_subscription_handle);
      break;
    }
    case vlink::Status::kOfferedDeadlineMissed: {
      const auto offered_deadline_missed = std::static_pointer_cast<vlink::Status::OfferedDeadlineMissed>(status);
      d["total_count"] = offered_deadline_missed->total_count;
      d["total_count_change"] = offered_deadline_missed->total_count_change;
      put_handle("last_instance_handle", offered_deadline_missed->last_instance_handle);
      break;
    }
    case vlink::Status::kOfferedIncompatibleQos: {
      const auto offered_incompatible_qos = std::static_pointer_cast<vlink::Status::OfferedIncompatibleQos>(status);
      d["total_count"] = offered_incompatible_qos->total_count;
      d["total_count_change"] = offered_incompatible_qos->total_count_change;
      d["last_policy_id"] = offered_incompatible_qos->last_policy_id;
      break;
    }
    case vlink::Status::kLivelinessLost: {
      const auto liveliness_lost = std::static_pointer_cast<vlink::Status::LivelinessLost>(status);
      d["total_count"] = liveliness_lost->total_count;
      d["total_count_change"] = liveliness_lost->total_count_change;
      break;
    }
    case vlink::Status::kSubscriptionMatched: {
      const auto subscription_matched = std::static_pointer_cast<vlink::Status::SubscriptionMatched>(status);
      d["total_count"] = subscription_matched->total_count;
      d["total_count_change"] = subscription_matched->total_count_change;
      d["current_count"] = subscription_matched->current_count;
      d["current_count_change"] = subscription_matched->current_count_change;
      put_handle("last_publication_handle", subscription_matched->last_publication_handle);
      break;
    }
    case vlink::Status::kRequestedDeadlineMissed: {
      const auto requested_deadline_missed = std::static_pointer_cast<vlink::Status::RequestedDeadlineMissed>(status);
      d["total_count"] = requested_deadline_missed->total_count;
      d["total_count_change"] = requested_deadline_missed->total_count_change;
      put_handle("last_instance_handle", requested_deadline_missed->last_instance_handle);
      break;
    }
    case vlink::Status::kLivelinessChanged: {
      const auto liveliness_changed = std::static_pointer_cast<vlink::Status::LivelinessChanged>(status);
      d["alive_count"] = liveliness_changed->alive_count;
      d["not_alive_count"] = liveliness_changed->not_alive_count;
      d["alive_count_change"] = liveliness_changed->alive_count_change;
      d["not_alive_count_change"] = liveliness_changed->not_alive_count_change;
      put_handle("last_publication_handle", liveliness_changed->last_publication_handle);
      break;
    }
    case vlink::Status::kSampleRejected: {
      const auto sample_rejected = std::static_pointer_cast<vlink::Status::SampleRejected>(status);
      d["total_count"] = sample_rejected->total_count;
      d["total_count_change"] = sample_rejected->total_count_change;
      d["last_reason"] = static_cast<int>(sample_rejected->last_reason);
      put_handle("last_instance_handle", sample_rejected->last_instance_handle);
      break;
    }
    case vlink::Status::kRequestedIncompatibleQos: {
      const auto requested_incompatible_qos = std::static_pointer_cast<vlink::Status::RequestedIncompatibleQos>(status);
      d["total_count"] = requested_incompatible_qos->total_count;
      d["total_count_change"] = requested_incompatible_qos->total_count_change;
      d["last_policy_id"] = requested_incompatible_qos->last_policy_id;
      break;
    }
    case vlink::Status::kSampleLost: {
      const auto sample_lost = std::static_pointer_cast<vlink::Status::SampleLost>(status);
      d["total_count"] = sample_lost->total_count;
      d["total_count_change"] = sample_lost->total_count_change;
      break;
    }
    default: {
      break;
    }
  }

  return d;
}

int bytes_getbuffer(PyObject* obj, Py_buffer* view, int flags) {
  auto* bytes = nb::inst_ptr<vlink::Bytes>(nb::handle(obj));

  try {
    pin_bytes_storage(*bytes);
  } catch (const std::bad_alloc&) {
    PyErr_NoMemory();
    return -1;
  } catch (const std::exception& error) {
    PyErr_SetString(PyExc_RuntimeError, error.what());
    return -1;
  } catch (...) {
    PyErr_SetString(PyExc_RuntimeError, "Failed to pin Bytes storage");
    return -1;
  }

  const int result = PyBuffer_FillInfo(view, obj, bytes->data(), static_cast<Py_ssize_t>(bytes->size()), 0, flags);

  if VUNLIKELY (result != 0) {
    unpin_bytes_storage(*bytes);
  }

  return result;
}

void bytes_releasebuffer(PyObject* obj, Py_buffer*) {
  auto* bytes = nb::inst_ptr<vlink::Bytes>(nb::handle(obj));
  unpin_bytes_storage(*bytes);
}

PyType_Slot bytes_type_slots[] = {
    {Py_bf_getbuffer, reinterpret_cast<void*>(bytes_getbuffer)},
    {Py_bf_releasebuffer, reinterpret_cast<void*>(bytes_releasebuffer)},
    {0, nullptr},
};

template <typename NodeT>
NodeT* make_url_node(const std::string& url, const std::string& ser_type, vlink::SchemaType schema_type,
                     bool auto_init) {
  const bool has_ser_type = !ser_type.empty();
  const bool has_schema_type = schema_type != vlink::SchemaType::kUnknown;

  if VUNLIKELY (!has_ser_type && has_schema_type) {
    throw nb::value_error("schema_type requires ser_type");
  }

  auto node = std::make_unique<NodeT>(url, vlink::InitType::kWithoutInit);

  if VLIKELY (has_ser_type) {
    node->set_ser_type(ser_type, schema_type);
  }

  if VLIKELY (auto_init) {
    node->init();
  }

  return node.release();
}

template <typename NodeT>
NodeT* make_url_security_node(const std::string& url, vlink::Security::Config sec_cfg, const std::string& ser_type,
                              vlink::SchemaType schema_type, bool auto_init) {
  const bool has_ser_type = !ser_type.empty();
  const bool has_schema_type = schema_type != vlink::SchemaType::kUnknown;

  if VUNLIKELY (!has_ser_type && has_schema_type) {
    throw nb::value_error("schema_type requires ser_type");
  }

  // SecurityXxx installs Security in its constructor, before this helper can call set_ser_type().

  if (has_ser_type && sec_cfg.advanced.aad_context.empty()) {
    const auto resolved_schema_type = has_schema_type ? schema_type : vlink::SchemaData::infer_ser_type(ser_type);
    sec_cfg.advanced.aad_context = url;
    sec_cfg.advanced.aad_context += "|";
    sec_cfg.advanced.aad_context += ser_type;
    sec_cfg.advanced.aad_context += "|";
    sec_cfg.advanced.aad_context += std::to_string(static_cast<uint32_t>(resolved_schema_type));
  }

  auto node = std::make_unique<NodeT>(url, std::move(sec_cfg), vlink::InitType::kWithoutInit);

  if VLIKELY (has_ser_type) {
    node->set_ser_type(ser_type, schema_type);
  }

  if VLIKELY (auto_init) {
    node->init();
  }

  return node.release();
}

template <typename NodeT>
void ensure_python_node_pre_destroy_hook(nb::handle instance, NodeT* node) {
  ensure_python_pre_destroy_hook(instance, node, [](NodeT& owner) { owner.deinit(); });
}

template <typename Class, typename NodeT>
void bind_node_common(Class& cls) {
  cls.def("init", &NodeT::init)
      .def("deinit",
           [](NodeT& self) {
             if VUNLIKELY (is_in_python_owner_callback(&self)) {
               throw std::runtime_error("Node.deinit() cannot be called from that node's active Python callback");
             }

             nb::gil_scoped_release release;
             return self.deinit();
           })
      .def("interrupt", &NodeT::interrupt)
      .def("has_inited", &NodeT::has_inited)
      .def("get_url", &NodeT::get_url)
      .def("get_ser_type", &NodeT::get_ser_type)
      .def("set_ser_type", &NodeT::set_ser_type, "ser_type"_a, "schema_type"_a = vlink::SchemaType::kUnknown,
           "Override serialization metadata. While a DDS node is initialized, its raw/CDR mode and CDR type name "
           "are immutable; call deinit(), update them, then init().")
      .def("get_schema_type", &NodeT::get_schema_type)
      .def("get_transport_type", &NodeT::get_transport_type)
      .def("set_property", &NodeT::set_property, "key"_a, "value"_a)
      .def("get_property", &NodeT::get_property, "key"_a)
      .def("set_discovery_enabled", &NodeT::set_discovery_enabled, "enable"_a)
      .def("get_discovery_enabled", &NodeT::get_discovery_enabled)
      .def("set_record_path", &NodeT::set_record_path, "path"_a)
      .def("set_ssl_options", &NodeT::set_ssl_options, "options"_a)
      .def("set_safety_quit", &NodeT::set_safety_quit, "enable"_a)
      .def("get_safety_quit", &NodeT::get_safety_quit)
      .def("is_support_loan", &NodeT::is_support_loan)
      .def(
          "loan", [](NodeT& self, int64_t size) { return self.loan(size); }, "size"_a)
      .def(
          "return_loan",
          [](NodeT& self, vlink::Bytes& bytes) {
            ensure_bytes_not_exported(bytes);
            return self.return_loan(bytes);
          },
          "bytes"_a)
      .def("suspend", &NodeT::suspend)
      .def("resume", &NodeT::resume)
      .def("is_suspend", &NodeT::is_suspend)
      .def(
          "attach",
          [](nb::object instance, nb::object loop) {
            auto& self = nb::cast<NodeT&>(instance);
            auto* loop_ptr = nb::cast<vlink::MessageLoop*>(loop);
            const bool result = self.attach(loop_ptr);

            if (self.get_message_loop() == loop_ptr) {
              ensure_python_node_pre_destroy_hook(instance, &self);
              bind_python_instance_owner(instance, std::move(loop));
            } else if (self.get_message_loop() == nullptr) {
              unbind_python_instance_owner(instance);
            }

            return result;
          },
          "loop"_a)
      .def("detach",
           [](nb::object instance) {
             auto& self = nb::cast<NodeT&>(instance);
             auto* attached_loop = self.get_message_loop();
             const bool called_from_loop = attached_loop != nullptr && attached_loop->is_in_same_thread();
             bool result;
             {
               nb::gil_scoped_release release;
               result = self.detach();
             }

             if (self.get_message_loop() == nullptr) {
               if (called_from_loop) {
                 unbind_python_instance_owner_after_loop_idle(instance, *attached_loop);
               } else {
                 unbind_python_instance_owner(instance);
               }
             }

             return result;
           })
      .def("get_message_loop", &NodeT::get_message_loop, nb::rv_policy::reference)
      .def(
          "get_abstract_node",
          [](const NodeT& self) -> nb::object {
            const auto* node = self.get_abstract_node();

            if VUNLIKELY (node == nullptr) {
              return nb::none();
            }

            return nb::int_(reinterpret_cast<uintptr_t>(node));
          },
          "Return the non-owning AbstractNode address, or None if unavailable.")
      .def("get_cpu_usage", &NodeT::get_cpu_usage)
      .def(
          "get_status",
          [](NodeT& self, vlink::Status::Type type) -> nb::object {
            auto status = self.get_status(type);

            if VUNLIKELY (!status) {
              return nb::none();
            }

            return nb::object(status_to_dict(status));
          },
          "type"_a)
      .def(
          "register_status_handler",
          [](nb::object instance, nb::callable callback) {
            auto& self = nb::cast<NodeT&>(instance);
            if VUNLIKELY (is_in_python_owner_callback(&self, python_node_status_callback_kind())) {
              throw std::runtime_error(
                  "Node status handler cannot be replaced from that node's active Python callback");
            }

            ensure_python_node_pre_destroy_hook(instance, &self);
            auto activity = std::make_shared<PythonCallbackActivity>();
            auto cb = std::make_shared<GilSafePyFunction>(std::move(callback));
            nb::gil_scoped_release release;
            self.register_status_handler([native = &self, activity, cb](const vlink::Status::BasePtr& status) {
              invoke_owned_python_callback(
                  native, activity, "vlink::register_status_handler", [&]() { cb->fn(status_to_dict(status)); },
                  python_node_status_callback_kind());
            });
          },
          "callback"_a);
}

template <typename Class, typename NodeT>
void bind_node_security_ctor(Class& cls) {
  cls.def(nb::new_([](const std::string& url, vlink::Security::Config sec_cfg, const std::string& ser_type,
                      vlink::SchemaType schema_type, bool auto_init) {
            return make_url_security_node<NodeT>(url, std::move(sec_cfg), ser_type, schema_type, auto_init);
          }),
          "url"_a, "sec_cfg"_a, "ser_type"_a = "", "schema_type"_a = vlink::SchemaType::kUnknown, "auto_init"_a = true);
}

template <typename PubT, typename MsgT, typename Codec = PythonCodec<MsgT>, bool SecurityNode = false>
void bind_publisher(nb::module_& m, const char* name, const char* doc) {
  auto cls = nb::class_<PubT>(m, name, doc, nb::is_weak_referenceable());
  if constexpr (SecurityNode) {
    bind_node_security_ctor<decltype(cls), PubT>(cls);
  } else {
    cls.def(nb::new_([](const std::string& url, const std::string& ser_type, vlink::SchemaType schema_type,
                        bool auto_init) { return make_url_node<PubT>(url, ser_type, schema_type, auto_init); }),
            "url"_a, "ser_type"_a = "", "schema_type"_a = vlink::SchemaType::kUnknown, "auto_init"_a = true);
  }
  bind_node_common<decltype(cls), PubT>(cls);
  cls.def(
         "detect_subscribers",
         [](nb::object instance, nb::callable callback) {
           auto& self = nb::cast<PubT&>(instance);
           ensure_python_node_pre_destroy_hook(instance, &self);
           self.detect_subscribers(
               make_owned_connect_callback(&self, std::move(callback), "vlink::Publisher.detect_subscribers"));
         },
         "callback"_a)
      .def(
          "wait_for_subscribers",
          [](PubT& self, int timeout_ms) {
            nb::gil_scoped_release release;
            return self.wait_for_subscribers(std::chrono::milliseconds(timeout_ms));
          },
          "timeout_ms"_a = 5000)
      .def("has_subscribers", &PubT::has_subscribers)
      .def(
          "publish",
          [](PubT& self, nb::handle data, bool force) {
            auto value = Codec::from_python_owned(data);
            nb::gil_scoped_release release;
            return self.publish(value, force);
          },
          "data"_a, "force"_a = false)
      .def(
          "publish_fbb",
          [](PubT& self, nb::handle data, bool force) {
            auto value = Codec::from_python_owned(data);
            nb::gil_scoped_release release;
            return self.publish(value, force);
          },
          "data"_a, "force"_a = false, "Publish a finished FlatBuffers byte buffer.")
      .def("mark_as_setter", &PubT::mark_as_setter)
      .def("__repr__",
           [name = std::string(name)](const PubT& self) { return name + "(url='" + self.get_url() + "')"; });
}

template <typename SubT, typename MsgT, typename Codec = PythonCodec<MsgT>, bool SecurityNode = false>
void bind_subscriber(nb::module_& m, const char* name, const char* doc) {
  auto cls = nb::class_<SubT>(m, name, doc, nb::is_weak_referenceable());
  if constexpr (SecurityNode) {
    bind_node_security_ctor<decltype(cls), SubT>(cls);
  } else {
    cls.def(nb::new_([](const std::string& url, const std::string& ser_type, vlink::SchemaType schema_type,
                        bool auto_init) { return make_url_node<SubT>(url, ser_type, schema_type, auto_init); }),
            "url"_a, "ser_type"_a = "", "schema_type"_a = vlink::SchemaType::kUnknown, "auto_init"_a = true);
  }
  bind_node_common<decltype(cls), SubT>(cls);
  cls.def(
         "listen",
         [](nb::object instance, nb::callable callback) {
           auto& self = nb::cast<SubT&>(instance);
           ensure_python_node_pre_destroy_hook(instance, &self);
           return self.listen(
               make_owned_value_callback<SubT, MsgT, Codec>(&self, std::move(callback), "vlink::Subscriber.listen"));
         },
         "callback"_a)
      .def("set_latency_and_lost_enabled", &SubT::set_latency_and_lost_enabled, "enable"_a)
      .def("is_latency_and_lost_enabled", &SubT::is_latency_and_lost_enabled)
      .def("get_latency", &SubT::get_latency)
      .def("get_lost", &SubT::get_lost)
      .def("mark_as_getter", &SubT::mark_as_getter)
      .def("__repr__",
           [name = std::string(name)](const SubT& self) { return name + "(url='" + self.get_url() + "')"; });
}

template <typename ServerT, typename ReqT, typename RespT, typename ReqCodec = PythonCodec<ReqT>,
          typename RespCodec = PythonCodec<RespT>, bool SecurityNode = false>
void bind_server(nb::module_& m, const char* name, const char* doc) {
  auto cls = nb::class_<ServerT>(m, name, doc, nb::is_weak_referenceable());
  if constexpr (SecurityNode) {
    bind_node_security_ctor<decltype(cls), ServerT>(cls);
  } else {
    cls.def(nb::new_([](const std::string& url, const std::string& ser_type, vlink::SchemaType schema_type,
                        bool auto_init) { return make_url_node<ServerT>(url, ser_type, schema_type, auto_init); }),
            "url"_a, "ser_type"_a = "", "schema_type"_a = vlink::SchemaType::kUnknown, "auto_init"_a = true);
  }
  bind_node_common<decltype(cls), ServerT>(cls);
  cls.def(
         "listen",
         [](nb::object instance, nb::callable callback) {
           auto& self = nb::cast<ServerT&>(instance);
           ensure_python_node_pre_destroy_hook(instance, &self);
           auto activity = std::make_shared<PythonCallbackActivity>();
           auto cb = std::make_shared<GilSafePyFunction>(std::move(callback));
           return self.listen([native = &self, activity, cb](const ReqT& req, RespT& resp) {
             invoke_owned_python_callback(native, activity, "vlink::Server.listen", [&]() {
               nb::object result = cb->fn(ReqCodec::to_python(req));

               if VLIKELY (!result.is_none()) {
                 resp = RespCodec::from_python_owned(result);
               }
             });
           });
         },
         "callback"_a, "callback(request) -> response or None")
      .def(
          "listen_for_reply",
          [](nb::object instance, nb::callable callback) {
            auto& self = nb::cast<ServerT&>(instance);
            ensure_python_node_pre_destroy_hook(instance, &self);
            auto activity = std::make_shared<PythonCallbackActivity>();
            auto cb = std::make_shared<GilSafePyFunction>(std::move(callback));
            return self.listen_for_reply([native = &self, activity, cb](uint64_t req_id, const ReqT& req) {
              invoke_owned_python_callback(native, activity, "vlink::Server.listen_for_reply",
                                           [&]() { cb->fn(req_id, ReqCodec::to_python(req)); });
            });
          },
          "callback"_a, "callback(req_id, request). Call reply(req_id, response) later")
      .def(
          "reply",
          [](ServerT& self, uint64_t req_id, nb::handle data) {
            return self.reply(req_id, RespCodec::from_python_owned(data));
          },
          "req_id"_a, "data"_a)
      .def("__repr__",
           [name = std::string(name)](const ServerT& self) { return name + "(url='" + self.get_url() + "')"; });
}

template <typename ServerT, typename ReqT, typename ReqCodec = PythonCodec<ReqT>, bool SecurityNode = false>
void bind_fire_forget_server(nb::module_& m, const char* name, const char* doc) {
  auto cls = nb::class_<ServerT>(m, name, doc, nb::is_weak_referenceable());
  if constexpr (SecurityNode) {
    bind_node_security_ctor<decltype(cls), ServerT>(cls);
  } else {
    cls.def(nb::new_([](const std::string& url, const std::string& ser_type, vlink::SchemaType schema_type,
                        bool auto_init) { return make_url_node<ServerT>(url, ser_type, schema_type, auto_init); }),
            "url"_a, "ser_type"_a = "", "schema_type"_a = vlink::SchemaType::kUnknown, "auto_init"_a = true);
  }
  bind_node_common<decltype(cls), ServerT>(cls);
  cls.def(
         "listen",
         [](nb::object instance, nb::callable callback) {
           auto& self = nb::cast<ServerT&>(instance);
           ensure_python_node_pre_destroy_hook(instance, &self);
           auto activity = std::make_shared<PythonCallbackActivity>();
           auto cb = std::make_shared<GilSafePyFunction>(std::move(callback));
           return self.listen([native = &self, activity, cb](const ReqT& req) {
             invoke_owned_python_callback(native, activity, "vlink::FireForgetServer.listen",
                                          [&]() { cb->fn(ReqCodec::to_python(req)); });
           });
         },
         "callback"_a, "callback(request) -> None")
      .def("__repr__",
           [name = std::string(name)](const ServerT& self) { return name + "(url='" + self.get_url() + "')"; });
}

template <typename ClientT, typename ReqT, typename RespT, typename ReqCodec = PythonCodec<ReqT>,
          typename RespCodec = PythonCodec<RespT>, bool SecurityNode = false>
void bind_client(nb::module_& m, const char* name, const char* doc) {
  auto cls = nb::class_<ClientT>(m, name, doc, nb::is_weak_referenceable());
  if constexpr (SecurityNode) {
    bind_node_security_ctor<decltype(cls), ClientT>(cls);
  } else {
    cls.def(nb::new_([](const std::string& url, const std::string& ser_type, vlink::SchemaType schema_type,
                        bool auto_init) { return make_url_node<ClientT>(url, ser_type, schema_type, auto_init); }),
            "url"_a, "ser_type"_a = "", "schema_type"_a = vlink::SchemaType::kUnknown, "auto_init"_a = true);
  }
  bind_node_common<decltype(cls), ClientT>(cls);
  cls.def(
         "detect_connected",
         [](nb::object instance, nb::callable callback) {
           auto& self = nb::cast<ClientT&>(instance);
           ensure_python_node_pre_destroy_hook(instance, &self);
           self.detect_connected(
               make_owned_connect_callback(&self, std::move(callback), "vlink::Client.detect_connected"));
         },
         "callback"_a)
      .def(
          "wait_for_connected",
          [](ClientT& self, int timeout_ms) {
            nb::gil_scoped_release release;
            return self.wait_for_connected(std::chrono::milliseconds(timeout_ms));
          },
          "timeout_ms"_a = 5000)
      .def("is_connected", &ClientT::is_connected)
      .def(
          "invoke",
          [](ClientT& self, nb::handle data, int timeout_ms) -> nb::object {
            auto req = ReqCodec::from_python_owned(data);
            std::optional<RespT> res;
            {
              nb::gil_scoped_release release;
              res = self.invoke(req, std::chrono::milliseconds(timeout_ms));
            }

            if VLIKELY (res.has_value()) {
              return RespCodec::to_python(*res);
            }

            return nb::none();
          },
          "data"_a, "timeout_ms"_a = 5000)
      .def(
          "invoke_async",
          [](nb::object instance, nb::handle data, nb::callable callback) {
            auto& self = nb::cast<ClientT&>(instance);
            ensure_python_node_pre_destroy_hook(instance, &self);
            auto req = ReqCodec::from_python_owned(data);
            auto activity = std::make_shared<PythonCallbackActivity>();
            auto cb = std::make_shared<GilSafePyFunction>(std::move(callback));
            return self.invoke(req, [native = &self, activity, cb](const RespT& resp) {
              invoke_owned_python_callback(native, activity, "vlink::Client.invoke_async",
                                           [&]() { cb->fn(RespCodec::to_python(resp)); });
            });
          },
          "data"_a, "callback"_a)
      .def(
          "async_invoke",
          [](nb::object instance, nb::handle data) {
            auto& self = nb::cast<ClientT&>(instance);
            ensure_python_node_pre_destroy_hook(instance, &self);
            auto req = ReqCodec::from_python_owned(data);
            nb::object py_future = nb::module_::import_("concurrent.futures").attr("Future")();
            auto activity = std::make_shared<PythonCallbackActivity>();
            auto future_ref = std::make_shared<GilSafePyObject>(nb::object(py_future));
            const bool accepted = self.invoke(req, [native = &self, activity, future_ref](const RespT& resp) {
              invoke_owned_python_callback(native, activity, "vlink::Client.async_invoke.set_result",
                                           [&]() { future_ref->obj.attr("set_result")(RespCodec::to_python(resp)); });
            });

            if VUNLIKELY (!accepted) {
              nb::object exc =
                  nb::module_::import_("builtins").attr("RuntimeError")("VLink async_invoke failed to submit request");
              py_future.attr("set_exception")(exc);
            }

            return py_future;
          },
          "data"_a, "Return a concurrent.futures.Future resolved with the response bytes.")
      .def("__repr__", [name = std::string(name)](const ClientT& self) {
        const char* connected = "Unknown";
        if (self.has_inited()) {
          connected = self.is_connected() ? "True" : "False";
        }
        return name + "(url='" + self.get_url() + "', connected=" + connected + ")";
      });
}

template <typename ClientT, typename ReqT, typename ReqCodec = PythonCodec<ReqT>, bool SecurityNode = false>
void bind_fire_forget_client(nb::module_& m, const char* name, const char* doc) {
  auto cls = nb::class_<ClientT>(m, name, doc, nb::is_weak_referenceable());
  if constexpr (SecurityNode) {
    bind_node_security_ctor<decltype(cls), ClientT>(cls);
  } else {
    cls.def(nb::new_([](const std::string& url, const std::string& ser_type, vlink::SchemaType schema_type,
                        bool auto_init) { return make_url_node<ClientT>(url, ser_type, schema_type, auto_init); }),
            "url"_a, "ser_type"_a = "", "schema_type"_a = vlink::SchemaType::kUnknown, "auto_init"_a = true);
  }
  bind_node_common<decltype(cls), ClientT>(cls);
  cls.def(
         "detect_connected",
         [](nb::object instance, nb::callable callback) {
           auto& self = nb::cast<ClientT&>(instance);
           ensure_python_node_pre_destroy_hook(instance, &self);
           self.detect_connected(
               make_owned_connect_callback(&self, std::move(callback), "vlink::FireForgetClient.detect_connected"));
         },
         "callback"_a)
      .def(
          "wait_for_connected",
          [](ClientT& self, int timeout_ms) {
            nb::gil_scoped_release release;
            return self.wait_for_connected(std::chrono::milliseconds(timeout_ms));
          },
          "timeout_ms"_a = 5000)
      .def("is_connected", &ClientT::is_connected)
      .def(
          "send",
          [](ClientT& self, nb::handle data) {
            auto req = ReqCodec::from_python_owned(data);
            nb::gil_scoped_release release;
            return self.send(req);
          },
          "data"_a)
      .def("__repr__", [name = std::string(name)](const ClientT& self) {
        const char* connected = "Unknown";
        if (self.has_inited()) {
          connected = self.is_connected() ? "True" : "False";
        }
        return name + "(url='" + self.get_url() + "', connected=" + connected + ")";
      });
}

template <typename SetterT, typename ValueT, typename Codec = PythonCodec<ValueT>, bool SecurityNode = false>
void bind_setter(nb::module_& m, const char* name, const char* doc) {
  auto cls = nb::class_<SetterT>(m, name, doc, nb::is_weak_referenceable());
  if constexpr (SecurityNode) {
    bind_node_security_ctor<decltype(cls), SetterT>(cls);
  } else {
    cls.def(nb::new_([](const std::string& url, const std::string& ser_type, vlink::SchemaType schema_type,
                        bool auto_init) { return make_url_node<SetterT>(url, ser_type, schema_type, auto_init); }),
            "url"_a, "ser_type"_a = "", "schema_type"_a = vlink::SchemaType::kUnknown, "auto_init"_a = true);
  }
  bind_node_common<decltype(cls), SetterT>(cls);
  cls.def(
         "set", [](SetterT& self, nb::handle data) { self.set(Codec::from_python_owned(data)); }, "data"_a)
      .def("mark_as_publisher", &SetterT::mark_as_publisher)
      .def("__repr__",
           [name = std::string(name)](const SetterT& self) { return name + "(url='" + self.get_url() + "')"; });
}

template <typename GetterT, typename ValueT, typename Codec = PythonCodec<ValueT>, bool SecurityNode = false>
void bind_getter(nb::module_& m, const char* name, const char* doc) {
  auto cls = nb::class_<GetterT>(m, name, doc, nb::is_weak_referenceable());
  if constexpr (SecurityNode) {
    bind_node_security_ctor<decltype(cls), GetterT>(cls);
  } else {
    cls.def(nb::new_([](const std::string& url, const std::string& ser_type, vlink::SchemaType schema_type,
                        bool auto_init) { return make_url_node<GetterT>(url, ser_type, schema_type, auto_init); }),
            "url"_a, "ser_type"_a = "", "schema_type"_a = vlink::SchemaType::kUnknown, "auto_init"_a = true);
  }
  bind_node_common<decltype(cls), GetterT>(cls);
  cls.def(
         "listen",
         [](nb::object instance, nb::callable callback) {
           auto& self = nb::cast<GetterT&>(instance);
           ensure_python_node_pre_destroy_hook(instance, &self);
           return self.listen(
               make_owned_value_callback<GetterT, ValueT, Codec>(&self, std::move(callback), "vlink::Getter.listen"));
         },
         "callback"_a)
      .def("get",
           [](GetterT& self) -> nb::object {
             auto result = self.get();

             if VLIKELY (result.has_value()) {
               return Codec::to_python(*result);
             }

             return nb::none();
           })
      .def(
          "wait_for_value",
          [](GetterT& self, int timeout_ms) {
            nb::gil_scoped_release release;
            return self.wait_for_value(std::chrono::milliseconds(timeout_ms));
          },
          "timeout_ms"_a = 5000)
      .def("set_change_reporting", &GetterT::set_change_reporting, "enable"_a)
      .def("get_change_reporting", &GetterT::get_change_reporting)
      .def("set_latency_and_lost_enabled", &GetterT::set_latency_and_lost_enabled, "enable"_a)
      .def("is_latency_and_lost_enabled", &GetterT::is_latency_and_lost_enabled)
      .def("get_latency", &GetterT::get_latency)
      .def("get_lost", &GetterT::get_lost)
      .def("mark_as_subscriber", &GetterT::mark_as_subscriber)
      .def("__repr__",
           [name = std::string(name)](const GetterT& self) { return name + "(url='" + self.get_url() + "')"; });
}

}  // namespace

NB_MODULE(_vlink_nanobind, m) {
  m.doc() = "VLink: Transport-agnostic pub/sub, field, and RPC communication middleware";

  nb::enum_<vlink::ImplType>(m, "ImplType", "Node role type")
      .value("Unknown", vlink::kUnknownImplType)
      .value("Publisher", vlink::kPublisher)
      .value("Subscriber", vlink::kSubscriber)
      .value("Setter", vlink::kSetter)
      .value("Getter", vlink::kGetter)
      .value("Server", vlink::kServer)
      .value("Client", vlink::kClient);

  nb::enum_<vlink::TransportType>(m, "TransportType", "Transport backend")
      .value("Unknown", vlink::TransportType::kUnknown)
      .value("Intra", vlink::TransportType::kIntra)
      .value("Shm", vlink::TransportType::kShm)
      .value("Shm2", vlink::TransportType::kShm2)
      .value("Zenoh", vlink::TransportType::kZenoh)
      .value("Dds", vlink::TransportType::kDds)
      .value("Ddsc", vlink::TransportType::kDdsc)
      .value("Ddsr", vlink::TransportType::kDdsr)
      .value("Someip", vlink::TransportType::kSomeip)
      .value("Mqtt", vlink::TransportType::kMqtt)
      .value("Fdbus", vlink::TransportType::kFdbus);

  nb::enum_<vlink::InitType>(m, "InitType")
      .value("WithoutInit", vlink::InitType::kWithoutInit)
      .value("WithInit", vlink::InitType::kWithInit);

  nb::enum_<vlink::SecurityType>(m, "SecurityType")
      .value("WithoutSecurity", vlink::SecurityType::kWithoutSecurity)
      .value("WithSecurity", vlink::SecurityType::kWithSecurity);

  nb::enum_<vlink::ActionType>(m, "ActionType", "Message recording action")
      .value("Unknown", vlink::ActionType::kUnknownAction)
      .value("ClientRequest", vlink::ActionType::kClientRequest)
      .value("ClientResponse", vlink::ActionType::kClientResponse)
      .value("ServerRequest", vlink::ActionType::kServerRequest)
      .value("ServerResponse", vlink::ActionType::kServerResponse)
      .value("Publish", vlink::ActionType::kPublish)
      .value("Subscribe", vlink::ActionType::kSubscribe)
      .value("Set", vlink::ActionType::kSet)
      .value("Get", vlink::ActionType::kGet);

  nb::enum_<vlink::SchemaType>(m, "SchemaType", nb::is_arithmetic(), "Coarse schema family")
      .value("Unknown", vlink::SchemaType::kUnknown)
      .value("Protobuf", vlink::SchemaType::kProtobuf)
      .value("Flatbuffers", vlink::SchemaType::kFlatbuffers)
      .value("Raw", vlink::SchemaType::kRaw)
      .value("ZeroCopy", vlink::SchemaType::kZeroCopy)
      .value("Cdr", vlink::SchemaType::kCdr);

  auto quantize = m.def_submodule("quantize", "Linear numeric quantization helpers");
  quantize.def(
      "encode_int16",
      [](double quant_min, double quant_max, double value) {
        return static_cast<int>(vlink::Quantize::encode<int16_t>(quant_min, quant_max, value));
      },
      "quant_min"_a, "quant_max"_a, "value"_a);
  quantize.def(
      "encode_int16",
      [](double extent, double value) { return static_cast<int>(vlink::Quantize::encode<int16_t>(extent, value)); },
      "extent"_a, "value"_a);
  quantize.def(
      "decode_int16",
      [](double quant_min, double quant_max, int value) {
        if VUNLIKELY (value < static_cast<int>(std::numeric_limits<int16_t>::lowest()) ||
                      value > static_cast<int>(std::numeric_limits<int16_t>::max())) {
          throw nb::value_error("value is outside int16 range");
        }

        return vlink::Quantize::decode<double>(quant_min, quant_max, static_cast<int16_t>(value));
      },
      "quant_min"_a, "quant_max"_a, "value"_a);
  quantize.def(
      "decode_int16",
      [](double extent, int value) {
        if VUNLIKELY (value < static_cast<int>(std::numeric_limits<int16_t>::lowest()) ||
                      value > static_cast<int>(std::numeric_limits<int16_t>::max())) {
          throw nb::value_error("value is outside int16 range");
        }

        return vlink::Quantize::decode<double>(extent, static_cast<int16_t>(value));
      },
      "extent"_a, "value"_a);

  nb::class_<vlink::Bytes>(m, "Bytes", nb::type_slots(bytes_type_slots), "Versatile byte buffer")
      .def(nb::init<>())
      .def_static("init_memory_pool", &vlink::Bytes::init_memory_pool)
      .def_static("release_memory_pool", &vlink::Bytes::release_memory_pool)
      .def_static(
          "create", [](size_t size, uint8_t offset) { return vlink::Bytes::create(size, offset); }, "size"_a,
          "offset"_a = static_cast<uint8_t>(0))
      .def_static(
          "from_bytes",
          [](nb::handle data, uint8_t offset) {
            PythonBufferView view(data);
            return vlink::Bytes::deep_copy(view.data(), view.size(), offset);
          },
          "data"_a, "offset"_a = static_cast<uint8_t>(0))
      .def_static(
          "from_string", [](const std::string& s, uint8_t offset) { return vlink::Bytes::from_string(s, offset); },
          "s"_a, "offset"_a = static_cast<uint8_t>(0))
      .def_static(
          "from_user_input",
          [](const std::string& s) {
            bool ok = false;
            auto bytes = vlink::Bytes::from_user_input(s, &ok);

            if VUNLIKELY (!ok) {
              throw std::runtime_error("Invalid hex/binary input");
            }

            return bytes;
          },
          "hex_or_bin"_a)
      .def_static(
          "encode_to_base64",
          [](nb::handle data) {
            PythonBufferView view(data);
            auto b = vlink::Bytes::shallow_copy(view.data(), view.size());
            return vlink::Bytes::encode_to_base64(b);
          },
          "bytes"_a)
      .def_static("decode_from_base64", &vlink::Bytes::decode_from_base64, "str"_a)
      .def_static(
          "get_crc_32",
          [](nb::handle data) {
            PythonBufferView view(data);
            auto b = vlink::Bytes::shallow_copy(view.data(), view.size());
            return vlink::Bytes::get_crc_32(b);
          },
          "bytes"_a)
      .def_static(
          "get_crc_64",
          [](nb::handle data) {
            PythonBufferView view(data);
            auto b = vlink::Bytes::shallow_copy(view.data(), view.size());
            return vlink::Bytes::get_crc_64(b);
          },
          "bytes"_a)
      .def_static(
          "compress",
          [](nb::handle data, bool high_ratio) {
            PythonBufferView view(data);
            return vlink::Bytes::compress_data(view.data(), view.size(), high_ratio);
          },
          "bytes"_a, "high_ratio"_a = false)
      .def_static(
          "uncompress",
          [](nb::handle data, bool check_valid) {
            PythonBufferView view(data);
            return vlink::Bytes::uncompress_data(view.data(), view.size(), check_valid);
          },
          "bytes"_a, "check_valid"_a = true)
      .def_static(
          "is_compress_data",
          [](nb::handle data) {
            PythonBufferView view(data);
            return vlink::Bytes::is_compress_data(view.data(), view.size());
          },
          "bytes"_a)
      .def_static(
          "reverse_order",
          [](nb::handle data) {
            PythonBufferView view(data);
            auto b = vlink::Bytes::shallow_copy(view.data(), view.size());
            return vlink::Bytes::reverse_order(b);
          },
          "bytes"_a)
      .def_static(
          "convert_to_hex_str",
          [](nb::handle data) {
            PythonBufferView view(data);
            return vlink::Bytes::convert_to_hex_str(view.data(), view.size());
          },
          "bytes"_a)
      .def_static("is_little_endian", &vlink::Bytes::is_little_endian)
      .def_static("is_big_endian", &vlink::Bytes::is_big_endian)
      .def_static("stack_size", &vlink::Bytes::stack_size)
      .def("size", &vlink::Bytes::size)
      .def("real_size", &vlink::Bytes::real_size)
      .def("capacity", &vlink::Bytes::capacity)
      .def("offset", &vlink::Bytes::offset)
      .def("empty", &vlink::Bytes::empty)
      .def("is_owner", &vlink::Bytes::is_owner)
      .def("is_loaned", &vlink::Bytes::is_loaned)
      .def("is_ptr", &vlink::Bytes::is_ptr)
      .def("clear",
           [](vlink::Bytes& self) {
             ensure_bytes_not_exported(self);
             self.clear();
           })
      .def(
          "resize",
          [](vlink::Bytes& self, size_t size) {
            if VUNLIKELY (bytes_export_total != 0 && self.is_owner() && size != self.size()) {
              ensure_bytes_not_exported(self);
            }

            return self.resize(size);
          },
          "size"_a)
      .def(
          "reserve",
          [](vlink::Bytes& self, size_t capacity) {
            if VUNLIKELY (bytes_export_total != 0 && self.is_owner() && capacity > self.capacity()) {
              ensure_bytes_not_exported(self);
            }

            return self.reserve(capacity);
          },
          "capacity"_a)
      .def(
          "shrink_to",
          [](vlink::Bytes& self, size_t size) {
            if VUNLIKELY (bytes_export_total != 0 && self.is_owner() && size < self.size()) {
              ensure_bytes_not_exported(self);
            }

            return self.shrink_to(size);
          },
          "size"_a)
      .def(
          "deep_copy_self",
          [](vlink::Bytes& self) -> vlink::Bytes& {
            if VUNLIKELY (bytes_export_total != 0 && !self.is_owner() && !self.empty()) {
              ensure_bytes_not_exported(self);
            }

            return self.deep_copy_self();
          },
          nb::rv_policy::reference_internal)
      .def("to_bytes", [](const vlink::Bytes& self) { return nb::bytes(self.data(), self.size()); })
      .def("to_string", [](const vlink::Bytes& self) { return self.to_string(); })
      .def("to_raw_data", [](const vlink::Bytes& self) { return self.to_raw_data(); })
      .def("hex", [](const vlink::Bytes& self) { return vlink::Bytes::convert_to_hex_str(self.data(), self.size()); })
      .def("__len__", [](const vlink::Bytes& self) { return self.size(); })
      .def("__bool__", [](const vlink::Bytes& self) { return !self.empty(); })
      .def("__bytes__", [](const vlink::Bytes& self) { return nb::bytes(self.data(), self.size()); })
      .def("__eq__", [](const vlink::Bytes& a, const vlink::Bytes& b) { return a == b; })
      .def("__ne__", [](const vlink::Bytes& a, const vlink::Bytes& b) { return a != b; })
      .def("__getitem__",
           [](const vlink::Bytes& self, Py_ssize_t i) -> uint8_t {
             if (i < 0) {
               i += static_cast<Py_ssize_t>(self.size());
             }

             if VUNLIKELY (i < 0 || static_cast<size_t>(i) >= self.size()) {
               throw nb::index_error();
             }

             return self.data()[static_cast<size_t>(i)];
           })
      .def("__repr__", [](const vlink::Bytes& self) {
        std::string repr = "Bytes(size=" + std::to_string(self.size());

        if (self.is_owner()) {
          repr += ", owned";
        } else if (self.is_loaned()) {
          repr += ", loaned";
        } else {
          repr += ", shallow";
        }

        return repr + ")";
      });

  nb::class_<vlink::Frame>(m, "Frame", "One bag record or replay message")
      .def(nb::init<>())
      .def_rw("timestamp", &vlink::Frame::timestamp)
      .def_rw("url", &vlink::Frame::url)
      .def_rw("ser_type", &vlink::Frame::ser_type)
      .def_rw("schema_type", &vlink::Frame::schema_type)
      .def_rw("action_type", &vlink::Frame::action_type)
      .def_prop_rw(
          "data", [](const vlink::Frame& self) { return PythonCodec<vlink::Bytes>::to_python(self.data); },
          [](vlink::Frame& self, nb::handle data) { self.data = PythonCodec<vlink::Bytes>::from_python_owned(data); })
      .def("__repr__", [](const vlink::Frame& self) {
        return "Frame(timestamp=" + std::to_string(self.timestamp) + ", url='" + self.url +
               "', size=" + std::to_string(self.data.size()) + ")";
      });

  //=========================================================================
  // Zero-copy data types
  //=========================================================================

  nb::class_<vlink::zerocopy::Header>(m, "ZeroCopyHeader", "Common timestamp and sequencing metadata header (40 bytes)")
      .def(nb::init<>())
      .def_prop_rw(
          "frame_id", [](const vlink::zerocopy::Header& self) { return std::string(self.frame_id_view()); },
          [](vlink::zerocopy::Header& self, const std::string& s) {
            constexpr size_t kMax = sizeof(vlink::zerocopy::Header::frame_id) - 1;
            const auto prefix = utf8_prefix(s, kMax);
            std::memset(self.frame_id, 0, sizeof(self.frame_id));
            std::memcpy(self.frame_id, prefix.data(), prefix.size());
          })
      .def_rw("seq", &vlink::zerocopy::Header::seq)
      .def_rw("reserved", &vlink::zerocopy::Header::reserved)
      .def_rw("time_meas", &vlink::zerocopy::Header::time_meas)
      .def_rw("time_pub", &vlink::zerocopy::Header::time_pub)
      .def("__repr__", [](const vlink::zerocopy::Header& self) {
        return std::string("ZeroCopyHeader(frame_id='") + std::string(self.frame_id_view()) +
               "', seq=" + std::to_string(self.seq) + ")";
      });

  using ZerocopyMessageParser = vlink::zerocopy::MessageParser;
  using PythonMessageParser = PythonZerocopyMessageParser;
  nb::class_<PythonMessageParser> message_parser_cls(m, "ZeroCopyMessageParser",
                                                     "Unified read-only parser for VLink zero-copy messages");
  nb::enum_<ZerocopyMessageParser::Type>(message_parser_cls, "Type")
      .value("Unknown", ZerocopyMessageParser::Type::kUnknown)
      .value("RawData", ZerocopyMessageParser::Type::kRawData)
      .value("CameraFrame", ZerocopyMessageParser::Type::kCameraFrame)
      .value("PointCloud", ZerocopyMessageParser::Type::kPointCloud)
      .value("ProxyData", ZerocopyMessageParser::Type::kProxyData)
      .value("OccupancyGrid", ZerocopyMessageParser::Type::kOccupancyGrid)
      .value("Tensor", ZerocopyMessageParser::Type::kTensor)
      .value("ObjectArray", ZerocopyMessageParser::Type::kObjectArray)
      .value("AudioFrame", ZerocopyMessageParser::Type::kAudioFrame);
  nb::enum_<ZerocopyMessageParser::ValueType>(message_parser_cls, "ValueType")
      .value("Unknown", ZerocopyMessageParser::ValueType::kValueUnknown)
      .value("Int64", ZerocopyMessageParser::ValueType::kInt64)
      .value("Uint64", ZerocopyMessageParser::ValueType::kUInt64)
      .value("Double", ZerocopyMessageParser::ValueType::kDouble)
      .value("String", ZerocopyMessageParser::ValueType::kString)
      .value("Bytes", ZerocopyMessageParser::ValueType::kBytes);
  nb::enum_<ZerocopyMessageParser::EnumKind>(message_parser_cls, "EnumKind")
      .value("NoEnum", ZerocopyMessageParser::EnumKind::kEnumNone)
      .value("CameraFormat", ZerocopyMessageParser::EnumKind::kEnumCameraFormat)
      .value("CameraStream", ZerocopyMessageParser::EnumKind::kEnumCameraStream)
      .value("GridCellType", ZerocopyMessageParser::EnumKind::kEnumGridCellType)
      .value("TensorDataType", ZerocopyMessageParser::EnumKind::kEnumTensorDataType)
      .value("TensorDevice", ZerocopyMessageParser::EnumKind::kEnumTensorDevice)
      .value("AudioFormat", ZerocopyMessageParser::EnumKind::kEnumAudioFormat)
      .value("AudioLayout", ZerocopyMessageParser::EnumKind::kEnumAudioLayout);
  nb::class_<ZerocopyMessageParser::Field>(message_parser_cls, "Field")
      .def_ro("name", &ZerocopyMessageParser::Field::name)
      .def_ro("type", &ZerocopyMessageParser::Field::type)
      .def_ro("native_type", &ZerocopyMessageParser::Field::native_type)
      .def_ro("storage_size", &ZerocopyMessageParser::Field::storage_size)
      .def_ro("enum_kind", &ZerocopyMessageParser::Field::enum_kind)
      .def_ro("is_time", &ZerocopyMessageParser::Field::is_time)
      .def_ro("is_bool", &ZerocopyMessageParser::Field::is_bool)
      .def_ro("is_reserved", &ZerocopyMessageParser::Field::is_reserved)
      .def_ro("byte_offset", &ZerocopyMessageParser::Field::byte_offset)
      .def_ro("element_index", &ZerocopyMessageParser::Field::element_index);

  const auto parser_value_to_python = [](const ZerocopyMessageParser::Value& value) -> nb::object {
    return std::visit(
        [](const auto& item) -> nb::object {
          using Item = std::decay_t<decltype(item)>;

          if constexpr (std::is_same_v<Item, vlink::Bytes>) {
            return nb::bytes(item.data(), item.size());
          } else {
            return nb::cast(item);
          }
        },
        value);
  };

  message_parser_cls.def(nb::init<>())
      .def("parse", &PythonMessageParser::parse, "serialized_type"_a, "bytes"_a)
      .def("parse_type", &PythonMessageParser::parse_type, "type"_a, "bytes"_a)
      .def("clear", &PythonMessageParser::clear)
      .def_prop_ro("type",
                   [](const PythonMessageParser& self) {
                     return self.backing_valid() ? self.parser.type() : ZerocopyMessageParser::Type::kUnknown;
                   })
      .def_prop_ro("valid", &PythonMessageParser::backing_valid)
      .def(
          "value",
          [parser_value_to_python](const PythonMessageParser& self, std::string_view path) -> nb::object {
            ZerocopyMessageParser::Value value;

            if VUNLIKELY (!self.backing_valid() || !self.parser.value(path, value)) {
              return nb::none();
            }

            return parser_value_to_python(value);
          },
          "path"_a)
      .def(
          "value_at",
          [parser_value_to_python](const PythonMessageParser& self, std::string_view collection, size_t index,
                                   std::string_view field) -> nb::object {
            ZerocopyMessageParser::Value value;

            if VUNLIKELY (!self.backing_valid() || !self.parser.value(collection, index, field, value)) {
              return nb::none();
            }

            return parser_value_to_python(value);
          },
          "collection"_a, "index"_a, "field"_a)
      .def(
          "collection_size",
          [](const PythonMessageParser& self, std::string_view collection) {
            return self.backing_valid() ? self.parser.collection_size(collection) : 0;
          },
          "collection"_a)
      .def("fields",
           [](const PythonMessageParser& self) {
             return self.backing_valid() ? self.parser.fields() : std::vector<ZerocopyMessageParser::Field>{};
           })
      .def(
          "element_fields",
          [](const PythonMessageParser& self, std::string_view collection) {
            return self.backing_valid() ? self.parser.element_fields(collection)
                                        : std::vector<ZerocopyMessageParser::Field>{};
          },
          "collection"_a)
      .def_static("detect_type", &ZerocopyMessageParser::detect_type, "serialized_type"_a)
      .def_static(
          "type_name",
          [](ZerocopyMessageParser::Type type) { return std::string(ZerocopyMessageParser::type_name(type)); },
          "type"_a);

  nb::class_<vlink::zerocopy::RawData>(m, "RawData", "Generic zero-copy raw-byte data container (64 bytes)")
      .def(nb::init<>())
      .def_rw("header", &vlink::zerocopy::RawData::header)
      .def("create", &zerocopy_create<vlink::zerocopy::RawData>, "size"_a)
      .def("clear", &zerocopy_clear<vlink::zerocopy::RawData>)
      .def("size", &vlink::zerocopy::RawData::size)
      .def("is_valid", &vlink::zerocopy::RawData::is_valid)
      .def("is_owner", &vlink::zerocopy::RawData::is_owner)
      .def("get_serialized_size", &vlink::zerocopy::RawData::get_serialized_size)
      .def_static("check_valid", &vlink::zerocopy::RawData::check_valid, "bytes"_a)
      .def_prop_rw(
          "reserved", [](vlink::zerocopy::RawData& self) { return self.get_reserved(); },
          [](vlink::zerocopy::RawData& self, uint16_t v) { self.get_reserved() = v; })
      .def("data", [](const vlink::zerocopy::RawData& self) { return nb::bytes(self.data(), self.size()); })
      .def("fill_data", &zerocopy_fill_data<vlink::zerocopy::RawData>, "data"_a)
      .def("to_bytes",
           [](const vlink::zerocopy::RawData& self) {
             vlink::Bytes b;
             self >> b;
             return b;
           })
      .def("from_bytes", &zerocopy_from_bytes<vlink::zerocopy::RawData>, "bytes"_a)
      .def("__repr__", [](const vlink::zerocopy::RawData& self) {
        return std::string("RawData(size=") + std::to_string(self.size()) + ")";
      });

  nb::class_<vlink::zerocopy::CameraFrame> camera_frame_cls(m, "CameraFrame",
                                                            "Zero-copy camera image frame (80 bytes)");
  nb::enum_<vlink::zerocopy::CameraFrame::Format>(camera_frame_cls, "Format")
      .value("Unknown", vlink::zerocopy::CameraFrame::kFormatUnknown)
      .value("Yuv420", vlink::zerocopy::CameraFrame::kFormatYuv420)
      .value("Yuv422", vlink::zerocopy::CameraFrame::kFormatYuv422)
      .value("Yuv444", vlink::zerocopy::CameraFrame::kFormatYuv444)
      .value("Nv12", vlink::zerocopy::CameraFrame::kFormatNv12)
      .value("Nv21", vlink::zerocopy::CameraFrame::kFormatNv21)
      .value("Yuyv", vlink::zerocopy::CameraFrame::kFormatYuyv)
      .value("Yvyu", vlink::zerocopy::CameraFrame::kFormatYvyu)
      .value("Uyvy", vlink::zerocopy::CameraFrame::kFormatUyvy)
      .value("Vyuy", vlink::zerocopy::CameraFrame::kFormatVyuy)
      .value("Bgr888Packed", vlink::zerocopy::CameraFrame::kFormatBgr888Packed)
      .value("Rgb888Packed", vlink::zerocopy::CameraFrame::kFormatRgb888Packed)
      .value("Rgb888Planar", vlink::zerocopy::CameraFrame::kFormatRgb888Planar)
      .value("Mono8", vlink::zerocopy::CameraFrame::kFormatMono8)
      .value("Mono16", vlink::zerocopy::CameraFrame::kFormatMono16)
      .value("Rgba8888Packed", vlink::zerocopy::CameraFrame::kFormatRgba8888Packed)
      .value("Bgra8888Packed", vlink::zerocopy::CameraFrame::kFormatBgra8888Packed)
      .value("Uint8C1", vlink::zerocopy::CameraFrame::kFormatUint8C1)
      .value("Uint8C2", vlink::zerocopy::CameraFrame::kFormatUint8C2)
      .value("Uint8C3", vlink::zerocopy::CameraFrame::kFormatUint8C3)
      .value("Uint8C4", vlink::zerocopy::CameraFrame::kFormatUint8C4)
      .value("Int8C1", vlink::zerocopy::CameraFrame::kFormatInt8C1)
      .value("Int8C2", vlink::zerocopy::CameraFrame::kFormatInt8C2)
      .value("Int8C3", vlink::zerocopy::CameraFrame::kFormatInt8C3)
      .value("Int8C4", vlink::zerocopy::CameraFrame::kFormatInt8C4)
      .value("Uint16C1", vlink::zerocopy::CameraFrame::kFormatUint16C1)
      .value("Uint16C2", vlink::zerocopy::CameraFrame::kFormatUint16C2)
      .value("Uint16C3", vlink::zerocopy::CameraFrame::kFormatUint16C3)
      .value("Uint16C4", vlink::zerocopy::CameraFrame::kFormatUint16C4)
      .value("Int16C1", vlink::zerocopy::CameraFrame::kFormatInt16C1)
      .value("Int16C2", vlink::zerocopy::CameraFrame::kFormatInt16C2)
      .value("Int16C3", vlink::zerocopy::CameraFrame::kFormatInt16C3)
      .value("Int16C4", vlink::zerocopy::CameraFrame::kFormatInt16C4)
      .value("Int32C1", vlink::zerocopy::CameraFrame::kFormatInt32C1)
      .value("Int32C2", vlink::zerocopy::CameraFrame::kFormatInt32C2)
      .value("Int32C3", vlink::zerocopy::CameraFrame::kFormatInt32C3)
      .value("Int32C4", vlink::zerocopy::CameraFrame::kFormatInt32C4)
      .value("Float32C1", vlink::zerocopy::CameraFrame::kFormatFloat32C1)
      .value("Float32C2", vlink::zerocopy::CameraFrame::kFormatFloat32C2)
      .value("Float32C3", vlink::zerocopy::CameraFrame::kFormatFloat32C3)
      .value("Float32C4", vlink::zerocopy::CameraFrame::kFormatFloat32C4)
      .value("Float64C1", vlink::zerocopy::CameraFrame::kFormatFloat64C1)
      .value("Float64C2", vlink::zerocopy::CameraFrame::kFormatFloat64C2)
      .value("Float64C3", vlink::zerocopy::CameraFrame::kFormatFloat64C3)
      .value("Float64C4", vlink::zerocopy::CameraFrame::kFormatFloat64C4)
      .value("BayerRggb8", vlink::zerocopy::CameraFrame::kFormatBayerRggb8)
      .value("BayerBggr8", vlink::zerocopy::CameraFrame::kFormatBayerBggr8)
      .value("BayerGbrg8", vlink::zerocopy::CameraFrame::kFormatBayerGbrg8)
      .value("BayerGrbg8", vlink::zerocopy::CameraFrame::kFormatBayerGrbg8)
      .value("BayerRggb16", vlink::zerocopy::CameraFrame::kFormatBayerRggb16)
      .value("BayerBggr16", vlink::zerocopy::CameraFrame::kFormatBayerBggr16)
      .value("BayerGbrg16", vlink::zerocopy::CameraFrame::kFormatBayerGbrg16)
      .value("BayerGrbg16", vlink::zerocopy::CameraFrame::kFormatBayerGrbg16)
      .value("Jpeg", vlink::zerocopy::CameraFrame::kFormatJpeg)
      .value("H264", vlink::zerocopy::CameraFrame::kFormatH264)
      .value("H265", vlink::zerocopy::CameraFrame::kFormatH265)
      .value("Png", vlink::zerocopy::CameraFrame::kFormatPng)
      .value("Mjpeg", vlink::zerocopy::CameraFrame::kFormatMjpeg)
      .value("H266", vlink::zerocopy::CameraFrame::kFormatH266)
      .value("Av1", vlink::zerocopy::CameraFrame::kFormatAv1)
      .value("Webp", vlink::zerocopy::CameraFrame::kFormatWebp);
  nb::enum_<vlink::zerocopy::CameraFrame::Stream>(camera_frame_cls, "Stream")
      .value("Unknown", vlink::zerocopy::CameraFrame::kStreamUnknown)
      .value("I", vlink::zerocopy::CameraFrame::kStreamI)
      .value("P", vlink::zerocopy::CameraFrame::kStreamP)
      .value("B", vlink::zerocopy::CameraFrame::kStreamB);
  camera_frame_cls.def(nb::init<>())
      .def_rw("header", &vlink::zerocopy::CameraFrame::header)
      .def("create", &zerocopy_create<vlink::zerocopy::CameraFrame>, "size"_a)
      .def("clear", &zerocopy_clear<vlink::zerocopy::CameraFrame>)
      .def("size", &vlink::zerocopy::CameraFrame::size)
      .def("is_valid", &vlink::zerocopy::CameraFrame::is_valid)
      .def("is_owner", &vlink::zerocopy::CameraFrame::is_owner)
      .def("get_serialized_size", &vlink::zerocopy::CameraFrame::get_serialized_size)
      .def_static("check_valid", &vlink::zerocopy::CameraFrame::check_valid, "bytes"_a)
      .def("channel", &vlink::zerocopy::CameraFrame::channel)
      .def("width", &vlink::zerocopy::CameraFrame::width)
      .def("height", &vlink::zerocopy::CameraFrame::height)
      .def("freq", &vlink::zerocopy::CameraFrame::freq)
      .def("format", &vlink::zerocopy::CameraFrame::format)
      .def("stream", &vlink::zerocopy::CameraFrame::stream)
      .def_static("format_from_encoding", &vlink::zerocopy::CameraFrame::format_from_encoding, "encoding"_a)
      .def_static(
          "encoding_from_format",
          [](vlink::zerocopy::CameraFrame::Format format) {
            return std::string(vlink::zerocopy::CameraFrame::encoding_from_format(format));
          },
          "format"_a)
      .def("set_channel", &vlink::zerocopy::CameraFrame::set_channel, "channel"_a)
      .def("set_width", &vlink::zerocopy::CameraFrame::set_width, "width"_a)
      .def("set_height", &vlink::zerocopy::CameraFrame::set_height, "height"_a)
      .def("set_freq", &vlink::zerocopy::CameraFrame::set_freq, "freq"_a)
      .def("set_format", &vlink::zerocopy::CameraFrame::set_format, "format"_a)
      .def("set_stream", &vlink::zerocopy::CameraFrame::set_stream, "stream"_a)
      .def_prop_rw(
          "reserved", [](vlink::zerocopy::CameraFrame& self) { return self.get_reserved(); },
          [](vlink::zerocopy::CameraFrame& self, uint32_t v) { self.get_reserved() = v; })
      .def("data", [](const vlink::zerocopy::CameraFrame& self) { return nb::bytes(self.data(), self.size()); })
      .def("fill_data", &zerocopy_fill_data<vlink::zerocopy::CameraFrame>, "data"_a)
      .def("to_bytes",
           [](const vlink::zerocopy::CameraFrame& self) {
             vlink::Bytes b;
             self >> b;
             return b;
           })
      .def("from_bytes", &zerocopy_from_bytes<vlink::zerocopy::CameraFrame>, "bytes"_a)
      .def("__repr__", [](const vlink::zerocopy::CameraFrame& self) {
        return std::string("CameraFrame(") + std::to_string(self.width()) + "x" + std::to_string(self.height()) +
               ", size=" + std::to_string(self.size()) + ")";
      });

  nb::class_<vlink::zerocopy::PointCloud> point_cloud_cls(m, "PointCloud",
                                                          "Schema-aware zero-copy 3-D point cloud (256 bytes)");
  nb::enum_<vlink::zerocopy::PointCloud::Type>(point_cloud_cls, "Type")
      .value("Unknown", vlink::zerocopy::PointCloud::kUnknownType)
      .value("Bool", vlink::zerocopy::PointCloud::kBoolType)
      .value("Int8", vlink::zerocopy::PointCloud::kInt8Type)
      .value("Uint8", vlink::zerocopy::PointCloud::kUint8Type)
      .value("Int16", vlink::zerocopy::PointCloud::kInt16Type)
      .value("Uint16", vlink::zerocopy::PointCloud::kUint16Type)
      .value("Int32", vlink::zerocopy::PointCloud::kInt32Type)
      .value("Uint32", vlink::zerocopy::PointCloud::kUint32Type)
      .value("Int64", vlink::zerocopy::PointCloud::kInt64Type)
      .value("Uint64", vlink::zerocopy::PointCloud::kUint64Type)
      .value("Float", vlink::zerocopy::PointCloud::kFloatType)
      .value("Double", vlink::zerocopy::PointCloud::kDoubleType);
  nb::class_<vlink::zerocopy::PointCloud::Key>(point_cloud_cls, "Key")
      .def(nb::init<>())
      .def_rw("name", &vlink::zerocopy::PointCloud::Key::name)
      .def_prop_rw(
          "type",
          [](const vlink::zerocopy::PointCloud::Key& self) {
            if (self.type > static_cast<uint8_t>(vlink::zerocopy::PointCloud::kDoubleType)) {
              return vlink::zerocopy::PointCloud::kUnknownType;
            }

            return static_cast<vlink::zerocopy::PointCloud::Type>(self.type);
          },
          [](vlink::zerocopy::PointCloud::Key& self, vlink::zerocopy::PointCloud::Type type) {
            self.type = static_cast<uint8_t>(type);
          })
      .def_rw("size", &vlink::zerocopy::PointCloud::Key::size);
  nb::class_<vlink::zerocopy::PointCloud::Vector3f>(point_cloud_cls, "Vector3f")
      .def(nb::init<>())
      .def(nb::init<float, float, float>(), "x"_a, "y"_a, "z"_a)
      .def_rw("x", &vlink::zerocopy::PointCloud::Vector3f::x)
      .def_rw("y", &vlink::zerocopy::PointCloud::Vector3f::y)
      .def_rw("z", &vlink::zerocopy::PointCloud::Vector3f::z)
      .def("__repr__", [](const vlink::zerocopy::PointCloud::Vector3f& v) {
        return std::string("Vector3f(") + std::to_string(v.x) + ", " + std::to_string(v.y) + ", " +
               std::to_string(v.z) + ")";
      });
  nb::class_<vlink::zerocopy::PointCloud::Vector3d>(point_cloud_cls, "Vector3d")
      .def(nb::init<>())
      .def(nb::init<double, double, double>(), "x"_a, "y"_a, "z"_a)
      .def_rw("x", &vlink::zerocopy::PointCloud::Vector3d::x)
      .def_rw("y", &vlink::zerocopy::PointCloud::Vector3d::y)
      .def_rw("z", &vlink::zerocopy::PointCloud::Vector3d::z)
      .def("__repr__", [](const vlink::zerocopy::PointCloud::Vector3d& v) {
        return std::string("Vector3d(") + std::to_string(v.x) + ", " + std::to_string(v.y) + ", " +
               std::to_string(v.z) + ")";
      });
  point_cloud_cls.def(nb::init<>())
      .def_rw("header", &vlink::zerocopy::PointCloud::header)
      .def(
          "create",
          [](nb::object instance, size_t size, uint64_t size_num, uint64_t type_num, const std::string& key_str,
             uint16_t extent, bool vertical) {
            const bool result = nb::cast<vlink::zerocopy::PointCloud&>(instance).create(size, size_num, type_num,
                                                                                        key_str, extent, vertical);

            if (result) {
              unbind_python_instance_owner(instance);
            }

            return result;
          },
          "size"_a, "size_num"_a, "type_num"_a, "key_str"_a, "extent"_a = static_cast<uint16_t>(0),
          "vertical"_a = false)
      .def(
          "fill_packed_data",
          [](vlink::zerocopy::PointCloud& self, nb::handle data, size_t count) {
            PythonBufferView view(data);
            const size_t pack_size = self.pack_size();

            if VUNLIKELY (pack_size != 0 && count > std::numeric_limits<size_t>::max() / pack_size) {
              throw nb::value_error("count * pack_size overflows size_t");
            }

            const size_t required_size = count * pack_size;

            if VUNLIKELY (view.size() < required_size) {
              throw nb::value_error("input buffer is smaller than count * pack_size");
            }

            return self.fill_packed_data(view.data(), count);
          },
          "data"_a, "count"_a)
      .def(
          "get_value_v3f",
          [](const vlink::zerocopy::PointCloud& self, size_t loop_index) { return self.get_value_v3f(loop_index); },
          "loop_index"_a)
      .def(
          "get_value_v3d",
          [](const vlink::zerocopy::PointCloud& self, size_t loop_index) { return self.get_value_v3d(loop_index); },
          "loop_index"_a)
      .def(
          "push_value_v3f",
          [](vlink::zerocopy::PointCloud& self, float x, float y, float z) { return self.push_value_v3f(x, y, z); },
          "x"_a, "y"_a, "z"_a)
      .def(
          "push_value_v3d",
          [](vlink::zerocopy::PointCloud& self, double x, double y, double z) { return self.push_value_v3d(x, y, z); },
          "x"_a, "y"_a, "z"_a)
      .def(
          "set_value_v3f",
          [](vlink::zerocopy::PointCloud& self, size_t loop_index, float x, float y, float z) {
            return self.set_value_v3f(loop_index, x, y, z);
          },
          "loop_index"_a, "x"_a, "y"_a, "z"_a)
      .def(
          "set_value_v3d",
          [](vlink::zerocopy::PointCloud& self, size_t loop_index, double x, double y, double z) {
            return self.set_value_v3d(loop_index, x, y, z);
          },
          "loop_index"_a, "x"_a, "y"_a, "z"_a)
      .def("resize", &vlink::zerocopy::PointCloud::resize, "size"_a)
      .def(
          "clear",
          [](nb::object instance, bool force) {
            nb::cast<vlink::zerocopy::PointCloud&>(instance).clear(force);

            if (force) {
              unbind_python_instance_owner(instance);
            }
          },
          "force"_a = false)
      .def("downsample", &vlink::zerocopy::PointCloud::downsample, "level"_a)
      .def("get_extent", &vlink::zerocopy::PointCloud::get_extent)
      .def("get_vertical", &vlink::zerocopy::PointCloud::get_vertical)
      .def("set_vertical", &vlink::zerocopy::PointCloud::set_vertical, "vertical"_a)
      .def("get_downsample", &vlink::zerocopy::PointCloud::get_downsample)
      .def_prop_rw(
          "reserved", [](vlink::zerocopy::PointCloud& self) { return self.get_reserved(); },
          [](vlink::zerocopy::PointCloud& self, uint32_t v) { self.get_reserved() = v; })
      .def_prop_rw(
          "reserved2", [](vlink::zerocopy::PointCloud& self) { return self.get_reserved2(); },
          [](vlink::zerocopy::PointCloud& self, uint32_t v) { self.get_reserved2() = v; })
      .def_prop_rw(
          "reserved3", [](vlink::zerocopy::PointCloud& self) { return self.get_reserved3(); },
          [](vlink::zerocopy::PointCloud& self, uint8_t v) { self.get_reserved3() = v; })
      .def("size", &vlink::zerocopy::PointCloud::size)
      .def("pack_size", &vlink::zerocopy::PointCloud::pack_size)
      .def("get_reserved_size", &vlink::zerocopy::PointCloud::get_reserved_size)
      .def("is_owner", &vlink::zerocopy::PointCloud::is_owner)
      .def("is_valid", &vlink::zerocopy::PointCloud::is_valid)
      .def("get_serialized_size", &vlink::zerocopy::PointCloud::get_serialized_size)
      .def_static("check_valid", &vlink::zerocopy::PointCloud::check_valid, "bytes"_a)
      .def("get_key_list", &vlink::zerocopy::PointCloud::get_key_list)
      .def("get_protocol_size_num", &vlink::zerocopy::PointCloud::get_protocol_size_num)
      .def("get_protocol_type_num", &vlink::zerocopy::PointCloud::get_protocol_type_num)
      .def("get_protocol_size_str", &vlink::zerocopy::PointCloud::get_protocol_size_str)
      .def("get_protocol_name_str", &vlink::zerocopy::PointCloud::get_protocol_name_str)
      .def("get_protocol_type_str", &vlink::zerocopy::PointCloud::get_protocol_type_str)
      .def("data",
           [](const vlink::zerocopy::PointCloud& self) {
             return nb::bytes(self.get_internal_data(), self.size() * self.pack_size());
           })
      .def("to_bytes",
           [](const vlink::zerocopy::PointCloud& self) {
             vlink::Bytes b;
             self >> b;
             return b;
           })
      .def("from_bytes", &zerocopy_from_bytes<vlink::zerocopy::PointCloud>, "bytes"_a)
      .def("__repr__", [](const vlink::zerocopy::PointCloud& self) {
        return std::string("PointCloud(size=") + std::to_string(self.size()) +
               ", pack_size=" + std::to_string(self.pack_size()) + ")";
      });

  nb::class_<vlink::zerocopy::ProxyData>(m, "ProxyData", "Proxy routing envelope (80 bytes)")
      .def(nb::init<>())
      .def(
          "create",
          [](nb::object instance, const vlink::Bytes& raw, const std::string& url, const std::string& ser,
             uint32_t schema, const std::string& hostname) {
            nb::cast<vlink::zerocopy::ProxyData&>(instance).create(raw, url, ser, schema, hostname);
            unbind_python_instance_owner(instance);
          },
          "raw"_a, "url"_a, "ser"_a, "schema"_a = static_cast<uint32_t>(0), "hostname"_a = std::string{})
      .def("clear", &zerocopy_clear<vlink::zerocopy::ProxyData>)
      .def("size", &vlink::zerocopy::ProxyData::size)
      .def("is_valid", &vlink::zerocopy::ProxyData::is_valid)
      .def("is_owner", &vlink::zerocopy::ProxyData::is_owner)
      .def("get_serialized_size", &vlink::zerocopy::ProxyData::get_serialized_size)
      .def_static("check_valid", &vlink::zerocopy::ProxyData::check_valid, "bytes"_a)
      .def("control_id", &vlink::zerocopy::ProxyData::control_id)
      .def("mode", &vlink::zerocopy::ProxyData::mode)
      .def("timestamp", &vlink::zerocopy::ProxyData::timestamp)
      .def("seq", &vlink::zerocopy::ProxyData::seq)
      .def("schema", &vlink::zerocopy::ProxyData::schema)
      .def("raw", &vlink::zerocopy::ProxyData::raw, nb::keep_alive<0, 1>())
      .def("url", [](const vlink::zerocopy::ProxyData& self) { return std::string(self.url()); })
      .def("ser", [](const vlink::zerocopy::ProxyData& self) { return std::string(self.ser()); })
      .def("hostname", [](const vlink::zerocopy::ProxyData& self) { return std::string(self.hostname()); })
      .def("set_control_id", &vlink::zerocopy::ProxyData::set_control_id, "control_id"_a)
      .def("set_mode", &vlink::zerocopy::ProxyData::set_mode, "mode"_a)
      .def("set_timestamp", &vlink::zerocopy::ProxyData::set_timestamp, "timestamp"_a)
      .def("set_seq", &vlink::zerocopy::ProxyData::set_seq, "seq"_a)
      .def("set_schema", &vlink::zerocopy::ProxyData::set_schema, "schema"_a)
      .def_prop_rw(
          "reserved", [](vlink::zerocopy::ProxyData& self) { return self.get_reserved(); },
          [](vlink::zerocopy::ProxyData& self, uint8_t v) { self.get_reserved() = v; })
      .def_prop_rw(
          "reserved2", [](vlink::zerocopy::ProxyData& self) { return self.get_reserved2(); },
          [](vlink::zerocopy::ProxyData& self, uint16_t v) { self.get_reserved2() = v; })
      .def("to_bytes",
           [](const vlink::zerocopy::ProxyData& self) {
             vlink::Bytes b;
             self >> b;
             return b;
           })
      .def("from_bytes", &zerocopy_from_bytes<vlink::zerocopy::ProxyData>, "bytes"_a)
      .def("__repr__", [](const vlink::zerocopy::ProxyData& self) {
        return std::string("ProxyData(url='") + std::string(self.url()) + "', size=" + std::to_string(self.size()) +
               ")";
      });

  nb::class_<vlink::zerocopy::OccupancyGrid> occupancy_grid_cls(m, "OccupancyGrid",
                                                                "Zero-copy 2-D occupancy grid map (152 bytes)");
  nb::enum_<vlink::zerocopy::OccupancyGrid::CellType>(occupancy_grid_cls, "CellType")
      .value("Unknown", vlink::zerocopy::OccupancyGrid::kCellUnknown)
      .value("Int8", vlink::zerocopy::OccupancyGrid::kCellInt8)
      .value("Uint8", vlink::zerocopy::OccupancyGrid::kCellUint8)
      .value("Uint16", vlink::zerocopy::OccupancyGrid::kCellUint16)
      .value("Float32", vlink::zerocopy::OccupancyGrid::kCellFloat32);
  occupancy_grid_cls.def(nb::init<>())
      .def_rw("header", &vlink::zerocopy::OccupancyGrid::header)
      .def("create", &zerocopy_create<vlink::zerocopy::OccupancyGrid>, "size"_a)
      .def("clear", &zerocopy_clear<vlink::zerocopy::OccupancyGrid>)
      .def("size", &vlink::zerocopy::OccupancyGrid::size)
      .def("is_valid", &vlink::zerocopy::OccupancyGrid::is_valid)
      .def("is_owner", &vlink::zerocopy::OccupancyGrid::is_owner)
      .def("get_serialized_size", &vlink::zerocopy::OccupancyGrid::get_serialized_size)
      .def_static("check_valid", &vlink::zerocopy::OccupancyGrid::check_valid, "bytes"_a)
      .def_static("cell_size_of", &vlink::zerocopy::OccupancyGrid::cell_size_of, "type"_a)
      .def("update_time_ns", &vlink::zerocopy::OccupancyGrid::update_time_ns)
      .def("map_id", [](const vlink::zerocopy::OccupancyGrid& self) { return std::string(self.map_id()); })
      .def("channel", &vlink::zerocopy::OccupancyGrid::channel)
      .def("freq", &vlink::zerocopy::OccupancyGrid::freq)
      .def("width", &vlink::zerocopy::OccupancyGrid::width)
      .def("height", &vlink::zerocopy::OccupancyGrid::height)
      .def("valid_cell_count", &vlink::zerocopy::OccupancyGrid::valid_cell_count)
      .def("resolution", &vlink::zerocopy::OccupancyGrid::resolution)
      .def("origin_x", &vlink::zerocopy::OccupancyGrid::origin_x)
      .def("origin_y", &vlink::zerocopy::OccupancyGrid::origin_y)
      .def("origin_z", &vlink::zerocopy::OccupancyGrid::origin_z)
      .def("origin_yaw", &vlink::zerocopy::OccupancyGrid::origin_yaw)
      .def("value_min", &vlink::zerocopy::OccupancyGrid::value_min)
      .def("value_max", &vlink::zerocopy::OccupancyGrid::value_max)
      .def("default_value", &vlink::zerocopy::OccupancyGrid::default_value)
      .def("occupied_threshold", &vlink::zerocopy::OccupancyGrid::occupied_threshold)
      .def("free_threshold", &vlink::zerocopy::OccupancyGrid::free_threshold)
      .def("cell_type", &vlink::zerocopy::OccupancyGrid::cell_type)
      .def("cell_size", &vlink::zerocopy::OccupancyGrid::cell_size)
      .def("set_update_time_ns", &vlink::zerocopy::OccupancyGrid::set_update_time_ns, "update_time_ns"_a)
      .def(
          "set_map_id",
          [](vlink::zerocopy::OccupancyGrid& self, const std::string& s) { self.set_map_id(utf8_prefix(s, 15)); },
          "map_id"_a)
      .def("set_channel", &vlink::zerocopy::OccupancyGrid::set_channel, "channel"_a)
      .def("set_freq", &vlink::zerocopy::OccupancyGrid::set_freq, "freq"_a)
      .def("set_width", &vlink::zerocopy::OccupancyGrid::set_width, "width"_a)
      .def("set_height", &vlink::zerocopy::OccupancyGrid::set_height, "height"_a)
      .def("set_valid_cell_count", &vlink::zerocopy::OccupancyGrid::set_valid_cell_count, "valid_cell_count"_a)
      .def("set_resolution", &vlink::zerocopy::OccupancyGrid::set_resolution, "resolution"_a)
      .def("set_origin_x", &vlink::zerocopy::OccupancyGrid::set_origin_x, "origin_x"_a)
      .def("set_origin_y", &vlink::zerocopy::OccupancyGrid::set_origin_y, "origin_y"_a)
      .def("set_origin_z", &vlink::zerocopy::OccupancyGrid::set_origin_z, "origin_z"_a)
      .def("set_origin_yaw", &vlink::zerocopy::OccupancyGrid::set_origin_yaw, "origin_yaw"_a)
      .def("set_value_min", &vlink::zerocopy::OccupancyGrid::set_value_min, "value_min"_a)
      .def("set_value_max", &vlink::zerocopy::OccupancyGrid::set_value_max, "value_max"_a)
      .def("set_default_value", &vlink::zerocopy::OccupancyGrid::set_default_value, "default_value"_a)
      .def("set_occupied_threshold", &vlink::zerocopy::OccupancyGrid::set_occupied_threshold, "occupied_threshold"_a)
      .def("set_free_threshold", &vlink::zerocopy::OccupancyGrid::set_free_threshold, "free_threshold"_a)
      .def("set_cell_type", &vlink::zerocopy::OccupancyGrid::set_cell_type, "cell_type"_a)
      .def_prop_rw(
          "reserved", [](vlink::zerocopy::OccupancyGrid& self) { return self.get_reserved(); },
          [](vlink::zerocopy::OccupancyGrid& self, uint16_t v) { self.get_reserved() = v; })
      .def_prop_rw(
          "reserved2", [](vlink::zerocopy::OccupancyGrid& self) { return self.get_reserved2(); },
          [](vlink::zerocopy::OccupancyGrid& self, uint32_t v) { self.get_reserved2() = v; })
      .def_prop_rw(
          "reserved3", [](vlink::zerocopy::OccupancyGrid& self) { return self.get_reserved3(); },
          [](vlink::zerocopy::OccupancyGrid& self, uint32_t v) { self.get_reserved3() = v; })
      .def("data", [](const vlink::zerocopy::OccupancyGrid& self) { return nb::bytes(self.data(), self.size()); })
      .def("fill_data", &zerocopy_fill_data<vlink::zerocopy::OccupancyGrid>, "data"_a)
      .def("to_bytes",
           [](const vlink::zerocopy::OccupancyGrid& self) {
             vlink::Bytes b;
             self >> b;
             return b;
           })
      .def("from_bytes", &zerocopy_from_bytes<vlink::zerocopy::OccupancyGrid>, "bytes"_a)
      .def("__repr__", [](const vlink::zerocopy::OccupancyGrid& self) {
        return std::string("OccupancyGrid(") + std::to_string(self.width()) + "x" + std::to_string(self.height()) +
               ", size=" + std::to_string(self.size()) + ")";
      });

  nb::class_<vlink::zerocopy::Tensor> tensor_cls(m, "Tensor", "Zero-copy N-D tensor (248 bytes)");
  nb::enum_<vlink::zerocopy::Tensor::DataType>(tensor_cls, "DataType")
      .value("Unknown", vlink::zerocopy::Tensor::kDataUnknown)
      .value("Bool", vlink::zerocopy::Tensor::kBool)
      .value("Int8", vlink::zerocopy::Tensor::kInt8)
      .value("Uint8", vlink::zerocopy::Tensor::kUint8)
      .value("Int16", vlink::zerocopy::Tensor::kInt16)
      .value("Uint16", vlink::zerocopy::Tensor::kUint16)
      .value("Int32", vlink::zerocopy::Tensor::kInt32)
      .value("Uint32", vlink::zerocopy::Tensor::kUint32)
      .value("Int64", vlink::zerocopy::Tensor::kInt64)
      .value("Uint64", vlink::zerocopy::Tensor::kUint64)
      .value("Float16", vlink::zerocopy::Tensor::kFloat16)
      .value("Bfloat16", vlink::zerocopy::Tensor::kBfloat16)
      .value("Float32", vlink::zerocopy::Tensor::kFloat32)
      .value("Float64", vlink::zerocopy::Tensor::kFloat64);
  nb::enum_<vlink::zerocopy::Tensor::Device>(tensor_cls, "Device")
      .value("Cpu", vlink::zerocopy::Tensor::kDeviceCpu)
      .value("Gpu", vlink::zerocopy::Tensor::kDeviceGpu)
      .value("Npu", vlink::zerocopy::Tensor::kDeviceNpu)
      .value("Dsp", vlink::zerocopy::Tensor::kDeviceDsp);
  tensor_cls.attr("kMaxRank") = vlink::zerocopy::Tensor::kMaxRank;
  tensor_cls.def(nb::init<>())
      .def_rw("header", &vlink::zerocopy::Tensor::header)
      .def("create", &zerocopy_create<vlink::zerocopy::Tensor>, "size"_a)
      .def("clear", &zerocopy_clear<vlink::zerocopy::Tensor>)
      .def("size", &vlink::zerocopy::Tensor::size)
      .def("is_valid", &vlink::zerocopy::Tensor::is_valid)
      .def("is_owner", &vlink::zerocopy::Tensor::is_owner)
      .def("get_serialized_size", &vlink::zerocopy::Tensor::get_serialized_size)
      .def_static("check_valid", &vlink::zerocopy::Tensor::check_valid, "bytes"_a)
      .def_static("element_size_of", &vlink::zerocopy::Tensor::element_size_of, "dtype"_a)
      .def("update_time_ns", &vlink::zerocopy::Tensor::update_time_ns)
      .def("num_elements", &vlink::zerocopy::Tensor::num_elements)
      .def("name", [](const vlink::zerocopy::Tensor& self) { return std::string(self.name()); })
      .def("model_id", [](const vlink::zerocopy::Tensor& self) { return std::string(self.model_id()); })
      .def("layout", [](const vlink::zerocopy::Tensor& self) { return std::string(self.layout()); })
      .def("shape",
           [](const vlink::zerocopy::Tensor& self) {
             return std::vector<uint32_t>(self.shape(), self.shape() + self.rank());
           })
      .def("shape_at", &vlink::zerocopy::Tensor::shape_at, "dim"_a)
      .def("strides",
           [](const vlink::zerocopy::Tensor& self) {
             return std::vector<uint32_t>(self.strides(), self.strides() + self.rank());
           })
      .def("stride_at", &vlink::zerocopy::Tensor::stride_at, "dim"_a)
      .def("channel", &vlink::zerocopy::Tensor::channel)
      .def("freq", &vlink::zerocopy::Tensor::freq)
      .def("batch_size", &vlink::zerocopy::Tensor::batch_size)
      .def("quant_scale", &vlink::zerocopy::Tensor::quant_scale)
      .def("quant_zero_point", &vlink::zerocopy::Tensor::quant_zero_point)
      .def("dtype", &vlink::zerocopy::Tensor::dtype)
      .def("rank", &vlink::zerocopy::Tensor::rank)
      .def("device", &vlink::zerocopy::Tensor::device)
      .def("element_size", &vlink::zerocopy::Tensor::element_size)
      .def("set_update_time_ns", &vlink::zerocopy::Tensor::set_update_time_ns, "update_time_ns"_a)
      .def(
          "set_name", [](vlink::zerocopy::Tensor& self, const std::string& s) { self.set_name(utf8_prefix(s, 31)); },
          "name"_a)
      .def(
          "set_model_id",
          [](vlink::zerocopy::Tensor& self, const std::string& s) { self.set_model_id(utf8_prefix(s, 31)); },
          "model_id"_a)
      .def(
          "set_layout",
          [](vlink::zerocopy::Tensor& self, const std::string& s) { self.set_layout(utf8_prefix(s, 15)); }, "layout"_a)
      .def(
          "set_shape",
          [](vlink::zerocopy::Tensor& self, const std::vector<uint32_t>& v) {
            const auto rank = std::min<size_t>(v.size(), vlink::zerocopy::Tensor::kMaxRank);
            self.set_shape(v.data(), static_cast<uint8_t>(rank));
          },
          "shape"_a)
      .def("set_shape_at", &vlink::zerocopy::Tensor::set_shape_at, "dim"_a, "value"_a)
      .def("set_stride_at", &vlink::zerocopy::Tensor::set_stride_at, "dim"_a, "value"_a)
      .def("set_channel", &vlink::zerocopy::Tensor::set_channel, "channel"_a)
      .def("set_freq", &vlink::zerocopy::Tensor::set_freq, "freq"_a)
      .def("set_batch_size", &vlink::zerocopy::Tensor::set_batch_size, "batch_size"_a)
      .def("set_quant_scale", &vlink::zerocopy::Tensor::set_quant_scale, "quant_scale"_a)
      .def("set_quant_zero_point", &vlink::zerocopy::Tensor::set_quant_zero_point, "quant_zero_point"_a)
      .def("set_dtype", &vlink::zerocopy::Tensor::set_dtype, "dtype"_a)
      .def("set_device", &vlink::zerocopy::Tensor::set_device, "device"_a)
      .def_prop_rw(
          "reserved", [](vlink::zerocopy::Tensor& self) { return self.get_reserved(); },
          [](vlink::zerocopy::Tensor& self, uint8_t v) { self.get_reserved() = v; })
      .def_prop_rw(
          "reserved2", [](vlink::zerocopy::Tensor& self) { return self.get_reserved2(); },
          [](vlink::zerocopy::Tensor& self, uint16_t v) { self.get_reserved2() = v; })
      .def_prop_rw(
          "reserved3", [](vlink::zerocopy::Tensor& self) { return self.get_reserved3(); },
          [](vlink::zerocopy::Tensor& self, uint32_t v) { self.get_reserved3() = v; })
      .def("data", [](const vlink::zerocopy::Tensor& self) { return nb::bytes(self.data(), self.size()); })
      .def("fill_data", &zerocopy_fill_data<vlink::zerocopy::Tensor>, "data"_a)
      .def("to_bytes",
           [](const vlink::zerocopy::Tensor& self) {
             vlink::Bytes b;
             self >> b;
             return b;
           })
      .def("from_bytes", &zerocopy_from_bytes<vlink::zerocopy::Tensor>, "bytes"_a)
      .def("__repr__", [](const vlink::zerocopy::Tensor& self) {
        return std::string("Tensor(rank=") + std::to_string(self.rank()) +
               ", num_elements=" + std::to_string(self.num_elements()) + ")";
      });

  nb::class_<vlink::zerocopy::ObjectArray> object_array_cls(
      m, "ObjectArray", "Zero-copy variable-length array of 3-D detection / tracking objects (112 bytes)");
  nb::enum_<vlink::zerocopy::ObjectArray::MotionState>(object_array_cls, "MotionState")
      .value("Unknown", vlink::zerocopy::ObjectArray::kMotionUnknown)
      .value("Stationary", vlink::zerocopy::ObjectArray::kMotionStationary)
      .value("Moving", vlink::zerocopy::ObjectArray::kMotionMoving)
      .value("Stopped", vlink::zerocopy::ObjectArray::kMotionStopped)
      .value("Parked", vlink::zerocopy::ObjectArray::kMotionParked);
  nb::enum_<vlink::zerocopy::ObjectArray::SourceType>(object_array_cls, "SourceType")
      .value("Unknown", vlink::zerocopy::ObjectArray::kSourceUnknown)
      .value("Lidar", vlink::zerocopy::ObjectArray::kSourceLidar)
      .value("Camera", vlink::zerocopy::ObjectArray::kSourceCamera)
      .value("Radar", vlink::zerocopy::ObjectArray::kSourceRadar)
      .value("Fusion", vlink::zerocopy::ObjectArray::kSourceFusion)
      .value("Ultrasonic", vlink::zerocopy::ObjectArray::kSourceUltrasonic);
  nb::class_<vlink::zerocopy::ObjectArray::Object>(object_array_cls, "Object")
      .def(nb::init<>())
      .def_prop_rw(
          "label",
          [](const vlink::zerocopy::ObjectArray::Object& self) {
            return std::string(self.label, ::strnlen(self.label, sizeof(self.label)));
          },
          [](vlink::zerocopy::ObjectArray::Object& self, const std::string& s) {
            constexpr size_t kMax = sizeof(vlink::zerocopy::ObjectArray::Object::label) - 1;
            const auto prefix = utf8_prefix(s, kMax);
            std::memset(self.label, 0, sizeof(self.label));
            std::memcpy(self.label, prefix.data(), prefix.size());
          })
      .def_prop_rw(
          "position",
          [](const vlink::zerocopy::ObjectArray::Object& self) {
            return std::vector<float>(self.position, self.position + 3);
          },
          [](vlink::zerocopy::ObjectArray::Object& self, const std::vector<float>& v) {
            const size_t n = std::min<size_t>(v.size(), 3);
            for (size_t i = 0; i < n; ++i) {
              self.position[i] = v[i];
            }
            for (size_t i = n; i < 3; ++i) {
              self.position[i] = 0.0F;
            }
          })
      .def_rw("yaw", &vlink::zerocopy::ObjectArray::Object::yaw)
      .def_prop_rw(
          "size",
          [](const vlink::zerocopy::ObjectArray::Object& self) { return std::vector<float>(self.size, self.size + 3); },
          [](vlink::zerocopy::ObjectArray::Object& self, const std::vector<float>& v) {
            const size_t n = std::min<size_t>(v.size(), 3);
            for (size_t i = 0; i < n; ++i) {
              self.size[i] = v[i];
            }
            for (size_t i = n; i < 3; ++i) {
              self.size[i] = 0.0F;
            }
          })
      .def_rw("yaw_rate", &vlink::zerocopy::ObjectArray::Object::yaw_rate)
      .def_prop_rw(
          "velocity",
          [](const vlink::zerocopy::ObjectArray::Object& self) {
            return std::vector<float>(self.velocity, self.velocity + 3);
          },
          [](vlink::zerocopy::ObjectArray::Object& self, const std::vector<float>& v) {
            const size_t n = std::min<size_t>(v.size(), 3);
            for (size_t i = 0; i < n; ++i) {
              self.velocity[i] = v[i];
            }
            for (size_t i = n; i < 3; ++i) {
              self.velocity[i] = 0.0F;
            }
          })
      .def_rw("score", &vlink::zerocopy::ObjectArray::Object::score)
      .def_prop_rw(
          "acceleration",
          [](const vlink::zerocopy::ObjectArray::Object& self) {
            return std::vector<float>(self.acceleration, self.acceleration + 3);
          },
          [](vlink::zerocopy::ObjectArray::Object& self, const std::vector<float>& v) {
            const size_t n = std::min<size_t>(v.size(), 3);
            for (size_t i = 0; i < n; ++i) {
              self.acceleration[i] = v[i];
            }
            for (size_t i = n; i < 3; ++i) {
              self.acceleration[i] = 0.0F;
            }
          })
      .def_rw("existence_probability", &vlink::zerocopy::ObjectArray::Object::existence_probability)
      .def_prop_rw(
          "position_covariance",
          [](const vlink::zerocopy::ObjectArray::Object& self) {
            return std::vector<float>(self.position_covariance, self.position_covariance + 6);
          },
          [](vlink::zerocopy::ObjectArray::Object& self, const std::vector<float>& v) {
            const size_t n = std::min<size_t>(v.size(), 6);
            for (size_t i = 0; i < n; ++i) {
              self.position_covariance[i] = v[i];
            }
            for (size_t i = n; i < 6; ++i) {
              self.position_covariance[i] = 0.0F;
            }
          })
      .def_rw("class_id", &vlink::zerocopy::ObjectArray::Object::class_id)
      .def_rw("track_id", &vlink::zerocopy::ObjectArray::Object::track_id)
      .def_rw("age", &vlink::zerocopy::ObjectArray::Object::age)
      .def_rw("num_observations", &vlink::zerocopy::ObjectArray::Object::num_observations)
      .def_rw("motion_state", &vlink::zerocopy::ObjectArray::Object::motion_state)
      .def_rw("source_type", &vlink::zerocopy::ObjectArray::Object::source_type)
      .def_rw("subtype_id", &vlink::zerocopy::ObjectArray::Object::subtype_id)
      .def_rw("reserved_buf", &vlink::zerocopy::ObjectArray::Object::reserved_buf)
      .def("__repr__", [](const vlink::zerocopy::ObjectArray::Object& self) {
        return std::string("Object(label='") + std::string(self.label, ::strnlen(self.label, sizeof(self.label))) +
               "', class_id=" + std::to_string(self.class_id) + ", track_id=" + std::to_string(self.track_id) + ")";
      });
  object_array_cls.def(nb::init<>())
      .def_rw("header", &vlink::zerocopy::ObjectArray::header)
      .def("create", &zerocopy_create<vlink::zerocopy::ObjectArray>, "count"_a)
      .def("clear", &zerocopy_clear<vlink::zerocopy::ObjectArray>)
      .def("push_value", &vlink::zerocopy::ObjectArray::push_value, "object"_a)
      .def("set_value", &vlink::zerocopy::ObjectArray::set_value, "index"_a, "object"_a)
      .def(
          "get_value", [](const vlink::zerocopy::ObjectArray& self, uint32_t index) { return self.get_value(index); },
          "index"_a)
      .def("resize", &vlink::zerocopy::ObjectArray::resize, "count"_a)
      .def("count", &vlink::zerocopy::ObjectArray::count)
      .def("pack_size", &vlink::zerocopy::ObjectArray::pack_size)
      .def("capacity", &vlink::zerocopy::ObjectArray::capacity)
      .def("is_valid", &vlink::zerocopy::ObjectArray::is_valid)
      .def("is_owner", &vlink::zerocopy::ObjectArray::is_owner)
      .def("get_serialized_size", &vlink::zerocopy::ObjectArray::get_serialized_size)
      .def_static("check_valid", &vlink::zerocopy::ObjectArray::check_valid, "bytes"_a)
      .def("update_time_ns", &vlink::zerocopy::ObjectArray::update_time_ns)
      .def("source_id", [](const vlink::zerocopy::ObjectArray& self) { return std::string(self.source_id()); })
      .def("channel", &vlink::zerocopy::ObjectArray::channel)
      .def("freq", &vlink::zerocopy::ObjectArray::freq)
      .def("set_update_time_ns", &vlink::zerocopy::ObjectArray::set_update_time_ns, "update_time_ns"_a)
      .def(
          "set_source_id",
          [](vlink::zerocopy::ObjectArray& self, const std::string& s) { self.set_source_id(utf8_prefix(s, 15)); },
          "source_id"_a)
      .def("set_channel", &vlink::zerocopy::ObjectArray::set_channel, "channel"_a)
      .def("set_freq", &vlink::zerocopy::ObjectArray::set_freq, "freq"_a)
      .def(
          "objects",
          [](const vlink::zerocopy::ObjectArray& self,
             uint32_t index) -> std::optional<vlink::zerocopy::ObjectArray::Object> {
            const auto* p = self.objects(index);

            if (!p) {
              return std::nullopt;
            }

            return *p;
          },
          "index"_a = static_cast<uint32_t>(0))
      .def_prop_rw(
          "reserved", [](vlink::zerocopy::ObjectArray& self) { return self.get_reserved(); },
          [](vlink::zerocopy::ObjectArray& self, uint8_t v) { self.get_reserved() = v; })
      .def_prop_rw(
          "reserved2", [](vlink::zerocopy::ObjectArray& self) { return self.get_reserved2(); },
          [](vlink::zerocopy::ObjectArray& self, uint16_t v) { self.get_reserved2() = v; })
      .def_prop_rw(
          "reserved3", [](vlink::zerocopy::ObjectArray& self) { return self.get_reserved3(); },
          [](vlink::zerocopy::ObjectArray& self, uint32_t v) { self.get_reserved3() = v; })
      .def_prop_rw(
          "reserved4", [](vlink::zerocopy::ObjectArray& self) { return self.get_reserved4(); },
          [](vlink::zerocopy::ObjectArray& self, uint32_t v) { self.get_reserved4() = v; })
      .def_prop_rw(
          "reserved5", [](vlink::zerocopy::ObjectArray& self) { return self.get_reserved5(); },
          [](vlink::zerocopy::ObjectArray& self, uint32_t v) { self.get_reserved5() = v; })
      .def("data",
           [](const vlink::zerocopy::ObjectArray& self) {
             const size_t payload_size = static_cast<size_t>(self.count()) * static_cast<size_t>(self.pack_size());
             return nb::bytes(self.data(), payload_size);
           })
      .def("to_bytes",
           [](const vlink::zerocopy::ObjectArray& self) {
             vlink::Bytes b;
             self >> b;
             return b;
           })
      .def("from_bytes", &zerocopy_from_bytes<vlink::zerocopy::ObjectArray>, "bytes"_a)
      .def("__repr__", [](const vlink::zerocopy::ObjectArray& self) {
        return std::string("ObjectArray(count=") + std::to_string(self.count()) +
               ", capacity=" + std::to_string(self.capacity()) + ")";
      });

  nb::class_<vlink::zerocopy::AudioFrame> audio_frame_cls(m, "AudioFrame", "Zero-copy audio frame (128 bytes)");
  nb::enum_<vlink::zerocopy::AudioFrame::Format>(audio_frame_cls, "Format")
      .value("Unknown", vlink::zerocopy::AudioFrame::kFormatUnknown)
      .value("PcmS16", vlink::zerocopy::AudioFrame::kFormatPcmS16)
      .value("PcmS24", vlink::zerocopy::AudioFrame::kFormatPcmS24)
      .value("PcmS32", vlink::zerocopy::AudioFrame::kFormatPcmS32)
      .value("PcmF32", vlink::zerocopy::AudioFrame::kFormatPcmF32)
      .value("PcmU8", vlink::zerocopy::AudioFrame::kFormatPcmU8)
      .value("Opus", vlink::zerocopy::AudioFrame::kFormatOpus)
      .value("Aac", vlink::zerocopy::AudioFrame::kFormatAac)
      .value("Mp3", vlink::zerocopy::AudioFrame::kFormatMp3)
      .value("Flac", vlink::zerocopy::AudioFrame::kFormatFlac);
  nb::enum_<vlink::zerocopy::AudioFrame::Layout>(audio_frame_cls, "Layout")
      .value("Unknown", vlink::zerocopy::AudioFrame::kLayoutUnknown)
      .value("Interleaved", vlink::zerocopy::AudioFrame::kLayoutInterleaved)
      .value("Planar", vlink::zerocopy::AudioFrame::kLayoutPlanar);
  audio_frame_cls.def(nb::init<>())
      .def_rw("header", &vlink::zerocopy::AudioFrame::header)
      .def("create", &zerocopy_create<vlink::zerocopy::AudioFrame>, "size"_a)
      .def("clear", &zerocopy_clear<vlink::zerocopy::AudioFrame>)
      .def("size", &vlink::zerocopy::AudioFrame::size)
      .def("is_valid", &vlink::zerocopy::AudioFrame::is_valid)
      .def("is_owner", &vlink::zerocopy::AudioFrame::is_owner)
      .def("get_serialized_size", &vlink::zerocopy::AudioFrame::get_serialized_size)
      .def_static("check_valid", &vlink::zerocopy::AudioFrame::check_valid, "bytes"_a)
      .def("update_time_ns", &vlink::zerocopy::AudioFrame::update_time_ns)
      .def("duration_ns", &vlink::zerocopy::AudioFrame::duration_ns)
      .def("codec", [](const vlink::zerocopy::AudioFrame& self) { return std::string(self.codec()); })
      .def("language", [](const vlink::zerocopy::AudioFrame& self) { return std::string(self.language()); })
      .def("channel", &vlink::zerocopy::AudioFrame::channel)
      .def("freq", &vlink::zerocopy::AudioFrame::freq)
      .def("sample_rate", &vlink::zerocopy::AudioFrame::sample_rate)
      .def("num_samples", &vlink::zerocopy::AudioFrame::num_samples)
      .def("bitrate", &vlink::zerocopy::AudioFrame::bitrate)
      .def("num_channels", &vlink::zerocopy::AudioFrame::num_channels)
      .def("bit_depth", &vlink::zerocopy::AudioFrame::bit_depth)
      .def("format", &vlink::zerocopy::AudioFrame::format)
      .def("layout", &vlink::zerocopy::AudioFrame::layout)
      .def("set_update_time_ns", &vlink::zerocopy::AudioFrame::set_update_time_ns, "update_time_ns"_a)
      .def("set_duration_ns", &vlink::zerocopy::AudioFrame::set_duration_ns, "duration_ns"_a)
      .def(
          "set_codec",
          [](vlink::zerocopy::AudioFrame& self, const std::string& s) { self.set_codec(utf8_prefix(s, 15)); },
          "codec"_a)
      .def(
          "set_language",
          [](vlink::zerocopy::AudioFrame& self, const std::string& s) { self.set_language(utf8_prefix(s, 7)); },
          "language"_a)
      .def("set_channel", &vlink::zerocopy::AudioFrame::set_channel, "channel"_a)
      .def("set_freq", &vlink::zerocopy::AudioFrame::set_freq, "freq"_a)
      .def("set_sample_rate", &vlink::zerocopy::AudioFrame::set_sample_rate, "sample_rate"_a)
      .def("set_num_samples", &vlink::zerocopy::AudioFrame::set_num_samples, "num_samples"_a)
      .def("set_bitrate", &vlink::zerocopy::AudioFrame::set_bitrate, "bitrate"_a)
      .def("set_num_channels", &vlink::zerocopy::AudioFrame::set_num_channels, "num_channels"_a)
      .def("set_bit_depth", &vlink::zerocopy::AudioFrame::set_bit_depth, "bit_depth"_a)
      .def("set_format", &vlink::zerocopy::AudioFrame::set_format, "format"_a)
      .def("set_layout", &vlink::zerocopy::AudioFrame::set_layout, "layout"_a)
      .def_prop_rw(
          "reserved", [](vlink::zerocopy::AudioFrame& self) { return self.get_reserved(); },
          [](vlink::zerocopy::AudioFrame& self, uint8_t v) { self.get_reserved() = v; })
      .def_prop_rw(
          "reserved2", [](vlink::zerocopy::AudioFrame& self) { return self.get_reserved2(); },
          [](vlink::zerocopy::AudioFrame& self, uint32_t v) { self.get_reserved2() = v; })
      .def("data", [](const vlink::zerocopy::AudioFrame& self) { return nb::bytes(self.data(), self.size()); })
      .def("fill_data", &zerocopy_fill_data<vlink::zerocopy::AudioFrame>, "data"_a)
      .def("to_bytes",
           [](const vlink::zerocopy::AudioFrame& self) {
             vlink::Bytes b;
             self >> b;
             return b;
           })
      .def("from_bytes", &zerocopy_from_bytes<vlink::zerocopy::AudioFrame>, "bytes"_a)
      .def("__repr__", [](const vlink::zerocopy::AudioFrame& self) {
        return std::string("AudioFrame(sample_rate=") + std::to_string(self.sample_rate()) +
               ", num_channels=" + std::to_string(self.num_channels()) + ", size=" + std::to_string(self.size()) + ")";
      });

  nb::class_<vlink::Version>(m, "Version", "Semantic versioning")
      .def(nb::init<>())
      .def(nb::init<uint8_t, uint8_t, uint8_t>(), "major"_a, "minor"_a, "patch"_a)
      .def_static("from_string", &vlink::Version::from_string, "str"_a)
      .def("to_string", &vlink::Version::to_string)
      .def("is_valid", &vlink::Version::is_valid)
      .def_rw("major", &vlink::Version::major)
      .def_rw("minor", &vlink::Version::minor)
      .def_rw("patch", &vlink::Version::patch)
      .def("__eq__", [](const vlink::Version& a, const vlink::Version& b) { return a == b; })
      .def("__ne__", [](const vlink::Version& a, const vlink::Version& b) { return a != b; })
      .def("__lt__", [](const vlink::Version& a, const vlink::Version& b) { return a < b; })
      .def("__gt__", [](const vlink::Version& a, const vlink::Version& b) { return a > b; })
      .def("__repr__", [](const vlink::Version& self) { return "Version(" + self.to_string() + ")"; });

  nb::class_<vlink::SchemaData>(m, "SchemaData", "Runtime schema descriptor")
      .def(nb::init<>())
      .def_rw("name", &vlink::SchemaData::name)
      .def_rw("encoding", &vlink::SchemaData::encoding)
      .def_rw("schema_type", &vlink::SchemaData::schema_type)
      .def_prop_rw(
          "data", [](vlink::SchemaData& self) -> vlink::Bytes& { return self.data; },
          [](vlink::SchemaData& self, const vlink::Bytes& data) {
            if (&self.data == &data) {
              return;
            }

            ensure_bytes_not_exported(self.data);
            self.data = data;
          },
          nb::rv_policy::reference_internal)
      .def_static("is_valid_type", &vlink::SchemaData::is_valid_type, "schema_type"_a)
      .def_static("is_real_type", &vlink::SchemaData::is_real_type, "schema_type"_a)
      .def_static(
          "convert_type",
          [](vlink::SchemaType schema_type) { return std::string(vlink::SchemaData::convert_type(schema_type)); },
          "schema_type"_a)
      .def_static("convert_encoding", &vlink::SchemaData::convert_encoding, "encoding"_a)
      .def_static(
          "infer_ser_type", [](const std::string& ser_type) { return vlink::SchemaData::infer_ser_type(ser_type); },
          "ser_type"_a)
      .def_static(
          "resolve_type",
          [](vlink::SchemaType schema_type, const std::string& ser_type, const std::string& encoding) {
            return vlink::SchemaData::resolve_type(schema_type, ser_type, encoding);
          },
          "schema_type"_a, "ser_type"_a = "", "encoding"_a = "")
      .def("__bool__",
           [](const vlink::SchemaData& self) {
             return !self.name.empty() || !self.encoding.empty() || self.schema_type != vlink::SchemaType::kUnknown ||
                    !self.data.empty();
           })
      .def("__repr__", [](const vlink::SchemaData& self) {
        return "SchemaData(name='" + self.name + "', encoding='" + self.encoding +
               "', size=" + std::to_string(self.data.size()) + ")";
      });

  nb::class_<vlink::SampleLostInfo>(m, "SampleLostInfo")
      .def(nb::init<>())
      .def_rw("total", &vlink::SampleLostInfo::total)
      .def_rw("lost", &vlink::SampleLostInfo::lost)
      .def("__repr__", [](const vlink::SampleLostInfo& self) {
        return "SampleLostInfo(total=" + std::to_string(self.total) + ", lost=" + std::to_string(self.lost) + ")";
      });

  nb::enum_<vlink::Logger::Level>(m, "LogLevel")
      .value("Trace", vlink::Logger::kTrace)
      .value("Debug", vlink::Logger::kDebug)
      .value("Info", vlink::Logger::kInfo)
      .value("Warn", vlink::Logger::kWarn)
      .value("Error", vlink::Logger::kError)
      .value("Fatal", vlink::Logger::kFatal)
      .value("Off", vlink::Logger::kOff);

  nb::class_<vlink::Logger>(m, "Logger")
      .def_static("init", &vlink::Logger::init, "app_name"_a = "", "log_path"_a = "")
      .def_static("get", &vlink::Logger::get, nb::rv_policy::reference)
      .def_static("flush", &vlink::Logger::flush)
      .def_static("set_console_level", &vlink::Logger::set_console_level, "level"_a)
      .def_static("get_console_level", &vlink::Logger::get_console_level)
      .def_static("set_file_level", &vlink::Logger::set_file_level, "level"_a)
      .def_static("get_file_level", &vlink::Logger::get_file_level)
      .def_static("set_console_fmt_enable", &vlink::Logger::set_console_fmt_enable, "enable"_a)
      .def_static("get_console_fmt_enable", &vlink::Logger::get_console_fmt_enable)
      .def_static(
          "set_stream_flag",
          [](int flags) { vlink::Logger::set_stream_flag(static_cast<std::ios_base::fmtflags>(flags)); }, "flags"_a)
      .def_static("get_stream_flag", []() { return static_cast<int>(vlink::Logger::get_stream_flag()); })
      .def_static("set_stream_precision", &vlink::Logger::set_stream_precision, "precision"_a)
      .def_static("get_stream_precision", &vlink::Logger::get_stream_precision)
      .def_static("set_stream_width", &vlink::Logger::set_stream_width, "width"_a)
      .def_static("get_stream_width", &vlink::Logger::get_stream_width)
      .def_static("is_busy", &vlink::Logger::is_busy)
      .def_static("is_writable", &vlink::Logger::is_writable, "level"_a)
      .def_static("enable_backtrace", &vlink::Logger::enable_backtrace, "size"_a)
      .def_static("disable_backtrace", &vlink::Logger::disable_backtrace)
      .def_static("dump_backtrace", &vlink::Logger::dump_backtrace)
      .def_static(
          "register_console_handler",
          [](nb::callable callback) {
            if VUNLIKELY (is_in_python_owner_callback(python_logger_callback_owner())) {
              throw std::runtime_error("Logger handlers cannot be replaced from an active logger callback");
            }

            auto activity = std::make_shared<PythonCallbackActivity>();
            auto cb = std::make_shared<GilSafePyFunction>(std::move(callback));
            auto previous = std::exchange(logger_console_callback_owner(), cb);
            {
              nb::gil_scoped_release release;
              vlink::Logger::register_console_handler([activity, cb](vlink::Logger::Level level, std::string_view msg) {
                PythonCallbackScope active(activity, python_logger_callback_owner());

                if VUNLIKELY (!Py_IsInitialized()) {
                  return;
                }

                nb::gil_scoped_acquire gil;
                try {
                  cb->fn(level, std::string(msg));
                } catch (std::exception&) {
                  report_current_exception("vlink::Logger.register_console_handler");
                }
              });
            }
          },
          "callback"_a)
      .def_static(
          "register_file_handler",
          [](nb::callable callback) {
            if VUNLIKELY (is_in_python_owner_callback(python_logger_callback_owner())) {
              throw std::runtime_error("Logger handlers cannot be replaced from an active logger callback");
            }

            auto activity = std::make_shared<PythonCallbackActivity>();
            auto cb = std::make_shared<GilSafePyFunction>(std::move(callback));
            auto previous = std::exchange(logger_file_callback_owner(), cb);
            {
              nb::gil_scoped_release release;
              vlink::Logger::register_file_handler([activity, cb](vlink::Logger::Level level, std::string_view msg) {
                PythonCallbackScope active(activity, python_logger_callback_owner());

                if VUNLIKELY (!Py_IsInitialized()) {
                  return;
                }

                nb::gil_scoped_acquire gil;
                try {
                  cb->fn(level, std::string(msg));
                } catch (std::exception&) {
                  report_current_exception("vlink::Logger.register_file_handler");
                }
              });
            }
          },
          "callback"_a);

  m.def("log_trace", [](const std::string& msg) { vlink::Logger::print<vlink::Logger::kTrace>(msg); }, "msg"_a);
  m.def("log_debug", [](const std::string& msg) { vlink::Logger::print<vlink::Logger::kDebug>(msg); }, "msg"_a);
  m.def("log_info", [](const std::string& msg) { vlink::Logger::print<vlink::Logger::kInfo>(msg); }, "msg"_a);
  m.def("log_warn", [](const std::string& msg) { vlink::Logger::print<vlink::Logger::kWarn>(msg); }, "msg"_a);
  m.def("log_error", [](const std::string& msg) { vlink::Logger::print<vlink::Logger::kError>(msg); }, "msg"_a);
  m.def("log_fatal", [](const std::string& msg) { vlink::Logger::print<vlink::Logger::kFatal>(msg); }, "msg"_a);

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
            return self.post_task(make_owned_void_callback(&self, std::move(callback), "vlink::MessageLoop.post_task"));
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
            return self.post_task_with_priority(
                make_owned_void_callback(&self, std::move(callback), "vlink::MessageLoop.post_task_with_priority"),
                static_cast<uint16_t>(priority));
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

  nb::class_<PythonProcess> proc(m, "Process", "Child process management");

  nb::enum_<vlink::Process::State>(proc, "State")
      .value("NotRunning", vlink::Process::kNotRunningState)
      .value("Starting", vlink::Process::kStartingState)
      .value("Running", vlink::Process::kRunningState);
  nb::enum_<vlink::Process::ExitStatus>(proc, "ExitStatus")
      .value("Normal", vlink::Process::kNormalExitStatus)
      .value("Crash", vlink::Process::kCrashExitStatus);
  nb::enum_<vlink::Process::Error>(proc, "Error")
      .value("NoError", vlink::Process::kNoError)
      .value("UnknownError", vlink::Process::kUnknownError)
      .value("StartError", vlink::Process::kStartError)
      .value("CrashedError", vlink::Process::kCrashedError)
      .value("TimedOutError", vlink::Process::kTimedOutError)
      .value("WriteError", vlink::Process::kWriteError)
      .value("ReadError", vlink::Process::kReadError)
      .value("BufferOverflowError", vlink::Process::kBufferOverflowError);
  nb::enum_<vlink::Process::Mode>(proc, "Mode")
      .value("Separate", vlink::Process::kSeparateMode)
      .value("Merged", vlink::Process::kMergedMode)
      .value("Forwarded", vlink::Process::kForwardedMode)
      .value("ForwardedOutput", vlink::Process::kForwardedOutputMode)
      .value("ForwardedError", vlink::Process::kForwardedErrorMode);

  proc.def(nb::init<>())
      .def("get_state", [](const PythonProcess& self) { return self->get_state(); })
      .def("get_error", [](const PythonProcess& self) { return self->get_error(); })
      .def("get_exit_code", [](const PythonProcess& self) { return self->get_exit_code(); })
      .def("get_exit_status", [](const PythonProcess& self) { return self->get_exit_status(); })
      .def("is_running", [](const PythonProcess& self) { return self->is_running(); })
      .def("get_process_id", [](const PythonProcess& self) { return self->get_process_id(); })
      .def(
          "set_max_buffer_size", [](PythonProcess& self, size_t size) { self->set_max_buffer_size(size); }, "size"_a)
      .def("get_max_buffer_size", [](const PythonProcess& self) { return self->get_max_buffer_size(); })
      .def(
          "set_process_mode", [](PythonProcess& self, vlink::Process::Mode mode) { self->set_process_mode(mode); },
          "mode"_a)
      .def("get_process_mode", [](const PythonProcess& self) { return self->get_process_mode(); })
      .def(
          "set_inherit_environment", [](PythonProcess& self, bool inherit) { self->set_inherit_environment(inherit); },
          "inherit"_a)
      .def("get_inherit_environment", [](const PythonProcess& self) { return self->get_inherit_environment(); })
      .def(
          "set_working_directory",
          [](PythonProcess& self, const std::string& dir) { self->set_working_directory(dir); }, "dir"_a)
      .def("get_working_directory", [](const PythonProcess& self) { return self->get_working_directory(); })
      .def(
          "set_environment",
          [](PythonProcess& self, const vlink::Process::EnvironmentMap& env_map) { self->set_environment(env_map); },
          "env_map"_a)
      .def("get_environment", [](const PythonProcess& self) { return self->get_environment(); })
      .def(
          "register_error_callback",
          [](PythonProcess& self, nb::callable callback) {
            auto cb = std::make_shared<GilSafePyFunction>(std::move(callback));
            self->register_error_callback([activity = self.activity, cb](vlink::Process::Error error) {
              PythonCallbackScope active(activity);

              if VUNLIKELY (!Py_IsInitialized()) {
                return;
              }

              nb::gil_scoped_acquire gil;
              try {
                cb->fn(error);
              } catch (std::exception&) {
                report_current_exception("vlink::Process.register_error_callback");
              }
            });
          },
          "callback"_a)
      .def(
          "register_finished_callback",
          [](PythonProcess& self, nb::callable callback) {
            auto cb = std::make_shared<GilSafePyFunction>(std::move(callback));
            self->register_finished_callback(
                [activity = self.activity, cb](int code, vlink::Process::ExitStatus status) {
                  PythonCallbackScope active(activity);

                  if VUNLIKELY (!Py_IsInitialized()) {
                    return;
                  }

                  nb::gil_scoped_acquire gil;
                  try {
                    cb->fn(code, status);
                  } catch (std::exception&) {
                    report_current_exception("vlink::Process.register_finished_callback");
                  }
                });
          },
          "callback"_a)
      .def(
          "register_state_changed_callback",
          [](PythonProcess& self, nb::callable callback) {
            auto cb = std::make_shared<GilSafePyFunction>(std::move(callback));
            self->register_state_changed_callback([activity = self.activity, cb](vlink::Process::State state) {
              PythonCallbackScope active(activity);

              if VUNLIKELY (!Py_IsInitialized()) {
                return;
              }

              nb::gil_scoped_acquire gil;
              try {
                cb->fn(state);
              } catch (std::exception&) {
                report_current_exception("vlink::Process.register_state_changed_callback");
              }
            });
          },
          "callback"_a)
      .def(
          "register_ready_read_stdout_callback",
          [](PythonProcess& self, nb::callable callback) {
            self->register_ready_read_stdout_callback(make_stateful_void_callback(
                self.activity, std::move(callback), "vlink::Process.register_ready_read_stdout_callback"));
          },
          "callback"_a)
      .def(
          "register_ready_read_stderr_callback",
          [](PythonProcess& self, nb::callable callback) {
            self->register_ready_read_stderr_callback(make_stateful_void_callback(
                self.activity, std::move(callback), "vlink::Process.register_ready_read_stderr_callback"));
          },
          "callback"_a)
      .def(
          "start",
          [](PythonProcess& self, const std::string& program, const std::vector<std::string>& arguments) {
            if VUNLIKELY (is_in_python_callback(self.activity.get())) {
              throw std::runtime_error("Process.start() cannot be called from that process's active callback");
            }

            PythonCallbackScope call(self.activity);
            self->start(program, arguments);
          },
          "program"_a, "arguments"_a = std::vector<std::string>{})
      .def(
          "start_command",
          [](PythonProcess& self, const std::string& command) {
            if VUNLIKELY (is_in_python_callback(self.activity.get())) {
              throw std::runtime_error("Process.start_command() cannot be called from that process's active callback");
            }

            PythonCallbackScope call(self.activity);
            self->start_command(command);
          },
          "command"_a)
      .def(
          "wait_for_started",
          [](PythonProcess& self, int timeout_ms) {
            PythonCallbackScope call(self.activity);
            nb::gil_scoped_release release;
            return self->wait_for_started(timeout_ms);
          },
          "timeout_ms"_a = 3000)
      .def(
          "wait_for_finished",
          [](PythonProcess& self, int timeout_ms) {
            PythonCallbackScope call(self.activity);
            nb::gil_scoped_release release;
            return self->wait_for_finished(timeout_ms);
          },
          "timeout_ms"_a = 3000)
      .def(
          "wait_for_ready_read",
          [](PythonProcess& self, int timeout_ms) {
            PythonCallbackScope call(self.activity);
            nb::gil_scoped_release release;
            return self->wait_for_ready_read(timeout_ms);
          },
          "timeout_ms"_a = 3000)
      .def("terminate",
           [](PythonProcess& self) {
             PythonCallbackScope call(self.activity);
             self->terminate();
           })
      .def("kill",
           [](PythonProcess& self) {
             PythonCallbackScope call(self.activity);
             self->kill();
           })
      .def(
          "close",
          [](PythonProcess& self, bool force_kill_on_timeout) {
            if VUNLIKELY (is_in_python_callback(self.activity.get())) {
              throw std::runtime_error("Process.close() cannot be called from that process's active callback");
            }

            PythonCallbackScope call(self.activity);
            nb::gil_scoped_release release;
            self->close(force_kill_on_timeout);
          },
          "force_kill_on_timeout"_a = false)
      .def("bytes_available_stdout", [](const PythonProcess& self) { return self->bytes_available_stdout(); })
      .def("bytes_available_stderr", [](const PythonProcess& self) { return self->bytes_available_stderr(); })
      .def("can_read_line_stdout", [](const PythonProcess& self) { return self->can_read_line_stdout(); })
      .def("can_read_line_stderr", [](const PythonProcess& self) { return self->can_read_line_stderr(); })
      .def("read_line_stdout",
           [](PythonProcess& self) -> nb::object {
             std::string line;

             if (!self->read_line_stdout(line)) {
               return nb::none();
             }

             return nb::object(nb::bytes(line.data(), line.size()));
           })
      .def("read_line_stderr",
           [](PythonProcess& self) -> nb::object {
             std::string line;

             if (!self->read_line_stderr(line)) {
               return nb::none();
             }

             return nb::object(nb::bytes(line.data(), line.size()));
           })
      .def(
          "read_stdout",
          [](PythonProcess& self, size_t max_size) {
            std::vector<uint8_t> buffer;
            self->read_stdout(buffer, max_size);
            return nb::bytes(buffer.empty() ? nullptr : reinterpret_cast<const char*>(buffer.data()), buffer.size());
          },
          "max_size"_a = static_cast<size_t>(0))
      .def(
          "read_stderr",
          [](PythonProcess& self, size_t max_size) {
            std::vector<uint8_t> buffer;
            self->read_stderr(buffer, max_size);
            return nb::bytes(buffer.empty() ? nullptr : reinterpret_cast<const char*>(buffer.data()), buffer.size());
          },
          "max_size"_a = static_cast<size_t>(0))
      .def("read_all_output",
           [](PythonProcess& self) {
             std::string result;
             self->read_all_output(result);
             return nb::bytes(result.data(), result.size());
           })
      .def("read_all_error",
           [](PythonProcess& self) {
             std::string result;
             self->read_all_error(result);
             return nb::bytes(result.data(), result.size());
           })
      .def("read_all",
           [](PythonProcess& self) {
             std::string result;
             self->read_all(result);
             return nb::bytes(result.data(), result.size());
           })
      .def(
          "write",
          [](PythonProcess& self, const std::string& data, int timeout_ms) {
            PythonCallbackScope call(self.activity);
            nb::gil_scoped_release release;
            return self->write(data, timeout_ms);
          },
          "data"_a, "timeout_ms"_a = 5000)
      .def(
          "write",
          [](PythonProcess& self, nb::handle data, int timeout_ms) {
            PythonBufferView view(data);
            std::vector<uint8_t> buffer(view.data(), view.data() + view.size());
            PythonCallbackScope call(self.activity);
            nb::gil_scoped_release release;
            return self->write(buffer, timeout_ms);
          },
          "data"_a, "timeout_ms"_a = 5000)
      .def("close_write_channel", [](PythonProcess& self) { self->close_write_channel(); })
      .def_static(
          "execute",
          [](const std::string& program, const std::vector<std::string>& arguments, int timeout_ms) {
            nb::gil_scoped_release release;
            return vlink::Process::execute(program, arguments, timeout_ms);
          },
          "program"_a, "arguments"_a = std::vector<std::string>{}, "timeout_ms"_a = 30000)
      .def_static("start_detached", &vlink::Process::start_detached, "program"_a,
                  "arguments"_a = std::vector<std::string>{});

  proc.attr("INFINITE") = vlink::Process::kInfinite;
  proc.attr("DEFAULT_WAIT_TIMEOUT_MS") = vlink::Process::kDefaultWaitTimeoutMs;
  proc.attr("DEFAULT_WRITE_TIMEOUT_MS") = vlink::Process::kDefaultWriteTimeoutMs;
  proc.attr("DEFAULT_EXECUTE_TIMEOUT_MS") = vlink::Process::kDefaultExecuteTimeoutMs;
  proc.attr("DESTRUCTOR_WAIT_TIMEOUT_MS") = vlink::Process::kDestructorWaitTimeoutMs;

  auto utils = m.def_submodule("utils", "System utilities");
  utils.def("get_app_path", &vlink::Utils::get_app_path);
  utils.def("get_app_dir", &vlink::Utils::get_app_dir);
  utils.def("get_app_name", &vlink::Utils::get_app_name);
  utils.def("get_host_name", &vlink::Utils::get_host_name);
  utils.def("get_pid", &vlink::Utils::get_pid);
  utils.def("get_pid_str", &vlink::Utils::get_pid_str);
  utils.def("get_tmp_dir", &vlink::Utils::get_tmp_dir);
  utils.def("get_machine_id", &vlink::Utils::get_machine_id);
  utils.def("get_env", &vlink::Utils::get_env, "key"_a, "default_value"_a = "");
  utils.def("set_env", &vlink::Utils::set_env, "key"_a, "value"_a, "force"_a = true);
  utils.def("unset_env", &vlink::Utils::unset_env, "key"_a);
  utils.def(
      "wait_for_device",
      [](const std::string& path, int timeout_ms, int poll_ms) {
        nb::gil_scoped_release release;
        return vlink::Utils::wait_for_device(path, timeout_ms, poll_ms);
      },
      "path"_a, "timeout_ms"_a, "poll_ms"_a = 50);
  utils.def("get_all_ipv4_address", &vlink::Utils::get_all_ipv4_address, "filter_available"_a = false);
  utils.def("get_all_ipv6_address", &vlink::Utils::get_all_ipv6_address, "filter_available"_a = false);
  utils.def("get_interface_name_by_ipv4", &vlink::Utils::get_interface_name_by_ipv4, "addr"_a);
  utils.def("get_interface_name_by_ipv6", &vlink::Utils::get_interface_name_by_ipv6, "addr"_a);
  utils.def("set_thread_name", [](const std::string& name) { return vlink::Utils::set_thread_name(name); }, "name"_a);
  utils.def(
      "set_thread_priority",
      [](int priority_level, int policy) { return vlink::Utils::set_thread_priority(priority_level, policy); },
      "priority_level"_a, "policy"_a = -1);
  utils.def(
      "set_thread_stick", [](uint32_t core_mask) { return vlink::Utils::set_thread_stick(core_mask); }, "core_mask"_a);
  utils.def("set_console_utf8_output", &vlink::Utils::set_console_utf8_output);
  utils.def("get_dds_default_address", &vlink::Utils::get_dds_default_address, "filter_available"_a = false,
            "max_count"_a = 5);
  utils.def("get_native_thread_id", &vlink::Utils::get_native_thread_id);
  utils.def("get_cpu_usage", &vlink::Utils::get_cpu_usage);
  utils.def("get_memory_usage", &vlink::Utils::get_memory_usage);
  utils.def("is_process_running", &vlink::Utils::is_process_running, "name"_a);
  utils.def("try_release_sys_memory", &vlink::Utils::try_release_sys_memory);
  utils.def("get_timezone_diff", &vlink::Utils::get_timezone_diff);
  utils.def("yield_cpu", &vlink::Utils::yield_cpu);
  utils.def("check_singleton", &vlink::Utils::check_singleton, "name"_a);
  utils.def(
      "get_terminal_size",
      []() {
        auto [w, h] = vlink::Utils::get_terminal_size();
        return nb::make_tuple(w, h);
      },
      "Returns (width, height) of the terminal");
  utils.def(
      "start_detect_keyboard",
      [](nb::callable callback, int poll_ms) {
        auto activity = std::make_shared<PythonCallbackActivity>();
        auto cb = std::make_shared<GilSafePyFunction>(std::move(callback));
        vlink::Utils::start_detect_keyboard(
            [activity, cb](const std::string& key) {
              PythonCallbackScope active(activity, python_keyboard_callback_owner());

              if VUNLIKELY (!Py_IsInitialized()) {
                return;
              }

              nb::gil_scoped_acquire gil;
              try {
                cb->fn(key);
              } catch (std::exception&) {
                report_current_exception("vlink::Utils.start_detect_keyboard");
              }
            },
            poll_ms);
      },
      "callback"_a, "poll_ms"_a = 100, "Start keyboard input detection: callback(key: str)");
  utils.def("stop_detect_keyboard", []() {
    if VUNLIKELY (is_in_python_owner_callback(python_keyboard_callback_owner())) {
      throw std::runtime_error("stop_detect_keyboard() cannot be called from its keyboard callback");
    }

    nb::gil_scoped_release release;
    vlink::Utils::stop_detect_keyboard();
  });
  utils.def(
      "register_crash_signal",
      [](nb::callable) {
        throw std::runtime_error(
            "Python callbacks cannot safely run from fatal signal handlers; use an external crash reporter");
      },
      "callback"_a,
      "Fatal signal callbacks are intentionally unsupported because Python execution is not async-signal-safe.");
  utils.def(
      "register_terminate_signal",
      [](nb::callable callback, bool async_mode, bool passthrough) {
        if VUNLIKELY (passthrough && async_mode) {
          throw nb::value_error("pass_through=True is incompatible with is_async=True");
        }

        auto cb = std::make_shared<GilSafePyFunction>(std::move(callback));
        nb::object handler = nb::cpp_function([cb, async_mode, passthrough](int sig, nb::object) {
          const auto pass_through_signal = [sig]() {
            if VUNLIKELY (std::signal(sig, SIG_DFL) == SIG_ERR) {
              throw std::runtime_error("failed to restore the default signal handler");
            }

            std::raise(sig);
          };

          if (async_mode) {
            std::thread([cb, sig]() {
              if VUNLIKELY (!Py_IsInitialized()) {
                return;
              }

              nb::gil_scoped_acquire gil;
              try {
                cb->fn(sig);
              } catch (std::exception&) {
                report_current_exception("vlink::Utils.register_terminate_signal");
              }
            }).detach();
          } else {
            try {
              cb->fn(sig);
            } catch (...) {
              if (passthrough) {
                pass_through_signal();
              }

              throw;
            }
          }

          if (passthrough) {
            pass_through_signal();
          }
        });
        nb::module_ signal = nb::module_::import_("signal");
        signal.attr("signal")(signal.attr("SIGINT"), handler);
        signal.attr("signal")(signal.attr("SIGTERM"), handler);
#ifndef _WIN32
        signal.attr("signal")(signal.attr("SIGHUP"), handler);
#endif
      },
      "callback"_a, "is_async"_a = false, "pass_through"_a = false,
      "Register graceful-termination callbacks through Python's main-thread signal dispatcher.");

  nb::class_<vlink::Uuid> uuid_cls(m, "Uuid", "RFC 4122 128-bit UUID value type with v4 random generation");

  nb::enum_<vlink::Uuid::Variant>(uuid_cls, "Variant", "UUID variant field (RFC 4122 section 4.1.1)")
      .value("Ncs", vlink::Uuid::Variant::kNcs)
      .value("Rfc", vlink::Uuid::Variant::kRfc)
      .value("Microsoft", vlink::Uuid::Variant::kMicrosoft)
      .value("Reserved", vlink::Uuid::Variant::kReserved);

  nb::enum_<vlink::Uuid::Version>(uuid_cls, "Version", "UUID version field (RFC 4122 section 4.1.3)")
      .value("None_", vlink::Uuid::Version::kNone)
      .value("TimeBased", vlink::Uuid::Version::kTimeBased)
      .value("DceSecurity", vlink::Uuid::Version::kDceSecurity)
      .value("NameBasedMd5", vlink::Uuid::Version::kNameBasedMd5)
      .value("RandomBased", vlink::Uuid::Version::kRandomBased)
      .value("NameBasedSha1", vlink::Uuid::Version::kNameBasedSha1);

  uuid_cls.def(nb::init<>(), "Default-constructs a nil (all-zero) UUID.")
      .def(nb::init<const std::array<uint8_t, vlink::Uuid::kByteSize>&>(), "data"_a,
           "Constructs from a 16-byte std::array payload.")
      .def(
          "__init__",
          [](vlink::Uuid* self, nb::bytes data) {
            if (data.size() != vlink::Uuid::kByteSize) {
              throw nb::value_error("Uuid requires exactly 16 bytes");
            }
            std::array<uint8_t, vlink::Uuid::kByteSize> array{};
            std::memcpy(array.data(), data.c_str(), vlink::Uuid::kByteSize);
            new (self) vlink::Uuid(array);
          },
          "data"_a, "Constructs from a Python bytes-like object of length 16.")
      .def_ro_static("BYTE_SIZE", &vlink::Uuid::kByteSize)
      .def_ro_static("STRING_SIZE", &vlink::Uuid::kStringSize)
      .def("variant", &vlink::Uuid::variant, "Returns the UUID variant field.")
      .def("version", &vlink::Uuid::version, "Returns the UUID version field.")
      .def("is_nil", &vlink::Uuid::is_nil, "Returns True when every byte is zero.")
      .def(
          "bytes",
          [](const vlink::Uuid& self) {
            const auto& data = self.bytes();
            return nb::bytes(reinterpret_cast<const char*>(data.data()), data.size());
          },
          "Returns the 16 underlying bytes as a Python bytes object.")
      .def("to_string", &vlink::Uuid::to_string, "Formats as the 36-character lowercase canonical form with hyphens.")
      .def("to_compact_string", &vlink::Uuid::to_compact_string,
           "Formats as a 32-character lowercase hex string with no hyphens.")
      .def("__str__", &vlink::Uuid::to_string)
      .def("__repr__", [](const vlink::Uuid& self) { return std::string("<vlink.Uuid '") + self.to_string() + "'>"; })
      .def("__eq__", [](const vlink::Uuid& lhs, const vlink::Uuid& rhs) { return lhs == rhs; })
      .def("__ne__", [](const vlink::Uuid& lhs, const vlink::Uuid& rhs) { return lhs != rhs; })
      .def("__lt__", [](const vlink::Uuid& lhs, const vlink::Uuid& rhs) { return lhs < rhs; })
      .def("__hash__", [](const vlink::Uuid& self) { return static_cast<int64_t>(std::hash<vlink::Uuid>{}(self)); })
      .def_static(
          "is_valid", [](const std::string& str) { return vlink::Uuid::is_valid(std::string_view(str)); }, "str"_a,
          "Returns True when str is a well-formed UUID textual representation.")
      .def_static(
          "from_string",
          [](const std::string& str) -> nb::object {
            auto parsed = vlink::Uuid::from_string(std::string_view(str));

            if (!parsed.has_value()) {
              return nb::none();
            }

            return nb::cast(*parsed);
          },
          "str"_a, "Parses a UUID string; returns None on malformed input.")
      .def_static("generate_random", static_cast<vlink::Uuid (*)() noexcept>(&vlink::Uuid::generate_random),
                  "Generates a random v4 UUID using a thread-local seeded engine.")
      .def_static(
          "random_bytes",
          [](size_t count) {
            auto buf = vlink::Uuid::random_bytes(count);
            return nb::bytes(reinterpret_cast<const char*>(buf.data()), buf.size());
          },
          "count"_a, "Returns count cryptographically-seeded pseudo-random bytes (NOT a CSPRNG).")
      .def_static("random_hex", &vlink::Uuid::random_hex, "byte_count"_a = static_cast<size_t>(16U),
                  "Returns byte_count random bytes encoded as a lowercase hex string (NOT a CSPRNG).");

  auto helpers = m.def_submodule("helpers", "String and formatting utilities");
  helpers.def("to_int", &vlink::Helpers::to_int, "str"_a, "default_value"_a = 0);
  helpers.def("to_long", &vlink::Helpers::to_long, "str"_a, "default_value"_a = 0, "offset"_a = 0);
  helpers.def("double_to_string", &vlink::Helpers::double_to_string, "value"_a, "precision"_a = 2);
  helpers.def("get_hash_code", nb::overload_cast<const std::string&>(&vlink::Helpers::get_hash_code), "str"_a);
  helpers.def("hash_combine", &vlink::Helpers::hash_combine, "a"_a, "b"_a);
  helpers.def("format_file_size", &vlink::Helpers::format_file_size, "bytes"_a);
  helpers.def("format_rate_size", &vlink::Helpers::format_rate_size, "bytes_per_sec"_a);
  helpers.def("format_milliseconds", &vlink::Helpers::format_milliseconds, "ms"_a, "show_millis"_a = true);
  helpers.def("format_date", &vlink::Helpers::format_date, "nanoseconds_since_epoch"_a);
  helpers.def("format_time_diff", &vlink::Helpers::format_time_diff, "milliseconds"_a);
  helpers.def("format_hex_number", nb::overload_cast<int64_t>(&vlink::Helpers::format_hex_number), "value"_a);
  helpers.def(
      "has_startwith",
      [](const std::string& s, const std::string& t) {
        return vlink::Helpers::has_startwith(std::string_view(s), std::string_view(t));
      },
      "str"_a, "target"_a);
  helpers.def(
      "has_endwith",
      [](const std::string& s, const std::string& t) {
        return vlink::Helpers::has_endwith(std::string_view(s), std::string_view(t));
      },
      "str"_a, "target"_a);
  helpers.def(
      "contains_substring",
      [](const std::string& s, const std::string& t) {
        return vlink::Helpers::contains_substring(std::string_view(s), std::string_view(t));
      },
      "str"_a, "target"_a);
  helpers.def("trim_string", &vlink::Helpers::trim_string, "str"_a);
  helpers.def(
      "trim_string_view",
      [](const std::string& str) { return std::string(vlink::Helpers::trim_string_view(std::string_view(str))); },
      "str"_a);
  helpers.def(
      "replace_string",
      [](std::string str, const std::string& from_substring, const std::string& to_substring) {
        vlink::Helpers::replace_string(str, from_substring, to_substring);
        return str;
      },
      "str"_a, "from_substring"_a, "to_substring"_a);
  helpers.def("string_local_to_utf8", &vlink::Helpers::string_local_to_utf8, "local_str"_a);
  helpers.def("string_utf8_to_local", &vlink::Helpers::string_utf8_to_local, "utf8_str"_a);
  helpers.def(
      "string_to_wstring",
      [](const std::string& input) { return wide_string_to_python_str(vlink::Helpers::string_to_wstring(input)); },
      "input"_a);
  helpers.def(
      "wstring_to_string",
      [](nb::str input) { return vlink::Helpers::wstring_to_string(python_str_to_wide_string(input)); }, "input"_a);
  helpers.def(
      "path_to_string",
      [](const std::string& path) { return vlink::Helpers::path_to_string(std::filesystem::path(path)); }, "path"_a);
  helpers.def("format_hex_number_unsigned", nb::overload_cast<uint64_t>(&vlink::Helpers::format_hex_number), "value"_a);
  helpers.def(
      "split",
      [](const std::string& s, const std::string& d) { return vlink::Helpers::split(s, d.empty() ? ',' : d[0]); },
      "str"_a, "delimiter"_a = ",");
  helpers.def(
      "split_view",
      [](const std::string& s, const std::string& d) {
        auto views = vlink::Helpers::split_view(s, d.empty() ? ',' : d[0]);
        std::vector<std::string> parts;
        parts.reserve(views.size());
        for (auto view : views) {
          parts.emplace_back(view);
        }
        return parts;
      },
      "str"_a, "delimiter"_a = ",");
  helpers.def(
      "split_any",
      [](const std::string& s, const std::string& delimiters) {
        return vlink::Helpers::split_any(s, std::string_view(delimiters));
      },
      "str"_a, "delimiters"_a = " ,");
  helpers.def(
      "split_any_view",
      [](const std::string& s, const std::string& delimiters) {
        auto views = vlink::Helpers::split_any_view(s, std::string_view(delimiters));
        std::vector<std::string> parts;
        parts.reserve(views.size());
        for (auto view : views) {
          parts.emplace_back(view);
        }
        return parts;
      },
      "str"_a, "delimiters"_a = " ,");
  helpers.def("convert_date_to_timestamp", &vlink::Helpers::convert_date_to_timestamp, "date_string"_a);

  nb::class_<vlink::Qos> qos_cls(m, "Qos", "DDS-compatible Quality of Service");

  nb::class_<vlink::Qos::Reliability> qos_rel(qos_cls, "Reliability");
  nb::enum_<vlink::Qos::Reliability::Kind>(qos_rel, "Kind")
      .value("BestEffort", vlink::Qos::Reliability::kBestEffort)
      .value("Reliable", vlink::Qos::Reliability::kReliable);
  qos_rel.def(nb::init<>())
      .def_rw("kind", &vlink::Qos::Reliability::kind)
      .def_rw("block_time", &vlink::Qos::Reliability::block_time)
      .def_rw("heartbeat_time", &vlink::Qos::Reliability::heartbeat_time);

  nb::class_<vlink::Qos::History> qos_hist(qos_cls, "History");
  nb::enum_<vlink::Qos::History::Kind>(qos_hist, "Kind")
      .value("KeepLast", vlink::Qos::History::kKeepLast)
      .value("KeepAll", vlink::Qos::History::kKeepAll);
  qos_hist.def(nb::init<>()).def_rw("kind", &vlink::Qos::History::kind).def_rw("depth", &vlink::Qos::History::depth);

  nb::class_<vlink::Qos::Durability> qos_dur(qos_cls, "Durability");
  nb::enum_<vlink::Qos::Durability::Kind>(qos_dur, "Kind")
      .value("Volatile", vlink::Qos::Durability::kVolatile)
      .value("TransientLocal", vlink::Qos::Durability::kTransientLocal)
      .value("Transient", vlink::Qos::Durability::kTransient)
      .value("Persistent", vlink::Qos::Durability::kPersistent);
  qos_dur.def(nb::init<>()).def_rw("kind", &vlink::Qos::Durability::kind);

  nb::class_<vlink::Qos::PublishMode> qos_pm(qos_cls, "PublishMode");
  nb::enum_<vlink::Qos::PublishMode::Kind>(qos_pm, "Kind")
      .value("Sync", vlink::Qos::PublishMode::kSync)
      .value("ASync", vlink::Qos::PublishMode::kASync);
  qos_pm.def(nb::init<>()).def_rw("kind", &vlink::Qos::PublishMode::kind);

  nb::class_<vlink::Qos::Liveliness> qos_lv(qos_cls, "Liveliness");
  nb::enum_<vlink::Qos::Liveliness::Kind>(qos_lv, "Kind")
      .value("Automatic", vlink::Qos::Liveliness::kAutomatic)
      .value("ManualParticipant", vlink::Qos::Liveliness::kManualParticipant)
      .value("ManualTopic", vlink::Qos::Liveliness::kManualTopic);
  qos_lv.def(nb::init<>())
      .def_rw("kind", &vlink::Qos::Liveliness::kind)
      .def_rw("duration", &vlink::Qos::Liveliness::duration);

  nb::class_<vlink::Qos::DestinationOrder> qos_do(qos_cls, "DestinationOrder");
  nb::enum_<vlink::Qos::DestinationOrder::Kind>(qos_do, "Kind")
      .value("ReceptionTimestamp", vlink::Qos::DestinationOrder::kReceptionTimestamp)
      .value("SourceTimestamp", vlink::Qos::DestinationOrder::kSourceTimestamp);
  qos_do.def(nb::init<>()).def_rw("kind", &vlink::Qos::DestinationOrder::kind);

  nb::class_<vlink::Qos::Ownership> qos_own(qos_cls, "Ownership");
  nb::enum_<vlink::Qos::Ownership::Kind>(qos_own, "Kind")
      .value("Shared", vlink::Qos::Ownership::kShared)
      .value("Exclusive", vlink::Qos::Ownership::kExclusive);
  qos_own.def(nb::init<>()).def_rw("kind", &vlink::Qos::Ownership::kind);

  nb::class_<vlink::Qos::Deadline>(qos_cls, "Deadline")
      .def(nb::init<>())
      .def_rw("period", &vlink::Qos::Deadline::period);
  nb::class_<vlink::Qos::Lifespan>(qos_cls, "Lifespan")
      .def(nb::init<>())
      .def_rw("duration", &vlink::Qos::Lifespan::duration);
  nb::class_<vlink::Qos::LatencyBudget>(qos_cls, "LatencyBudget")
      .def(nb::init<>())
      .def_rw("duration", &vlink::Qos::LatencyBudget::duration);

  nb::class_<vlink::Qos::ResourceLimits>(qos_cls, "ResourceLimits")
      .def(nb::init<>())
      .def_rw("max_samples", &vlink::Qos::ResourceLimits::max_samples)
      .def_rw("max_instances", &vlink::Qos::ResourceLimits::max_instances)
      .def_rw("max_samples_per_instance", &vlink::Qos::ResourceLimits::max_samples_per_instance);

  nb::class_<vlink::Qos::Additions> qos_add(qos_cls, "Additions");
  nb::enum_<vlink::Qos::Additions::Priority>(qos_add, "Priority")
      .value("RealTime", vlink::Qos::Additions::kPriorityRealTime)
      .value("High", vlink::Qos::Additions::kPriorityHigh)
      .value("Normal", vlink::Qos::Additions::kPriorityNormal)
      .value("Low", vlink::Qos::Additions::kPriorityLow)
      .value("Background", vlink::Qos::Additions::kPriorityBackground);
  qos_add.def(nb::init<>())
      .def_rw("priority", &vlink::Qos::Additions::priority)
      .def_rw("is_express", &vlink::Qos::Additions::is_express);

  qos_cls.def(nb::init<>())
      .def_prop_rw(
          "name", [](const vlink::Qos& q) { return std::string(q.name); },
          [](vlink::Qos& q, const std::string& s) {
            constexpr size_t kMax = sizeof(vlink::Qos::name) - 1;
            const auto prefix = utf8_prefix(s, kMax);
            std::memcpy(q.name, prefix.data(), prefix.size());
            q.name[prefix.size()] = '\0';
          })
      .def_rw("valid", &vlink::Qos::valid)
      .def_rw("reliability", &vlink::Qos::reliability)
      .def_rw("history", &vlink::Qos::history)
      .def_rw("durability", &vlink::Qos::durability)
      .def_rw("publish_mode", &vlink::Qos::publish_mode)
      .def_rw("liveliness", &vlink::Qos::liveliness)
      .def_rw("destination_order", &vlink::Qos::destination_order)
      .def_rw("ownership", &vlink::Qos::ownership)
      .def_rw("deadline", &vlink::Qos::deadline)
      .def_rw("lifespan", &vlink::Qos::lifespan)
      .def_rw("latency_budget", &vlink::Qos::latency_budget)
      .def_rw("resource_limits", &vlink::Qos::resource_limits)
      .def_rw("additions", &vlink::Qos::additions)
      .def("__repr__", [](const vlink::Qos& q) {
        return std::string("Qos(name='") + q.name + "', valid=" + (q.valid ? "True" : "False") + ")";
      });

  auto qos_profile = m.def_submodule("QosProfile", "Pre-defined QoS profiles");
  qos_profile.attr("Event") = vlink::QosProfile::kEvent;
  qos_profile.attr("Method") = vlink::QosProfile::kMethod;
  qos_profile.attr("Field") = vlink::QosProfile::kField;
  qos_profile.attr("Sensor") = vlink::QosProfile::kSensor;
  qos_profile.attr("Parameter") = vlink::QosProfile::kParameter;
  qos_profile.attr("Service") = vlink::QosProfile::kService;
  qos_profile.attr("Clock") = vlink::QosProfile::kClock;
  qos_profile.attr("Static") = vlink::QosProfile::kStatic;
  qos_profile.attr("Light") = vlink::QosProfile::kLight;
  qos_profile.attr("Poor") = vlink::QosProfile::kPoor;
  qos_profile.attr("Better") = vlink::QosProfile::kBetter;
  qos_profile.attr("Best") = vlink::QosProfile::kBest;
  qos_profile.attr("Stream") = vlink::QosProfile::kStream;
  qos_profile.attr("Alarm") = vlink::QosProfile::kAlarm;
  qos_profile.attr("Command") = vlink::QosProfile::kCommand;
  qos_profile.attr("Log") = vlink::QosProfile::kLog;
  qos_profile.def("get_available_qos_map", &vlink::QosProfile::get_available_qos_map);

  nb::enum_<vlink::Status::Type>(m, "StatusType")
      .value("PublicationMatched", vlink::Status::kPublicationMatched)
      .value("SubscriptionMatched", vlink::Status::kSubscriptionMatched)
      .value("OfferedDeadlineMissed", vlink::Status::kOfferedDeadlineMissed)
      .value("RequestedDeadlineMissed", vlink::Status::kRequestedDeadlineMissed)
      .value("OfferedIncompatibleQos", vlink::Status::kOfferedIncompatibleQos)
      .value("RequestedIncompatibleQos", vlink::Status::kRequestedIncompatibleQos)
      .value("LivelinessLost", vlink::Status::kLivelinessLost)
      .value("LivelinessChanged", vlink::Status::kLivelinessChanged)
      .value("SampleRejected", vlink::Status::kSampleRejected)
      .value("SampleLost", vlink::Status::kSampleLost)
      .value("Unknown", vlink::Status::kUnknown);

  nb::class_<vlink::SslOptions>(m, "SslOptions")
      .def(nb::init<>())
      .def_rw("verify_peer", &vlink::SslOptions::verify_peer)
      .def_rw("ca_file", &vlink::SslOptions::ca_file)
      .def_rw("cert_file", &vlink::SslOptions::cert_file)
      .def_rw("key_file", &vlink::SslOptions::key_file)
      .def_rw("key_password", &vlink::SslOptions::key_password)
      .def_rw("server_name", &vlink::SslOptions::server_name)
      .def_rw("ciphers", &vlink::SslOptions::ciphers)
      .def("is_valid", &vlink::SslOptions::is_valid);

  auto status = m.def_submodule("Status", "Status helper functions");
  status.def("is_for_writer", &vlink::Status::is_for_writer, "type"_a);
  status.def("is_for_reader", &vlink::Status::is_for_reader, "type"_a);

  nb::class_<vlink::Security::Config::Advanced>(
      m, "SecurityConfigAdvanced", "Low-frequency security options for AAD, replay protection, and signing")
      .def(nb::init<>())
      .def_rw("aad_context", &vlink::Security::Config::Advanced::aad_context)
      .def_rw("replay_window", &vlink::Security::Config::Advanced::replay_window)
      .def_rw("signing_key_pem", &vlink::Security::Config::Advanced::signing_key_pem)
      .def_rw("verify_key_pem", &vlink::Security::Config::Advanced::verify_key_pem);

  nb::class_<vlink::Security::Config>(m, "SecurityConfig",
                                      "Aggregate of every parameter accepted by the Security constructor")
      .def(nb::init<>())
      .def_rw("key", &vlink::Security::Config::key)
      .def_rw("passphrase", &vlink::Security::Config::passphrase)
      .def_rw("pbkdf2_salt", &vlink::Security::Config::pbkdf2_salt)
      .def_rw("pbkdf2_iterations", &vlink::Security::Config::pbkdf2_iterations)
      .def_rw("public_key_pem", &vlink::Security::Config::public_key_pem)
      .def_rw("private_key_pem", &vlink::Security::Config::private_key_pem)
      .def_rw("advanced", &vlink::Security::Config::advanced)
      .def_prop_rw(
          "encrypt_callback",
          [](const vlink::Security::Config& self) -> nb::object {
            // The original Python callable has been wrapped into a C++ Function and is no longer
            // retrievable as a Python object.  Return @c True when a callback is installed, @c None
            // otherwise, so Python callers can at least query whether the slot is populated.
            return self.encrypt_callback ? nb::cast(true) : nb::none();
          },
          [](vlink::Security::Config& self, nb::callable cb) {
            self.encrypt_callback = make_security_callback(std::move(cb), "vlink::Security::Config.encrypt_callback");
          })
      .def_prop_rw(
          "decrypt_callback",
          [](const vlink::Security::Config& self) -> nb::object {
            return self.decrypt_callback ? nb::cast(true) : nb::none();
          },
          [](vlink::Security::Config& self, nb::callable cb) {
            self.decrypt_callback = make_security_callback(std::move(cb), "vlink::Security::Config.decrypt_callback");
          });

  nb::class_<vlink::Security>(m, "Security", "Authenticated message-level encryption (AEAD)")
      .def(nb::new_([](vlink::Security::Config cfg) { return new vlink::Security(std::move(cfg)); }),
           "cfg"_a = vlink::Security::Config{})
      .def_static("from_private_key_path", &vlink::Security::from_private_key_path, "private_key_path"_a,
                  "Create a SecurityConfig by reading a private-key PEM file.")
      .def_static("from_public_key_path", &vlink::Security::from_public_key_path, "public_key_path"_a,
                  "Create a SecurityConfig by reading a public-key PEM file.")
      .def_static("from_key_paths", &vlink::Security::from_key_paths, "public_key_path"_a, "private_key_path"_a,
                  "Create a SecurityConfig by reading public- and private-key PEM files.")
      .def(
          "encrypt",
          [](vlink::Security& self, nb::handle data) -> nb::object {
            PythonBufferView view(data);
            auto in_bytes = vlink::Bytes::shallow_copy(view.data(), view.size());
            vlink::Bytes out;

            if VLIKELY (self.encrypt(in_bytes, out)) {
              return PythonCodec<vlink::Bytes>::to_python(out);
            }

            return nb::none();
          },
          "data"_a)
      .def(
          "decrypt",
          [](vlink::Security& self, nb::handle data) -> nb::object {
            PythonBufferView view(data);
            auto in_bytes = vlink::Bytes::shallow_copy(view.data(), view.size());
            vlink::Bytes out;

            if VLIKELY (self.decrypt(in_bytes, out)) {
              return PythonCodec<vlink::Bytes>::to_python(out);
            }

            return nb::none();
          },
          "data"_a)
      .def("is_configured", &vlink::Security::is_configured,
           "Return True iff at least one cryptographic slot (symmetric key, RSA keypair, or "
           "encrypt+decrypt callback pair) is usable.")
      .def("can_encrypt", &vlink::Security::can_encrypt,
           "Return True iff encrypt() will produce a ciphertext for at least one configured mode "
           "(custom callbacks > RSA public key > symmetric key).")
      .def("can_decrypt", &vlink::Security::can_decrypt,
           "Return True iff decrypt() can recover a plaintext for at least one configured mode "
           "(custom callbacks > RSA private key > symmetric key).");

  nb::class_<vlink::UrlRemap>(m, "UrlRemap", "JSON-driven URL pattern remapping")
      .def(nb::init<>())
      .def("load", &vlink::UrlRemap::load, "file_path"_a)
      .def("unload", &vlink::UrlRemap::unload)
      .def("reload", &vlink::UrlRemap::reload, "file_path"_a)
      .def("convert", &vlink::UrlRemap::convert, "url"_a, nb::rv_policy::reference)
      .def("set_enable_log", &vlink::UrlRemap::set_enable_log, "enable"_a)
      .def("is_enable_log", &vlink::UrlRemap::is_enable_log)
      .def("is_valid", &vlink::UrlRemap::is_valid)
      .def("get_error_string", &vlink::UrlRemap::get_error_string);

  using BytesPub = vlink::Publisher<vlink::Bytes>;
  using BytesSub = vlink::Subscriber<vlink::Bytes>;
  using BytesSrv = vlink::Server<vlink::Bytes, vlink::Bytes>;
  using BytesCli = vlink::Client<vlink::Bytes, vlink::Bytes>;
  using BytesFireSrv = vlink::Server<vlink::Bytes, vlink::Traits::EmptyType>;
  using BytesFireCli = vlink::Client<vlink::Bytes, vlink::Traits::EmptyType>;
  using BytesSet = vlink::Setter<vlink::Bytes>;
  using BytesGet = vlink::Getter<vlink::Bytes>;
  using SecBytesPub = vlink::SecurityPublisher<vlink::Bytes>;
  using SecBytesSub = vlink::SecuritySubscriber<vlink::Bytes>;
  using SecBytesSrv = vlink::SecurityServer<vlink::Bytes, vlink::Bytes>;
  using SecBytesCli = vlink::SecurityClient<vlink::Bytes, vlink::Bytes>;
  using SecBytesFireSrv = vlink::SecurityServer<vlink::Bytes, vlink::Traits::EmptyType>;
  using SecBytesFireCli = vlink::SecurityClient<vlink::Bytes, vlink::Traits::EmptyType>;
  using SecBytesSet = vlink::SecuritySetter<vlink::Bytes>;
  using SecBytesGet = vlink::SecurityGetter<vlink::Bytes>;

  bind_publisher<BytesPub, vlink::Bytes>(m, "Publisher", "Event-model publisher");
  bind_subscriber<BytesSub, vlink::Bytes>(m, "Subscriber", "Event-model subscriber");
  bind_server<BytesSrv, vlink::Bytes, vlink::Bytes>(m, "Server", "Method-model server");
  bind_client<BytesCli, vlink::Bytes, vlink::Bytes>(m, "Client", "Method-model client");
  bind_fire_forget_server<BytesFireSrv, vlink::Bytes>(m, "FireForgetServer", "Fire-and-forget method server");
  bind_fire_forget_client<BytesFireCli, vlink::Bytes>(m, "FireForgetClient", "Fire-and-forget method client");
  bind_setter<BytesSet, vlink::Bytes>(m, "Setter", "Field-model setter");
  bind_getter<BytesGet, vlink::Bytes>(m, "Getter", "Field-model getter");
  bind_publisher<SecBytesPub, vlink::Bytes, PythonCodec<vlink::Bytes>, true>(
      m, "SecurityPublisher", "Event-model publisher with payload security");
  bind_subscriber<SecBytesSub, vlink::Bytes, PythonCodec<vlink::Bytes>, true>(
      m, "SecuritySubscriber", "Event-model subscriber with payload security");
  bind_server<SecBytesSrv, vlink::Bytes, vlink::Bytes, PythonCodec<vlink::Bytes>, PythonCodec<vlink::Bytes>, true>(
      m, "SecurityServer", "Method-model server with payload security");
  bind_client<SecBytesCli, vlink::Bytes, vlink::Bytes, PythonCodec<vlink::Bytes>, PythonCodec<vlink::Bytes>, true>(
      m, "SecurityClient", "Method-model client with payload security");
  bind_fire_forget_server<SecBytesFireSrv, vlink::Bytes, PythonCodec<vlink::Bytes>, true>(
      m, "SecurityFireForgetServer", "Fire-and-forget method server with payload security");
  bind_fire_forget_client<SecBytesFireCli, vlink::Bytes, PythonCodec<vlink::Bytes>, true>(
      m, "SecurityFireForgetClient", "Fire-and-forget method client with payload security");
  bind_setter<SecBytesSet, vlink::Bytes, PythonCodec<vlink::Bytes>, true>(m, "SecuritySetter",
                                                                          "Field-model setter with payload security");
  bind_getter<SecBytesGet, vlink::Bytes, PythonCodec<vlink::Bytes>, true>(m, "SecurityGetter",
                                                                          "Field-model getter with payload security");

  nb::class_<vlink::DiscoveryViewer> dv(m, "DiscoveryViewer", "Active endpoint discovery", nb::is_weak_referenceable());
  nb::enum_<vlink::DiscoveryViewer::FilterType>(dv, "FilterType")
      .value("None_", vlink::DiscoveryViewer::kFilterNone)
      .value("Available", vlink::DiscoveryViewer::kFilterAvailable)
      .value("Native", vlink::DiscoveryViewer::kFilterNative);
  nb::class_<vlink::DiscoveryViewer::Process>(dv, "Process", "Process hosting an endpoint")
      .def_ro("type", &vlink::DiscoveryViewer::Process::type)
      .def_ro("host", &vlink::DiscoveryViewer::Process::host)
      .def_ro("pid", &vlink::DiscoveryViewer::Process::pid)
      .def_ro("name", &vlink::DiscoveryViewer::Process::name)
      .def_ro("ip", &vlink::DiscoveryViewer::Process::ip)
      .def_ro("profiler", &vlink::DiscoveryViewer::Process::profiler)
      .def("__repr__", [](const vlink::DiscoveryViewer::Process& p) {
        return "Process(name='" + p.name + "', pid=" + std::to_string(p.pid) + ", host='" + p.host + "')";
      });
  nb::class_<vlink::DiscoveryViewer::Info>(dv, "Info")
      .def_ro("sort_index", &vlink::DiscoveryViewer::Info::sort_index)
      .def_ro("type", &vlink::DiscoveryViewer::Info::type)
      .def_ro("url", &vlink::DiscoveryViewer::Info::url)
      .def_ro("ser_type", &vlink::DiscoveryViewer::Info::ser_type)
      .def_ro("schema_type", &vlink::DiscoveryViewer::Info::schema_type)
      .def_ro("process_list", &vlink::DiscoveryViewer::Info::process_list)
      .def("__repr__", [](const vlink::DiscoveryViewer::Info& i) {
        return "DiscoveryInfo(url='" + i.url + "', type=" + std::to_string(i.type) +
               ", processes=" + std::to_string(i.process_list.size()) + ")";
      });
  dv.def(nb::init<vlink::DiscoveryViewer::FilterType>(), "filter"_a = vlink::DiscoveryViewer::kFilterNone)
      .def(
          "register_callback",
          [](nb::object instance, nb::callable callback) {
            auto& self = nb::cast<vlink::DiscoveryViewer&>(instance);
            ensure_python_pre_destroy_hook(instance, &self, [](vlink::DiscoveryViewer& viewer) {
              viewer.quit(true);
              viewer.wait_for_quit(vlink::Timer::kInfinite, false);
            });
            auto activity = std::make_shared<PythonCallbackActivity>();
            auto cb = std::make_shared<GilSafePyFunction>(std::move(callback));
            self.register_callback(
                [native = &self, activity, cb](const std::vector<vlink::DiscoveryViewer::Info>& info_list) {
                  invoke_owned_python_callback(native, activity, "vlink::DiscoveryViewer.register_callback",
                                               [&]() { cb->fn(info_list); });
                });
          },
          "callback"_a, "Register callback for discovery changes: callback(info_list)")
      .def("get_info_list", &vlink::DiscoveryViewer::get_info_list)
      .def("get_ser_type", &vlink::DiscoveryViewer::get_ser_type, "url"_a)
      .def("get_schema_type", &vlink::DiscoveryViewer::get_schema_type, "url"_a)
      .def_static(
          "convert_type", [](const std::string& type) { return vlink::DiscoveryViewer::convert_type(type); }, "type"_a)
      .def_static("get_listen_address", &vlink::DiscoveryViewer::get_listen_address)
      .def_static("convert_type_to_view", nb::overload_cast<uint32_t>(&vlink::DiscoveryViewer::convert_type_to_view),
                  "type"_a)
      .def_static("convert_type_to_view",
                  nb::overload_cast<uint32_t, const std::vector<vlink::DiscoveryViewer::Process>&>(
                      &vlink::DiscoveryViewer::convert_type_to_view),
                  "type"_a, "process_list"_a);

  nb::class_<vlink::BagWriter> bw(m, "BagWriter", "Message recorder", nb::is_weak_referenceable());
  nb::enum_<vlink::BagWriter::CompressType>(bw, "CompressType")
      .value("NONE", vlink::BagWriter::kCompressNone)
      .value("AUTO", vlink::BagWriter::kCompressAuto)
      .value("ZSTD", vlink::BagWriter::kCompressZstd)
      .value("LZ4", vlink::BagWriter::kCompressLz4)
      .value("LZAV", vlink::BagWriter::kCompressLzav);
  nb::class_<vlink::BagWriter::Config>(bw, "Config")
      .def(nb::init<>())
      .def_rw("tag_name", &vlink::BagWriter::Config::tag_name)
      .def_rw("compress", &vlink::BagWriter::Config::compress)
      .def_rw("wal_mode", &vlink::BagWriter::Config::wal_mode)
      .def_rw("enable_limit", &vlink::BagWriter::Config::enable_limit)
      .def_rw("split_name_by_time", &vlink::BagWriter::Config::split_name_by_time)
      .def_rw("sync_mode", &vlink::BagWriter::Config::sync_mode)
      .def_rw("optimize_on_exit", &vlink::BagWriter::Config::optimize_on_exit)
      .def_rw("max_row_count", &vlink::BagWriter::Config::max_row_count)
      .def_rw("max_bytes_size", &vlink::BagWriter::Config::max_bytes_size)
      .def_rw("split_by_size", &vlink::BagWriter::Config::split_by_size)
      .def_rw("split_by_time", &vlink::BagWriter::Config::split_by_time)
      .def_rw("max_split_count", &vlink::BagWriter::Config::max_split_count)
      .def_rw("begin_time", &vlink::BagWriter::Config::begin_time)
      .def_rw("cache_size", &vlink::BagWriter::Config::cache_size)
      .def_rw("compress_start_size", &vlink::BagWriter::Config::compress_start_size)
      .def_rw("compress_level", &vlink::BagWriter::Config::compress_level)
      .def_rw("max_task_depth", &vlink::BagWriter::Config::max_task_depth)
      .def_rw("max_memory_size", &vlink::BagWriter::Config::max_memory_size)
      .def_rw("start_timestamp", &vlink::BagWriter::Config::start_timestamp)
      .def_rw("ignore_compress_urls", &vlink::BagWriter::Config::ignore_compress_urls);
  bw.def_static(
        "create",
        [](const std::string& path, const vlink::BagWriter::Config& cfg) {
          return cast_shared_message_loop(vlink::BagWriter::create(path, cfg), [](vlink::BagWriter& writer) {
            writer.wait_for_idle(vlink::Timer::kInfinite, false);
            writer.quit(true);
            writer.wait_for_quit(vlink::Timer::kInfinite, false);
            writer.close();
          });
        },
        "path"_a, "config"_a = vlink::BagWriter::Config())
      .def_static(
          "filter_get",
          [](const std::string& path) {
            return cast_shared_message_loop(vlink::BagWriter::filter_get(path), [](vlink::BagWriter& writer) {
              writer.wait_for_idle(vlink::Timer::kInfinite, false);
              writer.quit(true);
              writer.wait_for_quit(vlink::Timer::kInfinite, false);
              writer.close();
            });
          },
          "path"_a)
      .def_static("global_get", &vlink::BagWriter::global_get, nb::rv_policy::reference)
      .def(
          "push",
          [](vlink::BagWriter& self, const vlink::Frame& frame) {
            vlink::Frame owned = frame_from_python(frame);
            nb::gil_scoped_release release;
            return self.push(owned);
          },
          "frame"_a,
          "Record a frame. For direct asynchronous writes without a bag plugin, a non-negative result means "
          "the queue accepted the frame; a negative result means it was rejected without evicting an accepted write.")
      .def(
          "register_schema_callback",
          [](nb::object instance, nb::callable callback) {
            auto& self = nb::cast<vlink::BagWriter&>(instance);
            if VUNLIKELY (is_in_python_owner_callback(&self, python_bag_writer_schema_callback_kind())) {
              throw std::runtime_error(
                  "BagWriter callbacks cannot be replaced from that writer's active Python callback");
            }

            auto activity = std::make_shared<PythonCallbackActivity>();
            auto cb = std::make_shared<GilSafePyFunction>(std::move(callback));
            nb::gil_scoped_release release;
            self.register_schema_callback(
                [native = &self, activity, cb](const std::string& ser_type, vlink::SchemaType schema_type) {
                  vlink::SchemaData schema;
                  invoke_owned_python_callback(
                      native, activity, "vlink::BagWriter.register_schema_callback",
                      [&]() {
                        nb::object result = cb->fn(ser_type, schema_type);

                        if VLIKELY (!result.is_none()) {
                          schema = nb::cast<vlink::SchemaData>(result);
                        }
                      },
                      python_bag_writer_schema_callback_kind());
                  return schema;
                });
          },
          "callback"_a)
      .def(
          "push_schema",
          [](vlink::BagWriter& self, const vlink::SchemaData& schema_data) {
            // Keep a stable copy while the GIL is released.
            // NOLINTNEXTLINE(performance-unnecessary-copy-initialization)
            auto schema_copy = schema_data;
            nb::gil_scoped_release release;
            return self.push_schema(schema_copy);
          },
          "schema_data"_a)
      .def(
          "register_split_callback",
          [](nb::object instance, nb::callable callback, bool before) {
            auto& self = nb::cast<vlink::BagWriter&>(instance);
            if VUNLIKELY (is_in_python_owner_callback(&self, python_bag_writer_split_callback_kind())) {
              throw std::runtime_error(
                  "BagWriter callbacks cannot be replaced from that writer's active Python callback");
            }

            auto activity = std::make_shared<PythonCallbackActivity>();
            auto cb = std::make_shared<GilSafePyFunction>(std::move(callback));
            nb::gil_scoped_release release;
            self.register_split_callback(
                [native = &self, activity, cb](int idx, const std::string& file_name) {
                  invoke_owned_python_callback(
                      native, activity, "vlink::BagWriter.register_split_callback", [&]() { cb->fn(idx, file_name); },
                      python_bag_writer_split_callback_kind());
                },
                before);
          },
          "callback"_a, "before"_a = false)
      .def("is_dumping", &vlink::BagWriter::is_dumping)
      .def("is_split_mode", &vlink::BagWriter::is_split_mode)
      .def("get_split_index", &vlink::BagWriter::get_split_index)
      .def("set_url_loss", &vlink::BagWriter::set_url_loss, "url"_a, "loss"_a)
      .def("close",
           [](vlink::BagWriter& self) {
             nb::gil_scoped_release release;
             self.close();
           })
      .def("fail", &vlink::BagWriter::fail)
      .def("clear", &vlink::BagWriter::clear)
      .def(
          "wait_for_idle",
          [](vlink::BagWriter& self, int timeout_ms, bool check) {
            nb::gil_scoped_release release;
            return self.wait_for_idle(timeout_ms, check);
          },
          "timeout_ms"_a = -1, "check"_a = true)
      .def("get_task_count", &vlink::BagWriter::get_task_count)
      .def("__bool__", [](const vlink::BagWriter& self) { return static_cast<bool>(self); })
      .def(
          "__lshift__",
          [](vlink::BagWriter& self, const vlink::Frame& frame) -> vlink::BagWriter& {
            vlink::Frame owned = frame_from_python(frame);
            nb::gil_scoped_release release;
            self << owned;
            return self;
          },
          "frame"_a, nb::rv_policy::reference_internal)
      .def(
          "__lshift__",
          [](vlink::BagWriter& self, const vlink::SchemaData& schema_data) -> vlink::BagWriter& {
            // NOLINTNEXTLINE(performance-unnecessary-copy-initialization)
            auto schema_copy = schema_data;
            nb::gil_scoped_release release;
            self << schema_copy;
            return self;
          },
          "schema_data"_a, nb::rv_policy::reference_internal)
      .def("async_run",
           [](vlink::BagWriter& self) {
             nb::gil_scoped_release release;
             return self.async_run();
           })
      .def(
          "quit",
          [](vlink::BagWriter& self, bool force) {
            nb::gil_scoped_release release;
            return self.quit(force);
          },
          "force"_a = false)
      .def(
          "wait_for_quit",
          [](vlink::BagWriter& self, int timeout_ms) {
            nb::gil_scoped_release release;
            return self.wait_for_quit(timeout_ms);
          },
          "timeout_ms"_a = -1)
      .def("is_running", &vlink::BagWriter::is_running)
      .def("__repr__", [](const vlink::BagWriter& self) {
        return std::string("BagWriter(running=") + (self.is_running() ? "True" : "False") + ")";
      });

  [[maybe_unused]] auto bag_plugin_interface = nb::class_<vlink::BagPluginInterface>(
      m, "BagPluginInterface",
      "Opaque bag-plugin interface returned by Plugin.load_bag_plugin(); lifecycle hooks such as "
      "on_reset() and flush() are invoked by the C++ host");

  [[maybe_unused]] auto trigger_plugin_interface = nb::class_<vlink::TriggerPluginInterface>(
      m, "TriggerPluginInterface", "Opaque trigger-plugin interface returned by Plugin.load_trigger_plugin()");

  nb::class_<vlink::Plugin>(m, "Plugin", "Host-side shared-library plugin loader")
      .def(nb::init<>())
      .def(
          "load_bag_plugin",
          [](vlink::Plugin& self, const std::string& lib_name, const std::string& dir_name) {
            return self.load<vlink::BagPluginInterface>(lib_name, 2, 0, dir_name);
          },
          "lib_name"_a, "dir_name"_a = "",
          "Load a BagPluginInterface 2.0 implementation; return None when loading fails.")
      .def(
          "load_trigger_plugin",
          [](vlink::Plugin& self, const std::string& lib_name, const std::string& config,
             const std::string& dir_name) -> std::shared_ptr<vlink::TriggerPluginInterface> {
            auto plugin = self.load<vlink::TriggerPluginInterface>(lib_name, 2, 0, dir_name);

            if (!plugin || !plugin->init(config)) {
              return nullptr;
            }

            return plugin;
          },
          "lib_name"_a, "config"_a = "", "dir_name"_a = "",
          "Load a TriggerPluginInterface ABI 2.0 implementation and call init(config); return None when loading or "
          "init fails.");

  nb::class_<vlink::TriggerRecorder> tr(m, "TriggerRecorder", "Trigger-based event-data recorder");
  nb::enum_<vlink::TriggerRecorder::OverflowPolicy>(tr, "OverflowPolicy")
      .value("CoverOldest", vlink::TriggerRecorder::kCoverOldest)
      .value("DropNewest", vlink::TriggerRecorder::kDropNewest);
  nb::enum_<vlink::TriggerRecorder::FileType>(tr, "FileType")
      .value("Vdb", vlink::TriggerRecorder::kVdb)
      .value("Vcap", vlink::TriggerRecorder::kVcap);
  nb::class_<vlink::TriggerRecorder::UrlConfig>(tr, "UrlConfig")
      .def(nb::init<>())
      .def_rw("pre_ms", &vlink::TriggerRecorder::UrlConfig::pre_ms)
      .def_rw("post_ms", &vlink::TriggerRecorder::UrlConfig::post_ms)
      .def_rw("max_packet_size", &vlink::TriggerRecorder::UrlConfig::max_packet_size)
      .def_rw("max_size", &vlink::TriggerRecorder::UrlConfig::max_size)
      .def_rw("only_front", &vlink::TriggerRecorder::UrlConfig::only_front)
      .def_rw("only_back", &vlink::TriggerRecorder::UrlConfig::only_back);
  nb::class_<vlink::TriggerRecorder::Config>(tr, "Config")
      .def(nb::init<>())
      .def_rw("dump_dir", &vlink::TriggerRecorder::Config::dump_dir)
      .def_rw("file_type", &vlink::TriggerRecorder::Config::file_type)
      .def_rw("default_pre_ms", &vlink::TriggerRecorder::Config::default_pre_ms)
      .def_rw("default_post_ms", &vlink::TriggerRecorder::Config::default_post_ms)
      .def_rw("default_max_packet_size", &vlink::TriggerRecorder::Config::default_max_packet_size)
      .def_rw("default_max_size", &vlink::TriggerRecorder::Config::default_max_size)
      .def_rw("max_cache_size", &vlink::TriggerRecorder::Config::max_cache_size)
      .def_rw("retention_guard_ms", &vlink::TriggerRecorder::Config::retention_guard_ms)
      .def_rw("max_dump_file_count", &vlink::TriggerRecorder::Config::max_dump_file_count)
      .def_rw("enable_compress", &vlink::TriggerRecorder::Config::enable_compress)
      .def_rw("busy_skip_data", &vlink::TriggerRecorder::Config::busy_skip_data)
      .def_rw("destroy_on_offline", &vlink::TriggerRecorder::Config::destroy_on_offline)
      .def_rw("overflow", &vlink::TriggerRecorder::Config::overflow)
      .def_rw("sleep_interval", &vlink::TriggerRecorder::Config::sleep_interval)
      .def_rw("sleep_time_ms", &vlink::TriggerRecorder::Config::sleep_time_ms)
      .def_rw("discovery_filter", &vlink::TriggerRecorder::Config::discovery_filter)
      .def_rw("whitelist", &vlink::TriggerRecorder::Config::whitelist)
      .def_rw("blacklist", &vlink::TriggerRecorder::Config::blacklist)
      .def_rw("url_overrides", &vlink::TriggerRecorder::Config::url_overrides);
  nb::class_<vlink::TriggerRecorder::TriggerParams>(tr, "TriggerParams")
      .def(nb::init<>())
      .def_rw("reason", &vlink::TriggerRecorder::TriggerParams::reason)
      .def_rw("name_hint", &vlink::TriggerRecorder::TriggerParams::name_hint)
      .def_rw("out_file", &vlink::TriggerRecorder::TriggerParams::out_file)
      .def_rw("pre_ms", &vlink::TriggerRecorder::TriggerParams::pre_ms)
      .def_rw("post_ms", &vlink::TriggerRecorder::TriggerParams::post_ms)
      .def_rw("whitelist", &vlink::TriggerRecorder::TriggerParams::whitelist)
      .def_rw("blacklist", &vlink::TriggerRecorder::TriggerParams::blacklist)
      .def_rw("filter_str", &vlink::TriggerRecorder::TriggerParams::filter_str)
      .def_rw("black_mode", &vlink::TriggerRecorder::TriggerParams::black_mode);
  tr.def(nb::new_([](const vlink::TriggerRecorder::Config& config) {
           return new vlink::TriggerRecorder(config, [](const std::string& url, vlink::InitType type) {
             return vlink::TriggerRecorder::RawSub::create_shared(url, type);
           });
         }),
         "config"_a)
      .def("async_run",
           [](vlink::TriggerRecorder& self) {
             nb::gil_scoped_release release;
             const bool started = self.async_run();

             if (started) {
               self.invoke_task([]() {}).wait();
             }

             return started;
           })
      .def(
          "quit",
          [](vlink::TriggerRecorder& self, bool force) {
            nb::gil_scoped_release release;
            return self.quit(force);
          },
          "force"_a = false)
      .def(
          "wait_for_quit",
          [](vlink::TriggerRecorder& self, int timeout_ms) {
            nb::gil_scoped_release release;
            return self.wait_for_quit(timeout_ms);
          },
          "timeout_ms"_a = vlink::Timer::kInfinite)
      .def(
          "dump",
          [](vlink::TriggerRecorder& self, vlink::TriggerRecorder::TriggerParams params) {
            nb::gil_scoped_release release;
            return self.dump(params);
          },
          "params"_a = vlink::TriggerRecorder::TriggerParams())
      .def("bind_bag_interface", &vlink::TriggerRecorder::bind_bag_interface, "bag_interface"_a,
           "Bind a BagPluginInterface previously loaded by the host Plugin instance.")
      .def("clear_bag_interface", &vlink::TriggerRecorder::clear_bag_interface)
      .def("bind_trigger_interface", &vlink::TriggerRecorder::bind_trigger_interface, "trigger_interface"_a,
           "Bind a TriggerPluginInterface previously loaded by the host Plugin instance.")
      .def("clear_trigger_interface", &vlink::TriggerRecorder::clear_trigger_interface)
      .def("is_dumping", &vlink::TriggerRecorder::is_dumping)
      .def("is_running", &vlink::TriggerRecorder::is_running)
      .def("__repr__", [](const vlink::TriggerRecorder& self) {
        return std::string("TriggerRecorder(running=") + (self.is_running() ? "True" : "False") + ")";
      });

  nb::class_<vlink::BagReader> br(m, "BagReader", "Message playback", nb::is_weak_referenceable());
  nb::enum_<vlink::BagReader::Status>(br, "Status")
      .value("Stopped", vlink::BagReader::kStopped)
      .value("Paused", vlink::BagReader::kPaused)
      .value("Playing", vlink::BagReader::kPlaying);
  nb::class_<vlink::BagReader::Info::UrlMeta>(br, "UrlMeta")
      .def_ro("valid", &vlink::BagReader::Info::UrlMeta::valid)
      .def_ro("index", &vlink::BagReader::Info::UrlMeta::index)
      .def_ro("url", &vlink::BagReader::Info::UrlMeta::url)
      .def_ro("url_type", &vlink::BagReader::Info::UrlMeta::url_type)
      .def_ro("action_type", &vlink::BagReader::Info::UrlMeta::action_type)
      .def_ro("ser_type", &vlink::BagReader::Info::UrlMeta::ser_type)
      .def_ro("schema_type", &vlink::BagReader::Info::UrlMeta::schema_type)
      .def_ro("count", &vlink::BagReader::Info::UrlMeta::count)
      .def_ro("size", &vlink::BagReader::Info::UrlMeta::size)
      .def_ro("freq", &vlink::BagReader::Info::UrlMeta::freq)
      .def_ro("loss", &vlink::BagReader::Info::UrlMeta::loss)
      .def("__repr__", [](const vlink::BagReader::Info::UrlMeta& meta) {
        return "UrlMeta(url='" + meta.url + "', count=" + std::to_string(meta.count) + ")";
      });
  nb::class_<vlink::BagReader::Info>(br, "Info")
      .def_ro("file_name", &vlink::BagReader::Info::file_name)
      .def_ro("tag_name", &vlink::BagReader::Info::tag_name)
      .def_ro("version", &vlink::BagReader::Info::version)
      .def_ro("storage_type", &vlink::BagReader::Info::storage_type)
      .def_ro("compression_type", &vlink::BagReader::Info::compression_type)
      .def_ro("time_accuracy", &vlink::BagReader::Info::time_accuracy)
      .def_ro("process_name", &vlink::BagReader::Info::process_name)
      .def_ro("date_time", &vlink::BagReader::Info::date_time)
      .def_ro("has_completed", &vlink::BagReader::Info::has_completed)
      .def_ro("has_idx_elapsed", &vlink::BagReader::Info::has_idx_elapsed)
      .def_ro("has_idx_url", &vlink::BagReader::Info::has_idx_url)
      .def_ro("has_schema", &vlink::BagReader::Info::has_schema)
      .def_ro("timezone", &vlink::BagReader::Info::timezone)
      .def_ro("start_timestamp", &vlink::BagReader::Info::start_timestamp)
      .def_ro("blank_duration", &vlink::BagReader::Info::blank_duration)
      .def_ro("total_duration", &vlink::BagReader::Info::total_duration)
      .def_ro("file_size", &vlink::BagReader::Info::file_size)
      .def_ro("total_raw_size", &vlink::BagReader::Info::total_raw_size)
      .def_ro("message_count", &vlink::BagReader::Info::message_count)
      .def_ro("split_count", &vlink::BagReader::Info::split_count)
      .def_ro("split_by_size", &vlink::BagReader::Info::split_by_size)
      .def_ro("split_by_time", &vlink::BagReader::Info::split_by_time)
      .def_ro("url_metas", &vlink::BagReader::Info::url_metas)
      .def("__repr__", [](const vlink::BagReader::Info& info) {
        return "BagInfo(file='" + info.file_name + "', messages=" + std::to_string(info.message_count) +
               ", duration=" + std::to_string(info.total_duration) + "ms)";
      });
  nb::class_<vlink::BagReader::Config>(br, "Config")
      .def(nb::init<>())
      .def_rw("begin_time", &vlink::BagReader::Config::begin_time)
      .def_rw("end_time", &vlink::BagReader::Config::end_time)
      .def_rw("times", &vlink::BagReader::Config::times)
      .def_rw("rate", &vlink::BagReader::Config::rate)
      .def_rw("skip_blank", &vlink::BagReader::Config::skip_blank)
      .def_rw("force_delay", &vlink::BagReader::Config::force_delay)
      .def_rw("auto_pause", &vlink::BagReader::Config::auto_pause)
      .def_rw("auto_quit", &vlink::BagReader::Config::auto_quit)
      .def_rw("filter_urls", &vlink::BagReader::Config::filter_urls);
  br.attr("INFINITE") = vlink::BagReader::kInfinite;
  br.def_static(
        "create",
        [](const std::string& path, bool read_only, bool try_to_fix) {
          return cast_shared_message_loop(vlink::BagReader::create(path, read_only, try_to_fix),
                                          [](vlink::BagReader& reader) {
                                            reader.stop();
                                            reader.quit(true);
                                            reader.wait_for_quit(vlink::Timer::kInfinite, false);
                                          });
        },
        "path"_a, "read_only"_a = true, "try_to_fix"_a = false)
      .def(
          "register_output_callback",
          [](nb::object instance, nb::callable callback) {
            auto& self = nb::cast<vlink::BagReader&>(instance);
            if VUNLIKELY (is_in_python_owner_callback(&self, python_bag_reader_output_callback_kind())) {
              throw std::runtime_error(
                  "BagReader output callback cannot be replaced from that reader's active Python callback");
            }

            auto activity = std::make_shared<PythonCallbackActivity>();
            auto cb = std::make_shared<GilSafePyFunction>(std::move(callback));
            nb::gil_scoped_release release;
            self.register_output_callback([native = &self, activity, cb](const vlink::Frame& frame) {
              invoke_owned_python_callback(
                  native, activity, "vlink::BagReader.register_output_callback",
                  [&]() { cb->fn(nb::cast(vlink::Frame(frame))); }, python_bag_reader_output_callback_kind());
            });
          },
          "callback"_a)
      .def(
          "register_status_callback",
          [](nb::object instance, nb::callable callback) {
            auto& self = nb::cast<vlink::BagReader&>(instance);
            auto activity = std::make_shared<PythonCallbackActivity>();
            auto cb = std::make_shared<GilSafePyFunction>(std::move(callback));
            self.register_status_callback([native = &self, activity, cb](vlink::BagReader::Status status) {
              invoke_owned_python_callback(native, activity, "vlink::BagReader.register_status_callback",
                                           [&]() { cb->fn(status); });
            });
          },
          "callback"_a)
      .def(
          "register_ready_callback",
          [](nb::object instance, nb::callable callback) {
            auto& self = nb::cast<vlink::BagReader&>(instance);
            auto activity = std::make_shared<PythonCallbackActivity>();
            auto cb = std::make_shared<GilSafePyFunction>(std::move(callback));
            self.register_ready_callback([native = &self, activity, cb]() {
              invoke_owned_python_callback(native, activity, "vlink::BagReader.register_ready_callback",
                                           [&]() { cb->fn(); });
            });
          },
          "callback"_a)
      .def(
          "register_finish_callback",
          [](nb::object instance, nb::callable callback) {
            auto& self = nb::cast<vlink::BagReader&>(instance);
            auto activity = std::make_shared<PythonCallbackActivity>();
            auto cb = std::make_shared<GilSafePyFunction>(std::move(callback));
            self.register_finish_callback([native = &self, activity, cb](bool interrupted) {
              invoke_owned_python_callback(native, activity, "vlink::BagReader.register_finish_callback",
                                           [&]() { cb->fn(interrupted); });
            });
          },
          "callback"_a)
      .def("play", &vlink::BagReader::play, "config"_a = vlink::BagReader::Config())
      .def("stop", &vlink::BagReader::stop)
      .def("pause", &vlink::BagReader::pause)
      .def("resume", &vlink::BagReader::resume)
      .def("pause_to_next", &vlink::BagReader::pause_to_next)
      .def(
          "jump",
          [](vlink::BagReader& self, int64_t begin_time, double rate, int times, bool force_to_play) {
            nb::gil_scoped_release release;
            self.jump(begin_time, rate, times, force_to_play);
          },
          "begin_time"_a, "rate"_a = 1.0, "times"_a = 1, "force_to_play"_a = false)
      .def("check",
           [](vlink::BagReader& self) {
             auto future = self.check();
             nb::gil_scoped_release release;
             return future.get();
           })
      .def("reindex",
           [](vlink::BagReader& self) {
             auto future = self.reindex();
             nb::gil_scoped_release release;
             return future.get();
           })
      .def(
          "fix",
          [](vlink::BagReader& self, bool rebuild) {
            auto future = self.fix(rebuild);
            nb::gil_scoped_release release;
            return future.get();
          },
          "rebuild"_a = false)
      .def("tag", &vlink::BagReader::tag, "tag_name"_a)
      .def("get_timestamp", &vlink::BagReader::get_timestamp)
      .def("get_real_timestamp", &vlink::BagReader::get_real_timestamp)
      .def("get_status", &vlink::BagReader::get_status)
      .def("get_info", &vlink::BagReader::get_info, nb::rv_policy::reference_internal)
      .def("detect_schema", &vlink::BagReader::detect_schema)
      .def("get_ser_type", &vlink::BagReader::get_ser_type, "url"_a)
      .def("get_schema_type", &vlink::BagReader::get_schema_type, "url"_a)
      .def("is_split_mode", &vlink::BagReader::is_split_mode)
      .def("get_split_index", &vlink::BagReader::get_split_index)
      .def("is_jumping", &vlink::BagReader::is_jumping)
      .def(
          "open_cursor",
          [](vlink::BagReader& self, const vlink::BagReader::Config& config) {
            nb::gil_scoped_release release;
            return self.open_cursor(config);
          },
          "config"_a = vlink::BagReader::Config())
      .def("read_next",
           [](vlink::BagReader& self) -> nb::object {
             vlink::Frame frame;
             bool ok = false;

             {
               nb::gil_scoped_release release;
               ok = self.read_next(frame);
             }

             if (!ok) {
               return nb::none();
             }

             frame.data.deep_copy_self();
             return nb::cast(std::move(frame));
           })
      .def("eof", &vlink::BagReader::eof)
      .def("fail", &vlink::BagReader::fail)
      .def("__bool__", [](const vlink::BagReader& self) { return static_cast<bool>(self); })
      .def(
          "__iter__", [](vlink::BagReader& self) -> vlink::BagReader& { return self; },
          nb::rv_policy::reference_internal)
      .def("__next__",
           [](vlink::BagReader& self) -> nb::object {
             vlink::Frame frame;
             bool ok = false;

             {
               nb::gil_scoped_release release;
               ok = self.read_next(frame);
             }

             if (!ok) {
               throw nb::stop_iteration();
             }

             frame.data.deep_copy_self();
             return nb::cast(std::move(frame));
           })
      .def("async_run",
           [](vlink::BagReader& self) {
             nb::gil_scoped_release release;
             return self.async_run();
           })
      .def(
          "quit",
          [](vlink::BagReader& self, bool force) {
            nb::gil_scoped_release release;
            return self.quit(force);
          },
          "force"_a = false)
      .def(
          "wait_for_quit",
          [](vlink::BagReader& self, int timeout_ms) {
            nb::gil_scoped_release release;
            return self.wait_for_quit(timeout_ms);
          },
          "timeout_ms"_a = -1)
      .def("is_running", &vlink::BagReader::is_running)
      .def("__repr__", [](const vlink::BagReader& self) {
        const auto& info = self.get_info();
        return "BagReader(file='" + info.file_name + "', messages=" + std::to_string(info.message_count) + ")";
      });

  m.attr("VERSION") = VLINK_VERSION;
  m.attr("VERSION_MAJOR") = VLINK_VERSION_MAJOR;
  m.attr("VERSION_MINOR") = VLINK_VERSION_MINOR;
  m.attr("VERSION_PATCH") = VLINK_VERSION_PATCH;
}
