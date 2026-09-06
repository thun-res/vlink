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

#include <nanobind/stl/string.h>
#include <nanobind/stl/vector.h>
#include <vlink/base/helpers.h>
#include <vlink/base/quantize.h>
#include <vlink/base/utils.h>

#include <csignal>
#include <filesystem>
#include <limits>
#include <stdexcept>

#include "callbacks.h"

namespace vlink::python {

using namespace nb::literals;  // NOLINT

static nb::str wide_string_to_python_str(const std::wstring& value) {
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

static std::wstring python_str_to_wide_string(nb::str input) {
  Py_ssize_t size = 0;
  wchar_t* raw = PyUnicode_AsWideCharString(input.ptr(), &size);

  if VUNLIKELY (!raw) {
    throw nb::python_error();
  }

  std::unique_ptr<wchar_t, PyWideStringDeleter> wide(raw);
  return std::wstring(wide.get(), static_cast<size_t>(size));
}

static const void* python_keyboard_callback_owner() noexcept {
  static const char kOwner{};
  return &kOwner;
}

void bind_quantize(nb::module_& m) {
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
}

void bind_utils(nb::module_& m) {
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
}

void bind_helpers(nb::module_& m) {
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
}

}  // namespace vlink::python
