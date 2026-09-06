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

#include <vlink/version.h>

#include "bindings.h"

NB_MODULE(_vlink_nanobind, m) {
  m.doc() = "VLink: Transport-agnostic pub/sub, field, and RPC communication middleware";

  vlink::python::bind_enums(m);
  vlink::python::bind_quantize(m);
  vlink::python::bind_bytes(m);
  vlink::python::bind_frame(m);
  vlink::python::bind_zerocopy(m);
  vlink::python::bind_metadata(m);
  vlink::python::bind_logging(m);
  vlink::python::bind_runtime(m);
  vlink::python::bind_process(m);
  vlink::python::bind_utils(m);
  vlink::python::bind_uuid(m);
  vlink::python::bind_helpers(m);
  vlink::python::bind_qos(m);
  vlink::python::bind_status(m);
  vlink::python::bind_security(m);
  vlink::python::bind_communication(m);
  vlink::python::bind_discovery(m);
  vlink::python::bind_bag(m);

  m.attr("VERSION") = VLINK_VERSION;
  m.attr("VERSION_MAJOR") = VLINK_VERSION_MAJOR;
  m.attr("VERSION_MINOR") = VLINK_VERSION_MINOR;
  m.attr("VERSION_PATCH") = VLINK_VERSION_PATCH;
}
