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
#include <vlink/external/launcher_notify.h>
#include <vlink/external/launcher_server.h>

#include <cmath>
#include <cstdint>
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
  api.quit(true);
  api.wait_for_quit(Timer::kInfinite, false);
}

static void close_launcher_server(LauncherServer& server) {
  server.register_event_callback({});
  server.stop();
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
      throw std::invalid_argument("Non-finite float is not supported for LauncherParamValue");
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

    if (all_bool) {
      std::vector<bool> out;
      out.reserve(n);

      for (const auto& item : items) {
        out.push_back(nb::cast<bool>(item));
      }

      return out;
    }

    if (all_int) {
      std::vector<int64_t> out;
      out.reserve(n);

      for (const auto& item : items) {
        out.push_back(nb::cast<int64_t>(item));
      }

      return out;
    }

    if (all_num) {
      std::vector<double> out;
      out.reserve(n);

      for (const auto& item : items) {
        const auto d = nb::cast<double>(item);

        if (!std::isfinite(d)) {
          throw std::invalid_argument("Non-finite float is not supported in LauncherParamValue list");
        }

        out.push_back(d);
      }

      return out;
    }

    if (all_str) {
      std::vector<std::string> out;
      out.reserve(n);

      for (const auto& item : items) {
        out.push_back(nb::cast<std::string>(item));
      }

      return out;
    }

    throw std::invalid_argument("Unsupported mixed list type for LauncherParamValue");
  }

  throw std::invalid_argument("Unsupported value type for LauncherParamValue");
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
  nb::module_ lm = m.def_submodule("launcher", "VLink Launcher orchestration & control-plane data model");

  auto unit_type = nb::enum_<launcher::UnitType>(lm, "UnitType")
                       .value("Process", launcher::UnitType::kProcess)
                       .value("Component", launcher::UnitType::kComponent);

  auto unit_state = nb::enum_<launcher::UnitState>(lm, "UnitState")
                        .value("Pending", launcher::UnitState::kPending)
                        .value("Starting", launcher::UnitState::kStarting)
                        .value("Running", launcher::UnitState::kRunning)
                        .value("Ready", launcher::UnitState::kReady)
                        .value("Stopping", launcher::UnitState::kStopping)
                        .value("Stopped", launcher::UnitState::kStopped)
                        .value("Failed", launcher::UnitState::kFailed);

  auto error_code = nb::enum_<launcher::ErrorCode>(lm, "ErrorCode")
                        .value("Ok", launcher::ErrorCode::kOk)
                        .value("InvalidArgument", launcher::ErrorCode::kInvalidArgument)
                        .value("NotFound", launcher::ErrorCode::kNotFound)
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

  nb::class_<launcher::ParamEntry>(lm, "ParamEntry")
      .def(nb::init<>())
      .def_rw("key", &launcher::ParamEntry::key)
      .def_rw("type", &launcher::ParamEntry::type)
      .def_rw("value", &launcher::ParamEntry::value);

  nb::class_<launcher::UnitStatus>(lm, "UnitStatus")
      .def(nb::init<>())
      .def_rw("name", &launcher::UnitStatus::name)
      .def_rw("ns", &launcher::UnitStatus::ns)
      .def_rw("group", &launcher::UnitStatus::group)
      .def_rw("type", &launcher::UnitStatus::type)
      .def_rw("state", &launcher::UnitStatus::state)
      .def_rw("pid", &launcher::UnitStatus::pid)
      .def_rw("exit_code", &launcher::UnitStatus::exit_code)
      .def_rw("restart_count", &launcher::UnitStatus::restart_count)
      .def_rw("generation", &launcher::UnitStatus::generation)
      .def_rw("start_time_ms", &launcher::UnitStatus::start_time_ms)
      .def_rw("uptime_ms", &launcher::UnitStatus::uptime_ms)
      .def_rw("healthy", &launcher::UnitStatus::healthy)
      .def_rw("required", &launcher::UnitStatus::required)
      .def_rw("failed_reason", &launcher::UnitStatus::failed_reason)
      .def_rw("detail", &launcher::UnitStatus::detail)
      .def_rw("dependencies", &launcher::UnitStatus::dependencies);

  nb::class_<launcher::SystemSnapshot>(lm, "SystemSnapshot")
      .def(nb::init<>())
      .def_rw("session_id", &launcher::SystemSnapshot::session_id)
      .def_rw("version", &launcher::SystemSnapshot::version)
      .def_rw("host", &launcher::SystemSnapshot::host)
      .def_rw("server_pid", &launcher::SystemSnapshot::server_pid)
      .def_rw("timestamp_ms", &launcher::SystemSnapshot::timestamp_ms)
      .def_rw("uptime_ms", &launcher::SystemSnapshot::uptime_ms)
      .def_rw("config_file", &launcher::SystemSnapshot::config_file)
      .def_rw("units", &launcher::SystemSnapshot::units);

  nb::class_<launcher::EventRecord>(lm, "EventRecord")
      .def(nb::init<>())
      .def_rw("timestamp_ms", &launcher::EventRecord::timestamp_ms)
      .def_rw("unit_name", &launcher::EventRecord::unit_name)
      .def_rw("unit_type", &launcher::EventRecord::unit_type)
      .def_rw("old_state", &launcher::EventRecord::old_state)
      .def_rw("new_state", &launcher::EventRecord::new_state)
      .def_rw("event_name", &launcher::EventRecord::event_name)
      .def_rw("message", &launcher::EventRecord::message);

  nb::class_<launcher::CommandResult>(lm, "CommandResult")
      .def(nb::init<>())
      .def_rw("code", &launcher::CommandResult::code)
      .def_rw("message", &launcher::CommandResult::message)
      .def_rw("snapshot", &launcher::CommandResult::snapshot)
      .def_rw("params", &launcher::CommandResult::params)
      .def("ok", &launcher::CommandResult::ok);

  lm.def("unit_type_name", [](launcher::UnitType t) { return std::string(launcher::unit_type_name(t)); }, "type"_a);
  lm.def("unit_state_name", [](launcher::UnitState s) { return std::string(launcher::unit_state_name(s)); }, "state"_a);
  lm.def("error_code_name", [](launcher::ErrorCode c) { return std::string(launcher::error_code_name(c)); }, "code"_a);
  lm.def(
      "encode_param",
      [](const std::string& key, nb::handle value) {
        return launcher::param_value_to_entry(key, python_to_param_value(value));
      },
      "key"_a, "value"_a);
  lm.def(
      "decode_param",
      [](const launcher::ParamEntry& entry) -> nb::object {
        LauncherParamValue out;

        if (!launcher::entry_to_param_value(entry, out)) {
          throw std::invalid_argument("Failed to decode ParamEntry '" + entry.key + "'");
        }

        return param_value_to_python(out);
      },
      "entry"_a);

  (void)unit_type;
  (void)unit_state;
  (void)param_type;

  auto api = nb::class_<LauncherAPI, MessageLoop>(m, "LauncherAPI", "Control-plane client for LauncherServer",
                                                  nb::is_weak_referenceable());
  api.attr("ErrorCode") = error_code;

  nb::class_<LauncherAPI::Config>(api, "Config")
      .def(nb::init<>())
      .def_rw("scheme", &LauncherAPI::Config::scheme)
      .def_rw("namespace_prefix", &LauncherAPI::Config::namespace_prefix)
      .def_rw("domain_id", &LauncherAPI::Config::domain_id)
      .def_rw("security_key", &LauncherAPI::Config::security_key)
      .def_rw("default_timeout_ms", &LauncherAPI::Config::default_timeout_ms);

  api.def(nb::new_([](const LauncherAPI::Config& config) {
            // NOLINTNEXTLINE(performance-unnecessary-copy-initialization)
            const auto snapshot = config;
            std::shared_ptr<LauncherAPI> owner;
            {
              nb::gil_scoped_release release;
              owner = std::shared_ptr<LauncherAPI>(new LauncherAPI(snapshot), [](LauncherAPI* raw) {
                std::unique_ptr<LauncherAPI> native(raw);
                destroy_python_callback_owner(native, current_python_callback_activity());
              });
            }
            auto instance = nb::cast(owner);
            ensure_python_pre_destroy_hook(instance, owner.get(), [](LauncherAPI& inst) {
              if VUNLIKELY (current_python_callback_activity()) {
                return;
              }

              close_launcher_api(inst);
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
      .def(
          "wait_for_quit",
          [](LauncherAPI& self, int timeout_ms, bool check) {
            check_launcher_callback(self);
            nb::gil_scoped_release release;
            return self.wait_for_quit(timeout_ms, check);
          },
          "timeout_ms"_a = -1, "check"_a = true)
      .def(
          "__enter__", [](LauncherAPI& self) -> LauncherAPI& { return self; }, nb::rv_policy::reference)
      .def("__exit__",
           [](LauncherAPI& self, nb::args, nb::kwargs) {
             check_launcher_callback(self);
             nb::gil_scoped_release release;
             close_launcher_api(self);
           })
      .def("is_connected", &LauncherAPI::is_connected)
      .def("wait_for_connected", &LauncherAPI::wait_for_connected, "timeout_ms"_a = 3000,
           nb::call_guard<nb::gil_scoped_release>())
      .def("session_id", &LauncherAPI::session_id, nb::call_guard<nb::gil_scoped_release>())
      .def("latest_snapshot", &LauncherAPI::latest_snapshot, nb::call_guard<nb::gil_scoped_release>())
      .def("get_status", &LauncherAPI::get_status, "timeout_ms"_a = 0, nb::call_guard<nb::gil_scoped_release>())
      .def("start_unit", &LauncherAPI::start_unit, "target"_a, "timeout_ms"_a = 0,
           nb::call_guard<nb::gil_scoped_release>())
      .def("stop_unit", &LauncherAPI::stop_unit, "target"_a, "timeout_ms"_a = 0,
           nb::call_guard<nb::gil_scoped_release>())
      .def("restart_unit", &LauncherAPI::restart_unit, "target"_a, "timeout_ms"_a = 0,
           nb::call_guard<nb::gil_scoped_release>())
      .def("reload_unit", &LauncherAPI::reload_unit, "target"_a, "timeout_ms"_a = 0,
           nb::call_guard<nb::gil_scoped_release>())
      .def("get_params", &LauncherAPI::get_params, "unit_name"_a = "", "timeout_ms"_a = 0,
           nb::call_guard<nb::gil_scoped_release>())
      .def(
          "set_param",
          [](LauncherAPI& self, const std::string& unit_name, const launcher::ParamEntry& entry, uint32_t timeout_ms) {
            const launcher::ParamEntry snapshot(entry);
            nb::gil_scoped_release release;
            return self.set_param(unit_name, snapshot, timeout_ms);
          },
          "unit_name"_a, "entry"_a, "timeout_ms"_a = 0)
      .def("shutdown_server", &LauncherAPI::shutdown_server, "timeout_ms"_a = 0,
           nb::call_guard<nb::gil_scoped_release>());

  bind_launcher_callback(api, "register_connect_callback", &LauncherAPI::register_connect_callback);
  bind_launcher_callback(api, "register_snapshot_callback", &LauncherAPI::register_snapshot_callback);
  bind_launcher_callback(api, "register_event_callback", &LauncherAPI::register_event_callback);

  auto server = nb::class_<LauncherServer, MessageLoop>(m, "LauncherServer", "Embedded VLink orchestrator server",
                                                        nb::is_weak_referenceable());

  nb::class_<LauncherServer::Config>(server, "Config")
      .def(nb::init<>())
      .def_rw("enable_control_plane", &LauncherServer::Config::enable_control_plane)
      .def_rw("control_scheme_override", &LauncherServer::Config::control_scheme_override)
      .def_rw("control_namespace_override", &LauncherServer::Config::control_namespace_override)
      .def_rw("control_domain_override", &LauncherServer::Config::control_domain_override)
      .def_rw("security_key_override", &LauncherServer::Config::security_key_override)
      .def_rw("disable_respawn", &LauncherServer::Config::disable_respawn)
      .def_rw("args_override", &LauncherServer::Config::args_override)
      .def_rw("only_targets", &LauncherServer::Config::only_targets)
      .def_rw("skip_targets", &LauncherServer::Config::skip_targets);

  server
      .def(nb::new_([](const LauncherServer::Config& config) {
             // NOLINTNEXTLINE(performance-unnecessary-copy-initialization)
             const auto snapshot = config;
             std::shared_ptr<LauncherServer> owner;
             {
               nb::gil_scoped_release release;
               owner = std::shared_ptr<LauncherServer>(new LauncherServer(snapshot), [](LauncherServer* raw) {
                 std::unique_ptr<LauncherServer> native(raw);
                 destroy_python_callback_owner(native, current_python_callback_activity());
               });
             }
             auto instance = nb::cast(owner);
             ensure_python_pre_destroy_hook(instance, owner.get(), [](LauncherServer& inst) {
               if VUNLIKELY (current_python_callback_activity()) {
                 return;
               }

               close_launcher_server(inst);
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
      .def(
          "wait_for_quit",
          [](LauncherServer& self, int timeout_ms, bool check) {
            check_launcher_callback(self);
            nb::gil_scoped_release release;
            return self.wait_for_quit(timeout_ms, check);
          },
          "timeout_ms"_a = -1, "check"_a = true)
      .def(
          "__enter__", [](LauncherServer& self) -> LauncherServer& { return self; }, nb::rv_policy::reference)
      .def("__exit__",
           [](LauncherServer& self, nb::args, nb::kwargs) {
             check_launcher_callback(self);
             nb::gil_scoped_release release;
             close_launcher_server(self);
           })
      .def(
          "load_file",
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
      .def("startup_order", &LauncherServer::startup_order, nb::call_guard<nb::gil_scoped_release>())
      .def("start",
           [](LauncherServer& self) {
             check_launcher_callback(self);
             require_python_finalizers();
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
      .def("get_params", &LauncherServer::get_params, "unit_name"_a = "", nb::call_guard<nb::gil_scoped_release>())
      .def(
          "set_param",
          [](LauncherServer& self, const std::string& unit_name, const launcher::ParamEntry& entry) {
            const launcher::ParamEntry snapshot(entry);
            nb::gil_scoped_release release;
            return self.set_param(unit_name, snapshot);
          },
          "unit_name"_a, "entry"_a);

  bind_launcher_callback(server, "register_event_callback", &LauncherServer::register_event_callback);

  auto notify =
      nb::class_<LauncherNotify>(m, "LauncherNotify",
                                 "Process-side readiness, status, health, and watchdog client (sd_notify)")
          .def_static("is_managed", &LauncherNotify::is_managed)
          .def_static("unit_name", &LauncherNotify::unit_name)
          .def_static("unit_generation", &LauncherNotify::unit_generation)
          .def_static("watchdog_interval_ms", &LauncherNotify::watchdog_interval_ms)
          .def_static(
              "notify",
              [](const std::string& state) {
                nb::gil_scoped_release release;
                return LauncherNotify::notify(state);
              },
              "state"_a)
          .def_static(
              "notify_ready",
              [](const std::string& status) {
                nb::gil_scoped_release release;
                return LauncherNotify::notify_ready(status);
              },
              "status"_a = "")
          .def_static(
              "notify_reloading",
              [](const std::string& status) {
                nb::gil_scoped_release release;
                return LauncherNotify::notify_reloading(status);
              },
              "status"_a = "")
          .def_static(
              "notify_stopping",
              [](const std::string& status) {
                nb::gil_scoped_release release;
                return LauncherNotify::notify_stopping(status);
              },
              "status"_a = "")
          .def_static(
              "notify_status",
              [](const std::string& status) {
                nb::gil_scoped_release release;
                return LauncherNotify::notify_status(status);
              },
              "status"_a)
          .def_static(
              "notify_health",
              [](bool healthy, const std::string& status) {
                nb::gil_scoped_release release;
                return LauncherNotify::notify_health(healthy, status);
              },
              "healthy"_a, "status"_a = "")
          .def_static("notify_watchdog", &LauncherNotify::notify_watchdog, nb::call_guard<nb::gil_scoped_release>())
          .def_static(
              "notify_watchdog_trigger",
              [](const std::string& reason) {
                nb::gil_scoped_release release;
                return LauncherNotify::notify_watchdog_trigger(reason);
              },
              "reason"_a = "")
          .def_static("notify_extend_timeout", &LauncherNotify::notify_extend_timeout, "extra_ms"_a,
                      nb::call_guard<nb::gil_scoped_release>());
  lm.attr("LauncherNotify") = notify;
}

}  // namespace vlink::python
