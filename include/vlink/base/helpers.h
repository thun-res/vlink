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
 * @file helpers.h
 * @brief Stateless string, number, hash and formatting helpers used throughout VLink.
 *
 * @details
 * Functions in this header are portable, allocation-free where possible, and never throw; bad
 * input is reported via empty / default return values.  Encoding conversion routines fall back
 * to a pass-through on POSIX where the input is assumed to already be UTF-8.
 *
 * @par Helper categories
 *
 * | Category          | Representative entry points                                                                  |
 * | ----------------- | -------------------------------------------------------------------------------------------- |
 * | Number parsing    | @c to_int, @c to_long, @c double_to_string                                                   |
 * | Hashing           | @c hash_combine, @c get_hash_code                                                            |
 * | String editing    | @c replace_string, @c trim_string, @c trim_string_view                                       |
 * | Wide / locale     | @c string_to_wstring, @c wstring_to_string, @c string_local_to_utf8, @c string_utf8_to_local |
 * | Filesystem        | @c path_to_string                                                                            |
 * | Splitting         | @c split, @c split_view, @c split_any, @c split_any_view                                     |
 * | Field encoding    | @c escape_field, @c unescape_field                                                           |
 * | Time formatting   | @c format_milliseconds, @c format_date, @c format_time_diff                                  |
 * | Number formatting | @c format_hex_number, @c format_file_size, @c format_rate_size                               |
 * | Date parsing      | @c convert_date_to_timestamp                                                                 |
 * | Prefix / suffix   | @c has_startwith, @c has_endwith, @c contains_substring                                      |
 *
 * @par Example
 * @code
 *   auto parts = vlink::Helpers::split("a,b,c", ',');   // {"a", "b", "c"}
 *   std::string size = vlink::Helpers::format_file_size(1536);     // "1.50KB"
 *   bool startsWith = vlink::Helpers::has_startwith("vlink://hello", "vlink://");
 * @endcode
 */

#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "./macros.h"

