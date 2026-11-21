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

#include <vlink/base/message_loop.h>

#include "buffer.h"

namespace vlink::python {

inline void keep_alive_callback(nb::handle instance, void* data, void (*deleter)(void* payload) noexcept) {
#if NB_VERSION_MAJOR >= 3
  NB_CALL(keep_alive_ptr)(NB_CTX, instance.ptr(), data, deleter);
#else
  nb::detail::keep_alive(instance.ptr(), data, deleter);
#endif
}

void bind_python_instance_owner(nb::handle instance, nb::object owner);
void bind_python_bytes_owner(nb::handle instance, nb::object input, const vlink::Bytes& bytes);
void unbind_python_instance_owner(nb::handle instance);
void unbind_python_instance_owner_after_loop_idle(nb::handle instance, vlink::MessageLoop& loop) noexcept;

}  // namespace vlink::python
