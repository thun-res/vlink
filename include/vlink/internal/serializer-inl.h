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

#include <cstring>
#include <sstream>
#include <string>
#include <type_traits>
#include <utility>

#include "../base/helpers.h"
#include "../base/logger.h"
#include "../base/name_detector.h"
#include "../base/traits.h"
#include "../serializer.h"

#ifndef VLINK_FASTDDS_IDL_PREFIX
#define VLINK_FASTDDS_IDL_PREFIX "dds::"
#endif

// NOLINTBEGIN

#if __has_include(<fastcdr/Cdr.h>)
[[maybe_unused]] static constexpr bool kVlinkHasFastcdr = true;

#include <fastcdr/Cdr.h>
#else
#define FASTCDR_VERSION_MAJOR 1
[[maybe_unused]] static constexpr bool kVlinkHasFastcdr = false;

namespace eprosima::fastcdr {

class FastBuffer {
 public:
  explicit FastBuffer(char*, size_t) {}
};

class CdrVersion {
 public:
  enum Type {
    DDS_CDR,
  };
};

class Cdr {
 public:
  enum Type {
    DDS_CDR,
    DEFAULT_ENDIAN,
  };

  explicit Cdr(FastBuffer&, int, int) {}
};
}  // namespace eprosima::fastcdr
#endif

#if __has_include(<google/protobuf/message_lite.h>)
[[maybe_unused]] static constexpr bool kVlinkHasProtobuf = true;

#include <google/protobuf/message_lite.h>

#if __has_include(<google/protobuf/stubs/common.h>)
#include <google/protobuf/stubs/common.h>
#endif

#if __has_include(<google/protobuf/arena.h>)
#include <google/protobuf/arena.h>
#endif

#else

#ifndef GOOGLE_PROTOBUF_VERSION
#define GOOGLE_PROTOBUF_VERSION 0
#endif

[[maybe_unused]] static constexpr bool kVlinkHasProtobuf = false;

namespace google::protobuf {

struct Arena {
  template <typename T>
  [[maybe_unused]] static T* Create(Arena*) {
    return nullptr;
  }
};

}  // namespace google::protobuf

#endif

#if __has_include(<flatbuffers/flatbuffers.h>)
[[maybe_unused]] static constexpr bool kVlinkHasFlatbuffers = true;

#include <flatbuffers/flatbuffers.h>
#else
[[maybe_unused]] static constexpr bool kVlinkHasFlatbuffers = false;

namespace flatbuffers {

template <typename ReturnT, typename T>
[[maybe_unused]] static const ReturnT* GetRoot(const T&) {
  return nullptr;
}

struct FlatBufferBuilder {
  size_t GetSize() const { return 0; }

  const uint8_t* GetBufferPointer() const { return nullptr; }

  const uint8_t* GetCurrentBufferPointer() const { return nullptr; }

  void PushBytes(const uint8_t*, size_t) {}

  void Finish() const {}
};

struct Table {};

struct NativeTable {};

struct Verifier {
  Verifier(const uint8_t*, size_t) {}

  template <typename T>
  bool VerifyBuffer(void*) {
    return false;
  }
};

}  // namespace flatbuffers
#endif

// NOLINTEND