namespace vlink {

/**
 * @namespace vlink::Helpers
 * @brief Stateless utility functions shared across VLink subsystems.
 */
namespace Helpers {  // NOLINT(readability-identifier-naming)

/**
 * @brief Parses an integer string with auto base detection (@c 0x -> hex, leading @c 0 -> octal,
 *        otherwise decimal).
 *
 * @param str  Source string.
 * @param dv   Default value returned on parse failure.  Default: @c 0.
 * @return Parsed @c int or @p dv on failure.
 */
[[nodiscard]] VLINK_EXPORT int to_int(const std::string& str, int dv = 0) noexcept;

/**
 * @brief Parses a 64-bit integer string with optional trailing-character allowance.
 *
 * @details
 * Calls @c std::stoll with auto base detection (@c 0x -> hex, leading @c 0 -> octal, otherwise
 * decimal).  Parsing always starts at index @c 0; @p offset is the number of trailing characters
 * the parser may leave unconsumed (for example to skip a unit suffix such as @c "100s" with
 * @p offset @c == @c 1).  Mismatched consumption returns @p dv.
 *
 * @param str     Source string.
 * @param dv      Default value on failure.  Default: @c 0.
 * @param offset  Number of trailing characters allowed to remain unconsumed.  Default: @c 0.
 * @return Parsed @c int64_t or @p dv on failure.
 */
[[nodiscard]] VLINK_EXPORT int64_t to_long(const std::string& str, int64_t dv = 0, int offset = 0) noexcept;

/**
 * @brief Formats a @c double with the requested decimal precision.
 *
 * @param value      Source value.
 * @param precision  Number of digits after the decimal point.  Default: @c 2.
 * @return Decimal string representation.
 */
[[nodiscard]] VLINK_EXPORT std::string double_to_string(double value, int precision = 2) noexcept;

/**
 * @brief Combines two 64-bit hash values into one via a Murmur-style mix.
 *
 * @param a  First hash value.
 * @param b  Second hash value.
 * @return Mixed hash value.
 */
[[nodiscard]] VLINK_EXPORT uint64_t hash_combine(uint64_t a, uint64_t b) noexcept;

/**
 * @brief Replaces all occurrences of @p from in @p str with @p to in place.
 *
 * @param str   String mutated in place.
 * @param from  Substring to search for.
 * @param to    Replacement substring.
 */
VLINK_EXPORT void replace_string(std::string& str, const std::string& from, const std::string& to) noexcept;

/**
 * @brief Returns @p str with leading and trailing whitespace removed.
 *
 * @param str  Source string.
 * @return Trimmed copy.
 */
[[nodiscard]] VLINK_EXPORT std::string trim_string(const std::string& str) noexcept;

/**
 * @brief Trims whitespace from both sides of a @c string_view without allocating.
 *
 * @details
 * The returned view references the same storage as @p str; callers must keep
 * the original character storage alive while using the result.
 *
 * @param str  Source view.
 * @return Trimmed view, or an empty view when @p str is empty or all whitespace.
 */
[[nodiscard]] VLINK_EXPORT std::string_view trim_string_view(std::string_view str) noexcept;

/**
 * @brief Converts UTF-8 to a wide-character string.
 *
 * @param input  UTF-8 source.
 * @return Wide-character result; empty on failure.
 */
[[nodiscard]] VLINK_EXPORT std::wstring string_to_wstring(const std::string& input) noexcept;

/**
 * @brief Converts a wide-character string to UTF-8.
 *
 * @param input  Wide source.
 * @return UTF-8 result; empty on failure.
 */
[[nodiscard]] VLINK_EXPORT std::string wstring_to_string(const std::wstring& input) noexcept;

/**
 * @brief Converts a system-locale string to UTF-8.
 *
 * @details
 * Performs Windows ANSI-code-page conversion; returns the input unchanged on POSIX.
 *
 * @param local_str  Locally encoded string.
 * @return UTF-8 string, or the input when no conversion is needed.
 */
[[nodiscard]] VLINK_EXPORT std::string string_local_to_utf8(const std::string& local_str) noexcept;

/**
 * @brief Converts UTF-8 to the active system locale on Windows; pass-through on POSIX.
 *
 * @param utf8_str  UTF-8 source.
 * @return Locally encoded string.
 */
[[nodiscard]] VLINK_EXPORT std::string string_utf8_to_local(const std::string& utf8_str) noexcept;

/**
 * @brief Converts a filesystem path to a portable UTF-8 string.
 *
 * @param path  Source path.
 * @return UTF-8 path string.
 */
[[nodiscard]] VLINK_EXPORT std::string path_to_string(const std::filesystem::path& path) noexcept;

/**
 * @brief Splits a string by a single-character delimiter; empty parts are skipped.
 *
 * @param str  Source string.
 * @param f    Delimiter character. Defaults to @c ','.
 * @return Vector of non-empty substrings.
 */
[[nodiscard]] VLINK_EXPORT std::vector<std::string> split(const std::string& str, char f = ',') noexcept;

/**
 * @brief Splits a @c string_view by a delimiter and returns non-owning views.
 *
 * @details
 * The caller must keep @p str alive while the returned views are used.
 *
 * @param str  Source view.
 * @param f    Delimiter character. Defaults to @c ','.
 * @return Vector of non-empty views over the parts.
 */
[[nodiscard]] VLINK_EXPORT std::vector<std::string_view> split_view(std::string_view str, char f = ',') noexcept;

/**
 * @brief Splits a string by any delimiter in @p delimiters; each token is trimmed and empty tokens are skipped.
 *
 * @param str         Source string.
 * @param delimiters  Delimiter character set. Defaults to @c " ,".
 * @return Vector of trimmed, non-empty tokens.
 */
[[nodiscard]] VLINK_EXPORT std::vector<std::string> split_any(const std::string& str,
                                                              std::string_view delimiters = " ,") noexcept;

/**
 * @brief Splits a view by any delimiter in @p delimiters and returns non-owning trimmed views.
 *
 * @details
 * The caller must keep @p str alive while the returned views are used.
 *
 * @param str         Source view.
 * @param delimiters  Delimiter character set. Defaults to @c " ,".
 * @return Vector of trimmed, non-empty views over the tokens.
 */
[[nodiscard]] VLINK_EXPORT std::vector<std::string_view> split_any_view(std::string_view str,
                                                                        std::string_view delimiters = " ,") noexcept;

/**
 * @brief Percent-encodes one field of a delimiter-separated text frame.
 *
 * @details
 * Encodes @c %, space, @c :, LF and CR as uppercase @c %XX sequences; other bytes pass through.
 *
 * @param value  Field to encode.
 * @return Encoded field.
 */
[[nodiscard]] VLINK_EXPORT std::string escape_field(std::string_view value) noexcept;

/**
 * @brief Reverses @c escape_field encoding.
 *
 * @details
 * Decodes valid @c %XX sequences; preserves invalid or truncated sequences verbatim so legacy
 * frames remain parseable.
 *
 * @param value  Encoded field.
 * @return Decoded field.
 */
[[nodiscard]] VLINK_EXPORT std::string unescape_field(std::string_view value) noexcept;

/**
 * @brief Maps a string to a 32-bit code.
 *
 * @details
 * If @p str parses as an integer it is returned as its numeric value; otherwise the result is
 * @c std::hash<std::string> truncated to 32 bits.  An empty string yields @c 0.
 *
 * @param str  Source string.
 * @return 32-bit code.
 */
[[nodiscard]] VLINK_EXPORT uint32_t get_hash_code(const std::string& str) noexcept;

/**
 * @brief Renders a millisecond duration as a human-readable time string.
 *
 * @param milliseconds  Source duration.
 * @param show_millis   When @c true the millisecond field is appended.
 * @return Formatted duration string.
 */
[[nodiscard]] VLINK_EXPORT std::string format_milliseconds(int64_t milliseconds, bool show_millis) noexcept;

/**
 * @brief Renders a nanosecond Unix timestamp as @c "YYYY-MM-DD @c HH:MM:SS.mmm" in UTC.
 *
 * @param nanoseconds_since_epoch  Source timestamp.
 * @return Formatted date string.
 */
[[nodiscard]] VLINK_EXPORT std::string format_date(int64_t nanoseconds_since_epoch) noexcept;

/**
 * @brief Renders a millisecond time delta as @c "HH:MM:SS:mmm".
 *
 * @param milliseconds  Source delta.
 * @return Formatted delta string.
 */
[[nodiscard]] VLINK_EXPORT std::string format_time_diff(int32_t milliseconds) noexcept;

/**
 * @brief Renders a signed 64-bit integer as a @c "0x..." hex literal.
 *
 * @param hex_number  Source value.
 * @return Hex literal string.
 */
[[nodiscard]] VLINK_EXPORT std::string format_hex_number(int64_t hex_number) noexcept;

/**
 * @brief Renders an unsigned 64-bit integer as a @c "0x..." hex literal.
 *
 * @param hex_number  Source value.
 * @return Hex literal string.
 */
[[nodiscard]] VLINK_EXPORT std::string format_hex_number(uint64_t hex_number) noexcept;

/**
 * @brief Renders a byte count as a KB / MB / GB string with two decimal places.
 *
 * @param size  Source value in bytes.
 * @return Human-readable size string.
 */
[[nodiscard]] VLINK_EXPORT std::string format_file_size(size_t size) noexcept;

/**
 * @brief Renders a byte-per-second rate as a B/s, KB/s, MB/s or GB/s string.
 *
 * @param size  Source rate in bytes per second.
 * @return Human-readable rate string.
 */
[[nodiscard]] VLINK_EXPORT std::string format_rate_size(size_t size) noexcept;

/**
 * @brief Parses a @c "%Y/%m/%d @c %H:%M:%S" (optionally @c ":<ms>") date into a Unix nanosecond timestamp.
 *
 * @details
 * Interpreted in the local timezone via @c std::mktime.  ISO-8601 dashes are not accepted.
 *
 * @param date  Source date string.
 * @return Nanoseconds since Unix epoch, or @c -1 on parse failure.
 */
[[nodiscard]] VLINK_EXPORT int64_t convert_date_to_timestamp(const std::string& date) noexcept;

/**
 * @brief Compile-time prefix check against a string literal.
 *
 * @tparam SizeT  Size of @p target including the null terminator (deduced).
 * @param str     String to test.
 * @param target  Literal prefix.
 * @return @c true when @p str starts with @p target.
 */
template <uint8_t SizeT>
[[nodiscard]] bool has_startwith(const std::string& str, const char (&target)[SizeT]) noexcept;

/**
 * @brief Compile-time suffix check against a string literal.
 *
 * @tparam SizeT  Size of @p target including the null terminator (deduced).
 * @param str     String to test.
 * @param target  Literal suffix.
 * @return @c true when @p str ends with @p target.
 */
template <uint8_t SizeT>
[[nodiscard]] bool has_endwith(const std::string& str, const char (&target)[SizeT]) noexcept;

/**
 * @brief Constexpr prefix check for two @c string_view values.
 *
 * @param str     String to test.
 * @param target  Prefix to check for.
 * @return @c true when @p str starts with @p target.
 */
[[nodiscard]] constexpr bool has_startwith(std::string_view str, std::string_view target) noexcept;

/**
 * @brief Constexpr suffix check for two @c string_view values.
 *
 * @param str     String to test.
 * @param target  Suffix to check for.
 * @return @c true when @p str ends with @p target.
 */
[[nodiscard]] constexpr bool has_endwith(std::string_view str, std::string_view target) noexcept;

/**
 * @brief Constexpr substring search.
 *
 * @param sv      String to search.
 * @param needle  Substring to look for; an empty needle returns @c true.
 * @return @c true when @p sv contains @p needle.
 */
[[nodiscard]] constexpr bool contains_substring(std::string_view sv, std::string_view needle) noexcept;

////////////////////////////////////////////////////////////////
/// Details
////////////////////////////////////////////////////////////////

template <uint8_t SizeT>
inline bool has_startwith(const std::string& str, const char (&target)[SizeT]) noexcept {
  if constexpr (SizeT == 0) {
    (void)str;
    return false;
  }

  return str.compare(0, SizeT - 1, target) == 0;
}

template <uint8_t SizeT>
inline bool has_endwith(const std::string& str, const char (&target)[SizeT]) noexcept {
  if constexpr (SizeT == 0) {
    (void)str;
    return false;
  }

  if VUNLIKELY (str.size() < SizeT - 1) {
    return false;
  }

  return str.compare(str.size() - (SizeT - 1), SizeT - 1, target) == 0;
}

inline constexpr bool has_startwith(std::string_view str, std::string_view target) noexcept {
#if __cplusplus >= 202002L
  return str.starts_with(target);
#else
  return target.size() <= str.size() && str.substr(0, target.size()) == target;
#endif
}

inline constexpr bool has_endwith(std::string_view str, std::string_view target) noexcept {
#if __cplusplus >= 202002L
  return str.ends_with(target);
#else
  return target.size() <= str.size() && str.substr(str.size() - target.size(), target.size()) == target;
#endif
}

inline constexpr bool contains_substring(std::string_view sv, std::string_view needle) noexcept {
  if VUNLIKELY (needle.empty()) {
    return true;
  }

  if VUNLIKELY (sv.size() < needle.size()) {
    return false;
  }

  for (size_t i = 0; i <= sv.size() - needle.size(); ++i) {
    bool match = true;

    for (size_t j = 0; j < needle.size(); ++j) {
      if (sv[i + j] != needle[j]) {
        match = false;

        break;
      }
    }

    if (match) {
      return true;
    }
  }

  return false;
}

}  // namespace Helpers

}  // namespace vlink
