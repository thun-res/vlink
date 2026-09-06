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
#include <nanobind/stl/string_view.h>
#include <vlink/base/uuid.h>
#include <vlink/vlink.h>

#include <cstring>
#include <functional>

#include "buffer.h"

namespace vlink::python {

using namespace nb::literals;  // NOLINT

void bind_enums(nb::module_& m) {
  nb::enum_<vlink::ImplType>(m, "ImplType", "Node role type")
      .value("Unknown", vlink::kUnknownImplType)
      .value("Publisher", vlink::kPublisher)
      .value("Subscriber", vlink::kSubscriber)
      .value("Setter", vlink::kSetter)
      .value("Getter", vlink::kGetter)
      .value("Server", vlink::kServer)
      .value("Client", vlink::kClient);

  nb::enum_<vlink::TransportType>(m, "TransportType", "Transport backend")
      .value("Unknown", vlink::TransportType::kUnknown)
      .value("Intra", vlink::TransportType::kIntra)
      .value("Shm", vlink::TransportType::kShm)
      .value("Shm2", vlink::TransportType::kShm2)
      .value("Zenoh", vlink::TransportType::kZenoh)
      .value("Dds", vlink::TransportType::kDds)
      .value("Ddsc", vlink::TransportType::kDdsc)
      .value("Ddsr", vlink::TransportType::kDdsr)
      .value("Someip", vlink::TransportType::kSomeip)
      .value("Mqtt", vlink::TransportType::kMqtt)
      .value("Fdbus", vlink::TransportType::kFdbus);

  nb::enum_<vlink::InitType>(m, "InitType")
      .value("WithoutInit", vlink::InitType::kWithoutInit)
      .value("WithInit", vlink::InitType::kWithInit);

  nb::enum_<vlink::SecurityType>(m, "SecurityType")
      .value("WithoutSecurity", vlink::SecurityType::kWithoutSecurity)
      .value("WithSecurity", vlink::SecurityType::kWithSecurity);

  nb::enum_<vlink::ActionType>(m, "ActionType", "Message recording action")
      .value("Unknown", vlink::ActionType::kUnknownAction)
      .value("ClientRequest", vlink::ActionType::kClientRequest)
      .value("ClientResponse", vlink::ActionType::kClientResponse)
      .value("ServerRequest", vlink::ActionType::kServerRequest)
      .value("ServerResponse", vlink::ActionType::kServerResponse)
      .value("Publish", vlink::ActionType::kPublish)
      .value("Subscribe", vlink::ActionType::kSubscribe)
      .value("Set", vlink::ActionType::kSet)
      .value("Get", vlink::ActionType::kGet);

  nb::enum_<vlink::SchemaType>(m, "SchemaType", nb::is_arithmetic(), "Coarse schema family")
      .value("Unknown", vlink::SchemaType::kUnknown)
      .value("Protobuf", vlink::SchemaType::kProtobuf)
      .value("Flatbuffers", vlink::SchemaType::kFlatbuffers)
      .value("Raw", vlink::SchemaType::kRaw)
      .value("ZeroCopy", vlink::SchemaType::kZeroCopy)
      .value("Cdr", vlink::SchemaType::kCdr);
}

void bind_frame(nb::module_& m) {
  nb::class_<vlink::Frame>(m, "Frame", "One bag record or replay message")
      .def(nb::init<>())
      .def_rw("timestamp", &vlink::Frame::timestamp)
      .def_rw("url", &vlink::Frame::url)
      .def_rw("ser_type", &vlink::Frame::ser_type)
      .def_rw("schema_type", &vlink::Frame::schema_type)
      .def_rw("action_type", &vlink::Frame::action_type)
      .def_prop_rw(
          "data", [](const vlink::Frame& self) { return PythonCodec<vlink::Bytes>::to_python(self.data); },
          [](vlink::Frame& self, nb::handle data) { self.data = PythonCodec<vlink::Bytes>::from_python_owned(data); })
      .def("__repr__", [](const vlink::Frame& self) {
        return "Frame(timestamp=" + std::to_string(self.timestamp) + ", url='" + self.url +
               "', size=" + std::to_string(self.data.size()) + ")";
      });
}

void bind_metadata(nb::module_& m) {
  nb::class_<vlink::Version>(m, "Version", "Semantic versioning")
      .def(nb::init<>())
      .def(nb::init<uint8_t, uint8_t, uint8_t>(), "major"_a, "minor"_a, "patch"_a)
      .def_static("from_string", &vlink::Version::from_string, "str"_a)
      .def("to_string", &vlink::Version::to_string)
      .def("is_valid", &vlink::Version::is_valid)
      .def_rw("major", &vlink::Version::major)
      .def_rw("minor", &vlink::Version::minor)
      .def_rw("patch", &vlink::Version::patch)
      .def("__eq__", [](const vlink::Version& a, const vlink::Version& b) { return a == b; })
      .def("__ne__", [](const vlink::Version& a, const vlink::Version& b) { return a != b; })
      .def("__lt__", [](const vlink::Version& a, const vlink::Version& b) { return a < b; })
      .def("__gt__", [](const vlink::Version& a, const vlink::Version& b) { return a > b; })
      .def("__repr__", [](const vlink::Version& self) { return "Version(" + self.to_string() + ")"; });

  nb::class_<vlink::SchemaData>(m, "SchemaData", "Runtime schema descriptor")
      .def(nb::init<>())
      .def_rw("name", &vlink::SchemaData::name)
      .def_rw("encoding", &vlink::SchemaData::encoding)
      .def_rw("schema_type", &vlink::SchemaData::schema_type)
      .def_prop_rw(
          "data", [](vlink::SchemaData& self) -> vlink::Bytes& { return self.data; },
          [](vlink::SchemaData& self, const vlink::Bytes& data) {
            if (&self.data == &data) {
              return;
            }

            ensure_bytes_not_exported(self.data);
            self.data = data;
          },
          nb::rv_policy::reference_internal)
      .def_static("is_valid_type", &vlink::SchemaData::is_valid_type, "schema_type"_a)
      .def_static("is_real_type", &vlink::SchemaData::is_real_type, "schema_type"_a)
      .def_static(
          "convert_type",
          [](vlink::SchemaType schema_type) { return std::string(vlink::SchemaData::convert_type(schema_type)); },
          "schema_type"_a)
      .def_static("convert_encoding", &vlink::SchemaData::convert_encoding, "encoding"_a)
      .def_static(
          "infer_ser_type", [](const std::string& ser_type) { return vlink::SchemaData::infer_ser_type(ser_type); },
          "ser_type"_a)
      .def_static(
          "resolve_type",
          [](vlink::SchemaType schema_type, const std::string& ser_type, const std::string& encoding) {
            return vlink::SchemaData::resolve_type(schema_type, ser_type, encoding);
          },
          "schema_type"_a, "ser_type"_a = "", "encoding"_a = "")
      .def("__bool__",
           [](const vlink::SchemaData& self) {
             return !self.name.empty() || !self.encoding.empty() || self.schema_type != vlink::SchemaType::kUnknown ||
                    !self.data.empty();
           })
      .def("__repr__", [](const vlink::SchemaData& self) {
        return "SchemaData(name='" + self.name + "', encoding='" + self.encoding +
               "', size=" + std::to_string(self.data.size()) + ")";
      });

  nb::class_<vlink::SampleLostInfo>(m, "SampleLostInfo")
      .def(nb::init<>())
      .def_rw("total", &vlink::SampleLostInfo::total)
      .def_rw("lost", &vlink::SampleLostInfo::lost)
      .def("__repr__", [](const vlink::SampleLostInfo& self) {
        return "SampleLostInfo(total=" + std::to_string(self.total) + ", lost=" + std::to_string(self.lost) + ")";
      });
}

void bind_uuid(nb::module_& m) {
  nb::class_<vlink::Uuid> uuid_cls(m, "Uuid", "RFC 4122 128-bit UUID value type with v4 random generation");

  nb::enum_<vlink::Uuid::Variant>(uuid_cls, "Variant", "UUID variant field (RFC 4122 section 4.1.1)")
      .value("Ncs", vlink::Uuid::Variant::kNcs)
      .value("Rfc", vlink::Uuid::Variant::kRfc)
      .value("Microsoft", vlink::Uuid::Variant::kMicrosoft)
      .value("Reserved", vlink::Uuid::Variant::kReserved);

  nb::enum_<vlink::Uuid::Version>(uuid_cls, "Version", "UUID version field (RFC 4122 section 4.1.3)")
      .value("None_", vlink::Uuid::Version::kNone)
      .value("TimeBased", vlink::Uuid::Version::kTimeBased)
      .value("DceSecurity", vlink::Uuid::Version::kDceSecurity)
      .value("NameBasedMd5", vlink::Uuid::Version::kNameBasedMd5)
      .value("RandomBased", vlink::Uuid::Version::kRandomBased)
      .value("NameBasedSha1", vlink::Uuid::Version::kNameBasedSha1);

  uuid_cls.def(nb::init<>(), "Default-constructs a nil (all-zero) UUID.")
      .def(nb::init<const std::array<uint8_t, vlink::Uuid::kByteSize>&>(), "data"_a,
           "Constructs from a 16-byte std::array payload.")
      .def(
          "__init__",
          [](vlink::Uuid* self, nb::bytes data) {
            if (data.size() != vlink::Uuid::kByteSize) {
              throw nb::value_error("Uuid requires exactly 16 bytes");
            }
            std::array<uint8_t, vlink::Uuid::kByteSize> array{};
            std::memcpy(array.data(), data.c_str(), vlink::Uuid::kByteSize);
            new (self) vlink::Uuid(array);
          },
          "data"_a, "Constructs from a Python bytes-like object of length 16.")
      .def_ro_static("BYTE_SIZE", &vlink::Uuid::kByteSize)
      .def_ro_static("STRING_SIZE", &vlink::Uuid::kStringSize)
      .def("variant", &vlink::Uuid::variant, "Returns the UUID variant field.")
      .def("version", &vlink::Uuid::version, "Returns the UUID version field.")
      .def("is_nil", &vlink::Uuid::is_nil, "Returns True when every byte is zero.")
      .def(
          "bytes",
          [](const vlink::Uuid& self) {
            const auto& data = self.bytes();
            return nb::bytes(reinterpret_cast<const char*>(data.data()), data.size());
          },
          "Returns the 16 underlying bytes as a Python bytes object.")
      .def("to_string", &vlink::Uuid::to_string, "Formats as the 36-character lowercase canonical form with hyphens.")
      .def("to_compact_string", &vlink::Uuid::to_compact_string,
           "Formats as a 32-character lowercase hex string with no hyphens.")
      .def("__str__", &vlink::Uuid::to_string)
      .def("__repr__", [](const vlink::Uuid& self) { return std::string("<vlink.Uuid '") + self.to_string() + "'>"; })
      .def("__eq__", [](const vlink::Uuid& lhs, const vlink::Uuid& rhs) { return lhs == rhs; })
      .def("__ne__", [](const vlink::Uuid& lhs, const vlink::Uuid& rhs) { return lhs != rhs; })
      .def("__lt__", [](const vlink::Uuid& lhs, const vlink::Uuid& rhs) { return lhs < rhs; })
      .def("__hash__", [](const vlink::Uuid& self) { return static_cast<int64_t>(std::hash<vlink::Uuid>{}(self)); })
      .def_static(
          "is_valid", [](const std::string& str) { return vlink::Uuid::is_valid(std::string_view(str)); }, "str"_a,
          "Returns True when str is a well-formed UUID textual representation.")
      .def_static(
          "from_string",
          [](const std::string& str) -> nb::object {
            auto parsed = vlink::Uuid::from_string(std::string_view(str));

            if (!parsed.has_value()) {
              return nb::none();
            }

            return nb::cast(*parsed);
          },
          "str"_a, "Parses a UUID string; returns None on malformed input.")
      .def_static("generate_random", static_cast<vlink::Uuid (*)() noexcept>(&vlink::Uuid::generate_random),
                  "Generates a random v4 UUID using a thread-local seeded engine.")
      .def_static(
          "random_bytes",
          [](size_t count) {
            auto buf = vlink::Uuid::random_bytes(count);
            return nb::bytes(reinterpret_cast<const char*>(buf.data()), buf.size());
          },
          "count"_a, "Returns count cryptographically-seeded pseudo-random bytes (NOT a CSPRNG).")
      .def_static("random_hex", &vlink::Uuid::random_hex, "byte_count"_a = static_cast<size_t>(16U),
                  "Returns byte_count random bytes encoded as a lowercase hex string (NOT a CSPRNG).");
}

}  // namespace vlink::python
