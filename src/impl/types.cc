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

#include "./impl/types.h"

#include <charconv>
#include <sstream>
#include <string>

namespace vlink {

// SampleLostInfo
std::ostream& operator<<(std::ostream& ostream, const SampleLostInfo& info) noexcept {
  ostream << "SampleLostInfo:"
          << "[total]" << info.total << "[lost]" << info.lost;

  return ostream;
}

// SchemaData
bool SchemaData::is_valid_type(SchemaType schema_type) noexcept {
  switch (schema_type) {
    case SchemaType::kUnknown:
    case SchemaType::kProtobuf:
    case SchemaType::kFlatbuffers:
    case SchemaType::kRaw:
    case SchemaType::kZeroCopy:
      return true;
    default:
      return false;
  }
}

bool SchemaData::is_real_type(SchemaType schema_type) noexcept {
  switch (schema_type) {
    case SchemaType::kProtobuf:
    case SchemaType::kFlatbuffers:
    case SchemaType::kZeroCopy:
      return true;
    default:
      return false;
  }
}

std::string_view SchemaData::convert_type(SchemaType schema_type) noexcept {
  switch (schema_type) {
    case SchemaType::kProtobuf:
      return "protobuf";
    case SchemaType::kFlatbuffers:
      return "flatbuffers";
    case SchemaType::kRaw:
      return "raw";
    case SchemaType::kZeroCopy:
      return "zerocopy";
    default:
      return "";
  }
}

SchemaType SchemaData::convert_encoding(std::string_view encoding) noexcept {
  constexpr auto kIsAsciiSpace = [](char ch) noexcept {
    return ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r' || ch == '\f' || ch == '\v';
  };

  constexpr auto kAsciiLower = [](char ch) noexcept {
    return ch >= 'A' && ch <= 'Z' ? static_cast<char>(ch - 'A' + 'a') : ch;
  };

  const auto iequals = [kAsciiLower](std::string_view lhs, std::string_view rhs) noexcept {
    if (lhs.size() != rhs.size()) {
      return false;
    }

    for (size_t i = 0; i < lhs.size(); ++i) {
      if (kAsciiLower(lhs[i]) != kAsciiLower(rhs[i])) {
        return false;
      }
    }

    return true;
  };

  while (!encoding.empty() && kIsAsciiSpace(encoding.front())) {
    encoding.remove_prefix(1);
  }

  while (!encoding.empty() && kIsAsciiSpace(encoding.back())) {
    encoding.remove_suffix(1);
  }

  if (iequals(encoding, "protobuf") || iequals(encoding, "proto")) {
    return SchemaType::kProtobuf;
  }

  if (iequals(encoding, "flatbuffers") || iequals(encoding, "flatbuffer") || iequals(encoding, "fbs") ||
      iequals(encoding, "bfbs")) {
    return SchemaType::kFlatbuffers;
  }

  if (iequals(encoding, "raw") || iequals(encoding, "json") || iequals(encoding, "text") || iequals(encoding, "blob")) {
    return SchemaType::kRaw;
  }

  if (iequals(encoding, "string") || iequals(encoding, "std::string") || iequals(encoding, "application/json") ||
      iequals(encoding, "text/json")) {
    return SchemaType::kRaw;
  }

  if (iequals(encoding, "zerocopy") || iequals(encoding, "vlink_msg")) {
    return SchemaType::kZeroCopy;
  }

  return SchemaType::kUnknown;
}

SchemaType SchemaData::resolve_type(SchemaType schema_type, std::string_view ser_type,
                                    std::string_view encoding) noexcept {
  const auto normalized_schema_type = is_valid_type(schema_type) ? schema_type : SchemaType::kUnknown;

  if (normalized_schema_type != SchemaType::kUnknown) {
    return normalized_schema_type;
  }

  const auto inferred_encoding_type = convert_encoding(encoding);

  if (inferred_encoding_type != SchemaType::kUnknown) {
    return inferred_encoding_type;
  }

  return infer_ser_type(ser_type);
}

// Version
bool Version::operator==(const Version& target) const noexcept {
  return major == target.major && minor == target.minor && patch == target.patch;
}

bool Version::operator!=(const Version& target) const noexcept { return !(*this == target); }

bool Version::operator<(const Version& target) const noexcept {
  if (major != target.major) {
    return major < target.major;
  }

  if (minor != target.minor) {
    return minor < target.minor;
  }

  return patch < target.patch;
}

bool Version::operator>(const Version& target) const noexcept { return target < *this; }

Version Version::from_string(const std::string& version_str) noexcept {
  Version version;

  thread_local std::stringstream ss;
  ss.clear();
  ss.str(version_str);

  std::string token;

  if (std::getline(ss, token, '.')) {
    const auto* end = token.data() + token.size();
    auto result = std::from_chars(token.data(), end, version.major);
    if VUNLIKELY (result.ptr != end) {
      version.major = -1;
    }
  }

  if (std::getline(ss, token, '.')) {
    const auto* end = token.data() + token.size();
    auto result = std::from_chars(token.data(), end, version.minor);
    if VUNLIKELY (result.ptr != end) {
      version.minor = -1;
    }
  }

  if (std::getline(ss, token, '.')) {
    const auto* end = token.data() + token.size();
    auto result = std::from_chars(token.data(), end, version.patch);
    if VUNLIKELY (result.ptr != end) {
      version.patch = -1;
    }
  }

  return version;
}

std::string Version::to_string() const noexcept {
  return std::to_string(major) + "." + std::to_string(minor) + "." + std::to_string(patch);
}

bool Version::is_valid() const noexcept { return major >= 0 && minor >= 0 && patch >= 0; }

}  // namespace vlink
