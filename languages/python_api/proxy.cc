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
#include <nanobind/stl/shared_ptr.h>
#include <nanobind/stl/string.h>
#include <nanobind/stl/unordered_set.h>
#include <nanobind/stl/vector.h>
#include <vlink/base/timer.h>
#include <vlink/external/proxy_api.h>
#include <vlink/external/proxy_server.h>

#include <stdexcept>
#include <type_traits>
#include <utility>

#include "buffer.h"
#include "callbacks.h"

namespace vlink::python {

using namespace nb::literals;  // NOLINT

struct PythonProxyData final {
  std::string url;
  std::string ser;
  SchemaType schema{SchemaType::kUnknown};
  nb::bytes raw{"", 0};
  int64_t timestamp{-1};
  int64_t seq{0};
};

static void close_proxy_api(ProxyAPI& api) {
  api.quit(true);
  api.wait_for_quit(Timer::kInfinite, false);
  api.register_connect_callback({});
  api.register_error_callback({});
  api.register_time_callback({});
  api.register_info_callback({});
  api.register_data_callback({});
}

static void check_proxy_callback(const ProxyAPI& api) {
  if VUNLIKELY (is_in_python_owner_callback(&api) || api.is_in_same_thread()) {
    throw std::runtime_error("Cannot replace callbacks or wait for ProxyAPI shutdown inside its callback");
  }
}

static void check_proxy_callback(const ProxyServer& server) {
  if VUNLIKELY (server.is_running() || is_in_python_owner_callback(&server) || server.is_in_same_thread()) {
    throw std::runtime_error("Register ProxyServer callbacks before startup or after confirmed shutdown");
  }
}

template <typename ProxyT, typename... ArgsT>
static void bind_proxy_callback(nb::class_<ProxyT, MessageLoop>& cls, const char* name,
                                void (ProxyT::*method)(MoveFunction<void(ArgsT...)>&&)) {
  cls.def(
      name,
      [method, name](ProxyT& self, nb::object callback) {
        check_proxy_callback(self);
        MoveFunction<void(ArgsT...)> wrapped;

        if (!callback.is_none()) {
          auto cb = std::make_shared<GilSafePyFunction>(nb::cast<nb::callable>(callback));
          auto activity = std::make_shared<PythonCallbackActivity>();
          wrapped = [native = &self, activity, cb, name](ArgsT... args) {
            invoke_owned_python_callback(native, activity, name, [&]() { cb->fn(args...); });
          };
        }

        if constexpr (std::is_same_v<ProxyT, ProxyAPI>) {
          nb::gil_scoped_release release;
          (self.*method)(std::move(wrapped));
        } else {
          (self.*method)(std::move(wrapped));
        }
      },
      "callback"_a.none(), "Set the callback, or None to unregister; call outside this API's callbacks");
}

void bind_proxy(nb::module_& m) {
  nb::class_<ProxyAPI, MessageLoop> api(m, "ProxyAPI", "Proxy controller or listener", nb::is_weak_referenceable());
  nb::enum_<ProxyAPI::Role>(api, "Role")
      .value("Controller", ProxyAPI::kController)
      .value("Listener", ProxyAPI::kListener);
  nb::enum_<ProxyAPI::Mode>(api, "Mode")
      .value("Offline", ProxyAPI::kOffline)
      .value("ObserveOne", ProxyAPI::kObserveOne)
      .value("ObserveAll", ProxyAPI::kObserveAll)
      .value("Record", ProxyAPI::kRecord)
      .value("Play", ProxyAPI::kPlay)
      .value("Edit", ProxyAPI::kEdit)
      .value("Auto", ProxyAPI::kAuto)
      .value("AutoAndObserveAll", ProxyAPI::kAutoAndObserveAll);
  nb::enum_<ProxyAPI::Error>(api, "Error")
      .value("NoError", ProxyAPI::kNoError)
      .value("ModeError", ProxyAPI::kModeError)
      .value("ControlError", ProxyAPI::kControlError)
      .value("ReliableCompError", ProxyAPI::kReliableCompError)
      .value("TcpCompError", ProxyAPI::kTcpCompError)
      .value("DirectCompError", ProxyAPI::kDirectCompError)
      .value("MultiProxyError", ProxyAPI::kMultiProxyError)
      .value("VersionCompError", ProxyAPI::kVersionCompError)
      .value("TokenError", ProxyAPI::kTokenError)
      .value("UnknownError", ProxyAPI::kUnknownError);
  nb::enum_<ProxyAPI::Status>(api, "Status")
      .value("Active", ProxyAPI::kActive)
      .value("InActive", ProxyAPI::kInActive)
      .value("Pending", ProxyAPI::kPending)
      .value("Invalid", ProxyAPI::kInvalid);

  nb::class_<ProxyAPI::Config>(api, "Config")
      .def(nb::init<>())
      .def_rw("role", &ProxyAPI::Config::role)
      .def_rw("domain_id", &ProxyAPI::Config::domain_id)
      .def_rw("dds_impl", &ProxyAPI::Config::dds_impl)
      .def_rw("security_key", &ProxyAPI::Config::security_key)
      .def_rw("native", &ProxyAPI::Config::native)
      .def_rw("reliable", &ProxyAPI::Config::reliable)
      .def_rw("direct", &ProxyAPI::Config::direct)
      .def_rw("enable_tcp", &ProxyAPI::Config::enable_tcp)
      .def_rw("match_version", &ProxyAPI::Config::match_version)
      .def_rw("allow_ip", &ProxyAPI::Config::allow_ip)
      .def_rw("peer_ip", &ProxyAPI::Config::peer_ip)
      .def_rw("buf_size", &ProxyAPI::Config::buf_size)
      .def_rw("mtu_size", &ProxyAPI::Config::mtu_size);

  nb::class_<ProxyAPI::Process>(api, "Process")
      .def_ro("type", &ProxyAPI::Process::type)
      .def_ro("host", &ProxyAPI::Process::host)
      .def_ro("pid", &ProxyAPI::Process::pid)
      .def_ro("name", &ProxyAPI::Process::name)
      .def_ro("ip_list", &ProxyAPI::Process::ip_list);

  nb::class_<ProxyAPI::Info>(api, "Info")
      .def_ro("type", &ProxyAPI::Info::type)
      .def_ro("url", &ProxyAPI::Info::url)
      .def_ro("ser", &ProxyAPI::Info::ser)
      .def_ro("schema", &ProxyAPI::Info::schema)
      .def_ro("status", &ProxyAPI::Info::status)
      .def_ro("freq", &ProxyAPI::Info::freq)
      .def_ro("rate", &ProxyAPI::Info::rate)
      .def_ro("loss", &ProxyAPI::Info::loss)
      .def_ro("latency", &ProxyAPI::Info::latency)
      .def_ro("process_list", &ProxyAPI::Info::process_list);

  nb::class_<ProxyAPI::UrlMeta>(api, "UrlMeta")
      .def(nb::init<>())
      .def_rw("url", &ProxyAPI::UrlMeta::url)
      .def_rw("ser", &ProxyAPI::UrlMeta::ser)
      .def_rw("schema", &ProxyAPI::UrlMeta::schema)
      .def_rw("type", &ProxyAPI::UrlMeta::type);

  nb::class_<ProxyAPI::Control>(api, "Control")
      .def(nb::init<>())
      .def_rw("mode", &ProxyAPI::Control::mode)
      .def_rw("url_meta_list", &ProxyAPI::Control::url_meta_list)
      .def_rw("filter_by_process", &ProxyAPI::Control::filter_by_process)
      .def_rw("filter_str", &ProxyAPI::Control::filter_str)
      .def_rw("filter_type", &ProxyAPI::Control::filter_type)
      .def_rw("bridge", &ProxyAPI::Control::bridge);

  nb::class_<PythonProxyData>(api, "Data", "Owned payload and routing metadata; raw is immutable Python bytes")
      .def(nb::init<>())
      .def_rw("url", &PythonProxyData::url)
      .def_rw("ser", &PythonProxyData::ser)
      .def_rw("schema", &PythonProxyData::schema)
      .def_rw("timestamp", &PythonProxyData::timestamp)
      .def_rw("seq", &PythonProxyData::seq)
      .def_prop_rw(
          "raw", [](const PythonProxyData& self) { return self.raw; },
          [](PythonProxyData& self, nb::handle value) {
            if (nb::isinstance<nb::bytes>(value)) {
              self.raw = nb::borrow<nb::bytes>(value);
            } else {
              PythonBufferView view(value);
              self.raw = nb::bytes(view.data(), view.size());
            }
          });

  api.def(nb::new_([](const ProxyAPI::Config& config) {
            // NOLINTNEXTLINE(performance-unnecessary-copy-initialization)
            const auto snapshot = config;
            std::shared_ptr<ProxyAPI> owner;
            {
              nb::gil_scoped_release release;
              owner = std::shared_ptr<ProxyAPI>(new ProxyAPI(snapshot), [](ProxyAPI* api) {
                std::unique_ptr<ProxyAPI> native(api);
                destroy_python_callback_owner(native, current_python_callback_activity());
              });
            }
            auto instance = nb::cast(owner);
            ensure_python_pre_destroy_hook(instance, owner.get(), [](ProxyAPI& api) {
              if VUNLIKELY (current_python_callback_activity()) {
                return;
              }

              close_proxy_api(api);
            });
            return instance;
          }),
          "config"_a = ProxyAPI::Config{})
      .def("close",
           [](ProxyAPI& self) {
             check_proxy_callback(self);
             nb::gil_scoped_release release;
             close_proxy_api(self);
           })
      .def(
          "wait_for_quit",
          [](ProxyAPI& self, int timeout_ms, bool check) {
            check_proxy_callback(self);
            nb::gil_scoped_release release;
            return self.wait_for_quit(timeout_ms, check);
          },
          "timeout_ms"_a = -1, "check"_a = true)
      .def(
          "send_control",
          [](ProxyAPI& self, const ProxyAPI::Control& control, bool async) {
            // NOLINTNEXTLINE(performance-unnecessary-copy-initialization)
            const auto snapshot = control;
            nb::gil_scoped_release release;
            return self.send_control(snapshot, async);
          },
          "control"_a, "async_"_a = true)
      .def(
          "send_data",
          [](ProxyAPI& self, const PythonProxyData& input) {
            const auto raw = input.raw;
            ProxyAPI::Data data;
            data.url = input.url;
            data.ser = input.ser;
            data.schema = input.schema;
            data.timestamp = input.timestamp;
            data.seq = input.seq;
            data.raw = Bytes::shallow_copy(reinterpret_cast<const uint8_t*>(raw.c_str()), raw.size());
            nb::gil_scoped_release release;
            return self.send_data(data);
          },
          "data"_a)
      .def(
          "register_data_callback",
          [](ProxyAPI& self, nb::object callback) {
            check_proxy_callback(self);
            ProxyAPI::DataCallback wrapped;

            if (!callback.is_none()) {
              auto cb = std::make_shared<GilSafePyFunction>(nb::cast<nb::callable>(callback));
              auto activity = std::make_shared<PythonCallbackActivity>();
              wrapped = [native = &self, activity, cb](const ProxyAPI::Data& data) {
                invoke_owned_python_callback(native, activity, "vlink::ProxyAPI.register_data_callback", [&]() {
                  PythonProxyData owned;
                  owned.url = data.url;
                  owned.ser = data.ser;
                  owned.schema = data.schema;
                  owned.raw = nb::bytes(data.raw.data(), data.raw.size());
                  owned.timestamp = data.timestamp;
                  owned.seq = data.seq;
                  cb->fn(std::move(owned));
                });
              };
            }

            nb::gil_scoped_release release;
            self.register_data_callback(std::move(wrapped));
          },
          "callback"_a.none(), "callback(Data) receives an owned snapshot; None unregisters")
      .def("get_current_config", &ProxyAPI::get_current_config, nb::rv_policy::copy)
      .def("get_current_mode", &ProxyAPI::get_current_mode)
      .def("get_current_error", &ProxyAPI::get_current_error)
      .def("get_current_hostname", &ProxyAPI::get_current_hostname)
      .def("get_current_machine_id", &ProxyAPI::get_current_machine_id)
      .def("get_current_sys_time", &ProxyAPI::get_current_sys_time)
      .def("get_current_boot_time", &ProxyAPI::get_current_boot_time)
      .def("get_current_cpu_usage", &ProxyAPI::get_current_cpu_usage)
      .def("get_current_memory_usage", &ProxyAPI::get_current_memory_usage)
      .def("get_latency", &ProxyAPI::get_latency, nb::call_guard<nb::gil_scoped_release>())
      .def("get_lost", &ProxyAPI::get_lost, nb::call_guard<nb::gil_scoped_release>())
      .def("is_connected", &ProxyAPI::is_connected)
      .def("get_proxy_version", &ProxyAPI::get_proxy_version)
      .def("get_proxy_hostnames", &ProxyAPI::get_proxy_hostnames)
      .def("get_proxy_machine_ids", &ProxyAPI::get_proxy_machine_ids)
      .def_static("is_support_shm", &ProxyAPI::is_support_shm)
      .def_static("is_enable_filter", &ProxyAPI::is_enable_filter)
      .def_static("get_format_sys_time", &ProxyAPI::get_format_sys_time, "time"_a, "enable_utc"_a = false)
      .def_static("get_format_boot_time", &ProxyAPI::get_format_boot_time, "time"_a);

  bind_proxy_callback(api, "register_connect_callback", &ProxyAPI::register_connect_callback);
  bind_proxy_callback(api, "register_error_callback", &ProxyAPI::register_error_callback);
  bind_proxy_callback(api, "register_time_callback", &ProxyAPI::register_time_callback);
  bind_proxy_callback(api, "register_info_callback", &ProxyAPI::register_info_callback);

  nb::class_<ProxyServer, MessageLoop> server(m, "ProxyServer", "One proxy server per process",
                                              nb::is_weak_referenceable());
  nb::class_<ProxyServer::Config>(server, "Config")
      .def(nb::init<>())
      .def_rw("async_", &ProxyServer::Config::async)
      .def_rw("reliable", &ProxyServer::Config::reliable)
      .def_rw("enable_tcp", &ProxyServer::Config::enable_tcp)
      .def_rw("direct", &ProxyServer::Config::direct)
      .def_rw("native_mode", &ProxyServer::Config::native_mode)
      .def_rw("domain_id", &ProxyServer::Config::domain_id)
      .def_rw("buf_size", &ProxyServer::Config::buf_size)
      .def_rw("mtu_size", &ProxyServer::Config::mtu_size)
      .def_rw("max_packet_size", &ProxyServer::Config::max_packet_size)
      .def_rw("security_key", &ProxyServer::Config::security_key)
      .def_rw("allow_ip", &ProxyServer::Config::allow_ip)
      .def_rw("peer_ip", &ProxyServer::Config::peer_ip)
      .def_rw("dds_impl", &ProxyServer::Config::dds_impl)
      .def_rw("use_iox", &ProxyServer::Config::use_iox)
      .def_rw("iox_monitoring", &ProxyServer::Config::iox_monitoring)
      .def_rw("iox_strategy", &ProxyServer::Config::iox_strategy)
      .def_rw("iox_config", &ProxyServer::Config::iox_config)
      .def_rw("runnable_version_major", &ProxyServer::Config::runnable_version_major)
      .def_rw("runnable_version_minor", &ProxyServer::Config::runnable_version_minor)
      .def_rw("runnable_prefix", &ProxyServer::Config::runnable_prefix)
      .def_rw("runnable_list", &ProxyServer::Config::runnable_list)
      .def_rw("bridge", &ProxyServer::Config::bridge)
      .def_rw("bridge_filter", &ProxyServer::Config::bridge_filter)
      .def_rw("bridge_subscribe", &ProxyServer::Config::bridge_subscribe)
      .def_rw("callback_mode", &ProxyServer::Config::callback_mode);

  server
      .def(nb::new_([](const ProxyServer::Config& config) {
             // NOLINTNEXTLINE(performance-unnecessary-copy-initialization)
             const auto snapshot = config;
             std::shared_ptr<ProxyServer> owner;
             {
               nb::gil_scoped_release release;
               owner = std::shared_ptr<ProxyServer>(new ProxyServer(snapshot), [](ProxyServer* server) {
                 std::unique_ptr<ProxyServer> native(server);
                 destroy_python_callback_owner(native, current_python_callback_activity());
               });
             }
             auto instance = nb::cast(owner);
             ensure_python_pre_destroy_hook(instance, owner.get(), [](ProxyServer& server) {
               if VUNLIKELY (current_python_callback_activity()) {
                 return;
               }

               server.quit(true);
               server.wait_for_quit(Timer::kInfinite, false);

               nb::gil_scoped_acquire gil;
               server.register_data_callback({});
               server.register_info_callback({});
               server.register_time_callback({});
             });
             return instance;
           }),
           "config"_a = ProxyServer::Config{})
      .def("close",
           [](ProxyServer& self) {
             if VUNLIKELY (current_python_callback_activity()) {
               throw std::runtime_error("Cannot wait for ProxyServer shutdown inside a Python callback");
             }
             {
               nb::gil_scoped_release release;
               self.quit(true);
               self.wait_for_quit(Timer::kInfinite, false);
             }

             self.register_data_callback({});
             self.register_info_callback({});
             self.register_time_callback({});
           })
      .def(
          "wait_for_quit",
          [](ProxyServer& self, int timeout_ms, bool check) {
            if VUNLIKELY (current_python_callback_activity()) {
              throw std::runtime_error("Cannot wait for ProxyServer shutdown inside a Python callback");
            }

            nb::gil_scoped_release release;
            return self.wait_for_quit(timeout_ms, check);
          },
          "timeout_ms"_a = -1, "check"_a = true)
      .def(
          "send_control",
          [](ProxyServer& self, const ProxyAPI::Control& control) {
            // NOLINTNEXTLINE(performance-unnecessary-copy-initialization)
            const auto snapshot = control;
            nb::gil_scoped_release release;
            return self.send_control(snapshot);
          },
          "control"_a, "Queue local control in callback mode; True does not mean subscriptions are ready")
      .def(
          "register_data_callback",
          [](ProxyServer& self, nb::object callback) {
            check_proxy_callback(self);
            ProxyAPI::DataCallback wrapped;

            if (!callback.is_none()) {
              auto cb = std::make_shared<GilSafePyFunction>(nb::cast<nb::callable>(callback));
              auto activity = std::make_shared<PythonCallbackActivity>();
              wrapped = [native = &self, activity, cb](const ProxyAPI::Data& data) {
                invoke_owned_python_callback(native, activity, "vlink::ProxyServer.register_data_callback", [&]() {
                  PythonProxyData owned;
                  owned.url = data.url;
                  owned.ser = data.ser;
                  owned.schema = data.schema;
                  owned.raw = nb::bytes(data.raw.data(), data.raw.size());
                  owned.timestamp = data.timestamp;
                  owned.seq = data.seq;
                  cb->fn(std::move(owned));
                });
              };
            }

            self.register_data_callback(std::move(wrapped));
          },
          "callback"_a.none(), "Register while stopped; callback(Data) receives an owned snapshot; None unregisters")
      .def("get_token", &ProxyServer::get_token);

  bind_proxy_callback(server, "register_time_callback", &ProxyServer::register_time_callback);
  bind_proxy_callback(server, "register_info_callback", &ProxyServer::register_info_callback);
}

}  // namespace vlink::python
