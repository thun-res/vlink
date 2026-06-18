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

#include "./base/helpers.h"

#include <charconv>
#include <chrono>
#include <clocale>
#include <codecvt>
#include <cstdio>
#include <cwchar>
#include <filesystem>
#include <iomanip>
#include <locale>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#ifdef _WIN32
#include <Windows.h>
#endif

namespace vlink {

namespace Helpers {

int to_int(const std::string& str, int dv) noexcept {
  if (str.empty()) {
    return dv;
  }

  try {
    size_t pos = 0;
    auto value = std::stoi(str, &pos, 0);

    if (pos != str.size()) {
      return dv;
    }

    return value;
  } catch (std::exception&) {
    return dv;
  }
}

int64_t to_long(const std::string& str, int64_t dv, int offset) noexcept {
  if (str.empty()) {
    return dv;
  }

  try {
    size_t pos = 0;
    auto value = std::stoll(str, &pos, 0);

    if (pos != str.size() - offset) {
      return dv;
    }

    return value;
  } catch (std::exception&) {
    return dv;
  }
}

std::string double_to_string(double value, int precision) noexcept {
  // char buffer[64];

  // auto [ptr, ec] = std::to_chars(buffer, buffer + sizeof(buffer), value, std::chars_format::fixed, precision);

  // if (ec == std::errc()) {
  //   return std::string(buffer, ptr);
  // }

  // return {};

  thread_local std::ostringstream ss;
  ss.clear();
  ss.str("");

  ss << std::fixed << std::setprecision(precision) << value;

  return ss.str();
}

uint64_t hash_combine(uint64_t a, uint64_t b) noexcept {
  a ^= b + 0x9e3779b97f4a7c15ULL + (a << 6) + (a >> 2);
  return a;
}

void replace_string(std::string& str, const std::string& from, const std::string& to) noexcept {
  if (from.empty() || str.empty()) {
    return;
  }

  size_t pos = 0;

  while ((pos = str.find(from, pos)) != std::string::npos) {
    str.replace(pos, from.length(), to);
    pos += to.length();
  }
}

std::string trim_string(const std::string& str) noexcept {
  if (str.empty()) {
    return {};
  }

  auto start = str.begin();

  while (start != str.end() && std::isspace(static_cast<unsigned char>(*start)) != 0) {
    ++start;
  }

  if VUNLIKELY (start == str.end()) {
    return {};
  }

  auto end = str.rbegin();

  while (end != str.rend() && std::isspace(static_cast<unsigned char>(*end)) != 0) {
    ++end;
  }

  return std::string(start, end.base());
}

std::wstring string_to_wstring(const std::string& input) noexcept {
#ifdef _WIN32
  std::wstring dest;
  int length = ::MultiByteToWideChar(CP_UTF8, 0, input.c_str(), input.size(), nullptr, 0);

  if VUNLIKELY (length <= 0) {
    return dest;
  }

  auto* buffer = new (std::nothrow) WCHAR[length + 1];

  if VUNLIKELY (!buffer) {
    return dest;
  }

  ::MultiByteToWideChar(CP_UTF8, 0, input.c_str(), input.size(), buffer, length);

  buffer[length] = '\0';
  dest.append(buffer);
  delete[] buffer;

  return dest;
#else
  std::setlocale(LC_ALL, "en_US.UTF-8");

  std::mbstate_t state = std::mbstate_t();
  const char* src = input.c_str();

  size_t len = std::mbsrtowcs(nullptr, &src, 0, &state);

  if VUNLIKELY (len == static_cast<size_t>(-1)) {
    return std::wstring();
  }

  std::wstring dest(len, L'\0');
  std::mbsrtowcs(dest.data(), &src, len, &state);

  return dest;
#endif
}

std::string wstring_to_string(const std::wstring& input) noexcept {
#ifdef _WIN32
  std::string dest;
  int length = ::WideCharToMultiByte(CP_UTF8, 0, input.c_str(), input.size(), nullptr, 0, nullptr, nullptr);

  if VUNLIKELY (length <= 0) {
    return dest;
  }

  auto* buffer = new (std::nothrow) char[length + 1];

  if VUNLIKELY (!buffer) {
    return dest;
  }

  ::WideCharToMultiByte(CP_UTF8, 0, input.c_str(), input.size(), buffer, length, nullptr, nullptr);

  buffer[length] = '\0';
  dest.append(buffer);
  delete[] buffer;

  return dest;
#else
  std::setlocale(LC_ALL, "en_US.UTF-8");

  std::mbstate_t state = std::mbstate_t();
  const wchar_t* src = input.c_str();

  size_t len = std::wcsrtombs(nullptr, &src, 0, &state);

  if VUNLIKELY (len == static_cast<size_t>(-1)) {
    return std::string();
  }

  std::string dest(len, '\0');
  std::wcsrtombs(dest.data(), &src, len, &state);

  return dest;
#endif
}

std::string string_local_to_utf8(const std::string& local_str) noexcept {
#ifdef _WIN32

  if VUNLIKELY (local_str.empty()) {
    return {};
  }

  int wide_size = MultiByteToWideChar(CP_ACP, 0, local_str.c_str(), -1, nullptr, 0);

  if VUNLIKELY (wide_size <= 0) {
    return {};
  }

  std::wstring wide_str(wide_size, L'\0');
  MultiByteToWideChar(CP_ACP, 0, local_str.c_str(), -1, wide_str.data(), wide_size);

  int utf8_size = WideCharToMultiByte(CP_UTF8, 0, wide_str.c_str(), -1, nullptr, 0, nullptr, nullptr);

  if VUNLIKELY (utf8_size <= 0) {
    return {};
  }

  std::string utf8_str(utf8_size - 1, '\0');
  WideCharToMultiByte(CP_UTF8, 0, wide_str.c_str(), -1, utf8_str.data(), utf8_size, nullptr, nullptr);

  return utf8_str;
#else
  return local_str;
#endif
}

std::string string_utf8_to_local(const std::string& utf8_str) noexcept {
#ifdef _WIN32

  if VUNLIKELY (utf8_str.empty()) {
    return {};
  }

  int wide_size = MultiByteToWideChar(CP_UTF8, 0, utf8_str.c_str(), -1, nullptr, 0);

  if VUNLIKELY (wide_size <= 0) {
    return {};
  }

  std::wstring wide_str(wide_size, L'\0');
  MultiByteToWideChar(CP_UTF8, 0, utf8_str.c_str(), -1, wide_str.data(), wide_size);

  int local_size = WideCharToMultiByte(CP_ACP, 0, wide_str.c_str(), -1, nullptr, 0, nullptr, nullptr);

  if VUNLIKELY (local_size <= 0) {
    return {};
  }

  std::string local_str(local_size - 1, '\0');
  WideCharToMultiByte(CP_ACP, 0, wide_str.c_str(), -1, local_str.data(), local_size, nullptr, nullptr);

  return local_str;
#else
  return utf8_str;
#endif
}

std::string path_to_string(const std::filesystem::path& path) noexcept {
  try {
#ifdef _WIN32
#if __cplusplus >= 202002L
    auto u8str = path.u8string();
    return std::string(u8str.begin(), u8str.end());
#else
    return path.u8string();
#endif
#else
    return path.string();
#endif
  } catch (std::filesystem::filesystem_error&) {
    return std::string();
  }
}

std::vector<std::string> get_split_string(const std::string& str, char f) noexcept {
  if (str.empty()) {
    return {};
  }

  std::vector<std::string> result;

  size_t start = 0;
  size_t end = 0;

  while ((end = str.find(f, start)) != std::string::npos) {
    if (end > start) {
      result.emplace_back(str.substr(start, end - start));
    }

    start = end + 1;
  }

  if (start < str.size()) {
    result.emplace_back(str.substr(start));
  }

  return result;
}

std::vector<std::string_view> get_split_string_view(std::string_view str, char f) noexcept {
  if (str.empty()) {
    return {};
  }

  std::vector<std::string_view> result;

  size_t start = 0;
  size_t end = 0;

  while ((end = str.find(f, start)) != std::string_view::npos) {
    if (end > start) {
      result.emplace_back(str.substr(start, end - start));
    }

    start = end + 1;
  }

  if (start < str.size()) {
    result.emplace_back(str.substr(start));
  }

  return result;
}

std::vector<std::string> get_split_filter(const std::string& str) noexcept {
  if (str.empty()) {
    return {};
  }

  static constexpr const char* kFilterDelimiters = " ,";

  std::vector<std::string> result;

  size_t start = 0;
  size_t end = 0;

  while ((end = str.find_first_of(kFilterDelimiters, start)) != std::string::npos) {
    if (end > start) {
      auto token = trim_string(str.substr(start, end - start));

      if (!token.empty()) {
        result.emplace_back(std::move(token));
      }
    }

    start = end + 1;
  }

  if (start < str.size()) {
    auto token = trim_string(str.substr(start));

    if (!token.empty()) {
      result.emplace_back(std::move(token));
    }
  }

  return result;
}

std::pair<std::string, std::string> get_pair_string(const std::string& str, char f) noexcept {
  if (str.empty()) {
    return {};
  }

  auto pos = str.find(f);

  if (pos == std::string::npos) {
    return std::pair{trim_string(str), std::string()};
  }

  return std::pair{trim_string(str.substr(0, pos)), trim_string(str.substr(pos + 1))};
}

std::pair<std::string_view, std::string_view> get_pair_string_view(std::string_view str, char f) noexcept {
  if (str.empty()) {
    return {};
  }

  auto pos = str.find(f);

  if (pos == std::string_view::npos) {
    return std::pair{str, std::string_view()};
  }

  return std::pair{str.substr(0, pos), str.substr(pos + 1)};
}

std::string escape_field(std::string_view value) noexcept {
  static constexpr char kHex[] = "0123456789ABCDEF";

  std::string out;
  out.reserve(value.size());

  for (const unsigned char ch : value) {
    if (ch == '%' || ch == ' ' || ch == ':' || ch == '\n' || ch == '\r') {
      out.push_back('%');
      out.push_back(kHex[(ch >> 4U) & 0x0FU]);
      out.push_back(kHex[ch & 0x0FU]);
    } else {
      out.push_back(static_cast<char>(ch));
    }
  }

  return out;
}

std::string unescape_field(std::string_view value) noexcept {
  auto from_hex = [](char ch) noexcept -> int {
    if (ch >= '0' && ch <= '9') {
      return ch - '0';
    }

    if (ch >= 'A' && ch <= 'F') {
      return ch - 'A' + 10;
    }

    if (ch >= 'a' && ch <= 'f') {
      return ch - 'a' + 10;
    }

    return -1;
  };

  std::string out;
  out.reserve(value.size());

  for (size_t i = 0; i < value.size(); ++i) {
    if (value[i] == '%' && i + 2U < value.size()) {
      const int hi = from_hex(value[i + 1U]);
      const int lo = from_hex(value[i + 2U]);

      if VLIKELY (hi >= 0 && lo >= 0) {
        out.push_back(static_cast<char>((hi << 4U) | lo));
        i += 2U;

        continue;
      }
    }

    out.push_back(value[i]);
  }

  return out;
}

uint32_t get_hash_code(const std::string& str) noexcept {
  if (str.empty()) {
    return 0;
  }

  uint32_t value = 0;

  auto [p, error] = std::from_chars(str.data(), str.data() + str.size(), value);

  if (error == std::errc()) {
    return value;
  }

  value = static_cast<uint32_t>(std::hash<std::string>{}(str));

  return value;
}

std::string format_milliseconds(int64_t milliseconds, bool show_millis) noexcept {
  char buffer[32];

  if (milliseconds < 0) {
    milliseconds += 24 * 60 * 60 * 1000;
  }

  int64_t hours = milliseconds / (1000 * 60 * 60);
  int64_t minutes = (milliseconds % (1000 * 60 * 60)) / (1000 * 60);
  int64_t seconds = ((milliseconds % (1000 * 60 * 60)) % (1000 * 60)) / 1000;

  // NOLINTBEGIN

  if (show_millis) {
    int64_t millis = ((milliseconds % (1000 * 60 * 60)) % (1000 * 60)) % 1000;

    std::snprintf(buffer, sizeof(buffer), "%02lld:%02lld:%02lld:%03lld", static_cast<long long>(hours),
                  static_cast<long long>(minutes), static_cast<long long>(seconds), static_cast<long long>(millis));

  } else {
    std::snprintf(buffer, sizeof(buffer), "%02lld:%02lld:%02lld", static_cast<long long>(hours),
                  static_cast<long long>(minutes), static_cast<long long>(seconds));
  }

  // NOLINTEND

  return buffer;
}

std::string format_date(int64_t nanoseconds_since_epoch) noexcept {
  auto time_point = std::chrono::time_point<std::chrono::system_clock, std::chrono::nanoseconds>(
      std::chrono::nanoseconds(nanoseconds_since_epoch));

  auto system_time = std::chrono::time_point_cast<std::chrono::milliseconds>(time_point);
  auto duration_since_epoch = system_time.time_since_epoch();
  auto seconds = std::chrono::duration_cast<std::chrono::seconds>(duration_since_epoch);
  auto milliseconds = (duration_since_epoch - seconds).count();

  std::time_t time_t_seconds = seconds.count();
  std::tm tm_result{};
  std::tm* tm_ptr = nullptr;

#if defined(_WIN32) || defined(_WIN64)

  if (gmtime_s(&tm_result, &time_t_seconds) == 0) {
    tm_ptr = &tm_result;
  }
#else

  if (gmtime_r(&time_t_seconds, &tm_result) != nullptr) {
    tm_ptr = &tm_result;
  }
#endif

  if VUNLIKELY (!tm_ptr) {
    return {};
  }

  char buffer[32];
  std::strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", tm_ptr);

  char full_buffer[64];
  std::snprintf(full_buffer, sizeof(full_buffer), "%s.%03d", buffer, static_cast<int>(milliseconds));

  return full_buffer;
}

std::string format_time_diff(int32_t milliseconds) noexcept {
  bool negative = milliseconds < 0;

  int64_t abs_milliseconds = negative ? -static_cast<int64_t>(milliseconds) : static_cast<int64_t>(milliseconds);

  int hours = static_cast<int>(abs_milliseconds / (1000 * 60 * 60));
  int minutes = static_cast<int>((abs_milliseconds % (1000 * 60 * 60)) / (1000 * 60));
  int seconds = static_cast<int>((abs_milliseconds % (1000 * 60)) / 1000);
  int millis = static_cast<int>(abs_milliseconds % 1000);

  char buffer[32];

  if (negative) {
    std::snprintf(buffer, sizeof(buffer), "-%02d:%02d:%02d:%03d", hours, minutes, seconds, millis);
  } else {
    std::snprintf(buffer, sizeof(buffer), "%02d:%02d:%02d:%03d", hours, minutes, seconds, millis);
  }

  return buffer;
}

std::string format_hex_number(int64_t hex_number) noexcept {
  char buffer[32];
  auto [ptr, ec] = std::to_chars(buffer, buffer + sizeof(buffer), hex_number, 16);

  if (ec == std::errc()) {
    return "0x" + std::string(buffer, ptr);
  }

  return "0x0";
}

std::string format_hex_number(uint64_t hex_number) noexcept {
  char buffer[32];
  auto [ptr, ec] = std::to_chars(buffer, buffer + sizeof(buffer), hex_number, 16);

  if (ec == std::errc()) {
    std::string result = "0x";

    for (char* p = buffer; p < ptr; ++p) {
      result += static_cast<char>(std::toupper(*p));  // NOLINT(clang-analyzer-core.CallAndMessage)
    }

    return result;
  }

  return "0x0";
}

std::string format_file_size(size_t size) noexcept {
  char buffer[32];

  if (size < 1024LL * 1024) {
    std::snprintf(buffer, sizeof(buffer), "%.2fKB", size / 1024.0);
  } else if (size < 1024LL * 1024 * 1024) {
    std::snprintf(buffer, sizeof(buffer), "%.2fMB", size / 1024.0 / 1024.0);
  } else {
    std::snprintf(buffer, sizeof(buffer), "%.2fGB", size / 1024.0 / 1024.0 / 1024.0);
  }

  return buffer;
}

std::string format_rate_size(size_t size) noexcept {
  char buffer[32];

  if (size < 1024) {
    std::snprintf(buffer, sizeof(buffer), "%.2fB/s", static_cast<double>(size));
  } else if (size < 1024LL * 1024) {
    std::snprintf(buffer, sizeof(buffer), "%.2fKB/s", size / 1024.0);
  } else if (size < 1024LL * 1024 * 1024) {
    std::snprintf(buffer, sizeof(buffer), "%.2fMB/s", size / 1024.0 / 1024.0);
  } else {
    std::snprintf(buffer, sizeof(buffer), "%.2fGB/s", size / 1024.0 / 1024.0 / 1024.0);
  }

  return buffer;
}

int64_t convert_date_to_timestamp(const std::string& date) noexcept {
  std::tm tm = {};
  char delimiter_ms;
  int milliseconds = 0;

  thread_local std::istringstream ss;
  ss.clear();
  ss.str(date);

  ss >> std::get_time(&tm, "%Y/%m/%d %H:%M:%S");

  if (ss.fail()) {
    return -1;
  }

  if (ss >> delimiter_ms >> milliseconds) {
    if (delimiter_ms != ':' || milliseconds < 0 || milliseconds >= 1000) {
      return -1;
    }
  }

  auto tp = std::chrono::system_clock::from_time_t(std::mktime(&tm));
  auto ns = std::chrono::time_point_cast<std::chrono::nanoseconds>(tp).time_since_epoch().count();

  return ns + (static_cast<int64_t>(milliseconds) * 1'000'000LL);
}

}  // namespace Helpers

}  // namespace vlink
