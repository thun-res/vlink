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
 * @file quantize.h
 * @brief Header-only helpers for linear integer quantisation and dequantisation.
 *
 * @details
 * @c vlink::Quantize provides the small, deterministic numeric conversion used by compact
 * containers such as @c zerocopy::PointCloud.  It linearly maps a caller supplied real interval
 * (@c quant_min, @c quant_max) to an integral storage interval and provides the inverse mapping
 * for reads.  The helpers are templates so the public API stays allocation-free, exception-free
 * and independent of any particular point-cloud type.
 *
 * @par Mapping model
 *
 * | Step       | Behaviour                                                                 |
 * | ---------- | ------------------------------------------------------------------------- |
 * | Encode     | Map @c value from @c [quant_min,quant_max] into @c QuantT storage.         |
 * | Decode     | Map an integral storage value back with the same linear transform.         |
 * | Rounding   | Round half away from zero before casting to the storage type.              |
 * | Type math  | Use @c std::common_type_t<float,...> so all-integer inputs still divide in floating point. |
 * | Bad range  | Return zero when any input is NaN or @c quant_max <= @c quant_min.          |
 * | Saturation | Clamp encoded values to the storage limits when the scaled value overflows. |
 *
 * For signed storage types, the normal mapping interval is symmetric:
 * @c [-std::numeric_limits<T>::max(),std::numeric_limits<T>::max()].  The most negative storage
 * value, such as @c -32768 for @c int16_t, is reserved for negative saturation.  It is accepted
 * by @c decode, but because it sits one step below the normal interval it can decode slightly
 * below @c quant_min.  This keeps @c 0 centred for ranges like @c [-extent,+extent].
 *
 * @par Typical usage
 * @code
 *   constexpr int extent = 100;
 *
 *   int16_t qx = vlink::Quantize::encode<int16_t>(extent, x);  // maps [-extent,+extent]
 *   float x_out = vlink::Quantize::decode<float>(extent, qx);
 * @endcode
 *
 * @par Example
 * @code
 *   int16_t stored = vlink::Quantize::encode<int16_t>(-10, 10, 1.25f);
 *   float value = vlink::Quantize::decode<float>(-10, 10, stored);
 * @endcode
 */

#pragma once

#include <limits>
#include <type_traits>

#include "./macros.h"

