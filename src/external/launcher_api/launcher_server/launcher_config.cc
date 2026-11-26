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

#include "./launcher_config.h"
//
#include <nlohmann/json.hpp>
#include <toml++/toml.hpp>
//
#include <vlink/base/helpers.h>
#include <vlink/base/utils.h>
#include <vlink/impl/url_parser.h>
//
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <initializer_list>
#include <limits>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>

#include "../launcher_common.h"

#if !defined(_WIN32) && !defined(_WIN64)
#include <unistd.h>
#endif

namespace vlink {

namespace launcher {

static constexpr size_t kMaxIncludeDepth = 16U;
static constexpr size_t kMaxIncludeExpansions = 256U;
static constexpr size_t kMaxTotalExpandedEntries = 2048U;
static constexpr size_t kMaxConditionDepth = 64U;
static constexpr size_t kMaxParamTableDepth = 32U;
static constexpr size_t kMaxRemapUrlBytes = 120U;
static constexpr int64_t kMaxU32 = 4294967295LL;
static constexpr int64_t kMaxI32 = 2147483647LL;
static constexpr int64_t kMinI32 = -2147483648LL;
static constexpr int64_t kMaxMiB = static_cast<int64_t>(UINT64_MAX / (1024ULL * 1024ULL));

struct LexicalScope final {
  std::unordered_map<std::string, std::string> env;
  std::unordered_map<std::string, std::string> urls;
  std::vector<std::pair<std::string, std::string>> remaps;
  LauncherParamMap params;
};

struct ParseBudget final {
  size_t total_expansions{0U};
  size_t total_includes{0U};
};

[[nodiscard]] static bool ascii_iequals(std::string_view a, std::string_view b) noexcept;

[[nodiscard]] static std::string substitute_vars(std::string_view input,
                                                 const std::unordered_map<std::string, std::string>& args,
                                                 const std::string& this_dir, const std::string& current_ns);

static std::string_view current_os_name() noexcept {
#if defined(_WIN32) || defined(_WIN64)
  return "windows";
#elif defined(__APPLE__)
  return "macos";
#elif defined(__QNXNTO__)
  return "qnx";
#elif defined(__ANDROID__)
  return "android";
#elif defined(__linux__)
  return "linux";
#else
  return "unknown";
#endif
}

static std::string_view current_arch_name() noexcept {
#if defined(__x86_64__) || defined(_M_X64)
  return "x86_64";
#elif defined(__aarch64__) || defined(_M_ARM64)
  return "aarch64";
#elif defined(__arm__) || defined(_M_ARM)
  return "arm";
#elif defined(__riscv)
  return "riscv64";
#elif defined(__i386__) || defined(_M_IX86)
  return "x86";
#else
  return "unknown";
#endif
}

static bool eval_atom_truthy(std::string_view raw) noexcept {
  const std::string_view s = Helpers::trim_string_view(raw);

  if (s.empty() || ascii_iequals(s, "false") || s == "0" || ascii_iequals(s, "no") || ascii_iequals(s, "off")) {
    return false;
  }

  return true;
}

class ConditionParser final {
 public:
  ConditionParser(std::string_view expr, const std::unordered_map<std::string, std::string>& args,
                  const std::string& this_dir, const std::string& current_ns, std::string& error_out)
      : s_(expr), args_(args), this_dir_(this_dir), current_ns_(current_ns), error_(error_out) {}

  bool evaluate(bool& ok) {
    skip_ws();

    if (pos_ >= s_.size()) {
      fail("Condition expression cannot be empty.");
      ok = false;
      return false;
    }

    const bool res = parse_or(0U);
    skip_ws();

    if (ok_ && pos_ < s_.size()) {
      fail(s_[pos_] == ')' ? "Unbalanced parentheses or variable reference in condition expression."
                           : "Unexpected token in condition expression.");
    }

    ok = ok_;
    return ok_ && res;
  }

 private:
  void fail(const std::string& msg) {
    if (ok_) {
      error_ = msg;
      ok_ = false;
    }
  }

  void skip_ws() noexcept {
    while (pos_ < s_.size() && std::isspace(static_cast<unsigned char>(s_[pos_])) != 0) {
      ++pos_;
    }
  }

  bool match(std::string_view tok) noexcept {
    skip_ws();

    if ((pos_ + tok.size()) <= s_.size() && s_.substr(pos_, tok.size()) == tok) {
      pos_ += tok.size();
      return true;
    }

    return false;
  }

  bool parse_or(size_t depth) {
    if (depth > kMaxConditionDepth) {
      fail("Condition expression exceeded maximum nesting depth.");
      return false;
    }

    bool val = parse_and(depth + 1U);

    while (ok_ && match("||")) {
      if (val) {
        (void)parse_and(depth + 1U);
      } else {
        val = parse_and(depth + 1U);
      }
    }

    return val;
  }

  bool parse_and(size_t depth) {
    if (depth > kMaxConditionDepth) {
      fail("Condition expression exceeded maximum nesting depth.");
      return false;
    }

    bool val = parse_unary(depth + 1U);

    while (ok_ && match("&&")) {
      if (!val) {
        (void)parse_unary(depth + 1U);
      } else {
        val = parse_unary(depth + 1U);
      }
    }

    return val;
  }

  bool parse_unary(size_t depth) {
    if (depth > kMaxConditionDepth) {
      fail("Condition expression exceeded maximum nesting depth.");
      return false;
    }

    skip_ws();

    if (pos_ < s_.size() && s_[pos_] == '!' && ((pos_ + 1U) >= s_.size() || s_[pos_ + 1U] != '=')) {
      ++pos_;
      skip_ws();

      if (pos_ >= s_.size()) {
        fail("Dangling '!' operator in condition.");
        return false;
      }

      return !parse_unary(depth + 1U);
    }

    if (pos_ < s_.size() && s_[pos_] == '(') {
      ++pos_;
      const bool inner = parse_or(depth + 1U);
      skip_ws();

      if (ok_ && (pos_ >= s_.size() || s_[pos_] != ')')) {
        fail("Unbalanced parentheses or variable reference in condition expression.");
        return false;
      }

      if (ok_) {
        ++pos_;
      }

      return inner;
    }

    const std::string_view raw_lhs = scan_operand();

    if (!ok_) {
      return false;
    }

    if (raw_lhs.empty()) {
      fail("Empty sub-expression in condition.");
      return false;
    }

    const bool is_eq = match("==");
    const bool is_ne = !is_eq && match("!=");

    if (is_eq || is_ne) {
      const std::string_view raw_rhs = scan_operand();

      if (!ok_) {
        return false;
      }

      if (raw_rhs.empty()) {
        fail(std::string("Missing operand around '") + (is_eq ? "==" : "!=") + "' in condition.");
        return false;
      }

      skip_ws();

      if ((pos_ + 2U) <= s_.size() && (s_.substr(pos_, 2U) == "==" || s_.substr(pos_, 2U) == "!=")) {
        fail("Unexpected chained comparison operator in condition.");
        return false;
      }

      const std::string lhs = substitute_vars(raw_lhs, args_, this_dir_, current_ns_);
      const std::string rhs = substitute_vars(raw_rhs, args_, this_dir_, current_ns_);
      const bool equal = (Helpers::trim_string_view(lhs) == Helpers::trim_string_view(rhs));
      return is_eq ? equal : !equal;
    }

    const std::string expanded = substitute_vars(raw_lhs, args_, this_dir_, current_ns_);
    return eval_atom_truthy(expanded);
  }

  std::string_view scan_operand() {
    skip_ws();
    const size_t start = pos_;
    bool in_var = false;

    while (pos_ < s_.size()) {
      if (in_var) {
        if (s_[pos_] == '}') {
          in_var = false;
        }

        ++pos_;
        continue;
      }

      if ((pos_ + 1U) < s_.size() && s_[pos_] == '$' && s_[pos_ + 1U] == '{') {
        in_var = true;
        pos_ += 2U;
        continue;
      }

      if (s_[pos_] == '(' || s_[pos_] == ')') {
        break;
      }

      if ((pos_ + 1U) < s_.size()) {
        const std::string_view two = s_.substr(pos_, 2U);

        if (two == "||" || two == "&&" || two == "==" || two == "!=") {
          break;
        }
      }

      ++pos_;
    }

    if (in_var) {
      fail("Unbalanced parentheses or variable reference in condition expression.");
      return {};
    }

    return Helpers::trim_string_view(s_.substr(start, pos_ - start));
  }

