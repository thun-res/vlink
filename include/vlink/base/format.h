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
 * @file format.h
 * @brief Minimal heap-free @c {} placeholder formatter for the logger hot path.
 *
 * @details
 * VLink targets C++17 first; @c std::format is unavailable on that baseline and far heavier on
 * older standards.  This header provides a focused subset of std::format-style formatting that
 * writes through a stack-allocated buffer or a user-supplied output iterator, never allocates,
 * and dispatches via a compile-time type tag rather than a virtual call chain.
 *
 * @par Supported argument types
 *
 * | C++ type                              | Format token | Example output     |
 * | ------------------------------------- | ------------ | ------------------ |
 * | @c signed @c char / @c short / @c int | @c {}        | @c 42              |
 * | @c unsigned @c char / ...             | @c {}        | @c 42              |
 * | @c long / @c long @c long             | @c {}        | @c 123456789       |
 * | @c unsigned @c long / @c long @c long | @c {}        | @c 123456789       |
 * | @c bool                               | @c {}        | @c true / @c false |
 * | @c char                               | @c {}        | @c A               |
 * | @c float / @c double                  | @c {}        | @c 3.14            |
 * | @c const @c char* / @c char*          | @c {}        | @c hello           |
 * | @c std::string / @c std::string_view  | @c {}        | @c hello           |
 * | @c T* (any pointer)                   | @c {}        | @c 0x7ffe1234      |
 * | @c enum                               | @c {}        | underlying integer |
 *
 * @par Placeholder syntax
 *
 * | Token                | Meaning                                     |
 * | -------------------- | ------------------------------------------- |
 * | @c {}                | Consume the next argument in order          |
 * | @c {0}, @c {1}, ...  | Explicit positional index                   |
 * | @c {{ / @c }}        | Literal opening / closing brace             |
 *
 * @par Public API
 *
 * | Function                          | Output target                  | Truncation flag |
 * | --------------------------------- | ------------------------------ | --------------- |
 * | @c format_to_n(out, n, fmt, ...)  | @c char* buffer with cap @p n  | yes             |
 * | @c format_to(out[N], fmt, ...)    | Fixed-size array               | yes             |
 * | @c format_to(it, fmt, ...)        | Output iterator                | no              |
 *
 * @par Example
 * @code
 *   char buf[128];
 *   auto result = vlink::format::format_to_n(buf, sizeof(buf) - 1, "x={} y={}", 3, 4.5);
 *   buf[result.size] = '\0';
 *   // buf == "x=3 y=4.5"
 * @endcode
 *
 * @note Floats and doubles use the default @c "%g" precision; there is no precision modifier in
 *       the placeholder syntax.  Unsupported argument types trigger a compile-time @c static_assert.
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

#include "./macros.h"

