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

/**
 * @file name_detector.h
 * @brief Header-only compile-time introspection of type names and enumerator labels.
 *
 * @details
 * @c vlink::NameDetector exposes two primitives that resolve human-readable identifiers
 * at compile time without RTTI or any third-party dependency.  The implementation walks
 * the textual content of @c __PRETTY_FUNCTION__ on Clang/GCC and @c __FUNCSIG__ on MSVC,
 * then trims compiler-specific noise so the result is identical across toolchains.
 *
 * Validation rules applied by the internal @c pretty_name() pass:
 *
 * | Rule                                  | Result                                       |
 * | ------------------------------------- | -------------------------------------------- |
 * | Begins with a character literal quote | Returns an empty view (not a valid name)     |
 * | Begins with a digit                   | Returns an empty view (numeric literal)      |
 * | Contains a trailing parenthesised arg | Strips the argument list before the name     |
 * | Contains a trailing template list     | Strips the @c "<...>" suffix from the name   |
 * | Final character is not alnum/under    | Trims back to the last identifier character  |
 * | First character is not letter/under   | Returns an empty view (rejects the result)   |
 *
 * @par Example
 * @code
 * static_assert(vlink::NameDetector::is_support<MyPlugin>());
 * constexpr std::string_view kName = vlink::NameDetector::get<MyPlugin>();
 *
 * enum class Color { Red, Green, Blue };
 * std::string_view label = vlink::NameDetector::get_enum(Color::Green);
 * @endcode
 */

#pragma once

#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string_view>
#include <type_traits>
#include <utility>

#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunknown-warning-option"
#pragma clang diagnostic ignored "-Wenum-constexpr-conversion"
#elif defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wstringop-overflow"
#elif defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 26495)
#pragma warning(disable : 28020)

#pragma warning(disable : 26451)
#pragma warning(disable : 4514)
#endif

#if defined(__clang__) && __clang_major__ >= 5 || defined(__GNUC__) && __GNUC__ >= 7 || \
    defined(_MSC_VER) && _MSC_VER >= 1910
#undef VLINK_NAME_DETECTOR_TYPE_SUPPORTED
#define VLINK_NAME_DETECTOR_TYPE_SUPPORTED 1
#endif

#if defined(__clang__) && __clang_major__ >= 5 || defined(__GNUC__) && __GNUC__ >= 9 || \
    defined(_MSC_VER) && _MSC_VER >= 1910
#undef VLINK_NAME_DETECTOR_ENUM_SUPPORTED
#define VLINK_NAME_DETECTOR_ENUM_SUPPORTED 1
#endif

#if !defined(VLINK_NAME_DETECTOR_ENUM_RANGE_MIN)
#define VLINK_NAME_DETECTOR_ENUM_RANGE_MIN -128
#endif

#if !defined(VLINK_NAME_DETECTOR_ENUM_RANGE_MAX)
#define VLINK_NAME_DETECTOR_ENUM_RANGE_MAX 128
#endif