  std::string_view s_;
  size_t pos_{0U};
  const std::unordered_map<std::string, std::string>& args_;
  const std::string& this_dir_;
  const std::string& current_ns_;
  std::string& error_;
  bool ok_{true};
};

static bool match_remap_prefix(std::string_view url, std::string_view rule) noexcept {
  if (rule.empty() || !Helpers::has_startwith(url, rule)) {
    return false;
  }

  if (url.size() == rule.size()) {
    return true;
  }

  if (rule.back() == '/') {
    return true;
  }

  const char next = url[rule.size()];
  return next == '/' || next == '?';
}

static bool apply_remap_rules(std::string_view candidate,
                              const std::vector<std::pair<std::string, std::string>>& remaps, std::string& resolved) {
  for (auto it = remaps.rbegin(); it != remaps.rend(); ++it) {
    const auto& src = it->first;
    const auto& dst = it->second;

    if (candidate == src) {
      resolved = dst;
      return true;
    }

    if (match_remap_prefix(candidate, src)) {
      resolved = dst;
      resolved.append(candidate.substr(src.size()));
      return true;
    }
  }

  return false;
}

static std::string to_env_url_suffix(std::string_view key) {
  std::string out;
  out.reserve(key.size());

  for (char ch : key) {
    const auto uc = static_cast<unsigned char>(ch);

    if (std::isalnum(uc) != 0) {
      out.push_back(static_cast<char>(std::toupper(uc)));
    } else {
      out.push_back('_');
    }
  }

  return out;
}

Logger::Level parse_log_level_strict(std::string_view raw, bool& ok) noexcept {
  ok = true;
  const std::string_view trimmed = Helpers::trim_string_view(raw);

  if (ascii_iequals(trimmed, "trace")) {
    return Logger::kTrace;
  }

  if (ascii_iequals(trimmed, "debug")) {
    return Logger::kDebug;
  }

  if (ascii_iequals(trimmed, "info")) {
    return Logger::kInfo;
  }

  if (ascii_iequals(trimmed, "warn") || ascii_iequals(trimmed, "warning")) {
    return Logger::kWarn;
  }

  if (ascii_iequals(trimmed, "error")) {
    return Logger::kError;
  }

  if (ascii_iequals(trimmed, "fatal")) {
    return Logger::kFatal;
  }

  if (ascii_iequals(trimmed, "off") || ascii_iequals(trimmed, "none")) {
    return Logger::kOff;
  }

  ok = false;
  return Logger::kInfo;
}

bool split_command_prefix(std::string_view prefix, std::vector<std::string>& out, std::string* error_out) {
  out.clear();
  std::string cur;
  bool token_started = false;
  bool in_single = false;
  bool in_double = false;

  for (size_t i = 0U; i < prefix.size(); ++i) {
    const char ch = prefix[i];

    if (ch == '\'' && !in_double) {
      in_single = !in_single;
      token_started = true;
      continue;
    }

    if (ch == '"' && !in_single) {
      in_double = !in_double;
      token_started = true;
      continue;
    }

#if !defined(_WIN32) && !defined(_WIN64)
    if (ch == '\\' && !in_single && (i + 1U) < prefix.size()) {
      const char next = prefix[i + 1U];

      if (!in_double || next == '"' || next == '\\' || next == '$' || next == '`') {
        cur.push_back(next);
        token_started = true;
        ++i;
        continue;
      }
    }
#else
    if (ch == '\\' && in_double) {
      size_t bs = 0U;

      while ((i + bs) < prefix.size() && prefix[i + bs] == '\\') {
        ++bs;
      }

      if ((i + bs) < prefix.size() && prefix[i + bs] == '"') {
        cur.append(bs / 2U, '\\');

        if ((bs % 2U) == 1U) {
          cur.push_back('"');
          token_started = true;
          i += bs;
        } else {
          token_started = true;
          i += (bs - 1U);
        }

        continue;
      }
    }
#endif

    if (!in_single && !in_double && std::isspace(static_cast<unsigned char>(ch)) != 0) {
      if (token_started) {
        out.push_back(std::move(cur));
        cur.clear();
        token_started = false;
      }

      continue;
    }

    cur.push_back(ch);
    token_started = true;
  }

  if (in_single || in_double) {
    out.clear();

    if (error_out != nullptr) {
      *error_out = "Unclosed quote in command prefix '" + std::string(prefix) + "'.";
    }

    return false;
  }

  if (token_started) {
    out.push_back(std::move(cur));
  }

  return true;
}

std::string resolve_unit_url(std::string_view url, std::string_view ns,
                             const std::vector<std::pair<std::string, std::string>>& remaps) {
  auto normalize_triple_slash = [](std::string& value) {
    const size_t pos = value.find("://");

    if (pos == std::string::npos) {
      return;
    }

    const std::string_view scheme(value.data(), pos);
    const std::string_view rest(value.data() + pos + 3U, value.size() - (pos + 3U));

    if (rest.size() > 1U && rest[0] == '/' && !ascii_iequals(scheme, "uds")) {
      std::string normalized;
      normalized.reserve(value.size() - 1U);
      normalized.append(value.data(), pos + 3U).append(rest.substr(1U));
      value = std::move(normalized);
    }
  };

  std::string resolved(url);
  bool remapped = apply_remap_rules(url, remaps, resolved);
  const size_t scheme_pos = resolved.find("://");

  if (scheme_pos != std::string::npos) {
    const std::string_view rest(resolved.data() + scheme_pos + 3U, resolved.size() - (scheme_pos + 3U));

    if (rest.size() > 1U && rest[0] == '/') {
      normalize_triple_slash(resolved);

      if (!remapped) {
        std::string norm_remapped;

        if (apply_remap_rules(resolved, remaps, norm_remapped)) {
          resolved = std::move(norm_remapped);
          normalize_triple_slash(resolved);
        }
      }
    } else if (!remapped && !ns.empty() && !rest.empty() && rest[0] != '/') {
      const std::string ns_prefix = std::string(ns) + "/";

      if (!Helpers::has_startwith(rest, ns_prefix)) {
        std::string prefixed;
        prefixed.reserve(resolved.size() + ns.size() + 1U);
        prefixed.append(resolved.data(), scheme_pos + 3U).append(ns).append("/").append(rest);
        resolved = std::move(prefixed);
      }

      std::string ns_remapped;

      if (apply_remap_rules(resolved, remaps, ns_remapped)) {
        resolved = std::move(ns_remapped);
        normalize_triple_slash(resolved);
      }
    }
  }

  return resolved;
}

void inject_url_env(const std::unordered_map<std::string, std::string>& urls,
                    const std::unordered_map<std::string, std::string>& inputs,
                    const std::unordered_map<std::string, std::string>& outputs, std::string_view ns,
                    const std::vector<std::pair<std::string, std::string>>& remaps,
                    std::unordered_map<std::string, std::string>& env) {
  auto put_if_absent = [&](const std::string& env_key, const std::string& val) {
    if (!env_key.empty() && env.count(env_key) == 0U) {
      env.emplace(env_key, val);
    }
  };

  auto sorted_entries = [](const std::unordered_map<std::string, std::string>& m) {
    using Entry = std::unordered_map<std::string, std::string>::value_type;
    std::vector<const Entry*> entries;
    entries.reserve(m.size());

    for (const auto& kv : m) {
      entries.push_back(&kv);
    }

    std::sort(entries.begin(), entries.end(), [](const Entry* a, const Entry* b) { return a->first < b->first; });
    return entries;
  };

  for (const auto* entry : sorted_entries(urls)) {
    const std::string suffix = to_env_url_suffix(entry->first);

    if (!suffix.empty()) {
      put_if_absent("VLINK_URL_" + suffix, resolve_unit_url(entry->second, ns, remaps));
    }
  }

  for (const auto* entry : sorted_entries(inputs)) {
    const std::string suffix = to_env_url_suffix(entry->first);

    if (!suffix.empty()) {
      const std::string resolved = resolve_unit_url(entry->second, ns, remaps);
      put_if_absent("VLINK_INPUT_" + suffix, resolved);
      put_if_absent("VLINK_URL_" + suffix, resolved);
    }
  }

  for (const auto* entry : sorted_entries(outputs)) {
    const std::string suffix = to_env_url_suffix(entry->first);

    if (!suffix.empty()) {
      const std::string resolved = resolve_unit_url(entry->second, ns, remaps);
      put_if_absent("VLINK_OUTPUT_" + suffix, resolved);
      put_if_absent("VLINK_URL_" + suffix, resolved);
    }
  }
}

static bool ascii_iequals(std::string_view a, std::string_view b) noexcept {
  if (a.size() != b.size()) {
    return false;
  }

  for (size_t i = 0U; i < a.size(); ++i) {
    auto ca = static_cast<unsigned char>(a[i]);
    auto cb = static_cast<unsigned char>(b[i]);

    if (ca >= 'A' && ca <= 'Z') {
      ca = static_cast<unsigned char>(ca + ('a' - 'A'));
    }

    if (cb >= 'A' && cb <= 'Z') {
      cb = static_cast<unsigned char>(cb + ('a' - 'A'));
    }

    if (ca != cb) {
      return false;
    }
  }

  return true;
}

static std::string to_lower_copy(std::string_view input) {
  std::string out(input);

  for (char& ch : out) {
    ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
  }

  return out;
}

static std::string trim_slashes(std::string_view s) {
  size_t b = 0U;
  size_t e = s.size();

  while (b < e && s[b] == '/') {
    ++b;
  }

  while (e > b && s[e - 1U] == '/') {
    --e;
  }

  return std::string(s.substr(b, e - b));
}

static std::string join_namespace(std::string_view parent, std::string_view child) {
  std::string p = trim_slashes(parent);
  std::string c = trim_slashes(child);

  if (p.empty()) {
    return c;
  }

  if (c.empty()) {
    return p;
  }

  p.push_back('/');
  p.append(c);
  return p;
}

static std::string qualify_name(std::string_view ns, std::string_view name) {
  if (name.empty()) {
    return "";
  }

  if (name[0] == '/') {
    return trim_slashes(name);
  }

  if (name[0] == '@') {
    const std::string_view grp = name.substr(1U);

    if (grp.empty()) {
      return "";
    }

    if (grp[0] == '/') {
      return "@" + trim_slashes(grp);
    }

    const std::string clean_ns = trim_slashes(ns);

    if (clean_ns.empty() || grp.find('/') != std::string_view::npos) {
      return "@" + std::string(grp);
    }

    return "@" + clean_ns + "/" + std::string(grp);
  }

  const std::string clean_ns = trim_slashes(ns);

  if (clean_ns.empty() || name.find('/') != std::string_view::npos) {
    return std::string(name);
  }

  return clean_ns + "/" + std::string(name);
}

static std::string resolve_relative_path(const std::string& raw, const std::string& base_dir) {
  if (raw.empty()) {
    return {};
  }

  std::filesystem::path p(raw);

  if (p.is_relative()) {
    p = std::filesystem::path(base_dir) / p;
  }

  return Helpers::path_to_string(p);
}

static std::string substitute_vars(std::string_view input, const std::unordered_map<std::string, std::string>& args,
                                   const std::string& this_dir, const std::string& current_ns) {
  std::string out;
  out.reserve(input.size());
  size_t pos = 0U;

  while (pos < input.size()) {
    const size_t dollar = input.find('$', pos);

    if (dollar == std::string_view::npos) {
      out.append(input.substr(pos));
      break;
    }

    out.append(input.substr(pos, dollar - pos));

    if (dollar + 2U < input.size() && input[dollar + 1U] == '$' && input[dollar + 2U] == '{') {
      out.append("${");
      pos = dollar + 3U;
      continue;
    }

    if (dollar + 1U >= input.size() || input[dollar + 1U] != '{') {
      out.push_back('$');
      pos = dollar + 1U;
      continue;
    }

    const size_t close = input.find('}', dollar + 2U);

    if (close == std::string_view::npos) {
      out.append(input.substr(dollar));
      break;
    }

    const std::string_view expr = input.substr(dollar + 2U, close - (dollar + 2U));
    std::string_view key = expr;
    std::string_view fallback;
    bool has_fallback = false;
    const size_t colon = expr.find(":-");

    if (colon != std::string_view::npos) {
      key = expr.substr(0U, colon);
      fallback = expr.substr(colon + 2U);
      has_fallback = true;
    }

    std::string replacement;
    bool found = false;

    if (Helpers::has_startwith(key, "arg.")) {
      const std::string arg_name(key.substr(4U));
      auto it = args.find(arg_name);

      if (it != args.end()) {
        replacement = it->second;
        found = true;
      }
    } else if (key == "item" || key == "index") {
      auto it = args.find(std::string(key));

      if (it != args.end()) {
        replacement = it->second;
        found = true;
      }
    } else if (Helpers::has_startwith(key, "url.")) {
      const std::string url_key = "__url__." + std::string(key.substr(4U));
      auto it = args.find(url_key);

      if (it != args.end()) {
        replacement = it->second;
        found = true;
      }
    } else if (Helpers::has_startwith(key, "env.")) {
      const std::string env_name(key.substr(4U));
      const std::string val = Utils::get_env(env_name);

      if (!val.empty()) {
        replacement = val;
        found = true;
      }
    } else if (key == "this_dir") {
      replacement = this_dir;
      found = true;
    } else if (key == "cwd") {
      std::error_code ec;
      replacement = Helpers::path_to_string(std::filesystem::current_path(ec));
      found = true;
    } else if (key == "host") {
      replacement = Utils::get_host_name();
      found = true;
    } else if (key == "pid") {
      replacement = Utils::get_pid_str();
      found = true;
    } else if (key == "os") {
      replacement = current_os_name();
      found = true;
    } else if (key == "arch") {
      replacement = current_arch_name();
      found = true;
    } else if (key == "ns") {
      replacement = current_ns;
      found = true;
    }

    if (found) {
      out.append(replacement);
    } else if (has_fallback) {
      out.append(fallback);
    }

    pos = close + 1U;
  }

  return out;
}

static bool validate_allowed_keys(const toml::table& tbl, std::string_view section,
                                  std::initializer_list<std::string_view> allowed, std::string& error_out) {
  for (const auto& [k, v] : tbl) {
    (void)v;
    const std::string_view key_sv = k.str();
    bool found = false;

    for (const std::string_view ok_key : allowed) {
      if (key_sv == ok_key) {
        found = true;
        break;
      }
    }

    if (!found) {
      error_out = "Unknown key '" + std::string(key_sv) + "' in " + std::string(section) + ".";
      return false;
    }
  }

  return true;
}

static bool require_table_if_present(const toml::table& tbl, std::string_view key, std::string_view section,
                                     const toml::table*& out, std::string& error_out) {
  out = nullptr;
  const auto* node = tbl.get(key);

  if (node == nullptr) {
    return true;
  }

  out = node->as_table();

  if (out == nullptr) {
    error_out = "Key '" + std::string(key) + "' in " + std::string(section) + " must be a table.";
    return false;
  }

  return true;
}

static bool require_array_if_present(const toml::table& tbl, std::string_view key, std::string_view section,
                                     const toml::array*& out, std::string& error_out) {
  out = nullptr;
  const auto* node = tbl.get(key);

  if (node == nullptr) {
    return true;
  }

  out = node->as_array();

  if (out == nullptr) {
    error_out = "Key '" + std::string(key) + "' in " + std::string(section) + " must be an array.";
    return false;
  }

  return true;
}

static bool parse_condition_node(const toml::table& tbl, std::string_view key,
                                 const std::unordered_map<std::string, std::string>& args, const std::string& this_dir,
                                 const std::string& current_ns, std::string& error_out, bool& ok, bool default_val) {
  ok = true;
  const auto* node = tbl.get(key);

  if (node == nullptr) {
    return default_val;
  }

  const auto* b = node->as_boolean();

  if (b != nullptr) {
    return b->get();
  }

  const auto* s = node->as_string();

  if (s != nullptr) {
    ConditionParser parser(s->get(), args, this_dir, current_ns, error_out);
    return parser.evaluate(ok);
  }

  error_out = "Field '" + std::string(key) + "' must be a boolean or string expression.";
  ok = false;
  return false;
}

static std::string get_str_strict(const toml::table& tbl, std::string_view key,
                                  const std::unordered_map<std::string, std::string>& args, const std::string& this_dir,
                                  const std::string& current_ns, std::string_view default_val, std::string& error_out,
                                  bool& ok) {
  ok = true;
  const auto* node = tbl.get(key);

  if (node == nullptr) {
    return std::string(default_val);
  }

  const auto* s = node->as_string();

  if (s != nullptr) {
    return substitute_vars(s->get(), args, this_dir, current_ns);
  }

  error_out = "Field '" + std::string(key) + "' must be a string.";
  ok = false;
  return std::string(default_val);
}

static bool get_bool_strict(const toml::table& tbl, std::string_view key, bool default_val, std::string& error_out,
                            bool& ok) {
  ok = true;
  const auto* node = tbl.get(key);

  if (node == nullptr) {
    return default_val;
  }

  const auto* b = node->as_boolean();

  if (b != nullptr) {
    return b->get();
  }

  error_out = "Field '" + std::string(key) + "' must be a boolean.";
  ok = false;
  return default_val;
}

static std::optional<bool> get_opt_bool_strict(const toml::table& tbl, std::string_view key, std::string& error_out,
                                               bool& ok) {
  ok = true;
  const auto* node = tbl.get(key);

  if (node == nullptr) {
    return std::nullopt;
  }

  const auto* b = node->as_boolean();

  if (b != nullptr) {
    return b->get();
  }

  error_out = "Field '" + std::string(key) + "' must be a boolean.";
  ok = false;
  return std::nullopt;
}

static int64_t get_i64_range(const toml::table& tbl, std::string_view key, int64_t default_val, int64_t min_val,
                             int64_t max_val, std::string& error_out, bool& ok) {
  ok = true;
  const auto* node = tbl.get(key);

  if (node == nullptr) {
    return default_val;
  }

  const auto* i = node->as_integer();

  if (i == nullptr) {
    error_out = "Field '" + std::string(key) + "' must be an integer.";
    ok = false;
    return default_val;
  }

  const int64_t val = i->get();

  if (val < min_val || val > max_val) {
    error_out = "Field '" + std::string(key) + "' must be in [" + std::to_string(min_val) + ", " +
                std::to_string(max_val) + "].";
    ok = false;
    return default_val;
  }

  return val;
}

static std::optional<int64_t> get_opt_i64_range(const toml::table& tbl, std::string_view key, int64_t min_val,
                                                int64_t max_val, std::string& error_out, bool& ok) {
  ok = true;
  const auto* node = tbl.get(key);

  if (node == nullptr) {
    return std::nullopt;
  }

  const auto* i = node->as_integer();

  if (i == nullptr) {
    error_out = "Field '" + std::string(key) + "' must be an integer.";
    ok = false;
    return std::nullopt;
  }

  const int64_t val = i->get();

  if (val < min_val || val > max_val) {
    error_out = "Field '" + std::string(key) + "' must be in [" + std::to_string(min_val) + ", " +
                std::to_string(max_val) + "].";
    ok = false;
    return std::nullopt;
  }

  return val;
}

static double get_f64_strict(const toml::table& tbl, std::string_view key, double default_val, double min_val,
                             std::string& error_out, bool& ok) {
  ok = true;
  const auto* node = tbl.get(key);

  if (node == nullptr) {
    return default_val;
  }

  double val = default_val;
  const auto* f = node->as_floating_point();
  const auto* i = node->as_integer();

  if (f != nullptr) {
    val = f->get();
  } else if (i != nullptr) {
    val = static_cast<double>(i->get());
  } else {
    error_out = "Field '" + std::string(key) + "' must be a number.";
    ok = false;
    return default_val;
  }

  if (!std::isfinite(val) || val < min_val) {
    error_out = "Field '" + std::string(key) + "' must be a finite number >= " + std::to_string(min_val) + ".";
    ok = false;
    return default_val;
  }

  return val;
}

static std::vector<std::string> get_str_list_strict(const toml::table& tbl, std::string_view key,
                                                    const std::unordered_map<std::string, std::string>& args,
                                                    const std::string& this_dir, const std::string& current_ns,
                                                    std::string& error_out, bool& ok) {
  ok = true;
  std::vector<std::string> out;
  const auto* node = tbl.get(key);

  if (node == nullptr) {
    return out;
  }

  const auto* arr = node->as_array();

  if (arr == nullptr) {
    error_out = "Field '" + std::string(key) + "' must be an array of strings.";
    ok = false;
    return out;
  }

  out.reserve(arr->size());

  for (const auto& elem : *arr) {
    const auto* s = elem.as_string();

    if (s == nullptr) {
      error_out = "Field '" + std::string(key) + "' elements must be strings.";
      ok = false;
      out.clear();
      return out;
    }

    out.push_back(substitute_vars(s->get(), args, this_dir, current_ns));
  }

  return out;
}

static std::vector<std::string> get_qualified_ref_list_strict(const toml::table& tbl, std::string_view key,
                                                              const std::unordered_map<std::string, std::string>& args,
                                                              const std::string& this_dir,
                                                              const std::string& current_ns, std::string& error_out,
                                                              bool& ok) {
  auto raw = get_str_list_strict(tbl, key, args, this_dir, current_ns, error_out, ok);

  if (!ok) {
    return {};
  }

  for (auto& item : raw) {
    item = qualify_name(current_ns, item);
  }

  return raw;
}

static std::vector<uint32_t> get_u32_list_strict(const toml::table& tbl, std::string_view key, std::string& error_out,
                                                 bool& ok) {
  ok = true;
  std::vector<uint32_t> out;
  const auto* node = tbl.get(key);

  if (node == nullptr) {
    return out;
  }

  const auto* arr = node->as_array();

  if (arr == nullptr) {
    error_out = "Field '" + std::string(key) + "' must be an array of non-negative integers.";
    ok = false;
    return out;
  }

  out.reserve(arr->size());

  for (const auto& elem : *arr) {
    const auto* v = elem.as_integer();

    if (v == nullptr || v->get() < 0 || v->get() > kMaxU32) {
      error_out = "Field '" + std::string(key) + "' elements must be integers in [0, 4294967295].";
      ok = false;
      out.clear();
      return out;
    }

    out.push_back(static_cast<uint32_t>(v->get()));
  }

  return out;
}

static bool parse_exit_code_list_strict(const toml::table& tbl, std::string_view key, std::vector<int32_t>& out,
                                        std::string& error_out) {
  out.clear();
  const auto* node = tbl.get(key);

  if (node == nullptr) {
    return true;
  }

  const auto* arr = node->as_array();

  if (arr == nullptr) {
    error_out = "Field '" + std::string(key) + "' must be an array of integers in [0, 255].";
    return false;
  }

  out.reserve(arr->size());

  for (const auto& elem : *arr) {
    const auto* v = elem.as_integer();

    if (v == nullptr) {
      error_out = "Field '" + std::string(key) + "' elements must be integers in [0, 255].";
      return false;
    }

    const int64_t code = v->get();

    if (code < 0 || code > 255) {
      error_out = "Field '" + std::string(key) + "' contains out-of-range exit code " + std::to_string(code) +
                  " (expected 0..255).";
      return false;
    }

    out.push_back(static_cast<int32_t>(code));
  }

  return true;
}

static bool is_valid_record_format(std::string_view fmt) noexcept {
  return ascii_iequals(fmt, "vdb") || ascii_iequals(fmt, "vdbx") || ascii_iequals(fmt, "vcap") ||
         ascii_iequals(fmt, "vcapx");
}

static bool has_valid_bag_extension(std::string_view path) noexcept {
  const size_t dot = path.rfind('.');

  if (dot == std::string_view::npos || dot + 1U >= path.size()) {
    return false;
  }

  return is_valid_record_format(path.substr(dot + 1U));
}

static bool is_valid_vlink_url(std::string_view url) noexcept {
  const size_t pos = url.find("://");
  return pos != std::string_view::npos && pos > 0U && (pos + 3U) < url.size();
}

static bool is_supported_ctrl_scheme(std::string_view scheme) noexcept {
  return scheme.empty() || ascii_iequals(scheme, "dds") || ascii_iequals(scheme, "ddsc") ||
         ascii_iequals(scheme, "ddsr") || ascii_iequals(scheme, "zenoh") || ascii_iequals(scheme, "intra");
}

static bool parse_env_table_strict(const toml::table& tbl, std::string_view owner,
                                   const std::unordered_map<std::string, std::string>& args,
                                   const std::string& this_dir, const std::string& current_ns,
                                   std::unordered_map<std::string, std::string>& out, std::string& error_out) {
  for (const auto& [k, v] : tbl) {
    const std::string key(k.str());
    const auto* s = v.as_string();
    const auto* i = v.as_integer();
    const auto* b = v.as_boolean();
    const auto* f = v.as_floating_point();

    if (s != nullptr) {
      out[key] = substitute_vars(s->get(), args, this_dir, current_ns);
    } else if (i != nullptr) {
      out[key] = std::to_string(i->get());
    } else if (b != nullptr) {
      out[key] = b->get() ? "true" : "false";
    } else if (f != nullptr) {
      char buf[64];
      const size_t n = Helpers::format_floating_spec_to(buf, sizeof(buf) - 1U, f->get(), 'g', 17, false);
      out[key].assign(buf, n);
    } else {
      error_out = std::string(owner) + " [env]." + key + " must be a string, integer, float, or boolean.";
      return false;
    }
  }

  return true;
}

static void merge_remaps(const std::vector<std::pair<std::string, std::string>>& src,
                         std::vector<std::pair<std::string, std::string>>& dst) {
  for (const auto& entry : src) {
    const std::string& from = entry.first;
    dst.erase(std::remove_if(dst.begin(), dst.end(), [&](const auto& e) { return e.first == from; }), dst.end());
    dst.emplace_back(entry.first, entry.second);
  }
}

static bool parse_remaps_table_strict(const toml::table& tbl, std::string_view owner,
                                      const std::unordered_map<std::string, std::string>& args,
                                      const std::string& this_dir, const std::string& current_ns,
                                      std::vector<std::pair<std::string, std::string>>& out, std::string& error_out) {
  struct OrderedEntry final {
    uint32_t line{0U};
    uint32_t col{0U};
    std::string from;
    std::string to;
  };

  std::vector<OrderedEntry> entries;
  entries.reserve(tbl.size());

  for (const auto& [k, v] : tbl) {
    const auto* s = v.as_string();

    if (s == nullptr) {
      error_out = std::string(owner) + " [remaps].'" + std::string(k.str()) + "' must be a string.";
      return false;
    }

    const auto src_pos = v.source().begin;
    entries.push_back(OrderedEntry{src_pos.line, src_pos.column, substitute_vars(k.str(), args, this_dir, current_ns),
                                   substitute_vars(s->get(), args, this_dir, current_ns)});
  }

  std::stable_sort(entries.begin(), entries.end(), [](const OrderedEntry& a, const OrderedEntry& b) {
    if (a.line != b.line) {
      return a.line < b.line;
    }

    return a.col < b.col;
  });

  std::vector<std::pair<std::string, std::string>> pairs;
  pairs.reserve(entries.size());

  for (auto& e : entries) {
    pairs.emplace_back(std::move(e.from), std::move(e.to));
  }

  merge_remaps(pairs, out);
  return true;
}

static bool parse_remap_list_strict(const toml::array& arr, std::string_view owner,
                                    const std::unordered_map<std::string, std::string>& args,
                                    const std::string& this_dir, const std::string& current_ns,
                                    std::vector<std::pair<std::string, std::string>>& out, std::string& error_out) {
  std::vector<std::pair<std::string, std::string>> pairs;
  pairs.reserve(arr.size());

  for (const auto& item : arr) {
    const auto* pair_arr = item.as_array();

    if (pair_arr == nullptr || pair_arr->size() != 2U) {
      error_out = std::string(owner) + " remap_list entries must be 2-element string arrays.";
      return false;
    }

    const auto* s0 = pair_arr->at(0).as_string();
    const auto* s1 = pair_arr->at(1).as_string();

    if (s0 == nullptr || s1 == nullptr) {
      error_out = std::string(owner) + " remap_list entries must be 2-element string arrays.";
      return false;
    }

    pairs.emplace_back(substitute_vars(s0->get(), args, this_dir, current_ns),
                       substitute_vars(s1->get(), args, this_dir, current_ns));
  }

  merge_remaps(pairs, out);
  return true;
}

static bool parse_url_table(const toml::table& tbl, const std::string& owner, const char* table_name,
                            const std::unordered_map<std::string, std::string>& args, const std::string& this_dir,
                            const std::string& current_ns, std::unordered_map<std::string, std::string>& out,
                            std::string& error_out) {
  for (const auto& [k, v] : tbl) {
    const auto* s = v.as_string();

    if (s == nullptr) {
      error_out = owner + " [" + std::string(table_name) + "]." + std::string(k.str()) + " must be a string URL.";
      return false;
    }

    out[std::string(k.str())] = substitute_vars(s->get(), args, this_dir, current_ns);
  }

  return true;
}

static void inject_url_vars(const std::unordered_map<std::string, std::string>& urls,
                            std::unordered_map<std::string, std::string>& args) {
  for (const auto& [k, v] : urls) {
    args["__url__." + k] = v;
  }
}

static bool flatten_toml_table(const toml::table& tbl, const std::string& prefix,
                               const std::unordered_map<std::string, std::string>& args, const std::string& this_dir,
                               const std::string& current_ns, LauncherParamMap& out, std::string& error_out,
                               size_t depth) {
  if (depth > kMaxParamTableDepth) {
    error_out = "Parameter table nesting exceeded maximum depth (" + std::to_string(kMaxParamTableDepth) + ").";
    return false;
  }

  for (const auto& [k, v] : tbl) {
    const std::string key = prefix.empty() ? std::string(k.str()) : (prefix + "." + std::string(k.str()));
    const auto* sub = v.as_table();
    const auto* b = v.as_boolean();
    const auto* i = v.as_integer();
    const auto* f = v.as_floating_point();
    const auto* s = v.as_string();
    const auto* arr = v.as_array();

    if (sub != nullptr) {
      if (!flatten_toml_table(*sub, key, args, this_dir, current_ns, out, error_out, depth + 1U)) {
        return false;
      }
    } else if (b != nullptr) {
      out[key] = b->get();
    } else if (i != nullptr) {
      out[key] = static_cast<int64_t>(i->get());
    } else if (f != nullptr) {
      out[key] = f->get();
    } else if (s != nullptr) {
      out[key] = substitute_vars(s->get(), args, this_dir, current_ns);
    } else if (arr != nullptr) {
      if (arr->empty()) {
        out[key] = std::vector<std::string>{};
      } else if (arr->at(0).is_boolean()) {
        std::vector<bool> list;
        list.reserve(arr->size());

        for (const auto& elem : *arr) {
          const auto* eb = elem.as_boolean();

          if (eb == nullptr) {
            error_out = "Parameter array '" + key + "' mixes boolean and non-boolean elements.";
            return false;
          }

          list.push_back(eb->get());
        }

        out[key] = std::move(list);
      } else if (arr->at(0).is_integer() || arr->at(0).is_floating_point()) {
        bool has_float = false;

        for (const auto& elem : *arr) {
          if (elem.is_floating_point()) {
            has_float = true;
          } else if (!elem.is_integer()) {
            error_out = "Parameter array '" + key + "' mixes numeric and non-numeric elements.";
            return false;
          }
        }

        if (has_float) {
          std::vector<double> list;
          list.reserve(arr->size());

          for (const auto& elem : *arr) {
            const auto* ef = elem.as_floating_point();
            const auto* ei = elem.as_integer();
            list.push_back(ef != nullptr ? ef->get() : static_cast<double>(ei->get()));
          }

          out[key] = std::move(list);
        } else {
          std::vector<int64_t> list;
          list.reserve(arr->size());

          for (const auto& elem : *arr) {
            list.push_back(static_cast<int64_t>(elem.as_integer()->get()));
          }

          out[key] = std::move(list);
        }
      } else if (arr->at(0).is_string()) {
        std::vector<std::string> list;
        list.reserve(arr->size());

        for (const auto& elem : *arr) {
          const auto* es = elem.as_string();

          if (es == nullptr) {
            error_out = "Parameter array '" + key + "' mixes string and non-string elements.";
            return false;
          }

          list.push_back(substitute_vars(es->get(), args, this_dir, current_ns));
        }

        out[key] = std::move(list);
      } else {
        error_out = "Parameter array '" + key + "' has unsupported element type.";
        return false;
      }
    } else {
      error_out = "Parameter '" + key + "' has unsupported TOML value type.";
      return false;
    }
  }

  return true;
}

template <typename EnumT, size_t CountT>
static bool parse_enum_ci(std::string_view s, EnumT default_val,
                          const std::pair<std::string_view, EnumT> (&table)[CountT], EnumT& out) noexcept {
  if (s.empty()) {
    out = default_val;
    return true;
  }

  for (const auto& entry : table) {
    if (ascii_iequals(s, entry.first)) {
      out = entry.second;
      return true;
    }
  }

  return false;
}

static bool parse_output_mode_strict(std::string_view s, OutputMode& out) noexcept {
  static constexpr std::pair<std::string_view, OutputMode> kTable[] = {
      {"screen", OutputMode::kScreen},
      {"log", OutputMode::kLog},
      {"both", OutputMode::kBoth},
      {"none", OutputMode::kNone},
  };
  return parse_enum_ci(s, OutputMode::kScreen, kTable, out);
}

static bool parse_on_exit_strict(std::string_view s, OnExitAction& out) noexcept {
  static constexpr std::pair<std::string_view, OnExitAction> kTable[] = {
      {"ignore", OnExitAction::kIgnore},
      {"shutdown", OnExitAction::kShutdown},
      {"restart", OnExitAction::kRestart},
  };
  return parse_enum_ci(s, OnExitAction::kIgnore, kTable, out);
}

static bool parse_restart_policy_strict(std::string_view s, RestartPolicy& out) noexcept {
  static constexpr std::pair<std::string_view, RestartPolicy> kTable[] = {
      {"no", RestartPolicy::kNo},
      {"on_failure", RestartPolicy::kOnFailure},
      {"on-failure", RestartPolicy::kOnFailure},
      {"on_abnormal", RestartPolicy::kOnAbnormal},
      {"on-abnormal", RestartPolicy::kOnAbnormal},
      {"always", RestartPolicy::kAlways},
  };
  return parse_enum_ci(s, RestartPolicy::kNo, kTable, out);
}

static bool parse_kill_mode_strict(std::string_view s, KillMode& out) noexcept {
  static constexpr std::pair<std::string_view, KillMode> kTable[] = {
      {"process", KillMode::kProcess},
      {"control-group", KillMode::kControlGroup},
      {"control_group", KillMode::kControlGroup},
      {"mixed", KillMode::kMixed},
  };
  return parse_enum_ci(s, KillMode::kProcess, kTable, out);
}

static bool parse_stop_signal_strict(std::string_view s, StopSignal& out) noexcept {
  static constexpr std::pair<std::string_view, StopSignal> kTable[] = {
      {"sigterm", StopSignal::kSigTerm}, {"term", StopSignal::kSigTerm},  {"sigint", StopSignal::kSigInt},
      {"int", StopSignal::kSigInt},      {"sighup", StopSignal::kSigHup}, {"hup", StopSignal::kSigHup},
      {"sigkill", StopSignal::kSigKill}, {"kill", StopSignal::kSigKill},
  };
  return parse_enum_ci(s, StopSignal::kSigTerm, kTable, out);
}

static bool parse_reload_mode_strict(std::string_view s, ReloadMode& out) noexcept {
  static constexpr std::pair<std::string_view, ReloadMode> kTable[] = {
      {"none", ReloadMode::kNone},
      {"sighup", ReloadMode::kSigHup},
      {"component", ReloadMode::kComponent},
  };
  return parse_enum_ci(s, ReloadMode::kNone, kTable, out);
}

static bool parse_probe_type_strict(std::string_view s, ReadyProbeType& out) noexcept {
  static constexpr std::pair<std::string_view, ReadyProbeType> kTable[] = {
      {"none", ReadyProbeType::kNone},     {"component", ReadyProbeType::kComponent}, {"delay", ReadyProbeType::kDelay},
      {"stdout", ReadyProbeType::kStdout}, {"file", ReadyProbeType::kFile},           {"topic", ReadyProbeType::kTopic},
  };
  return parse_enum_ci(s, ReadyProbeType::kNone, kTable, out);
}

static bool parse_scheduler_mode_strict(std::string_view s, SchedulerMode& out) noexcept {
  static constexpr std::pair<std::string_view, SchedulerMode> kTable[] = {
      {"shared", SchedulerMode::kShared},   {"dedicated", SchedulerMode::kDedicated},
      {"multi", SchedulerMode::kMulti},     {"pinned", SchedulerMode::kChoreography},
      {"pool", SchedulerMode::kThreadPool},
  };
  return parse_enum_ci(s, SchedulerMode::kShared, kTable, out);
}

static bool parse_queue_type_strict(std::string_view s, MessageLoop::Type& out) noexcept {
  static constexpr std::pair<std::string_view, MessageLoop::Type> kTable[] = {
      {"normal", MessageLoop::kNormalType},
      {"lockfree", MessageLoop::kLockfreeType},
      {"priority", MessageLoop::kPriorityType},
  };
  return parse_enum_ci(s, MessageLoop::kNormalType, kTable, out);
}

static bool parse_strategy_strict(std::string_view s, MessageLoop::Strategy& out) noexcept {
  static constexpr std::pair<std::string_view, MessageLoop::Strategy> kTable[] = {
      {"optimization", MessageLoop::kOptimizationStrategy},
      {"pop", MessageLoop::kPopStrategy},
      {"block", MessageLoop::kBlockStrategy},
  };
  return parse_enum_ci(s, MessageLoop::kOptimizationStrategy, kTable, out);
}

static bool parse_sched_policy_strict(std::string_view s, SchedPolicy& out) noexcept {
  static constexpr std::pair<std::string_view, SchedPolicy> kTable[] = {
      {"default", SchedPolicy::kDefault},
      {"other", SchedPolicy::kOther},
      {"fifo", SchedPolicy::kFifo},
      {"rr", SchedPolicy::kRr},
  };
  return parse_enum_ci(s, SchedPolicy::kDefault, kTable, out);
}

static bool load_params_files(const toml::table& tbl, const std::unordered_map<std::string, std::string>& args,
                              const std::string& this_dir, const std::string& current_ns, LauncherParamMap& out,
                              std::string& error_out) {
  bool ok = true;
  std::vector<std::string> files;
  const std::string single = get_str_strict(tbl, "params_file", args, this_dir, current_ns, "", error_out, ok);

  if (!ok) {
    return false;
  }

  if (!single.empty()) {
    files.push_back(single);
  }

  auto extra_files = get_str_list_strict(tbl, "params_files", args, this_dir, current_ns, error_out, ok);

  if (!ok) {
    return false;
  }

  for (auto& f : extra_files) {
    if (!f.empty()) {
      files.push_back(std::move(f));
    }
  }

  for (const auto& raw_path : files) {
    std::filesystem::path p(raw_path);

    if (p.is_relative()) {
      p = std::filesystem::path(this_dir) / p;
    }

    try {
      const auto parsed = toml::parse_file(Helpers::path_to_string(p));

      if (!flatten_toml_table(parsed, "", args, this_dir, current_ns, out, error_out, 0U)) {
        return false;
      }
    } catch (const toml::parse_error& err) {
      error_out = "Failed to parse params file '" + Helpers::path_to_string(p) + "': " + std::string(err.description());
      return false;
    } catch (const std::exception& err) {
      error_out = "Failed to load params file '" + Helpers::path_to_string(p) + "': " + err.what();
      return false;
    }
  }

  return true;
}

static bool parse_unit_endpoints(const toml::table& tbl, const std::string& owner, const std::string& unit_name,
                                 const std::string& task_group, const std::vector<GroupConfig>& groups,
                                 const std::string& this_dir, const std::string& current_ns,
                                 std::unordered_map<std::string, std::string>& unit_args,
                                 std::unordered_map<std::string, std::string>& urls,
                                 std::unordered_map<std::string, std::string>& inputs,
                                 std::unordered_map<std::string, std::string>& outputs, std::string& error_out) {
  std::string eff_group = task_group;

  if (eff_group.empty() && !unit_name.empty()) {
    for (const auto& g : groups) {
      if (std::find(g.members.begin(), g.members.end(), unit_name) != g.members.end()) {
        eff_group = g.name;
        break;
      }
    }
  }

  if (!eff_group.empty()) {
    for (const auto& g : groups) {
      if (g.name == eff_group) {
        inject_url_vars(g.urls, unit_args);
        break;
      }
    }
  }

  const toml::table* urls_tbl = nullptr;

  if (!require_table_if_present(tbl, "urls", owner, urls_tbl, error_out)) {
    return false;
  }

  if (urls_tbl != nullptr &&
      !parse_url_table(*urls_tbl, owner, "urls", unit_args, this_dir, current_ns, urls, error_out)) {
    return false;
  }

  const toml::table* inputs_tbl = nullptr;

  if (!require_table_if_present(tbl, "inputs", owner, inputs_tbl, error_out)) {
    return false;
  }

  if (inputs_tbl != nullptr &&
      !parse_url_table(*inputs_tbl, owner, "inputs", unit_args, this_dir, current_ns, inputs, error_out)) {
    return false;
  }

  const toml::table* outputs_tbl = nullptr;

  if (!require_table_if_present(tbl, "outputs", owner, outputs_tbl, error_out)) {
    return false;
  }

  if (outputs_tbl != nullptr &&
      !parse_url_table(*outputs_tbl, owner, "outputs", unit_args, this_dir, current_ns, outputs, error_out)) {
    return false;
  }

  inject_url_vars(inputs, unit_args);
  inject_url_vars(outputs, unit_args);
  inject_url_vars(urls, unit_args);
  return true;
}

static bool parse_ready_probe(const toml::table& unit_tbl, const std::unordered_map<std::string, std::string>& args,
                              const std::string& this_dir, const std::string& current_ns, ReadyProbeConfig& out,
                              std::string& err_out) {
  const toml::table* probe = nullptr;

  if (!require_table_if_present(unit_tbl, "ready", "unit", probe, err_out)) {
    return false;
  }

  if (probe == nullptr) {
    return true;
  }

  if (!validate_allowed_keys(*probe, "[ready]", {"type", "pattern", "path", "topic", "delay_ms", "timeout_ms"},
                             err_out)) {
    return false;
  }

  bool ok = true;
  const std::string type_str = get_str_strict(*probe, "type", args, this_dir, current_ns, "", err_out, ok);

  if (!ok) {
    return false;
  }

  if (!parse_probe_type_strict(type_str, out.type)) {
    err_out = "Unknown ready probe type '" + type_str + "'.";
    return false;
  }

  out.pattern = get_str_strict(*probe, "pattern", args, this_dir, current_ns, "", err_out, ok);

  if (!ok) {
    return false;
  }

  out.path = get_str_strict(*probe, "path", args, this_dir, current_ns, "", err_out, ok);

  if (!ok) {
    return false;
  }

  out.topic = get_str_strict(*probe, "topic", args, this_dir, current_ns, "", err_out, ok);

  if (!ok) {
    return false;
  }

  const int64_t dv = get_i64_range(*probe, "delay_ms", 0, 0, kMaxU32, err_out, ok);

  if (!ok) {
    return false;
  }

  out.delay_ms = static_cast<uint32_t>(dv);
  const int64_t tv = get_i64_range(*probe, "timeout_ms", 10000, 0, kMaxU32, err_out, ok);

  if (!ok) {
    return false;
  }

  out.timeout_ms = static_cast<uint32_t>(std::max<int64_t>(100, tv));
  return true;
}

static bool parse_liveness_probe(const toml::table& unit_tbl, const std::unordered_map<std::string, std::string>& args,
                                 const std::string& this_dir, const std::string& current_ns, LivenessProbeConfig& out,
                                 std::string& err_out) {
  const toml::table* probe = nullptr;

  if (!require_table_if_present(unit_tbl, "liveness", "unit", probe, err_out)) {
    return false;
  }

  if (probe == nullptr) {
    return true;
  }

  if (!validate_allowed_keys(
          *probe, "[liveness]",
          {"enabled", "type", "path", "topic", "interval_ms", "failure_threshold", "restart_on_failure"}, err_out)) {
    return false;
  }

  bool ok = true;
  const bool explicit_enabled = get_bool_strict(*probe, "enabled", true, err_out, ok);

  if (!ok) {
    return false;
  }

  const std::string type_str = get_str_strict(*probe, "type", args, this_dir, current_ns, "", err_out, ok);

  if (!ok) {
    return false;
  }

  if (!parse_probe_type_strict(type_str, out.type) ||
      (out.type != ReadyProbeType::kFile && out.type != ReadyProbeType::kTopic && out.type != ReadyProbeType::kNone)) {
    err_out = "Invalid liveness probe type '" + type_str + "' (expected file|topic).";
    return false;
  }

  out.enabled = explicit_enabled && (out.type != ReadyProbeType::kNone);
  out.path = get_str_strict(*probe, "path", args, this_dir, current_ns, "", err_out, ok);

  if (!ok) {
    return false;
  }

  out.topic = get_str_strict(*probe, "topic", args, this_dir, current_ns, "", err_out, ok);

  if (!ok) {
    return false;
  }

  const int64_t iv = get_i64_range(*probe, "interval_ms", 5000, 0, kMaxU32, err_out, ok);

  if (!ok) {
    return false;
  }

  out.interval_ms = static_cast<uint32_t>(std::max<int64_t>(100, iv));
  const int64_t ft = get_i64_range(*probe, "failure_threshold", 3, 1, kMaxU32, err_out, ok);

  if (!ok) {
    return false;
  }

  out.failure_threshold = static_cast<uint32_t>(ft);
  out.restart_on_failure = get_bool_strict(*probe, "restart_on_failure", true, err_out, ok);
  return ok;
}

static bool parse_resources_table(const toml::table& res, bool is_slice,
                                  const std::unordered_map<std::string, std::string>& args, const std::string& this_dir,
                                  const std::string& current_ns, ResourceLimits& out, std::string& err_out) {
  if (is_slice) {
    if (!validate_allowed_keys(res, "[[slice]]",
                               {"name", "strict", "cpus", "cpu_quota_percent", "cpu_weight", "mem_max_mb",
                                "mem_high_mb", "pids_max", "oom_adj", "nice", "nofile", "vmem_max_mb", "core_max_mb"},
                               err_out)) {
      return false;
    }
  } else {
    if (!validate_allowed_keys(res, "[resources]",
                               {"slice", "strict", "cpus", "cpu_quota_percent", "cpu_weight", "mem_max_mb",
                                "mem_high_mb", "pids_max", "oom_adj", "nice", "nofile", "vmem_max_mb", "core_max_mb"},
                               err_out)) {
      return false;
    }
  }

  bool ok = true;

  if (!is_slice) {
    out.slice = get_str_strict(res, "slice", args, this_dir, current_ns, "", err_out, ok);

    if (!ok) {
      return false;
    }
  }

  out.strict = get_bool_strict(res, "strict", false, err_out, ok);

  if (!ok) {
    return false;
  }

  out.cpu_affinity = get_u32_list_strict(res, "cpus", err_out, ok);

  if (!ok) {
    return false;
  }

  const auto quota = get_opt_i64_range(res, "cpu_quota_percent", 1, kMaxU32, err_out, ok);

  if (!ok) {
    return false;
  }

  if (quota.has_value()) {
    out.cpu_quota_percent = static_cast<uint32_t>(*quota);
  }

  const auto weight = get_opt_i64_range(res, "cpu_weight", 1, 10000, err_out, ok);

  if (!ok) {
    return false;
  }

  if (weight.has_value()) {
    out.cpu_weight = static_cast<uint32_t>(*weight);
  }

  const auto mem_max = get_opt_i64_range(res, "mem_max_mb", 1, kMaxMiB, err_out, ok);

  if (!ok) {
    return false;
  }

  if (mem_max.has_value()) {
    out.memory_max_mb = static_cast<uint64_t>(*mem_max);
  }

  const auto mem_high = get_opt_i64_range(res, "mem_high_mb", 1, kMaxMiB, err_out, ok);

  if (!ok) {
    return false;
  }

  if (mem_high.has_value()) {
    out.memory_high_mb = static_cast<uint64_t>(*mem_high);
  }

  if (out.memory_high_mb.has_value() && out.memory_max_mb.has_value() && *out.memory_high_mb > *out.memory_max_mb) {
    err_out = "resources.mem_high_mb cannot exceed resources.mem_max_mb.";
    return false;
  }

  const auto pids = get_opt_i64_range(res, "pids_max", 1, kMaxU32, err_out, ok);

  if (!ok) {
    return false;
  }

  if (pids.has_value()) {
    out.pids_max = static_cast<uint32_t>(*pids);
  }

  const auto oom = get_opt_i64_range(res, "oom_adj", -1000, 1000, err_out, ok);

  if (!ok) {
    return false;
  }

  if (oom.has_value()) {
    out.oom_score_adj = static_cast<int32_t>(*oom);
  }

  const auto nice_v = get_opt_i64_range(res, "nice", -20, 19, err_out, ok);

  if (!ok) {
    return false;
  }

  if (nice_v.has_value()) {
    out.nice = static_cast<int32_t>(*nice_v);
  }

  const auto nofile = get_opt_i64_range(res, "nofile", 0, std::numeric_limits<int64_t>::max(), err_out, ok);

  if (!ok) {
    return false;
  }

  if (nofile.has_value()) {
    out.rlimit_nofile = static_cast<uint64_t>(*nofile);
  }

  const auto vmem = get_opt_i64_range(res, "vmem_max_mb", 0, kMaxMiB, err_out, ok);

  if (!ok) {
    return false;
  }

  if (vmem.has_value()) {
    out.rlimit_as_mb = static_cast<uint64_t>(*vmem);
  }

  const auto core = get_opt_i64_range(res, "core_max_mb", 0, kMaxMiB, err_out, ok);

  if (!ok) {
    return false;
  }

  if (core.has_value()) {
    out.rlimit_core_mb = static_cast<uint64_t>(*core);
  }

  return true;
}

static bool parse_resources(const toml::table& tbl, const std::unordered_map<std::string, std::string>& args,
                            const std::string& this_dir, const std::string& current_ns, ResourceLimits& out,
                            std::string& err_out) {
  const toml::table* res = nullptr;

  if (!require_table_if_present(tbl, "resources", "unit", res, err_out)) {
    return false;
  }

  if (res == nullptr) {
    return true;
  }

  return parse_resources_table(*res, false, args, this_dir, current_ns, out, err_out);
}

static bool parse_respawn(const toml::table& tbl, const std::unordered_map<std::string, std::string>& args,
                          const std::string& this_dir, const std::string& current_ns, RespawnConfig& out,
                          std::string& err_out) {
  const auto* policy_node = tbl.get("restart");

  if (policy_node != nullptr) {
    const auto* b = policy_node->as_boolean();
    const auto* s = policy_node->as_string();

    if (b != nullptr) {
      out.enabled = b->get();
      out.policy = b->get() ? RestartPolicy::kOnFailure : RestartPolicy::kNo;
    } else if (s != nullptr) {
      const std::string policy_str = substitute_vars(s->get(), args, this_dir, current_ns);

      if (!parse_restart_policy_strict(policy_str, out.policy)) {
        err_out = "Unknown restart policy '" + policy_str + "'.";
        return false;
      }

      out.enabled = (out.policy != RestartPolicy::kNo);
    } else {
      err_out = "Field 'restart' must be a policy string (no|on_failure|on_abnormal|always) or boolean.";
      return false;
    }
  }

  bool ok = true;
  const int64_t d_ms = get_i64_range(tbl, "restart_delay_ms", 1000, 0, kMaxU32, err_out, ok);

  if (!ok) {
    return false;
  }

  out.delay_ms = static_cast<uint32_t>(d_ms);
  const int64_t max_ms = get_i64_range(tbl, "restart_max_ms", 30000, 0, kMaxU32, err_out, ok);

  if (!ok) {
    return false;
  }

  out.max_delay_ms = static_cast<uint32_t>(std::max<int64_t>(out.delay_ms, max_ms));
  const double backoff = get_f64_strict(tbl, "restart_backoff", 1.5, 1.0, err_out, ok);

  if (!ok) {
    return false;
  }

  out.backoff = backoff;
  const int64_t burst = get_i64_range(tbl, "start_burst", 5, 0, kMaxU32, err_out, ok);

  if (!ok) {
    return false;
  }

  out.start_limit_burst = static_cast<uint32_t>(burst);
  const int64_t win = get_i64_range(tbl, "start_window_ms", 60000, 0, kMaxU32, err_out, ok);

  if (!ok) {
    return false;
  }

  out.start_limit_interval_ms = static_cast<uint32_t>(win);

  if (!parse_exit_code_list_strict(tbl, "ok_codes", out.success_exit_codes, err_out)) {
    return false;
  }

  if (!parse_exit_code_list_strict(tbl, "no_restart_codes", out.restart_prevent_exit_codes, err_out)) {
    return false;
  }

  return true;
}

static bool parse_deps_strict(const toml::table& tbl, const std::unordered_map<std::string, std::string>& args,
                              const std::string& this_dir, const std::string& current_ns, DependencySpec& out,
                              std::string& err_out) {
  bool ok = true;
  out.depends_on = get_qualified_ref_list_strict(tbl, "depends_on", args, this_dir, current_ns, err_out, ok);

  if (!ok) {
    return false;
  }

  out.after = get_qualified_ref_list_strict(tbl, "after", args, this_dir, current_ns, err_out, ok);

  if (!ok) {
    return false;
  }

  out.before = get_qualified_ref_list_strict(tbl, "before", args, this_dir, current_ns, err_out, ok);

  if (!ok) {
    return false;
  }

  out.requires_units = get_qualified_ref_list_strict(tbl, "requires", args, this_dir, current_ns, err_out, ok);

  if (!ok) {
    return false;
  }

  out.wants = get_qualified_ref_list_strict(tbl, "wants", args, this_dir, current_ns, err_out, ok);

  if (!ok) {
    return false;
  }

  out.binds_to = get_qualified_ref_list_strict(tbl, "binds_to", args, this_dir, current_ns, err_out, ok);

  if (!ok) {
    return false;
  }

  out.part_of = get_qualified_ref_list_strict(tbl, "part_of", args, this_dir, current_ns, err_out, ok);

  if (!ok) {
    return false;
  }

  out.conflicts = get_qualified_ref_list_strict(tbl, "conflicts", args, this_dir, current_ns, err_out, ok);
  return ok;
}

static bool parse_unit_triggers_strict(const toml::table& tbl, const std::unordered_map<std::string, std::string>& args,
                                       const std::string& this_dir, const std::string& current_ns,
                                       std::vector<std::string>& on_start, std::vector<std::string>& on_ready,
                                       std::vector<std::string>& on_exit, std::vector<std::string>& on_error,
                                       std::string& err_out) {
  bool ok = true;
  on_start = get_qualified_ref_list_strict(tbl, "on_start", args, this_dir, current_ns, err_out, ok);

  if (!ok) {
    return false;
  }

  on_ready = get_qualified_ref_list_strict(tbl, "on_ready", args, this_dir, current_ns, err_out, ok);

  if (!ok) {
    return false;
  }

  on_exit = get_qualified_ref_list_strict(tbl, "on_exit_trigger", args, this_dir, current_ns, err_out, ok);

  if (!ok) {
    return false;
  }

  on_error = get_qualified_ref_list_strict(tbl, "on_error", args, this_dir, current_ns, err_out, ok);
  return ok;
}

static bool parse_launcher_root_section(const toml::table& launcher_tbl,
                                        const std::unordered_map<std::string, std::string>& local_args,
                                        const std::string& this_dir, const std::string& current_ns, LaunchManifest& out,
                                        std::string& error_out) {
  if (!validate_allowed_keys(
          launcher_tbl, "[launcher]",
          {"name", "namespace", "startup", "max_parallel", "target", "stop_timeout_ms", "ctrl_scheme", "ctrl_namespace",
           "ctrl_domain", "ctrl", "key", "bind_ip", "native", "remote_load", "plugin_dirs"},
          error_out)) {
    return false;
  }

  bool ok = true;
  out.name = get_str_strict(launcher_tbl, "name", local_args, this_dir, current_ns, "vlink-launcher", error_out, ok);

  if (!ok) {
    return false;
  }

  out.ns = current_ns;
  const std::string smode =
      get_str_strict(launcher_tbl, "startup", local_args, this_dir, current_ns, "parallel", error_out, ok);

  if (!ok) {
    return false;
  }

  const int64_t mp = get_i64_range(launcher_tbl, "max_parallel", 0, 0, kMaxU32, error_out, ok);

  if (!ok) {
    return false;
  }

  if (ascii_iequals(smode, "parallel")) {
    out.startup_mode = StartupMode::kParallel;
    out.max_parallel = static_cast<uint32_t>(mp);
  } else if (ascii_iequals(smode, "serial")) {
    out.startup_mode = StartupMode::kSerial;
    out.max_parallel = 1U;
  } else if (ascii_iequals(smode, "max_parallel")) {
    out.startup_mode = StartupMode::kMaxParallel;

    if (mp < 1) {
      error_out = "[launcher].startup = \"max_parallel\" requires max_parallel >= 1.";
      return false;
    }

    out.max_parallel = static_cast<uint32_t>(mp);
  } else {
    error_out = "Unknown [launcher].startup '" + smode + "' (expected parallel|serial|max_parallel).";
    return false;
  }

  const std::string raw_target =
      get_str_strict(launcher_tbl, "target", local_args, this_dir, current_ns, "", error_out, ok);

  if (!ok) {
    return false;
  }

  out.default_target = qualify_name(current_ns, raw_target);
  const int64_t stop_to = get_i64_range(launcher_tbl, "stop_timeout_ms", 5000, 0, kMaxU32, error_out, ok);

  if (!ok) {
    return false;
  }

  out.shutdown_timeout_ms = static_cast<uint32_t>(std::max<int64_t>(200, stop_to));
  out.control_scheme =
      to_lower_copy(get_str_strict(launcher_tbl, "ctrl_scheme", local_args, this_dir, current_ns, "", error_out, ok));

  if (!ok) {
    return false;
  }

  out.control_namespace =
      get_str_strict(launcher_tbl, "ctrl_namespace", local_args, this_dir, current_ns, "launcher", error_out, ok);

  if (!ok) {
    return false;
  }

  const int64_t ctrl_dom = get_i64_range(launcher_tbl, "ctrl_domain", 0, 0, kMaxU32, error_out, ok);

  if (!ok) {
    return false;
  }

  out.control_domain = static_cast<uint32_t>(ctrl_dom);
  out.control_enabled = get_bool_strict(launcher_tbl, "ctrl", true, error_out, ok);

  if (!ok) {
    return false;
  }

  out.security_key = get_str_strict(launcher_tbl, "key", local_args, this_dir, current_ns, "", error_out, ok);

  if (!ok) {
    return false;
  }

  out.bind_ip = get_str_strict(launcher_tbl, "bind_ip", local_args, this_dir, current_ns, "", error_out, ok);

  if (!ok) {
    return false;
  }

  out.native_mode = get_bool_strict(launcher_tbl, "native", false, error_out, ok);

  if (!ok) {
    return false;
  }

  out.allow_remote_load = get_bool_strict(launcher_tbl, "remote_load", false, error_out, ok);
  return ok;
}

static bool parse_logger_and_record_sections(const toml::table& root,
                                             const std::unordered_map<std::string, std::string>& local_args,
                                             const std::string& this_dir, const std::string& current_ns,
                                             LaunchManifest& out, std::string& error_out) {
  const toml::table* log_tbl = nullptr;

  if (!require_table_if_present(root, "logger", "manifest root", log_tbl, error_out)) {
    return false;
  }

  if (log_tbl != nullptr) {
    if (!validate_allowed_keys(
            *log_tbl, "[logger]",
            {"dir", "console_level", "file_level", "max_size_mb", "max_files", "flush_ms", "history", "fixed_name",
             "append", "utc", "console_fmt", "capture_file", "strip_ansi", "plugin", "plugin_strict", "plugin_capture"},
            error_out)) {
      return false;
    }

    bool ok = true;
    out.logger.dir = get_str_strict(*log_tbl, "dir", local_args, this_dir, current_ns, "", error_out, ok);

    if (!ok) {
      return false;
    }

    if (log_tbl->contains("console_level")) {
      const std::string lv =
          get_str_strict(*log_tbl, "console_level", local_args, this_dir, current_ns, "", error_out, ok);

      if (!ok) {
        return false;
      }

      bool lv_ok = false;
      out.logger.console_level = parse_log_level_strict(lv, lv_ok);

      if (!lv_ok) {
        error_out = "Unknown [logger].console_level '" + lv + "'.";
        return false;
      }
    }

    if (log_tbl->contains("file_level")) {
      const std::string lv =
          get_str_strict(*log_tbl, "file_level", local_args, this_dir, current_ns, "", error_out, ok);

      if (!ok) {
        return false;
      }

      bool lv_ok = false;
      out.logger.file_level = parse_log_level_strict(lv, lv_ok);

      if (!lv_ok) {
        error_out = "Unknown [logger].file_level '" + lv + "'.";
        return false;
      }
    }

    const int64_t max_mb = get_i64_range(*log_tbl, "max_size_mb", 10, 1, 1024 * 1024, error_out, ok);

    if (!ok) {
      return false;
    }

    out.logger.max_file_size = static_cast<size_t>(max_mb) * 1024U * 1024U;
    const int64_t max_files = get_i64_range(*log_tbl, "max_files", 10, 1, kMaxU32, error_out, ok);

    if (!ok) {
      return false;
    }

    out.logger.max_files = static_cast<size_t>(max_files);
    const int64_t flush_ms = get_i64_range(*log_tbl, "flush_ms", 500, 10, kMaxU32, error_out, ok);

    if (!ok) {
      return false;
    }

    out.logger.flush_delay_ms = static_cast<uint32_t>(flush_ms);
    const int64_t hist = get_i64_range(*log_tbl, "history", 4096, 0, kMaxU32, error_out, ok);

    if (!ok) {
      return false;
    }

    out.logger.history_max_records = static_cast<size_t>(hist);
    out.logger.fixed_filename = get_bool_strict(*log_tbl, "fixed_name", false, error_out, ok);

    if (!ok) {
      return false;
    }

    out.logger.append = get_bool_strict(*log_tbl, "append", false, error_out, ok);

    if (!ok) {
      return false;
    }

    out.logger.utc = get_bool_strict(*log_tbl, "utc", false, error_out, ok);

    if (!ok) {
      return false;
    }

    out.logger.console_fmt = get_bool_strict(*log_tbl, "console_fmt", false, error_out, ok);

    if (!ok) {
      return false;
    }

    out.logger.capture_to_file = get_bool_strict(*log_tbl, "capture_file", true, error_out, ok);

    if (!ok) {
      return false;
    }

    out.logger.strip_ansi = get_bool_strict(*log_tbl, "strip_ansi", true, error_out, ok);

    if (!ok) {
      return false;
    }

    out.logger.plugin = get_str_strict(*log_tbl, "plugin", local_args, this_dir, current_ns, "", error_out, ok);

    if (!ok) {
      return false;
    }

    out.logger.plugin_strict = get_bool_strict(*log_tbl, "plugin_strict", false, error_out, ok);

    if (!ok) {
      return false;
    }

    out.logger.plugin_capture = get_bool_strict(*log_tbl, "plugin_capture", false, error_out, ok);

    if (!ok) {
      return false;
    }
  }

  const toml::table* rec_tbl = nullptr;

  if (!require_table_if_present(root, "record", "manifest root", rec_tbl, error_out)) {
    return false;
  }

  if (rec_tbl != nullptr) {
    if (!validate_allowed_keys(*rec_tbl, "[record]", {"enabled", "dir", "format", "tag", "force"}, error_out)) {
      return false;
    }

    bool ok = true;
    out.record.enabled = get_bool_strict(*rec_tbl, "enabled", false, error_out, ok);

    if (!ok) {
      return false;
    }

    out.record.dir = get_str_strict(*rec_tbl, "dir", local_args, this_dir, current_ns, "", error_out, ok);

    if (!ok) {
      return false;
    }

    const std::string fmt_raw =
        get_str_strict(*rec_tbl, "format", local_args, this_dir, current_ns, "vcap", error_out, ok);

    if (!ok) {
      return false;
    }

    out.record.format = to_lower_copy(fmt_raw);
    out.record.tag = get_str_strict(*rec_tbl, "tag", local_args, this_dir, current_ns, "", error_out, ok);

    if (!ok) {
      return false;
    }

    out.record.force_publish = get_bool_strict(*rec_tbl, "force", false, error_out, ok);

    if (!ok) {
      return false;
    }
  }

  return true;
}

static bool parse_proxy_section(const toml::table& proxy_tbl,
                                const std::unordered_map<std::string, std::string>& local_args,
                                const std::string& this_dir, const std::string& current_ns, ProxyConfig& proxy,
                                std::string& error_out) {
  if (!validate_allowed_keys(proxy_tbl, "[proxy]",
                             {"enabled",
                              "name",
                              "cmd",
                              "dds",
                              "domain",
                              "iox",
                              "iox_strategy",
                              "iox_config",
                              "config",
                              "extra_args",
                              "stage",
                              "group",
                              "condition",
                              "required",
                              "autostart",
                              "ready_delay_ms",
                              "stop_timeout_ms",
                              "output",
                              "kill_mode",
                              "stop_signal",
                              "reload",
                              "depends_on",
                              "after",
                              "before",
                              "requires",
                              "wants",
                              "binds_to",
                              "part_of",
                              "conflicts",
                              "restart",
                              "restart_delay_ms",
                              "restart_max_ms",
                              "restart_backoff",
                              "start_burst",
                              "start_window_ms",
                              "ok_codes",
                              "no_restart_codes",
                              "resources",
                              "env"},
                             error_out)) {
    return false;
  }

  bool ok = true;
  proxy.enabled = get_bool_strict(proxy_tbl, "enabled", false, error_out, ok);

  if (!ok) {
    return false;
  }

  proxy.condition = parse_condition_node(proxy_tbl, "condition", local_args, this_dir, current_ns, error_out, ok, true);

  if (!ok) {
    error_out.insert(0U, "[proxy]: ");
    return false;
  }

  const std::string raw_name =
      get_str_strict(proxy_tbl, "name", local_args, this_dir, current_ns, "vlink_proxy", error_out, ok);

  if (!ok) {
    return false;
  }

  proxy.name = qualify_name(current_ns, raw_name);
  proxy.program = get_str_strict(proxy_tbl, "cmd", local_args, this_dir, current_ns, "vlink-proxy", error_out, ok);

  if (!ok) {
    return false;
  }

  proxy.dds_impl = get_str_strict(proxy_tbl, "dds", local_args, this_dir, current_ns, "", error_out, ok);

  if (!ok) {
    return false;
  }

  const int64_t dom = get_i64_range(proxy_tbl, "domain", 0, 0, kMaxU32, error_out, ok);

  if (!ok) {
    return false;
  }

  proxy.domain_id = static_cast<uint32_t>(dom);
  proxy.use_iox = get_bool_strict(proxy_tbl, "iox", false, error_out, ok);

  if (!ok) {
    return false;
  }

  const int64_t iox_strat = get_i64_range(proxy_tbl, "iox_strategy", 0, 0, kMaxI32, error_out, ok);

  if (!ok) {
    return false;
  }

  proxy.iox_strategy = static_cast<int32_t>(iox_strat);
  const std::string raw_iox_cfg =
      get_str_strict(proxy_tbl, "iox_config", local_args, this_dir, current_ns, "", error_out, ok);

  if (!ok) {
    return false;
  }

  proxy.iox_config = resolve_relative_path(raw_iox_cfg, this_dir);
  const std::string raw_cfg = get_str_strict(proxy_tbl, "config", local_args, this_dir, current_ns, "", error_out, ok);

  if (!ok) {
    return false;
  }

  proxy.config_file = resolve_relative_path(raw_cfg, this_dir);
  proxy.extra_args = get_str_list_strict(proxy_tbl, "extra_args", local_args, this_dir, current_ns, error_out, ok);

  if (!ok) {
    return false;
  }

  const int64_t stage_v = get_i64_range(proxy_tbl, "stage", -100, kMinI32, kMaxI32, error_out, ok);

  if (!ok) {
    return false;
  }

  proxy.stage = static_cast<int32_t>(stage_v);
  const std::string raw_grp = get_str_strict(proxy_tbl, "group", local_args, this_dir, current_ns, "", error_out, ok);

  if (!ok) {
    return false;
  }

  proxy.task_group = qualify_name(current_ns, raw_grp);
  proxy.required = get_bool_strict(proxy_tbl, "required", false, error_out, ok);

  if (!ok) {
    return false;
  }

  proxy.autostart = get_bool_strict(proxy_tbl, "autostart", true, error_out, ok);

  if (!ok) {
    return false;
  }

  const int64_t rdy_ms = get_i64_range(proxy_tbl, "ready_delay_ms", 300, 0, kMaxU32, error_out, ok);

  if (!ok) {
    return false;
  }

  proxy.ready_delay_ms = static_cast<uint32_t>(rdy_ms);
  const int64_t stop_to = get_i64_range(proxy_tbl, "stop_timeout_ms", 5000, 0, kMaxU32, error_out, ok);

  if (!ok) {
    return false;
  }

  proxy.sigterm_timeout_ms = static_cast<uint32_t>(std::max<int64_t>(200, stop_to));
  const std::string out_str =
      get_str_strict(proxy_tbl, "output", local_args, this_dir, current_ns, "log", error_out, ok);

  if (!ok || !parse_output_mode_strict(out_str, proxy.output)) {
    if (ok) {
      error_out = "Unknown [proxy].output mode '" + out_str + "'.";
    }

    return false;
  }

  const std::string km_str =
      get_str_strict(proxy_tbl, "kill_mode", local_args, this_dir, current_ns, "", error_out, ok);

  if (!ok || !parse_kill_mode_strict(km_str, proxy.kill_mode)) {
    if (ok) {
      error_out = "Unknown [proxy].kill_mode '" + km_str + "'.";
    }

    return false;
  }

  const std::string sig_str =
      get_str_strict(proxy_tbl, "stop_signal", local_args, this_dir, current_ns, "", error_out, ok);

  if (!ok || !parse_stop_signal_strict(sig_str, proxy.stop_signal)) {
    if (ok) {
      error_out = "Unknown [proxy].stop_signal '" + sig_str + "'.";
    }

    return false;
  }

  const std::string rel_str = get_str_strict(proxy_tbl, "reload", local_args, this_dir, current_ns, "", error_out, ok);

  if (!ok || !parse_reload_mode_strict(rel_str, proxy.reload_mode)) {
    if (ok) {
      error_out = "Unknown [proxy].reload mode '" + rel_str + "'.";
    }

    return false;
  }

  if (!parse_deps_strict(proxy_tbl, local_args, this_dir, current_ns, proxy.deps, error_out)) {
    error_out.insert(0U, "[proxy]: ");
    return false;
  }

  if (!parse_respawn(proxy_tbl, local_args, this_dir, current_ns, proxy.respawn, error_out)) {
    error_out.insert(0U, "[proxy]: ");
    return false;
  }

  if (!parse_resources(proxy_tbl, local_args, this_dir, current_ns, proxy.resources, error_out)) {
    error_out.insert(0U, "[proxy]: ");
    return false;
  }

  const toml::table* proxy_env_tbl = nullptr;

  if (!require_table_if_present(proxy_tbl, "env", "[proxy]", proxy_env_tbl, error_out)) {
    return false;
  }

  if (proxy_env_tbl != nullptr &&
      !parse_env_table_strict(*proxy_env_tbl, "[proxy]", local_args, this_dir, current_ns, proxy.env, error_out)) {
    return false;
  }

  return true;
}

static bool parse_slices_groups_schedulers(const toml::table& root,
                                           const std::unordered_map<std::string, std::string>& local_args,
                                           const std::string& this_dir, const std::string& current_ns,
                                           LaunchManifest& out, std::string& error_out) {
  const auto* slices_arr = root["slice"].as_array();

  if (slices_arr != nullptr) {
    for (const auto& item : *slices_arr) {
      const auto* tbl = item.as_table();

      if (tbl == nullptr) {
        error_out = "[[slice]] entries must be tables.";
        return false;
      }

      bool ok = true;
      SliceConfig sc;
      sc.name = get_str_strict(*tbl, "name", local_args, this_dir, current_ns, "", error_out, ok);

      if (!ok) {
        error_out.insert(0U, "[[slice]]: ");
        return false;
      }

      if (!parse_resources_table(*tbl, true, local_args, this_dir, current_ns, sc.limits, error_out)) {
        error_out.insert(0U, "Slice '" + sc.name + "': ");
        return false;
      }

      out.slices.push_back(std::move(sc));
    }
  }

  const auto* groups_arr = root["group"].as_array();

  if (groups_arr != nullptr) {
    for (const auto& item : *groups_arr) {
      const auto* tbl = item.as_table();

      if (tbl == nullptr) {
        error_out = "[[group]] entries must be tables.";
        return false;
      }

      bool ok = true;
      GroupConfig gc;
      const std::string gns = get_str_strict(*tbl, "namespace", local_args, this_dir, current_ns, "", error_out, ok);

      if (!ok) {
        return false;
      }

      gc.ns = join_namespace(current_ns, gns);
      const std::string raw_name = get_str_strict(*tbl, "name", local_args, this_dir, current_ns, "", error_out, ok);

      if (!ok) {
        return false;
      }

      gc.name = qualify_name(current_ns, raw_name);

      if (!validate_allowed_keys(*tbl, "[[group]] '" + gc.name + "'",
                                 {"name", "namespace", "slice", "condition", "autostart", "mode", "members", "after",
                                  "before", "urls", "env", "remaps", "params_file", "params_files", "params"},
                                 error_out)) {
        return false;
      }

      const std::string& grp_ns = gc.ns;
      gc.slice = get_str_strict(*tbl, "slice", local_args, this_dir, grp_ns, "", error_out, ok);

      if (!ok) {
        return false;
      }

      gc.condition = parse_condition_node(*tbl, "condition", local_args, this_dir, grp_ns, error_out, ok, true);

      if (!ok) {
        error_out.insert(0U, "Group '" + gc.name + "': ");
        return false;
      }

      gc.autostart = get_bool_strict(*tbl, "autostart", true, error_out, ok);

      if (!ok) {
        return false;
      }

      const std::string mode_str =
          get_str_strict(*tbl, "mode", local_args, this_dir, grp_ns, "parallel", error_out, ok);

      if (!ok) {
        return false;
      }

      if (ascii_iequals(mode_str, "parallel")) {
        gc.mode = GroupExecutionMode::kParallel;
      } else if (ascii_iequals(mode_str, "serial")) {
        gc.mode = GroupExecutionMode::kSerial;
      } else {
        error_out = "Group '" + gc.name + "' has unknown mode '" + mode_str + "' (expected parallel|serial).";
        return false;
      }

      gc.members = get_qualified_ref_list_strict(*tbl, "members", local_args, this_dir, current_ns, error_out, ok);

      if (!ok) {
        return false;
      }

      gc.after = get_qualified_ref_list_strict(*tbl, "after", local_args, this_dir, current_ns, error_out, ok);

      if (!ok) {
        return false;
      }

      gc.before = get_qualified_ref_list_strict(*tbl, "before", local_args, this_dir, current_ns, error_out, ok);

      if (!ok) {
        return false;
      }

      const toml::table* grp_urls_tbl = nullptr;

      if (!require_table_if_present(*tbl, "urls", "Group '" + gc.name + "'", grp_urls_tbl, error_out)) {
        return false;
      }

      if (grp_urls_tbl != nullptr && !parse_url_table(*grp_urls_tbl, "Group '" + gc.name + "'", "urls", local_args,
                                                      this_dir, grp_ns, gc.urls, error_out)) {
        return false;
      }

      const toml::table* grp_env_tbl = nullptr;

      if (!require_table_if_present(*tbl, "env", "Group '" + gc.name + "'", grp_env_tbl, error_out)) {
        return false;
      }

      if (grp_env_tbl != nullptr && !parse_env_table_strict(*grp_env_tbl, "Group '" + gc.name + "'", local_args,
                                                            this_dir, grp_ns, gc.env, error_out)) {
        return false;
      }

      const toml::table* grp_remaps_tbl = nullptr;

      if (!require_table_if_present(*tbl, "remaps", "Group '" + gc.name + "'", grp_remaps_tbl, error_out)) {
        return false;
      }

      if (grp_remaps_tbl != nullptr && !parse_remaps_table_strict(*grp_remaps_tbl, "Group '" + gc.name + "'",
                                                                  local_args, this_dir, grp_ns, gc.remaps, error_out)) {
        return false;
      }

      if (!load_params_files(*tbl, local_args, this_dir, grp_ns, gc.params, error_out)) {
        return false;
      }

      const toml::table* grp_params_tbl = nullptr;

      if (!require_table_if_present(*tbl, "params", "Group '" + gc.name + "'", grp_params_tbl, error_out)) {
        return false;
      }

      if (grp_params_tbl != nullptr &&
          !flatten_toml_table(*grp_params_tbl, "", local_args, this_dir, grp_ns, gc.params, error_out, 0U)) {
        return false;
      }

      out.groups.push_back(std::move(gc));
    }
  }

  const auto* sched_arr = root["scheduler"].as_array();

  if (sched_arr != nullptr) {
    for (const auto& item : *sched_arr) {
      const auto* tbl = item.as_table();

      if (tbl == nullptr) {
        error_out = "[[scheduler]] entries must be tables.";
        return false;
      }

      bool ok = true;
      SchedulerConfig sc;
      sc.name = get_str_strict(*tbl, "name", local_args, this_dir, current_ns, "", error_out, ok);

      if (!ok) {
        return false;
      }

      if (!validate_allowed_keys(
              *tbl, "[[scheduler]] '" + sc.name + "'",
              {"name", "mode", "queue", "strategy", "policy", "threads", "cpus", "priority", "max_elapsed_ms"},
              error_out)) {
        return false;
      }

      const std::string mode_s =
          get_str_strict(*tbl, "mode", local_args, this_dir, current_ns, "shared", error_out, ok);

      if (!ok || !parse_scheduler_mode_strict(mode_s, sc.mode)) {
        if (ok) {
          error_out = "Scheduler '" + sc.name + "' has unknown mode '" + mode_s + "'.";
        }

        return false;
      }

      const std::string queue_s =
          get_str_strict(*tbl, "queue", local_args, this_dir, current_ns, "normal", error_out, ok);

      if (!ok || !parse_queue_type_strict(queue_s, sc.queue_type)) {
        if (ok) {
          error_out = "Scheduler '" + sc.name + "' has unknown queue type '" + queue_s + "'.";
        }

        return false;
      }

      const std::string strat_s =
          get_str_strict(*tbl, "strategy", local_args, this_dir, current_ns, "optimization", error_out, ok);

      if (!ok || !parse_strategy_strict(strat_s, sc.strategy)) {
        if (ok) {
          error_out = "Scheduler '" + sc.name + "' has unknown strategy '" + strat_s + "'.";
        }

        return false;
      }

      const std::string pol_s =
          get_str_strict(*tbl, "policy", local_args, this_dir, current_ns, "default", error_out, ok);

      if (!ok || !parse_sched_policy_strict(pol_s, sc.sched_policy)) {
        if (ok) {
          error_out = "Scheduler '" + sc.name + "' has unknown policy '" + pol_s + "'.";
        }

        return false;
      }

      const int64_t threads = get_i64_range(*tbl, "threads", 1, 1, 1024, error_out, ok);

      if (!ok) {
        error_out.insert(0U, "Scheduler '" + sc.name + "': ");
        return false;
      }

      sc.threads = static_cast<size_t>(threads);
      sc.cpu_affinity = get_u32_list_strict(*tbl, "cpus", error_out, ok);

      if (!ok) {
        error_out.insert(0U, "Scheduler '" + sc.name + "': ");
        return false;
      }

      const int64_t prio = get_i64_range(*tbl, "priority", 0, kMinI32, kMaxI32, error_out, ok);

      if (!ok) {
        error_out.insert(0U, "Scheduler '" + sc.name + "': ");
        return false;
      }

      sc.thread_priority = static_cast<int32_t>(prio);
      const int64_t max_el = get_i64_range(*tbl, "max_elapsed_ms", 0, 0, kMaxU32, error_out, ok);

      if (!ok) {
        error_out.insert(0U, "Scheduler '" + sc.name + "': ");
        return false;
      }

      sc.max_elapsed_ms = static_cast<uint32_t>(max_el);
      out.schedulers.push_back(std::move(sc));
    }
  }

  return true;
}

static bool parse_manifest_args_section(const toml::table& root, const std::string& this_dir,
                                        const std::string& parent_ns, bool is_root,
                                        std::unordered_map<std::string, std::string>& local_args, LaunchManifest& out,
                                        std::string& error_out) {
  auto unescape_var_literals = [](std::string_view raw) -> std::string {
    std::string out_str;
    out_str.reserve(raw.size());
    size_t pos = 0U;

    while (pos < raw.size()) {
      if (pos + 2U < raw.size() && raw[pos] == '$' && raw[pos + 1U] == '$' && raw[pos + 2U] == '{') {
        out_str.append("${");
        pos += 3U;
      } else {
        out_str.push_back(raw[pos]);
        ++pos;
      }
    }

    return out_str;
  };

  const toml::table* args_tbl = nullptr;

  if (!require_table_if_present(root, "args", "manifest root", args_tbl, error_out)) {
    return false;
  }

  if (args_tbl == nullptr) {
    return true;
  }

  for (const auto& [k, v] : *args_tbl) {
    const std::string key(k.str());
    ArgSpec spec;
    spec.name = key;
    const auto* sub = v.as_table();
    const auto* vs = v.as_string();
    const auto* vi = v.as_integer();
    const auto* vf = v.as_floating_point();
    const auto* vb = v.as_boolean();

    if (sub != nullptr) {
      if (!validate_allowed_keys(*sub, "[args." + key + "]", {"default", "desc", "choices"}, error_out)) {
        return false;
      }

      const auto* def = sub->get("default");

      if (def != nullptr) {
        const auto* ds = def->as_string();
        const auto* di = def->as_integer();
        const auto* df = def->as_floating_point();
        const auto* db = def->as_boolean();

        if (ds != nullptr) {
          spec.default_value = unescape_var_literals(ds->get());
        } else if (di != nullptr) {
          spec.default_value = std::to_string(di->get());
        } else if (df != nullptr) {
          char buf[64];
          const size_t n = Helpers::format_floating_spec_to(buf, sizeof(buf) - 1U, df->get(), 'g', 17, false);
          spec.default_value.assign(buf, n);
        } else if (db != nullptr) {
          spec.default_value = db->get() ? "true" : "false";
        } else {
          error_out = "[args." + key + "].default must be a scalar value.";
          return false;
        }
      }

      bool ok = true;
      spec.description = get_str_strict(*sub, "desc", local_args, this_dir, parent_ns, "", error_out, ok);

      if (!ok) {
        return false;
      }

      spec.choices = get_str_list_strict(*sub, "choices", local_args, this_dir, parent_ns, error_out, ok);

      if (!ok) {
        return false;
      }
    } else if (vs != nullptr) {
      spec.default_value = unescape_var_literals(vs->get());
    } else if (vi != nullptr) {
      spec.default_value = std::to_string(vi->get());
    } else if (vf != nullptr) {
      char buf[64];
      const size_t n = Helpers::format_floating_spec_to(buf, sizeof(buf) - 1U, vf->get(), 'g', 17, false);
      spec.default_value.assign(buf, n);
    } else if (vb != nullptr) {
      spec.default_value = vb->get() ? "true" : "false";
    } else {
      error_out = "[args]." + key + " must be a scalar or arg specification table.";
      return false;
    }

    auto it = local_args.find(key);

    if (it == local_args.end()) {
      local_args[key] = spec.default_value;
      spec.resolved_value = spec.default_value;
    } else {
      spec.resolved_value = it->second;
    }

    if (!spec.choices.empty() &&
        std::find(spec.choices.begin(), spec.choices.end(), spec.resolved_value) == spec.choices.end()) {
      error_out = "Argument '" + key + "' value '" + spec.resolved_value + "' is not in allowed choices.";
      return false;
    }

    if (is_root) {
      out.arg_specs.push_back(std::move(spec));
    }
  }

  return true;
}

static bool expand_for_each_strict(const toml::table& tbl,
                                   const std::unordered_map<std::string, std::string>& local_args,
                                   const std::string& this_dir, const std::string& current_ns, ParseBudget& budget,
                                   std::vector<std::unordered_map<std::string, std::string>>& expanded,
                                   std::string& error_out) {
  expanded.clear();
  const auto* fe_node = tbl.get("for_each");
  const auto* cnt_node = tbl.get("count");

  if (fe_node != nullptr && cnt_node != nullptr) {
    error_out = "Cannot specify both 'for_each' and 'count' in the same table.";
    return false;
  }

  std::vector<std::string> items;

  if (fe_node != nullptr) {
    const auto* fe_arr = fe_node->as_array();
    const auto* fe_str = fe_node->as_string();

    if (fe_arr != nullptr) {
      items.reserve(fe_arr->size());

      for (const auto& elem : *fe_arr) {
        const auto* es = elem.as_string();
        const auto* eiv = elem.as_integer();

        if (es != nullptr) {
          items.push_back(substitute_vars(es->get(), local_args, this_dir, current_ns));
        } else if (eiv != nullptr) {
          items.push_back(std::to_string(eiv->get()));
        } else {
          error_out = "Field 'for_each' array elements must be strings or integers.";
          return false;
        }
      }
    } else if (fe_str != nullptr) {
      const std::string resolved = substitute_vars(fe_str->get(), local_args, this_dir, current_ns);

      for (const auto tok : Helpers::split_view(resolved, ',')) {
        const auto trimmed = Helpers::trim_string_view(tok);

        if (!trimmed.empty()) {
          items.emplace_back(trimmed);
        }
      }
    } else {
      error_out = "Field 'for_each' must be an array or comma-separated string.";
      return false;
    }

    if (items.size() > 256U) {
      error_out = "Field 'for_each' expansion count (" + std::to_string(items.size()) + ") exceeds maximum (256).";
      return false;
    }
  } else if (cnt_node != nullptr) {
    const auto* cnt = cnt_node->as_integer();

    if (cnt == nullptr) {
      error_out = "Field 'count' must be an integer in [0, 256].";
      return false;
    }

    const int64_t raw = cnt->get();

    if (raw < 0 || raw > 256) {
      error_out = "Field 'count' must be in range [0, 256], got " + std::to_string(raw) + ".";
      return false;
    }

    items.reserve(static_cast<size_t>(raw));

    for (int64_t k = 0; k < raw; ++k) {
      items.push_back(std::to_string(k));
    }
  } else {
    if (budget.total_expansions + 1U > kMaxTotalExpandedEntries) {
      error_out =
          "Manifest expansion exceeded maximum total entries (" + std::to_string(kMaxTotalExpandedEntries) + ").";
      return false;
    }

    ++budget.total_expansions;
    expanded.push_back(local_args);
    return true;
  }

  if (budget.total_expansions + items.size() > kMaxTotalExpandedEntries) {
    error_out = "Manifest expansion exceeded maximum total entries (" + std::to_string(kMaxTotalExpandedEntries) + ").";
    return false;
  }

  budget.total_expansions += items.size();
  expanded.reserve(items.size());

  for (size_t idx = 0U; idx < items.size(); ++idx) {
    auto iter_args = local_args;
    iter_args["item"] = items[idx];
    iter_args["index"] = std::to_string(idx);
    expanded.push_back(std::move(iter_args));
  }

  return true;
}

// NOLINTNEXTLINE(google-readability-function-size)
static bool parse_table_recursive(const toml::table& root, const std::string& this_dir, const std::string& parent_ns,
                                  const std::unordered_map<std::string, std::string>& inherited_args,
                                  const LexicalScope& inherited_scope, std::vector<LexicalScope>& scopes,
                                  std::unordered_map<std::string, size_t>& unit_scopes,
                                  std::set<std::string>& active_files, ParseBudget& budget, size_t depth, bool is_root,
                                  LaunchManifest& out, std::string& error_out) {
  if (depth > kMaxIncludeDepth) {
    error_out = "TOML include depth exceeded maximum (" + std::to_string(kMaxIncludeDepth) + ").";
    return false;
  }

  if (!validate_allowed_keys(
          root, "manifest root",
          {"args", "launcher", "urls", "env", "remaps", "params_file", "params_files", "params", "logger", "record",
           "proxy", "slice", "group", "scheduler", "container", "component", "process", "task", "include"},
          error_out)) {
    return false;
  }

  for (const std::string_view arr_key :
       {"slice", "group", "scheduler", "container", "component", "process", "task", "include"}) {
    const auto* node = root.get(arr_key);

    if (node != nullptr && !node->is_array()) {
      error_out =
          "Top-level key '" + std::string(arr_key) + "' must be an array of tables ([[" + std::string(arr_key) + "]]).";
      return false;
    }
  }

  if (!is_root) {
    for (const std::string_view root_only : {"logger", "record", "proxy"}) {
      if (root.contains(root_only)) {
        error_out = "Section '[" + std::string(root_only) + "]' is only allowed in the root manifest.";
        return false;
      }
    }
  }

  std::unordered_map<std::string, std::string> local_args = inherited_args;

  if (!parse_manifest_args_section(root, this_dir, parent_ns, is_root, local_args, out, error_out)) {
    return false;
  }

  if (is_root) {
    for (const auto& [k, v] : local_args) {
      if (!Helpers::has_startwith(k, "__url__.") && k != "item" && k != "index") {
        out.args[k] = v;
      }
    }
  }

  std::string current_ns = parent_ns;
  const toml::table* launcher_tbl = nullptr;

  if (!require_table_if_present(root, "launcher", "manifest root", launcher_tbl, error_out)) {
    return false;
  }

  if (launcher_tbl != nullptr) {
    bool ok = true;
    const std::string file_ns =
        get_str_strict(*launcher_tbl, "namespace", local_args, this_dir, current_ns, "", error_out, ok);

    if (!ok) {
      return false;
    }

    current_ns = join_namespace(parent_ns, file_ns);

    if (is_root) {
      if (!parse_launcher_root_section(*launcher_tbl, local_args, this_dir, current_ns, out, error_out)) {
        return false;
      }
    } else if (!validate_allowed_keys(*launcher_tbl, "included [launcher]", {"namespace", "plugin_dirs"}, error_out)) {
      return false;
    }

    auto pdirs = get_str_list_strict(*launcher_tbl, "plugin_dirs", local_args, this_dir, current_ns, error_out, ok);

    if (!ok) {
      return false;
    }

    for (auto& dir : pdirs) {
      if (!dir.empty()) {
        std::filesystem::path p(dir);

        if (p.is_relative()) {
          p = std::filesystem::path(this_dir) / p;
        }

        out.plugin_dirs.push_back(Helpers::path_to_string(p));
      }
    }
  }

  LexicalScope file_scope = inherited_scope;
  inject_url_vars(file_scope.urls, local_args);

  const toml::table* urls_tbl = nullptr;

  if (!require_table_if_present(root, "urls", "manifest root", urls_tbl, error_out)) {
    return false;
  }

  if (urls_tbl != nullptr) {
    if (!parse_url_table(*urls_tbl, "Manifest", "urls", local_args, this_dir, current_ns, file_scope.urls, error_out)) {
      return false;
    }

    inject_url_vars(file_scope.urls, local_args);
  }

  const toml::table* env_tbl = nullptr;

  if (!require_table_if_present(root, "env", "manifest root", env_tbl, error_out)) {
    return false;
  }

  if (env_tbl != nullptr &&
      !parse_env_table_strict(*env_tbl, "Manifest", local_args, this_dir, current_ns, file_scope.env, error_out)) {
    return false;
  }

  const toml::table* remaps_tbl = nullptr;

  if (!require_table_if_present(root, "remaps", "manifest root", remaps_tbl, error_out)) {
    return false;
  }

  if (remaps_tbl != nullptr && !parse_remaps_table_strict(*remaps_tbl, "Manifest", local_args, this_dir, current_ns,
                                                          file_scope.remaps, error_out)) {
    return false;
  }

  if (!load_params_files(root, local_args, this_dir, current_ns, file_scope.params, error_out)) {
    return false;
  }

  const toml::table* params_tbl = nullptr;

  if (!require_table_if_present(root, "params", "manifest root", params_tbl, error_out)) {
    return false;
  }

  if (params_tbl != nullptr &&
      !flatten_toml_table(*params_tbl, "", local_args, this_dir, current_ns, file_scope.params, error_out, 0U)) {
    return false;
  }

  const size_t this_scope_idx = scopes.size();
  scopes.push_back(file_scope);

  if (is_root) {
    out.global_urls = file_scope.urls;
    out.global_env = file_scope.env;
    out.global_remaps = file_scope.remaps;
    out.global_params = file_scope.params;

    if (!parse_logger_and_record_sections(root, local_args, this_dir, current_ns, out, error_out)) {
      return false;
    }

    const toml::table* proxy_tbl = nullptr;

    if (!require_table_if_present(root, "proxy", "manifest root", proxy_tbl, error_out)) {
      return false;
    }

    if (proxy_tbl != nullptr) {
      if (!parse_proxy_section(*proxy_tbl, local_args, this_dir, current_ns, out.proxy, error_out)) {
        return false;
      }

      unit_scopes[out.proxy.name] = this_scope_idx;
    }
  }

  if (!parse_slices_groups_schedulers(root, local_args, this_dir, current_ns, out, error_out)) {
    return false;
  }

  const auto* cont_arr = root["container"].as_array();

  if (cont_arr != nullptr) {
    for (const auto& item : *cont_arr) {
      const auto* tbl = item.as_table();

      if (tbl == nullptr) {
        error_out = "[[container]] entries must be tables.";
        return false;
      }

      if (!validate_allowed_keys(*tbl, "[[container]]",
                                 {"name",
                                  "namespace",
                                  "group",
                                  "stage",
                                  "for_each",
                                  "count",
                                  "condition",
                                  "autostart",
                                  "required",
                                  "delay_ms",
                                  "stop_timeout_ms",
                                  "inherit_env",
                                  "cwd",
                                  "prefix",
                                  "output",
                                  "kill_mode",
                                  "stop_signal",
                                  "on_exit",
                                  "restart",
                                  "restart_delay_ms",
                                  "restart_max_ms",
                                  "restart_backoff",
                                  "start_burst",
                                  "start_window_ms",
                                  "ok_codes",
                                  "no_restart_codes",
                                  "resources",
                                  "depends_on",
                                  "after",
                                  "before",
                                  "requires",
                                  "wants",
                                  "binds_to",
                                  "part_of",
                                  "conflicts",
                                  "on_start",
                                  "on_ready",
                                  "on_exit_trigger",
                                  "on_error",
                                  "env",
                                  "record",
                                  "record_path"},
                                 error_out)) {
        return false;
      }

      std::vector<std::unordered_map<std::string, std::string>> iter_list;

      if (!expand_for_each_strict(*tbl, local_args, this_dir, current_ns, budget, iter_list, error_out)) {
        return false;
      }

      for (const auto& iter_args : iter_list) {
        bool ok = true;
        ContainerSpec cs;
        const std::string cns = get_str_strict(*tbl, "namespace", iter_args, this_dir, current_ns, "", error_out, ok);

        if (!ok) {
          return false;
        }

        cs.ns = join_namespace(current_ns, cns);
        const std::string raw_name = get_str_strict(*tbl, "name", iter_args, this_dir, current_ns, "", error_out, ok);

        if (!ok) {
          return false;
        }

        cs.name = qualify_name(current_ns, raw_name);
        const std::string raw_grp = get_str_strict(*tbl, "group", iter_args, this_dir, current_ns, "", error_out, ok);

        if (!ok) {
          return false;
        }

        cs.task_group = qualify_name(current_ns, raw_grp);
        const int64_t stage_v = get_i64_range(*tbl, "stage", 0, kMinI32, kMaxI32, error_out, ok);

        if (!ok) {
          error_out.insert(0U, "Container '" + cs.name + "': ");
          return false;
        }

        cs.stage = static_cast<int32_t>(stage_v);
        cs.condition = parse_condition_node(*tbl, "condition", iter_args, this_dir, cs.ns, error_out, ok, true);

        if (!ok) {
          error_out.insert(0U, "Container '" + cs.name + "': ");
          return false;
        }

        cs.autostart = get_bool_strict(*tbl, "autostart", true, error_out, ok);

        if (!ok) {
          error_out.insert(0U, "Container '" + cs.name + "': ");
          return false;
        }

        cs.required = get_bool_strict(*tbl, "required", false, error_out, ok);

        if (!ok) {
          error_out.insert(0U, "Container '" + cs.name + "': ");
          return false;
        }

        const int64_t delay_v = get_i64_range(*tbl, "delay_ms", 0, 0, kMaxU32, error_out, ok);

        if (!ok) {
          error_out.insert(0U, "Container '" + cs.name + "': ");
          return false;
        }

        cs.delay_ms = static_cast<uint32_t>(delay_v);
        const int64_t stop_to = get_i64_range(*tbl, "stop_timeout_ms", 5000, 0, kMaxU32, error_out, ok);

        if (!ok) {
          error_out.insert(0U, "Container '" + cs.name + "': ");
          return false;
        }

        cs.sigterm_timeout_ms = static_cast<uint32_t>(std::max<int64_t>(200, stop_to));
        cs.inherit_env = get_bool_strict(*tbl, "inherit_env", true, error_out, ok);

        if (!ok) {
          error_out.insert(0U, "Container '" + cs.name + "': ");
          return false;
        }

        cs.cwd = get_str_strict(*tbl, "cwd", iter_args, this_dir, cs.ns, "", error_out, ok);

        if (!ok) {
          error_out.insert(0U, "Container '" + cs.name + "': ");
          return false;
        }

        cs.launch_prefix = get_str_strict(*tbl, "prefix", iter_args, this_dir, cs.ns, "", error_out, ok);

        if (!ok) {
          error_out.insert(0U, "Container '" + cs.name + "': ");
          return false;
        }

        const std::string out_s = get_str_strict(*tbl, "output", iter_args, this_dir, cs.ns, "screen", error_out, ok);

        if (!ok || !parse_output_mode_strict(out_s, cs.output)) {
          if (ok) {
            error_out = "Container '" + cs.name + "' has unknown output mode '" + out_s + "'.";
          }

          return false;
        }

        const std::string km_s =
            get_str_strict(*tbl, "kill_mode", iter_args, this_dir, cs.ns, "control-group", error_out, ok);

        if (!ok || !parse_kill_mode_strict(km_s, cs.kill_mode)) {
          if (ok) {
            error_out = "Container '" + cs.name + "' has unknown kill_mode '" + km_s + "'.";
          }

          return false;
        }

        const std::string sig_s = get_str_strict(*tbl, "stop_signal", iter_args, this_dir, cs.ns, "", error_out, ok);

        if (!ok || !parse_stop_signal_strict(sig_s, cs.stop_signal)) {
          if (ok) {
            error_out = "Container '" + cs.name + "' has unknown stop_signal '" + sig_s + "'.";
          }

          return false;
        }

        const std::string oe_s = get_str_strict(*tbl, "on_exit", iter_args, this_dir, cs.ns, "ignore", error_out, ok);

        if (!ok || !parse_on_exit_strict(oe_s, cs.on_exit)) {
          if (ok) {
            error_out = "Container '" + cs.name + "' has unknown on_exit action '" + oe_s + "'.";
          }

          return false;
        }

        if (!parse_respawn(*tbl, iter_args, this_dir, cs.ns, cs.respawn, error_out)) {
          error_out.insert(0U, "Container '" + cs.name + "': ");
          return false;
        }

        if (!parse_resources(*tbl, iter_args, this_dir, cs.ns, cs.resources, error_out)) {
          error_out.insert(0U, "Container '" + cs.name + "': ");
          return false;
        }

        if (!parse_deps_strict(*tbl, iter_args, this_dir, current_ns, cs.deps, error_out)) {
          error_out.insert(0U, "Container '" + cs.name + "': ");
          return false;
        }

        if (!parse_unit_triggers_strict(*tbl, iter_args, this_dir, current_ns, cs.on_start_trigger, cs.on_ready_trigger,
                                        cs.on_exit_trigger, cs.on_error_trigger, error_out)) {
          error_out.insert(0U, "Container '" + cs.name + "': ");
          return false;
        }

        const toml::table* cont_env_tbl = nullptr;

        if (!require_table_if_present(*tbl, "env", "Container '" + cs.name + "'", cont_env_tbl, error_out)) {
          return false;
        }

        if (cont_env_tbl != nullptr && !parse_env_table_strict(*cont_env_tbl, "Container '" + cs.name + "'", iter_args,
                                                               this_dir, cs.ns, cs.env, error_out)) {
          return false;
        }

        cs.record_enabled = get_opt_bool_strict(*tbl, "record", error_out, ok);

        if (!ok) {
          error_out.insert(0U, "Container '" + cs.name + "': ");
          return false;
        }

        cs.record_path = get_str_strict(*tbl, "record_path", iter_args, this_dir, cs.ns, "", error_out, ok);

        if (!ok) {
          error_out.insert(0U, "Container '" + cs.name + "': ");
          return false;
        }

        unit_scopes[cs.name] = this_scope_idx;
        out.containers.push_back(std::move(cs));
      }
    }
  }

  const auto* comp_arr = root["component"].as_array();

  if (comp_arr != nullptr) {
    for (const auto& item : *comp_arr) {
      const auto* tbl = item.as_table();

      if (tbl == nullptr) {
        error_out = "[[component]] entries must be tables.";
        return false;
      }

      if (!validate_allowed_keys(*tbl, "[[component]]",
                                 {"name",
                                  "namespace",
                                  "group",
                                  "lib",
                                  "class",
                                  "scheduler",
                                  "pool",
                                  "container",
                                  "stage",
                                  "worker",
                                  "priority",
                                  "watchdog_ms",
                                  "legacy",
                                  "for_each",
                                  "count",
                                  "condition",
                                  "autostart",
                                  "init_state",
                                  "required",
                                  "delay_ms",
                                  "reload",
                                  "depends_on",
                                  "after",
                                  "before",
                                  "requires",
                                  "wants",
                                  "binds_to",
                                  "part_of",
                                  "conflicts",
                                  "restart",
                                  "restart_delay_ms",
                                  "restart_max_ms",
                                  "restart_backoff",
                                  "start_burst",
                                  "start_window_ms",
                                  "ok_codes",
                                  "no_restart_codes",
                                  "urls",
                                  "inputs",
                                  "outputs",
                                  "params_file",
                                  "params_files",
                                  "params",
                                  "remaps",
                                  "remap_list",
                                  "ready",
                                  "liveness",
                                  "on_start",
                                  "on_ready",
                                  "on_exit_trigger",
                                  "on_error",
                                  "record",
                                  "record_path",
                                  "record_force"},
                                 error_out)) {
        return false;
      }

      std::vector<std::unordered_map<std::string, std::string>> iter_list;

      if (!expand_for_each_strict(*tbl, local_args, this_dir, current_ns, budget, iter_list, error_out)) {
        return false;
      }

      for (const auto& iter_args : iter_list) {
        bool ok = true;
        ComponentSpec cs;
        const std::string cns = get_str_strict(*tbl, "namespace", iter_args, this_dir, current_ns, "", error_out, ok);

        if (!ok) {
          return false;
        }

        cs.ns = join_namespace(current_ns, cns);
        const std::string raw_name = get_str_strict(*tbl, "name", iter_args, this_dir, current_ns, "", error_out, ok);

        if (!ok) {
          return false;
        }

        cs.name = qualify_name(current_ns, raw_name);
        const std::string raw_grp = get_str_strict(*tbl, "group", iter_args, this_dir, current_ns, "", error_out, ok);

        if (!ok) {
          return false;
        }

        cs.task_group = qualify_name(current_ns, raw_grp);
        cs.library = get_str_strict(*tbl, "lib", iter_args, this_dir, cs.ns, "", error_out, ok);

        if (!ok) {
          error_out.insert(0U, "Component '" + cs.name + "': ");
          return false;
        }

        cs.class_name = get_str_strict(*tbl, "class", iter_args, this_dir, cs.ns, "", error_out, ok);

        if (!ok) {
          error_out.insert(0U, "Component '" + cs.name + "': ");
          return false;
        }

        cs.scheduler_group = get_str_strict(*tbl, "scheduler", iter_args, this_dir, cs.ns, "default", error_out, ok);

        if (!ok) {
          error_out.insert(0U, "Component '" + cs.name + "': ");
          return false;
        }

        cs.compute_pool = get_str_strict(*tbl, "pool", iter_args, this_dir, cs.ns, "", error_out, ok);

        if (!ok) {
          error_out.insert(0U, "Component '" + cs.name + "': ");
          return false;
        }

        const std::string raw_cont =
            get_str_strict(*tbl, "container", iter_args, this_dir, current_ns, "", error_out, ok);

        if (!ok) {
          error_out.insert(0U, "Component '" + cs.name + "': ");
          return false;
        }

        cs.container = qualify_name(current_ns, raw_cont);
        const int64_t stage_v = get_i64_range(*tbl, "stage", 0, kMinI32, kMaxI32, error_out, ok);

        if (!ok) {
          error_out.insert(0U, "Component '" + cs.name + "': ");
          return false;
        }

        cs.stage = static_cast<int32_t>(stage_v);
        const auto* w_node = tbl->get("worker");

        if (w_node != nullptr) {
          const auto* wi = w_node->as_integer();
          const auto* ws = w_node->as_string();

          if (wi != nullptr) {
            if (wi->get() < kMinI32 || wi->get() > kMaxI32) {
              error_out = "Component '" + cs.name + "' worker index out of int32 range.";
              return false;
            }

            cs.worker_index = static_cast<int32_t>(wi->get());
          } else if (ws != nullptr) {
            const std::string resolved = substitute_vars(ws->get(), iter_args, this_dir, cs.ns);
            char* end_ptr = nullptr;
            const auto parsed = static_cast<int64_t>(std::strtoll(resolved.c_str(), &end_ptr, 10));

            if (end_ptr != resolved.c_str() && *end_ptr == '\0' && parsed >= kMinI32 && parsed <= kMaxI32) {
              cs.worker_index = static_cast<int32_t>(parsed);
            } else {
              error_out = "Component '" + cs.name + "' has invalid worker '" + resolved + "'.";
              return false;
            }
          } else {
            error_out = "Component '" + cs.name + "': Field 'worker' must be an integer or numeric string.";
            return false;
          }
        }

        const int64_t t_prio = get_i64_range(*tbl, "priority", 0, 0, 65535, error_out, ok);

        if (!ok) {
          error_out.insert(0U, "Component '" + cs.name + "': ");
          return false;
        }

        cs.task_priority = static_cast<uint16_t>(t_prio);
        const int64_t wd_ms = get_i64_range(*tbl, "watchdog_ms", 0, 0, kMaxU32, error_out, ok);

        if (!ok) {
          error_out.insert(0U, "Component '" + cs.name + "': ");
          return false;
        }

        cs.watchdog_ms = static_cast<uint32_t>(wd_ms);
        cs.legacy_runnable = get_bool_strict(*tbl, "legacy", false, error_out, ok);

        if (!ok) {
          error_out.insert(0U, "Component '" + cs.name + "': ");
          return false;
        }

        cs.condition = parse_condition_node(*tbl, "condition", iter_args, this_dir, cs.ns, error_out, ok, true);

        if (!ok) {
          error_out.insert(0U, "Component '" + cs.name + "': ");
          return false;
        }

        cs.autostart = get_bool_strict(*tbl, "autostart", true, error_out, ok);

        if (!ok) {
          error_out.insert(0U, "Component '" + cs.name + "': ");
          return false;
        }

        const std::string auto_state =
            get_str_strict(*tbl, "init_state", iter_args, this_dir, cs.ns, "active", error_out, ok);

        if (!ok) {
          error_out.insert(0U, "Component '" + cs.name + "': ");
          return false;
        }

        if (ascii_iequals(auto_state, "inactive")) {
          cs.start_inactive = true;
        } else if (!ascii_iequals(auto_state, "active")) {
          error_out = "Component '" + cs.name + "' has unknown init_state '" + auto_state + "'.";
          return false;
        }

        cs.required = get_bool_strict(*tbl, "required", false, error_out, ok);

        if (!ok) {
          error_out.insert(0U, "Component '" + cs.name + "': ");
          return false;
        }

        const int64_t delay_v = get_i64_range(*tbl, "delay_ms", 0, 0, kMaxU32, error_out, ok);

        if (!ok) {
          error_out.insert(0U, "Component '" + cs.name + "': ");
          return false;
        }

        cs.delay_ms = static_cast<uint32_t>(delay_v);
        const std::string rel_s = get_str_strict(*tbl, "reload", iter_args, this_dir, cs.ns, "", error_out, ok);

        if (!ok || !parse_reload_mode_strict(rel_s, cs.reload_mode)) {
          if (ok) {
            error_out = "Component '" + cs.name + "' has unknown reload mode '" + rel_s + "'.";
          }

          return false;
        }

        if (!parse_deps_strict(*tbl, iter_args, this_dir, current_ns, cs.deps, error_out)) {
          error_out.insert(0U, "Component '" + cs.name + "': ");
          return false;
        }

        if (!parse_respawn(*tbl, iter_args, this_dir, cs.ns, cs.respawn, error_out)) {
          error_out.insert(0U, "Component '" + cs.name + "': ");
          return false;
        }

        std::unordered_map<std::string, std::string> unit_args = iter_args;

        if (!parse_unit_endpoints(*tbl, "Component '" + cs.name + "'", cs.name, cs.task_group, out.groups, this_dir,
                                  cs.ns, unit_args, cs.urls, cs.inputs, cs.outputs, error_out)) {
          return false;
        }

        if (!load_params_files(*tbl, unit_args, this_dir, cs.ns, cs.params, error_out)) {
          return false;
        }

        const toml::table* comp_params_tbl = nullptr;

        if (!require_table_if_present(*tbl, "params", "Component '" + cs.name + "'", comp_params_tbl, error_out)) {
          return false;
        }

        if (comp_params_tbl != nullptr &&
            !flatten_toml_table(*comp_params_tbl, "", unit_args, this_dir, cs.ns, cs.params, error_out, 0U)) {
          return false;
        }

        const toml::table* comp_remaps_tbl = nullptr;

        if (!require_table_if_present(*tbl, "remaps", "Component '" + cs.name + "'", comp_remaps_tbl, error_out)) {
          return false;
        }

        if (comp_remaps_tbl != nullptr &&
            !parse_remaps_table_strict(*comp_remaps_tbl, "Component '" + cs.name + "'", unit_args, this_dir, cs.ns,
                                       cs.remaps, error_out)) {
          return false;
        }

        const toml::array* comp_remap_list = nullptr;

        if (!require_array_if_present(*tbl, "remap_list", "Component '" + cs.name + "'", comp_remap_list, error_out)) {
          return false;
        }

        if (comp_remap_list != nullptr && !parse_remap_list_strict(*comp_remap_list, "Component '" + cs.name + "'",
                                                                   unit_args, this_dir, cs.ns, cs.remaps, error_out)) {
          return false;
        }

        if (!parse_ready_probe(*tbl, unit_args, this_dir, cs.ns, cs.ready, error_out)) {
          error_out.insert(0U, "Component '" + cs.name + "': ");
          return false;
        }

        if (!parse_liveness_probe(*tbl, unit_args, this_dir, cs.ns, cs.liveness, error_out)) {
          error_out.insert(0U, "Component '" + cs.name + "': ");
          return false;
        }

        if (!parse_unit_triggers_strict(*tbl, iter_args, this_dir, current_ns, cs.on_start_trigger, cs.on_ready_trigger,
                                        cs.on_exit_trigger, cs.on_error_trigger, error_out)) {
          error_out.insert(0U, "Component '" + cs.name + "': ");
          return false;
        }

        cs.record_enabled = get_opt_bool_strict(*tbl, "record", error_out, ok);

        if (!ok) {
          error_out.insert(0U, "Component '" + cs.name + "': ");
          return false;
        }

        cs.record_path = get_str_strict(*tbl, "record_path", unit_args, this_dir, cs.ns, "", error_out, ok);

        if (!ok) {
          error_out.insert(0U, "Component '" + cs.name + "': ");
          return false;
        }

        cs.record_force_publish = get_opt_bool_strict(*tbl, "record_force", error_out, ok);

        if (!ok) {
          error_out.insert(0U, "Component '" + cs.name + "': ");
          return false;
        }

        unit_scopes[cs.name] = this_scope_idx;
        out.components.push_back(std::move(cs));
      }
    }
  }

  const auto* proc_arr = root["process"].as_array();

  if (proc_arr != nullptr) {
    for (const auto& item : *proc_arr) {
      const auto* tbl = item.as_table();

      if (tbl == nullptr) {
        error_out = "[[process]] entries must be tables.";
        return false;
      }

      if (!validate_allowed_keys(*tbl, "[[process]]",
                                 {"name",
                                  "namespace",
                                  "group",
                                  "stage",
                                  "for_each",
                                  "count",
                                  "cmd",
                                  "args",
                                  "prefix",
                                  "cwd",
                                  "inherit_env",
                                  "condition",
                                  "autostart",
                                  "required",
                                  "delay_ms",
                                  "stop_timeout_ms",
                                  "output",
                                  "kill_mode",
                                  "stop_signal",
                                  "reload",
                                  "on_exit",
                                  "restart",
                                  "restart_delay_ms",
                                  "restart_max_ms",
                                  "restart_backoff",
                                  "start_burst",
                                  "start_window_ms",
                                  "ok_codes",
                                  "no_restart_codes",
                                  "resources",
                                  "depends_on",
                                  "after",
                                  "before",
                                  "requires",
                                  "wants",
                                  "binds_to",
                                  "part_of",
                                  "conflicts",
                                  "urls",
                                  "inputs",
                                  "outputs",
                                  "env",
                                  "remaps",
                                  "ready",
                                  "liveness",
                                  "on_start",
                                  "on_ready",
                                  "on_exit_trigger",
                                  "on_error",
                                  "record",
                                  "record_path"},
                                 error_out)) {
        return false;
      }

      std::vector<std::unordered_map<std::string, std::string>> iter_list;

      if (!expand_for_each_strict(*tbl, local_args, this_dir, current_ns, budget, iter_list, error_out)) {
        return false;
      }

      for (const auto& iter_args : iter_list) {
        bool ok = true;
        ProcessSpec ps;
        const std::string pns = get_str_strict(*tbl, "namespace", iter_args, this_dir, current_ns, "", error_out, ok);

        if (!ok) {
          return false;
        }

        ps.ns = join_namespace(current_ns, pns);
        const std::string raw_name = get_str_strict(*tbl, "name", iter_args, this_dir, current_ns, "", error_out, ok);

        if (!ok) {
          return false;
        }

        ps.name = qualify_name(current_ns, raw_name);
        const std::string raw_grp = get_str_strict(*tbl, "group", iter_args, this_dir, current_ns, "", error_out, ok);

        if (!ok) {
          return false;
        }

        ps.task_group = qualify_name(current_ns, raw_grp);
        std::unordered_map<std::string, std::string> unit_args = iter_args;

        if (!parse_unit_endpoints(*tbl, "Process '" + ps.name + "'", ps.name, ps.task_group, out.groups, this_dir,
                                  ps.ns, unit_args, ps.urls, ps.inputs, ps.outputs, error_out)) {
          return false;
        }

        const int64_t stage_v = get_i64_range(*tbl, "stage", 0, kMinI32, kMaxI32, error_out, ok);

        if (!ok) {
          error_out.insert(0U, "Process '" + ps.name + "': ");
          return false;
        }

        ps.stage = static_cast<int32_t>(stage_v);
        ps.program = get_str_strict(*tbl, "cmd", unit_args, this_dir, ps.ns, "", error_out, ok);

        if (!ok) {
          error_out.insert(0U, "Process '" + ps.name + "': ");
          return false;
        }

        ps.args = get_str_list_strict(*tbl, "args", unit_args, this_dir, ps.ns, error_out, ok);

        if (!ok) {
          error_out.insert(0U, "Process '" + ps.name + "': ");
          return false;
        }

        ps.launch_prefix = get_str_strict(*tbl, "prefix", unit_args, this_dir, ps.ns, "", error_out, ok);

        if (!ok) {
          error_out.insert(0U, "Process '" + ps.name + "': ");
          return false;
        }

        ps.cwd = get_str_strict(*tbl, "cwd", unit_args, this_dir, ps.ns, "", error_out, ok);

        if (!ok) {
          error_out.insert(0U, "Process '" + ps.name + "': ");
          return false;
        }

        ps.inherit_env = get_bool_strict(*tbl, "inherit_env", true, error_out, ok);

        if (!ok) {
          error_out.insert(0U, "Process '" + ps.name + "': ");
          return false;
        }

        ps.condition = parse_condition_node(*tbl, "condition", iter_args, this_dir, ps.ns, error_out, ok, true);

        if (!ok) {
          error_out.insert(0U, "Process '" + ps.name + "': ");
          return false;
        }

        ps.autostart = get_bool_strict(*tbl, "autostart", true, error_out, ok);

        if (!ok) {
          error_out.insert(0U, "Process '" + ps.name + "': ");
          return false;
        }

        ps.required = get_bool_strict(*tbl, "required", false, error_out, ok);

        if (!ok) {
          error_out.insert(0U, "Process '" + ps.name + "': ");
          return false;
        }

        const int64_t delay_v = get_i64_range(*tbl, "delay_ms", 0, 0, kMaxU32, error_out, ok);

        if (!ok) {
          error_out.insert(0U, "Process '" + ps.name + "': ");
          return false;
        }

        ps.delay_ms = static_cast<uint32_t>(delay_v);
        const int64_t stop_to = get_i64_range(*tbl, "stop_timeout_ms", 5000, 0, kMaxU32, error_out, ok);

        if (!ok) {
          error_out.insert(0U, "Process '" + ps.name + "': ");
          return false;
        }

        ps.sigterm_timeout_ms = static_cast<uint32_t>(std::max<int64_t>(200, stop_to));
        const std::string out_s = get_str_strict(*tbl, "output", iter_args, this_dir, ps.ns, "screen", error_out, ok);

        if (!ok || !parse_output_mode_strict(out_s, ps.output)) {
          if (ok) {
            error_out = "Process '" + ps.name + "' has unknown output mode '" + out_s + "'.";
          }

          return false;
        }

        const std::string km_s = get_str_strict(*tbl, "kill_mode", iter_args, this_dir, ps.ns, "", error_out, ok);

        if (!ok || !parse_kill_mode_strict(km_s, ps.kill_mode)) {
          if (ok) {
            error_out = "Process '" + ps.name + "' has unknown kill_mode '" + km_s + "'.";
          }

          return false;
        }

        const std::string sig_s = get_str_strict(*tbl, "stop_signal", iter_args, this_dir, ps.ns, "", error_out, ok);

        if (!ok || !parse_stop_signal_strict(sig_s, ps.stop_signal)) {
          if (ok) {
            error_out = "Process '" + ps.name + "' has unknown stop_signal '" + sig_s + "'.";
          }

          return false;
        }

        const std::string rel_s = get_str_strict(*tbl, "reload", iter_args, this_dir, ps.ns, "", error_out, ok);

        if (!ok || !parse_reload_mode_strict(rel_s, ps.reload_mode)) {
          if (ok) {
            error_out = "Process '" + ps.name + "' has unknown reload mode '" + rel_s + "'.";
          }

          return false;
        }

        const std::string oe_s = get_str_strict(*tbl, "on_exit", iter_args, this_dir, ps.ns, "ignore", error_out, ok);

        if (!ok || !parse_on_exit_strict(oe_s, ps.on_exit)) {
          if (ok) {
            error_out = "Process '" + ps.name + "' has unknown on_exit action '" + oe_s + "'.";
          }

          return false;
        }

        if (!parse_respawn(*tbl, iter_args, this_dir, ps.ns, ps.respawn, error_out)) {
          error_out.insert(0U, "Process '" + ps.name + "': ");
          return false;
        }

        if (!parse_resources(*tbl, iter_args, this_dir, ps.ns, ps.resources, error_out)) {
          error_out.insert(0U, "Process '" + ps.name + "': ");
          return false;
        }

        if (!parse_deps_strict(*tbl, unit_args, this_dir, current_ns, ps.deps, error_out)) {
          error_out.insert(0U, "Process '" + ps.name + "': ");
          return false;
        }

        const toml::table* proc_env_tbl = nullptr;

        if (!require_table_if_present(*tbl, "env", "Process '" + ps.name + "'", proc_env_tbl, error_out)) {
          return false;
        }

        if (proc_env_tbl != nullptr && !parse_env_table_strict(*proc_env_tbl, "Process '" + ps.name + "'", unit_args,
                                                               this_dir, ps.ns, ps.env, error_out)) {
          return false;
        }

        const toml::table* proc_remaps_tbl = nullptr;

        if (!require_table_if_present(*tbl, "remaps", "Process '" + ps.name + "'", proc_remaps_tbl, error_out)) {
          return false;
        }

        if (proc_remaps_tbl != nullptr &&
            !parse_remaps_table_strict(*proc_remaps_tbl, "Process '" + ps.name + "'", unit_args, this_dir, ps.ns,
                                       ps.remaps, error_out)) {
          return false;
        }

        if (!parse_ready_probe(*tbl, unit_args, this_dir, ps.ns, ps.ready, error_out)) {
          error_out.insert(0U, "Process '" + ps.name + "': ");
          return false;
        }

        if (!parse_liveness_probe(*tbl, unit_args, this_dir, ps.ns, ps.liveness, error_out)) {
          error_out.insert(0U, "Process '" + ps.name + "': ");
          return false;
        }

        if (!parse_unit_triggers_strict(*tbl, iter_args, this_dir, current_ns, ps.on_start_trigger, ps.on_ready_trigger,
                                        ps.on_exit_trigger, ps.on_error_trigger, error_out)) {
          error_out.insert(0U, "Process '" + ps.name + "': ");
          return false;
        }

        ps.record_enabled = get_opt_bool_strict(*tbl, "record", error_out, ok);

        if (!ok) {
          error_out.insert(0U, "Process '" + ps.name + "': ");
          return false;
        }

        ps.record_path = get_str_strict(*tbl, "record_path", unit_args, this_dir, ps.ns, "", error_out, ok);

        if (!ok) {
          error_out.insert(0U, "Process '" + ps.name + "': ");
          return false;
        }

        unit_scopes[ps.name] = this_scope_idx;
        out.processes.push_back(std::move(ps));
      }
    }
  }

  const auto* task_arr = root["task"].as_array();

  if (task_arr != nullptr) {
    for (const auto& item : *task_arr) {
      const auto* tbl = item.as_table();

      if (tbl == nullptr) {
        error_out = "[[task]] entries must be tables.";
        return false;
      }

      if (!validate_allowed_keys(*tbl, "[[task]]", {"name",        "namespace",
                                                    "group",       "stage",
                                                    "for_each",    "count",
                                                    "kind",        "cmd",
                                                    "args",        "prefix",
                                                    "cwd",         "inherit_env",
                                                    "condition",   "autostart",
                                                    "required",    "delay_ms",
                                                    "interval_ms", "timeout_ms",
                                                    "ok_codes",    "output",
                                                    "kill_mode",   "resources",
                                                    "depends_on",  "after",
                                                    "before",      "requires",
                                                    "wants",       "binds_to",
                                                    "part_of",     "conflicts",
                                                    "urls",        "inputs",
                                                    "outputs",     "env",
                                                    "remaps",      "on_start",
                                                    "on_ready",    "on_exit_trigger",
                                                    "on_error"},
                                 error_out)) {
        return false;
      }

      std::vector<std::unordered_map<std::string, std::string>> iter_list;

      if (!expand_for_each_strict(*tbl, local_args, this_dir, current_ns, budget, iter_list, error_out)) {
        return false;
      }

      for (const auto& iter_args : iter_list) {
        bool ok = true;
        TaskSpec ts;
        const std::string tns = get_str_strict(*tbl, "namespace", iter_args, this_dir, current_ns, "", error_out, ok);

        if (!ok) {
          return false;
        }

        ts.ns = join_namespace(current_ns, tns);
        const std::string raw_name = get_str_strict(*tbl, "name", iter_args, this_dir, current_ns, "", error_out, ok);

        if (!ok) {
          return false;
        }

        ts.name = qualify_name(current_ns, raw_name);
        const std::string raw_grp = get_str_strict(*tbl, "group", iter_args, this_dir, current_ns, "", error_out, ok);

        if (!ok) {
          return false;
        }

        ts.task_group = qualify_name(current_ns, raw_grp);
        std::unordered_map<std::string, std::string> unit_args = iter_args;

        if (!parse_unit_endpoints(*tbl, "Task '" + ts.name + "'", ts.name, ts.task_group, out.groups, this_dir, ts.ns,
                                  unit_args, ts.urls, ts.inputs, ts.outputs, error_out)) {
          return false;
        }

        const int64_t stage_v = get_i64_range(*tbl, "stage", 0, kMinI32, kMaxI32, error_out, ok);

        if (!ok) {
          error_out.insert(0U, "Task '" + ts.name + "': ");
          return false;
        }

        ts.stage = static_cast<int32_t>(stage_v);
        const std::string kind_str = get_str_strict(*tbl, "kind", unit_args, this_dir, ts.ns, "oneshot", error_out, ok);

        if (!ok) {
          error_out.insert(0U, "Task '" + ts.name + "': ");
          return false;
        }

        if (ascii_iequals(kind_str, "oneshot")) {
          ts.kind = TaskKind::kOneshot;
        } else if (ascii_iequals(kind_str, "periodic")) {
          ts.kind = TaskKind::kPeriodic;
        } else {
          error_out = "Task '" + ts.name + "' has unknown kind '" + kind_str + "'.";
          return false;
        }

        ts.program = get_str_strict(*tbl, "cmd", unit_args, this_dir, ts.ns, "", error_out, ok);

        if (!ok) {
          error_out.insert(0U, "Task '" + ts.name + "': ");
          return false;
        }

        ts.args = get_str_list_strict(*tbl, "args", unit_args, this_dir, ts.ns, error_out, ok);

        if (!ok) {
          error_out.insert(0U, "Task '" + ts.name + "': ");
          return false;
        }

        ts.launch_prefix = get_str_strict(*tbl, "prefix", unit_args, this_dir, ts.ns, "", error_out, ok);

        if (!ok) {
          error_out.insert(0U, "Task '" + ts.name + "': ");
          return false;
        }

        ts.cwd = get_str_strict(*tbl, "cwd", unit_args, this_dir, ts.ns, "", error_out, ok);

        if (!ok) {
          error_out.insert(0U, "Task '" + ts.name + "': ");
          return false;
        }

        ts.inherit_env = get_bool_strict(*tbl, "inherit_env", true, error_out, ok);

        if (!ok) {
          error_out.insert(0U, "Task '" + ts.name + "': ");
          return false;
        }

        ts.condition = parse_condition_node(*tbl, "condition", iter_args, this_dir, ts.ns, error_out, ok, true);

        if (!ok) {
          error_out.insert(0U, "Task '" + ts.name + "': ");
          return false;
        }

        ts.autostart = get_bool_strict(*tbl, "autostart", true, error_out, ok);

        if (!ok) {
          error_out.insert(0U, "Task '" + ts.name + "': ");
          return false;
        }

        ts.required = get_bool_strict(*tbl, "required", false, error_out, ok);

        if (!ok) {
          error_out.insert(0U, "Task '" + ts.name + "': ");
          return false;
        }

        const int64_t delay_v = get_i64_range(*tbl, "delay_ms", 0, 0, kMaxU32, error_out, ok);

        if (!ok) {
          error_out.insert(0U, "Task '" + ts.name + "': ");
          return false;
        }

        ts.delay_ms = static_cast<uint32_t>(delay_v);
        const int64_t iv_v = get_i64_range(*tbl, "interval_ms", 5000, 0, kMaxU32, error_out, ok);

        if (!ok) {
          error_out.insert(0U, "Task '" + ts.name + "': ");
          return false;
        }

        ts.interval_ms = static_cast<uint32_t>(std::max<int64_t>(50, iv_v));
        const int64_t to_v = get_i64_range(*tbl, "timeout_ms", 10000, 0, kMaxU32, error_out, ok);

        if (!ok) {
          error_out.insert(0U, "Task '" + ts.name + "': ");
          return false;
        }

        ts.timeout_ms = static_cast<uint32_t>(std::max<int64_t>(100, to_v));

        if (!parse_exit_code_list_strict(*tbl, "ok_codes", ts.success_exit_codes, error_out)) {
          error_out.insert(0U, "Task '" + ts.name + "': ");
          return false;
        }

        const std::string out_s = get_str_strict(*tbl, "output", iter_args, this_dir, ts.ns, "screen", error_out, ok);

        if (!ok || !parse_output_mode_strict(out_s, ts.output)) {
          if (ok) {
            error_out = "Task '" + ts.name + "' has unknown output mode '" + out_s + "'.";
          }

          return false;
        }

        const std::string km_s = get_str_strict(*tbl, "kill_mode", iter_args, this_dir, ts.ns, "", error_out, ok);

        if (!ok || !parse_kill_mode_strict(km_s, ts.kill_mode)) {
          if (ok) {
            error_out = "Task '" + ts.name + "' has unknown kill_mode '" + km_s + "'.";
          }

          return false;
        }

        if (!parse_resources(*tbl, iter_args, this_dir, ts.ns, ts.resources, error_out)) {
          error_out.insert(0U, "Task '" + ts.name + "': ");
          return false;
        }

        if (!parse_deps_strict(*tbl, unit_args, this_dir, current_ns, ts.deps, error_out)) {
          error_out.insert(0U, "Task '" + ts.name + "': ");
          return false;
        }

        const toml::table* task_env_tbl = nullptr;

        if (!require_table_if_present(*tbl, "env", "Task '" + ts.name + "'", task_env_tbl, error_out)) {
          return false;
        }

        if (task_env_tbl != nullptr && !parse_env_table_strict(*task_env_tbl, "Task '" + ts.name + "'", unit_args,
                                                               this_dir, ts.ns, ts.env, error_out)) {
          return false;
        }

        const toml::table* task_remaps_tbl = nullptr;

        if (!require_table_if_present(*tbl, "remaps", "Task '" + ts.name + "'", task_remaps_tbl, error_out)) {
          return false;
        }

        if (task_remaps_tbl != nullptr &&
            !parse_remaps_table_strict(*task_remaps_tbl, "Task '" + ts.name + "'", unit_args, this_dir, ts.ns,
                                       ts.remaps, error_out)) {
          return false;
        }

        if (!parse_unit_triggers_strict(*tbl, iter_args, this_dir, current_ns, ts.on_start_trigger, ts.on_ready_trigger,
                                        ts.on_exit_trigger, ts.on_error_trigger, error_out)) {
          error_out.insert(0U, "Task '" + ts.name + "': ");
          return false;
        }

        unit_scopes[ts.name] = this_scope_idx;
        out.tasks.push_back(std::move(ts));
      }
    }
  }

  const auto* inc_arr = root["include"].as_array();

  if (inc_arr != nullptr) {
    for (const auto& item : *inc_arr) {
      const auto* tbl = item.as_table();

      if (tbl == nullptr) {
        error_out = "[[include]] entries must be tables.";
        return false;
      }

      if (!validate_allowed_keys(*tbl, "[[include]]", {"path", "namespace", "args", "condition", "for_each", "count"},
                                 error_out)) {
        return false;
      }

      std::vector<std::unordered_map<std::string, std::string>> iter_list;

      if (!expand_for_each_strict(*tbl, local_args, this_dir, current_ns, budget, iter_list, error_out)) {
        return false;
      }

      for (const auto& iter_args : iter_list) {
        bool ok = true;
        const bool inc_cond =
            parse_condition_node(*tbl, "condition", iter_args, this_dir, current_ns, error_out, ok, true);

        if (!ok) {
          error_out.insert(0U, "[[include]]: ");
          return false;
        }

        if (!inc_cond) {
          continue;
        }

        if (++budget.total_includes > kMaxIncludeExpansions) {
          error_out = "Total [[include]] expansions exceeded maximum (" + std::to_string(kMaxIncludeExpansions) + ").";
          return false;
        }

        const std::string raw_path = get_str_strict(*tbl, "path", iter_args, this_dir, current_ns, "", error_out, ok);

        if (!ok) {
          error_out.insert(0U, "[[include]]: ");
          return false;
        }

        if (raw_path.empty()) {
          error_out = "[[include]] entry has empty path.";
          return false;
        }

        std::filesystem::path inc_path(raw_path);

        if (inc_path.is_relative()) {
          inc_path = std::filesystem::path(this_dir) / inc_path;
        }

        std::error_code ec;
        const auto canonical = std::filesystem::weakly_canonical(inc_path, ec);
        const std::string canonical_str = Helpers::path_to_string(ec ? inc_path : canonical);

        if (active_files.count(canonical_str) != 0U) {
          error_out = "Circular [[include]] detected for '" + canonical_str + "'.";
          return false;
        }

        std::unordered_map<std::string, std::string> child_args = iter_args;
        const toml::table* inc_args = nullptr;

        if (!require_table_if_present(*tbl, "args", "[[include]]", inc_args, error_out)) {
          return false;
        }

        if (inc_args != nullptr) {
          for (const auto& [k, v] : *inc_args) {
            const auto* is = v.as_string();
            const auto* ii = v.as_integer();
            const auto* ifp = v.as_floating_point();
            const auto* ib = v.as_boolean();

            if (is != nullptr) {
              child_args[std::string(k.str())] = substitute_vars(is->get(), iter_args, this_dir, current_ns);
            } else if (ii != nullptr) {
              child_args[std::string(k.str())] = std::to_string(ii->get());
            } else if (ifp != nullptr) {
              char buf[64];
              const size_t n = Helpers::format_floating_spec_to(buf, sizeof(buf) - 1U, ifp->get(), 'g', 17, false);
              child_args[std::string(k.str())].assign(buf, n);
            } else if (ib != nullptr) {
              child_args[std::string(k.str())] = ib->get() ? "true" : "false";
            } else {
              error_out = "[[include]].args." + std::string(k.str()) + " must be a scalar value.";
              return false;
            }
          }
        }

        const std::string raw_inc_ns =
            get_str_strict(*tbl, "namespace", iter_args, this_dir, current_ns, "", error_out, ok);

        if (!ok) {
          error_out.insert(0U, "[[include]]: ");
          return false;
        }

        const std::string inc_ns = join_namespace(current_ns, raw_inc_ns);
        const std::string inc_dir = Helpers::path_to_string(inc_path.parent_path());

        try {
          const auto parsed = toml::parse_file(canonical_str);
          active_files.insert(canonical_str);
          const bool inc_ok =
              parse_table_recursive(parsed, inc_dir.empty() ? "." : inc_dir, inc_ns, child_args, file_scope, scopes,
                                    unit_scopes, active_files, budget, depth + 1U, false, out, error_out);
          active_files.erase(canonical_str);

          if (!inc_ok) {
            return false;
          }
        } catch (const toml::parse_error& err) {
          error_out = "Failed to parse included file '" + canonical_str + "': " + std::string(err.description());
          return false;
        } catch (const std::exception& err) {
          error_out = "Failed to include file '" + canonical_str + "': " + err.what();
          return false;
        }
      }
    }
  }

  return true;
}

static void apply_group_inheritance(LaunchManifest& manifest, const std::vector<LexicalScope>& scopes,
                                    const std::unordered_map<std::string, size_t>& unit_scopes) {
  std::unordered_map<std::string, GroupConfig*> group_map;
  std::unordered_map<std::string, std::string> member_to_group;

  for (auto& g : manifest.groups) {
    if (!g.name.empty()) {
      group_map[g.name] = &g;

      for (const auto& m : g.members) {
        if (!m.empty() && member_to_group.count(m) == 0U) {
          member_to_group[m] = g.name;
        }
      }
    }
  }

  auto assign_explicit_group = [&](const std::string& unit_name, std::string& task_group) {
    if (task_group.empty()) {
      auto it = member_to_group.find(unit_name);

      if (it != member_to_group.end()) {
        task_group = it->second;
      }
    }
  };

  if (manifest.proxy.enabled) {
    assign_explicit_group(manifest.proxy.name, manifest.proxy.task_group);
  }

  for (auto& ps : manifest.processes) {
    assign_explicit_group(ps.name, ps.task_group);
  }

  for (auto& cs : manifest.containers) {
    assign_explicit_group(cs.name, cs.task_group);
  }

  for (auto& cs : manifest.components) {
    assign_explicit_group(cs.name, cs.task_group);
  }

  for (auto& ts : manifest.tasks) {
    assign_explicit_group(ts.name, ts.task_group);
  }

  auto scope_for = [&](const std::string& unit_name) -> const LexicalScope* {
    auto it = unit_scopes.find(unit_name);

    if (it != unit_scopes.end() && it->second < scopes.size()) {
      return &scopes[it->second];
    }

    return scopes.empty() ? nullptr : &scopes.front();
  };

  auto register_member = [&](const std::string& unit_name, const std::string& grp_name) {
    if (grp_name.empty()) {
      return;
    }

    auto it = group_map.find(grp_name);

    if (it != group_map.end()) {
      auto& members = it->second->members;

      if (std::find(members.begin(), members.end(), unit_name) == members.end()) {
        members.push_back(unit_name);
      }
    }
  };

  auto apply_to_unit_env_res = [&](const std::string& grp_name, std::unordered_map<std::string, std::string>& env,
                                   ResourceLimits& res, bool& cond, bool& autostart) {
    auto it = group_map.find(grp_name);

    if (it == group_map.end()) {
      return;
    }

    const auto& g = *it->second;

    if (!g.condition) {
      cond = false;
    }

    if (!g.autostart) {
      autostart = false;
    }

    if (res.slice.empty() && !g.slice.empty()) {
      res.slice = g.slice;
    }

    for (const auto& [k, v] : g.env) {
      if (env.count(k) == 0U) {
        env[k] = v;
      }
    }
  };

  auto apply_scope_env = [&](const LexicalScope* sc, std::unordered_map<std::string, std::string>& env) {
    if (sc == nullptr) {
      return;
    }

    for (const auto& [k, v] : sc->env) {
      if (env.count(k) == 0U) {
        env[k] = v;
      }
    }
  };

  auto apply_layered_remaps = [&](const LexicalScope* sc,
                                  const std::vector<std::pair<std::string, std::string>>* group_remaps,
                                  std::vector<std::pair<std::string, std::string>>& unit_remaps) {
    const bool has_scope = (sc != nullptr && !sc->remaps.empty());
    const bool has_group = (group_remaps != nullptr && !group_remaps->empty());

    if (!has_scope && !has_group) {
      return;
    }

    std::vector<std::pair<std::string, std::string>> merged;

    if (has_scope) {
      merged = sc->remaps;
    }

    if (has_group) {
      merge_remaps(*group_remaps, merged);
    }

    merge_remaps(unit_remaps, merged);
    unit_remaps = std::move(merged);
  };

  auto inherit_urls = [](const std::unordered_map<std::string, std::string>& src,
                         std::unordered_map<std::string, std::string>& urls,
                         const std::unordered_map<std::string, std::string>& inputs,
                         const std::unordered_map<std::string, std::string>& outputs) {
    for (const auto& [k, v] : src) {
      if (urls.count(k) == 0U && inputs.count(k) == 0U && outputs.count(k) == 0U) {
        urls[k] = v;
      }
    }
  };

  auto inherit_group_ns = [](const GroupConfig& g, std::string& unit_ns) {
    if (g.ns.empty()) {
      return;
    }

    if (unit_ns.empty() || (g.ns.size() > unit_ns.size() && Helpers::has_startwith(g.ns, unit_ns + "/"))) {
      unit_ns = g.ns;
    }
  };

  if (manifest.proxy.enabled) {
    register_member(manifest.proxy.name, manifest.proxy.task_group);

    if (!manifest.proxy.task_group.empty()) {
      apply_to_unit_env_res(manifest.proxy.task_group, manifest.proxy.env, manifest.proxy.resources,
                            manifest.proxy.condition, manifest.proxy.autostart);
    }

    apply_scope_env(scope_for(manifest.proxy.name), manifest.proxy.env);
  }

  for (auto& ps : manifest.processes) {
    register_member(ps.name, ps.task_group);
    const std::vector<std::pair<std::string, std::string>>* g_remaps = nullptr;

    if (!ps.task_group.empty()) {
      apply_to_unit_env_res(ps.task_group, ps.env, ps.resources, ps.condition, ps.autostart);
      auto it = group_map.find(ps.task_group);

      if (it != group_map.end()) {
        const auto& g = *it->second;
        inherit_group_ns(g, ps.ns);
        g_remaps = &g.remaps;
        inherit_urls(g.urls, ps.urls, ps.inputs, ps.outputs);
      }
    }

    const LexicalScope* sc = scope_for(ps.name);
    apply_scope_env(sc, ps.env);
    apply_layered_remaps(sc, g_remaps, ps.remaps);

    if (sc != nullptr) {
      inherit_urls(sc->urls, ps.urls, ps.inputs, ps.outputs);
    }
  }

  std::unordered_map<std::string, const ContainerSpec*> container_map;

  for (auto& cs : manifest.containers) {
    register_member(cs.name, cs.task_group);

    if (!cs.task_group.empty()) {
      apply_to_unit_env_res(cs.task_group, cs.env, cs.resources, cs.condition, cs.autostart);
      auto it = group_map.find(cs.task_group);

      if (it != group_map.end()) {
        inherit_group_ns(*it->second, cs.ns);
      }
    }

    apply_scope_env(scope_for(cs.name), cs.env);
    container_map[cs.name] = &cs;
  }

  for (auto& ts : manifest.tasks) {
    register_member(ts.name, ts.task_group);
    const std::vector<std::pair<std::string, std::string>>* g_remaps = nullptr;

    if (!ts.task_group.empty()) {
      apply_to_unit_env_res(ts.task_group, ts.env, ts.resources, ts.condition, ts.autostart);
      auto it = group_map.find(ts.task_group);

      if (it != group_map.end()) {
        const auto& g = *it->second;
        inherit_group_ns(g, ts.ns);
        g_remaps = &g.remaps;
        inherit_urls(g.urls, ts.urls, ts.inputs, ts.outputs);
      }
    }

    const LexicalScope* sc = scope_for(ts.name);
    apply_scope_env(sc, ts.env);
    apply_layered_remaps(sc, g_remaps, ts.remaps);

    if (sc != nullptr) {
      inherit_urls(sc->urls, ts.urls, ts.inputs, ts.outputs);
    }
  }

  for (auto& cs : manifest.components) {
    register_member(cs.name, cs.task_group);
    const std::vector<std::pair<std::string, std::string>>* g_remaps = nullptr;

    if (!cs.task_group.empty()) {
      auto it = group_map.find(cs.task_group);

      if (it != group_map.end()) {
        const auto& g = *it->second;
        inherit_group_ns(g, cs.ns);
        g_remaps = &g.remaps;

        if (!g.condition) {
          cs.condition = false;
        }

        if (!g.autostart) {
          cs.autostart = false;
        }

        for (const auto& [k, v] : g.params) {
          if (cs.params.count(k) == 0U) {
            cs.params[k] = v;
          }
        }

        inherit_urls(g.urls, cs.urls, cs.inputs, cs.outputs);
      }
    }

    if (!cs.container.empty()) {
      auto cit = container_map.find(cs.container);

      if (cit != container_map.end() && !cit->second->condition) {
        cs.condition = false;
      }
    }

    const LexicalScope* sc = scope_for(cs.name);

    if (sc != nullptr) {
      for (const auto& [k, v] : sc->params) {
        if (cs.params.count(k) == 0U) {
          cs.params[k] = v;
        }
      }
    }

    apply_layered_remaps(sc, g_remaps, cs.remaps);

    if (sc != nullptr) {
      inherit_urls(sc->urls, cs.urls, cs.inputs, cs.outputs);
    }
  }
}

bool parse_manifest_file(const std::string& file_path,
                         const std::unordered_map<std::string, std::string>& arg_overrides, LaunchManifest& out,
                         std::string* error_out) {
  out = LaunchManifest{};
  std::error_code ec;
  const auto canonical = std::filesystem::weakly_canonical(file_path, ec);
  const std::string canonical_str = Helpers::path_to_string(ec ? std::filesystem::path(file_path) : canonical);
  out.source_file = canonical_str;

  std::string err;

  try {
    const auto parsed = toml::parse_file(canonical_str);
    std::set<std::string> active{canonical_str};
    const std::string dir = Helpers::path_to_string(std::filesystem::path(canonical_str).parent_path());
    LexicalScope root_scope;
    std::vector<LexicalScope> scopes;
    std::unordered_map<std::string, size_t> unit_scopes;
    ParseBudget budget;

    if (!parse_table_recursive(parsed, dir.empty() ? "." : dir, "", arg_overrides, root_scope, scopes, unit_scopes,
                               active, budget, 0U, true, out, err)) {
      if (error_out != nullptr) {
        *error_out = err;
      }

      return false;
    }

    apply_group_inheritance(out, scopes, unit_scopes);
    return true;
  } catch (const toml::parse_error& e) {
    if (error_out != nullptr) {
      *error_out = "TOML parse error in '" + canonical_str + "': " + std::string(e.description());
    }

    return false;
  } catch (const std::exception& e) {
    if (error_out != nullptr) {
      *error_out = "Failed to load manifest '" + canonical_str + "': " + e.what();
    }

    return false;
  }
}

bool parse_manifest_string(std::string_view toml_content, const std::string& base_dir,
                           const std::unordered_map<std::string, std::string>& arg_overrides, LaunchManifest& out,
                           std::string* error_out) {
  out = LaunchManifest{};
  std::string err;

  try {
    const auto parsed = toml::parse(toml_content);
    std::set<std::string> active;
    LexicalScope root_scope;
    std::vector<LexicalScope> scopes;
    std::unordered_map<std::string, size_t> unit_scopes;
    ParseBudget budget;

    if (!parse_table_recursive(parsed, base_dir.empty() ? "." : base_dir, "", arg_overrides, root_scope, scopes,
                               unit_scopes, active, budget, 0U, true, out, err)) {
      if (error_out != nullptr) {
        *error_out = err;
      }

      return false;
    }

    apply_group_inheritance(out, scopes, unit_scopes);
    return true;
  } catch (const toml::parse_error& e) {
    if (error_out != nullptr) {
      *error_out = "TOML parse error: " + std::string(e.description());
    }

    return false;
  } catch (const std::exception& e) {
    if (error_out != nullptr) {
      *error_out = std::string("Failed to parse manifest: ") + e.what();
    }

    return false;
  }
}

std::vector<DataflowEdge> compute_dataflow_edges(const LaunchManifest& manifest) {
  struct Endpoint final {
    std::string unit;
    std::string key;
    std::string url;
  };

  std::vector<Endpoint> producers;
  std::vector<Endpoint> consumers;

  auto normalize_edge_url = [](const std::string& url) -> std::string {
    try {
      const UrlParser parser(url);
      const std::string& transport = parser.get_transport();

      if (transport.empty()) {
        return url;
      }

      std::string out = transport;
      out.append("://");

      if (!parser.get_host().empty()) {
        out.append(parser.get_host());

        if (parser.get_port() > 0) {
          out.push_back(':');
          out.append(std::to_string(parser.get_port()));
        }

        if (!parser.get_path().empty()) {
          out.push_back('/');
          out.append(parser.get_path());
        }
      } else if (!parser.get_path().empty()) {
        out.append(parser.get_path());
      }

      const auto& qdict = parser.get_query_dictionary();
      bool has_q = false;
      const auto dom_it = qdict.find("domain");

      if (dom_it != qdict.end() && !dom_it->second.empty() && dom_it->second != "0") {
        out.append("?domain=").append(dom_it->second);
        has_q = true;
      }

      const auto ev_it = qdict.find("event");

      if (ev_it != qdict.end() && !ev_it->second.empty()) {
        out.push_back(has_q ? '&' : '?');
        out.append("event=").append(ev_it->second);
        has_q = true;
      }

      if (transport == "someip") {
        const auto m_it = qdict.find("method");

        if (m_it != qdict.end() && !m_it->second.empty()) {
          out.push_back(has_q ? '&' : '?');
          out.append("method=").append(m_it->second);
          has_q = true;
        }

        const auto g_it = qdict.find("groups");

        if (g_it != qdict.end() && !g_it->second.empty()) {
          out.push_back(has_q ? '&' : '?');
          out.append("groups=").append(g_it->second);
        }
      }

      return out;
    } catch (const std::exception&) {
      return url;
    }
  };

  auto collect = [&](const std::string& unit_name, bool enabled, const std::string& ns,
                     const std::vector<std::pair<std::string, std::string>>& remaps,
                     const std::unordered_map<std::string, std::string>& inputs,
                     const std::unordered_map<std::string, std::string>& outputs) {
    if (!enabled) {
      return;
    }

    for (const auto& [k, v] : outputs) {
      if (!v.empty()) {
        producers.push_back(Endpoint{unit_name, k, normalize_edge_url(resolve_unit_url(v, ns, remaps))});
      }
    }

    for (const auto& [k, v] : inputs) {
      if (!v.empty()) {
        consumers.push_back(Endpoint{unit_name, k, normalize_edge_url(resolve_unit_url(v, ns, remaps))});
      }
    }
  };

  for (const auto& cs : manifest.components) {
    collect(cs.name, cs.condition, cs.ns, cs.remaps, cs.inputs, cs.outputs);
  }

  for (const auto& ps : manifest.processes) {
    collect(ps.name, ps.condition, ps.ns, ps.remaps, ps.inputs, ps.outputs);
  }

  for (const auto& ts : manifest.tasks) {
    collect(ts.name, ts.condition, ts.ns, ts.remaps, ts.inputs, ts.outputs);
  }

  std::unordered_map<std::string, std::vector<size_t>> consumers_by_url;
  consumers_by_url.reserve(consumers.size());

  for (size_t i = 0U; i < consumers.size(); ++i) {
    consumers_by_url[consumers[i].url].push_back(i);
  }

  std::vector<DataflowEdge> edges;

  for (const auto& prod : producers) {
    auto it = consumers_by_url.find(prod.url);

    if (it == consumers_by_url.end()) {
      continue;
    }

    for (const size_t cidx : it->second) {
      const auto& cons = consumers[cidx];

      if (prod.unit != cons.unit) {
        edges.push_back(DataflowEdge{prod.unit, cons.unit, prod.key, cons.key, prod.url});
      }
    }
  }

  std::sort(edges.begin(), edges.end(), [](const DataflowEdge& a, const DataflowEdge& b) {
    if (a.from != b.from) {
      return a.from < b.from;
    }

    if (a.to != b.to) {
      return a.to < b.to;
    }

    if (a.url != b.url) {
      return a.url < b.url;
    }

    if (a.output_key != b.output_key) {
      return a.output_key < b.output_key;
    }

    return a.input_key < b.input_key;
  });

  edges.erase(std::unique(edges.begin(), edges.end(),
                          [](const DataflowEdge& a, const DataflowEdge& b) {
                            return a.from == b.from && a.to == b.to && a.output_key == b.output_key &&
                                   a.input_key == b.input_key && a.url == b.url;
                          }),
              edges.end());

  return edges;
}

static bool program_exists_on_host(const std::string& prog, const std::string& cwd = "") {
  if (prog.empty()) {
    return false;
  }

  std::error_code ec;
  std::filesystem::path p(prog);

  if (p.is_relative() && !cwd.empty() &&
      (prog.find('/') != std::string::npos || prog.find('\\') != std::string::npos)) {
    p = std::filesystem::path(cwd) / p;
  }

  auto is_executable_file = [&](const std::filesystem::path& candidate) -> bool {
    if (!std::filesystem::is_regular_file(candidate, ec)) {
      return false;
    }

#if defined(_WIN32) || defined(_WIN64)
    return true;
#else
    return ::access(candidate.c_str(), X_OK) == 0;
#endif
  };

  if (p.is_absolute() || prog.find('/') != std::string::npos || prog.find('\\') != std::string::npos) {
    if (is_executable_file(p)) {
      return true;
    }

#if defined(_WIN32) || defined(_WIN64)
    if (!p.has_extension() && is_executable_file(std::filesystem::path(Helpers::path_to_string(p) + ".exe"))) {
      return true;
    }
#endif

    return false;
  }

  const std::string path_env = Utils::get_env("PATH");
#if defined(_WIN32) || defined(_WIN64)
  const char sep = ';';
#else
  const char sep = ':';
#endif

  for (const auto& dir : Helpers::split_view(path_env, sep)) {
    const std::filesystem::path candidate = std::filesystem::path(dir) / p;

    if (is_executable_file(candidate)) {
      return true;
    }

#if defined(_WIN32) || defined(_WIN64)
    if (is_executable_file(std::filesystem::path(dir) / (prog + ".exe"))) {
      return true;
    }
#endif
  }

  return false;
}

LauncherServer::ValidationReport validate_manifest(const LaunchManifest& manifest, bool check_programs) {
  LauncherServer::ValidationReport rep;
  rep.scheduler_group_count = manifest.schedulers.size();
  rep.slice_count = manifest.slices.size();
  rep.group_count = manifest.groups.size();
  rep.process_count = manifest.processes.size();
  rep.component_count = manifest.components.size();
  rep.container_count = manifest.containers.size();
  rep.task_count = manifest.tasks.size();
  rep.has_proxy = manifest.proxy.enabled;

  if (!is_supported_ctrl_scheme(manifest.control_scheme)) {
    rep.errors.push_back("[launcher].ctrl_scheme '" + manifest.control_scheme +
                         "' is invalid (expected dds|ddsc|ddsr|zenoh|intra).");
  }

  std::unordered_set<std::string> sched_names{"default"};
  std::unordered_map<std::string, SchedulerMode> sched_modes{{"default", SchedulerMode::kShared}};
  bool saw_explicit_default = false;

  for (const auto& sc : manifest.schedulers) {
    if (sc.name.empty()) {
      rep.errors.emplace_back("[[scheduler]] entry has empty name.");
    } else if (sc.name == "default") {
      if (saw_explicit_default) {
        rep.errors.emplace_back("Duplicate scheduler group 'default'.");
      }

      saw_explicit_default = true;
      sched_modes["default"] = sc.mode;
    } else if (!sched_names.insert(sc.name).second) {
      rep.errors.push_back("Duplicate scheduler group '" + sc.name + "'.");
    } else {
      sched_modes[sc.name] = sc.mode;
    }

    if (sc.mode == SchedulerMode::kThreadPool && sc.queue_type == MessageLoop::kPriorityType) {
      rep.warnings.push_back("Scheduler '" + sc.name +
                             "' uses mode='pool' with queue='priority'; ThreadPool uses normal FIFO queue "
                             "(use mode='multi' for priority queue).");
    }
  }

  std::unordered_set<std::string> slice_names;

  for (const auto& sl : manifest.slices) {
    if (sl.name.empty()) {
      rep.errors.emplace_back("[[slice]] entry has empty name.");
    } else if (!slice_names.insert(sl.name).second) {
      rep.errors.push_back("Duplicate slice '" + sl.name + "'.");
    }
  }

  std::unordered_map<std::string, const GroupConfig*> groups_by_name;

  for (const auto& gc : manifest.groups) {
    if (gc.name.empty()) {
      rep.errors.emplace_back("[[group]] entry has empty name.");
    } else if (!groups_by_name.emplace(gc.name, &gc).second) {
      rep.errors.push_back("Duplicate group '" + gc.name + "'.");
    }

    if (!gc.slice.empty() && slice_names.count(gc.slice) == 0U) {
      rep.errors.push_back("Group '" + gc.name + "' references unknown slice '" + gc.slice + "'.");
    }
  }

  std::unordered_set<std::string> unit_names;
  std::unordered_map<std::string, int32_t> container_stages;
  std::unordered_map<std::string, bool> unit_autostart_active;

  auto register_unit = [&](const std::string& name, const char* kind, bool active) {
    if (name.empty()) {
      rep.errors.push_back(std::string(kind) + " entry has empty name.");
      return;
    }

    if (!unit_names.insert(name).second) {
      rep.errors.push_back("Duplicate unit name '" + name + "'.");
      return;
    }

    unit_autostart_active[name] = active;
  };

  auto check_group_ref = [&](const std::string& unit, const std::string& task_group) {
    if (!task_group.empty() && groups_by_name.count(task_group) == 0U) {
      rep.errors.push_back("Unit '" + unit + "' references unknown group '" + task_group + "'.");
    }
  };

  auto check_exit_codes = [&](const std::string& unit, const std::vector<int32_t>& codes, const char* field) {
    for (const int32_t c : codes) {
      if (c < 0 || c > 255) {
        rep.errors.push_back("Unit '" + unit + "' " + field + " contains out-of-range exit code " + std::to_string(c) +
                             " (expected 0..255).");
      }
    }
  };

  auto check_slice_ref = [&](const std::string& unit, const ResourceLimits& res) {
    if (!res.slice.empty() && slice_names.count(res.slice) == 0U) {
      rep.errors.push_back("Unit '" + unit + "' references unknown slice '" + res.slice + "'.");
    }
  };

  auto check_prefix_syntax = [&](const std::string& unit, const std::string& prefix) {
    if (prefix.empty()) {
      return;
    }

    std::vector<std::string> tokens;
    std::string perr;

    if (!split_command_prefix(prefix, tokens, &perr)) {
      rep.errors.push_back("Unit '" + unit + "': " + perr);
    }
  };

  auto check_probes = [&](const std::string& unit, bool is_component, const ReadyProbeConfig& ready,
                          const LivenessProbeConfig& liveness) {
    if (ready.type == ReadyProbeType::kComponent && !is_component) {
      rep.errors.push_back("Unit '" + unit + "' cannot use ready.type='component' (supported for components only).");
    }

    if (ready.type == ReadyProbeType::kStdout && ready.pattern.empty()) {
      rep.errors.push_back("Unit '" + unit + "' ready.type='stdout' requires a non-empty 'pattern'.");
    }

    if (ready.type == ReadyProbeType::kFile && ready.path.empty()) {
      rep.errors.push_back("Unit '" + unit + "' ready.type='file' requires a non-empty 'path'.");
    }

    if (ready.type == ReadyProbeType::kTopic && ready.topic.empty()) {
      rep.errors.push_back("Unit '" + unit + "' ready.type='topic' requires a non-empty 'topic'.");
    }

    if (liveness.enabled) {
      if (liveness.type == ReadyProbeType::kFile && liveness.path.empty()) {
        rep.errors.push_back("Unit '" + unit + "' liveness.type='file' requires a non-empty 'path'.");
      }

      if (liveness.type == ReadyProbeType::kTopic && liveness.topic.empty()) {
        rep.errors.push_back("Unit '" + unit + "' liveness.type='topic' requires a non-empty 'topic'.");
      }
    }
  };

  auto check_remaps = [&](const std::string& unit, const std::vector<std::pair<std::string, std::string>>& remaps) {
    for (const auto& [from, to] : remaps) {
      if (from.size() > kMaxRemapUrlBytes || to.size() > kMaxRemapUrlBytes) {
        rep.warnings.push_back("Unit '" + unit + "' remap URL exceeds VLink 120-byte UrlRemap limit.");
      }
    }
  };

  auto check_url_map = [&](const std::string& owner, const char* kind,
                           const std::unordered_map<std::string, std::string>& map) {
    for (const auto& [k, v] : map) {
      if (!is_valid_vlink_url(v)) {
        std::string msg = owner;
        msg.append(" ")
            .append(kind)
            .append(" '")
            .append(k)
            .append("' has invalid VLink URL '")
            .append(v)
            .append("' (expected scheme://...).");
        rep.errors.push_back(std::move(msg));
      } else if (v.size() > kMaxRemapUrlBytes) {
        std::string msg = owner;
        msg.append(" ").append(kind).append(" '").append(k).append("' URL exceeds VLink 120-byte URL length limit.");
        rep.warnings.push_back(std::move(msg));
      }
    }
  };

  check_url_map("[urls]", "entry", manifest.global_urls);

  if (manifest.proxy.enabled) {
    register_unit(manifest.proxy.name, "[proxy]", manifest.proxy.condition && manifest.proxy.autostart);
    check_group_ref(manifest.proxy.name, manifest.proxy.task_group);
    check_slice_ref(manifest.proxy.name, manifest.proxy.resources);
    check_exit_codes(manifest.proxy.name, manifest.proxy.respawn.success_exit_codes, "ok_codes");
    check_exit_codes(manifest.proxy.name, manifest.proxy.respawn.restart_prevent_exit_codes, "no_restart_codes");

    if (manifest.proxy.reload_mode == ReloadMode::kComponent) {
      rep.errors.push_back("Proxy '" + manifest.proxy.name + "' cannot use reload='component' (use 'sighup').");
    }

    if (check_programs && !program_exists_on_host(manifest.proxy.program)) {
      rep.warnings.push_back("Proxy program '" + manifest.proxy.program + "' was not found in PATH.");
    }
  }

  for (const auto& cs : manifest.containers) {
    register_unit(cs.name, "[[container]]", cs.condition && cs.autostart);
    container_stages[cs.name] = cs.stage;
    check_group_ref(cs.name, cs.task_group);
    check_slice_ref(cs.name, cs.resources);
    check_prefix_syntax(cs.name, cs.launch_prefix);
    check_exit_codes(cs.name, cs.respawn.success_exit_codes, "ok_codes");
    check_exit_codes(cs.name, cs.respawn.restart_prevent_exit_codes, "no_restart_codes");
  }

  std::unordered_map<std::string, std::string> comp_owner;

  for (const auto& cs : manifest.components) {
    register_unit(cs.name, "[[component]]", cs.condition && cs.autostart);
    check_group_ref(cs.name, cs.task_group);

    if (cs.library.empty()) {
      rep.errors.push_back("Component '" + cs.name + "' has empty lib.");
    }

    if (!cs.scheduler_group.empty() && sched_names.count(cs.scheduler_group) == 0U) {
      rep.errors.push_back("Component '" + cs.name + "' references unknown scheduler '" + cs.scheduler_group + "'.");
    }

    if (!cs.compute_pool.empty()) {
      auto sit = sched_modes.find(cs.compute_pool);

      if (sit == sched_modes.end()) {
        rep.errors.push_back("Component '" + cs.name + "' references unknown pool '" + cs.compute_pool + "'.");
      } else if (sit->second != SchedulerMode::kThreadPool) {
        rep.errors.push_back("Component '" + cs.name + "' pool '" + cs.compute_pool +
                             "' must reference a scheduler with mode='pool'.");
      }
    }

    if (!cs.container.empty()) {
      auto cit = container_stages.find(cs.container);

      if (cit == container_stages.end()) {
        rep.errors.push_back("Component '" + cs.name + "' references unknown container '" + cs.container + "'.");
      } else {
        comp_owner.emplace(cs.name, cs.container);

        if (cs.stage > cit->second) {
          rep.warnings.push_back("Component '" + cs.name + "' has stage=" + std::to_string(cs.stage) +
                                 " higher than its container '" + cs.container +
                                 "' stage=" + std::to_string(cit->second) +
                                 "; components inside a container start according to container-local stages.");
        }
      }
    }

    if (cs.legacy_runnable && cs.watchdog_ms > 0U) {
      rep.errors.push_back("Component '" + cs.name + "' with legacy=true does not support watchdog_ms.");
    }

    if (cs.ready.type == ReadyProbeType::kStdout) {
      rep.errors.push_back("Component '" + cs.name +
                           "' cannot use ready.type='stdout' (supported for processes and tasks only).");
    }

    if (cs.legacy_runnable && cs.reload_mode == ReloadMode::kComponent) {
      rep.errors.push_back("Component '" + cs.name + "' with legacy=true does not support reload='component'.");
    }

    if (cs.reload_mode == ReloadMode::kSigHup) {
      rep.errors.push_back("Component '" + cs.name + "' cannot use reload='sighup' (use 'component').");
    }

    if (cs.legacy_runnable && cs.start_inactive) {
      rep.errors.push_back("Component '" + cs.name + "' cannot combine legacy=true with init_state='inactive'.");
    }

    check_probes(cs.name, true, cs.ready, cs.liveness);
    check_remaps(cs.name, cs.remaps);
    check_url_map("Component '" + cs.name + "'", "urls", cs.urls);
    check_url_map("Component '" + cs.name + "'", "inputs", cs.inputs);
    check_url_map("Component '" + cs.name + "'", "outputs", cs.outputs);
  }

  for (const auto& ps : manifest.processes) {
    register_unit(ps.name, "[[process]]", ps.condition && ps.autostart);
    check_group_ref(ps.name, ps.task_group);

    if (ps.program.empty()) {
      rep.errors.push_back("Process '" + ps.name + "' has empty cmd.");
    } else if (check_programs && !program_exists_on_host(ps.program, ps.cwd)) {
      rep.errors.push_back("Process '" + ps.name + "' cmd '" + ps.program + "' not found.");
    }

    if (ps.reload_mode == ReloadMode::kComponent) {
      rep.errors.push_back("Process '" + ps.name + "' cannot use reload='component' (use 'sighup').");
    }

    check_slice_ref(ps.name, ps.resources);
    check_prefix_syntax(ps.name, ps.launch_prefix);
    check_probes(ps.name, false, ps.ready, ps.liveness);
    check_exit_codes(ps.name, ps.respawn.success_exit_codes, "ok_codes");
    check_exit_codes(ps.name, ps.respawn.restart_prevent_exit_codes, "no_restart_codes");
    check_remaps(ps.name, ps.remaps);
    check_url_map("Process '" + ps.name + "'", "urls", ps.urls);
    check_url_map("Process '" + ps.name + "'", "inputs", ps.inputs);
    check_url_map("Process '" + ps.name + "'", "outputs", ps.outputs);
  }

  for (const auto& ts : manifest.tasks) {
    register_unit(ts.name, "[[task]]", ts.condition && ts.autostart);
    check_group_ref(ts.name, ts.task_group);

    if (ts.program.empty()) {
      rep.errors.push_back("Task '" + ts.name + "' has empty cmd.");
    } else if (check_programs && !program_exists_on_host(ts.program, ts.cwd)) {
      rep.errors.push_back("Task '" + ts.name + "' cmd '" + ts.program + "' not found.");
    }

    check_slice_ref(ts.name, ts.resources);
    check_prefix_syntax(ts.name, ts.launch_prefix);
    check_exit_codes(ts.name, ts.success_exit_codes, "ok_codes");
    check_remaps(ts.name, ts.remaps);
    check_url_map("Task '" + ts.name + "'", "urls", ts.urls);
    check_url_map("Task '" + ts.name + "'", "inputs", ts.inputs);
    check_url_map("Task '" + ts.name + "'", "outputs", ts.outputs);
  }

  if (unit_names.size() > kMaxUnitCount) {
    rep.errors.push_back("Manifest defines " + std::to_string(unit_names.size()) +
                         " units, exceeding the maximum supported limit of " + std::to_string(kMaxUnitCount) + ".");
  }

  auto expand_ref = [&](const std::string& owner, const std::string& ref, const char* field,
                        std::vector<std::string>& out_units) {
    if (ref.empty()) {
      return;
    }

    if (ref[0] == '@') {
      const std::string gname = ref.substr(1U);
      auto git = groups_by_name.find(gname);

      if (git == groups_by_name.end()) {
        rep.errors.push_back("Unit '" + owner + "' " + field + " references unknown group '" + ref + "'.");
        return;
      }

      for (const auto& m : git->second->members) {
        if (m != owner) {
          out_units.push_back(m);
        }
      }

      return;
    }

    if (unit_names.count(ref) == 0U) {
      rep.errors.push_back("Unit '" + owner + "' " + field + " references unknown unit '" + ref + "'.");
      return;
    }

    if (ref == owner) {
      rep.errors.push_back("Unit '" + owner + "' " + field + " cannot reference itself.");
      return;
    }

    out_units.push_back(ref);
  };

  auto check_triggers = [&](const std::string& owner, const std::vector<std::string>& on_start,
                            const std::vector<std::string>& on_ready, const std::vector<std::string>& on_exit,
                            const std::vector<std::string>& on_error) {
    std::vector<std::string> targets;

    for (const auto& r : on_start) {
      targets.clear();
      expand_ref(owner, r, "on_start", targets);
    }

    for (const auto& r : on_ready) {
      targets.clear();
      expand_ref(owner, r, "on_ready", targets);
    }

    for (const auto& r : on_exit) {
      targets.clear();
      expand_ref(owner, r, "on_exit_trigger", targets);
    }

    for (const auto& r : on_error) {
      targets.clear();
      expand_ref(owner, r, "on_error", targets);
    }
  };

  std::unordered_map<std::string, std::vector<std::string>> adj;
  std::unordered_map<std::string, size_t> indegree;
  std::unordered_map<std::string, int32_t> unit_stage;

  for (const auto& name : unit_names) {
    adj[name] = {};
    indegree[name] = 0U;
  }

  if (manifest.proxy.enabled) {
    unit_stage[manifest.proxy.name] = manifest.proxy.stage;
  }

  for (const auto& cs : manifest.containers) {
    unit_stage[cs.name] = cs.stage;
  }

  for (const auto& cs : manifest.components) {
    if (cs.container.empty()) {
      unit_stage[cs.name] = cs.stage;
    }
  }

  for (const auto& ps : manifest.processes) {
    unit_stage[ps.name] = ps.stage;
  }

  for (const auto& ts : manifest.tasks) {
    unit_stage[ts.name] = ts.stage;
  }

  std::set<std::pair<std::string, std::string>> added_edges;

  auto add_order_edge = [&](const std::string& pred, const std::string& succ) {
    if (pred == succ || adj.count(pred) == 0U || adj.count(succ) == 0U) {
      return;
    }

    if (!added_edges.emplace(pred, succ).second) {
      return;
    }

    adj[pred].push_back(succ);
    ++indegree[succ];
  };

  auto validate_deps = [&](const std::string& name, const DependencySpec& deps) {
    const bool owner_is_host = (comp_owner.count(name) == 0U);
    std::vector<std::string> targets;

    auto check_list = [&](const std::vector<std::string>& list, const char* field, bool is_after, bool is_before,
                          bool is_conflict, bool observes_remote_ready) {
      for (const auto& ref : list) {
        targets.clear();
        expand_ref(name, ref, field, targets);

        for (const auto& t : targets) {
          const auto t_owner = comp_owner.find(t);
          const bool cross_owner_remote =
              (t_owner != comp_owner.end()) && (owner_is_host || comp_owner[name] != t_owner->second);

          if (cross_owner_remote && observes_remote_ready && !manifest.control_enabled) {
            const std::string_view field_sv(field);
            std::string err_msg;
            err_msg.reserve(name.size() + field_sv.size() + t.size() + 100U);
            err_msg.append("Unit '")
                .append(name)
                .append("' ")
                .append(field_sv)
                .append(" references container component '")
                .append(t)
                .append("', which requires [launcher].ctrl = true to observe component readiness.");
            rep.errors.push_back(std::move(err_msg));
          }

          if (is_after) {
            add_order_edge(t, name);
          }

          if (is_before) {
            add_order_edge(name, t);
          }

          if (is_conflict && unit_autostart_active[name] && unit_autostart_active[t]) {
            std::string msg = "Conflicting autostart units: '";
            msg.append(name).append("' conflicts with '").append(t).append("'.");
            rep.errors.push_back(std::move(msg));
          }
        }
      }
    };

    check_list(deps.depends_on, "depends_on", true, false, false, true);
    check_list(deps.after, "after", true, false, false, true);
    check_list(deps.before, "before", false, true, false, false);
    check_list(deps.requires_units, "requires", false, false, false, true);
    check_list(deps.wants, "wants", false, false, false, false);
    check_list(deps.binds_to, "binds_to", false, false, false, true);
    check_list(deps.part_of, "part_of", false, false, false, false);
    check_list(deps.conflicts, "conflicts", false, false, true, false);
  };

  if (manifest.proxy.enabled) {
    validate_deps(manifest.proxy.name, manifest.proxy.deps);
  }

  for (const auto& cs : manifest.containers) {
    validate_deps(cs.name, cs.deps);
    check_triggers(cs.name, cs.on_start_trigger, cs.on_ready_trigger, cs.on_exit_trigger, cs.on_error_trigger);
  }

  for (const auto& cs : manifest.components) {
    if (!cs.container.empty()) {
      add_order_edge(cs.container, cs.name);
    }

    validate_deps(cs.name, cs.deps);
    check_triggers(cs.name, cs.on_start_trigger, cs.on_ready_trigger, cs.on_exit_trigger, cs.on_error_trigger);
  }

  for (const auto& ps : manifest.processes) {
    validate_deps(ps.name, ps.deps);
    check_triggers(ps.name, ps.on_start_trigger, ps.on_ready_trigger, ps.on_exit_trigger, ps.on_error_trigger);
  }

  for (const auto& ts : manifest.tasks) {
    validate_deps(ts.name, ts.deps);
    check_triggers(ts.name, ts.on_start_trigger, ts.on_ready_trigger, ts.on_exit_trigger, ts.on_error_trigger);
  }

  for (const auto& gc : manifest.groups) {
    for (const auto& m : gc.members) {
      if (unit_names.count(m) == 0U) {
        rep.errors.push_back("Group '" + gc.name + "' references unknown member '" + m + "'.");
      }
    }

    if (gc.mode == GroupExecutionMode::kSerial && gc.members.size() >= 2U) {
      for (size_t i = 1U; i < gc.members.size(); ++i) {
        add_order_edge(gc.members[i - 1U], gc.members[i]);
      }
    }

    for (const auto& after_ref : gc.after) {
      std::vector<std::string> preds;
      expand_ref("@" + gc.name, after_ref, "after", preds);

      for (const auto& p : preds) {
        for (const auto& m : gc.members) {
          add_order_edge(p, m);
        }
      }
    }

    for (const auto& before_ref : gc.before) {
      std::vector<std::string> succs;
      expand_ref("@" + gc.name, before_ref, "before", succs);

      for (const auto& s : succs) {
        for (const auto& m : gc.members) {
          add_order_edge(m, s);
        }
      }
    }
  }

  auto check_dag_cycle =
      [](std::unordered_map<std::string, std::vector<std::string>>& graph,
         std::unordered_map<std::string, size_t>& indeg, std::set<std::pair<std::string, std::string>>& edges,
         const std::unordered_map<std::string, int32_t>& stages_map, const char* barrier_prefix) -> std::string {
    std::set<int32_t> stage_set;
    std::unordered_map<int32_t, std::vector<std::string>> by_stage;

    for (const auto& [name, st] : stages_map) {
      if (graph.count(name) != 0U) {
        stage_set.insert(st);
        by_stage[st].push_back(name);
      }
    }

    if (stage_set.size() > 1U) {
      std::vector<int32_t> sorted_st(stage_set.begin(), stage_set.end());

      for (size_t i = 1U; i < sorted_st.size(); ++i) {
        std::string barrier(1U, '\x01');
        barrier.append(barrier_prefix).append(std::to_string(i));
        graph.emplace(barrier, std::vector<std::string>{});
        indeg.emplace(barrier, 0U);

        for (const auto& p : by_stage[sorted_st[i - 1U]]) {
          if (edges.emplace(p, barrier).second) {
            graph[p].push_back(barrier);
            ++indeg[barrier];
          }
        }

        for (const auto& c : by_stage[sorted_st[i]]) {
          if (edges.emplace(barrier, c).second) {
            graph[barrier].push_back(c);
            ++indeg[c];
          }
        }
      }
    }

    std::vector<std::string> q;
    q.reserve(indeg.size());

    for (const auto& [name, deg] : indeg) {
      if (deg == 0U) {
        q.push_back(name);
      }
    }

    size_t visited = 0U;

    for (size_t idx = 0U; idx < q.size(); ++idx) {
      ++visited;

      for (const auto& nxt : graph[q[idx]]) {
        if (--indeg[nxt] == 0U) {
          q.push_back(nxt);
        }
      }
    }

    if (visited >= indeg.size()) {
      return {};
    }

    std::unordered_map<std::string, uint8_t> color;
    std::vector<std::string> stack;
    std::vector<std::string> cycle;

    auto dfs = [&](auto&& self, const std::string& node) -> bool {
      color[node] = 1U;
      stack.push_back(node);
      const auto git = graph.find(node);

      if (git != graph.end()) {
        for (const auto& nxt : git->second) {
          const auto dit = indeg.find(nxt);

          if (dit == indeg.end() || dit->second == 0U) {
            continue;
          }

          const uint8_t c = color[nxt];

          if (c == 1U) {
            const auto pos = std::find(stack.begin(), stack.end(), nxt);

            if (pos != stack.end()) {
              cycle.assign(pos, stack.end());
              cycle.push_back(nxt);
              return true;
            }
          } else if (c == 0U && self(self, nxt)) {
            return true;
          }
        }
      }

      stack.pop_back();
      color[node] = 2U;
      return false;
    };

    std::vector<std::string> candidates;

    for (const auto& [name, deg] : indeg) {
      if (deg > 0U) {
        candidates.push_back(name);
      }
    }

    std::sort(candidates.begin(), candidates.end());

    for (const auto& cand : candidates) {
      if (color[cand] == 0U && dfs(dfs, cand)) {
        break;
      }
    }

    std::string out;

    for (const auto& n : cycle) {
      if (!out.empty()) {
        out.append(" -> ");
      }

      out.append((!n.empty() && n[0] == '\x01') ? "<stage>" : n);
    }

    if (out.empty()) {
      for (const auto& cand : candidates) {
        if (!cand.empty() && cand[0] != '\x01') {
          if (!out.empty()) {
            out.append(", ");
          }

          out.append(cand);
        }
      }
    }

    return out;
  };

  const std::string primary_cycle = check_dag_cycle(adj, indegree, added_edges, unit_stage, "stage_barrier_");

  if (!primary_cycle.empty()) {
    rep.errors.push_back("Ordering cycle detected involving units: " + primary_cycle + ".");
  }

  if (!manifest.containers.empty() && primary_cycle.empty()) {
    for (const auto& ctr : manifest.containers) {
      std::unordered_map<std::string, int32_t> local_stages;

      for (const auto& cs : manifest.components) {
        if (cs.container == ctr.name) {
          local_stages[cs.name] = cs.stage;
        }
      }

      if (local_stages.size() <= 1U) {
        continue;
      }

      std::unordered_map<std::string, std::vector<std::string>> c_adj;
      std::unordered_map<std::string, size_t> c_indeg;
      std::set<std::pair<std::string, std::string>> c_edges;

      for (const auto& [cname, _] : local_stages) {
        c_adj.emplace(cname, std::vector<std::string>{});
        c_indeg.emplace(cname, 0U);
      }

      for (const auto& [u, v] : added_edges) {
        if (local_stages.count(u) != 0U && local_stages.count(v) != 0U && c_edges.emplace(u, v).second) {
          c_adj[u].push_back(v);
          ++c_indeg[v];
        }
      }

      const std::string c_cycle = check_dag_cycle(c_adj, c_indeg, c_edges, local_stages, "c_stage_barrier_");

      if (!c_cycle.empty()) {
        rep.errors.push_back("Container '" + ctr.name + "' local ordering cycle detected involving units: " + c_cycle +
                             ".");
      }
    }
  }

  if (!manifest.containers.empty() && primary_cycle.empty()) {
    auto host_endpoint = [&](const std::string& n) -> std::string {
      const auto it = comp_owner.find(n);
      return (it != comp_owner.end()) ? it->second : n;
    };

    std::unordered_map<std::string, std::vector<std::string>> host_adj;
    std::unordered_map<std::string, size_t> host_indeg;
    std::set<std::pair<std::string, std::string>> host_edges;

    for (const auto& uname : unit_names) {
      if (comp_owner.count(uname) == 0U) {
        host_adj.emplace(uname, std::vector<std::string>{});
        host_indeg.emplace(uname, 0U);
      }
    }

    for (const auto& [u, v] : added_edges) {
      if ((!u.empty() && u[0] == '\x01') || (!v.empty() && v[0] == '\x01')) {
        continue;
      }

      const std::string hu = host_endpoint(u);
      const std::string hv = host_endpoint(v);

      if (hu != hv && host_adj.count(hu) != 0U && host_adj.count(hv) != 0U && host_edges.emplace(hu, hv).second) {
        host_adj[hu].push_back(hv);
        ++host_indeg[hv];
      }
    }

    const std::string host_cycle = check_dag_cycle(host_adj, host_indeg, host_edges, unit_stage, "host_stage_barrier_");

    if (!host_cycle.empty()) {
      rep.errors.push_back("Cross-container dependency lifting introduces a host-level ordering cycle: " + host_cycle +
                           ".");
    }
  }

  if (!manifest.default_target.empty()) {
    const std::string& dt = manifest.default_target;

    if (dt[0] == '@') {
      if (groups_by_name.count(dt.substr(1U)) == 0U) {
        rep.errors.push_back("[launcher].target references unknown group '" + dt + "'.");
      }
    } else if (unit_names.count(dt) == 0U) {
      rep.errors.push_back("[launcher].target references unknown unit '" + dt + "'.");
    }
  }

  const std::string effective_ctrl_scheme = resolve_scheme(manifest.control_scheme);

  if (!manifest.security_key.empty() && effective_ctrl_scheme == "intra") {
    rep.errors.emplace_back("[launcher].key cannot be used with ctrl_scheme='intra'.");
  }

  if (manifest.control_enabled && !manifest.containers.empty() && effective_ctrl_scheme == "intra") {
    rep.errors.emplace_back("[[container]] requires a cross-process ctrl_scheme (cannot use 'intra').");
  }

  if (ascii_iequals(manifest.logger.plugin, "logd") && effective_ctrl_scheme == "intra" &&
      (!manifest.processes.empty() || !manifest.containers.empty())) {
    rep.warnings.emplace_back(
        "[logger].plugin='logd' with ctrl_scheme='intra' cannot aggregate logs from child processes or containers; "
        "use a cross-process ctrl_scheme (dds/ddsc/zenoh).");
  }

  if (!manifest.record.format.empty() && !is_valid_record_format(manifest.record.format)) {
    rep.errors.push_back("[record].format '" + manifest.record.format + "' is invalid (expected vdb|vdbx|vcap|vcapx).");
  }

  {
    struct RecordOwner final {
      std::string owner;
      std::string proc_domain;
    };

    std::unordered_map<std::string, RecordOwner> record_path_owners;

    auto normalize_bag_key = [&](const std::string& path) -> std::string {
      std::filesystem::path p(path);

      if (p.is_relative() && !manifest.record.dir.empty()) {
        p = std::filesystem::path(manifest.record.dir) / p;
      }

      std::error_code ec;
      const auto abs_p = std::filesystem::absolute(p, ec);
      const std::filesystem::path base_p = ec ? p : abs_p;
      std::error_code can_ec;
      const auto canon_p = std::filesystem::weakly_canonical(base_p, can_ec);
      const std::filesystem::path norm_p = (!can_ec && !canon_p.empty()) ? canon_p : base_p.lexically_normal();
      std::string s = Helpers::path_to_string(norm_p);
#if defined(_WIN32) || defined(_WIN64)
      s = to_lower_copy(s);
#endif
      return s;
    };

    auto check_record_path = [&](const std::string& owner, const std::string& path, const std::string& proc_domain) {
      if (path.empty()) {
        return;
      }

      if (!has_valid_bag_extension(path)) {
        rep.errors.push_back(owner + " record_path '" + path + "' must end with .vdb, .vdbx, .vcap, or .vcapx.");
        return;
      }

      const std::string norm = normalize_bag_key(path);
      auto [it, inserted] = record_path_owners.emplace(norm, RecordOwner{owner, proc_domain});

      if (!inserted && it->second.proc_domain != proc_domain) {
        rep.errors.push_back(owner + " and " + it->second.owner + " share the same record_path '" + path +
                             "'; distinct OS processes must write to distinct bag files.");
      }
    };

    for (const auto& cs : manifest.components) {
      if (!cs.condition) {
        continue;
      }

      const std::string dom = cs.container.empty() ? "__host__" : ("container:" + cs.container);
      check_record_path("Component '" + cs.name + "'", cs.record_path, dom);
    }

    for (const auto& cs : manifest.containers) {
      if (!cs.condition) {
        continue;
      }

      check_record_path("Container '" + cs.name + "'", cs.record_path, "container:" + cs.name);
    }

    for (const auto& ps : manifest.processes) {
      if (!ps.condition) {
        continue;
      }

      check_record_path("Process '" + ps.name + "'", ps.record_path, "process:" + ps.name);
    }

    if (manifest.global_env.count("VLINK_BAG_PATH") != 0U &&
        (manifest.processes.size() + manifest.containers.size()) > 1U) {
      rep.warnings.emplace_back(
          "[env].VLINK_BAG_PATH is inherited by multiple OS processes and will truncate the shared bag file; use "
          "[record] or per-unit record_path instead.");
    }
  }

  {
    std::unordered_map<std::string, std::string> process_domain;

    for (const auto& cs : manifest.components) {
      process_domain[cs.name] = cs.container.empty() ? "__host__" : ("container:" + cs.container);
    }

    for (const auto& ps : manifest.processes) {
      process_domain[ps.name] = "process:" + ps.name;
    }

    for (const auto& ts : manifest.tasks) {
      process_domain[ts.name] = "task:" + ts.name;
    }

    for (const auto& edge : compute_dataflow_edges(manifest)) {
      if (Helpers::has_startwith(edge.url, "intra://")) {
        const auto it_from = process_domain.find(edge.from);
        const auto it_to = process_domain.find(edge.to);

        if (it_from != process_domain.end() && it_to != process_domain.end() && it_from->second != it_to->second) {
          rep.warnings.push_back("Dataflow '" + edge.from + "' -> '" + edge.to + "' uses intra:// URL '" + edge.url +
                                 "' across distinct OS processes; use dds://, shm://, or zenoh:// instead.");
        }
      }
    }
  }

  rep.valid = rep.errors.empty();
  return rep;
}

std::string escape_dot_label(std::string_view raw) {
  std::string out;
  out.reserve(raw.size());

  for (const char ch : raw) {
    if (ch == '\\' || ch == '"') {
      out.push_back('\\');
      out.push_back(ch);
    } else if (ch == '\n') {
      out.append("\\n");
    } else if (ch == '\r') {
      continue;
    } else {
      out.push_back(ch);
    }
  }

  return out;
}

std::string escape_mermaid_label(std::string_view raw) {
  std::string out;
  out.reserve(raw.size());

  for (const char ch : raw) {
    if (ch == '"') {
      out.append("&quot;");
    } else if (ch == '\n' || ch == '\r') {
      out.push_back(' ');
    } else {
      out.push_back(ch);
    }
  }

  return out;
}

std::string mermaid_node_id(std::string_view unit_name) {
  static constexpr const char* kHex = "0123456789abcdef";
  std::string out = "u_";
  out.reserve(unit_name.size() + 8U);

  for (const char ch : unit_name) {
    const auto uc = static_cast<unsigned char>(ch);

    if (std::isalnum(uc) != 0) {
      out.push_back(ch);
    } else if (ch == '_') {
      out.append("__");
    } else if (ch == '/') {
      out.append("_s_");
    } else {
      out.append("_x");
      out.push_back(kHex[(uc >> 4U) & 0x0FU]);
      out.push_back(kHex[uc & 0x0FU]);
      out.push_back('_');
    }
  }

  return out;
}

template <typename MapT>
static std::vector<const typename MapT::value_type*> sorted_map_entries(const MapT& map) {
  std::vector<const typename MapT::value_type*> entries;
  entries.reserve(map.size());

  for (const auto& kv : map) {
    entries.push_back(&kv);
  }

  std::sort(entries.begin(), entries.end(),
            [](const typename MapT::value_type* a, const typename MapT::value_type* b) { return a->first < b->first; });
  return entries;
}

static nlohmann::ordered_json str_map_to_json(const std::unordered_map<std::string, std::string>& map) {
  nlohmann::ordered_json obj = nlohmann::ordered_json::object();

  for (const auto* entry : sorted_map_entries(map)) {
    obj[entry->first] = entry->second;
  }

  return obj;
}

static nlohmann::ordered_json resolved_url_map_to_json(const std::unordered_map<std::string, std::string>& map,
                                                       std::string_view ns,
                                                       const std::vector<std::pair<std::string, std::string>>& remaps) {
  nlohmann::ordered_json obj = nlohmann::ordered_json::object();

  for (const auto* entry : sorted_map_entries(map)) {
    obj[entry->first] = resolve_unit_url(entry->second, ns, remaps);
  }

  return obj;
}

static nlohmann::ordered_json param_map_to_json(const LauncherParamMap& params) {
  nlohmann::ordered_json obj = nlohmann::ordered_json::object();

  for (const auto* entry : sorted_map_entries(params)) {
    std::visit([&](const auto& val) { obj[entry->first] = val; }, entry->second);
  }

  return obj;
}

std::string format_manifest_json(const LaunchManifest& manifest) {
  nlohmann::ordered_json root = nlohmann::ordered_json::object();
  root["name"] = manifest.name;
  root["source_file"] = manifest.source_file;
  root["namespace"] = manifest.ns;
  const char* startup_mode_str = "parallel";

  if (manifest.startup_mode == StartupMode::kSerial) {
    startup_mode_str = "serial";
  } else if (manifest.startup_mode == StartupMode::kMaxParallel) {
    startup_mode_str = "max_parallel";
  }

  root["startup_mode"] = startup_mode_str;
  root["max_parallel"] = manifest.max_parallel;
  root["default_target"] = manifest.default_target;
  root["shutdown_timeout_ms"] = manifest.shutdown_timeout_ms;
  root["control_scheme"] = manifest.control_scheme;
  root["control_namespace"] = manifest.control_namespace;
  root["control_domain"] = manifest.control_domain;
  root["control_enabled"] = manifest.control_enabled;
  root["security_enabled"] = !manifest.security_key.empty();

  nlohmann::ordered_json args_arr = nlohmann::ordered_json::array();

  for (const auto& a : manifest.arg_specs) {
    nlohmann::ordered_json item;
    item["name"] = a.name;
    item["default"] = a.default_value;
    item["resolved"] = a.resolved_value;
    item["description"] = a.description;
    item["choices"] = a.choices;
    args_arr.push_back(std::move(item));
  }

  root["args"] = std::move(args_arr);
  root["urls"] = str_map_to_json(manifest.global_urls);
  root["global_params"] = param_map_to_json(manifest.global_params);

  nlohmann::ordered_json groups_arr = nlohmann::ordered_json::array();

  for (const auto& g : manifest.groups) {
    nlohmann::ordered_json item;
    item["name"] = g.name;
    item["mode"] = (g.mode == GroupExecutionMode::kSerial) ? "serial" : "parallel";
    item["slice"] = g.slice;
    item["members"] = g.members;
    item["after"] = g.after;
    item["before"] = g.before;
    groups_arr.push_back(std::move(item));
  }

  root["groups"] = std::move(groups_arr);

  nlohmann::ordered_json units_arr = nlohmann::ordered_json::array();

  auto push_unit =
      [&](const std::string& name, const char* type, const std::string& grp, int32_t stage, bool cond, bool autostart,
          const DependencySpec& deps, const std::unordered_map<std::string, std::string>* urls = nullptr,
          const std::unordered_map<std::string, std::string>* inputs = nullptr,
          const std::unordered_map<std::string, std::string>* outputs = nullptr, std::string_view unit_ns = "",
          const std::vector<std::pair<std::string, std::string>>* remaps = nullptr) {
        static const std::vector<std::pair<std::string, std::string>> kEmptyRemaps;
        const auto& active_remaps = (remaps != nullptr) ? *remaps : kEmptyRemaps;
        nlohmann::ordered_json item;
        item["name"] = name;
        item["type"] = type;
        item["group"] = grp;
        item["stage"] = stage;
        item["condition"] = cond;
        item["autostart"] = autostart;
        item["depends_on"] = deps.depends_on;
        item["after"] = deps.after;
        item["before"] = deps.before;
        item["requires"] = deps.requires_units;
        item["wants"] = deps.wants;
        item["binds_to"] = deps.binds_to;
        item["part_of"] = deps.part_of;
        item["conflicts"] = deps.conflicts;

        if (urls != nullptr && !urls->empty()) {
          item["urls"] = resolved_url_map_to_json(*urls, unit_ns, active_remaps);
        }

        if (inputs != nullptr && !inputs->empty()) {
          item["inputs"] = resolved_url_map_to_json(*inputs, unit_ns, active_remaps);
        }

        if (outputs != nullptr && !outputs->empty()) {
          item["outputs"] = resolved_url_map_to_json(*outputs, unit_ns, active_remaps);
        }

        units_arr.push_back(std::move(item));
      };

  if (manifest.proxy.enabled) {
    push_unit(manifest.proxy.name, "proxy", manifest.proxy.task_group, manifest.proxy.stage, manifest.proxy.condition,
              manifest.proxy.autostart, manifest.proxy.deps);
  }

  for (const auto& c : manifest.containers) {
    push_unit(c.name, "container", c.task_group, c.stage, c.condition, c.autostart, c.deps);
  }

  for (const auto& c : manifest.components) {
    push_unit(c.name, "component", c.task_group, c.stage, c.condition, c.autostart, c.deps, &c.urls, &c.inputs,
              &c.outputs, c.ns, &c.remaps);
  }

  for (const auto& p : manifest.processes) {
    push_unit(p.name, "process", p.task_group, p.stage, p.condition, p.autostart, p.deps, &p.urls, &p.inputs,
              &p.outputs, p.ns, &p.remaps);
  }

  for (const auto& t : manifest.tasks) {
    push_unit(t.name, "task", t.task_group, t.stage, t.condition, t.autostart, t.deps, &t.urls, &t.inputs, &t.outputs,
              t.ns, &t.remaps);
  }

  root["units"] = std::move(units_arr);

  nlohmann::ordered_json df_arr = nlohmann::ordered_json::array();

  for (const auto& edge : compute_dataflow_edges(manifest)) {
    nlohmann::ordered_json ej;
    ej["from"] = edge.from;
    ej["to"] = edge.to;
    ej["output"] = edge.output_key;
    ej["input"] = edge.input_key;
    ej["url"] = edge.url;
    df_arr.push_back(std::move(ej));
  }

  root["dataflow"] = std::move(df_arr);
  return root.dump(2);
}

static toml::array to_toml_str_array(const std::vector<std::string>& items) {
  toml::array arr;

  for (const auto& s : items) {
    arr.push_back(s);
  }

  return arr;
}

static void insert_str_map_to_toml(const char* table_name, const std::unordered_map<std::string, std::string>& map,
                                   toml::table& parent) {
  if (map.empty()) {
    return;
  }

  toml::table sub;

  for (const auto& [k, v] : map) {
    sub.insert(k, v);
  }

  parent.insert(table_name, std::move(sub));
}

static void insert_param_map_to_toml(const LauncherParamMap& params, toml::table& tbl) {
  for (const auto& entry : params) {
    const std::string& key = entry.first;
    std::visit(
        [&tbl, &key](const auto& val) {
          using T = std::decay_t<decltype(val)>;

          if constexpr (std::is_same_v<T, bool> || std::is_same_v<T, int64_t> || std::is_same_v<T, double> ||
                        std::is_same_v<T, std::string>) {
            tbl.insert_or_assign(key, val);
          } else if constexpr (std::is_same_v<T, std::vector<bool>>) {
            toml::array arr;

            for (const bool elem : val) {
              arr.push_back(elem);
            }

            tbl.insert_or_assign(key, std::move(arr));
          } else {
            toml::array arr;

            for (const auto& elem : val) {
              arr.push_back(elem);
            }

            tbl.insert_or_assign(key, std::move(arr));
          }
        },
        entry.second);
  }
}

static void insert_deps_to_toml(const DependencySpec& d, toml::table& tbl) {
  if (!d.depends_on.empty()) {
    tbl.insert("depends_on", to_toml_str_array(d.depends_on));
  }

  if (!d.after.empty()) {
    tbl.insert("after", to_toml_str_array(d.after));
  }

  if (!d.before.empty()) {
    tbl.insert("before", to_toml_str_array(d.before));
  }

  if (!d.requires_units.empty()) {
    tbl.insert("requires", to_toml_str_array(d.requires_units));
  }

  if (!d.wants.empty()) {
    tbl.insert("wants", to_toml_str_array(d.wants));
  }

  if (!d.binds_to.empty()) {
    tbl.insert("binds_to", to_toml_str_array(d.binds_to));
  }

  if (!d.part_of.empty()) {
    tbl.insert("part_of", to_toml_str_array(d.part_of));
  }

  if (!d.conflicts.empty()) {
    tbl.insert("conflicts", to_toml_str_array(d.conflicts));
  }
}

static void insert_resources_to_toml(const ResourceLimits& r, toml::table& parent) {
  if (r.empty()) {
    return;
  }

  toml::table rt;

  if (!r.slice.empty()) {
    rt.insert("slice", r.slice);
  }

  if (r.cpu_quota_percent.has_value()) {
    rt.insert("cpu_quota_percent", static_cast<int64_t>(*r.cpu_quota_percent));
  }

  if (r.cpu_weight.has_value()) {
    rt.insert("cpu_weight", static_cast<int64_t>(*r.cpu_weight));
  }

  if (r.memory_max_mb.has_value()) {
    rt.insert("mem_max_mb", static_cast<int64_t>(*r.memory_max_mb));
  }

  if (r.memory_high_mb.has_value()) {
    rt.insert("mem_high_mb", static_cast<int64_t>(*r.memory_high_mb));
  }

  if (r.pids_max.has_value()) {
    rt.insert("pids_max", static_cast<int64_t>(*r.pids_max));
  }

  if (r.nice.has_value()) {
    rt.insert("nice", static_cast<int64_t>(*r.nice));
  }

  if (r.oom_score_adj.has_value()) {
    rt.insert("oom_adj", static_cast<int64_t>(*r.oom_score_adj));
  }

  if (!r.cpu_affinity.empty()) {
    toml::array cpus;

    for (const uint32_t c : r.cpu_affinity) {
      cpus.push_back(static_cast<int64_t>(c));
    }

    rt.insert("cpus", std::move(cpus));
  }

  if (r.rlimit_nofile.has_value()) {
    rt.insert("nofile", static_cast<int64_t>(*r.rlimit_nofile));
  }

  if (r.rlimit_as_mb.has_value()) {
    rt.insert("vmem_max_mb", static_cast<int64_t>(*r.rlimit_as_mb));
  }

  if (r.rlimit_core_mb.has_value()) {
    rt.insert("core_max_mb", static_cast<int64_t>(*r.rlimit_core_mb));
  }

  rt.insert("strict", r.strict);
  parent.insert("resources", std::move(rt));
}

static const char* log_level_to_toml_string(Logger::Level level) noexcept {
  switch (level) {
    case Logger::kTrace:
      return "trace";
    case Logger::kDebug:
      return "debug";
    case Logger::kInfo:
      return "info";
    case Logger::kWarn:
      return "warn";
    case Logger::kError:
      return "error";
    case Logger::kFatal:
      return "fatal";
    default:
      return "off";
  }
}

static std::string serialize_toml_doc(const toml::table& doc) {
  if (doc.empty()) {
    return "";
  }

  std::ostringstream oss;
  oss << doc << "\n";
  return oss.str();
}

std::string format_proxy_config_toml(const ProxyConfig& spec) {
  toml::table doc;
  toml::table pt;
  pt.insert("name", spec.name);
  pt.insert("cmd", spec.program);
  pt.insert("dds", spec.dds_impl);
  pt.insert("domain", static_cast<int64_t>(spec.domain_id));
  pt.insert("stage", static_cast<int64_t>(spec.stage));
  insert_deps_to_toml(spec.deps, pt);
  insert_resources_to_toml(spec.resources, pt);
  doc.insert("proxy", std::move(pt));
  return serialize_toml_doc(doc);
}

std::string format_process_spec_toml(const ProcessSpec& ps) {
  toml::table doc;
  toml::table pt;
  pt.insert("name", ps.name);
  pt.insert("namespace", ps.ns);
  pt.insert("group", ps.task_group);
  pt.insert("stage", static_cast<int64_t>(ps.stage));
  pt.insert("cmd", ps.program);
  pt.insert("args", to_toml_str_array(ps.args));

  if (!ps.launch_prefix.empty()) {
    pt.insert("prefix", ps.launch_prefix);
  }

  pt.insert("cwd", ps.cwd);
  pt.insert("required", ps.required);
  pt.insert("autostart", ps.autostart);
  pt.insert("stop_timeout_ms", static_cast<int64_t>(ps.sigterm_timeout_ms));
  insert_deps_to_toml(ps.deps, pt);
  insert_resources_to_toml(ps.resources, pt);
  insert_str_map_to_toml("urls", ps.urls, pt);
  insert_str_map_to_toml("inputs", ps.inputs, pt);
  insert_str_map_to_toml("outputs", ps.outputs, pt);
  doc.insert("process", std::move(pt));
  return serialize_toml_doc(doc);
}

std::string format_component_spec_toml(const ComponentSpec& cs) {
  toml::table doc;
  toml::table ct;
  ct.insert("name", cs.name);
  ct.insert("namespace", cs.ns);
  ct.insert("group", cs.task_group);
  ct.insert("lib", cs.library);
  ct.insert("class", cs.class_name);
  ct.insert("scheduler", cs.scheduler_group);

  if (!cs.compute_pool.empty()) {
    ct.insert("pool", cs.compute_pool);
  }

  if (!cs.container.empty()) {
    ct.insert("container", cs.container);
  }

  ct.insert("stage", static_cast<int64_t>(cs.stage));
  ct.insert("worker", static_cast<int64_t>(cs.worker_index));
  ct.insert("priority", static_cast<int64_t>(cs.task_priority));
  ct.insert("watchdog_ms", static_cast<int64_t>(cs.watchdog_ms));
  insert_deps_to_toml(cs.deps, ct);

  if (!cs.params.empty()) {
    toml::table pt;
    insert_param_map_to_toml(cs.params, pt);
    ct.insert("params", std::move(pt));
  }

  insert_str_map_to_toml("urls", cs.urls, ct);
  insert_str_map_to_toml("inputs", cs.inputs, ct);
  insert_str_map_to_toml("outputs", cs.outputs, ct);

  if (cs.record_enabled.has_value()) {
    ct.insert("record", *cs.record_enabled);
  }

  if (!cs.record_path.empty()) {
    ct.insert("record_path", cs.record_path);
  }

  if (cs.record_force_publish.has_value()) {
    ct.insert("record_force", *cs.record_force_publish);
  }

  doc.insert("component", std::move(ct));
  return serialize_toml_doc(doc);
}

std::string format_container_spec_toml(const ContainerSpec& cs) {
  toml::table doc;
  toml::table ct;
  ct.insert("name", cs.name);
  ct.insert("namespace", cs.ns);
  ct.insert("group", cs.task_group);
  ct.insert("stage", static_cast<int64_t>(cs.stage));
  ct.insert("required", cs.required);
  ct.insert("stop_timeout_ms", static_cast<int64_t>(cs.sigterm_timeout_ms));
  insert_deps_to_toml(cs.deps, ct);
  insert_resources_to_toml(cs.resources, ct);
  doc.insert("container", std::move(ct));
  return serialize_toml_doc(doc);
}

std::string format_task_spec_toml(const TaskSpec& ts) {
  toml::table doc;
  toml::table tt;
  tt.insert("name", ts.name);
  tt.insert("namespace", ts.ns);
  tt.insert("group", ts.task_group);
  tt.insert("kind", ts.kind == TaskKind::kPeriodic ? "periodic" : "oneshot");
  tt.insert("cmd", ts.program);
  tt.insert("args", to_toml_str_array(ts.args));
  tt.insert("stage", static_cast<int64_t>(ts.stage));
  tt.insert("interval_ms", static_cast<int64_t>(ts.interval_ms));
  tt.insert("timeout_ms", static_cast<int64_t>(ts.timeout_ms));
  insert_deps_to_toml(ts.deps, tt);
  insert_resources_to_toml(ts.resources, tt);
  insert_str_map_to_toml("urls", ts.urls, tt);
  insert_str_map_to_toml("inputs", ts.inputs, tt);
  insert_str_map_to_toml("outputs", ts.outputs, tt);
  doc.insert("task", std::move(tt));
  return serialize_toml_doc(doc);
}

std::string format_unit_config_toml(const LaunchManifest& manifest, const std::string& unit_name) {
  if (manifest.proxy.enabled && manifest.proxy.name == unit_name) {
    return format_proxy_config_toml(manifest.proxy);
  }

  for (const auto& ps : manifest.processes) {
    if (ps.name == unit_name) {
      return format_process_spec_toml(ps);
    }
  }

  for (const auto& cs : manifest.components) {
    if (cs.name == unit_name) {
      return format_component_spec_toml(cs);
    }
  }

  for (const auto& cs : manifest.containers) {
    if (cs.name == unit_name) {
      return format_container_spec_toml(cs);
    }
  }

  for (const auto& ts : manifest.tasks) {
    if (ts.name == unit_name) {
      return format_task_spec_toml(ts);
    }
  }

  return "";
}

std::string serialize_container_manifest(const LaunchManifest& manifest, const ContainerSpec& container,
                                         const std::vector<ComponentSpec>& container_components,
                                         const std::string& control_scheme, const std::string& control_namespace,
                                         uint32_t control_domain, const std::string& bind_ip, bool native_mode,
                                         bool control_enabled, bool allow_remote_load,
                                         const LoggerConfig* logger_override, const RecordConfig* record_override) {
  const LoggerConfig& eff_logger = (logger_override != nullptr) ? *logger_override : manifest.logger;
  const RecordConfig& eff_record = (record_override != nullptr) ? *record_override : manifest.record;
  toml::table doc;

  toml::table launcher_tbl;
  launcher_tbl.insert("name", container.name);
  launcher_tbl.insert("namespace", "");
  launcher_tbl.insert("stop_timeout_ms", static_cast<int64_t>(container.sigterm_timeout_ms));
  launcher_tbl.insert("ctrl_scheme", control_scheme);
  launcher_tbl.insert("ctrl_namespace", control_namespace);
  launcher_tbl.insert("ctrl_domain", static_cast<int64_t>(control_domain));
  launcher_tbl.insert("ctrl", control_enabled);
  launcher_tbl.insert("bind_ip", bind_ip);
  launcher_tbl.insert("native", native_mode);

  if (allow_remote_load || manifest.allow_remote_load) {
    launcher_tbl.insert("remote_load", true);
  }

  if (!manifest.plugin_dirs.empty()) {
    launcher_tbl.insert("plugin_dirs", to_toml_str_array(manifest.plugin_dirs));
  }

  doc.insert("launcher", std::move(launcher_tbl));

  {
    toml::table log_tbl;

    if (!eff_logger.dir.empty()) {
      log_tbl.insert("dir", eff_logger.dir);
    }

    log_tbl.insert("console_level", log_level_to_toml_string(eff_logger.console_level));
    log_tbl.insert("file_level", log_level_to_toml_string(eff_logger.file_level));
    log_tbl.insert("max_size_mb",
                   static_cast<int64_t>(std::max<size_t>(1U, eff_logger.max_file_size / (1024U * 1024U))));
    log_tbl.insert("max_files", static_cast<int64_t>(eff_logger.max_files));
    log_tbl.insert("flush_ms", static_cast<int64_t>(eff_logger.flush_delay_ms));
    log_tbl.insert("history", static_cast<int64_t>(eff_logger.history_max_records));
    log_tbl.insert("fixed_name", eff_logger.fixed_filename);
    log_tbl.insert("append", eff_logger.append);
    log_tbl.insert("utc", eff_logger.utc);
    log_tbl.insert("console_fmt", eff_logger.console_fmt);
    log_tbl.insert("capture_file", eff_logger.capture_to_file);
    log_tbl.insert("strip_ansi", eff_logger.strip_ansi);

    if (!eff_logger.plugin.empty()) {
      log_tbl.insert("plugin", eff_logger.plugin);
      log_tbl.insert("plugin_strict", eff_logger.plugin_strict);
      log_tbl.insert("plugin_capture", eff_logger.plugin_capture);
    }

    doc.insert("logger", std::move(log_tbl));
  }

  if (eff_record.enabled || !eff_record.dir.empty() || eff_record.force_publish || eff_record.format != "vcap" ||
      !eff_record.tag.empty()) {
    toml::table rec_tbl;
    rec_tbl.insert("enabled", eff_record.enabled);

    if (!eff_record.dir.empty()) {
      rec_tbl.insert("dir", eff_record.dir);
    }

    if (!eff_record.format.empty()) {
      rec_tbl.insert("format", eff_record.format);
    }

    if (!eff_record.tag.empty()) {
      rec_tbl.insert("tag", eff_record.tag);
    }

    rec_tbl.insert("force", eff_record.force_publish);
    doc.insert("record", std::move(rec_tbl));
  }

  if (!manifest.global_params.empty()) {
    toml::table gp;
    insert_param_map_to_toml(manifest.global_params, gp);
    doc.insert("params", std::move(gp));
  }

  if (!manifest.schedulers.empty()) {
    toml::array sched_arr;

    for (const auto& sc : manifest.schedulers) {
      toml::table st;
      st.insert("name", sc.name);
      const char* mode_s = "shared";

      if (sc.mode == SchedulerMode::kDedicated) {
        mode_s = "dedicated";
      } else if (sc.mode == SchedulerMode::kMulti) {
        mode_s = "multi";
      } else if (sc.mode == SchedulerMode::kChoreography) {
        mode_s = "pinned";
      } else if (sc.mode == SchedulerMode::kThreadPool) {
        mode_s = "pool";
      }

      st.insert("mode", mode_s);
      const char* queue_s = "normal";

      if (sc.queue_type == MessageLoop::kLockfreeType) {
        queue_s = "lockfree";
      } else if (sc.queue_type == MessageLoop::kPriorityType) {
        queue_s = "priority";
      }

      const char* strat_s = "optimization";

      if (sc.strategy == MessageLoop::kPopStrategy) {
        strat_s = "pop";
      } else if (sc.strategy == MessageLoop::kBlockStrategy) {
        strat_s = "block";
      }

      st.insert("queue", queue_s);
      st.insert("strategy", strat_s);
      st.insert("threads", static_cast<int64_t>(sc.threads));
      st.insert("priority", static_cast<int64_t>(sc.thread_priority));
      st.insert("max_elapsed_ms", static_cast<int64_t>(sc.max_elapsed_ms));

      if (sc.sched_policy != SchedPolicy::kDefault) {
        const char* pol_s = "other";

        if (sc.sched_policy == SchedPolicy::kFifo) {
          pol_s = "fifo";
        } else if (sc.sched_policy == SchedPolicy::kRr) {
          pol_s = "rr";
        }

        st.insert("policy", pol_s);
      }

      if (!sc.cpu_affinity.empty()) {
        toml::array cpus;

        for (const uint32_t c : sc.cpu_affinity) {
          cpus.push_back(static_cast<int64_t>(c));
        }

        st.insert("cpus", std::move(cpus));
      }

      sched_arr.push_back(std::move(st));
    }

    doc.insert("scheduler", std::move(sched_arr));
  }

  std::unordered_set<std::string> local_names;

  for (const auto& comp : container_components) {
    local_names.insert(comp.name);
  }

  std::unordered_map<std::string, std::vector<std::string>> group_members_map;
  std::unordered_map<std::string, std::vector<std::string>> local_extra_after;
  std::unordered_map<std::string, std::vector<std::string>> local_extra_before;

  for (const auto& gc : manifest.groups) {
    if (!gc.name.empty()) {
      group_members_map[gc.name] = gc.members;
    }
  }

  for (const auto& gc : manifest.groups) {
    if (gc.name.empty()) {
      continue;
    }

    if (gc.mode == GroupExecutionMode::kSerial) {
      std::string prev_local;

      for (const auto& m : gc.members) {
        if (local_names.count(m) != 0U) {
          if (!prev_local.empty() && prev_local != m) {
            local_extra_after[m].push_back(prev_local);
          }

          prev_local = m;
        }
      }
    }

    for (const auto& m : gc.members) {
      if (local_names.count(m) == 0U) {
        continue;
      }

      for (const auto& aref : gc.after) {
        local_extra_after[m].push_back(aref);
      }

      for (const auto& bref : gc.before) {
        local_extra_before[m].push_back(bref);
      }
    }
  }

  {
    std::unordered_set<std::string> needed_groups;

    for (const auto& comp : container_components) {
      if (!comp.task_group.empty()) {
        needed_groups.insert(comp.task_group);
      }
    }

    if (!needed_groups.empty()) {
      toml::array grp_arr;

      for (const auto& gc : manifest.groups) {
        if (needed_groups.count(gc.name) == 0U) {
          continue;
        }

        toml::table gt;
        gt.insert("name", "/" + gc.name);
        gt.insert("mode", "parallel");
        grp_arr.push_back(std::move(gt));
      }

      if (!grp_arr.empty()) {
        doc.insert("group", std::move(grp_arr));
      }
    }
  }

  if (!container_components.empty()) {
    toml::array comp_arr;

    for (const auto& comp : container_components) {
      toml::table ct;
      ct.insert("name", "/" + comp.name);
      ct.insert("namespace", comp.ns);
      ct.insert("lib", comp.library);
      ct.insert("class", comp.class_name);
      ct.insert("scheduler", comp.scheduler_group);

      if (!comp.compute_pool.empty()) {
        ct.insert("pool", comp.compute_pool);
      }

      ct.insert("worker", static_cast<int64_t>(comp.worker_index));
      ct.insert("priority", static_cast<int64_t>(comp.task_priority));
      ct.insert("watchdog_ms", static_cast<int64_t>(comp.watchdog_ms));
      ct.insert("legacy", comp.legacy_runnable);
      ct.insert("condition", comp.condition);
      ct.insert("autostart", comp.autostart);

      if (comp.start_inactive) {
        ct.insert("init_state", "inactive");
      }

      ct.insert("required", comp.required);
      ct.insert("stage", static_cast<int64_t>(comp.stage));
      ct.insert("delay_ms", static_cast<int64_t>(comp.delay_ms));

      if (!comp.task_group.empty()) {
        ct.insert("group", comp.task_group);
      }

      if (comp.reload_mode == ReloadMode::kComponent) {
        ct.insert("reload", "component");
      }

      if (comp.record_enabled.has_value()) {
        ct.insert("record", *comp.record_enabled);
      }

      if (!comp.record_path.empty()) {
        ct.insert("record_path", comp.record_path);
      }

      if (comp.record_force_publish.has_value()) {
        ct.insert("record_force", *comp.record_force_publish);
      }

      auto filter_local = [&](const std::vector<std::string>& refs) {
        toml::array out_arr;
        std::unordered_set<std::string> seen;

        auto push_local = [&](const std::string& name) {
          if (local_names.count(name) != 0U && name != comp.name && seen.insert(name).second) {
            out_arr.push_back("/" + name);
          }
        };

        for (const auto& r : refs) {
          if (!r.empty() && r[0] == '@') {
            auto git = group_members_map.find(r.substr(1U));

            if (git != group_members_map.end()) {
              for (const auto& m : git->second) {
                push_local(m);
              }
            }
          } else {
            push_local(r);
          }
        }

        return out_arr;
      };

      auto insert_local_rel = [&](const char* field, const std::vector<std::string>& refs) {
        auto arr = filter_local(refs);

        if (!arr.empty()) {
          ct.insert(field, std::move(arr));
        }
      };

      insert_local_rel("depends_on", comp.deps.depends_on);

      {
        std::vector<std::string> after_refs = comp.deps.after;
        auto sit = local_extra_after.find(comp.name);

        if (sit != local_extra_after.end()) {
          after_refs.insert(after_refs.end(), sit->second.begin(), sit->second.end());
        }

        insert_local_rel("after", after_refs);
      }

      {
        std::vector<std::string> before_refs = comp.deps.before;
        auto bit = local_extra_before.find(comp.name);

        if (bit != local_extra_before.end()) {
          before_refs.insert(before_refs.end(), bit->second.begin(), bit->second.end());
        }

        insert_local_rel("before", before_refs);
      }

      insert_local_rel("requires", comp.deps.requires_units);
      insert_local_rel("wants", comp.deps.wants);
      insert_local_rel("binds_to", comp.deps.binds_to);
      insert_local_rel("part_of", comp.deps.part_of);
      insert_local_rel("conflicts", comp.deps.conflicts);
      insert_local_rel("on_start", comp.on_start_trigger);
      insert_local_rel("on_ready", comp.on_ready_trigger);
      insert_local_rel("on_exit_trigger", comp.on_exit_trigger);
      insert_local_rel("on_error", comp.on_error_trigger);

      if (comp.respawn.enabled || comp.respawn.policy != RestartPolicy::kNo) {
        const char* pstr = "no";

        if (comp.respawn.policy == RestartPolicy::kOnFailure) {
          pstr = "on_failure";
        } else if (comp.respawn.policy == RestartPolicy::kOnAbnormal) {
          pstr = "on_abnormal";
        } else if (comp.respawn.policy == RestartPolicy::kAlways) {
          pstr = "always";
        }

        ct.insert("restart", pstr);
        ct.insert("restart_delay_ms", static_cast<int64_t>(comp.respawn.delay_ms));
        ct.insert("restart_backoff", comp.respawn.backoff);
        ct.insert("restart_max_ms", static_cast<int64_t>(comp.respawn.max_delay_ms));
      }

      ct.insert("start_burst", static_cast<int64_t>(comp.respawn.start_limit_burst));
      ct.insert("start_window_ms", static_cast<int64_t>(comp.respawn.start_limit_interval_ms));

      if (!comp.respawn.success_exit_codes.empty()) {
        toml::array codes;

        for (const int32_t c : comp.respawn.success_exit_codes) {
          codes.push_back(static_cast<int64_t>(c));
        }

        ct.insert("ok_codes", std::move(codes));
      }

      if (!comp.respawn.restart_prevent_exit_codes.empty()) {
        toml::array codes;

        for (const int32_t c : comp.respawn.restart_prevent_exit_codes) {
          codes.push_back(static_cast<int64_t>(c));
        }

        ct.insert("no_restart_codes", std::move(codes));
      }

      if (comp.liveness.enabled && comp.liveness.type != ReadyProbeType::kNone) {
        toml::table lt;

        if (comp.liveness.type == ReadyProbeType::kFile) {
          lt.insert("type", "file");
          lt.insert("path", comp.liveness.path);
        } else if (comp.liveness.type == ReadyProbeType::kTopic) {
          lt.insert("type", "topic");
          lt.insert("topic", comp.liveness.topic);
        }

        lt.insert("interval_ms", static_cast<int64_t>(comp.liveness.interval_ms));
        lt.insert("failure_threshold", static_cast<int64_t>(comp.liveness.failure_threshold));
        lt.insert("restart_on_failure", comp.liveness.restart_on_failure);
        ct.insert("liveness", std::move(lt));
      }

      if (!comp.params.empty()) {
        toml::table pt;
        insert_param_map_to_toml(comp.params, pt);
        ct.insert("params", std::move(pt));
      }

      insert_str_map_to_toml("urls", comp.urls, ct);
      insert_str_map_to_toml("inputs", comp.inputs, ct);
      insert_str_map_to_toml("outputs", comp.outputs, ct);

      if (!comp.remaps.empty()) {
        toml::array rlist;

        for (const auto& [src, dst] : comp.remaps) {
          toml::array pair_arr;
          pair_arr.push_back(src);
          pair_arr.push_back(dst);
          rlist.push_back(std::move(pair_arr));
        }

        ct.insert("remap_list", std::move(rlist));
      }

      if (comp.ready.type != ReadyProbeType::kNone) {
        toml::table rdy;
        const char* rtype = "none";

        if (comp.ready.type == ReadyProbeType::kComponent) {
          rtype = "component";
        } else if (comp.ready.type == ReadyProbeType::kDelay) {
          rtype = "delay";
        } else if (comp.ready.type == ReadyProbeType::kFile) {
          rtype = "file";
        } else if (comp.ready.type == ReadyProbeType::kTopic) {
          rtype = "topic";
        } else if (comp.ready.type == ReadyProbeType::kStdout) {
          rtype = "stdout";
        }

        rdy.insert("type", rtype);
        rdy.insert("delay_ms", static_cast<int64_t>(comp.ready.delay_ms));
        rdy.insert("timeout_ms", static_cast<int64_t>(comp.ready.timeout_ms));

        if (!comp.ready.path.empty()) {
          rdy.insert("path", comp.ready.path);
        }

        if (!comp.ready.topic.empty()) {
          rdy.insert("topic", comp.ready.topic);
        }

        if (!comp.ready.pattern.empty()) {
          rdy.insert("pattern", comp.ready.pattern);
        }

        ct.insert("ready", std::move(rdy));
      }

      comp_arr.push_back(std::move(ct));
    }

    doc.insert("component", std::move(comp_arr));
  }

  auto escape_template_str = [](std::string& s) {
    size_t pos = s.find("${", 0U);

    while (pos != std::string::npos) {
      s.insert(pos, 1U, '$');
      pos = s.find("${", pos + 3U);
    }
  };

  auto escape_node = [&](auto&& self, toml::node& node) -> void {
    auto* s = node.as_string();

    if (s != nullptr) {
      escape_template_str(s->get());
      return;
    }

    auto* t = node.as_table();

    if (t != nullptr) {
      for (auto& [k, v] : *t) {
        (void)k;
        self(self, v);
      }

      return;
    }

    auto* a = node.as_array();

    if (a != nullptr) {
      for (auto& elem : *a) {
        self(self, elem);
      }
    }
  };

  escape_node(escape_node, doc);

  std::ostringstream oss;
  oss << doc << "\n";
  return oss.str();
}

}  // namespace launcher

}  // namespace vlink