namespace vlink {

/**
 * @namespace vlink::format
 * @brief Minimal allocation-free @c {} placeholder formatter.
 */
namespace format {

namespace detail {

template <typename TypeT>
using RemoveCvref = typename std::remove_cv_t<std::remove_reference_t<TypeT>>;

template <typename TypeT, typename = void>
struct IsOutputIteratorImpl final : std::false_type {};

template <typename TypeT>
struct IsOutputIteratorImpl<TypeT, std::enable_if_t<std::is_assignable_v<decltype(*std::declval<TypeT&>()++), char>>>
    final : std::true_type {};

template <>
struct IsOutputIteratorImpl<char*> final : std::true_type {};

template <typename TypeT, size_t NumT>
struct IsOutputIteratorImpl<TypeT[NumT]> final : std::false_type {};

template <typename TypeT>
inline constexpr bool kIsOutputIterator = IsOutputIteratorImpl<TypeT>::value;

enum class Type : uint8_t {
  kNone,
  kInt,
  kUint,
  kLongLong,
  kUlongLong,
  kBool,
  kChar,
  kFloat,
  kDouble,
  kString,
  kCstring,
  kPointer
};

// NOLINTBEGIN
template <typename T>
struct TypeConstant final : std::integral_constant<Type, Type::kNone> {};

template <>
struct TypeConstant<signed char> final : std::integral_constant<Type, Type::kInt> {};

template <>
struct TypeConstant<unsigned char> final : std::integral_constant<Type, Type::kUint> {};

template <>
struct TypeConstant<short> final : std::integral_constant<Type, Type::kInt> {};

template <>
struct TypeConstant<unsigned short> final : std::integral_constant<Type, Type::kUint> {};

template <>
struct TypeConstant<int> final : std::integral_constant<Type, Type::kInt> {};

template <>
struct TypeConstant<unsigned> final : std::integral_constant<Type, Type::kUint> {};

template <>
struct TypeConstant<long> final : std::integral_constant<Type, Type::kLongLong> {};

template <>
struct TypeConstant<unsigned long> final : std::integral_constant<Type, Type::kUlongLong> {};

template <>
struct TypeConstant<long long> final : std::integral_constant<Type, Type::kLongLong> {};

template <>
struct TypeConstant<unsigned long long> final : std::integral_constant<Type, Type::kUlongLong> {};

template <>
struct TypeConstant<bool> final : std::integral_constant<Type, Type::kBool> {};

template <>
struct TypeConstant<char> final : std::integral_constant<Type, Type::kChar> {};

template <>
struct TypeConstant<float> final : std::integral_constant<Type, Type::kFloat> {};

template <>
struct TypeConstant<double> final : std::integral_constant<Type, Type::kDouble> {};

template <>
struct TypeConstant<const char*> final : std::integral_constant<Type, Type::kCstring> {};

template <>
struct TypeConstant<char*> final : std::integral_constant<Type, Type::kCstring> {};

template <>
struct TypeConstant<std::string_view> final : std::integral_constant<Type, Type::kString> {};

template <>
struct TypeConstant<std::string> final : std::integral_constant<Type, Type::kString> {};

template <size_t NumT>
struct TypeConstant<char[NumT]> final : std::integral_constant<Type, Type::kCstring> {};

template <size_t NumT>
struct TypeConstant<const char[NumT]> final : std::integral_constant<Type, Type::kCstring> {};

template <typename T>
struct TypeConstant<T*> final : std::integral_constant<Type, Type::kPointer> {};
// NOLINTEND

VLINK_EXPORT inline size_t format_uint_to(char* buf, unsigned value) noexcept;

VLINK_EXPORT inline size_t format_int_to(char* buf, int value) noexcept;

VLINK_EXPORT inline size_t format_ulong_long_to(
    char* buf, unsigned long long value) noexcept;  // NOLINT(runtime/int,google-runtime-int)

VLINK_EXPORT inline size_t format_long_long_to(char* buf,
                                               long long value) noexcept;  // NOLINT(runtime/int,google-runtime-int)

VLINK_EXPORT size_t format_pointer_to(char* buf, const void* ptr) noexcept;

VLINK_EXPORT size_t format_float_to(char* buf, size_t buflen, float value) noexcept;

VLINK_EXPORT size_t format_double_to(char* buf, size_t buflen, double value) noexcept;

class VLINK_EXPORT StringWriter {
 public:
  StringWriter(char* buf, size_t size) noexcept;

  char* out() const noexcept;

  size_t written() const noexcept;

  size_t total_size() const noexcept;

  void write(char c);

  void write(const char* s, size_t count);

  void write(std::string_view sv);

 private:
  char* begin_{nullptr};
  char* ptr_{nullptr};
  char* end_{nullptr};
  size_t total_size_{0};
};

template <typename OutputItT>
class IteratorWriter {
 public:
  inline explicit IteratorWriter(OutputItT out) : out_(out) {}

  inline OutputItT out() const noexcept { return out_; }

  inline size_t size() const noexcept { return count_; }

  inline void write(char c) {
    *out_++ = c;
    ++count_;
  }

  inline void write(const char* s, size_t count) {
    for (size_t i = 0; i < count; ++i) {
      *out_++ = s[i];
    }

    count_ += count;
  }

  inline void write(std::string_view sv) { write(sv.data(), sv.size()); }

 private:
  OutputItT out_;
  size_t count_{0};
};

template <typename CharT>
class Value {
 public:
  // NOLINTBEGIN
  union {
    int int_value;
    unsigned uint_value;
    long long long_long_value;
    unsigned long long ulong_long_value;
    bool bool_value;
    CharT char_value;
    float float_value;
    double double_value;
    const CharT* string_value;
    std::string_view string_view_value;
    const void* pointer_value;
  };

