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

#pragma once

#include <nanobind/nanobind.h>

namespace vlink::python {

namespace nb = nanobind;

void bind_enums(nb::module_& m);
void bind_quantize(nb::module_& m);
void bind_bytes(nb::module_& m);
void bind_frame(nb::module_& m);
void bind_zerocopy(nb::module_& m);
void bind_metadata(nb::module_& m);
void bind_logging(nb::module_& m);
void bind_runtime(nb::module_& m);
void bind_process(nb::module_& m);
void bind_utils(nb::module_& m);
void bind_uuid(nb::module_& m);
void bind_helpers(nb::module_& m);
void bind_qos(nb::module_& m);
void bind_status(nb::module_& m);
void bind_security(nb::module_& m);
void bind_communication(nb::module_& m);
void bind_discovery(nb::module_& m);
void bind_bag(nb::module_& m);

}  // namespace vlink::python
