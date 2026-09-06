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
#include <nanobind/stl/string_view.h>
#include <nanobind/stl/vector.h>
#include <vlink/zerocopy/audio_frame.h>
#include <vlink/zerocopy/camera_frame.h>
#include <vlink/zerocopy/header.h>
#include <vlink/zerocopy/message_parser.h>
#include <vlink/zerocopy/object_array.h>
#include <vlink/zerocopy/occupancy_grid.h>
#include <vlink/zerocopy/point_cloud.h>
#include <vlink/zerocopy/proxy_data.h>
#include <vlink/zerocopy/raw_data.h>
#include <vlink/zerocopy/tensor.h>

#include <algorithm>
#include <cstring>
#include <limits>
#include <type_traits>
#include <unordered_map>
#include <utility>

#include "bindings.h"
#include "ownership.h"
#include "strings.h"

namespace vlink::python {

using namespace nb::literals;  // NOLINT

static std::unordered_map<const vlink::zerocopy::ProxyData*, size_t> proxy_export_counts;

static void unpin_proxy_storage(void* data) noexcept {
  auto iter = proxy_export_counts.find(static_cast<vlink::zerocopy::ProxyData*>(data));

  if (--iter->second == 0) {
    proxy_export_counts.erase(iter);
  }
}

static void ensure_proxy_not_exported(const vlink::zerocopy::ProxyData& proxy) {
  if VUNLIKELY (proxy_export_counts.find(&proxy) != proxy_export_counts.end()) {
    PyErr_SetString(PyExc_BufferError, "Existing raw views: ProxyData storage cannot be replaced");
    throw nb::python_error();
  }
}

struct PythonZerocopyMessageParser final {
  using Parser = vlink::zerocopy::MessageParser;

  bool parse(std::string_view serialized_type, nb::object input) {
    return this->parse_input(input, [&serialized_type](Parser& parser, const vlink::Bytes& bytes) {
      return parser.parse(serialized_type, bytes);
    });
  }

  bool parse_type(Parser::Type type, nb::object input) {
    return this->parse_input(input,
                             [type](Parser& parser, const vlink::Bytes& bytes) { return parser.parse(type, bytes); });
  }

  void clear() {
    parser.clear();
    input_owner = nb::object();
    input_data = nullptr;
    input_size = 0;
  }

  [[nodiscard]] bool backing_valid() const {
    if (!input_owner.is_valid()) {
      return false;
    }

    const auto& bytes = nb::cast<const vlink::Bytes&>(input_owner);

    if (bytes.data() != input_data || bytes.size() != input_size) {
      return false;
    }

    return parser.valid();
  }

  template <typename ParseFunction>
  bool parse_input(const nb::object& input, ParseFunction&& parse_function) {
    const auto& bytes = nb::cast<const vlink::Bytes&>(input);

    if VUNLIKELY (!parse_function(parser, bytes)) {
      input_owner = nb::object();
      input_data = nullptr;
      input_size = 0;
      return false;
    }

    input_owner = input;
    input_data = bytes.data();
    input_size = bytes.size();
    return true;
  }