  constexpr Value() : int_value(0) {}

  constexpr explicit Value(signed char val) : int_value(static_cast<int>(val)) {}

  constexpr explicit Value(unsigned char val) : uint_value(static_cast<unsigned>(val)) {}

  constexpr explicit Value(short val) : int_value(static_cast<int>(val)) {}

  constexpr explicit Value(unsigned short val) : uint_value(static_cast<unsigned>(val)) {}

  constexpr explicit Value(int val) : int_value(val) {}

  constexpr explicit Value(unsigned val) : uint_value(val) {}

  constexpr explicit Value(long val) : long_long_value(val) {}

  constexpr explicit Value(unsigned long val) : ulong_long_value(val) {}

  constexpr explicit Value(long long val) : long_long_value(val) {}

  constexpr explicit Value(unsigned long long val) : ulong_long_value(val) {}

  constexpr explicit Value(bool val) : bool_value(val) {}

  constexpr explicit Value(CharT val) : char_value(val) {}

  constexpr explicit Value(float val) : float_value(val) {}

  constexpr explicit Value(double val) : double_value(val) {}

  constexpr explicit Value(const CharT* val) : string_value(val) {}

  constexpr explicit Value(CharT* val) : string_value(val) {}

  constexpr explicit Value(std::string_view val) : string_view_value(val) {}

  constexpr explicit Value(const std::string& val) : string_view_value(val) {}

  template <size_t NumT>
  constexpr explicit Value(const CharT (&val)[NumT]) : string_value(val) {}

  template <size_t NumT>
  constexpr explicit Value(CharT (&val)[NumT]) : string_value(val) {}

  template <typename TypeT>
  constexpr explicit Value(TypeT* val) : pointer_value(static_cast<const void*>(val)) {}
  // NOLINTEND
};

template <typename CharT>
class FormatArg {
 public:
  constexpr FormatArg() = default;  // LCOV_EXCL_LINE GCOVR_EXCL_LINE

  template <typename TypeT>
  constexpr explicit FormatArg(const TypeT& val) {
    if constexpr (std::is_enum_v<TypeT>) {
      using UnderlyingT = std::underlying_type_t<TypeT>;
      value_ = Value<CharT>(static_cast<UnderlyingT>(val));
      type_ = TypeConstant<UnderlyingT>::value;
    } else if constexpr (TypeConstant<RemoveCvref<TypeT>>::value != Type::kNone) {
      value_ = Value<CharT>(val);
      type_ = TypeConstant<RemoveCvref<TypeT>>::value;
    } else {
      static_assert(!sizeof(TypeT),
                    "[vlink::format] unsupported type for format_to/MLOG, "
                    "convert to string first");
    }
  }

  constexpr Type type() const { return type_; }
  constexpr const Value<CharT>& value() const { return value_; }

 private:
  Value<CharT> value_;
  Type type_{Type::kNone};
};

template <typename CharT, typename... ArgsT>
struct FormatArgStore final {
  static constexpr size_t kNumArgs = sizeof...(ArgsT);
  FormatArg<CharT> args[kNumArgs > 0 ? kNumArgs : 1];

  template <typename... ValuesT>
  constexpr explicit FormatArgStore(const ValuesT&... values) : args{FormatArg<CharT>(values)...} {}
};

template <typename CharT>
class BasicFormatArgs {
 public:
  constexpr BasicFormatArgs() : args_(nullptr), size_(0) {}

  template <typename... ArgsT>
  constexpr explicit BasicFormatArgs(const FormatArgStore<CharT, ArgsT...>& store)
      : args_(store.args), size_(sizeof...(ArgsT)) {}

  constexpr FormatArg<CharT> get(size_t id) const {
    return id < size_ ? args_[id] : FormatArg<CharT>();
  }  // LCOV_EXCL_LINE GCOVR_EXCL_LINE

  constexpr size_t size() const { return size_; }

 private:
  const FormatArg<CharT>* args_;
  size_t size_;
};

using FormatArgs = BasicFormatArgs<char>;

template <typename CharT, typename WriterT>
class FormatWriter {
 public:
  inline explicit FormatWriter(WriterT writer) : writer_(writer) {}

