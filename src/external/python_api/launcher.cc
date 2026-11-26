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

#include <nanobind/stl/optional.h>
#include <nanobind/stl/pair.h>
#include <nanobind/stl/shared_ptr.h>
#include <nanobind/stl/string.h>
#include <nanobind/stl/unordered_map.h>
#include <nanobind/stl/vector.h>
#include <vlink/base/timer.h>
#include <vlink/external/launcher_api.h>
#include <vlink/external/launcher_server.h>

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include "callbacks.h"

namespace vlink::python {

using nb::literals::operator""_a;

static void close_launcher_api(LauncherAPI& api) {
  api.register_connect_callback({});
  api.register_snapshot_callback({});
  api.register_event_callback({});
  api.register_log_callback({});
  api.quit(true);
  api.wait_for_quit(Timer::kInfinite, false);
  api.register_connect_callback({});
  api.register_snapshot_callback({});
  api.register_event_callback({});
  api.register_log_callback({});
}

static void close_launcher_server(LauncherServer& server) {
  server.register_event_callback({});
  server.register_log_callback({});
  server.stop();
  server.register_event_callback({});
  server.register_log_callback({});
}

template <typename OwnerT>
static void check_launcher_callback(const OwnerT& owner) {
  if VUNLIKELY (is_in_python_owner_callback(&owner) || owner.is_in_same_thread()) {
    throw std::runtime_error(std::is_same_v<OwnerT, LauncherAPI>
                                 ? "Cannot replace callbacks or wait for LauncherAPI shutdown inside its callback"
                                 : "Cannot replace callbacks or wait for LauncherServer shutdown inside its callback");
  }
}

template <typename OwnerT, typename... ArgsT>
static void bind_launcher_callback(nb::class_<OwnerT, MessageLoop>& cls, const char* name,
                                   void (OwnerT::*method)(MoveFunction<void(ArgsT...)>&&)) {
  cls.def(
      name,
      [method, name](OwnerT& self, nb::object callback) {
        check_launcher_callback(self);

        MoveFunction<void(ArgsT...)> wrapped;

        if (!callback.is_none()) {
          GilSafePyFunction cb(nb::cast<nb::callable>(callback));
          auto activity = std::make_shared<PythonCallbackActivity>();

          wrapped = [native = &self, activity = std::move(activity), cb = std::move(cb), name](ArgsT... args) {
            invoke_owned_python_callback(native, activity, name, [&]() { cb.fn(args...); });
          };
        }

        nb::gil_scoped_release release;
        (self.*method)(std::move(wrapped));
      },
      "callback"_a.none(), "Set the callback, or None to unregister; call outside this object's callbacks");
}

static LauncherParamValue python_to_param_value(nb::handle value) {
  if (nb::isinstance<nb::bool_>(value)) {
    return nb::cast<bool>(value);
  }

  if (nb::isinstance<nb::int_>(value)) {
    return nb::cast<int64_t>(value);
  }

  if (nb::isinstance<nb::float_>(value)) {
    const auto d = nb::cast<double>(value);

    if (!std::isfinite(d)) {
      throw std::invalid_argument("Non-finite float (NaN/Inf) is not supported for LauncherParamValue");
    }

    return d;
  }

  if (nb::isinstance<nb::str>(value)) {
    return nb::cast<std::string>(value);
  }

  if (nb::isinstance<nb::sequence>(value) && !nb::isinstance<nb::bytes>(value) &&
      !nb::isinstance<nb::bytearray>(value)) {
    auto seq = nb::borrow<nb::sequence>(value);
    const size_t n = nb::len(seq);

    if (n == 0U) {
      return std::vector<std::string>{};
    }

    std::vector<nb::object> items;
    items.reserve(n);
    bool all_bool = true;
    bool all_int = true;
    bool all_num = true;
    bool all_str = true;

    for (size_t i = 0U; i < n; ++i) {
      nb::object item = seq[i];

      if (!nb::isinstance<nb::bool_>(item)) {
        all_bool = false;
      }

      if (nb::isinstance<nb::bool_>(item) || !nb::isinstance<nb::int_>(item)) {
        all_int = false;
      }

      if (nb::isinstance<nb::bool_>(item) || (!nb::isinstance<nb::int_>(item) && !nb::isinstance<nb::float_>(item))) {
        all_num = false;
      }

      if (!nb::isinstance<nb::str>(item)) {
        all_str = false;
      }

      items.push_back(std::move(item));
    }

    auto cast_seq = [&items, n](auto tag) {
      using ElemT = decltype(tag);
      std::vector<ElemT> out;
      out.reserve(n);

      for (const auto& item : items) {
        out.push_back(nb::cast<ElemT>(item));
      }

      return out;
    };

    if (all_bool) {
      return cast_seq(bool{});
    }

    if (all_int) {
      return cast_seq(int64_t{});
    }

    if (all_num) {
      static constexpr int64_t kMaxExactDoubleInt = (int64_t{1} << 53);
      std::vector<double> out;
      out.reserve(n);

      for (const auto& item : items) {
        if (nb::isinstance<nb::int_>(item)) {
          const auto iv = nb::cast<int64_t>(item);

          if (iv > kMaxExactDoubleInt || iv < -kMaxExactDoubleInt) {
            throw std::invalid_argument("Integer in mixed float list exceeds exact IEEE-754 double precision (2^53)");
          }

          out.push_back(static_cast<double>(iv));
        } else {
          const auto d = nb::cast<double>(item);

          if (!std::isfinite(d)) {
            throw std::invalid_argument("Non-finite float (NaN/Inf) is not supported in LauncherParamValue list");
          }

          out.push_back(d);
        }
      }

      return out;
    }

    if (all_str) {
      return cast_seq(std::string{});
    }

    throw std::invalid_argument("Unsupported mixed or non-primitive list element type for LauncherParamValue");
  }

  throw std::invalid_argument(
      "Unsupported value type for LauncherParamValue (expected bool, int, float, str, or homogeneous list)");
}

static nb::object param_value_to_python(const LauncherParamValue& value) {
  return std::visit(
      [](const auto& v) -> nb::object {
        using T = std::decay_t<decltype(v)>;

        if constexpr (std::is_same_v<T, std::vector<bool>>) {
          nb::list out;

          for (const bool item : v) {
            out.append(nb::bool_(item));
          }

          return out;
        } else {
          return nb::cast(v);
        }
      },
      value);
}

void bind_launcher(nb::module_& m) {
  nb::module_ lm = m.def_submodule("launcher", "VLink Launcher orchestration & control-plane data model and helpers");

  auto unit_type = nb::enum_<launcher::UnitType>(lm, "UnitType")
                       .value("Component", launcher::UnitType::kComponent)
                       .value("Process", launcher::UnitType::kProcess)
                       .value("Container", launcher::UnitType::kContainer)
                       .value("Proxy", launcher::UnitType::kProxy)
                       .value("Task", launcher::UnitType::kTask);

  auto unit_state = nb::enum_<launcher::UnitState>(lm, "UnitState")
                        .value("Pending", launcher::UnitState::kPending)
                        .value("Starting", launcher::UnitState::kStarting)
                        .value("Running", launcher::UnitState::kRunning)
                        .value("Ready", launcher::UnitState::kReady)
                        .value("Stopping", launcher::UnitState::kStopping)
                        .value("Stopped", launcher::UnitState::kStopped)
                        .value("Restarting", launcher::UnitState::kRestarting)
                        .value("Failed", launcher::UnitState::kFailed)
                        .value("Skipped", launcher::UnitState::kSkipped)
                        .value("Inactive", launcher::UnitState::kInactive);

  auto error_code = nb::enum_<launcher::ErrorCode>(lm, "ErrorCode")
                        .value("Ok", launcher::ErrorCode::kOk)
                        .value("InvalidArgument", launcher::ErrorCode::kInvalidArgument)
                        .value("NotFound", launcher::ErrorCode::kNotFound)
                        .value("AlreadyExists", launcher::ErrorCode::kAlreadyExists)
                        .value("AlreadyRunning", launcher::ErrorCode::kAlreadyRunning)
                        .value("NotRunning", launcher::ErrorCode::kNotRunning)
                        .value("DependencyFailed", launcher::ErrorCode::kDependencyFailed)
                        .value("Unsupported", launcher::ErrorCode::kUnsupported)
                        .value("Timeout", launcher::ErrorCode::kTimeout)
                        .value("Disconnected", launcher::ErrorCode::kDisconnected)
                        .value("VersionMismatch", launcher::ErrorCode::kVersionMismatch)
                        .value("Unauthorized", launcher::ErrorCode::kUnauthorized)
                        .value("InternalError", launcher::ErrorCode::kInternalError);

  auto param_type = nb::enum_<launcher::ParamType>(lm, "ParamType")
                        .value("Bool", launcher::ParamType::kBool)
                        .value("Int", launcher::ParamType::kInt)
                        .value("Double", launcher::ParamType::kDouble)
                        .value("String", launcher::ParamType::kString)
                        .value("BoolList", launcher::ParamType::kBoolList)
                        .value("IntList", launcher::ParamType::kIntList)
                        .value("DoubleList", launcher::ParamType::kDoubleList)
                        .value("StringList", launcher::ParamType::kStringList);

  auto param_entry = nb::class_<launcher::ParamEntry>(lm, "ParamEntry")
                         .def(nb::init<>())
                         .def_rw("key", &launcher::ParamEntry::key)
                         .def_rw("type", &launcher::ParamEntry::type)
                         .def_rw("value", &launcher::ParamEntry::value);

  auto unit_status = nb::class_<launcher::UnitStatus>(lm, "UnitStatus")
                         .def(nb::init<>())
                         .def_rw("name", &launcher::UnitStatus::name)
                         .def_rw("ns", &launcher::UnitStatus::ns)
                         .def_rw("group", &launcher::UnitStatus::group)
                         .def_rw("task_group", &launcher::UnitStatus::task_group)
                         .def_rw("slice", &launcher::UnitStatus::slice)
                         .def_rw("cgroup_path", &launcher::UnitStatus::cgroup_path)
                         .def_rw("container", &launcher::UnitStatus::container)
                         .def_rw("type", &launcher::UnitStatus::type)
                         .def_rw("state", &launcher::UnitStatus::state)
                         .def_rw("stage", &launcher::UnitStatus::stage)
                         .def_rw("pid", &launcher::UnitStatus::pid)
                         .def_rw("exit_code", &launcher::UnitStatus::exit_code)
                         .def_rw("restart_count", &launcher::UnitStatus::restart_count)
                         .def_rw("liveness_failures", &launcher::UnitStatus::liveness_failures)
                         .def_rw("thread_count", &launcher::UnitStatus::thread_count)
                         .def_rw("worker_index", &launcher::UnitStatus::worker_index)
                         .def_rw("start_time_ms", &launcher::UnitStatus::start_time_ms)
                         .def_rw("uptime_ms", &launcher::UnitStatus::uptime_ms)
                         .def_rw("queued_offset_ms", &launcher::UnitStatus::queued_offset_ms)
                         .def_rw("started_offset_ms", &launcher::UnitStatus::started_offset_ms)
                         .def_rw("ready_offset_ms", &launcher::UnitStatus::ready_offset_ms)
                         .def_rw("startup_duration_ms", &launcher::UnitStatus::startup_duration_ms)
                         .def_rw("generation", &launcher::UnitStatus::generation)
                         .def_rw("memory_rss_kb", &launcher::UnitStatus::memory_rss_kb)
                         .def_rw("cgroup_memory_kb", &launcher::UnitStatus::cgroup_memory_kb)
                         .def_rw("cpu_usage", &launcher::UnitStatus::cpu_usage)
                         .def_rw("healthy", &launcher::UnitStatus::healthy)
                         .def_rw("required", &launcher::UnitStatus::required)
                         .def_rw("failed_reason", &launcher::UnitStatus::failed_reason)
                         .def_rw("blocker_unit", &launcher::UnitStatus::blocker_unit)
                         .def_rw("detail", &launcher::UnitStatus::detail)
                         .def_rw("dependencies", &launcher::UnitStatus::dependencies)
                         .def_rw("wants", &launcher::UnitStatus::wants)
                         .def_rw("binds_to", &launcher::UnitStatus::binds_to)
                         .def_rw("conflicts", &launcher::UnitStatus::conflicts);

  auto system_snapshot = nb::class_<launcher::SystemSnapshot>(lm, "SystemSnapshot")
                             .def(nb::init<>())
                             .def_rw("session_id", &launcher::SystemSnapshot::session_id)
                             .def_rw("version", &launcher::SystemSnapshot::version)
                             .def_rw("host", &launcher::SystemSnapshot::host)
                             .def_rw("server_pid", &launcher::SystemSnapshot::server_pid)
                             .def_rw("timestamp_ms", &launcher::SystemSnapshot::timestamp_ms)
                             .def_rw("uptime_ms", &launcher::SystemSnapshot::uptime_ms)
                             .def_rw("boot_duration_ms", &launcher::SystemSnapshot::boot_duration_ms)
                             .def_rw("cpu_usage", &launcher::SystemSnapshot::cpu_usage)
                             .def_rw("memory_usage", &launcher::SystemSnapshot::memory_usage)
                             .def_rw("startup_mode", &launcher::SystemSnapshot::startup_mode)
                             .def_rw("max_parallel", &launcher::SystemSnapshot::max_parallel)
                             .def_rw("log_dir", &launcher::SystemSnapshot::log_dir)
                             .def_rw("config_file", &launcher::SystemSnapshot::config_file)
                             .def_prop_rw(
                                 "units", [](const launcher::SystemSnapshot& self) { return self.units; },
                                 [](launcher::SystemSnapshot& self, std::vector<launcher::UnitStatus> units) {
                                   self.units = std::move(units);
                                 });

  auto event_record = nb::class_<launcher::EventRecord>(lm, "EventRecord")
                          .def(nb::init<>())
                          .def_rw("timestamp_ms", &launcher::EventRecord::timestamp_ms)
                          .def_rw("unit_name", &launcher::EventRecord::unit_name)
                          .def_rw("unit_type", &launcher::EventRecord::unit_type)
                          .def_rw("old_state", &launcher::EventRecord::old_state)
                          .def_rw("new_state", &launcher::EventRecord::new_state)
                          .def_rw("event_name", &launcher::EventRecord::event_name)
                          .def_rw("message", &launcher::EventRecord::message);

  auto log_record = nb::class_<launcher::LogRecord>(lm, "LogRecord")
                        .def(nb::init<>())
                        .def_rw("seq", &launcher::LogRecord::seq)
                        .def_rw("timestamp_ms", &launcher::LogRecord::timestamp_ms)
                        .def_rw("unit_name", &launcher::LogRecord::unit_name)
                        .def_rw("level", &launcher::LogRecord::level)
                        .def_rw("is_stderr", &launcher::LogRecord::is_stderr)
                        .def_rw("text", &launcher::LogRecord::text);

  auto log_query = nb::class_<launcher::LogQuery>(lm, "LogQuery")
                       .def(nb::init<>())
                       .def_rw("unit_filter", &launcher::LogQuery::unit_filter)
                       .def_rw("grep", &launcher::LogQuery::grep)
                       .def_rw("after_seq", &launcher::LogQuery::after_seq)
                       .def_rw("min_level", &launcher::LogQuery::min_level)
                       .def_rw("stderr_only", &launcher::LogQuery::stderr_only)
                       .def_rw("limit", &launcher::LogQuery::limit);

  auto load_comp_req = nb::class_<launcher::LoadComponentRequest>(lm, "LoadComponentRequest")
                           .def(nb::init<>())
                           .def_rw("name", &launcher::LoadComponentRequest::name)
                           .def_rw("ns", &launcher::LoadComponentRequest::ns)
                           .def_rw("library", &launcher::LoadComponentRequest::library)
                           .def_rw("class_name", &launcher::LoadComponentRequest::class_name)
                           .def_rw("scheduler_group", &launcher::LoadComponentRequest::scheduler_group)
                           .def_rw("compute_pool", &launcher::LoadComponentRequest::compute_pool)
                           .def_rw("container", &launcher::LoadComponentRequest::container)
                           .def_rw("worker_index", &launcher::LoadComponentRequest::worker_index)
                           .def_rw("task_priority", &launcher::LoadComponentRequest::task_priority)
                           .def_rw("legacy_runnable", &launcher::LoadComponentRequest::legacy_runnable)
                           .def_rw("auto_start", &launcher::LoadComponentRequest::auto_start)
                           .def_prop_rw(
                               "params", [](const launcher::LoadComponentRequest& self) { return self.params; },
                               [](launcher::LoadComponentRequest& self, std::vector<launcher::ParamEntry> params) {
                                 self.params = std::move(params);
                               })
                           .def_rw("remaps", &launcher::LoadComponentRequest::remaps)
                           .def_rw("urls", &launcher::LoadComponentRequest::urls)
                           .def_rw("inputs", &launcher::LoadComponentRequest::inputs)
                           .def_rw("outputs", &launcher::LoadComponentRequest::outputs);

  auto cmd_result =
      nb::class_<launcher::CommandResult>(lm, "CommandResult")
          .def(nb::init<>())
          .def_rw("code", &launcher::CommandResult::code)
          .def_rw("message", &launcher::CommandResult::message)
          .def_prop_rw(
              "params", [](const launcher::CommandResult& self) { return self.params; },
              [](launcher::CommandResult& self, std::vector<launcher::ParamEntry> params) {
                self.params = std::move(params);
              })
          .def_prop_rw(
              "logs", [](const launcher::CommandResult& self) { return self.logs; },
              [](launcher::CommandResult& self, std::vector<launcher::LogRecord> logs) { self.logs = std::move(logs); })
          .def_prop_rw(
              "snapshot", [](const launcher::CommandResult& self) { return self.snapshot; },
              [](launcher::CommandResult& self, launcher::SystemSnapshot snapshot) {
                self.snapshot = std::move(snapshot);
              })
          .def("ok", &launcher::CommandResult::ok)
          .def("__bool__", &launcher::CommandResult::ok);

  lm.def("unit_type_to_string", &launcher::unit_type_to_string, "type"_a)
      .def("unit_state_to_string", &launcher::unit_state_to_string, "state"_a)
      .def("error_code_to_string", &launcher::error_code_to_string, "code"_a)
      .def(
          "encode_param_entry",
          [](const std::string& key, nb::handle value) {
            return launcher::encode_param_entry(key, python_to_param_value(value));
          },
          "key"_a, "value"_a)
      .def(
          "decode_param_entry",
          [](const launcher::ParamEntry& entry) -> nb::object {
            LauncherParamValue out;

            if (!launcher::decode_param_entry(entry, out)) {
              return nb::none();
            }

            return param_value_to_python(out);
          },
          "entry"_a, "Decodes a wire ParamEntry to a native Python value, or None if malformed");

  nb::class_<LauncherAPI, MessageLoop> api(m, "LauncherAPI", "Launcher control-plane client SDK",
                                           nb::is_weak_referenceable());
  api.attr("UnitType") = unit_type;
  api.attr("UnitState") = unit_state;
  api.attr("ErrorCode") = error_code;
  api.attr("ParamType") = param_type;
  api.attr("ParamEntry") = param_entry;
  api.attr("UnitStatus") = unit_status;
  api.attr("SystemSnapshot") = system_snapshot;
  api.attr("EventRecord") = event_record;
  api.attr("LogRecord") = log_record;
  api.attr("LogQuery") = log_query;
  api.attr("LoadComponentRequest") = load_comp_req;
  api.attr("CommandResult") = cmd_result;

  nb::class_<LauncherAPI::Config>(api, "Config")
      .def(nb::init<>())
      .def_rw("scheme", &LauncherAPI::Config::scheme)
      .def_rw("namespace_prefix", &LauncherAPI::Config::namespace_prefix)
      .def_rw("domain_id", &LauncherAPI::Config::domain_id)
      .def_rw("security_key", &LauncherAPI::Config::security_key)
      .def_rw("bind_ip", &LauncherAPI::Config::bind_ip)
      .def_rw("native_mode", &LauncherAPI::Config::native_mode)
      .def_rw("subscribe_events", &LauncherAPI::Config::subscribe_events)
      .def_rw("subscribe_logs", &LauncherAPI::Config::subscribe_logs)
      .def_rw("default_timeout_ms", &LauncherAPI::Config::default_timeout_ms);

  api.def(nb::new_([](const LauncherAPI::Config& config) {
            // NOLINTNEXTLINE(performance-unnecessary-copy-initialization)
            const auto snapshot = config;
            std::shared_ptr<LauncherAPI> owner;
            {
              nb::gil_scoped_release release;
              owner = std::shared_ptr<LauncherAPI>(new LauncherAPI(snapshot), [](LauncherAPI* ptr) {
                std::unique_ptr<LauncherAPI> native(ptr);
                destroy_python_callback_owner(native, current_python_callback_activity());
              });
            }
            auto instance = nb::cast(owner);
            ensure_python_pre_destroy_hook(instance, owner.get(), [](LauncherAPI& client) {
              if VUNLIKELY (current_python_callback_activity()) {
                return;
              }

              close_launcher_api(client);
            });
            return instance;
          }),
          "config"_a = LauncherAPI::Config{})
      .def("close",
           [](LauncherAPI& self) {
             check_launcher_callback(self);
             nb::gil_scoped_release release;
             close_launcher_api(self);
           })
      .def("__enter__", [](nb::object self) -> nb::object { return self; })
      .def("__exit__",
           [](LauncherAPI& self, nb::args, nb::kwargs) {
             check_launcher_callback(self);
             nb::gil_scoped_release release;
             close_launcher_api(self);
           })
      .def(
          "wait_for_quit",
          [](LauncherAPI& self, int timeout_ms, bool check) {
            check_launcher_callback(self);
            nb::gil_scoped_release release;
            return self.wait_for_quit(timeout_ms, check);
          },
          "timeout_ms"_a = -1, "check"_a = true)
      .def("get_current_config", &LauncherAPI::get_current_config, nb::rv_policy::copy)
      .def("is_connected", &LauncherAPI::is_connected, nb::call_guard<nb::gil_scoped_release>())
      .def("wait_for_connected", &LauncherAPI::wait_for_connected, "timeout_ms"_a = 0,
           nb::call_guard<nb::gil_scoped_release>())
      .def("get_cached_snapshot", &LauncherAPI::get_cached_snapshot, nb::call_guard<nb::gil_scoped_release>())
      .def("get_status", &LauncherAPI::get_status, "timeout_ms"_a = 0, nb::call_guard<nb::gil_scoped_release>())
      .def("start_unit", &LauncherAPI::start_unit, "target"_a, "timeout_ms"_a = 0,
           nb::call_guard<nb::gil_scoped_release>())
      .def("stop_unit", &LauncherAPI::stop_unit, "target"_a, "timeout_ms"_a = 0,
           nb::call_guard<nb::gil_scoped_release>())
      .def("restart_unit", &LauncherAPI::restart_unit, "target"_a, "timeout_ms"_a = 0,
           nb::call_guard<nb::gil_scoped_release>())
      .def("reload_unit", &LauncherAPI::reload_unit, "target"_a, "timeout_ms"_a = 0,
           nb::call_guard<nb::gil_scoped_release>())
      .def("activate_unit", &LauncherAPI::activate_unit, "target"_a, "timeout_ms"_a = 0,
           nb::call_guard<nb::gil_scoped_release>())
      .def("deactivate_unit", &LauncherAPI::deactivate_unit, "target"_a, "timeout_ms"_a = 0,
           nb::call_guard<nb::gil_scoped_release>())
      .def("reset_failed", &LauncherAPI::reset_failed, "target"_a = "", "timeout_ms"_a = 0,
           nb::call_guard<nb::gil_scoped_release>())
      .def("isolate_group", &LauncherAPI::isolate_group, "group_name"_a, "timeout_ms"_a = 0,
           nb::call_guard<nb::gil_scoped_release>())
      .def("get_unit_config", &LauncherAPI::get_unit_config, "unit_name"_a, "timeout_ms"_a = 0,
           nb::call_guard<nb::gil_scoped_release>())
      .def(
          "query_logs",
          [](LauncherAPI& self, const launcher::LogQuery& query, uint32_t timeout_ms) {
            // NOLINTNEXTLINE(performance-unnecessary-copy-initialization)
            const auto snapshot = query;
            nb::gil_scoped_release release;
            return self.query_logs(snapshot, timeout_ms);
          },
          "query"_a = launcher::LogQuery{}, "timeout_ms"_a = 0)
      .def(
          "load_component",
          [](LauncherAPI& self, const launcher::LoadComponentRequest& request, uint32_t timeout_ms) {
            // NOLINTNEXTLINE(performance-unnecessary-copy-initialization)
            const auto snapshot = request;
            nb::gil_scoped_release release;
            return self.load_component(snapshot, timeout_ms);
          },
          "request"_a, "timeout_ms"_a = 0)
      .def("unload_component", &LauncherAPI::unload_component, "unit_name"_a, "timeout_ms"_a = 0,
           nb::call_guard<nb::gil_scoped_release>())
      .def("get_params", &LauncherAPI::get_params, "unit_name"_a = "", "timeout_ms"_a = 0,
           nb::call_guard<nb::gil_scoped_release>())
      .def(
          "set_param",
          [](LauncherAPI& self, const std::string& unit_name, const launcher::ParamEntry& entry, uint32_t timeout_ms) {
            // NOLINTNEXTLINE(performance-unnecessary-copy-initialization)
            const auto snapshot = entry;
            nb::gil_scoped_release release;
            return self.set_param(unit_name, snapshot, timeout_ms);
          },
          "unit_name"_a, "entry"_a, "timeout_ms"_a = 0)
      .def("set_record", &LauncherAPI::set_record, "target"_a, "enable"_a, "path"_a = "", "timeout_ms"_a = 0,
           nb::call_guard<nb::gil_scoped_release>())
      .def("resume_startup", &LauncherAPI::resume_startup, "timeout_ms"_a = 0, nb::call_guard<nb::gil_scoped_release>())
      .def("shutdown_server", &LauncherAPI::shutdown_server, "timeout_ms"_a = 0,
           nb::call_guard<nb::gil_scoped_release>());

  bind_launcher_callback(api, "register_connect_callback", &LauncherAPI::register_connect_callback);
  bind_launcher_callback(api, "register_snapshot_callback", &LauncherAPI::register_snapshot_callback);
  bind_launcher_callback(api, "register_event_callback", &LauncherAPI::register_event_callback);
  bind_launcher_callback(api, "register_log_callback", &LauncherAPI::register_log_callback);

  nb::class_<LauncherServer, MessageLoop> server(m, "LauncherServer", "Embedded VLink Launcher orchestration server",
                                                 nb::is_weak_referenceable());

  nb::class_<LauncherServer::Config>(server, "Config")
      .def(nb::init<>())
      .def_rw("control_scheme", &LauncherServer::Config::control_scheme)
      .def_rw("control_namespace", &LauncherServer::Config::control_namespace)
      .def_rw("control_domain_id", &LauncherServer::Config::control_domain_id)
      .def_rw("security_key", &LauncherServer::Config::security_key)
      .def_rw("bind_ip", &LauncherServer::Config::bind_ip)
      .def_rw("native_mode", &LauncherServer::Config::native_mode)
      .def_rw("enable_control_plane", &LauncherServer::Config::enable_control_plane)
      .def_rw("publish_logs", &LauncherServer::Config::publish_logs)
      .def_rw("manage_console", &LauncherServer::Config::manage_console)
      .def_rw("color_output", &LauncherServer::Config::color_output)
      .def_rw("container_mode", &LauncherServer::Config::container_mode)
      .def_rw("container_name", &LauncherServer::Config::container_name)
      .def_rw("log_dir_override", &LauncherServer::Config::log_dir_override)
      .def_rw("log_level_override", &LauncherServer::Config::log_level_override)
      .def_rw("log_plugin_override", &LauncherServer::Config::log_plugin_override)
      .def_rw("record_dir_override", &LauncherServer::Config::record_dir_override)
      .def_rw("launcher_executable", &LauncherServer::Config::launcher_executable)
      .def_rw("startup_mode_override", &LauncherServer::Config::startup_mode_override)
      .def_rw("max_parallel_override", &LauncherServer::Config::max_parallel_override)
      .def_rw("disable_respawn", &LauncherServer::Config::disable_respawn)
      .def_rw("pause_on_fail", &LauncherServer::Config::pause_on_fail)
      .def_rw("allow_remote_load", &LauncherServer::Config::allow_remote_load)
      .def_rw("only_units", &LauncherServer::Config::only_units)
      .def_rw("skip_units", &LauncherServer::Config::skip_units)
      .def_rw("debug_prefixes", &LauncherServer::Config::debug_prefixes)
      .def_rw("arg_overrides", &LauncherServer::Config::arg_overrides);

  nb::enum_<LauncherServer::GraphFormat>(server, "GraphFormat")
      .value("Dot", LauncherServer::GraphFormat::kDot)
      .value("Ascii", LauncherServer::GraphFormat::kAscii)
      .value("Mermaid", LauncherServer::GraphFormat::kMermaid);

  nb::class_<LauncherServer::ValidationReport>(server, "ValidationReport")
      .def(nb::init<>())
      .def_rw("valid", &LauncherServer::ValidationReport::valid)
      .def_rw("errors", &LauncherServer::ValidationReport::errors)
      .def_rw("warnings", &LauncherServer::ValidationReport::warnings)
      .def_rw("scheduler_group_count", &LauncherServer::ValidationReport::scheduler_group_count)
      .def_rw("slice_count", &LauncherServer::ValidationReport::slice_count)
      .def_rw("group_count", &LauncherServer::ValidationReport::group_count)
      .def_rw("process_count", &LauncherServer::ValidationReport::process_count)
      .def_rw("component_count", &LauncherServer::ValidationReport::component_count)
      .def_rw("container_count", &LauncherServer::ValidationReport::container_count)
      .def_rw("task_count", &LauncherServer::ValidationReport::task_count)
      .def_rw("has_proxy", &LauncherServer::ValidationReport::has_proxy);

  nb::class_<LauncherServer::PluginInfo>(server, "PluginInfo")
      .def(nb::init<>())
      .def_rw("path", &LauncherServer::PluginInfo::path)
      .def_rw("loaded", &LauncherServer::PluginInfo::loaded)
      .def_rw("is_launcher_module", &LauncherServer::PluginInfo::is_launcher_module)
      .def_rw("is_legacy_runnable", &LauncherServer::PluginInfo::is_legacy_runnable)
      .def_rw("is_logger_plugin", &LauncherServer::PluginInfo::is_logger_plugin)
      .def_rw("version_major", &LauncherServer::PluginInfo::version_major)
      .def_rw("version_minor", &LauncherServer::PluginInfo::version_minor)
      .def_rw("components", &LauncherServer::PluginInfo::components)
      .def_rw("error", &LauncherServer::PluginInfo::error);

  server
      .def(nb::new_([](const LauncherServer::Config& config) {
             // NOLINTNEXTLINE(performance-unnecessary-copy-initialization)
             const auto snapshot = config;
             std::shared_ptr<LauncherServer> owner;
             {
               nb::gil_scoped_release release;
               owner = std::shared_ptr<LauncherServer>(new LauncherServer(snapshot), [](LauncherServer* ptr) {
                 std::unique_ptr<LauncherServer> native(ptr);
                 destroy_python_callback_owner(native, current_python_callback_activity());
               });
             }
             auto instance = nb::cast(owner);
             ensure_python_pre_destroy_hook(instance, owner.get(), [](LauncherServer& srv) {
               if VUNLIKELY (current_python_callback_activity()) {
                 return;
               }

               close_launcher_server(srv);
             });
             return instance;
           }),
           "config"_a = LauncherServer::Config{})
      .def("close",
           [](LauncherServer& self) {
             check_launcher_callback(self);
             nb::gil_scoped_release release;
             close_launcher_server(self);
           })
      .def("__enter__", [](nb::object self) -> nb::object { return self; })
      .def("__exit__",
           [](LauncherServer& self, nb::args, nb::kwargs) {
             check_launcher_callback(self);
             nb::gil_scoped_release release;
             close_launcher_server(self);
           })
      .def(
          "wait_for_quit",
          [](LauncherServer& self, int timeout_ms, bool check) {
            check_launcher_callback(self);
            nb::gil_scoped_release release;
            return self.wait_for_quit(timeout_ms, check);
          },
          "timeout_ms"_a = -1, "check"_a = true)
      .def_static("inspect_plugin", &LauncherServer::inspect_plugin, "library_path"_a,
                  nb::call_guard<nb::gil_scoped_release>())
      .def(
          "load_file",
          [](LauncherServer& self, const std::string& file_path) {
            nb::gil_scoped_release release;
            return self.load_file(file_path, nullptr);
          },
          "file_path"_a)
      .def(
          "load_file_with_error",
          [](LauncherServer& self, const std::string& file_path) {
            std::string err;
            bool ok = false;
            {
              nb::gil_scoped_release release;
              ok = self.load_file(file_path, &err);
            }
            return std::make_pair(ok, std::move(err));
          },
          "file_path"_a)
      .def(
          "load_string",
          [](LauncherServer& self, const std::string& toml_content, const std::string& base_dir) {
            nb::gil_scoped_release release;
            return self.load_string(toml_content, base_dir, nullptr);
          },
          "toml_content"_a, "base_dir"_a = ".")
      .def(
          "load_string_with_error",
          [](LauncherServer& self, const std::string& toml_content, const std::string& base_dir) {
            std::string err;
            bool ok = false;
            {
              nb::gil_scoped_release release;
              ok = self.load_string(toml_content, base_dir, &err);
            }
            return std::make_pair(ok, std::move(err));
          },
          "toml_content"_a, "base_dir"_a = ".")
      .def("validate", &LauncherServer::validate, "check_programs"_a = false, nb::call_guard<nb::gil_scoped_release>())
      .def("export_graph", &LauncherServer::export_graph, "format"_a = LauncherServer::GraphFormat::kDot,
           nb::call_guard<nb::gil_scoped_release>())
      .def("export_dot", &LauncherServer::export_dot, nb::call_guard<nb::gil_scoped_release>())
      .def("inspect_manifest_json", &LauncherServer::inspect_manifest_json, nb::call_guard<nb::gil_scoped_release>())
      .def("start",
           [](LauncherServer& self) {
             check_launcher_callback(self);
             nb::gil_scoped_release release;
             return self.start();
           })
      .def("request_stop", &LauncherServer::request_stop, "exit_code"_a = 0, nb::call_guard<nb::gil_scoped_release>())
      .def("stop",
           [](LauncherServer& self) {
             check_launcher_callback(self);
             nb::gil_scoped_release release;
             self.stop();
           })
      .def(
          "wait",
          [](LauncherServer& self, int32_t timeout_ms) {
            check_launcher_callback(self);
            nb::gil_scoped_release release;
            return self.wait(timeout_ms);
          },
          "timeout_ms"_a = -1)
      .def("is_active", &LauncherServer::is_active)
      .def("get_exit_code", &LauncherServer::get_exit_code)
      .def("get_snapshot", &LauncherServer::get_snapshot, nb::call_guard<nb::gil_scoped_release>())
      .def("start_unit", &LauncherServer::start_unit, "target"_a, nb::call_guard<nb::gil_scoped_release>())
      .def("stop_unit", &LauncherServer::stop_unit, "target"_a, nb::call_guard<nb::gil_scoped_release>())
      .def("restart_unit", &LauncherServer::restart_unit, "target"_a, nb::call_guard<nb::gil_scoped_release>())
      .def("reload_unit", &LauncherServer::reload_unit, "target"_a, nb::call_guard<nb::gil_scoped_release>())
      .def("activate_unit", &LauncherServer::activate_unit, "target"_a, nb::call_guard<nb::gil_scoped_release>())
      .def("deactivate_unit", &LauncherServer::deactivate_unit, "target"_a, nb::call_guard<nb::gil_scoped_release>())
      .def("reset_failed", &LauncherServer::reset_failed, "target"_a = "", nb::call_guard<nb::gil_scoped_release>())
      .def("isolate_group", &LauncherServer::isolate_group, "group_name"_a, nb::call_guard<nb::gil_scoped_release>())
      .def("get_unit_config", &LauncherServer::get_unit_config, "unit_name"_a, nb::call_guard<nb::gil_scoped_release>())
      .def(
          "query_logs",
          [](LauncherServer& self, const launcher::LogQuery& query) {
            // NOLINTNEXTLINE(performance-unnecessary-copy-initialization)
            const auto snapshot = query;
            nb::gil_scoped_release release;
            return self.query_logs(snapshot);
          },
          "query"_a = launcher::LogQuery{})
      .def(
          "load_component",
          [](LauncherServer& self, const launcher::LoadComponentRequest& request) {
            // NOLINTNEXTLINE(performance-unnecessary-copy-initialization)
            const auto snapshot = request;
            nb::gil_scoped_release release;
            return self.load_component(snapshot);
          },
          "request"_a)
      .def("unload_component", &LauncherServer::unload_component, "unit_name"_a,
           nb::call_guard<nb::gil_scoped_release>())
      .def("get_params", &LauncherServer::get_params, "unit_name"_a = "", nb::call_guard<nb::gil_scoped_release>())
      .def(
          "set_param",
          [](LauncherServer& self, const std::string& unit_name, const launcher::ParamEntry& entry) {
            // NOLINTNEXTLINE(performance-unnecessary-copy-initialization)
            const auto snapshot = entry;
            nb::gil_scoped_release release;
            return self.set_param(unit_name, snapshot);
          },
          "unit_name"_a, "entry"_a)
      .def("set_record", &LauncherServer::set_record, "target"_a, "enable"_a, "path"_a = "",
           nb::call_guard<nb::gil_scoped_release>())
      .def("resume_startup", &LauncherServer::resume_startup, nb::call_guard<nb::gil_scoped_release>())
      .def("get_log_dir", &LauncherServer::get_log_dir, nb::call_guard<nb::gil_scoped_release>());

  bind_launcher_callback(server, "register_event_callback", &LauncherServer::register_event_callback);
  bind_launcher_callback(server, "register_log_callback", &LauncherServer::register_log_callback);
}

}  // namespace vlink::python
