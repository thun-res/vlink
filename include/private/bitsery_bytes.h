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
//
#include <bitsery/adapter/buffer.h>
#include <bitsery/bitsery.h>
#include <bitsery/brief_syntax.h>
#include <bitsery/brief_syntax/string.h>
#include <bitsery/brief_syntax/vector.h>
#include <bitsery/ext/growable.h>
#include <bitsery/traits/vector.h>
//
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <new>
#include <type_traits>
#include <utility>

namespace bitsery {

namespace traits {

template <>
struct ContainerTraits<vlink::Bytes> {
  using TValue = uint8_t;
  static constexpr bool isResizable = true;   // NOLINT(readability-identifier-naming)
  static constexpr bool isContiguous = true;  // NOLINT(readability-identifier-naming)

  static size_t size(const vlink::Bytes& c) noexcept { return c.size(); }
};

template <>
struct BufferAdapterTraits<vlink::Bytes> {
  using TIterator = uint8_t*;
  using TConstIterator = const uint8_t*;
  using TValue = uint8_t;

  // NOLINTNEXTLINE(readability-identifier-naming)
  static void increaseBufferSize(vlink::Bytes& c, size_t currSize, size_t minSize) {
    const size_t cap = c.capacity();
    size_t target = 0;

    if (cap == 0) {
      target = std::max<size_t>(minSize, vlink::Bytes::stack_size());
    } else {
      target = cap + (cap >> 1) + 64;
      target -= target % 64;

      if (target < minSize) {
        target = (minSize + 63) & ~size_t{63};
      }
    }

    if VUNLIKELY (!c.is_owner()) {
      auto new_buf = vlink::Bytes::create(target);

      if VUNLIKELY (!new_buf.is_owner()) {
        throw std::bad_alloc{};
      }

      if (currSize > 0 && c.data() != nullptr) {
        std::memcpy(new_buf.data(), c.data(), currSize);
      }

      c = std::move(new_buf);
    } else if VUNLIKELY (!c.resize(target)) {
      throw std::bad_alloc{};
    }
  }
};

}  // namespace traits

}  // namespace bitsery

namespace vlink {

namespace bitsery_bytes {

using OutputAdapter = bitsery::OutputBufferAdapter<Bytes>;
using InputAdapter = bitsery::InputBufferAdapter<const uint8_t*>;

template <typename SerializerT, typename T>
inline auto bounded(SerializerT& s, T& value, size_t limit) {
  if constexpr (std::is_same_v<SerializerT, bitsery::Serializer<OutputAdapter, bool>>) {
    if VUNLIKELY (value.size() > limit) {
      s.template context<bool>() = false;
      static const T kEmpty;
      return bitsery::maxSize(kEmpty, limit);
    }

    return bitsery::maxSize(std::as_const(value), limit);
  } else {
    return bitsery::maxSize(value, limit);
  }
}

}  // namespace bitsery_bytes

}  // namespace vlink