  void format(std::string_view fmt, BasicFormatArgs<CharT> args) {
    size_t arg_id = 0;
    const char* p = fmt.data();
    const char* end = p + fmt.size();

    while (p != end) {
      char c = *p++;

      if (c == '}') {
        if (p != end && *p == '}') {
          writer_.write('}');
          ++p;
        } else {
          writer_.write('}');
        }

        continue;
      }

      if VLIKELY (c != '{') {
        const char* begin = p - 1;

        while (p != end && *p != '{' && *p != '}') {
          ++p;
        }

        writer_.write(begin, static_cast<size_t>(p - begin));
        continue;
      }

      if VUNLIKELY (p == end) {
        writer_.write('{');
        break;
      }

      if (*p == '{') {
        writer_.write('{');
        ++p;
        continue;
      }

      if VLIKELY (*p == '}') {
        if VLIKELY (arg_id < args.size()) {
          write_arg(args.get(arg_id++));
        }

        ++p;

        continue;
      }

      size_t index = arg_id;
      bool has_explicit_index = false;

      if (*p >= '0' && *p <= '9') {
        index = 0;
        has_explicit_index = true;

        while (p != end && *p >= '0' && *p <= '9') {
          index = index * 10 + static_cast<size_t>(*p++ - '0');
        }
      }

      while (p != end && *p != '}') {
        ++p;
      }

      if VLIKELY (p != end) {
        if VLIKELY (index < args.size()) {
          write_arg(args.get(index));
        }

        if VLIKELY (!has_explicit_index) {
          ++arg_id;
        }

        ++p;
      }
    }
  }

  template <typename... ArgsT>
  bool try_format(std::string_view fmt, const ArgsT&... args) {
    return try_format_args(fmt, args...);
  }

  inline auto out() const { return writer_.out(); }

  template <typename WriterImplT = WriterT>
  inline auto total_size() const -> decltype(std::declval<WriterImplT>().total_size()) {
    return writer_.total_size();
  }

  inline size_t size() const { return writer_.size(); }

 private:
  // NOLINTBEGIN
  bool try_format_args(std::string_view fmt) {
    for (char c : fmt) {
      if VUNLIKELY (c == '{' || c == '}') {
        return false;
      }
    }

    writer_.write(fmt);
    return true;
  }

  template <typename ArgT, typename... ArgsT>
  bool try_format_args(std::string_view fmt, const ArgT& arg, const ArgsT&... args) {
    size_t pos = 0;

    while (pos < fmt.size() && fmt[pos] != '{' && fmt[pos] != '}') {
      ++pos;
    }

    if (pos == fmt.size()) {
      writer_.write(fmt);
      return true;
    }

    if VUNLIKELY (fmt[pos] != '{' || pos + 1 >= fmt.size() || fmt[pos + 1] != '}') {
      return false;
    }

    writer_.write(fmt.data(), pos);
    write_value(arg);

    return try_format_args(fmt.substr(pos + 2), args...);
  }

  template <typename TypeT>
  void write_value(const TypeT& value) {
    using ValueT = RemoveCvref<TypeT>;

    if constexpr (std::is_enum_v<ValueT>) {
      write_value(static_cast<std::underlying_type_t<ValueT>>(value));
    } else if constexpr (TypeConstant<ValueT>::value == Type::kInt) {
      write_int(static_cast<int>(value));
    } else if constexpr (TypeConstant<ValueT>::value == Type::kUint) {
      write_uint(static_cast<unsigned>(value));
    } else if constexpr (TypeConstant<ValueT>::value == Type::kLongLong) {
      write_long_long(static_cast<long long>(value));
    } else if constexpr (TypeConstant<ValueT>::value == Type::kUlongLong) {
      write_ulong_long(static_cast<unsigned long long>(value));
    } else if constexpr (TypeConstant<ValueT>::value == Type::kBool) {
      write_bool(value);
    } else if constexpr (TypeConstant<ValueT>::value == Type::kChar) {
      write_char(value);
    } else if constexpr (TypeConstant<ValueT>::value == Type::kFloat) {
      write_float(value);
    } else if constexpr (TypeConstant<ValueT>::value == Type::kDouble) {
      write_double(value);
    } else if constexpr (TypeConstant<ValueT>::value == Type::kCstring) {
      write_string(value);
    } else if constexpr (TypeConstant<ValueT>::value == Type::kString) {
      write_string_view(value);
    } else if constexpr (TypeConstant<ValueT>::value == Type::kPointer) {
      write_pointer(static_cast<const void*>(value));
    } else {
      static_assert(!sizeof(TypeT),
                    "[vlink::format] unsupported type for format_to/MLOG, "
                    "convert to string first");
    }
  }