namespace vlink {

/**
 * @namespace vlink::NameDetector
 * @brief Compile-time type-name and enum-name reflection primitives.
 */
namespace NameDetector {  // NOLINT(readability-identifier-naming)

/**
 * @brief Reports whether the current toolchain can extract a textual name for @p TypeT.
 *
 * @tparam TypeT  Type whose support status is queried.
 * @return @c true when @c get<TypeT>() returns a meaningful non-empty view on this compiler.
 */
template <typename TypeT>
[[nodiscard]] constexpr bool is_support() noexcept;

/**
 * @brief Returns the trimmed compile-time identifier for @p TypeT.
 *
 * @details
 * MSVC inserts a @c "struct " or @c "class " prefix into @c __FUNCSIG__; both are stripped
 * before the value is returned so the same textual identifier is produced on every supported
 * toolchain.  The returned view always points at static read-only storage.
 *
 * @tparam TypeT  Type whose identifier is requested.
 * @return Read-only @c std::string_view with the resolved identifier.
 */
template <typename TypeT>
[[nodiscard]] constexpr std::string_view get() noexcept;

/**
 * @brief Resolves the source identifier of an enumerator literal at compile time.
 *
 * @tparam EnumT  Enumeration type, deduced from @p value.
 * @param  value  Enumerator value whose label is requested.
 * @return Identifier text, or an empty view when @p value is outside the configured
 *         scanning window or is not a named enumerator.
 */
template <typename EnumT>
[[nodiscard]] constexpr std::string_view get_enum(EnumT value) noexcept;

/**
 * @namespace vlink::NameDetector::customize
 * @brief Customisation points overriding the default enumerator scanning window.
 *
 * @details
 * Specialise @c EnumRange<EnumT> on a per-type basis to widen or narrow the
 * compile-time search bounds used by @c get_enum().
 */
namespace customize {  // NOLINT(readability-identifier-naming)

/**
 * @struct EnumRange
 * @brief Inclusive integer range scanned for @p EnumT enumerator labels.
 *
 * @tparam EnumT  Enumeration type for which the range is being customised.
 */
template <typename EnumT>
struct EnumRange {
  static_assert(std::is_enum_v<EnumT>, "vlink::NameDetector::customize::EnumRange requires enum type.");
  inline static constexpr int kMin = VLINK_NAME_DETECTOR_ENUM_RANGE_MIN;  ///< Inclusive lower scan bound.
  inline static constexpr int kMax = VLINK_NAME_DETECTOR_ENUM_RANGE_MAX;  ///< Inclusive upper scan bound.
  static_assert(kMax > kMin, "vlink::NameDetector::customize::EnumRange requires kMax > kMin.");
};

static_assert(VLINK_NAME_DETECTOR_ENUM_RANGE_MIN <= 0,
              "VLINK_NAME_DETECTOR_ENUM_RANGE_MIN must be less or equals than 0.");
// NOLINTNEXTLINE(readability-redundant-parentheses)
static_assert(VLINK_NAME_DETECTOR_ENUM_RANGE_MIN > (std::numeric_limits<int16_t>::min)(),
              "VLINK_NAME_DETECTOR_ENUM_RANGE_MIN must be greater than INT16_MIN.");

static_assert(VLINK_NAME_DETECTOR_ENUM_RANGE_MAX > 0, "VLINK_NAME_DETECTOR_ENUM_RANGE_MAX must be greater than 0.");
// NOLINTNEXTLINE(readability-redundant-parentheses)
static_assert(VLINK_NAME_DETECTOR_ENUM_RANGE_MAX < (std::numeric_limits<int16_t>::max)(),
              "VLINK_NAME_DETECTOR_ENUM_RANGE_MAX must be less than INT16_MAX.");

static_assert(VLINK_NAME_DETECTOR_ENUM_RANGE_MAX > VLINK_NAME_DETECTOR_ENUM_RANGE_MIN,
              "VLINK_NAME_DETECTOR_ENUM_RANGE_MAX must be greater than VLINK_NAME_DETECTOR_ENUM_RANGE_MIN.");

}  // namespace customize

}  // namespace NameDetector

////////////////////////////////////////////////////////////////
/// Details
////////////////////////////////////////////////////////////////

namespace NameDetector {
namespace detail {  // NOLINT(readability-identifier-naming)

template <typename TypeT>
struct TypeNameSupported
#if defined(VLINK_NAME_DETECTOR_TYPE_SUPPORTED) && VLINK_NAME_DETECTOR_TYPE_SUPPORTED
    : std::true_type {
};
#else
    : std::false_type {
};
#endif

template <typename TypeT>
struct EnumNameSupported
#if defined(VLINK_NAME_DETECTOR_ENUM_SUPPORTED) && VLINK_NAME_DETECTOR_ENUM_SUPPORTED
    : std::true_type {
};
#else
    : std::false_type {
};
#endif

template <typename TypeT>
inline constexpr bool kIsEnum = std::is_enum_v<TypeT> && std::is_same_v<TypeT, std::decay_t<TypeT>>;

template <typename TypeT>
using RemoveCvrefT = std::remove_cv_t<std::remove_reference_t<TypeT>>;

template <uint16_t SizeT>
class [[nodiscard]] CString {
 public:
  constexpr explicit CString(std::string_view str) noexcept
      : CString{str, std::make_integer_sequence<uint16_t, SizeT>{}} {
    assert(str.size() == SizeT);
  }
  constexpr CString() noexcept : chars_{} {}
  constexpr CString(const CString&) = default;
  constexpr CString(CString&&) noexcept = default;
  ~CString() = default;
  CString& operator=(const CString&) = default;
  CString& operator=(CString&&) noexcept = default;

