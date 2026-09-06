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

#include "buffer.h"

#include <nanobind/stl/string.h>
#include <nanobind/stl/vector.h>

#include <stdexcept>
#include <unordered_map>

namespace vlink::python {

using namespace nb::literals;  // NOLINT

static std::unordered_map<const vlink::Bytes*, size_t> bytes_export_counts;
static size_t bytes_export_total{0};

void pin_bytes_storage(const vlink::Bytes& bytes) {
  ++bytes_export_counts[&bytes];
  ++bytes_export_total;
}

void unpin_bytes_storage(const vlink::Bytes& bytes) noexcept {
  auto iter = bytes_export_counts.find(&bytes);

  if VUNLIKELY (iter == bytes_export_counts.end()) {
    return;
  }

  if (--iter->second == 0) {
    bytes_export_counts.erase(iter);
  }

  --bytes_export_total;
}

[[nodiscard]] static bool bytes_has_active_exports(const vlink::Bytes& bytes) {
  if VLIKELY (bytes_export_total == 0) {
    return false;
  }

  const auto iter = bytes_export_counts.find(&bytes);
  return iter != bytes_export_counts.end() && iter->second != 0;
}

void ensure_bytes_not_exported(const vlink::Bytes& bytes) {
  if VUNLIKELY (bytes_has_active_exports(bytes)) {
    PyErr_SetString(PyExc_BufferError, "Existing exports of data: Bytes object cannot be resized");
    throw nb::python_error();
  }
}

static int bytes_getbuffer(PyObject* obj, Py_buffer* view, int flags) {
  auto* bytes = nb::inst_ptr<vlink::Bytes>(nb::handle(obj));

  try {
    pin_bytes_storage(*bytes);
  } catch (const std::bad_alloc&) {
    PyErr_NoMemory();
    return -1;
  } catch (const std::exception& error) {
    PyErr_SetString(PyExc_RuntimeError, error.what());
    return -1;
  } catch (...) {
    PyErr_SetString(PyExc_RuntimeError, "Failed to pin Bytes storage");
    return -1;
  }

  const int result = PyBuffer_FillInfo(view, obj, bytes->data(), static_cast<Py_ssize_t>(bytes->size()), 0, flags);

  if VUNLIKELY (result != 0) {
    unpin_bytes_storage(*bytes);
  }

  return result;
}

static void bytes_releasebuffer(PyObject* obj, Py_buffer*) {
  auto* bytes = nb::inst_ptr<vlink::Bytes>(nb::handle(obj));
  unpin_bytes_storage(*bytes);
}

static PyType_Slot bytes_type_slots[] = {
    {Py_bf_getbuffer, reinterpret_cast<void*>(bytes_getbuffer)},
    {Py_bf_releasebuffer, reinterpret_cast<void*>(bytes_releasebuffer)},
    {0, nullptr},
};

void bind_bytes(nb::module_& m) {
  nb::class_<vlink::Bytes>(m, "Bytes", nb::type_slots(bytes_type_slots), "Versatile byte buffer")
      .def(nb::init<>())
      .def_static("init_memory_pool", &vlink::Bytes::init_memory_pool)
      .def_static("release_memory_pool", &vlink::Bytes::release_memory_pool)
      .def_static(
          "create", [](size_t size, uint8_t offset) { return vlink::Bytes::create(size, offset); }, "size"_a,
          "offset"_a = static_cast<uint8_t>(0))
      .def_static(
          "from_bytes",
          [](nb::handle data, uint8_t offset) {
            PythonBufferView view(data);
            return vlink::Bytes::deep_copy(view.data(), view.size(), offset);
          },
          "data"_a, "offset"_a = static_cast<uint8_t>(0))
      .def_static(
          "from_string", [](const std::string& s, uint8_t offset) { return vlink::Bytes::from_string(s, offset); },
          "s"_a, "offset"_a = static_cast<uint8_t>(0))
      .def_static(
          "from_user_input",
          [](const std::string& s) {
            bool ok = false;
            auto bytes = vlink::Bytes::from_user_input(s, &ok);

            if VUNLIKELY (!ok) {
              throw std::runtime_error("Invalid hex/binary input");
            }

            return bytes;
          },
          "hex_or_bin"_a)
      .def_static(
          "encode_to_base64",
          [](nb::handle data) {
            PythonBufferView view(data);
            auto b = vlink::Bytes::shallow_copy(view.data(), view.size());
            return vlink::Bytes::encode_to_base64(b);
          },
          "bytes"_a)
      .def_static("decode_from_base64", &vlink::Bytes::decode_from_base64, "str"_a)
      .def_static(
          "get_crc_32",
          [](nb::handle data) {
            PythonBufferView view(data);
            auto b = vlink::Bytes::shallow_copy(view.data(), view.size());
            return vlink::Bytes::get_crc_32(b);
          },
          "bytes"_a)
      .def_static(
          "get_crc_64",
          [](nb::handle data) {
            PythonBufferView view(data);
            auto b = vlink::Bytes::shallow_copy(view.data(), view.size());
            return vlink::Bytes::get_crc_64(b);
          },
          "bytes"_a)
      .def_static(
          "compress",
          [](nb::handle data, bool high_ratio) {
            PythonBufferView view(data);
            return vlink::Bytes::compress_data(view.data(), view.size(), high_ratio);
          },
          "bytes"_a, "high_ratio"_a = false)
      .def_static(
          "uncompress",
          [](nb::handle data, bool check_valid) {
            PythonBufferView view(data);
            return vlink::Bytes::uncompress_data(view.data(), view.size(), check_valid);
          },
          "bytes"_a, "check_valid"_a = true)
      .def_static(
          "is_compress_data",
          [](nb::handle data) {
            PythonBufferView view(data);
            return vlink::Bytes::is_compress_data(view.data(), view.size());
          },
          "bytes"_a)
      .def_static(
          "reverse_order",
          [](nb::handle data) {
            PythonBufferView view(data);
            auto b = vlink::Bytes::shallow_copy(view.data(), view.size());
            return vlink::Bytes::reverse_order(b);
          },
          "bytes"_a)
      .def_static(
          "convert_to_hex_str",
          [](nb::handle data) {
            PythonBufferView view(data);
            return vlink::Bytes::convert_to_hex_str(view.data(), view.size());
          },
          "bytes"_a)
      .def_static("is_little_endian", &vlink::Bytes::is_little_endian)
      .def_static("is_big_endian", &vlink::Bytes::is_big_endian)
      .def_static("stack_size", &vlink::Bytes::stack_size)
      .def("size", &vlink::Bytes::size)
      .def("real_size", &vlink::Bytes::real_size)
      .def("capacity", &vlink::Bytes::capacity)
      .def("offset", &vlink::Bytes::offset)
      .def("empty", &vlink::Bytes::empty)
      .def("is_owner", &vlink::Bytes::is_owner)
      .def("is_loaned", &vlink::Bytes::is_loaned)
      .def("is_ptr", &vlink::Bytes::is_ptr)
      .def("clear",
           [](vlink::Bytes& self) {
             ensure_bytes_not_exported(self);
             self.clear();
           })
      .def(
          "resize",
          [](vlink::Bytes& self, size_t size) {
            if VUNLIKELY (bytes_export_total != 0 && self.is_owner() && size != self.size()) {
              ensure_bytes_not_exported(self);
            }

            return self.resize(size);
          },
          "size"_a)
      .def(
          "reserve",
          [](vlink::Bytes& self, size_t capacity) {
            if VUNLIKELY (bytes_export_total != 0 && self.is_owner() && capacity > self.capacity()) {
              ensure_bytes_not_exported(self);
            }

            return self.reserve(capacity);
          },
          "capacity"_a)
      .def(
          "shrink_to",
          [](vlink::Bytes& self, size_t size) {
            if VUNLIKELY (bytes_export_total != 0 && self.is_owner() && size < self.size()) {
              ensure_bytes_not_exported(self);
            }

            return self.shrink_to(size);
          },
          "size"_a)
      .def(
          "deep_copy_self",
          [](vlink::Bytes& self) -> vlink::Bytes& {
            if VUNLIKELY (bytes_export_total != 0 && !self.is_owner() && !self.empty()) {
              ensure_bytes_not_exported(self);
            }

            return self.deep_copy_self();
          },
          nb::rv_policy::reference_internal)
      .def("to_bytes", [](const vlink::Bytes& self) { return nb::bytes(self.data(), self.size()); })
      .def("to_string", [](const vlink::Bytes& self) { return self.to_string(); })
      .def("to_raw_data", [](const vlink::Bytes& self) { return self.to_raw_data(); })
      .def("hex", [](const vlink::Bytes& self) { return vlink::Bytes::convert_to_hex_str(self.data(), self.size()); })
      .def("__len__", [](const vlink::Bytes& self) { return self.size(); })
      .def("__bool__", [](const vlink::Bytes& self) { return !self.empty(); })
      .def("__bytes__", [](const vlink::Bytes& self) { return nb::bytes(self.data(), self.size()); })
      .def("__eq__", [](const vlink::Bytes& a, const vlink::Bytes& b) { return a == b; })
      .def("__ne__", [](const vlink::Bytes& a, const vlink::Bytes& b) { return a != b; })
      .def("__getitem__",
           [](const vlink::Bytes& self, Py_ssize_t i) -> uint8_t {
             if (i < 0) {
               i += static_cast<Py_ssize_t>(self.size());
             }

             if VUNLIKELY (i < 0 || static_cast<size_t>(i) >= self.size()) {
               throw nb::index_error();
             }

             return self.data()[static_cast<size_t>(i)];
           })
      .def("__repr__", [](const vlink::Bytes& self) {
        std::string repr = "Bytes(size=" + std::to_string(self.size());

        if (self.is_owner()) {
          repr += ", owned";
        } else if (self.is_loaned()) {
          repr += ", loaned";
        } else {
          repr += ", shallow";
        }

        return repr + ")";
      });
}

}  // namespace vlink::python
