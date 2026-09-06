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

#include <vlink/base/bytes.h>

#include "bindings.h"

namespace vlink::python {

class PythonBufferView {
 public:
  explicit PythonBufferView(nb::handle handle, int flags = PyBUF_SIMPLE) {
    if VUNLIKELY (PyObject_GetBuffer(handle.ptr(), &view_, flags) != 0) {
      throw nb::python_error();
    }
  }

  PythonBufferView(const PythonBufferView&) = delete;
  PythonBufferView& operator=(const PythonBufferView&) = delete;

  ~PythonBufferView() { PyBuffer_Release(&view_); }

  [[nodiscard]] const uint8_t* data() const { return static_cast<const uint8_t*>(view_.buf); }
  [[nodiscard]] size_t size() const { return static_cast<size_t>(view_.len); }

 private:
  Py_buffer view_{};
};

template <typename T>
struct PythonCodec {
  static T from_python_owned(nb::handle handle) { return nb::cast<T>(handle); }
  static nb::object to_python(const T& value) { return nb::cast(value); }
};

template <>
struct PythonCodec<vlink::Bytes> {
  static vlink::Bytes from_python_owned(nb::handle handle) {
    PythonBufferView view(handle);
    return vlink::Bytes::deep_copy(view.data(), view.size());
  }

  static nb::object to_python(const vlink::Bytes& value) { return nb::bytes(value.data(), value.size()); }
};

void pin_bytes_storage(const vlink::Bytes& bytes);
void unpin_bytes_storage(const vlink::Bytes& bytes) noexcept;
void ensure_bytes_not_exported(const vlink::Bytes& bytes);

}  // namespace vlink::python