  [[nodiscard]] constexpr const char* data() const noexcept { return chars_; }
  [[nodiscard]] constexpr uint16_t size() const noexcept { return SizeT; }
  [[nodiscard]] constexpr bool empty() const noexcept { return SizeT == 0; }

  // NOLINTNEXTLINE(google-explicit-constructor,hicpp-explicit-conversions)
  [[nodiscard]] constexpr operator std::string_view() const noexcept { return std::string_view{data(), size()}; }

 private:
  template <uint16_t... IndexT>
  constexpr CString(std::string_view str, std::integer_sequence<uint16_t, IndexT...>) noexcept
      : chars_{str[IndexT]..., '\0'} {}

  // NOLINTNEXTLINE(runtime/arrays)
  char chars_[static_cast<size_t>(SizeT) + 1];
};

constexpr std::string_view pretty_name(std::string_view name, bool remove_suffix = true) noexcept {
  if (!name.empty() && (name[0] == '"' || name[0] == '\'')) {
    return {};
  } else if (name.size() >= 2 && name[0] == 'R' && (name[1] == '"' || name[1] == '\'')) {
    return {};
  } else if (name.size() >= 2 && name[0] == 'L' && (name[1] == '"' || name[1] == '\'')) {
    return {};
  } else if (name.size() >= 2 && name[0] == 'U' && (name[1] == '"' || name[1] == '\'')) {
    return {};
  } else if (name.size() >= 2 && name[0] == 'u' && (name[1] == '"' || name[1] == '\'')) {
    return {};
  } else if (name.size() >= 3 && name[0] == 'u' && name[1] == '8' && (name[2] == '"' || name[2] == '\'')) {
    return {};
  } else if (!name.empty() && (name[0] >= '0' && name[0] <= '9')) {
    return {};
  }

  for (size_t i = name.size(), h = 0, s = 0; i > 0; --i) {
    if (name[i - 1] == ')') {
      ++h;
      ++s;
      continue;
    } else if (name[i - 1] == '(') {
      --h;
      ++s;
      continue;
    }

    if (h == 0) {
      name.remove_suffix(s);
      break;
    } else {
      ++s;
      continue;
    }
  }

  size_t s = 0;
  for (size_t i = name.size(), h = 0; i > 0; --i) {
    if (name[i - 1] == '>') {
      ++h;
      ++s;
      continue;
    } else if (name[i - 1] == '<') {
      --h;
      ++s;
      continue;
    }

    if (h == 0) {
      break;
    } else {
      ++s;
      continue;
    }
  }

  for (size_t i = name.size() - s; i > 0; --i) {
    if (!((name[i - 1] >= '0' && name[i - 1] <= '9') || (name[i - 1] >= 'a' && name[i - 1] <= 'z') ||
          (name[i - 1] >= 'A' && name[i - 1] <= 'Z') || (name[i - 1] == '_'))) {
      name.remove_prefix(i);
      break;
    }
  }

  if (remove_suffix) {
    name.remove_suffix(s);
  }

  if (!name.empty() && ((name[0] >= 'a' && name[0] <= 'z') || (name[0] >= 'A' && name[0] <= 'Z') || (name[0] == '_'))) {
    return name;
  }

  return {};
}

#if defined(__cpp_lib_array_constexpr) && __cpp_lib_array_constexpr >= 201603L
#define VLINK_NAME_DETECTOR_ARRAY_CONSTEXPR 1
#else
template <typename TypeT, size_t SizeT, size_t... IndexT>
constexpr std::array<std::remove_cv_t<TypeT>, SizeT> to_array(TypeT (&a)[SizeT],
                                                              std::index_sequence<IndexT...>) noexcept {
  return {{a[IndexT]...}};
}
#endif

template <typename LeftT, typename RightT>
constexpr bool cmp_less(LeftT lhs, RightT rhs) noexcept {
  static_assert(std::is_integral_v<LeftT> && std::is_integral_v<RightT>,
                "vlink::NameDetector::detail::cmp_less requires integral type.");

  if constexpr (std::is_signed_v<LeftT> == std::is_signed_v<RightT>) {
    return lhs < rhs;
  } else if constexpr (std::is_same_v<LeftT, bool>) {
    return static_cast<RightT>(lhs) < rhs;
  } else if constexpr (std::is_same_v<RightT, bool>) {
    return lhs < static_cast<LeftT>(rhs);
  } else if constexpr (std::is_signed_v<RightT>) {
    return rhs > 0 && lhs < static_cast<std::make_unsigned_t<RightT>>(rhs);
  } else {
    return lhs < 0 || static_cast<std::make_unsigned_t<LeftT>>(lhs) < rhs;
  }
}

template <typename EnumT, EnumT ValueV>
constexpr auto raw_enum_name() noexcept {
  static_assert(kIsEnum<EnumT>, "vlink::NameDetector::detail::raw_enum_name requires enum type.");

  if constexpr (EnumNameSupported<EnumT>::value) {
#if defined(__clang__) || defined(__GNUC__)
    constexpr auto kName = pretty_name({__PRETTY_FUNCTION__, sizeof(__PRETTY_FUNCTION__) - 2});
#elif defined(_MSC_VER)
    constexpr auto kName = pretty_name({__FUNCSIG__, sizeof(__FUNCSIG__) - 17});
#else
    constexpr auto kName = std::string_view{};
#endif
    return kName;
  } else {
    return std::string_view{};
  }
}

template <typename EnumT, EnumT ValueV>
constexpr auto enum_name() noexcept {
  constexpr auto kName = raw_enum_name<EnumT, ValueV>();
  return CString<kName.size()>{kName};
}

template <typename EnumT, EnumT ValueV>
// NOLINTNEXTLINE(google-readability-casting,modernize-avoid-c-style-cast)
inline constexpr auto kEnumName = enum_name<EnumT, ValueV>();

template <typename EnumT, auto ValueV>
constexpr bool is_valid() noexcept {
#if defined(__clang__) && __clang_major__ >= 16
  constexpr auto kV = __builtin_bit_cast(EnumT, ValueV);
#else
  constexpr auto kV = static_cast<EnumT>(ValueV);
#endif
  return raw_enum_name<EnumT, kV>().size() != 0;
}

template <typename EnumT, int OffsetT, typename UnderlyingT = std::underlying_type_t<EnumT>>
constexpr UnderlyingT ualue(size_t i) noexcept {
  if constexpr (std::is_same_v<UnderlyingT, bool>) {
    static_assert(OffsetT == 0, "vlink::NameDetector::detail::ualue requires zero offset for bool.");
    return static_cast<UnderlyingT>(i);
  } else {
    return static_cast<UnderlyingT>(static_cast<int>(i) + OffsetT);
  }
}

template <typename EnumT, int OffsetT, typename UnderlyingT = std::underlying_type_t<EnumT>>
constexpr EnumT enum_value_at(size_t i) noexcept {
  return static_cast<EnumT>(ualue<EnumT, OffsetT>(i));
}

template <typename EnumT, typename UnderlyingT = std::underlying_type_t<EnumT>>
constexpr int reflected_min() noexcept {
  constexpr auto kLhs = customize::EnumRange<EnumT>::kMin;
  // NOLINTNEXTLINE(readability-redundant-parentheses)
  constexpr auto kRhs = (std::numeric_limits<UnderlyingT>::min)();

  // NOLINTNEXTLINE(readability-suspicious-call-argument)
  if constexpr (cmp_less(kRhs, kLhs)) {
    return kLhs;
  } else {
    return kRhs;
  }
}

template <typename EnumT, typename UnderlyingT = std::underlying_type_t<EnumT>>
constexpr int reflected_max() noexcept {
  constexpr auto kLhs = customize::EnumRange<EnumT>::kMax;
  // NOLINTNEXTLINE(readability-redundant-parentheses)
  constexpr auto kRhs = (std::numeric_limits<UnderlyingT>::max)();

  if constexpr (cmp_less(kLhs, kRhs)) {
    return kLhs;
  } else {
    return kRhs;
  }
}

// clang-format off
#define VLINK_NAME_DETECTOR_FOR_EACH_256(X)                                                                            \
  X(0)                                                                                                                 \
  X(1) X(2) X(3) X(4) X(5) X(6) X(7) X(8) X(9) X(10) X(11) X(12) X(13) X(14) X(15) X(16) X(17) X(18) X(19) X(20) X(21) \
  X(22) X(23) X(24) X(25) X(26) X(27) X(28) X(29) X(30) X(31) X(32) X(33) X(34) X(35) X(36) X(37) X(38) X(39) X(40)    \
  X(41) X(42) X(43) X(44) X(45) X(46) X(47) X(48) X(49) X(50) X(51) X(52) X(53) X(54) X(55) X(56) X(57) X(58) X(59)    \
  X(60) X(61) X(62) X(63) X(64) X(65) X(66) X(67) X(68) X(69) X(70) X(71) X(72) X(73) X(74) X(75) X(76) X(77) X(78)    \
  X(79) X(80) X(81) X(82) X(83) X(84) X(85) X(86) X(87) X(88) X(89) X(90) X(91) X(92) X(93) X(94) X(95) X(96) X(97)    \
  X(98) X(99) X(100) X(101) X(102) X(103) X(104) X(105) X(106) X(107) X(108) X(109) X(110) X(111) X(112) X(113)        \
  X(114) X(115) X(116) X(117) X(118) X(119) X(120) X(121) X(122) X(123) X(124) X(125) X(126) X(127) X(128) X(129)      \
  X(130) X(131) X(132) X(133) X(134) X(135) X(136) X(137) X(138) X(139) X(140) X(141) X(142) X(143) X(144) X(145)      \
  X(146) X(147) X(148) X(149) X(150) X(151) X(152) X(153) X(154) X(155) X(156) X(157) X(158) X(159) X(160) X(161)      \
  X(162) X(163) X(164) X(165) X(166) X(167) X(168) X(169) X(170) X(171) X(172) X(173) X(174) X(175) X(176) X(177)      \
  X(178) X(179) X(180) X(181) X(182) X(183) X(184) X(185) X(186) X(187) X(188) X(189) X(190) X(191) X(192) X(193)      \
  X(194) X(195) X(196) X(197) X(198) X(199) X(200) X(201) X(202) X(203) X(204) X(205) X(206) X(207) X(208) X(209)      \
  X(210) X(211) X(212) X(213) X(214) X(215) X(216) X(217) X(218) X(219) X(220) X(221) X(222) X(223) X(224) X(225)      \
  X(226) X(227) X(228) X(229) X(230) X(231) X(232) X(233) X(234) X(235) X(236) X(237) X(238) X(239) X(240) X(241)      \
  X(242) X(243) X(244) X(245) X(246) X(247) X(248) X(249) X(250) X(251) X(252) X(253) X(254) X(255)
// clang-format on

template <typename EnumT, size_t SizeT, int MinT, size_t IndexT>
// NOLINTNEXTLINE(google-readability-function-size,readability-function-size)
constexpr void valid_count_step(bool* valid, size_t& count) noexcept {
#define VLINK_NAME_DETECTOR_ENUM_V(O)                                    \
  if constexpr ((IndexT + (O)) < SizeT) {                                \
    if constexpr (is_valid<EnumT, ualue<EnumT, MinT>(IndexT + (O))>()) { \
      valid[IndexT + (O)] = true;                                        \
      ++count;                                                           \
    }                                                                    \
  }
  VLINK_NAME_DETECTOR_FOR_EACH_256(VLINK_NAME_DETECTOR_ENUM_V)
#undef VLINK_NAME_DETECTOR_ENUM_V

  if constexpr ((IndexT + 256) < SizeT) {
    valid_count_step<EnumT, SizeT, MinT, IndexT + 256>(valid, count);
  }
}

template <size_t SizeT>
struct ValidCountResult {
  size_t count = 0;
  bool valid[SizeT] = {};
};

template <typename EnumT, size_t SizeT, int MinT>
constexpr auto valid_count() noexcept {
  ValidCountResult<SizeT> result;
  valid_count_step<EnumT, SizeT, MinT, 0>(result.valid, result.count);
  return result;
}

template <typename EnumT, size_t SizeT, int MinT>
constexpr auto values_in_range() noexcept {
  constexpr auto kVc = valid_count<EnumT, SizeT, MinT>();

  if constexpr (kVc.count > 0) {
#if defined(VLINK_NAME_DETECTOR_ARRAY_CONSTEXPR)
    // NOLINTNEXTLINE(bugprone-invalid-enum-default-initialization)
    std::array<EnumT, kVc.count> result = {};
#else
    EnumT result[kVc.count] = {};
#endif
    for (size_t i = 0, v = 0; v < kVc.count; ++i) {
      if (kVc.valid[i]) {
        result[v++] = enum_value_at<EnumT, MinT>(i);
      }
    }
#if defined(VLINK_NAME_DETECTOR_ARRAY_CONSTEXPR)
    return result;
#else
    return to_array(result, std::make_index_sequence<kVc.count>{});
#endif
  } else {
    return std::array<EnumT, 0>{};
  }
}

template <typename EnumT, typename UnderlyingT = std::underlying_type_t<EnumT>>
constexpr auto values() noexcept {
  constexpr auto kMin = reflected_min<EnumT>();
  constexpr auto kMax = reflected_max<EnumT>();
  constexpr auto kRangeSize = kMax - kMin + 1;
  static_assert(kRangeSize > 0, "vlink::NameDetector enum range requires positive size.");
  // NOLINTNEXTLINE(readability-redundant-parentheses)
  static_assert(kRangeSize < (std::numeric_limits<uint16_t>::max)(),
                "vlink::NameDetector enum range exceeds supported size.");

  return values_in_range<EnumT, kRangeSize, kMin>();
}

template <typename EnumT>
inline constexpr auto kValues = values<EnumT>();

template <typename EnumT>
inline constexpr auto kCount = kValues<EnumT>.size();

template <typename EnumT, typename UnderlyingT = std::underlying_type_t<EnumT>>
inline constexpr UnderlyingT kMinTalue =
    (kCount<EnumT> > 0) ? static_cast<UnderlyingT>(kValues<EnumT>.front()) : UnderlyingT{0};

template <typename EnumT, typename UnderlyingT = std::underlying_type_t<EnumT>>
inline constexpr UnderlyingT kMaxValue =
    (kCount<EnumT> > 0) ? static_cast<UnderlyingT>(kValues<EnumT>.back()) : UnderlyingT{0};

template <typename EnumT, size_t... IndexT>
constexpr auto names_impl(std::index_sequence<IndexT...>) noexcept {
  return std::array<std::string_view, sizeof...(IndexT)>{{kEnumName<EnumT, kValues<EnumT>[IndexT]>...}};
}

template <typename EnumT>
inline constexpr auto kNames = names_impl<EnumT>(std::make_index_sequence<kCount<EnumT>>{});

template <typename EnumT, typename UnderlyingT = std::underlying_type_t<EnumT>>
constexpr bool is_sparse() noexcept {
  if constexpr (kCount<EnumT> == 0) {
    return false;
  } else if constexpr (std::is_same_v<UnderlyingT, bool>) {
    return false;
  } else {
    constexpr auto kMaxV = kMaxValue<EnumT>;
    constexpr auto kMinT = kMinTalue<EnumT>;
    constexpr auto kRangeSize = kMaxV - kMinT + 1;
    return static_cast<size_t>(kRangeSize) != kCount<EnumT>;
  }
}

template <typename EnumT>
inline constexpr bool kIsSparse = is_sparse<EnumT>();

template <typename EnumT, typename UnderlyingT = std::underlying_type_t<EnumT>>
constexpr EnumT enum_value(size_t i) noexcept {
  if constexpr (kIsSparse<EnumT>) {
    return kValues<EnumT>[i];
  } else {
    constexpr auto kMinT = kMinTalue<EnumT>;
    return enum_value_at<EnumT, static_cast<int>(kMinT)>(i);
  }
}

template <typename TypeT>
constexpr auto raw_type_name() noexcept {
  if constexpr (TypeNameSupported<TypeT>::value) {
#if defined(__clang__)
    constexpr std::string_view kName{__PRETTY_FUNCTION__ + 59, sizeof(__PRETTY_FUNCTION__) - 61};
#elif defined(__GNUC__)
    constexpr std::string_view kName{__PRETTY_FUNCTION__ + 74, sizeof(__PRETTY_FUNCTION__) - 76};
#elif defined(_MSC_VER)
    constexpr std::string_view kName{__FUNCSIG__ + 56,
                                     sizeof(__FUNCSIG__) - 73 - (__FUNCSIG__[sizeof(__FUNCSIG__) - 18] == ' ' ? 1 : 0)};
#else
    constexpr auto kName = std::string_view{};
#endif
    return CString<static_cast<uint16_t>(kName.size())>{kName};
  } else {
    return CString<0>{};
  }
}

template <typename TypeT>
inline constexpr auto kTypeName = raw_type_name<TypeT>();

}  // namespace detail
}  // namespace NameDetector

template <typename TypeT>
inline constexpr bool NameDetector::is_support() noexcept {
  return detail::TypeNameSupported<TypeT>::value;
}

template <typename TypeT>
inline constexpr std::string_view NameDetector::get() noexcept {
  using DecayT = detail::RemoveCvrefT<TypeT>;

  static_assert(detail::TypeNameSupported<DecayT>::value, "vlink::NameDetector::get requires a supported compiler.");

#if defined(_WIN32) || defined(__CYGWIN__)
  std::string_view view = detail::kTypeName<DecayT>;

  constexpr std::string_view kStructPrefix = "struct ";

  if (view.size() >= kStructPrefix.size() && view.substr(0, kStructPrefix.size()) == kStructPrefix) {
    view.remove_prefix(kStructPrefix.size());
  }

  constexpr std::string_view kClassPrefix = "class ";

  if (view.size() >= kClassPrefix.size() && view.substr(0, kClassPrefix.size()) == kClassPrefix) {
    view.remove_prefix(kClassPrefix.size());
  }

  return view;
#else
  return detail::kTypeName<DecayT>;
#endif
}

template <typename EnumT>
inline constexpr std::string_view NameDetector::get_enum(EnumT value) noexcept {
  static_assert(std::is_enum_v<EnumT>, "vlink::NameDetector::get_enum requires enum type.");

  using DecayT = std::decay_t<EnumT>;
  using UnderlyingT = std::underlying_type_t<DecayT>;

  static_assert(detail::EnumNameSupported<DecayT>::value,
                "vlink::NameDetector::get_enum requires a supported compiler.");
  static_assert(detail::kCount<DecayT> > 0,
                "vlink::NameDetector::get_enum requires at least one enumerator within range.");

  if constexpr (detail::kIsSparse<DecayT>) {
    for (size_t i = 0; i < detail::kCount<DecayT>; ++i) {
      if (detail::enum_value<DecayT>(i) == value) {
        return detail::kNames<DecayT>[i];
      }
    }
  } else {
    const auto v = static_cast<UnderlyingT>(value);

    if (v >= detail::kMinTalue<DecayT> && v <= detail::kMaxValue<DecayT>) {
      return detail::kNames<DecayT>[static_cast<size_t>(v - detail::kMinTalue<DecayT>)];
    }
  }

  return {};
}

}  // namespace vlink

#if defined(__clang__)
#pragma clang diagnostic pop
#elif defined(__GNUC__)
#pragma GCC diagnostic pop
#elif defined(_MSC_VER)
#pragma warning(pop)
#endif