namespace vlink {

/**
 * @namespace vlink::Quantize
 * @brief Stateless linear quantisation helpers.
 */
namespace Quantize {  // NOLINT(readability-identifier-naming)

/**
 * @brief Quantizes a value from a real range into an integral type.
 *
 * @param quant_min Minimum value of the real range.
 * @param quant_max Maximum value of the real range.
 * @param value Value to quantize.
 * @return Quantized value.
 */
template <typename QuantT, typename MinT, typename MaxT, typename ValueT>
[[nodiscard]] QuantT encode(MinT quant_min, MaxT quant_max, ValueT value) noexcept;

/**
 * @brief Dequantizes an integral value into a real range.
 *
 * @param quant_min Minimum value of the real range.
 * @param quant_max Maximum value of the real range.
 * @param value Quantized value.
 * @return Dequantized value.
 */
template <typename ReturnT, typename MinT, typename MaxT, typename ValueT>
[[nodiscard]] ReturnT decode(MinT quant_min, MaxT quant_max, ValueT value) noexcept;

/**
 * @brief Quantizes a value from @c [-extent,+extent] into a signed integral type.
 *
 * @details Equivalent to @c encode<QuantT>(-extent, extent, value), but avoids
 *          the generic min/max mapping setup for symmetric extent ranges.
 *
 * @param extent Positive symmetric extent.
 * @param value Value to quantize.
 * @return Quantized value, or @c 0 for invalid extent / NaN input.
 */
template <typename QuantT, typename ExtentT, typename ValueT>
[[nodiscard]] QuantT encode(ExtentT extent, ValueT value) noexcept;

/**
 * @brief Dequantizes a signed integral value from @c [-extent,+extent].
 *
 * @details Equivalent to @c decode<ReturnT>(-extent, extent, value), but avoids
 *          the generic min/max mapping setup for symmetric extent ranges.
 *
 * @param extent Positive symmetric extent.
 * @param value Quantized value.
 * @return Dequantized value, or @c 0 for invalid extent.
 */
template <typename ReturnT, typename ExtentT, typename ValueT>
[[nodiscard]] ReturnT decode(ExtentT extent, ValueT value) noexcept;

////////////////////////////////////////////////////////////////
/// Details
////////////////////////////////////////////////////////////////

template <typename QuantT, typename MinT, typename MaxT, typename ValueT>
inline QuantT encode(MinT quant_min, MaxT quant_max, ValueT value) noexcept {
  static_assert(std::is_integral_v<QuantT> && !std::is_same_v<QuantT, bool>, "QuantT must be integral.");
  static_assert(std::is_arithmetic_v<MinT>, "MinT must be arithmetic.");
  static_assert(std::is_arithmetic_v<MaxT>, "MaxT must be arithmetic.");
  static_assert(std::is_arithmetic_v<ValueT>, "ValueT must be arithmetic.");

  using CalcT = std::common_type_t<float, MinT, MaxT, ValueT>;

  auto min = static_cast<CalcT>(quant_min);
  auto max = static_cast<CalcT>(quant_max);
  auto target = static_cast<CalcT>(value);

  if VUNLIKELY (min != min || max != max || target != target || max <= min) {
    return static_cast<QuantT>(0);
  }

  auto quant_max_value = static_cast<CalcT>(std::numeric_limits<QuantT>::max());
  auto quant_min_value =
      std::is_signed_v<QuantT> ? -quant_max_value : static_cast<CalcT>(std::numeric_limits<QuantT>::lowest());

  auto quant_lowest_value = static_cast<CalcT>(std::numeric_limits<QuantT>::lowest());
  auto quant_range = quant_max_value - quant_min_value;

  auto scaled = ((target - min) * quant_range / (max - min)) + quant_min_value;
  auto rounded = scaled >= static_cast<CalcT>(0) ? scaled + static_cast<CalcT>(0.5) : scaled - static_cast<CalcT>(0.5);

  if VUNLIKELY (rounded >= quant_max_value) {
    return std::numeric_limits<QuantT>::max();
  }

  if VUNLIKELY (rounded <= quant_lowest_value) {
    return std::numeric_limits<QuantT>::lowest();
  }

  return static_cast<QuantT>(rounded);
}

template <typename QuantT, typename ExtentT, typename ValueT>
inline QuantT encode(ExtentT extent, ValueT value) noexcept {
  static_assert(std::is_integral_v<QuantT> && std::is_signed_v<QuantT> && !std::is_same_v<QuantT, bool>,
                "QuantT must be a signed integral type.");
  static_assert(std::is_arithmetic_v<ExtentT>, "ExtentT must be arithmetic.");
  static_assert(std::is_arithmetic_v<ValueT>, "ValueT must be arithmetic.");

  using CalcT = std::common_type_t<float, ExtentT, ValueT>;

  auto extent_value = static_cast<CalcT>(extent);
  auto target = static_cast<CalcT>(value);

  if VUNLIKELY (extent_value != extent_value || target != target || extent_value <= static_cast<CalcT>(0)) {
    return static_cast<QuantT>(0);
  }

  auto quant_max_value = static_cast<CalcT>(std::numeric_limits<QuantT>::max());
  auto quant_lowest_value = static_cast<CalcT>(std::numeric_limits<QuantT>::lowest());
  auto scaled = target * quant_max_value / extent_value;
  auto rounded = scaled >= static_cast<CalcT>(0) ? scaled + static_cast<CalcT>(0.5) : scaled - static_cast<CalcT>(0.5);

  if VUNLIKELY (rounded >= quant_max_value) {
    return std::numeric_limits<QuantT>::max();
  }

  if VUNLIKELY (rounded <= quant_lowest_value) {
    return std::numeric_limits<QuantT>::lowest();
  }

  return static_cast<QuantT>(rounded);
}

template <typename ReturnT, typename MinT, typename MaxT, typename ValueT>
inline ReturnT decode(MinT quant_min, MaxT quant_max, ValueT value) noexcept {
  static_assert(std::is_arithmetic_v<ReturnT>, "ReturnT must be arithmetic.");
  static_assert(std::is_arithmetic_v<MinT>, "MinT must be arithmetic.");
  static_assert(std::is_arithmetic_v<MaxT>, "MaxT must be arithmetic.");
  static_assert(std::is_integral_v<ValueT> && !std::is_same_v<ValueT, bool>, "ValueT must be integral.");

  using CalcT = std::common_type_t<float, ReturnT, MinT, MaxT>;

  auto min = static_cast<CalcT>(quant_min);
  auto max = static_cast<CalcT>(quant_max);
  auto target = static_cast<CalcT>(value);

  if VUNLIKELY (min != min || max != max || target != target || max <= min) {
    return static_cast<ReturnT>(0);
  }

  auto quant_max_value = static_cast<CalcT>(std::numeric_limits<ValueT>::max());
  auto quant_min_value =
      std::is_signed_v<ValueT> ? -quant_max_value : static_cast<CalcT>(std::numeric_limits<ValueT>::lowest());

  auto quant_lowest_value = static_cast<CalcT>(std::numeric_limits<ValueT>::lowest());
  auto quant_range = quant_max_value - quant_min_value;

  if VUNLIKELY (target >= quant_max_value) {
    target = quant_max_value;
  } else if VUNLIKELY (target <= quant_lowest_value) {
    target = quant_lowest_value;
  }

  return static_cast<ReturnT>(((target - quant_min_value) * (max - min) / quant_range) + min);
}

template <typename ReturnT, typename ExtentT, typename ValueT>
inline ReturnT decode(ExtentT extent, ValueT value) noexcept {
  static_assert(std::is_arithmetic_v<ReturnT>, "ReturnT must be arithmetic.");
  static_assert(std::is_arithmetic_v<ExtentT>, "ExtentT must be arithmetic.");
  static_assert(std::is_integral_v<ValueT> && std::is_signed_v<ValueT> && !std::is_same_v<ValueT, bool>,
                "ValueT must be a signed integral type.");

  using CalcT = std::common_type_t<float, ReturnT, ExtentT, ValueT>;

  auto extent_value = static_cast<CalcT>(extent);

  if VUNLIKELY (extent_value != extent_value || extent_value <= static_cast<CalcT>(0)) {
    return static_cast<ReturnT>(0);
  }

  auto target = static_cast<CalcT>(value);
  auto quant_max_value = static_cast<CalcT>(std::numeric_limits<ValueT>::max());

  return static_cast<ReturnT>(target * extent_value / quant_max_value);
}

}  // namespace Quantize

}  // namespace vlink
