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

#include <vlink/vlink.h>

#include <atomic>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <ostream>
#include <sstream>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

enum class DumpType : uint8_t {
  kConsole = 0,
  kCsv,
  kJson,
  kBin,
  kJpg,
  kH264,
  kH265,
  kRaw,
  kPcd,
  kSlice,
  kScan,
};

using VariantType = std::variant<int64_t, uint64_t, double, std::string, vlink::Bytes>;
using RawSub = vlink::Subscriber<vlink::Bytes>;

struct DumpRecord final {
  int64_t timestamp{0};
  std::vector<VariantType> values;
  std::vector<double> expr_results;
};

using DumpCallback = vlink::Function<void(int64_t timestamp, const std::string& url, const std::string& ser,
                                          vlink::SchemaType schema_type, const vlink::Bytes& bytes)>;

inline std::string variant_to_string(const VariantType& v) {
  if (std::holds_alternative<int64_t>(v)) {
    return std::to_string(std::get<int64_t>(v));
  }

  if (std::holds_alternative<uint64_t>(v)) {
    return std::to_string(std::get<uint64_t>(v));
  }

  if (std::holds_alternative<double>(v)) {
    std::ostringstream oss;
    oss << std::setprecision(12) << std::get<double>(v);
    return oss.str();
  }

  if (std::holds_alternative<std::string>(v)) {
    return std::get<std::string>(v);
  }

  if (std::holds_alternative<vlink::Bytes>(v)) {
    return "<bytes:" + std::to_string(std::get<vlink::Bytes>(v).size()) + ">";
  }

  return {};
}

inline bool variant_to_double(const VariantType& v, double& out) {
  static std::atomic_bool warned_integer_precision{false};
  const bool signed_precision_loss = std::holds_alternative<int64_t>(v) && (std::get<int64_t>(v) > 9007199254740992LL ||
                                                                            std::get<int64_t>(v) < -9007199254740992LL);
  const bool unsigned_precision_loss =
      std::holds_alternative<uint64_t>(v) && std::get<uint64_t>(v) > 9007199254740992ULL;

  if VUNLIKELY ((signed_precision_loss || unsigned_precision_loss) &&
                !warned_integer_precision.exchange(true, std::memory_order_relaxed)) {
    std::cerr << "Warning: expression input exceeds the exact integer range of ExprTk double values (2^53)."
              << std::endl;
  }

  if (std::holds_alternative<int64_t>(v)) {
    out = static_cast<double>(std::get<int64_t>(v));
    return true;
  }

  if (std::holds_alternative<uint64_t>(v)) {
    out = static_cast<double>(std::get<uint64_t>(v));
    return true;
  }

  if (std::holds_alternative<double>(v)) {
    out = std::get<double>(v);
    return true;
  }

  return false;
}

inline void write_csv_cell(std::ostream& out, std::string_view cell) {
  bool needs_quote = false;

  for (char ch : cell) {
    if (ch == ',' || ch == '"' || ch == '\n' || ch == '\r') {
      needs_quote = true;
      break;
    }
  }

  if (!needs_quote) {
    out << cell;
    return;
  }

  out << '"';

  for (char ch : cell) {
    if (ch == '"') {
      out << "\"\"";
    } else {
      out << ch;
    }
  }

  out << '"';
}

inline void write_seconds_from_us(std::ostream& out, int64_t timestamp_us) {
  const int64_t seconds = timestamp_us / 1000000;
  const int64_t remainder = timestamp_us % 1000000;

  if (timestamp_us < 0 && seconds == 0) {
    out << '-';
  }

  out << seconds << '.';
  const auto previous_fill = out.fill('0');
  out << std::setw(6) << (remainder < 0 ? -remainder : remainder);
  out.fill(previous_fill);
}

inline std::string seconds_string_from_us(int64_t timestamp_us) {
  std::ostringstream oss;
  write_seconds_from_us(oss, timestamp_us);
  return oss.str();
}
