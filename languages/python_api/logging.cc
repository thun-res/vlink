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
#include <nanobind/stl/string.h>
#include <vlink/base/logger.h>

#include <mutex>
#include <stdexcept>

#include "callbacks.h"

namespace vlink::python {

using namespace nb::literals;  // NOLINT

static std::mutex logger_callback_mutex;

static std::shared_ptr<GilSafePyFunction>& logger_console_callback_owner() {
  static auto* owner = new std::shared_ptr<GilSafePyFunction>();
  return *owner;
}

static std::shared_ptr<GilSafePyFunction>& logger_file_callback_owner() {
  static auto* owner = new std::shared_ptr<GilSafePyFunction>();
  return *owner;
}

static const void* python_logger_callback_owner() noexcept {
  static const char kOwner{};
  return &kOwner;
}

static void register_python_logger_handler(std::shared_ptr<GilSafePyFunction>& owner,
                                           void (*register_handler)(vlink::Logger::Callback&&),
                                           std::optional<nb::callable> callback, const char* context) {
  if VUNLIKELY (is_in_python_owner_callback(python_logger_callback_owner())) {
    throw std::runtime_error("Logger handlers cannot be replaced from an active logger callback");
  }

  std::shared_ptr<GilSafePyFunction> cb;
  vlink::Logger::Callback handler;

  if (callback) {
    auto activity = std::make_shared<PythonCallbackActivity>();
    cb = std::make_shared<GilSafePyFunction>(std::move(*callback));
    handler = [activity, cb, context](vlink::Logger::Level level, std::string_view msg) {
      PythonCallbackScope active(activity, python_logger_callback_owner());

      if VUNLIKELY (!Py_IsInitialized()) {
        return;
      }

      nb::gil_scoped_acquire gil;
      try {
        cb->fn(level, std::string(msg));
      } catch (std::exception&) {
        report_current_exception(context);
      }
    };
  }

  std::shared_ptr<GilSafePyFunction> previous;
  {
    nb::gil_scoped_release release;
    std::lock_guard lock(logger_callback_mutex);
    previous = std::exchange(owner, cb);
    register_handler(std::move(handler));
  }
}

void bind_logging(nb::module_& m) {
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
          [](std::optional<nb::callable> callback) {
            register_python_logger_handler(logger_console_callback_owner(), &vlink::Logger::register_console_handler,
                                           std::move(callback), "vlink::Logger.register_console_handler");
          },
          "callback"_a.none(), "Set a console handler, or pass None to restore default output.")
      .def_static(
          "register_file_handler",
          [](std::optional<nb::callable> callback) {
            register_python_logger_handler(logger_file_callback_owner(), &vlink::Logger::register_file_handler,
                                           std::move(callback), "vlink::Logger.register_file_handler");
          },
          "callback"_a.none(), "Set a file handler, or pass None to restore default output.");

  m.def("log_trace", [](const std::string& msg) { vlink::Logger::print<vlink::Logger::kTrace>(msg); }, "msg"_a);
  m.def("log_debug", [](const std::string& msg) { vlink::Logger::print<vlink::Logger::kDebug>(msg); }, "msg"_a);
  m.def("log_info", [](const std::string& msg) { vlink::Logger::print<vlink::Logger::kInfo>(msg); }, "msg"_a);
  m.def("log_warn", [](const std::string& msg) { vlink::Logger::print<vlink::Logger::kWarn>(msg); }, "msg"_a);
  m.def("log_error", [](const std::string& msg) { vlink::Logger::print<vlink::Logger::kError>(msg); }, "msg"_a);
  m.def("log_fatal", [](const std::string& msg) { vlink::Logger::print<vlink::Logger::kFatal>(msg); }, "msg"_a);
}

}  // namespace vlink::python