  void write_int(int value) {
    char buf[11];
    size_t n = format_int_to(buf, value);
    writer_.write(buf, n);
  }

  void write_uint(unsigned value) {
    char buf[10];
    size_t n = format_uint_to(buf, value);
    writer_.write(buf, n);
  }

  void write_long_long(long long value) {
    char buf[20];
    size_t n = format_long_long_to(buf, value);
    writer_.write(buf, n);
  }

  void write_ulong_long(unsigned long long value) {
    char buf[20];
    size_t n = format_ulong_long_to(buf, value);
    writer_.write(buf, n);
  }

  void write_bool(bool value) {
    if (value) {
      writer_.write("true", 4);
    } else {
      writer_.write("false", 5);
    }
  }

  void write_char(char value) { writer_.write(value); }

  void write_string(const char* str) {
    if VLIKELY (str) {
      writer_.write(str, std::strlen(str));
    } else {
      writer_.write("(null)", 6);
    }
  }

  void write_string_view(std::string_view sv) { writer_.write(sv); }

  void write_pointer(const void* ptr) {
    char buf[18];
    size_t n = format_pointer_to(buf, ptr);
    writer_.write(buf, n);
  }

  void write_float(float value) {
    char buf[32];
    size_t n = format_float_to(buf, sizeof(buf), value);
    writer_.write(buf, n);
  }

  void write_double(double value) {
    char buf[32];
    size_t n = format_double_to(buf, sizeof(buf), value);
    writer_.write(buf, n);
  }

  void write_arg(const FormatArg<CharT>& arg) {
    switch (arg.type()) {
      case Type::kInt:
        write_int(arg.value().int_value);
        break;
      case Type::kUint:
        write_uint(arg.value().uint_value);
        break;
      case Type::kLongLong:
        write_long_long(arg.value().long_long_value);
        break;
      case Type::kUlongLong:
        write_ulong_long(arg.value().ulong_long_value);
        break;
      case Type::kBool:
        write_bool(arg.value().bool_value);
        break;
      case Type::kChar:
        write_char(arg.value().char_value);
        break;
      case Type::kFloat:
        write_float(arg.value().float_value);
        break;
      case Type::kDouble:
        write_double(arg.value().double_value);
        break;
      case Type::kCstring:
        write_string(arg.value().string_value);
        break;
      case Type::kString:
        write_string_view(arg.value().string_view_value);
        break;
      case Type::kPointer:
        write_pointer(arg.value().pointer_value);
        break;
      default:  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
        break;  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
    }
  }
  // NOLINTEND

  WriterT writer_;
};

}  // namespace detail

template <typename... ArgsT>
struct FString final {
  std::string_view str;
  using t = FString;

  template <size_t NumT>
  // NOLINTNEXTLINE(runtime/explicit,google-explicit-constructor,hicpp-explicit-conversions)
  constexpr FString(const char (&s)[NumT]) : str(s, NumT - 1) {}

  // NOLINTNEXTLINE(modernize-use-constraints)
  template <typename StrT, std::enable_if_t<std::is_convertible_v<const StrT&, std::string_view>, int> = 0>
  // NOLINTNEXTLINE(runtime/explicit,google-explicit-constructor,hicpp-explicit-conversions)
  constexpr FString(const StrT& s) : str(s) {}

  // NOLINTNEXTLINE(google-explicit-constructor,hicpp-explicit-conversions)
  inline operator std::string_view() const { return str; }

