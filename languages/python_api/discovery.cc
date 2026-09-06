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
#include <vlink/base/timer.h>
#include <vlink/extension/discovery_viewer.h>
#include <vlink/extension/url_remap.h>

#include "callbacks.h"

namespace vlink::python {

using namespace nb::literals;  // NOLINT

void bind_discovery(nb::module_& m) {
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

  nb::class_<vlink::DiscoveryViewer, vlink::MessageLoop> dv(m, "DiscoveryViewer", "Active endpoint discovery",
                                                            nb::is_weak_referenceable());
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
}

}  // namespace vlink::python