namespace vlink {

namespace Serializer {  // NOLINT(readability-identifier-naming)

[[maybe_unused]] inline constexpr bool is_supported(Type type) noexcept { return type != kUnknownType; }

template <typename T>
inline constexpr Type get_type_of() noexcept {
  if constexpr (is_bytes_type<T>()) {
    return kBytesType;
  } else if constexpr (is_dynamic_type<T>()) {
    return kDynamicType;
  } else if constexpr (is_cdr_type<T>()) {
    return kCdrType;
  } else if constexpr (is_proto_type<T>()) {
    return kProtoType;
  } else if constexpr (is_proto_ptr_type<T>()) {
    return kProtoPtrType;
  } else if constexpr (is_flat_table_type<T>()) {
    return kFlatTableType;
  } else if constexpr (is_flat_ptr_type<T>()) {
    return kFlatPtrType;
  } else if constexpr (is_flat_builder_type<T>()) {
    return kFlatBuilderType;
  } else if constexpr (is_custom_type<T>()) {
    return kCustomType;
  } else if constexpr (is_string_type<T>()) {
    return kStringType;
  } else if constexpr (is_chars_type<T>()) {
    return kCharsType;
  } else if constexpr (is_standard_type<T>()) {
    return kStandardType;
  } else if constexpr (is_standard_ptr_type<T>()) {
    return kStandardPtrType;
  } else if constexpr (is_stream_type<T>()) {
    return kStreamType;
  } else {
    return kUnknownType;
  }
}

template <Type TypeT, typename T>
inline static constexpr SchemaType get_schema_type() noexcept {
  using RealType = typename Traits::RemoveSharedPtr<T>::Type;

  if constexpr (TypeT == kBytesType) {
    return SchemaType::kRaw;
  } else if constexpr (TypeT == kCustomType) {
    if constexpr (VLINK_HAS_MEMBER(RealType, kZerocopyTypes)) {
      if constexpr (RealType::kZerocopyTypes) {
        return SchemaType::kZeroCopy;
      } else {
        return SchemaType::kRaw;
      }
    } else if constexpr (VLINK_HAS_MEMBER(RealType, get_schema_type())) {
      return RealType::get_schema_type();
    } else {
      return SchemaType::kRaw;
    }
  } else if constexpr (TypeT == kProtoType || TypeT == kProtoPtrType) {
    return SchemaType::kProtobuf;
  } else if constexpr (TypeT == kFlatTableType || TypeT == kFlatPtrType || TypeT == kFlatBuilderType) {
    return SchemaType::kFlatbuffers;
  } else {
    return SchemaType::kRaw;
  }
}

template <typename T>
inline static constexpr SchemaType get_schema_type() noexcept {
  constexpr auto kType = get_type_of<T>();

  return get_schema_type<kType, T>();
}

template <Type TypeT, typename T>
inline std::string get_serialized_type() noexcept {
  using RealType = typename Traits::RemoveSharedPtr<T>::Type;
  using NamedType = std::remove_pointer_t<RealType>;

  if constexpr (TypeT == kBytesType) {
    return "";
  } else if constexpr (TypeT == kDynamicType) {
    return "vlink::DynamicData";
  } else if constexpr (TypeT == kCustomType && VLINK_HAS_MEMBER(RealType, get_serialized_type())) {
    return RealType::get_serialized_type();
  } else if constexpr (TypeT == kProtoType) {
#if GOOGLE_PROTOBUF_VERSION >= 6030000
    return std::string(RealType{}.GetTypeName());
#else
    return RealType{}.GetTypeName();
#endif
  } else if constexpr (TypeT == kProtoPtrType) {
#if GOOGLE_PROTOBUF_VERSION >= 6030000
    return std::string(std::remove_pointer_t<T>().GetTypeName());
#else
    return std::remove_pointer_t<T>().GetTypeName();
#endif
  } else if constexpr (TypeT == kFlatTableType) {
    using TableType = typename RealType::TableType;
    if constexpr (VLINK_HAS_MEMBER(TableType, GetFullyQualifiedName)) {
      return TableType::GetFullyQualifiedName();
    } else {
      std::string name = NameDetector::get<TableType>().data();
      Helpers::replace_string(name, "::", ".");
      return name;
    }
  } else if constexpr (TypeT == kFlatBuilderType) {
    using TableType = typename RealType::Table;
    if constexpr (VLINK_HAS_MEMBER(TableType, GetFullyQualifiedName)) {
      return TableType::GetFullyQualifiedName();
    } else {
      std::string name = NameDetector::get<TableType>().data();
      Helpers::replace_string(name, "::", ".");
      return name;
    }
  } else if constexpr (TypeT == kFlatPtrType) {
    using TableType = std::remove_pointer_t<T>;
    if constexpr (VLINK_HAS_MEMBER(TableType, GetFullyQualifiedName)) {
      return TableType::GetFullyQualifiedName();
    } else {
      std::string name = NameDetector::get<TableType>().data();
      Helpers::replace_string(name, "::", ".");
      return name;
    }
  } else if constexpr (TypeT == kStringType || TypeT == kCharsType) {
    return "string";
  } else if constexpr (NameDetector::is_support<NamedType>()) {
    return NameDetector::get<NamedType>().data();
  } else {
    return "";
  }
}

template <typename T>
inline std::string get_serialized_type() noexcept {
  constexpr auto kType = get_type_of<T>();

  return get_serialized_type<kType, T>();
}

template <Type TypeT, typename T>
inline size_t get_serialized_size(const T& src) noexcept {
  using RealType = typename Traits::RemoveSharedPtr<T>::Type;

  if constexpr (TypeT == kBytesType) {
    return 0;
  } else if constexpr (TypeT == kDynamicType) {
    return 0;
  } else if constexpr (TypeT == kCdrType) {
    if constexpr (VLINK_HAS_MEMBER(RealType, getCdrSerializedSize(deref(src)))) {
      return RealType::getCdrSerializedSize(deref(src));
    } else {
      return 0;
    }
  } else if constexpr (TypeT == kProtoType) {
    if constexpr (VLINK_HAS_MEMBER(RealType, ByteSizeLong())) {
      return deref(src).ByteSizeLong();
    } else {
      return deref(src).ByteSize();
    }
  } else if constexpr (TypeT == kProtoPtrType) {
    if constexpr (VLINK_HAS_MEMBER(std::remove_pointer_t<T>, ByteSizeLong())) {
      return src->ByteSizeLong();
    } else {
      return src->ByteSize();
    }
  } else if constexpr (TypeT == kFlatTableType) {
    return 0;
  } else if constexpr (TypeT == kFlatBuilderType) {
    return src.fbb_.GetSize();
  } else if constexpr (TypeT == kFlatPtrType) {
    return 0;
  } else if constexpr (TypeT == kCustomType) {
    if constexpr (VLINK_HAS_MEMBER(RealType, get_serialized_size())) {
      return deref(src).get_serialized_size();
    } else {
      return 0;
    }
  } else if constexpr (TypeT == kStringType) {
    return 0;
  } else if constexpr (TypeT == kCharsType) {
    return 0;
  } else if constexpr (TypeT == kStreamType) {
    return 0;
  } else if constexpr (TypeT == kStandardType) {
    return 0;
  } else if constexpr (TypeT == kStandardPtrType) {
    return 0;
  } else {
    return 0;
  }
}

template <typename T>
inline size_t get_serialized_size(const T& src) noexcept {
  constexpr auto kType = get_type_of<T>();

  return get_serialized_size<kType>(src);
}

template <Type TypeT, typename T>
inline bool serialize(const T& src, Bytes& des, [[maybe_unused]] TransportType transport,
                      [[maybe_unused]] uint8_t offset) {
  using RealType = typename Traits::RemoveSharedPtr<T>::Type;

  if constexpr (TypeT == kBytesType) {
    des = Bytes::deep_copy(src.data(), src.size(), offset);
  } else if constexpr (TypeT == kDynamicType) {
    using ReturnT = decltype(deref(src) >> des);

    if constexpr (std::is_convertible_v<ReturnT, bool>) {
      if VUNLIKELY (!(deref(src) >> des)) {
        VLOG_T("Serializer: Dynamic serialize failed.");
        return false;
      }
    } else {
      deref(src) >> des;
    }
  } else if constexpr (TypeT == kCdrType) {
    if (transport == TransportType::kDds) {
      des = Bytes::shallow_copy_ptr(&const_cast<RealType&>(deref(src)));
    } else {
      if (!des.is_loaned()) {
        if constexpr (VLINK_HAS_MEMBER(RealType, getCdrSerializedSize(deref(src)))) {
          size_t target_size = RealType::getCdrSerializedSize(deref(src));

          if VUNLIKELY (des.size() != target_size) {
            des = Bytes::create(target_size, offset);
          }
        } else {
          VLOG_W("Serializer: FastBuffer serialize is not supported without dds(v3).");
          return false;
        }
      }

      eprosima::fastcdr::FastBuffer buffer(reinterpret_cast<char*>(des.data()), des.size());
#if FASTCDR_VERSION_MAJOR >= 2
      eprosima::fastcdr::Cdr cdr(buffer, eprosima::fastcdr::Cdr::DEFAULT_ENDIAN,
                                 eprosima::fastcdr::CdrVersion::DDS_CDR);
#else
      eprosima::fastcdr::Cdr cdr(buffer, eprosima::fastcdr::Cdr::DEFAULT_ENDIAN, eprosima::fastcdr::Cdr::DDS_CDR);
#endif
      deref(src).serialize(cdr);
    }
  } else if constexpr (TypeT == kProtoType) {
    if (!des.is_loaned()) {
      if constexpr (VLINK_HAS_MEMBER(RealType, ByteSizeLong())) {
        size_t target_size = deref(src).ByteSizeLong();

        if VUNLIKELY (des.size() != target_size) {
          des = Bytes::create(target_size, offset);
        }
      } else {
        size_t target_size = deref(src).ByteSize();

        if VUNLIKELY (des.size() != target_size) {
          des = Bytes::create(target_size, offset);
        }
      }
    }

    if VUNLIKELY (!deref(src).SerializeToArray(des.data(), des.size())) {
      VLOG_T("Serializer: Protobuf serialize failed.");
      return false;
    }
  } else if constexpr (TypeT == kProtoPtrType) {
    if (!des.is_loaned()) {
      if constexpr (VLINK_HAS_MEMBER(std::remove_pointer_t<T>, ByteSizeLong())) {
        size_t target_size = src->ByteSizeLong();

        if VUNLIKELY (des.size() != target_size) {
          des = Bytes::create(target_size, offset);
        }
      } else {
        size_t target_size = src->ByteSize();

        if VUNLIKELY (des.size() != target_size) {
          des = Bytes::create(target_size, offset);
        }
      }
    }

    if VUNLIKELY (!src->SerializeToArray(des.data(), des.size())) {
      VLOG_T("Serializer: Protobuf ptr serialize failed.");
      return false;
    }
  } else if constexpr (TypeT == kFlatTableType) {
    flatbuffers::FlatBufferBuilder fbb;
    fbb.Finish(T::TableType::Pack(fbb, &deref(src)));

    if (des.is_loaned() && des.size() >= fbb.GetSize() + offset) {
      std::memcpy(des.data() + offset, fbb.GetBufferPointer(), fbb.GetSize());
    } else {
      des = Bytes::deep_copy(fbb.GetBufferPointer(), fbb.GetSize(), offset);
    }
  } else if constexpr (TypeT == kFlatBuilderType) {
    src.fbb_.Finish(const_cast<T&>(src).Finish());

    if (des.is_loaned()) {
      des = Bytes::shallow_copy(src.fbb_.GetBufferPointer(), src.fbb_.GetSize());
    } else {
      des = Bytes::deep_copy(src.fbb_.GetBufferPointer(), src.fbb_.GetSize(), offset);
    }
  } else if constexpr (TypeT == kFlatPtrType) {
    static_assert(Traits::ExpectFalse<T>(), "Not support flat ptr type.");
  } else if constexpr (TypeT == kCustomType) {
    using ReturnT = decltype(const_cast<RealType&>(deref(src)) >> des);

    if constexpr (std::is_convertible_v<ReturnT, bool>) {
      if VUNLIKELY (!(const_cast<RealType&>(deref(src)) >> des)) {
        VLOG_T("Serializer: Custom serialize failed.");
        return false;
      }
    } else {
      const_cast<RealType&>(deref(src)) >> des;
    }

    if (offset > 0) {
      des = Bytes::deep_copy(des.data(), des.size(), offset);
    }
  } else if constexpr (TypeT == kStringType) {
    des = Bytes::deep_copy(reinterpret_cast<const uint8_t*>(deref(src).data()), deref(src).size(), offset);
  } else if constexpr (TypeT == kCharsType) {
    des = Bytes::deep_copy(reinterpret_cast<const uint8_t*>(src), std::strlen(src), offset);
  } else if constexpr (TypeT == kStreamType) {
    thread_local std::stringstream ss;
    ss.clear();
    ss.str("");

    ss << deref(src);
    const std::string& str = ss.str();
    des = Bytes::deep_copy(reinterpret_cast<const uint8_t*>(str.data()), str.size(), offset);
  } else if constexpr (TypeT == kStandardType) {
    const auto* src_ptr = reinterpret_cast<const uint8_t*>(&deref(src));
    des = Bytes::deep_copy(src_ptr, sizeof(RealType), offset);
  } else if constexpr (TypeT == kStandardPtrType) {
    if VUNLIKELY (offset > 0) {
      VLOG_T("Serializer: Standard ptr does not support offset.");
      return false;
    }

    const auto* src_ptr = reinterpret_cast<const uint8_t*>(src);
    constexpr size_t kSize = sizeof(std::remove_pointer_t<T>);

    if (des.is_loaned() && des.size() >= kSize) {
      std::memcpy(des.data(), src_ptr, kSize);
    } else {
      des = Bytes::shallow_copy(src_ptr, kSize);
    }
  } else {
    static_assert(Traits::ExpectFalse<T>(), "Not support serialize.");
    return false;
  }

  return true;
}

template <typename T>
inline bool serialize(const T& src, Bytes& des) {
  constexpr auto kType = get_type_of<T>();

  return serialize<kType>(src, des, TransportType::kUnknown);
}

template <Type TypeT, typename T>
inline bool deserialize(const Bytes& src, T& des, [[maybe_unused]] TransportType transport) {
  using RealType = typename Traits::RemoveSharedPtr<T>::Type;

  if constexpr (TypeT == kBytesType) {
    des = src;
    return true;
  } else if constexpr (TypeT == kDynamicType) {
    try {
      using ReturnT = decltype(deref(des) << src);

      if constexpr (std::is_convertible_v<ReturnT, bool>) {
        if VUNLIKELY (!(deref(des) << src)) {
          VLOG_T("Serializer: Dynamic deserialize failed.");
          return false;
        }
      } else {
        deref(des) << src;
      }
    } catch (const std::exception& e) {
      VLOG_T("Serializer: Dynamic deserialize threw: ", e.what(), ".");
      return false;
    }
  } else if constexpr (TypeT == kCdrType) {
    if (transport == TransportType::kDds) {
      if VUNLIKELY (!src.is_ptr()) {
        VLOG_T("Serializer: Fastcdr src is not ptr.");
        return false;
      }

      deref(des) = *src.to_ptr<RealType>();
    } else {
      if constexpr (VLINK_HAS_MEMBER(RealType, getCdrSerializedSize(deref(des)))) {
        eprosima::fastcdr::FastBuffer buffer(reinterpret_cast<char*>(const_cast<uint8_t*>(src.data())), src.size());
#if FASTCDR_VERSION_MAJOR >= 2
        eprosima::fastcdr::Cdr cdr(buffer, eprosima::fastcdr::Cdr::DEFAULT_ENDIAN,
                                   eprosima::fastcdr::CdrVersion::DDS_CDR);
#else
        eprosima::fastcdr::Cdr cdr(buffer, eprosima::fastcdr::Cdr::DEFAULT_ENDIAN, eprosima::fastcdr::Cdr::DDS_CDR);
#endif
        deref(des).deserialize(cdr);
      } else {
        VLOG_W("Serializer: FastBuffer deserialize is not supported without dds(v3).");
        return false;
      }
    }
  } else if constexpr (TypeT == kProtoType) {
    if (!src.empty()) {
      if VUNLIKELY (!deref(des).ParseFromArray(src.data(), src.size())) {
        VLOG_T("Serializer: Protobuf deserialize failed.");
        return false;
      }
    } else {
      deref(des).Clear();
    }
  } else if constexpr (TypeT == kProtoPtrType) {
    if (!src.empty()) {
      if VUNLIKELY (!des->ParseFromArray(src.data(), src.size())) {
        VLOG_T("Serializer: Protobuf ptr deserialize failed.");
        return false;
      }
    } else {
      des->Clear();
    }
  } else if constexpr (TypeT == kFlatTableType) {
    deref(des) = RealType{};

    if VLIKELY (!src.empty()) {
      flatbuffers::Verifier verifier(src.data(), src.size());

      if VUNLIKELY (!verifier.VerifyBuffer<typename T::TableType>(nullptr)) {
        VLOG_T("Serializer: Flatbuffers table deserialize failed.");
        return false;
      }

      auto target = flatbuffers::GetRoot<typename RealType::TableType>(src.data());
      target->UnPackTo(&deref(des));
    }
  } else if constexpr (TypeT == kFlatPtrType) {
    flatbuffers::Verifier verifier(src.data(), src.size());

    if VUNLIKELY (!verifier.VerifyBuffer<std::remove_pointer_t<T>>(nullptr)) {
      VLOG_T("Serializer: Flatbuffers ptr verify failed.");
      return false;
    }

    des = const_cast<T>(flatbuffers::GetRoot<std::remove_pointer_t<T>>(src.data()));

    if VUNLIKELY (!des) {
      VLOG_T("Serializer: Flatbuffers ptr deserialize failed.");
      return false;
    }
  } else if constexpr (TypeT == kCustomType) {
    try {
      using ReturnT = decltype(deref(des) << src);

      if constexpr (std::is_convertible_v<ReturnT, bool>) {
        if VUNLIKELY (!(deref(des) << src)) {
          VLOG_T("Serializer: Custom deserialize failed.");
          return false;
        }
      } else {
        deref(des) << src;
      }
    } catch (const std::exception& e) {
      VLOG_T("Serializer: Custom deserialize threw: ", e.what(), ".");
      return false;
    }
  } else if constexpr (TypeT == kStringType) {
    if VLIKELY (!src.empty()) {
      deref(des) = std::string(reinterpret_cast<const char*>(src.data()), src.size());
    } else {
      deref(des) = std::string();
    }
  } else if constexpr (TypeT == kCharsType) {
    if VLIKELY (!src.empty()) {
      des = reinterpret_cast<const char*>(src.data());
    } else {
      des = "";
    }
  } else if constexpr (TypeT == kStreamType) {
    thread_local std::stringstream ss;
    ss.clear();
    ss.str(std::string(reinterpret_cast<const char*>(src.data()), src.size()));

    ss >> deref(des);

    if VUNLIKELY (ss.fail()) {
      VLOG_T("Serializer: Stream deserialize failed.");
      return false;
    }
  } else if constexpr (TypeT == kStandardType) {
    if VLIKELY (!src.empty() && src.size() == sizeof(RealType)) {
      std::memcpy(&deref(des), src.data(), src.size());
    } else {
      VLOG_T("Serializer: Standard layout deserialize failed.");
      return false;
    }
  } else if constexpr (TypeT == kStandardPtrType) {
    if VLIKELY (!src.empty() && src.size() == sizeof(std::remove_pointer_t<T>)) {
      des = reinterpret_cast<T>(const_cast<uint8_t*>(src.data()));
    } else {
      VLOG_T("Serializer: Standard ptr layout deserialize failed.");
      return false;
    }
  } else {
    static_assert(Traits::ExpectFalse<T>(), "Not support deserialize.");
    return false;
  }

  return true;
}

template <typename T>
inline bool deserialize(const Bytes& src, T& des) {
  constexpr auto kType = get_type_of<T>();
  return deserialize<kType>(src, des, TransportType::kUnknown);
}

template <typename SrcT, typename DesT>
inline bool convert(const SrcT& src, DesT& des) {
  static_assert(is_bytes_type<SrcT>() || is_bytes_type<DesT>(), "SrcT or DesT must be Bytes.");

  if constexpr (is_bytes_type<SrcT>() && is_bytes_type<DesT>()) {
    des.shallow_copy(src);
    return true;
  } else if constexpr (is_bytes_type<DesT>()) {
    return serialize(src, des);
  } else {
    return deserialize(src, des);
  }
}

template <typename T>
inline constexpr auto& deref(const T& t) noexcept {
  if constexpr (Traits::IsSharedPtr<T>()) {
    return *const_cast<T&>(t).get();
  } else {
    return const_cast<T&>(t);
  }
}

template <typename T>
inline constexpr bool is_bytes_type() noexcept {
  return std::is_same_v<T, Bytes>;
}

template <typename T>
inline constexpr bool is_dynamic_type() noexcept {
  using RealType = typename Traits::RemoveSharedPtr<T>::Type;
  return VLINK_HAS_MEMBER(RealType, is_vlink_dynamic_data());
}

template <typename T>
inline constexpr bool is_cdr_type() noexcept {
  using RealType = typename Traits::RemoveSharedPtr<T>::Type;

  return kVlinkHasFastcdr && ((VLINK_HAS_MEMBER(RealType, serialize(std::declval<eprosima::fastcdr::Cdr&>())) &&
                               VLINK_HAS_MEMBER(RealType, deserialize(std::declval<eprosima::fastcdr::Cdr&>()))) ||
                              Helpers::contains_substring(NameDetector::get<RealType>(), VLINK_FASTDDS_IDL_PREFIX));
}

template <typename T>
inline constexpr bool is_proto_type() noexcept {
  using RealType = typename Traits::RemoveSharedPtr<T>::Type;
  return kVlinkHasProtobuf && VLINK_HAS_MEMBER(RealType, SerializeToArray(0, 0)) &&
         VLINK_HAS_MEMBER(RealType, ParseFromArray(0, 0));
}

template <typename T>
inline constexpr bool is_proto_ptr_type() noexcept {
  return kVlinkHasProtobuf && std::is_pointer_v<T> &&
         VLINK_HAS_MEMBER(std::remove_pointer_t<T>, SerializeToArray(0, 0)) &&
         VLINK_HAS_MEMBER(std::remove_pointer_t<T>, ParseFromArray(0, 0));
}

template <typename T>
inline constexpr bool is_flat_table_type() noexcept {
  using RealType = typename Traits::RemoveSharedPtr<T>::Type;
  return kVlinkHasFlatbuffers && std::is_base_of_v<flatbuffers::NativeTable, RealType>;
}

template <typename T>
inline constexpr bool is_flat_builder_type() noexcept {
  return kVlinkHasFlatbuffers && VLINK_HAS_MEMBER(T, fbb_) && VLINK_HAS_MEMBER(T, Finish());
}

template <typename T>
inline constexpr bool is_flat_ptr_type() noexcept {
  return kVlinkHasFlatbuffers && std::is_pointer_v<T> &&
         std::is_base_of_v<flatbuffers::Table, std::remove_pointer_t<T>>;
}

template <typename T>
inline constexpr bool is_custom_type() noexcept {
  using RealType = typename Traits::RemoveSharedPtr<T>::Type;
  return Traits::Operatorable<std::decay_t<RealType>, Bytes>();
}

template <typename T>
inline constexpr bool is_string_type() noexcept {
  using RealType = typename Traits::RemoveSharedPtr<T>::Type;
  return std::is_same_v<RealType, std::string>;
}

template <typename T>
inline constexpr bool is_chars_type() noexcept {
  return !std::is_same_v<T, std::string> && std::is_constructible_v<std::string, T>;
}

template <typename T>
inline constexpr bool is_stream_type() noexcept {
  using RealType = typename Traits::RemoveSharedPtr<T>::Type;
  return !std::is_pointer_v<RealType> && Traits::Operatorable<std::stringstream, std::decay_t<RealType>>();
}

template <typename T>
inline constexpr bool is_standard_type() noexcept {
  using RealType = typename Traits::RemoveSharedPtr<T>::Type;
  return !std::is_pointer_v<RealType> && std::is_trivial_v<RealType> && std::is_standard_layout_v<RealType>;
}

template <typename T>
inline constexpr bool is_standard_ptr_type() noexcept {
  return std::is_pointer_v<T> && std::is_trivial_v<std::remove_pointer_t<T>> &&
         std::is_standard_layout_v<std::remove_pointer_t<T>>;
}

}  // namespace Serializer

}  // namespace vlink
