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
#include <nanobind/stl/unordered_map.h>
#include <nanobind/stl/vector.h>
#include <vlink/base/process.h>

#include <stdexcept>

#include "buffer.h"
#include "callbacks.h"

namespace vlink::python {

using namespace nb::literals;  // NOLINT

struct PythonProcess final {
  PythonProcess() : activity(std::make_shared<PythonCallbackActivity>()), process(std::make_unique<vlink::Process>()) {}

  PythonProcess(const PythonProcess&) = delete;
  PythonProcess& operator=(const PythonProcess&) = delete;

  ~PythonProcess() { destroy_python_callback_owner(process, activity); }

  vlink::Process* operator->() const { return process.get(); }

  std::shared_ptr<PythonCallbackActivity> activity;
  std::unique_ptr<vlink::Process> process;
};

void bind_process(nb::module_& m) {
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
}

}  // namespace vlink::python