  Parser parser;
  nb::object input_owner;
  const uint8_t* input_data{nullptr};
  size_t input_size{0};
};

template <typename MessageT, typename = void>
struct HasInternalData : std::false_type {};

template <typename MessageT>
struct HasInternalData<MessageT, std::void_t<decltype(std::declval<const MessageT&>().get_internal_data())>>
    : std::true_type {};

template <typename MessageT, typename = void>
struct HasData : std::false_type {};

template <typename MessageT>
struct HasData<MessageT, std::void_t<decltype(std::declval<const MessageT&>().data())>> : std::true_type {};

template <typename MessageT, typename = void>
struct HasFullClear : std::false_type {};

template <typename MessageT>
struct HasFullClear<MessageT, std::void_t<decltype(std::declval<MessageT&>().clear(true))>> : std::true_type {};

template <typename MessageT>
static const uint8_t* zerocopy_payload_address(const MessageT& message) noexcept {
  if constexpr (HasInternalData<MessageT>::value) {
    return message.get_internal_data();
  } else if constexpr (HasData<MessageT>::value) {
    return message.data();
  } else {
    const vlink::Bytes raw = message.raw();

    if (raw.data() != nullptr) {
      return raw.data();
    }

    for (const std::string_view field : {message.url(), message.ser(), message.hostname()}) {
      if (field.data() != nullptr) {
        return reinterpret_cast<const uint8_t*>(field.data());
      }
    }

    return nullptr;
  }
}

template <typename MessageT>
static void zerocopy_full_clear(MessageT& target) noexcept {
  if constexpr (HasFullClear<MessageT>::value) {
    target.clear(true);
  } else {
    target.clear();
  }
}

template <typename MessageT>
static bool zerocopy_create(nb::object instance, size_t size) {
  const bool result = nb::cast<MessageT&>(instance).create(size);

  if (result) {
    unbind_python_instance_owner(instance);
  }

  return result;
}

template <typename MessageT>
static void zerocopy_clear(nb::object instance) {
  auto& self = nb::cast<MessageT&>(instance);

  if constexpr (std::is_same_v<MessageT, vlink::zerocopy::ProxyData>) {
    ensure_proxy_not_exported(self);
  }

  self.clear();
  unbind_python_instance_owner(instance);
}

template <typename MessageT>
static bool zerocopy_fill_data(nb::object instance, nb::handle data) {
  PythonBufferView view(data);
  const bool result = nb::cast<MessageT&>(instance).fill_data(const_cast<uint8_t*>(view.data()), view.size());

  if (result) {
    unbind_python_instance_owner(instance);
  }

  return result;
}

template <typename MessageT>
static bool zerocopy_from_bytes(MessageT& target, const vlink::Bytes& bytes) {
  if constexpr (std::is_same_v<MessageT, vlink::zerocopy::ProxyData>) {
    ensure_proxy_not_exported(target);
  }

  const uint8_t* previous_payload = target.is_owner() ? nullptr : zerocopy_payload_address(target);
  const uint8_t* input_data = bytes.data();
  const auto input_begin = reinterpret_cast<uintptr_t>(input_data);
  const auto uses_input_storage = [&](const uint8_t* payload) {
    const auto payload_address = reinterpret_cast<uintptr_t>(payload);
    return payload != nullptr && input_data != nullptr && payload_address >= input_begin &&
           payload_address - input_begin < bytes.size();
  };
  const bool reuses_input_storage = uses_input_storage(previous_payload);

  const auto bind_input = [&]() {
    try {
      nb::object instance = nb::find(&target);
      nb::object input = nb::find(const_cast<vlink::Bytes*>(&bytes));
      bind_python_bytes_owner(instance, std::move(input), bytes);
    } catch (...) {
      zerocopy_full_clear(target);
      throw;
    }
  };

  if VUNLIKELY (!(target << bytes)) {
    const uint8_t* current_payload = target.is_owner() ? nullptr : zerocopy_payload_address(target);

    if (target.is_owner() || current_payload == nullptr) {
      nb::object instance = nb::find(&target);
      unbind_python_instance_owner(instance);
    } else if (uses_input_storage(current_payload)) {
      if (!reuses_input_storage) {
        bind_input();
      }
    } else if (current_payload != previous_payload) {
      nb::object instance = nb::find(&target);
      unbind_python_instance_owner(instance);
    }

    return false;
  }

  if (target.is_owner()) {
    nb::object instance = nb::find(&target);
    unbind_python_instance_owner(instance);
  } else if (!reuses_input_storage) {
    bind_input();
  }

  return true;
}

template <typename MessageT>
static bool zerocopy_deep_copy(nb::object instance, const MessageT& source) {
  auto& target = nb::cast<MessageT&>(instance);
  const bool result = target.deep_copy(source);

  if (target.is_owner() || zerocopy_payload_address(target) == nullptr) {
    unbind_python_instance_owner(instance);
  }

  return result;
}

void bind_zerocopy(nb::module_& m) {
  nb::class_<vlink::zerocopy::Header>(m, "ZeroCopyHeader", "Common timestamp and sequencing metadata header (40 bytes)")
      .def(nb::init<>())
      .def_prop_rw(
          "frame_id", [](const vlink::zerocopy::Header& self) { return std::string(self.frame_id_view()); },
          [](vlink::zerocopy::Header& self, const std::string& s) {
            constexpr size_t kMax = sizeof(vlink::zerocopy::Header::frame_id) - 1;
            const auto prefix = utf8_prefix(s, kMax);
            std::memset(self.frame_id, 0, sizeof(self.frame_id));
            std::memcpy(self.frame_id, prefix.data(), prefix.size());
          })
      .def_rw("seq", &vlink::zerocopy::Header::seq)
      .def_rw("reserved", &vlink::zerocopy::Header::reserved)
      .def_rw("time_meas", &vlink::zerocopy::Header::time_meas)
      .def_rw("time_pub", &vlink::zerocopy::Header::time_pub)
      .def("__repr__", [](const vlink::zerocopy::Header& self) {
        return std::string("ZeroCopyHeader(frame_id='") + std::string(self.frame_id_view()) +
               "', seq=" + std::to_string(self.seq) + ")";
      });

  using ZerocopyMessageParser = vlink::zerocopy::MessageParser;
  using PythonMessageParser = PythonZerocopyMessageParser;
  nb::class_<PythonMessageParser> message_parser_cls(m, "ZeroCopyMessageParser",
                                                     "Unified read-only parser for VLink zero-copy messages");
  nb::enum_<ZerocopyMessageParser::Type>(message_parser_cls, "Type")
      .value("Unknown", ZerocopyMessageParser::Type::kUnknown)
      .value("RawData", ZerocopyMessageParser::Type::kRawData)
      .value("CameraFrame", ZerocopyMessageParser::Type::kCameraFrame)
      .value("PointCloud", ZerocopyMessageParser::Type::kPointCloud)
      .value("ProxyData", ZerocopyMessageParser::Type::kProxyData)
      .value("OccupancyGrid", ZerocopyMessageParser::Type::kOccupancyGrid)
      .value("Tensor", ZerocopyMessageParser::Type::kTensor)
      .value("ObjectArray", ZerocopyMessageParser::Type::kObjectArray)
      .value("AudioFrame", ZerocopyMessageParser::Type::kAudioFrame);
  nb::enum_<ZerocopyMessageParser::ValueType>(message_parser_cls, "ValueType")
      .value("Unknown", ZerocopyMessageParser::ValueType::kValueUnknown)
      .value("Int64", ZerocopyMessageParser::ValueType::kInt64)
      .value("Uint64", ZerocopyMessageParser::ValueType::kUInt64)
      .value("Double", ZerocopyMessageParser::ValueType::kDouble)
      .value("String", ZerocopyMessageParser::ValueType::kString)
      .value("Bytes", ZerocopyMessageParser::ValueType::kBytes);
  nb::enum_<ZerocopyMessageParser::EnumKind>(message_parser_cls, "EnumKind")
      .value("NoEnum", ZerocopyMessageParser::EnumKind::kEnumNone)
      .value("CameraFormat", ZerocopyMessageParser::EnumKind::kEnumCameraFormat)
      .value("CameraStream", ZerocopyMessageParser::EnumKind::kEnumCameraStream)
      .value("GridCellType", ZerocopyMessageParser::EnumKind::kEnumGridCellType)
      .value("TensorDataType", ZerocopyMessageParser::EnumKind::kEnumTensorDataType)
      .value("TensorDevice", ZerocopyMessageParser::EnumKind::kEnumTensorDevice)
      .value("AudioFormat", ZerocopyMessageParser::EnumKind::kEnumAudioFormat)
      .value("AudioLayout", ZerocopyMessageParser::EnumKind::kEnumAudioLayout);
  nb::class_<ZerocopyMessageParser::Field>(message_parser_cls, "Field")
      .def_ro("name", &ZerocopyMessageParser::Field::name)
      .def_ro("type", &ZerocopyMessageParser::Field::type)
      .def_ro("native_type", &ZerocopyMessageParser::Field::native_type)
      .def_ro("storage_size", &ZerocopyMessageParser::Field::storage_size)
      .def_ro("enum_kind", &ZerocopyMessageParser::Field::enum_kind)
      .def_ro("is_time", &ZerocopyMessageParser::Field::is_time)
      .def_ro("is_bool", &ZerocopyMessageParser::Field::is_bool)
      .def_ro("is_reserved", &ZerocopyMessageParser::Field::is_reserved)
      .def_ro("byte_offset", &ZerocopyMessageParser::Field::byte_offset)
      .def_ro("element_index", &ZerocopyMessageParser::Field::element_index);

  const auto parser_value_to_python = [](const ZerocopyMessageParser::Value& value) -> nb::object {
    return std::visit(
        [](const auto& item) -> nb::object {
          using Item = std::decay_t<decltype(item)>;

          if constexpr (std::is_same_v<Item, vlink::Bytes>) {
            return nb::bytes(item.data(), item.size());
          } else {
            return nb::cast(item);
          }
        },
        value);
  };

  message_parser_cls.def(nb::init<>())
      .def("parse", &PythonMessageParser::parse, "serialized_type"_a, "bytes"_a)
      .def("parse_type", &PythonMessageParser::parse_type, "type"_a, "bytes"_a)
      .def("clear", &PythonMessageParser::clear)
      .def_prop_ro("type",
                   [](const PythonMessageParser& self) {
                     return self.backing_valid() ? self.parser.type() : ZerocopyMessageParser::Type::kUnknown;
                   })
      .def_prop_ro("valid", &PythonMessageParser::backing_valid)
      .def(
          "value",
          [parser_value_to_python](const PythonMessageParser& self, std::string_view path) -> nb::object {
            ZerocopyMessageParser::Value value;

            if VUNLIKELY (!self.backing_valid() || !self.parser.value(path, value)) {
              return nb::none();
            }

            return parser_value_to_python(value);
          },
          "path"_a)
      .def(
          "value_at",
          [parser_value_to_python](const PythonMessageParser& self, std::string_view collection, size_t index,
                                   std::string_view field) -> nb::object {
            ZerocopyMessageParser::Value value;

            if VUNLIKELY (!self.backing_valid() || !self.parser.value(collection, index, field, value)) {
              return nb::none();
            }

            return parser_value_to_python(value);
          },
          "collection"_a, "index"_a, "field"_a)
      .def(
          "collection_size",
          [](const PythonMessageParser& self, std::string_view collection) {
            return self.backing_valid() ? self.parser.collection_size(collection) : 0;
          },
          "collection"_a)
      .def("fields",
           [](const PythonMessageParser& self) {
             return self.backing_valid() ? self.parser.fields() : std::vector<ZerocopyMessageParser::Field>{};
           })
      .def(
          "element_fields",
          [](const PythonMessageParser& self, std::string_view collection) {
            return self.backing_valid() ? self.parser.element_fields(collection)
                                        : std::vector<ZerocopyMessageParser::Field>{};
          },
          "collection"_a)
      .def_static("detect_type", &ZerocopyMessageParser::detect_type, "serialized_type"_a)
      .def_static(
          "type_name",
          [](ZerocopyMessageParser::Type type) { return std::string(ZerocopyMessageParser::type_name(type)); },
          "type"_a);

  nb::class_<vlink::zerocopy::RawData>(m, "RawData", "Generic zero-copy raw-byte data container (64 bytes)")
      .def(nb::init<>())
      .def_rw("header", &vlink::zerocopy::RawData::header)
      .def("create", &zerocopy_create<vlink::zerocopy::RawData>, "size"_a)
      .def("clear", &zerocopy_clear<vlink::zerocopy::RawData>)
      .def("size", &vlink::zerocopy::RawData::size)
      .def("is_valid", &vlink::zerocopy::RawData::is_valid)
      .def("is_owner", &vlink::zerocopy::RawData::is_owner)
      .def("get_serialized_size", &vlink::zerocopy::RawData::get_serialized_size)
      .def_static("check_valid", &vlink::zerocopy::RawData::check_valid, "bytes"_a)
      .def_prop_rw(
          "reserved", [](vlink::zerocopy::RawData& self) { return self.get_reserved(); },
          [](vlink::zerocopy::RawData& self, uint16_t v) { self.get_reserved() = v; })
      .def("data", [](const vlink::zerocopy::RawData& self) { return nb::bytes(self.data(), self.size()); })
      .def("fill_data", &zerocopy_fill_data<vlink::zerocopy::RawData>, "data"_a)
      .def("to_bytes",
           [](const vlink::zerocopy::RawData& self) {
             vlink::Bytes b;
             self >> b;
             return b;
           })
      .def("from_bytes", &zerocopy_from_bytes<vlink::zerocopy::RawData>, "bytes"_a)
      .def("__repr__", [](const vlink::zerocopy::RawData& self) {
        return std::string("RawData(size=") + std::to_string(self.size()) + ")";
      });

  nb::class_<vlink::zerocopy::CameraFrame> camera_frame_cls(m, "CameraFrame",
                                                            "Zero-copy camera image frame (80 bytes)");
  nb::enum_<vlink::zerocopy::CameraFrame::Format>(camera_frame_cls, "Format")
      .value("Unknown", vlink::zerocopy::CameraFrame::kFormatUnknown)
      .value("Yuv420", vlink::zerocopy::CameraFrame::kFormatYuv420)
      .value("Yuv422", vlink::zerocopy::CameraFrame::kFormatYuv422)
      .value("Yuv444", vlink::zerocopy::CameraFrame::kFormatYuv444)
      .value("Nv12", vlink::zerocopy::CameraFrame::kFormatNv12)
      .value("Nv21", vlink::zerocopy::CameraFrame::kFormatNv21)
      .value("Yuyv", vlink::zerocopy::CameraFrame::kFormatYuyv)
      .value("Yvyu", vlink::zerocopy::CameraFrame::kFormatYvyu)
      .value("Uyvy", vlink::zerocopy::CameraFrame::kFormatUyvy)
      .value("Vyuy", vlink::zerocopy::CameraFrame::kFormatVyuy)
      .value("Bgr888Packed", vlink::zerocopy::CameraFrame::kFormatBgr888Packed)
      .value("Rgb888Packed", vlink::zerocopy::CameraFrame::kFormatRgb888Packed)
      .value("Rgb888Planar", vlink::zerocopy::CameraFrame::kFormatRgb888Planar)
      .value("Mono8", vlink::zerocopy::CameraFrame::kFormatMono8)
      .value("Mono16", vlink::zerocopy::CameraFrame::kFormatMono16)
      .value("Rgba8888Packed", vlink::zerocopy::CameraFrame::kFormatRgba8888Packed)
      .value("Bgra8888Packed", vlink::zerocopy::CameraFrame::kFormatBgra8888Packed)
      .value("Uint8C1", vlink::zerocopy::CameraFrame::kFormatUint8C1)
      .value("Uint8C2", vlink::zerocopy::CameraFrame::kFormatUint8C2)
      .value("Uint8C3", vlink::zerocopy::CameraFrame::kFormatUint8C3)
      .value("Uint8C4", vlink::zerocopy::CameraFrame::kFormatUint8C4)
      .value("Int8C1", vlink::zerocopy::CameraFrame::kFormatInt8C1)
      .value("Int8C2", vlink::zerocopy::CameraFrame::kFormatInt8C2)
      .value("Int8C3", vlink::zerocopy::CameraFrame::kFormatInt8C3)
      .value("Int8C4", vlink::zerocopy::CameraFrame::kFormatInt8C4)
      .value("Uint16C1", vlink::zerocopy::CameraFrame::kFormatUint16C1)
      .value("Uint16C2", vlink::zerocopy::CameraFrame::kFormatUint16C2)
      .value("Uint16C3", vlink::zerocopy::CameraFrame::kFormatUint16C3)
      .value("Uint16C4", vlink::zerocopy::CameraFrame::kFormatUint16C4)
      .value("Int16C1", vlink::zerocopy::CameraFrame::kFormatInt16C1)
      .value("Int16C2", vlink::zerocopy::CameraFrame::kFormatInt16C2)
      .value("Int16C3", vlink::zerocopy::CameraFrame::kFormatInt16C3)
      .value("Int16C4", vlink::zerocopy::CameraFrame::kFormatInt16C4)
      .value("Int32C1", vlink::zerocopy::CameraFrame::kFormatInt32C1)
      .value("Int32C2", vlink::zerocopy::CameraFrame::kFormatInt32C2)
      .value("Int32C3", vlink::zerocopy::CameraFrame::kFormatInt32C3)
      .value("Int32C4", vlink::zerocopy::CameraFrame::kFormatInt32C4)
      .value("Float32C1", vlink::zerocopy::CameraFrame::kFormatFloat32C1)
      .value("Float32C2", vlink::zerocopy::CameraFrame::kFormatFloat32C2)
      .value("Float32C3", vlink::zerocopy::CameraFrame::kFormatFloat32C3)
      .value("Float32C4", vlink::zerocopy::CameraFrame::kFormatFloat32C4)
      .value("Float64C1", vlink::zerocopy::CameraFrame::kFormatFloat64C1)
      .value("Float64C2", vlink::zerocopy::CameraFrame::kFormatFloat64C2)
      .value("Float64C3", vlink::zerocopy::CameraFrame::kFormatFloat64C3)
      .value("Float64C4", vlink::zerocopy::CameraFrame::kFormatFloat64C4)
      .value("BayerRggb8", vlink::zerocopy::CameraFrame::kFormatBayerRggb8)
      .value("BayerBggr8", vlink::zerocopy::CameraFrame::kFormatBayerBggr8)
      .value("BayerGbrg8", vlink::zerocopy::CameraFrame::kFormatBayerGbrg8)
      .value("BayerGrbg8", vlink::zerocopy::CameraFrame::kFormatBayerGrbg8)
      .value("BayerRggb16", vlink::zerocopy::CameraFrame::kFormatBayerRggb16)
      .value("BayerBggr16", vlink::zerocopy::CameraFrame::kFormatBayerBggr16)
      .value("BayerGbrg16", vlink::zerocopy::CameraFrame::kFormatBayerGbrg16)
      .value("BayerGrbg16", vlink::zerocopy::CameraFrame::kFormatBayerGrbg16)
      .value("Jpeg", vlink::zerocopy::CameraFrame::kFormatJpeg)
      .value("H264", vlink::zerocopy::CameraFrame::kFormatH264)
      .value("H265", vlink::zerocopy::CameraFrame::kFormatH265)
      .value("Png", vlink::zerocopy::CameraFrame::kFormatPng)
      .value("Mjpeg", vlink::zerocopy::CameraFrame::kFormatMjpeg)
      .value("H266", vlink::zerocopy::CameraFrame::kFormatH266)
      .value("Av1", vlink::zerocopy::CameraFrame::kFormatAv1)
      .value("Webp", vlink::zerocopy::CameraFrame::kFormatWebp);
  nb::enum_<vlink::zerocopy::CameraFrame::Stream>(camera_frame_cls, "Stream")
      .value("Unknown", vlink::zerocopy::CameraFrame::kStreamUnknown)
      .value("I", vlink::zerocopy::CameraFrame::kStreamI)
      .value("P", vlink::zerocopy::CameraFrame::kStreamP)
      .value("B", vlink::zerocopy::CameraFrame::kStreamB);
  camera_frame_cls.def(nb::init<>())
      .def_rw("header", &vlink::zerocopy::CameraFrame::header)
      .def("create", &zerocopy_create<vlink::zerocopy::CameraFrame>, "size"_a)
      .def("clear", &zerocopy_clear<vlink::zerocopy::CameraFrame>)
      .def("size", &vlink::zerocopy::CameraFrame::size)
      .def("is_valid", &vlink::zerocopy::CameraFrame::is_valid)
      .def("is_owner", &vlink::zerocopy::CameraFrame::is_owner)
      .def("get_serialized_size", &vlink::zerocopy::CameraFrame::get_serialized_size)
      .def_static("check_valid", &vlink::zerocopy::CameraFrame::check_valid, "bytes"_a)
      .def("channel", &vlink::zerocopy::CameraFrame::channel)
      .def("width", &vlink::zerocopy::CameraFrame::width)
      .def("height", &vlink::zerocopy::CameraFrame::height)
      .def("freq", &vlink::zerocopy::CameraFrame::freq)
      .def("format", &vlink::zerocopy::CameraFrame::format)
      .def("stream", &vlink::zerocopy::CameraFrame::stream)
      .def_static("format_from_encoding", &vlink::zerocopy::CameraFrame::format_from_encoding, "encoding"_a)
      .def_static(
          "encoding_from_format",
          [](vlink::zerocopy::CameraFrame::Format format) {
            return std::string(vlink::zerocopy::CameraFrame::encoding_from_format(format));
          },
          "format"_a)
      .def("set_channel", &vlink::zerocopy::CameraFrame::set_channel, "channel"_a)
      .def("set_width", &vlink::zerocopy::CameraFrame::set_width, "width"_a)
      .def("set_height", &vlink::zerocopy::CameraFrame::set_height, "height"_a)
      .def("set_freq", &vlink::zerocopy::CameraFrame::set_freq, "freq"_a)
      .def("set_format", &vlink::zerocopy::CameraFrame::set_format, "format"_a)
      .def("set_stream", &vlink::zerocopy::CameraFrame::set_stream, "stream"_a)
      .def_prop_rw(
          "reserved", [](vlink::zerocopy::CameraFrame& self) { return self.get_reserved(); },
          [](vlink::zerocopy::CameraFrame& self, uint32_t v) { self.get_reserved() = v; })
      .def("data", [](const vlink::zerocopy::CameraFrame& self) { return nb::bytes(self.data(), self.size()); })
      .def("fill_data", &zerocopy_fill_data<vlink::zerocopy::CameraFrame>, "data"_a)
      .def("to_bytes",
           [](const vlink::zerocopy::CameraFrame& self) {
             vlink::Bytes b;
             self >> b;
             return b;
           })
      .def("from_bytes", &zerocopy_from_bytes<vlink::zerocopy::CameraFrame>, "bytes"_a)
      .def("__repr__", [](const vlink::zerocopy::CameraFrame& self) {
        return std::string("CameraFrame(") + std::to_string(self.width()) + "x" + std::to_string(self.height()) +
               ", size=" + std::to_string(self.size()) + ")";
      });

  nb::class_<vlink::zerocopy::PointCloud> point_cloud_cls(m, "PointCloud",
                                                          "Schema-aware zero-copy 3-D point cloud (256 bytes)");
  nb::enum_<vlink::zerocopy::PointCloud::Type>(point_cloud_cls, "Type")
      .value("Unknown", vlink::zerocopy::PointCloud::kUnknownType)
      .value("Bool", vlink::zerocopy::PointCloud::kBoolType)
      .value("Int8", vlink::zerocopy::PointCloud::kInt8Type)
      .value("Uint8", vlink::zerocopy::PointCloud::kUint8Type)
      .value("Int16", vlink::zerocopy::PointCloud::kInt16Type)
      .value("Uint16", vlink::zerocopy::PointCloud::kUint16Type)
      .value("Int32", vlink::zerocopy::PointCloud::kInt32Type)
      .value("Uint32", vlink::zerocopy::PointCloud::kUint32Type)
      .value("Int64", vlink::zerocopy::PointCloud::kInt64Type)
      .value("Uint64", vlink::zerocopy::PointCloud::kUint64Type)
      .value("Float", vlink::zerocopy::PointCloud::kFloatType)
      .value("Double", vlink::zerocopy::PointCloud::kDoubleType);
  nb::class_<vlink::zerocopy::PointCloud::Key>(point_cloud_cls, "Key")
      .def(nb::init<>())
      .def_rw("name", &vlink::zerocopy::PointCloud::Key::name)
      .def_prop_rw(
          "type",
          [](const vlink::zerocopy::PointCloud::Key& self) {
            if (self.type > static_cast<uint8_t>(vlink::zerocopy::PointCloud::kDoubleType)) {
              return vlink::zerocopy::PointCloud::kUnknownType;
            }

            return static_cast<vlink::zerocopy::PointCloud::Type>(self.type);
          },
          [](vlink::zerocopy::PointCloud::Key& self, vlink::zerocopy::PointCloud::Type type) {
            self.type = static_cast<uint8_t>(type);
          })
      .def_rw("size", &vlink::zerocopy::PointCloud::Key::size);
  nb::class_<vlink::zerocopy::PointCloud::Vector3f>(point_cloud_cls, "Vector3f")
      .def(nb::init<>())
      .def(nb::init<float, float, float>(), "x"_a, "y"_a, "z"_a)
      .def_rw("x", &vlink::zerocopy::PointCloud::Vector3f::x)
      .def_rw("y", &vlink::zerocopy::PointCloud::Vector3f::y)
      .def_rw("z", &vlink::zerocopy::PointCloud::Vector3f::z)
      .def("__repr__", [](const vlink::zerocopy::PointCloud::Vector3f& v) {
        return std::string("Vector3f(") + std::to_string(v.x) + ", " + std::to_string(v.y) + ", " +
               std::to_string(v.z) + ")";
      });
  nb::class_<vlink::zerocopy::PointCloud::Vector3d>(point_cloud_cls, "Vector3d")
      .def(nb::init<>())
      .def(nb::init<double, double, double>(), "x"_a, "y"_a, "z"_a)
      .def_rw("x", &vlink::zerocopy::PointCloud::Vector3d::x)
      .def_rw("y", &vlink::zerocopy::PointCloud::Vector3d::y)
      .def_rw("z", &vlink::zerocopy::PointCloud::Vector3d::z)
      .def("__repr__", [](const vlink::zerocopy::PointCloud::Vector3d& v) {
        return std::string("Vector3d(") + std::to_string(v.x) + ", " + std::to_string(v.y) + ", " +
               std::to_string(v.z) + ")";
      });
  point_cloud_cls.def(nb::init<>())
      .def("deep_copy", &zerocopy_deep_copy<vlink::zerocopy::PointCloud>, "source"_a,
           "Copy protocol, metadata and points into independent storage.")
      .def_rw("header", &vlink::zerocopy::PointCloud::header)
      .def(
          "create",
          [](nb::object instance, size_t size, uint64_t size_num, uint64_t type_num, const std::string& key_str,
             uint16_t extent, bool vertical) {
            const bool result = nb::cast<vlink::zerocopy::PointCloud&>(instance).create(size, size_num, type_num,
                                                                                        key_str, extent, vertical);

            if (result) {
              unbind_python_instance_owner(instance);
            }

            return result;
          },
          "size"_a, "size_num"_a, "type_num"_a, "key_str"_a, "extent"_a = static_cast<uint16_t>(0),
          "vertical"_a = false)
      .def(
          "fill_packed_data",
          [](vlink::zerocopy::PointCloud& self, nb::handle data, size_t count) {
            PythonBufferView view(data);
            const size_t pack_size = self.pack_size();

            if VUNLIKELY (pack_size != 0 && count > std::numeric_limits<size_t>::max() / pack_size) {
              throw nb::value_error("count * pack_size overflows size_t");
            }

            const size_t required_size = count * pack_size;

            if VUNLIKELY (view.size() < required_size) {
              throw nb::value_error("input buffer is smaller than count * pack_size");
            }

            return self.fill_packed_data(view.data(), count);
          },
          "data"_a, "count"_a)
      .def(
          "get_value_v3f",
          [](const vlink::zerocopy::PointCloud& self, size_t loop_index) { return self.get_value_v3f(loop_index); },
          "loop_index"_a)
      .def(
          "get_value_v3d",
          [](const vlink::zerocopy::PointCloud& self, size_t loop_index) { return self.get_value_v3d(loop_index); },
          "loop_index"_a)
      .def(
          "push_value_v3f",
          [](vlink::zerocopy::PointCloud& self, float x, float y, float z) { return self.push_value_v3f(x, y, z); },
          "x"_a, "y"_a, "z"_a)
      .def(
          "push_value_v3d",
          [](vlink::zerocopy::PointCloud& self, double x, double y, double z) { return self.push_value_v3d(x, y, z); },
          "x"_a, "y"_a, "z"_a)
      .def(
          "set_value_v3f",
          [](vlink::zerocopy::PointCloud& self, size_t loop_index, float x, float y, float z) {
            return self.set_value_v3f(loop_index, x, y, z);
          },
          "loop_index"_a, "x"_a, "y"_a, "z"_a)
      .def(
          "set_value_v3d",
          [](vlink::zerocopy::PointCloud& self, size_t loop_index, double x, double y, double z) {
            return self.set_value_v3d(loop_index, x, y, z);
          },
          "loop_index"_a, "x"_a, "y"_a, "z"_a)
      .def("resize", &vlink::zerocopy::PointCloud::resize, "size"_a)
      .def(
          "clear",
          [](nb::object instance, bool force) {
            nb::cast<vlink::zerocopy::PointCloud&>(instance).clear(force);

            if (force) {
              unbind_python_instance_owner(instance);
            }
          },
          "force"_a = false)
      .def("downsample", &vlink::zerocopy::PointCloud::downsample, "level"_a)
      .def("get_extent", &vlink::zerocopy::PointCloud::get_extent)
      .def("get_vertical", &vlink::zerocopy::PointCloud::get_vertical)
      .def("set_vertical", &vlink::zerocopy::PointCloud::set_vertical, "vertical"_a)
      .def("get_downsample", &vlink::zerocopy::PointCloud::get_downsample)
      .def_prop_rw(
          "reserved", [](vlink::zerocopy::PointCloud& self) { return self.get_reserved(); },
          [](vlink::zerocopy::PointCloud& self, uint32_t v) { self.get_reserved() = v; })
      .def_prop_rw(
          "reserved2", [](vlink::zerocopy::PointCloud& self) { return self.get_reserved2(); },
          [](vlink::zerocopy::PointCloud& self, uint32_t v) { self.get_reserved2() = v; })
      .def_prop_rw(
          "reserved3", [](vlink::zerocopy::PointCloud& self) { return self.get_reserved3(); },
          [](vlink::zerocopy::PointCloud& self, uint8_t v) { self.get_reserved3() = v; })
      .def("size", &vlink::zerocopy::PointCloud::size)
      .def("pack_size", &vlink::zerocopy::PointCloud::pack_size)
      .def("get_reserved_size", &vlink::zerocopy::PointCloud::get_reserved_size)
      .def("is_owner", &vlink::zerocopy::PointCloud::is_owner)
      .def("is_valid", &vlink::zerocopy::PointCloud::is_valid)
      .def("get_serialized_size", &vlink::zerocopy::PointCloud::get_serialized_size)
      .def_static("check_valid", &vlink::zerocopy::PointCloud::check_valid, "bytes"_a)
      .def("get_key_list", &vlink::zerocopy::PointCloud::get_key_list)
      .def("get_protocol_size_num", &vlink::zerocopy::PointCloud::get_protocol_size_num)
      .def("get_protocol_type_num", &vlink::zerocopy::PointCloud::get_protocol_type_num)
      .def("get_protocol_size_str", &vlink::zerocopy::PointCloud::get_protocol_size_str)
      .def("get_protocol_name_str", &vlink::zerocopy::PointCloud::get_protocol_name_str)
      .def("get_protocol_type_str", &vlink::zerocopy::PointCloud::get_protocol_type_str)
      .def("data",
           [](const vlink::zerocopy::PointCloud& self) {
             return nb::bytes(self.get_internal_data(), self.size() * self.pack_size());
           })
      .def("to_bytes",
           [](const vlink::zerocopy::PointCloud& self) {
             vlink::Bytes b;
             self >> b;
             return b;
           })
      .def("from_bytes", &zerocopy_from_bytes<vlink::zerocopy::PointCloud>, "bytes"_a)
      .def("__repr__", [](const vlink::zerocopy::PointCloud& self) {
        return std::string("PointCloud(size=") + std::to_string(self.size()) +
               ", pack_size=" + std::to_string(self.pack_size()) + ")";
      });

  nb::class_<vlink::zerocopy::ProxyData>(m, "ProxyData", "Proxy routing envelope (80 bytes)")
      .def(nb::init<>())
      .def(
          "create",
          [](nb::object instance, const vlink::Bytes& raw, const std::string& url, const std::string& ser,
             uint32_t schema, const std::string& hostname) {
            auto& self = nb::cast<vlink::zerocopy::ProxyData&>(instance);
            ensure_proxy_not_exported(self);
            self.create(raw, url, ser, schema, hostname);
            unbind_python_instance_owner(instance);
          },
          "raw"_a, "url"_a, "ser"_a, "schema"_a = static_cast<uint32_t>(0), "hostname"_a = std::string{})
      .def("clear", &zerocopy_clear<vlink::zerocopy::ProxyData>)
      .def("size", &vlink::zerocopy::ProxyData::size)
      .def("is_valid", &vlink::zerocopy::ProxyData::is_valid)
      .def("is_owner", &vlink::zerocopy::ProxyData::is_owner)
      .def("get_serialized_size", &vlink::zerocopy::ProxyData::get_serialized_size)
      .def_static("check_valid", &vlink::zerocopy::ProxyData::check_valid, "bytes"_a)
      .def("control_id", &vlink::zerocopy::ProxyData::control_id)
      .def("mode", &vlink::zerocopy::ProxyData::mode)
      .def("timestamp", &vlink::zerocopy::ProxyData::timestamp)
      .def("seq", &vlink::zerocopy::ProxyData::seq)
      .def("schema", &vlink::zerocopy::ProxyData::schema)
      .def(
          "raw",
          [](vlink::zerocopy::ProxyData& self) {
            nb::object raw = nb::cast(self.raw());
            ++proxy_export_counts[&self];

            nb::detail::keep_alive(raw.ptr(), &self, unpin_proxy_storage);
            return raw;
          },
          nb::keep_alive<0, 1>(), "Borrow the payload; release all raw views before clear/create/from_bytes.")
      .def("url", [](const vlink::zerocopy::ProxyData& self) { return std::string(self.url()); })
      .def("ser", [](const vlink::zerocopy::ProxyData& self) { return std::string(self.ser()); })
      .def("hostname", [](const vlink::zerocopy::ProxyData& self) { return std::string(self.hostname()); })
      .def("set_control_id", &vlink::zerocopy::ProxyData::set_control_id, "control_id"_a)
      .def("set_mode", &vlink::zerocopy::ProxyData::set_mode, "mode"_a)
      .def("set_timestamp", &vlink::zerocopy::ProxyData::set_timestamp, "timestamp"_a)
      .def("set_seq", &vlink::zerocopy::ProxyData::set_seq, "seq"_a)
      .def("set_schema", &vlink::zerocopy::ProxyData::set_schema, "schema"_a)
      .def_prop_rw(
          "reserved", [](vlink::zerocopy::ProxyData& self) { return self.get_reserved(); },
          [](vlink::zerocopy::ProxyData& self, uint8_t v) { self.get_reserved() = v; })
      .def_prop_rw(
          "reserved2", [](vlink::zerocopy::ProxyData& self) { return self.get_reserved2(); },
          [](vlink::zerocopy::ProxyData& self, uint16_t v) { self.get_reserved2() = v; })
      .def("to_bytes",
           [](const vlink::zerocopy::ProxyData& self) {
             vlink::Bytes b;
             self >> b;
             return b;
           })
      .def("from_bytes", &zerocopy_from_bytes<vlink::zerocopy::ProxyData>, "bytes"_a)
      .def("__repr__", [](const vlink::zerocopy::ProxyData& self) {
        return std::string("ProxyData(url='") + std::string(self.url()) + "', size=" + std::to_string(self.size()) +
               ")";
      });

  nb::class_<vlink::zerocopy::OccupancyGrid> occupancy_grid_cls(m, "OccupancyGrid",
                                                                "Zero-copy 2-D occupancy grid map (152 bytes)");
  nb::enum_<vlink::zerocopy::OccupancyGrid::CellType>(occupancy_grid_cls, "CellType")
      .value("Unknown", vlink::zerocopy::OccupancyGrid::kCellUnknown)
      .value("Int8", vlink::zerocopy::OccupancyGrid::kCellInt8)
      .value("Uint8", vlink::zerocopy::OccupancyGrid::kCellUint8)
      .value("Uint16", vlink::zerocopy::OccupancyGrid::kCellUint16)
      .value("Float32", vlink::zerocopy::OccupancyGrid::kCellFloat32);
  occupancy_grid_cls.def(nb::init<>())
      .def_rw("header", &vlink::zerocopy::OccupancyGrid::header)
      .def("create", &zerocopy_create<vlink::zerocopy::OccupancyGrid>, "size"_a)
      .def("clear", &zerocopy_clear<vlink::zerocopy::OccupancyGrid>)
      .def("size", &vlink::zerocopy::OccupancyGrid::size)
      .def("is_valid", &vlink::zerocopy::OccupancyGrid::is_valid)
      .def("is_owner", &vlink::zerocopy::OccupancyGrid::is_owner)
      .def("get_serialized_size", &vlink::zerocopy::OccupancyGrid::get_serialized_size)
      .def_static("check_valid", &vlink::zerocopy::OccupancyGrid::check_valid, "bytes"_a)
      .def_static("cell_size_of", &vlink::zerocopy::OccupancyGrid::cell_size_of, "type"_a)
      .def("update_time_ns", &vlink::zerocopy::OccupancyGrid::update_time_ns)
      .def("map_id", [](const vlink::zerocopy::OccupancyGrid& self) { return std::string(self.map_id()); })
      .def("channel", &vlink::zerocopy::OccupancyGrid::channel)
      .def("freq", &vlink::zerocopy::OccupancyGrid::freq)
      .def("width", &vlink::zerocopy::OccupancyGrid::width)
      .def("height", &vlink::zerocopy::OccupancyGrid::height)
      .def("valid_cell_count", &vlink::zerocopy::OccupancyGrid::valid_cell_count)
      .def("resolution", &vlink::zerocopy::OccupancyGrid::resolution)
      .def("origin_x", &vlink::zerocopy::OccupancyGrid::origin_x)
      .def("origin_y", &vlink::zerocopy::OccupancyGrid::origin_y)
      .def("origin_z", &vlink::zerocopy::OccupancyGrid::origin_z)
      .def("origin_yaw", &vlink::zerocopy::OccupancyGrid::origin_yaw)
      .def("value_min", &vlink::zerocopy::OccupancyGrid::value_min)
      .def("value_max", &vlink::zerocopy::OccupancyGrid::value_max)
      .def("default_value", &vlink::zerocopy::OccupancyGrid::default_value)
      .def("occupied_threshold", &vlink::zerocopy::OccupancyGrid::occupied_threshold)
      .def("free_threshold", &vlink::zerocopy::OccupancyGrid::free_threshold)
      .def("cell_type", &vlink::zerocopy::OccupancyGrid::cell_type)
      .def("cell_size", &vlink::zerocopy::OccupancyGrid::cell_size)
      .def("set_update_time_ns", &vlink::zerocopy::OccupancyGrid::set_update_time_ns, "update_time_ns"_a)
      .def(
          "set_map_id",
          [](vlink::zerocopy::OccupancyGrid& self, const std::string& s) { self.set_map_id(utf8_prefix(s, 15)); },
          "map_id"_a)
      .def("set_channel", &vlink::zerocopy::OccupancyGrid::set_channel, "channel"_a)
      .def("set_freq", &vlink::zerocopy::OccupancyGrid::set_freq, "freq"_a)
      .def("set_width", &vlink::zerocopy::OccupancyGrid::set_width, "width"_a)
      .def("set_height", &vlink::zerocopy::OccupancyGrid::set_height, "height"_a)
      .def("set_valid_cell_count", &vlink::zerocopy::OccupancyGrid::set_valid_cell_count, "valid_cell_count"_a)
      .def("set_resolution", &vlink::zerocopy::OccupancyGrid::set_resolution, "resolution"_a)
      .def("set_origin_x", &vlink::zerocopy::OccupancyGrid::set_origin_x, "origin_x"_a)
      .def("set_origin_y", &vlink::zerocopy::OccupancyGrid::set_origin_y, "origin_y"_a)
      .def("set_origin_z", &vlink::zerocopy::OccupancyGrid::set_origin_z, "origin_z"_a)
      .def("set_origin_yaw", &vlink::zerocopy::OccupancyGrid::set_origin_yaw, "origin_yaw"_a)
      .def("set_value_min", &vlink::zerocopy::OccupancyGrid::set_value_min, "value_min"_a)
      .def("set_value_max", &vlink::zerocopy::OccupancyGrid::set_value_max, "value_max"_a)
      .def("set_default_value", &vlink::zerocopy::OccupancyGrid::set_default_value, "default_value"_a)
      .def("set_occupied_threshold", &vlink::zerocopy::OccupancyGrid::set_occupied_threshold, "occupied_threshold"_a)
      .def("set_free_threshold", &vlink::zerocopy::OccupancyGrid::set_free_threshold, "free_threshold"_a)
      .def("set_cell_type", &vlink::zerocopy::OccupancyGrid::set_cell_type, "cell_type"_a)
      .def_prop_rw(
          "reserved", [](vlink::zerocopy::OccupancyGrid& self) { return self.get_reserved(); },
          [](vlink::zerocopy::OccupancyGrid& self, uint16_t v) { self.get_reserved() = v; })
      .def_prop_rw(
          "reserved2", [](vlink::zerocopy::OccupancyGrid& self) { return self.get_reserved2(); },
          [](vlink::zerocopy::OccupancyGrid& self, uint32_t v) { self.get_reserved2() = v; })
      .def_prop_rw(
          "reserved3", [](vlink::zerocopy::OccupancyGrid& self) { return self.get_reserved3(); },
          [](vlink::zerocopy::OccupancyGrid& self, uint32_t v) { self.get_reserved3() = v; })
      .def("data", [](const vlink::zerocopy::OccupancyGrid& self) { return nb::bytes(self.data(), self.size()); })
      .def("fill_data", &zerocopy_fill_data<vlink::zerocopy::OccupancyGrid>, "data"_a)
      .def("to_bytes",
           [](const vlink::zerocopy::OccupancyGrid& self) {
             vlink::Bytes b;
             self >> b;
             return b;
           })
      .def("from_bytes", &zerocopy_from_bytes<vlink::zerocopy::OccupancyGrid>, "bytes"_a)
      .def("__repr__", [](const vlink::zerocopy::OccupancyGrid& self) {
        return std::string("OccupancyGrid(") + std::to_string(self.width()) + "x" + std::to_string(self.height()) +
               ", size=" + std::to_string(self.size()) + ")";
      });

  nb::class_<vlink::zerocopy::Tensor> tensor_cls(m, "Tensor", "Zero-copy N-D tensor (248 bytes)");
  nb::enum_<vlink::zerocopy::Tensor::DataType>(tensor_cls, "DataType")
      .value("Unknown", vlink::zerocopy::Tensor::kDataUnknown)
      .value("Bool", vlink::zerocopy::Tensor::kBool)
      .value("Int8", vlink::zerocopy::Tensor::kInt8)
      .value("Uint8", vlink::zerocopy::Tensor::kUint8)
      .value("Int16", vlink::zerocopy::Tensor::kInt16)
      .value("Uint16", vlink::zerocopy::Tensor::kUint16)
      .value("Int32", vlink::zerocopy::Tensor::kInt32)
      .value("Uint32", vlink::zerocopy::Tensor::kUint32)
      .value("Int64", vlink::zerocopy::Tensor::kInt64)
      .value("Uint64", vlink::zerocopy::Tensor::kUint64)
      .value("Float16", vlink::zerocopy::Tensor::kFloat16)
      .value("Bfloat16", vlink::zerocopy::Tensor::kBfloat16)
      .value("Float32", vlink::zerocopy::Tensor::kFloat32)
      .value("Float64", vlink::zerocopy::Tensor::kFloat64);
  nb::enum_<vlink::zerocopy::Tensor::Device>(tensor_cls, "Device")
      .value("Cpu", vlink::zerocopy::Tensor::kDeviceCpu)
      .value("Gpu", vlink::zerocopy::Tensor::kDeviceGpu)
      .value("Npu", vlink::zerocopy::Tensor::kDeviceNpu)
      .value("Dsp", vlink::zerocopy::Tensor::kDeviceDsp);
  tensor_cls.attr("kMaxRank") = vlink::zerocopy::Tensor::kMaxRank;
  tensor_cls.def(nb::init<>())
      .def_rw("header", &vlink::zerocopy::Tensor::header)
      .def("create", &zerocopy_create<vlink::zerocopy::Tensor>, "size"_a)
      .def("clear", &zerocopy_clear<vlink::zerocopy::Tensor>)
      .def("size", &vlink::zerocopy::Tensor::size)
      .def("is_valid", &vlink::zerocopy::Tensor::is_valid)
      .def("is_owner", &vlink::zerocopy::Tensor::is_owner)
      .def("get_serialized_size", &vlink::zerocopy::Tensor::get_serialized_size)
      .def_static("check_valid", &vlink::zerocopy::Tensor::check_valid, "bytes"_a)
      .def_static("element_size_of", &vlink::zerocopy::Tensor::element_size_of, "dtype"_a)
      .def("update_time_ns", &vlink::zerocopy::Tensor::update_time_ns)
      .def("num_elements", &vlink::zerocopy::Tensor::num_elements)
      .def("name", [](const vlink::zerocopy::Tensor& self) { return std::string(self.name()); })
      .def("model_id", [](const vlink::zerocopy::Tensor& self) { return std::string(self.model_id()); })
      .def("layout", [](const vlink::zerocopy::Tensor& self) { return std::string(self.layout()); })
      .def("shape",
           [](const vlink::zerocopy::Tensor& self) {
             return std::vector<uint32_t>(self.shape(), self.shape() + self.rank());
           })
      .def("shape_at", &vlink::zerocopy::Tensor::shape_at, "dim"_a)
      .def("strides",
           [](const vlink::zerocopy::Tensor& self) {
             return std::vector<uint32_t>(self.strides(), self.strides() + self.rank());
           })
      .def("stride_at", &vlink::zerocopy::Tensor::stride_at, "dim"_a)
      .def("channel", &vlink::zerocopy::Tensor::channel)
      .def("freq", &vlink::zerocopy::Tensor::freq)
      .def("batch_size", &vlink::zerocopy::Tensor::batch_size)
      .def("quant_scale", &vlink::zerocopy::Tensor::quant_scale)
      .def("quant_zero_point", &vlink::zerocopy::Tensor::quant_zero_point)
      .def("dtype", &vlink::zerocopy::Tensor::dtype)
      .def("rank", &vlink::zerocopy::Tensor::rank)
      .def("device", &vlink::zerocopy::Tensor::device)
      .def("element_size", &vlink::zerocopy::Tensor::element_size)
      .def("set_update_time_ns", &vlink::zerocopy::Tensor::set_update_time_ns, "update_time_ns"_a)
      .def(
          "set_name", [](vlink::zerocopy::Tensor& self, const std::string& s) { self.set_name(utf8_prefix(s, 31)); },
          "name"_a)
      .def(
          "set_model_id",
          [](vlink::zerocopy::Tensor& self, const std::string& s) { self.set_model_id(utf8_prefix(s, 31)); },
          "model_id"_a)
      .def(
          "set_layout",
          [](vlink::zerocopy::Tensor& self, const std::string& s) { self.set_layout(utf8_prefix(s, 15)); }, "layout"_a)
      .def(
          "set_shape",
          [](vlink::zerocopy::Tensor& self, const std::vector<uint32_t>& v) {
            const auto rank = std::min<size_t>(v.size(), vlink::zerocopy::Tensor::kMaxRank);
            self.set_shape(v.data(), static_cast<uint8_t>(rank));
          },
          "shape"_a)
      .def("set_shape_at", &vlink::zerocopy::Tensor::set_shape_at, "dim"_a, "value"_a)
      .def("set_stride_at", &vlink::zerocopy::Tensor::set_stride_at, "dim"_a, "value"_a)
      .def("set_channel", &vlink::zerocopy::Tensor::set_channel, "channel"_a)
      .def("set_freq", &vlink::zerocopy::Tensor::set_freq, "freq"_a)
      .def("set_batch_size", &vlink::zerocopy::Tensor::set_batch_size, "batch_size"_a)
      .def("set_quant_scale", &vlink::zerocopy::Tensor::set_quant_scale, "quant_scale"_a)
      .def("set_quant_zero_point", &vlink::zerocopy::Tensor::set_quant_zero_point, "quant_zero_point"_a)
      .def("set_dtype", &vlink::zerocopy::Tensor::set_dtype, "dtype"_a)
      .def("set_device", &vlink::zerocopy::Tensor::set_device, "device"_a)
      .def_prop_rw(
          "reserved", [](vlink::zerocopy::Tensor& self) { return self.get_reserved(); },
          [](vlink::zerocopy::Tensor& self, uint8_t v) { self.get_reserved() = v; })
      .def_prop_rw(
          "reserved2", [](vlink::zerocopy::Tensor& self) { return self.get_reserved2(); },
          [](vlink::zerocopy::Tensor& self, uint16_t v) { self.get_reserved2() = v; })
      .def_prop_rw(
          "reserved3", [](vlink::zerocopy::Tensor& self) { return self.get_reserved3(); },
          [](vlink::zerocopy::Tensor& self, uint32_t v) { self.get_reserved3() = v; })
      .def("data", [](const vlink::zerocopy::Tensor& self) { return nb::bytes(self.data(), self.size()); })
      .def("fill_data", &zerocopy_fill_data<vlink::zerocopy::Tensor>, "data"_a)
      .def("to_bytes",
           [](const vlink::zerocopy::Tensor& self) {
             vlink::Bytes b;
             self >> b;
             return b;
           })
      .def("from_bytes", &zerocopy_from_bytes<vlink::zerocopy::Tensor>, "bytes"_a)
      .def("__repr__", [](const vlink::zerocopy::Tensor& self) {
        return std::string("Tensor(rank=") + std::to_string(self.rank()) +
               ", num_elements=" + std::to_string(self.num_elements()) + ")";
      });

  nb::class_<vlink::zerocopy::ObjectArray> object_array_cls(
      m, "ObjectArray", "Zero-copy variable-length array of 3-D detection / tracking objects (112 bytes)");
  nb::enum_<vlink::zerocopy::ObjectArray::MotionState>(object_array_cls, "MotionState")
      .value("Unknown", vlink::zerocopy::ObjectArray::kMotionUnknown)
      .value("Stationary", vlink::zerocopy::ObjectArray::kMotionStationary)
      .value("Moving", vlink::zerocopy::ObjectArray::kMotionMoving)
      .value("Stopped", vlink::zerocopy::ObjectArray::kMotionStopped)
      .value("Parked", vlink::zerocopy::ObjectArray::kMotionParked);
  nb::enum_<vlink::zerocopy::ObjectArray::SourceType>(object_array_cls, "SourceType")
      .value("Unknown", vlink::zerocopy::ObjectArray::kSourceUnknown)
      .value("Lidar", vlink::zerocopy::ObjectArray::kSourceLidar)
      .value("Camera", vlink::zerocopy::ObjectArray::kSourceCamera)
      .value("Radar", vlink::zerocopy::ObjectArray::kSourceRadar)
      .value("Fusion", vlink::zerocopy::ObjectArray::kSourceFusion)
      .value("Ultrasonic", vlink::zerocopy::ObjectArray::kSourceUltrasonic);
  nb::class_<vlink::zerocopy::ObjectArray::Object>(object_array_cls, "Object")
      .def(nb::init<>())
      .def_prop_rw(
          "label",
          [](const vlink::zerocopy::ObjectArray::Object& self) {
            return std::string(self.label, ::strnlen(self.label, sizeof(self.label)));
          },
          [](vlink::zerocopy::ObjectArray::Object& self, const std::string& s) {
            constexpr size_t kMax = sizeof(vlink::zerocopy::ObjectArray::Object::label) - 1;
            const auto prefix = utf8_prefix(s, kMax);
            std::memset(self.label, 0, sizeof(self.label));
            std::memcpy(self.label, prefix.data(), prefix.size());
          })
      .def_prop_rw(
          "position",
          [](const vlink::zerocopy::ObjectArray::Object& self) {
            return std::vector<float>(self.position, self.position + 3);
          },
          [](vlink::zerocopy::ObjectArray::Object& self, const std::vector<float>& v) {
            const size_t n = std::min<size_t>(v.size(), 3);
            for (size_t i = 0; i < n; ++i) {
              self.position[i] = v[i];
            }
            for (size_t i = n; i < 3; ++i) {
              self.position[i] = 0.0F;
            }
          })
      .def_rw("yaw", &vlink::zerocopy::ObjectArray::Object::yaw)
      .def_prop_rw(
          "size",
          [](const vlink::zerocopy::ObjectArray::Object& self) { return std::vector<float>(self.size, self.size + 3); },
          [](vlink::zerocopy::ObjectArray::Object& self, const std::vector<float>& v) {
            const size_t n = std::min<size_t>(v.size(), 3);
            for (size_t i = 0; i < n; ++i) {
              self.size[i] = v[i];
            }
            for (size_t i = n; i < 3; ++i) {
              self.size[i] = 0.0F;
            }
          })
      .def_rw("yaw_rate", &vlink::zerocopy::ObjectArray::Object::yaw_rate)
      .def_prop_rw(
          "velocity",
          [](const vlink::zerocopy::ObjectArray::Object& self) {
            return std::vector<float>(self.velocity, self.velocity + 3);
          },
          [](vlink::zerocopy::ObjectArray::Object& self, const std::vector<float>& v) {
            const size_t n = std::min<size_t>(v.size(), 3);
            for (size_t i = 0; i < n; ++i) {
              self.velocity[i] = v[i];
            }
            for (size_t i = n; i < 3; ++i) {
              self.velocity[i] = 0.0F;
            }
          })
      .def_rw("score", &vlink::zerocopy::ObjectArray::Object::score)
      .def_prop_rw(
          "acceleration",
          [](const vlink::zerocopy::ObjectArray::Object& self) {
            return std::vector<float>(self.acceleration, self.acceleration + 3);
          },
          [](vlink::zerocopy::ObjectArray::Object& self, const std::vector<float>& v) {
            const size_t n = std::min<size_t>(v.size(), 3);
            for (size_t i = 0; i < n; ++i) {
              self.acceleration[i] = v[i];
            }
            for (size_t i = n; i < 3; ++i) {
              self.acceleration[i] = 0.0F;
            }
          })
      .def_rw("existence_probability", &vlink::zerocopy::ObjectArray::Object::existence_probability)
      .def_prop_rw(
          "position_covariance",
          [](const vlink::zerocopy::ObjectArray::Object& self) {
            return std::vector<float>(self.position_covariance, self.position_covariance + 6);
          },
          [](vlink::zerocopy::ObjectArray::Object& self, const std::vector<float>& v) {
            const size_t n = std::min<size_t>(v.size(), 6);
            for (size_t i = 0; i < n; ++i) {
              self.position_covariance[i] = v[i];
            }
            for (size_t i = n; i < 6; ++i) {
              self.position_covariance[i] = 0.0F;
            }
          })
      .def_rw("class_id", &vlink::zerocopy::ObjectArray::Object::class_id)
      .def_rw("track_id", &vlink::zerocopy::ObjectArray::Object::track_id)
      .def_rw("age", &vlink::zerocopy::ObjectArray::Object::age)
      .def_rw("num_observations", &vlink::zerocopy::ObjectArray::Object::num_observations)
      .def_rw("motion_state", &vlink::zerocopy::ObjectArray::Object::motion_state)
      .def_rw("source_type", &vlink::zerocopy::ObjectArray::Object::source_type)
      .def_rw("subtype_id", &vlink::zerocopy::ObjectArray::Object::subtype_id)
      .def_rw("reserved_buf", &vlink::zerocopy::ObjectArray::Object::reserved_buf)
      .def("__repr__", [](const vlink::zerocopy::ObjectArray::Object& self) {
        return std::string("Object(label='") + std::string(self.label, ::strnlen(self.label, sizeof(self.label))) +
               "', class_id=" + std::to_string(self.class_id) + ", track_id=" + std::to_string(self.track_id) + ")";
      });
  object_array_cls.def(nb::init<>())
      .def("deep_copy", &zerocopy_deep_copy<vlink::zerocopy::ObjectArray>, "source"_a,
           "Copy metadata and records into independent storage.")
      .def_rw("header", &vlink::zerocopy::ObjectArray::header)
      .def("create", &zerocopy_create<vlink::zerocopy::ObjectArray>, "count"_a)
      .def("clear", &zerocopy_clear<vlink::zerocopy::ObjectArray>)
      .def("push_value", &vlink::zerocopy::ObjectArray::push_value, "object"_a)
      .def("set_value", &vlink::zerocopy::ObjectArray::set_value, "index"_a, "object"_a)
      .def(
          "get_value", [](const vlink::zerocopy::ObjectArray& self, uint32_t index) { return self.get_value(index); },
          "index"_a)
      .def("resize", &vlink::zerocopy::ObjectArray::resize, "count"_a)
      .def("count", &vlink::zerocopy::ObjectArray::count)
      .def("pack_size", &vlink::zerocopy::ObjectArray::pack_size)
      .def("capacity", &vlink::zerocopy::ObjectArray::capacity)
      .def("is_valid", &vlink::zerocopy::ObjectArray::is_valid)
      .def("is_owner", &vlink::zerocopy::ObjectArray::is_owner)
      .def("get_serialized_size", &vlink::zerocopy::ObjectArray::get_serialized_size)
      .def_static("check_valid", &vlink::zerocopy::ObjectArray::check_valid, "bytes"_a)
      .def("update_time_ns", &vlink::zerocopy::ObjectArray::update_time_ns)
      .def("source_id", [](const vlink::zerocopy::ObjectArray& self) { return std::string(self.source_id()); })
      .def("channel", &vlink::zerocopy::ObjectArray::channel)
      .def("freq", &vlink::zerocopy::ObjectArray::freq)
      .def("set_update_time_ns", &vlink::zerocopy::ObjectArray::set_update_time_ns, "update_time_ns"_a)
      .def(
          "set_source_id",
          [](vlink::zerocopy::ObjectArray& self, const std::string& s) { self.set_source_id(utf8_prefix(s, 15)); },
          "source_id"_a)
      .def("set_channel", &vlink::zerocopy::ObjectArray::set_channel, "channel"_a)
      .def("set_freq", &vlink::zerocopy::ObjectArray::set_freq, "freq"_a)
      .def(
          "objects",
          [](const vlink::zerocopy::ObjectArray& self,
             uint32_t index) -> std::optional<vlink::zerocopy::ObjectArray::Object> {
            const auto* p = self.objects(index);

            if (!p) {
              return std::nullopt;
            }

            return *p;
          },
          "index"_a = static_cast<uint32_t>(0))
      .def_prop_rw(
          "reserved", [](vlink::zerocopy::ObjectArray& self) { return self.get_reserved(); },
          [](vlink::zerocopy::ObjectArray& self, uint8_t v) { self.get_reserved() = v; })
      .def_prop_rw(
          "reserved2", [](vlink::zerocopy::ObjectArray& self) { return self.get_reserved2(); },
          [](vlink::zerocopy::ObjectArray& self, uint16_t v) { self.get_reserved2() = v; })
      .def_prop_rw(
          "reserved3", [](vlink::zerocopy::ObjectArray& self) { return self.get_reserved3(); },
          [](vlink::zerocopy::ObjectArray& self, uint32_t v) { self.get_reserved3() = v; })
      .def_prop_rw(
          "reserved4", [](vlink::zerocopy::ObjectArray& self) { return self.get_reserved4(); },
          [](vlink::zerocopy::ObjectArray& self, uint32_t v) { self.get_reserved4() = v; })
      .def_prop_rw(
          "reserved5", [](vlink::zerocopy::ObjectArray& self) { return self.get_reserved5(); },
          [](vlink::zerocopy::ObjectArray& self, uint32_t v) { self.get_reserved5() = v; })
      .def("data",
           [](const vlink::zerocopy::ObjectArray& self) {
             const size_t payload_size = static_cast<size_t>(self.count()) * static_cast<size_t>(self.pack_size());
             return nb::bytes(self.data(), payload_size);
           })
      .def("to_bytes",
           [](const vlink::zerocopy::ObjectArray& self) {
             vlink::Bytes b;
             self >> b;
             return b;
           })
      .def("from_bytes", &zerocopy_from_bytes<vlink::zerocopy::ObjectArray>, "bytes"_a)
      .def("__repr__", [](const vlink::zerocopy::ObjectArray& self) {
        return std::string("ObjectArray(count=") + std::to_string(self.count()) +
               ", capacity=" + std::to_string(self.capacity()) + ")";
      });

  nb::class_<vlink::zerocopy::AudioFrame> audio_frame_cls(m, "AudioFrame", "Zero-copy audio frame (128 bytes)");
  nb::enum_<vlink::zerocopy::AudioFrame::Format>(audio_frame_cls, "Format")
      .value("Unknown", vlink::zerocopy::AudioFrame::kFormatUnknown)
      .value("PcmS16", vlink::zerocopy::AudioFrame::kFormatPcmS16)
      .value("PcmS24", vlink::zerocopy::AudioFrame::kFormatPcmS24)
      .value("PcmS32", vlink::zerocopy::AudioFrame::kFormatPcmS32)
      .value("PcmF32", vlink::zerocopy::AudioFrame::kFormatPcmF32)
      .value("PcmU8", vlink::zerocopy::AudioFrame::kFormatPcmU8)
      .value("Opus", vlink::zerocopy::AudioFrame::kFormatOpus)
      .value("Aac", vlink::zerocopy::AudioFrame::kFormatAac)
      .value("Mp3", vlink::zerocopy::AudioFrame::kFormatMp3)
      .value("Flac", vlink::zerocopy::AudioFrame::kFormatFlac);
  nb::enum_<vlink::zerocopy::AudioFrame::Layout>(audio_frame_cls, "Layout")
      .value("Unknown", vlink::zerocopy::AudioFrame::kLayoutUnknown)
      .value("Interleaved", vlink::zerocopy::AudioFrame::kLayoutInterleaved)
      .value("Planar", vlink::zerocopy::AudioFrame::kLayoutPlanar);
  audio_frame_cls.def(nb::init<>())
      .def_rw("header", &vlink::zerocopy::AudioFrame::header)
      .def("create", &zerocopy_create<vlink::zerocopy::AudioFrame>, "size"_a)
      .def("clear", &zerocopy_clear<vlink::zerocopy::AudioFrame>)
      .def("size", &vlink::zerocopy::AudioFrame::size)
      .def("is_valid", &vlink::zerocopy::AudioFrame::is_valid)
      .def("is_owner", &vlink::zerocopy::AudioFrame::is_owner)
      .def("get_serialized_size", &vlink::zerocopy::AudioFrame::get_serialized_size)
      .def_static("check_valid", &vlink::zerocopy::AudioFrame::check_valid, "bytes"_a)
      .def("update_time_ns", &vlink::zerocopy::AudioFrame::update_time_ns)
      .def("duration_ns", &vlink::zerocopy::AudioFrame::duration_ns)
      .def("codec", [](const vlink::zerocopy::AudioFrame& self) { return std::string(self.codec()); })
      .def("language", [](const vlink::zerocopy::AudioFrame& self) { return std::string(self.language()); })
      .def("channel", &vlink::zerocopy::AudioFrame::channel)
      .def("freq", &vlink::zerocopy::AudioFrame::freq)
      .def("sample_rate", &vlink::zerocopy::AudioFrame::sample_rate)
      .def("num_samples", &vlink::zerocopy::AudioFrame::num_samples)
      .def("bitrate", &vlink::zerocopy::AudioFrame::bitrate)
      .def("num_channels", &vlink::zerocopy::AudioFrame::num_channels)
      .def("bit_depth", &vlink::zerocopy::AudioFrame::bit_depth)
      .def("format", &vlink::zerocopy::AudioFrame::format)
      .def("layout", &vlink::zerocopy::AudioFrame::layout)
      .def("set_update_time_ns", &vlink::zerocopy::AudioFrame::set_update_time_ns, "update_time_ns"_a)
      .def("set_duration_ns", &vlink::zerocopy::AudioFrame::set_duration_ns, "duration_ns"_a)
      .def(
          "set_codec",
          [](vlink::zerocopy::AudioFrame& self, const std::string& s) { self.set_codec(utf8_prefix(s, 15)); },
          "codec"_a)
      .def(
          "set_language",
          [](vlink::zerocopy::AudioFrame& self, const std::string& s) { self.set_language(utf8_prefix(s, 7)); },
          "language"_a)
      .def("set_channel", &vlink::zerocopy::AudioFrame::set_channel, "channel"_a)
      .def("set_freq", &vlink::zerocopy::AudioFrame::set_freq, "freq"_a)
      .def("set_sample_rate", &vlink::zerocopy::AudioFrame::set_sample_rate, "sample_rate"_a)
      .def("set_num_samples", &vlink::zerocopy::AudioFrame::set_num_samples, "num_samples"_a)
      .def("set_bitrate", &vlink::zerocopy::AudioFrame::set_bitrate, "bitrate"_a)
      .def("set_num_channels", &vlink::zerocopy::AudioFrame::set_num_channels, "num_channels"_a)
      .def("set_bit_depth", &vlink::zerocopy::AudioFrame::set_bit_depth, "bit_depth"_a)
      .def("set_format", &vlink::zerocopy::AudioFrame::set_format, "format"_a)
      .def("set_layout", &vlink::zerocopy::AudioFrame::set_layout, "layout"_a)
      .def_prop_rw(
          "reserved", [](vlink::zerocopy::AudioFrame& self) { return self.get_reserved(); },
          [](vlink::zerocopy::AudioFrame& self, uint8_t v) { self.get_reserved() = v; })
      .def_prop_rw(
          "reserved2", [](vlink::zerocopy::AudioFrame& self) { return self.get_reserved2(); },
          [](vlink::zerocopy::AudioFrame& self, uint32_t v) { self.get_reserved2() = v; })
      .def("data", [](const vlink::zerocopy::AudioFrame& self) { return nb::bytes(self.data(), self.size()); })
      .def("fill_data", &zerocopy_fill_data<vlink::zerocopy::AudioFrame>, "data"_a)
      .def("to_bytes",
           [](const vlink::zerocopy::AudioFrame& self) {
             vlink::Bytes b;
             self >> b;
             return b;
           })
      .def("from_bytes", &zerocopy_from_bytes<vlink::zerocopy::AudioFrame>, "bytes"_a)
      .def("__repr__", [](const vlink::zerocopy::AudioFrame& self) {
        return std::string("AudioFrame(sample_rate=") + std::to_string(self.sample_rate()) +
               ", num_channels=" + std::to_string(self.num_channels()) + ", size=" + std::to_string(self.size()) + ")";
      });
}

}  // namespace vlink::python