  std::string_view get() const { return str; }
};

template <typename... ArgsT>
using format_string = typename FString<ArgsT...>::t;

template <typename OutputItT>
struct FormatToNResult final {
  OutputItT out;
  size_t size{0};
  bool truncated{false};
};

struct FormatToResult final {
  char* out{nullptr};
  size_t size{0};
  bool truncated{false};
};

////////////////////////////////////////////////////////////////
/// Public API
////////////////////////////////////////////////////////////////

/**
 * @brief Builds a type-erased argument store from a parameter pack.
 *
 * @tparam ArgsT  Argument types.
 * @param args    Argument values.
 * @return Argument store usable by the format engine.
 */
template <typename... ArgsT>
inline detail::FormatArgStore<char, detail::RemoveCvref<ArgsT>...> make_format_args(const ArgsT&... args);

/**
 * @brief Formats @p args into @p out, writing at most @p n characters.
 *
 * @details
 * Placeholders in @p fmt are substituted with @p args in order.  When the produced text would
 * exceed @p n the output is truncated and @c truncated is set to @c true.  The caller is
 * responsible for null termination if needed.
 *
 * @tparam ArgsT  Argument types (deduced).
 * @param out   Destination buffer with capacity of at least @p n bytes.
 * @param n     Maximum characters to write, not counting a null terminator.
 * @param fmt   Format string with @c {} placeholders.
 * @param args  Arguments to substitute.
 * @return @c FormatToNResult describing the end iterator, total size and truncation flag.
 */
template <typename... ArgsT>
inline FormatToNResult<char*> format_to_n(char* out, size_t n, format_string<ArgsT...> fmt, const ArgsT&... args);

/**
 * @brief Formats @p args into a fixed-size character array; equivalent to @c format_to_n.
 *
 * @tparam NumT   Array size (deduced).
 * @tparam ArgsT  Argument types.
 * @param out   Destination array.
 * @param fmt   Format string.
 * @param args  Arguments to substitute.
 * @return @c FormatToResult describing the end pointer, total size and truncation flag.
 */
template <size_t NumT, typename... ArgsT>
inline FormatToResult format_to(char (&out)[NumT], format_string<ArgsT...> fmt, const ArgsT&... args);

/**
 * @brief Formats @p args through an output iterator.
 *
 * @details
 * Writes each character via @c *out++ = @c c.  The iterator must satisfy the OutputIterator
 * concept; arrays are explicitly excluded so the fixed-array overload wins overload resolution.
 *
 * @tparam OutputItT  Output iterator type.
 * @tparam ArgsT      Argument types.
 * @param out   Destination iterator.
 * @param fmt   Format string.
 * @param args  Arguments to substitute.
 * @return Iterator one past the last written character.
 */
template <typename OutputItT, typename... ArgsT,
          // NOLINTNEXTLINE(modernize-use-constraints)
          std::enable_if_t<detail::kIsOutputIterator<detail::RemoveCvref<OutputItT>> &&
                               !std::is_array_v<std::remove_reference_t<OutputItT>>,
                           int> = 0>
inline detail::RemoveCvref<OutputItT> format_to(OutputItT&& out, format_string<ArgsT...> fmt, const ArgsT&... args);

}  // namespace format

////////////////////////////////////////////////////////////////
/// Details
////////////////////////////////////////////////////////////////

namespace format {
namespace detail {

// NOLINTBEGIN
template <typename UIntT>
inline int count_digits(UIntT n) noexcept {
  int count = 1;

  while (n >= 10) {
    n /= 10;
    ++count;
  }

  return count;
}

template <typename UIntT>
inline void write_int_digits(char* buf, UIntT value, int num_digits) noexcept {
  char* end = buf + num_digits;

  while (value >= 10) {
    auto digit = static_cast<unsigned>(value % 10);
    *--end = static_cast<char>('0' + digit);
    value /= 10;
  }

  *--end = static_cast<char>('0' + value);
}

inline size_t format_uint_to(char* buf, unsigned value) noexcept {
  int num_digits = count_digits(value);
  write_int_digits(buf, value, num_digits);

  return static_cast<size_t>(num_digits);
}

inline size_t format_int_to(char* buf, int value) noexcept {
  if (value < 0) {
    buf[0] = '-';
    unsigned u = static_cast<unsigned>(-(value + 1)) + 1;

    return 1 + format_uint_to(buf + 1, u);
  }

  return format_uint_to(buf, static_cast<unsigned>(value));
}

inline size_t format_ulong_long_to(char* buf, unsigned long long value) noexcept {
  int num_digits = count_digits(value);
  write_int_digits(buf, value, num_digits);

  return static_cast<size_t>(num_digits);
}

inline size_t format_long_long_to(char* buf, long long value) noexcept {
  if (value < 0) {
    buf[0] = '-';
    unsigned long long u = static_cast<unsigned long long>(-(value + 1)) + 1;

    return 1 + format_ulong_long_to(buf + 1, u);
  }

  return format_ulong_long_to(buf, static_cast<unsigned long long>(value));
}
// NOLINTEND

}  // namespace detail
}  // namespace format

inline format::detail::StringWriter::StringWriter(char* buf, size_t size) noexcept
    : begin_(buf), ptr_(buf), end_(buf + size) {}

inline char* format::detail::StringWriter::out() const noexcept { return ptr_; }

inline size_t format::detail::StringWriter::written() const noexcept { return static_cast<size_t>(ptr_ - begin_); }

inline size_t format::detail::StringWriter::total_size() const noexcept { return total_size_; }

inline void format::detail::StringWriter::write(char c) {
  ++total_size_;

  if VLIKELY (ptr_ < end_) {
    *ptr_++ = c;
  }
}

inline void format::detail::StringWriter::write(const char* s, size_t count) {
  total_size_ += count;

  auto avail = static_cast<size_t>(end_ - ptr_);
  size_t n = (count <= avail) ? count : avail;

  if VLIKELY (n > 0) {
    std::memcpy(ptr_, s, n);
    ptr_ += n;
  }
}

inline void format::detail::StringWriter::write(std::string_view sv) { write(sv.data(), sv.size()); }

template <typename... ArgsT>
inline format::detail::FormatArgStore<char, format::detail::RemoveCvref<ArgsT>...> format::make_format_args(
    const ArgsT&... args) {
  return format::detail::FormatArgStore<char, format::detail::RemoveCvref<ArgsT>...>{args...};
}

template <typename... ArgsT>
inline format::FormatToNResult<char*> format::format_to_n(char* out, size_t n, format_string<ArgsT...> fmt,
                                                          const ArgsT&... args) {
  format::detail::StringWriter sw(out, n);
  format::detail::FormatWriter<char, format::detail::StringWriter> writer(sw);

  if VLIKELY (writer.try_format(fmt.get(), args...)) {
    size_t total = writer.total_size();

    return {writer.out(), total, total > n};
  }

  format::detail::FormatArgStore<char, format::detail::RemoveCvref<ArgsT>...> arg_store{args...};
  format::detail::FormatArgs fargs(arg_store);
  format::detail::StringWriter fallback_sw(out, n);
  format::detail::FormatWriter<char, format::detail::StringWriter> fallback_writer(fallback_sw);

  fallback_writer.format(fmt.get(), fargs);

  size_t total = fallback_writer.total_size();

  return {fallback_writer.out(), total, total > n};
}

template <size_t NumT, typename... ArgsT>
inline format::FormatToResult format::format_to(char (&out)[NumT], format_string<ArgsT...> fmt, const ArgsT&... args) {
  auto result = ::vlink::format::format_to_n(out, NumT, fmt, args...);

  return {result.out, result.size, result.truncated};
}

template <typename OutputItT, typename... ArgsT,
          std::enable_if_t<format::detail::kIsOutputIterator<format::detail::RemoveCvref<OutputItT>> &&
                               !std::is_array_v<std::remove_reference_t<OutputItT>>,
                           int>>
inline format::detail::RemoveCvref<OutputItT> format::format_to(OutputItT&& out, format_string<ArgsT...> fmt,
                                                                const ArgsT&... args) {
  using ItT = format::detail::RemoveCvref<OutputItT>;

  auto arg_store = ::vlink::format::make_format_args(args...);

  format::detail::FormatArgs fargs(arg_store);
  format::detail::IteratorWriter<ItT> iter_writer(out);
  format::detail::FormatWriter<char, format::detail::IteratorWriter<ItT>> writer(iter_writer);

  writer.format(fmt.get(), fargs);

  return writer.out();
}

}  // namespace vlink
