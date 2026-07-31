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
 * | C++ type                                                  | Example output         |
 * | --------------------------------------------------------- | ---------------------- |
 * | all integral types incl. @c char and @c bool              | @c 42 / @c A / @c true |
 * | @c float / @c double / @c long @c double                  | @c 3.14                |
 * | @c const @c char* / @c std::string / @c std::string_view  | @c hello               |
 * | @c T* (any pointer) and @c nullptr                        | @c 0x7ffe1234 / @c 0x0 |
 * | @c enum                                                   | underlying integer     |
 * | any type with an ADL @c format_as(T)                      | mapped result          |
 *
 * @par Placeholder syntax
 *
 * | Token                   | Meaning                                     |
 * | ----------------------- | ------------------------------------------- |
 * | @c {}                   | Consume the next argument in order          |
 * | @c {0}, @c {1}, ...     | Explicit positional index                   |
 * | @c {:spec}, @c {0:spec} | Apply a std::format-style format spec       |
 * | @c {{ / @c }}           | Literal opening / closing brace             |
 *
 * @par Format specification
 *
 * The std::format grammar: @c [[fill]align][sign][#][0][width][.precision][type]
 *
 * - fill + align: any byte except braces before @c < (left), @c > (right), @c ^ (center).
 * - sign: @c + / @c - / space for arithmetic presentations.
 * - @c #: @c 0b / @c 0B / @c 0 / @c 0x / @c 0X prefix; floats keep the decimal point and
 *   @c g / @c G keep trailing zeros.
 * - @c 0: zero padding after sign/prefix; ignored with an explicit align.
 * - width / precision: literal digits, or dynamic @c {} / @c {n} consuming an integral
 *   argument; precision also truncates strings.
 * - type: @c b @c B @c c @c d @c o @c x @c X integral; @c a @c A @c e @c E @c f @c F
 *   @c g @c G floating; @c s string/bool; @c ? escaped debug string/char; @c p pointer.
 *
 * @par Public API
 *
 * | Function                          | Output target                  | Allocates |
 * | --------------------------------- | ------------------------------ | --------- |
 * | @c format_to_n(out, n, fmt, ...)  | @c char* buffer with cap @p n  | no        |
 * | @c format_to(out[N], fmt, ...)    | Fixed-size array               | no        |
 * | @c format_to(it, fmt, ...)        | Output iterator                | no        |
 * | @c format(fmt, ...)               | Returned @c std::string        | yes       |
 *
 * @par Example
 * @code
 *   char buf[128];
 *   auto result = vlink::format::format_to_n(buf, sizeof(buf) - 1, "x={} y={:.1f}", 3, 4.5);
 *   buf[result.size] = '\0';
 *   // buf == "x=3 y=4.5"
 * @endcode
 *
 * @note Deviations from std::format: locale (@c L) is not supported, the fill byte is
 *       single-byte, and invalid or inapplicable spec fields are ignored instead of throwing,
 *       so formatting never fails at runtime.  A dynamic width/precision referencing a missing,
 *       non-integral or negative argument degrades to "not specified".  Width and precision
 *       count bytes, so multi-byte UTF-8 sequences may be split by a string precision.
 *       Floating precision is clamped to 340 digits.  A @c c presentation outside the @c char
 *       range and a @c c presentation of @c bool fall back to the decimal form.  Without a
 *       spec (and for @c {:#} without type and precision), floating values use the @c "%g"
 *       6 significant digit representation.  Platforms without floating-point @c std::to_chars
 *       format floats through @c snprintf with printf semantics, including the @c %a
 *       representation.  Nested width/precision references that cannot be parsed are treated as
 *       literal text.  The @c ? presentation escapes ASCII control bytes, quotes and backslashes;
 *       string bytes at or above @c 0x80 pass through unescaped while a lone @c char code unit
 *       above @c 0x7F is escaped as @c \\x{..}.  Unsupported argument types trigger a compile-time @c
 * static_assert.
 *
 * @note The allocating entry point shares its name with this namespace; call it fully qualified
 *       as @c vlink::format::format(...).  @c vlink::format(...) resolves to the namespace and
 *       does not compile.
 *
 * @note Custom types opt in by defining @c format_as(const T&) in the namespace of @c T,
 *       returning a directly formattable value; specs then apply to the mapped result.
 *       Class-type results must be returned by reference or as @c std::string_view.
 */

#pragma once

#include <climits>
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
  kLongDouble,
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
struct TypeConstant<long double> final : std::integral_constant<Type, Type::kLongDouble> {};

template <>
struct TypeConstant<std::nullptr_t> final : std::integral_constant<Type, Type::kPointer> {};

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

template <typename TypeT, typename = void>
struct HasFormatAs final : std::false_type {};

template <typename TypeT>
struct HasFormatAs<TypeT, std::void_t<decltype(format_as(std::declval<const TypeT&>()))>> final : std::true_type {};

VLINK_EXPORT inline size_t format_uint_to(char* buf, unsigned value) noexcept;

VLINK_EXPORT inline size_t format_int_to(char* buf, int value) noexcept;

VLINK_EXPORT inline size_t format_ulong_long_to(
    char* buf, unsigned long long value) noexcept;  // NOLINT(runtime/int,google-runtime-int)

VLINK_EXPORT inline size_t format_long_long_to(char* buf,
                                               long long value) noexcept;  // NOLINT(runtime/int,google-runtime-int)

VLINK_EXPORT size_t format_pointer_to(char* buf, const void* ptr) noexcept;

VLINK_EXPORT size_t format_float_to(char* buf, size_t buflen, float value) noexcept;

VLINK_EXPORT size_t format_double_to(char* buf, size_t buflen, double value) noexcept;

VLINK_EXPORT size_t format_long_double_to(char* buf, size_t buflen,
                                          long double value) noexcept;  // NOLINT(google-runtime-float)

VLINK_EXPORT size_t format_double_spec_to(char* buf, size_t buflen, double value, char type, int precision,
                                          bool alt) noexcept;

// NOLINTNEXTLINE(google-runtime-float)
VLINK_EXPORT size_t format_long_double_spec_to(char* buf, size_t buflen, long double value, char type, int precision,
                                               bool alt) noexcept;

inline constexpr int kSpecRefNone = -1;
inline constexpr int kSpecRefAuto = -2;

enum class Align : uint8_t { kNone, kLeft, kRight, kCenter };

enum class Sign : uint8_t { kMinus, kPlus, kSpace };

struct FormatSpec final {
  char fill{' '};
  Align align{Align::kNone};
  Sign sign{Sign::kMinus};
  bool alt{false};
  bool zero{false};
  int width{0};
  int precision{-1};
  int width_ref{kSpecRefNone};
  int precision_ref{kSpecRefNone};
  char type{'\0'};
};

VLINK_EXPORT const char* parse_spec_ref(const char* p, const char* end, int& ref) noexcept;

VLINK_EXPORT const char* parse_format_spec(const char* p, const char* end, FormatSpec& spec) noexcept;

// NOLINTNEXTLINE(runtime/int,google-runtime-int)
VLINK_EXPORT inline size_t format_base_digits_to(char (&buf)[64], unsigned long long value, unsigned base_bits,
                                                 bool upper) noexcept;

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
    long double long_double_value;
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

  constexpr explicit Value(long double val) : long_double_value(val) {}

  constexpr explicit Value(std::nullptr_t) : pointer_value(nullptr) {}

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
    if constexpr (HasFormatAs<TypeT>::value) {
      using ResultT = decltype(format_as(val));
      using MappedT = RemoveCvref<ResultT>;
      static_assert(std::is_enum_v<MappedT> || TypeConstant<MappedT>::value != Type::kNone,
                    "[vlink::format] format_as must return a directly formattable type");
      static_assert(std::is_reference_v<ResultT> || !std::is_same_v<MappedT, std::string>,
                    "[vlink::format] format_as returning std::string by value would dangle, "
                    "return a reference or std::string_view");

      if constexpr (std::is_enum_v<MappedT>) {
        using UnderlyingT = std::underlying_type_t<MappedT>;
        value_ = Value<CharT>(static_cast<UnderlyingT>(format_as(val)));
        type_ = TypeConstant<UnderlyingT>::value;
      } else {
        value_ = Value<CharT>(format_as(val));
        type_ = TypeConstant<MappedT>::value;
      }
    } else if constexpr (std::is_enum_v<TypeT>) {
      using UnderlyingT = std::underlying_type_t<TypeT>;
      value_ = Value<CharT>(static_cast<UnderlyingT>(val));
      type_ = TypeConstant<UnderlyingT>::value;
    } else if constexpr (TypeConstant<RemoveCvref<TypeT>>::value != Type::kNone) {
      value_ = Value<CharT>(val);
      type_ = TypeConstant<RemoveCvref<TypeT>>::value;
    } else {
      static_assert(!sizeof(TypeT),
                    "[vlink::format] unsupported type for format_to/MLOG, "
                    "convert to string first or provide format_as");
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

      if (!has_explicit_index) {
        ++arg_id;
      }

      FormatSpec spec;
      bool has_spec = false;

      if (p != end && *p == ':') {
        ++p;
        p = parse_format_spec(p, end, spec);
        resolve_spec_refs(spec, args, arg_id);
        has_spec = true;
      }

      while (p != end && *p != '}') {
        ++p;
      }

      if VLIKELY (p != end) {
        if VLIKELY (index < args.size()) {
          if (has_spec) {
            write_arg_spec(args.get(index), spec);
          } else {
            write_arg(args.get(index));
          }
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

    if constexpr (HasFormatAs<ValueT>::value) {
      write_value(format_as(value));
    } else if constexpr (std::is_enum_v<ValueT>) {
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
    } else if constexpr (TypeConstant<ValueT>::value == Type::kLongDouble) {
      write_long_double(value);
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

  void write_long_double(long double value) {
    char buf[64];
    size_t n = format_long_double_to(buf, sizeof(buf), value);
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
      case Type::kLongDouble:
        write_long_double(arg.value().long_double_value);
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

  static long long spec_ref_value(const FormatArg<CharT>& arg) {
    switch (arg.type()) {
      case Type::kInt:
        return arg.value().int_value;
      case Type::kUint:
        return arg.value().uint_value;
      case Type::kLongLong:
        return arg.value().long_long_value;
      case Type::kUlongLong:
        return arg.value().ulong_long_value > static_cast<unsigned long long>(LLONG_MAX)
                   ? LLONG_MAX
                   : static_cast<long long>(arg.value().ulong_long_value);
      default:
        return -1;
    }
  }

  void resolve_spec_refs(FormatSpec& spec, const BasicFormatArgs<CharT>& args, size_t& arg_id) {
    if (spec.width_ref != kSpecRefNone) {
      const size_t ref = spec.width_ref == kSpecRefAuto ? arg_id++ : static_cast<size_t>(spec.width_ref);
      const long long value = spec_ref_value(args.get(ref));
      spec.width = value > 0 ? (value > 999999999 ? 999999999 : static_cast<int>(value)) : 0;
    }

    if (spec.precision_ref != kSpecRefNone) {
      const size_t ref = spec.precision_ref == kSpecRefAuto ? arg_id++ : static_cast<size_t>(spec.precision_ref);
      const long long value = spec_ref_value(args.get(ref));
      spec.precision = value >= 0 ? (value > 999999999 ? 999999999 : static_cast<int>(value)) : -1;
    }
  }

  void write_fill(char fill, size_t count) {
    char chunk[64];

    if (count == 0U) {
      return;
    }

    std::memset(chunk, fill, count < sizeof(chunk) ? count : sizeof(chunk));

    while (count > 0U) {
      const size_t n = count < sizeof(chunk) ? count : sizeof(chunk);
      writer_.write(chunk, n);
      count -= n;
    }
  }

  void write_padded(const FormatSpec& spec, std::string_view prefix, std::string_view body, Align default_align,
                    bool allow_zero) {
    const size_t content = prefix.size() + body.size();
    const size_t width = static_cast<size_t>(spec.width);
    const size_t pad = width > content ? width - content : 0U;

    if VLIKELY (pad == 0U) {
      if (!prefix.empty()) {
        writer_.write(prefix);
      }

      writer_.write(body);

      return;
    }

    if (spec.zero && spec.align == Align::kNone && allow_zero) {
      if (!prefix.empty()) {
        writer_.write(prefix);
      }

      write_fill('0', pad);
      writer_.write(body);

      return;
    }

    const Align align = spec.align == Align::kNone ? default_align : spec.align;
    size_t left = 0U;

    if (align == Align::kRight) {
      left = pad;
    } else if (align == Align::kCenter) {
      left = pad / 2U;
    }

    write_fill(spec.fill, left);

    if (!prefix.empty()) {
      writer_.write(prefix);
    }

    writer_.write(body);
    write_fill(spec.fill, pad - left);
  }

  void write_int_spec(long long value, const FormatSpec& spec) {
    if VUNLIKELY (spec.type == 'c' && value >= CHAR_MIN && value <= CHAR_MAX) {
      write_char_spec(static_cast<char>(value), spec);

      return;
    }

    if (value < 0) {
      write_uint_spec(static_cast<unsigned long long>(-(value + 1)) + 1U, true, spec);
    } else {
      write_uint_spec(static_cast<unsigned long long>(value), false, spec);
    }
  }

  void write_uint_spec(unsigned long long magnitude, bool negative, const FormatSpec& spec) {
    if VUNLIKELY (spec.type == 'c' && !negative && magnitude <= static_cast<unsigned long long>(CHAR_MAX)) {
      write_char_spec(static_cast<char>(magnitude), spec);

      return;
    }

    char prefix[3];
    size_t prefix_size = 0;

    if (negative) {
      prefix[prefix_size++] = '-';
    } else if (spec.sign == Sign::kPlus) {
      prefix[prefix_size++] = '+';
    } else if (spec.sign == Sign::kSpace) {
      prefix[prefix_size++] = ' ';
    }

    char digits[64];
    size_t digit_count = 0;
    const char* digit_begin = digits;

    switch (spec.type) {
      case 'b':
      case 'B':
        digit_count = format_base_digits_to(digits, magnitude, 1U, false);
        digit_begin = digits + sizeof(digits) - digit_count;

        if (spec.alt) {
          prefix[prefix_size++] = '0';
          prefix[prefix_size++] = spec.type;
        }

        break;
      case 'o':
        digit_count = format_base_digits_to(digits, magnitude, 3U, false);
        digit_begin = digits + sizeof(digits) - digit_count;

        if (spec.alt && magnitude != 0U) {
          prefix[prefix_size++] = '0';
        }

        break;
      case 'x':
      case 'X':
        digit_count = format_base_digits_to(digits, magnitude, 4U, spec.type == 'X');
        digit_begin = digits + sizeof(digits) - digit_count;

        if (spec.alt) {
          prefix[prefix_size++] = '0';
          prefix[prefix_size++] = spec.type;
        }

        break;
      default:
        digit_count = format_ulong_long_to(digits, magnitude);
        break;
    }

    write_padded(spec, std::string_view(prefix, prefix_size), std::string_view(digit_begin, digit_count), Align::kRight,
                 true);
  }

  void write_bool_spec(bool value, const FormatSpec& spec) {
    if (spec.type != '\0' && spec.type != 's') {
      FormatSpec decimal_spec = spec;
      decimal_spec.type = spec.type == 'c' ? 'd' : spec.type;
      write_uint_spec(value ? 1U : 0U, false, decimal_spec);

      return;
    }

    write_padded(spec, std::string_view(), value ? std::string_view("true") : std::string_view("false"), Align::kLeft,
                 false);
  }

  void write_char_spec(char value, const FormatSpec& spec) {
    if VUNLIKELY (spec.type == '?') {
      write_debug_spec(std::string_view(&value, 1), '\'', spec);

      return;
    }

    if (spec.type != '\0' && spec.type != 'c' && spec.type != 's') {
      write_int_spec(static_cast<long long>(static_cast<unsigned char>(value)), spec);

      return;
    }

    write_padded(spec, std::string_view(), std::string_view(&value, 1), Align::kLeft, false);
  }

  template <typename FloatT>
  void write_float_spec(FloatT value, const FormatSpec& spec) {
    constexpr size_t kMaxIntegerDigits = std::is_same_v<FloatT, long double> ? 4933U : 309U;
    constexpr int kMaxPrecision = 340;
    char buf[kMaxIntegerDigits + 2U + kMaxPrecision];
    size_t size = 0;
    const int precision = spec.precision > kMaxPrecision ? kMaxPrecision : spec.precision;

    if (spec.type == '\0' && precision < 0 && !spec.alt) {
      if constexpr (std::is_same_v<FloatT, long double>) {
        size = format_long_double_to(buf, sizeof(buf), value);
      } else if constexpr (std::is_same_v<FloatT, float>) {
        size = format_float_to(buf, sizeof(buf), value);
      } else {
        size = format_double_to(buf, sizeof(buf), value);
      }
    } else {
      if constexpr (std::is_same_v<FloatT, long double>) {
        size = format_long_double_spec_to(buf, sizeof(buf), value, spec.type, precision, spec.alt);
      } else {
        size = format_double_spec_to(buf, sizeof(buf), static_cast<double>(value), spec.type, precision, spec.alt);
      }
    }

    std::string_view text(buf, size);
    char sign = '\0';

    if (!text.empty() && text.front() == '-') {
      sign = '-';
      text.remove_prefix(1);
    } else if (spec.sign == Sign::kPlus) {
      sign = '+';
    } else if (spec.sign == Sign::kSpace) {
      sign = ' ';
    }

    const bool numeric = !text.empty() && text.front() >= '0' && text.front() <= '9';

    write_padded(spec, sign != '\0' ? std::string_view(&sign, 1) : std::string_view(), text, Align::kRight, numeric);
  }

  static size_t escape_debug_char(char c, char quote, bool escape_high, char (&out)[8]) {
    switch (c) {
      case '\t':
        out[0] = '\\';
        out[1] = 't';
        return 2U;
      case '\n':
        out[0] = '\\';
        out[1] = 'n';
        return 2U;
      case '\r':
        out[0] = '\\';
        out[1] = 'r';
        return 2U;
      case '\\':
        out[0] = '\\';
        out[1] = '\\';
        return 2U;
      default:
        break;
    }

    if (c == quote) {
      out[0] = '\\';
      out[1] = quote;
      return 2U;
    }

    const auto code = static_cast<unsigned char>(c);

    if VUNLIKELY (code < 0x20U || code == 0x7FU || (escape_high && code > 0x7FU)) {
      static constexpr const char kHexDigits[] = "0123456789abcdef";
      size_t n = 0;
      out[n++] = '\\';
      out[n++] = code > 0x7FU ? 'x' : 'u';
      out[n++] = '{';

      if (code >= 0x10U) {
        out[n++] = kHexDigits[code >> 4U];
      }

      out[n++] = kHexDigits[code & 0xFU];
      out[n++] = '}';

      return n;
    }

    out[0] = c;

    return 1U;
  }

  void write_debug_spec(std::string_view sv, char quote, const FormatSpec& spec) {
    const bool escape_high = quote == '\'';
    char piece[8];
    size_t body = 2U;

    for (const char c : sv) {
      body += escape_debug_char(c, quote, escape_high, piece);
    }

    const size_t shown =
        spec.precision >= 0 && static_cast<size_t>(spec.precision) < body ? static_cast<size_t>(spec.precision) : body;
    const size_t width = static_cast<size_t>(spec.width);
    const size_t pad = width > shown ? width - shown : 0U;
    const Align align = spec.align == Align::kNone ? Align::kLeft : spec.align;
    size_t left = 0U;

    if (align == Align::kRight) {
      left = pad;
    } else if (align == Align::kCenter) {
      left = pad / 2U;
    }

    write_fill(spec.fill, left);

    size_t budget = shown;
    const auto put = [this, &budget](const char* data, size_t count) {
      const size_t n = count < budget ? count : budget;

      if (n > 0U) {
        writer_.write(data, n);
        budget -= n;
      }
    };

    put(&quote, 1U);

    for (const char c : sv) {
      if (budget == 0U) {
        break;
      }

      put(piece, escape_debug_char(c, quote, escape_high, piece));
    }

    put(&quote, 1U);
    write_fill(spec.fill, pad - left);
  }

  void write_cstring_spec(const char* str, const FormatSpec& spec) {
    if VLIKELY (str) {
      write_string_spec(std::string_view(str, std::strlen(str)), spec);
    } else {
      write_string_spec(std::string_view("(null)"), spec);
    }
  }

  void write_string_spec(std::string_view sv, const FormatSpec& spec) {
    if VUNLIKELY (spec.type == '?') {
      write_debug_spec(sv, '"', spec);

      return;
    }

    if (spec.precision >= 0 && static_cast<size_t>(spec.precision) < sv.size()) {
      sv = sv.substr(0, static_cast<size_t>(spec.precision));
    }

    write_padded(spec, std::string_view(), sv, Align::kLeft, false);
  }

  void write_pointer_spec(const void* ptr, const FormatSpec& spec) {
    char buf[18];
    const size_t size = format_pointer_to(buf, ptr);
    write_padded(spec, std::string_view(buf, 2), std::string_view(buf + 2, size - 2), Align::kRight, true);
  }

  void write_arg_spec(const FormatArg<CharT>& arg, const FormatSpec& spec) {
    switch (arg.type()) {
      case Type::kInt:
        write_int_spec(arg.value().int_value, spec);
        break;
      case Type::kUint:
        write_uint_spec(arg.value().uint_value, false, spec);
        break;
      case Type::kLongLong:
        write_int_spec(arg.value().long_long_value, spec);
        break;
      case Type::kUlongLong:
        write_uint_spec(arg.value().ulong_long_value, false, spec);
        break;
      case Type::kBool:
        write_bool_spec(arg.value().bool_value, spec);
        break;
      case Type::kChar:
        write_char_spec(arg.value().char_value, spec);
        break;
      case Type::kFloat:
        write_float_spec(arg.value().float_value, spec);
        break;
      case Type::kDouble:
        write_float_spec(arg.value().double_value, spec);
        break;
      case Type::kLongDouble:
        write_float_spec(arg.value().long_double_value, spec);
        break;
      case Type::kCstring:
        write_cstring_spec(arg.value().string_value, spec);
        break;
      case Type::kString:
        write_string_spec(arg.value().string_view_value, spec);
        break;
      case Type::kPointer:
        write_pointer_spec(arg.value().pointer_value, spec);
        break;
      default:
        break;
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

/**
 * @brief Formats @p args into a newly allocated @c std::string.
 *
 * @details
 * Convenience wrapper over the iterator @c format_to; the only entry point that allocates.
 * Hot paths should prefer @c format_to_n with a caller-owned buffer.
 *
 * @tparam ArgsT  Argument types.
 * @param fmt   Format string.
 * @param args  Arguments to substitute.
 * @return Formatted result.
 */
template <typename... ArgsT>
[[nodiscard]] inline std::string format(format_string<ArgsT...> fmt, const ArgsT&... args);

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

inline size_t format_base_digits_to(char (&buf)[64], unsigned long long value, unsigned base_bits,
                                    bool upper) noexcept {
  static constexpr const char kLowerDigits[] = "0123456789abcdef";
  static constexpr const char kUpperDigits[] = "0123456789ABCDEF";
  const char* digit_chars = upper ? kUpperDigits : kLowerDigits;
  const unsigned mask = (1U << base_bits) - 1U;
  char* p = buf + sizeof(buf);

  do {
    *--p = digit_chars[value & mask];
    value >>= base_bits;
  } while (value != 0U);

  return static_cast<size_t>(buf + sizeof(buf) - p);
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

template <typename... ArgsT>
inline std::string format::format(format_string<ArgsT...> fmt, const ArgsT&... args) {
  char buf[256];
  const auto result = ::vlink::format::format_to_n(buf, sizeof(buf), fmt, args...);

  if VLIKELY (!result.truncated) {
    return std::string(buf, result.size);
  }

  std::string full(result.size, '\0');
  (void)::vlink::format::format_to_n(full.data(), full.size(), fmt, args...);

  return full;
}

}  // namespace vlink
