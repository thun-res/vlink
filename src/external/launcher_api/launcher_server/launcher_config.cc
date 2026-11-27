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

#include "launcher_config.h"

#include <vlink/base/helpers.h>
#include <vlink/base/macros.h>
#include <vlink/base/utils.h>
//
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <nlohmann/json.hpp>
#include <queue>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <toml++/toml.hpp>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>

#if !defined(_WIN32) && !defined(_WIN64)
#include <unistd.h>
#endif

#include "../launcher_common.h"

namespace vlink::launcher {

static constexpr int64_t kMaxU32 = static_cast<int64_t>(std::numeric_limits<uint32_t>::max());
static constexpr size_t kMaxIncludeDepth = 16U;

struct ParseContext final {
  std::string this_dir;
  std::string default_ns;
  std::string default_group;
  std::unordered_map<std::string, std::string> args;
  std::unordered_map<std::string, std::string> default_remaps;
  bool is_root{true};
  size_t depth{0};
  std::vector<std::string>* active_stack{nullptr};
};

static std::string to_lower_ascii(std::string_view s) {
  std::string out;
  out.reserve(s.size());

  for (const char ch : s) {
    out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
  }

  return out;
}

static std::string canonical_include_key(const std::filesystem::path& p) {
  std::error_code ec;
  const auto canon = std::filesystem::weakly_canonical(p, ec);

  if (!ec && !canon.empty()) {
    return Helpers::path_to_string(canon);
  }

  return Helpers::path_to_string(p.lexically_normal());
}

static std::string expand_vars(std::string_view raw, const ParseContext& ctx) {
  std::string out;
  out.reserve(raw.size());

  for (size_t i = 0U; i < raw.size();) {
    if (i + 1U < raw.size() && raw[i] == '$' && raw[i + 1U] == '{') {
      const size_t close = raw.find('}', i + 2U);
      const size_t nested = raw.find("${", i + 2U);

      if (close != std::string_view::npos && (nested == std::string_view::npos || close < nested)) {
        const std::string_view expr = raw.substr(i + 2U, close - i - 2U);

        if (expr == "this_dir") {
          out.append(ctx.this_dir);
        } else if (expr.size() > 4U && expr.substr(0U, 4U) == "env.") {
          out.append(Utils::get_env(std::string(expr.substr(4U))));
        } else if (expr.size() > 4U && expr.substr(0U, 4U) == "arg.") {
          const std::string key(expr.substr(4U));
          auto it = ctx.args.find(key);

          if (it != ctx.args.end()) {
            out.append(it->second);
          } else {
            out.append(raw.substr(i, close - i + 1U));
          }
        } else {
          out.append(raw.substr(i, close - i + 1U));
        }

        i = close + 1U;
        continue;
      }
    }

    out.push_back(raw[i++]);
  }

  return out;
}

static bool validate_allowed_keys(const toml::table& tbl, std::string_view section,
                                  const std::unordered_set<std::string_view>& allowed, std::string& err_out) {
  for (const auto& [k, v] : tbl) {
    (void)v;

    if (allowed.count(k.str()) == 0U) {
      err_out = "Unknown key '" + std::string(k.str()) + "' in " + std::string(section) + ".";
      return false;
    }
  }

  return true;
}

static std::string get_str(const toml::table& tbl, std::string_view key, const ParseContext& ctx,
                           const std::string& def, std::string& err, bool& ok) {
  const auto* node = tbl.get(key);

  if (node == nullptr) {
    return def;
  }

  const auto* sv = node->as_string();

  if (sv == nullptr) {
    ok = false;
    err = "Key '" + std::string(key) + "' must be a string.";
    return def;
  }

  return expand_vars(sv->get(), ctx);
}

static bool get_bool(const toml::table& tbl, std::string_view key, bool def, std::string& err, bool& ok) {
  const auto* node = tbl.get(key);

  if (node == nullptr) {
    return def;
  }

  const auto* bv = node->as_boolean();

  if (bv == nullptr) {
    ok = false;
    err = "Key '" + std::string(key) + "' must be a boolean.";
    return def;
  }

  return bv->get();
}

static int64_t get_i64_range(const toml::table& tbl, std::string_view key, int64_t def, int64_t min_v, int64_t max_v,
                             std::string& err, bool& ok) {
  const auto* node = tbl.get(key);

  if (node == nullptr) {
    return def;
  }

  const auto* iv = node->as_integer();

  if (iv == nullptr) {
    ok = false;
    err = "Key '" + std::string(key) + "' must be an integer.";
    return def;
  }

  const int64_t v = iv->get();

  if (v < min_v || v > max_v) {
    ok = false;
    err = "Key '" + std::string(key) + "' out of range [" + std::to_string(min_v) + ", " + std::to_string(max_v) + "].";
    return def;
  }

  return v;
}

static std::vector<std::string> get_str_list(const toml::table& tbl, std::string_view key, const ParseContext& ctx,
                                             std::string& err, bool& ok) {
  std::vector<std::string> res;
  const auto* node = tbl.get(key);

  if (node == nullptr) {
    return res;
  }

  const auto* arr = node->as_array();

  if (arr == nullptr) {
    ok = false;
    err = "Key '" + std::string(key) + "' must be an array of strings.";
    return res;
  }

  res.reserve(arr->size());

  for (const auto& item : *arr) {
    const auto* sv = item.as_string();

    if (sv == nullptr) {
      ok = false;
      err = "Key '" + std::string(key) + "' elements must be strings.";
      return {};
    }

    res.push_back(expand_vars(sv->get(), ctx));
  }

  return res;
}

static bool parse_args_table(const toml::table& tbl, const ParseContext& ctx,
                             std::unordered_map<std::string, std::string>& target, bool overwrite_existing,
                             std::string& err) {
  for (const auto& [k, v] : tbl) {
    const std::string key(k.str());

    if (!overwrite_existing && target.count(key) != 0U) {
      continue;
    }

    const auto* sv = v.as_string();
    const auto* iv = v.as_integer();
    const auto* fv = v.as_floating_point();
    const auto* bv = v.as_boolean();

    if (sv != nullptr) {
      target[key] = expand_vars(sv->get(), ctx);
    } else if (iv != nullptr) {
      target[key] = std::to_string(iv->get());
    } else if (fv != nullptr) {
      if (!std::isfinite(fv->get())) {
        err = "Argument '" + key + "' must be a finite number.";
        return false;
      }

      target[key] = std::to_string(fv->get());
    } else if (bv != nullptr) {
      target[key] = bv->get() ? "true" : "false";
    } else {
      err = "Argument '" + key + "' must be string, int, float, or bool.";
      return false;
    }
  }

  return true;
}

static bool parse_env_table(const toml::table& tbl, const ParseContext& ctx,
                            std::unordered_map<std::string, std::string>& out, std::string& err) {
  for (const auto& [k, v] : tbl) {
    const auto* sv = v.as_string();
    const auto* iv = v.as_integer();
    const auto* bv = v.as_boolean();

    if (sv != nullptr) {
      out[std::string(k.str())] = expand_vars(sv->get(), ctx);
    } else if (iv != nullptr) {
      out[std::string(k.str())] = std::to_string(iv->get());
    } else if (bv != nullptr) {
      out[std::string(k.str())] = bv->get() ? "true" : "false";
    } else {
      err = "Environment value for '" + std::string(k.str()) + "' must be string, int, or bool.";
      return false;
    }
  }

  return true;
}

static bool parse_params_table(const toml::table& tbl, const ParseContext& ctx, LauncherParamMap& out,
                               std::string& err) {
  for (const auto& [k, v] : tbl) {
    const std::string key(k.str());
    const auto* bv = v.as_boolean();
    const auto* iv = v.as_integer();
    const auto* fv = v.as_floating_point();
    const auto* sv = v.as_string();
    const auto* arr = v.as_array();

    if (bv != nullptr) {
      out[key] = bv->get();
    } else if (iv != nullptr) {
      out[key] = iv->get();
    } else if (fv != nullptr) {
      if (!std::isfinite(fv->get())) {
        err = "Param '" + key + "' must be a finite float.";
        return false;
      }

      out[key] = fv->get();
    } else if (sv != nullptr) {
      out[key] = expand_vars(sv->get(), ctx);
    } else if (arr != nullptr) {
      if (arr->empty()) {
        out[key] = std::vector<std::string>{};
      } else {
        bool all_bool = true;
        bool all_int = true;
        bool all_num = true;
        bool all_str = true;

        for (const auto& e : *arr) {
          if (!e.is_boolean()) {
            all_bool = false;
          }

          if (!e.is_integer()) {
            all_int = false;
          }

          if (!e.is_integer() && !e.is_floating_point()) {
            all_num = false;
          }

          if (e.is_floating_point() && !std::isfinite(e.as_floating_point()->get())) {
            err = "Param array '" + key + "' contains non-finite float.";
            return false;
          }

          if (!e.is_string()) {
            all_str = false;
          }
        }

        if (all_bool) {
          std::vector<bool> list;
          list.reserve(arr->size());

          for (const auto& e : *arr) {
            list.push_back(e.as_boolean()->get());
          }

          out[key] = std::move(list);
        } else if (all_int) {
          std::vector<int64_t> list;
          list.reserve(arr->size());

          for (const auto& e : *arr) {
            list.push_back(e.as_integer()->get());
          }

          out[key] = std::move(list);
        } else if (all_num) {
          std::vector<double> list;
          list.reserve(arr->size());

          for (const auto& e : *arr) {
            const auto* d = e.as_floating_point();

            if (d != nullptr) {
              list.push_back(d->get());
            } else {
              list.push_back(static_cast<double>(e.as_integer()->get()));
            }
          }

          out[key] = std::move(list);
        } else if (all_str) {
          std::vector<std::string> list;
          list.reserve(arr->size());

          for (const auto& e : *arr) {
            list.push_back(expand_vars(e.as_string()->get(), ctx));
          }

          out[key] = std::move(list);
        } else {
          err = "Param array '" + key + "' must be homogeneous.";
          return false;
        }
      }
    } else {
      err = "Param '" + key + "' has unsupported value type.";
      return false;
    }
  }

  return true;
}

static bool parse_enabled_flag(const toml::table& tbl, const ParseContext& ctx, bool& enabled_out, std::string& err) {
  enabled_out = true;
  const auto* node = tbl.get("enabled");

  if (node == nullptr) {
    return true;
  }

  if (const auto* bv = node->as_boolean(); bv != nullptr) {
    enabled_out = bv->get();
    return true;
  }

  const auto* sv = node->as_string();

  if (sv == nullptr) {
    err = "Key 'enabled' must be a boolean or string.";
    return false;
  }

  std::string expanded = expand_vars(sv->get(), ctx);
  size_t start = 0U;
  size_t end = expanded.size();

  while (start < end && std::isspace(static_cast<unsigned char>(expanded[start])) != 0) {
    ++start;
  }

  while (end > start && std::isspace(static_cast<unsigned char>(expanded[end - 1U])) != 0) {
    --end;
  }

  std::string token = to_lower_ascii(std::string_view(expanded).substr(start, end - start));
  bool negate = false;

  if (!token.empty() && token.front() == '!') {
    negate = true;
    token.erase(0U, 1U);
    size_t tstart = 0U;

    while (tstart < token.size() && std::isspace(static_cast<unsigned char>(token[tstart])) != 0) {
      ++tstart;
    }

    if (tstart > 0U) {
      token.erase(0U, tstart);
    }
  }

  bool val = false;

  if (token == "true" || token == "1" || token == "yes" || token == "on") {
    val = true;
  } else if (token.empty() || token == "false" || token == "0" || token == "no" || token == "off") {
    val = false;
  } else {
    err =
        "Invalid 'enabled' expression value '" + expanded + "' (expected true|false|1|0|yes|no|on|off, optional '!').";
    return false;
  }

  enabled_out = negate ? !val : val;
  return true;
}

static bool parse_remaps_table(const toml::table& tbl, const ParseContext& ctx,
                               std::unordered_map<std::string, std::string>& out, std::string& err) {
  for (const auto& [k, v] : tbl) {
    const auto* sv = v.as_string();

    if (sv == nullptr) {
      err = "Remap target for '" + std::string(k.str()) + "' must be a string.";
      return false;
    }

    out[expand_vars(k.str(), ctx)] = expand_vars(sv->get(), ctx);
  }

  return true;
}

static bool json_fits_int64_cfg(const nlohmann::json& val) noexcept {
  if (val.is_number_unsigned()) {
    return val.get<uint64_t>() <= static_cast<uint64_t>(std::numeric_limits<int64_t>::max());
  }

  return val.is_number_integer();
}

static bool parse_params_json_file(const std::filesystem::path& path, const ParseContext& ctx, LauncherParamMap& out,
                                   std::string& err) {
  std::ifstream ifs(path);

  if (!ifs) {
    err = "Failed to open params_file '" + Helpers::path_to_string(path) + "'.";
    return false;
  }

  try {
    const auto parsed_doc = nlohmann::json::parse(ifs);

    if (!parsed_doc.is_object()) {
      err = "JSON params_file '" + Helpers::path_to_string(path) + "' root must be an object.";
      return false;
    }

    const nlohmann::json& doc =
        (parsed_doc.size() == 1U && parsed_doc.contains("params") && parsed_doc["params"].is_object())
            ? parsed_doc["params"]
            : parsed_doc;

    for (auto it = doc.begin(); it != doc.end(); ++it) {
      const std::string& key = it.key();
      const auto& val = it.value();

      if (val.is_boolean()) {
        out[key] = val.get<bool>();
      } else if (json_fits_int64_cfg(val)) {
        out[key] = val.get<int64_t>();
      } else if (val.is_number()) {
        const double dv = val.get<double>();

        if (!std::isfinite(dv)) {
          err = "Param '" + key + "' in params_file must be a finite float.";
          return false;
        }

        out[key] = dv;
      } else if (val.is_string()) {
        out[key] = expand_vars(val.get<std::string>(), ctx);
      } else if (val.is_array()) {
        if (val.empty()) {
          out[key] = std::vector<std::string>{};
        } else {
          bool all_bool = true;
          bool all_int = true;
          bool all_num = true;
          bool all_str = true;

          for (const auto& e : val) {
            if (!e.is_boolean()) {
              all_bool = false;
            }

            if (!json_fits_int64_cfg(e)) {
              all_int = false;
            }

            if (!e.is_number() || !std::isfinite(e.get<double>())) {
              all_num = false;
            }

            if (!e.is_string()) {
              all_str = false;
            }
          }

          if (all_bool) {
            std::vector<bool> list;
            list.reserve(val.size());

            for (const auto& e : val) {
              list.push_back(e.get<bool>());
            }

            out[key] = std::move(list);
          } else if (all_int) {
            std::vector<int64_t> list;
            list.reserve(val.size());

            for (const auto& e : val) {
              list.push_back(e.get<int64_t>());
            }

            out[key] = std::move(list);
          } else if (all_num) {
            std::vector<double> list;
            list.reserve(val.size());

            for (const auto& e : val) {
              list.push_back(e.get<double>());
            }

            out[key] = std::move(list);
          } else if (all_str) {
            std::vector<std::string> list;
            list.reserve(val.size());

            for (const auto& e : val) {
              list.push_back(expand_vars(e.get<std::string>(), ctx));
            }

            out[key] = std::move(list);
          } else {
            err = "Param array '" + key + "' in params_file must be homogeneous.";
            return false;
          }
        }
      } else {
        err = "Param '" + key + "' in params_file has unsupported JSON type.";
        return false;
      }
    }
  } catch (const std::exception& e) {
    err = "Failed to parse JSON params_file '" + Helpers::path_to_string(path) + "': " + e.what();
    return false;
  }

  return true;
}

static bool load_params_files(const toml::table& unit_tbl, const ParseContext& ctx, LauncherParamMap& out,
                              std::string& err) {
  const auto* node = unit_tbl.get("params_file");

  if (node == nullptr) {
    return true;
  }

  std::vector<std::string> files;

  if (const auto* sv = node->as_string(); sv != nullptr) {
    files.push_back(expand_vars(sv->get(), ctx));
  } else if (const auto* arr = node->as_array(); arr != nullptr) {
    files.reserve(arr->size());

    for (const auto& elem : *arr) {
      const auto* es = elem.as_string();

      if (es == nullptr) {
        err = "Key 'params_file' array elements must be strings.";
        return false;
      }

      files.push_back(expand_vars(es->get(), ctx));
    }
  } else {
    err = "Key 'params_file' must be a string or array of strings.";
    return false;
  }

  for (const auto& f : files) {
    if (f.empty()) {
      err = "params_file path cannot be empty.";
      return false;
    }

    std::filesystem::path fpath(f);

    if (fpath.is_relative()) {
      fpath = std::filesystem::path(ctx.this_dir) / fpath;
    }

    const std::string ext = to_lower_ascii(Helpers::path_to_string(fpath.extension()));
    ParseContext file_ctx = ctx;
    const std::string pdir = Helpers::path_to_string(fpath.parent_path());
    file_ctx.this_dir = pdir.empty() ? "." : pdir;

    if (ext == ".json") {
      if (!parse_params_json_file(fpath, file_ctx, out, err)) {
        return false;
      }

      continue;
    }

    try {
      const auto pdoc = toml::parse_file(Helpers::path_to_string(fpath));

      const auto* sub = pdoc.get("params");

      if (sub != nullptr && sub->is_table() && pdoc.size() == 1U) {
        if (!parse_params_table(*sub->as_table(), file_ctx, out, err)) {
          return false;
        }
      } else {
        if (!parse_params_table(pdoc, file_ctx, out, err)) {
          return false;
        }
      }
    } catch (const toml::parse_error& e) {
      err = "Failed to parse params_file '" + f + "': " + std::string(e.description());
      return false;
    } catch (const std::exception& e) {
      err = "Failed to load params_file '" + f + "': " + e.what();
      return false;
    }
  }

  return true;
}

static bool parse_scheduler_and_queue(const toml::table& tbl, const ParseContext& ctx, bool is_component,
                                      SchedulerMode& mode_out, uint32_t& threads_out, MessageLoop::Type& qtype_out,
                                      MessageLoop::Strategy& qstrat_out, std::string& err) {
  bool ok = true;
  const bool has_dedicated = (tbl.get("dedicated") != nullptr);
  const bool dedicated = is_component ? get_bool(tbl, "dedicated", false, err, ok) : false;

  if (!ok) {
    return false;
  }

  const bool has_sched = (tbl.get("scheduler") != nullptr);
  const std::string sched =
      to_lower_ascii(get_str(tbl, "scheduler", ctx, is_component ? "shared" : "dedicated", err, ok));

  if (!ok) {
    return false;
  }

  if (sched == "shared" || sched.empty()) {
    mode_out = is_component ? SchedulerMode::kShared : SchedulerMode::kDedicated;
  } else if (sched == "dedicated") {
    mode_out = SchedulerMode::kDedicated;
  } else if (sched == "multi") {
    mode_out = SchedulerMode::kMulti;
  } else {
    err = "Unknown scheduler '" + sched + "' (expected shared|dedicated|multi).";
    return false;
  }

  if (is_component && has_dedicated) {
    if (has_sched) {
      const bool sched_is_dedicated = (mode_out == SchedulerMode::kDedicated);

      if (dedicated != sched_is_dedicated) {
        err = std::string("Conflicting 'dedicated = ") + (dedicated ? "true" : "false") + "' with 'scheduler = \"" +
              sched + "\"'.";
        return false;
      }
    } else if (dedicated) {
      mode_out = SchedulerMode::kDedicated;
    }
  }

  const int64_t min_threads = (mode_out == SchedulerMode::kMulti) ? 2 : 1;
  threads_out = static_cast<uint32_t>(get_i64_range(tbl, "threads", 2, min_threads, 64, err, ok));

  if (!ok) {
    return false;
  }

  const std::string qstr = to_lower_ascii(get_str(tbl, "queue", ctx, "normal", err, ok));

  if (!ok) {
    return false;
  }

  if (qstr == "normal" || qstr.empty()) {
    qtype_out = MessageLoop::kNormalType;
  } else if (qstr == "lockfree") {
    qtype_out = MessageLoop::kLockfreeType;
  } else if (qstr == "priority") {
    qtype_out = MessageLoop::kPriorityType;
  } else {
    err = "Unknown queue '" + qstr + "' (expected normal|lockfree|priority).";
    return false;
  }

  const std::string sstr = to_lower_ascii(get_str(tbl, "queue_strategy", ctx, "optimization", err, ok));

  if (!ok) {
    return false;
  }

  if (sstr == "optimization" || sstr.empty()) {
    qstrat_out = MessageLoop::kOptimizationStrategy;
  } else if (sstr == "pop") {
    qstrat_out = MessageLoop::kPopStrategy;
  } else if (sstr == "block") {
    qstrat_out = MessageLoop::kBlockStrategy;
  } else {
    err = "Unknown queue_strategy '" + sstr + "' (expected optimization|pop|block).";
    return false;
  }

  return true;
}

static bool parse_ready_probe(const toml::table& unit_tbl, const ParseContext& ctx, ReadyProbeConfig& out,
                              std::string& err) {
  const auto* node = unit_tbl.get("ready");

  if (node == nullptr) {
    return true;
  }

  const auto* tbl = node->as_table();

  if (tbl == nullptr) {
    err = "'ready' must be a table.";
    return false;
  }

  if (!validate_allowed_keys(*tbl, "ready", {"type", "pattern", "delay_ms", "timeout_ms"}, err)) {
    return false;
  }

  bool ok = true;
  const std::string t = to_lower_ascii(get_str(*tbl, "type", ctx, "none", err, ok));

  if (!ok) {
    return false;
  }

  if (t == "none" || t.empty()) {
    out.type = ReadyProbeType::kNone;
  } else if (t == "component") {
    out.type = ReadyProbeType::kComponent;
  } else if (t == "notify") {
    out.type = ReadyProbeType::kNotify;
  } else if (t == "stdout") {
    out.type = ReadyProbeType::kStdout;
  } else if (t == "delay") {
    out.type = ReadyProbeType::kDelay;
  } else {
    err = "Unknown ready.type '" + t + "' (expected none|component|notify|stdout|delay).";
    return false;
  }

  out.pattern = get_str(*tbl, "pattern", ctx, "", err, ok);

  if (!ok) {
    return false;
  }

  out.delay_ms = static_cast<uint32_t>(get_i64_range(*tbl, "delay_ms", 0, 0, kMaxU32, err, ok));

  if (!ok) {
    return false;
  }

  out.timeout_ms = static_cast<uint32_t>(get_i64_range(*tbl, "timeout_ms", 10000, 100, kMaxU32, err, ok));
  return ok;
}

static bool parse_respawn(const toml::table& tbl, const ParseContext& ctx, RespawnConfig& out, std::string& err) {
  bool ok = true;
  const std::string pol = to_lower_ascii(get_str(tbl, "restart", ctx, "no", err, ok));

  if (!ok) {
    return false;
  }

  if (pol == "no" || pol == "never" || pol.empty()) {
    out.policy = RestartPolicy::kNo;
  } else if (pol == "on_failure" || pol == "on-failure") {
    out.policy = RestartPolicy::kOnFailure;
  } else if (pol == "always") {
    out.policy = RestartPolicy::kAlways;
  } else {
    err = "Unknown restart policy '" + pol + "' (expected no|on_failure|always).";
    return false;
  }

  out.delay_ms = static_cast<uint32_t>(get_i64_range(tbl, "restart_delay_ms", 1000, 50, kMaxU32, err, ok));

  if (!ok) {
    return false;
  }

  out.max_delay_ms = static_cast<uint32_t>(
      get_i64_range(tbl, "restart_max_ms", std::max<int64_t>(30000, out.delay_ms), out.delay_ms, kMaxU32, err, ok));

  if (!ok) {
    return false;
  }

  out.start_burst = static_cast<uint32_t>(get_i64_range(tbl, "start_burst", 5, 0, kMaxU32, err, ok));

  if (!ok) {
    return false;
  }

  out.start_window_ms = static_cast<uint32_t>(get_i64_range(tbl, "start_window_ms", 60000, 1000, kMaxU32, err, ok));
  return ok;
}

static std::string join_ns(const std::string& base_ns, const std::string& local_ns) {
  if (local_ns.empty()) {
    return base_ns;
  }

  if (base_ns.empty()) {
    return local_ns;
  }

  if (local_ns.front() == '/') {
    return local_ns;
  }

  if (base_ns.back() == '/') {
    return base_ns + local_ns;
  }

  return base_ns + "/" + local_ns;
}

static bool parse_manifest_table(const toml::table& root, ParseContext ctx, LaunchManifest& out, std::string& err);

static bool load_include_file(const std::string& inc_str, const ParseContext& parent_ctx,
                              const std::string& ns_override, const std::string& group_override,
                              const std::string& prefix, const std::unordered_map<std::string, std::string>& inc_args,
                              const std::unordered_map<std::string, std::string>& inc_remaps, LaunchManifest& out,
                              std::string& err) {
  if (inc_str.empty()) {
    err = "Include file path cannot be empty.";
    return false;
  }

  if (parent_ctx.depth >= kMaxIncludeDepth) {
    err = "Include nesting depth exceeded (max " + std::to_string(kMaxIncludeDepth) + ").";
    return false;
  }

  std::filesystem::path inc_path(inc_str);

  if (inc_path.is_relative()) {
    inc_path = std::filesystem::path(parent_ctx.this_dir) / inc_path;
  }

  const std::string canon_key = canonical_include_key(inc_path);

  if (parent_ctx.active_stack != nullptr) {
    for (const auto& active : *parent_ctx.active_stack) {
      if (active == canon_key) {
        err = "Circular include detected: '" + inc_str + "'.";
        return false;
      }
    }
  }

  ParseContext child_ctx;
  const std::string inc_dir = Helpers::path_to_string(inc_path.parent_path());
  child_ctx.this_dir = inc_dir.empty() ? "." : inc_dir;
  child_ctx.default_ns = ns_override.empty() ? parent_ctx.default_ns : join_ns(parent_ctx.default_ns, ns_override);
  child_ctx.default_group = group_override.empty() ? parent_ctx.default_group : group_override;
  child_ctx.args = parent_ctx.args;

  for (const auto& kv : inc_args) {
    child_ctx.args[kv.first] = kv.second;
  }

  child_ctx.default_remaps = parent_ctx.default_remaps;

  for (const auto& kv : inc_remaps) {
    child_ctx.default_remaps[kv.first] = kv.second;
  }

  child_ctx.is_root = false;
  child_ctx.depth = parent_ctx.depth + 1U;
  child_ctx.active_stack = parent_ctx.active_stack;

  const size_t proc_start = out.processes.size();
  const size_t comp_start = out.components.size();

  if (parent_ctx.active_stack != nullptr) {
    parent_ctx.active_stack->push_back(canon_key);
  }

  bool parsed_ok = false;

  try {
    const auto inc_doc = toml::parse_file(Helpers::path_to_string(inc_path));
    parsed_ok = parse_manifest_table(inc_doc, std::move(child_ctx), out, err);
  } catch (const toml::parse_error& e) {
    err = "Failed to parse include '" + inc_str + "': " + std::string(e.description());
    parsed_ok = false;
  } catch (const std::exception& e) {
    err = "Failed to load include '" + inc_str + "': " + e.what();
    parsed_ok = false;
  }

  if (parent_ctx.active_stack != nullptr) {
    parent_ctx.active_stack->pop_back();
  }

  if (!parsed_ok) {
    return false;
  }

  if (!prefix.empty()) {
    std::unordered_set<std::string> local_names;

    for (size_t i = proc_start; i < out.processes.size(); ++i) {
      if (!out.processes[i].name.empty()) {
        local_names.insert(out.processes[i].name);
      }
    }

    for (size_t i = comp_start; i < out.components.size(); ++i) {
      if (!out.components[i].name.empty()) {
        local_names.insert(out.components[i].name);
      }
    }

    auto remap_deps = [&](std::vector<std::string>& deps) {
      for (auto& d : deps) {
        if (local_names.count(d) != 0U) {
          d.insert(0U, prefix);
        }
      }
    };

    for (size_t i = proc_start; i < out.processes.size(); ++i) {
      if (!out.processes[i].name.empty()) {
        out.processes[i].name.insert(0U, prefix);
      }

      remap_deps(out.processes[i].depends_on);
      remap_deps(out.processes[i].after);
    }

    for (size_t i = comp_start; i < out.components.size(); ++i) {
      if (!out.components[i].name.empty()) {
        out.components[i].name.insert(0U, prefix);
      }

      remap_deps(out.components[i].depends_on);
      remap_deps(out.components[i].after);
    }
  }

  return true;
}

static bool parse_manifest_table(const toml::table& root, ParseContext ctx, LaunchManifest& out, std::string& err) {
  if (!validate_allowed_keys(root, "root", {"launcher", "args", "include", "process", "component"}, err)) {
    return false;
  }

  const auto* anode = root.get("args");

  if (anode != nullptr) {
    const auto* atbl = anode->as_table();

    if (atbl == nullptr) {
      err = "[args] must be a table.";
      return false;
    }

    if (!parse_args_table(*atbl, ctx, ctx.args, false, err)) {
      return false;
    }
  }

  std::vector<std::string> launcher_includes;
  const auto* lnode = root.get("launcher");

  if (lnode != nullptr) {
    const auto* ltbl = lnode->as_table();

    if (ltbl == nullptr) {
      err = "[launcher] must be a table.";
      return false;
    }

    if (!validate_allowed_keys(*ltbl, "[launcher]",
                               {"name", "ns", "ctrl_enabled", "ctrl_scheme", "ctrl_namespace", "ctrl_domain", "key",
                                "shutdown_timeout_ms", "plugin_dirs", "includes"},
                               err)) {
      return false;
    }

    bool ok = true;

    if (ctx.is_root) {
      out.name = get_str(*ltbl, "name", ctx, out.name, err, ok);

      if (!ok) {
        return false;
      }

      out.ns = get_str(*ltbl, "ns", ctx, out.ns, err, ok);

      if (!ok) {
        return false;
      }

      ctx.default_ns = out.ns;
      out.control_enabled = get_bool(*ltbl, "ctrl_enabled", out.control_enabled, err, ok);

      if (!ok) {
        return false;
      }

      out.control_scheme = get_str(*ltbl, "ctrl_scheme", ctx, out.control_scheme, err, ok);

      if (!ok) {
        return false;
      }

      out.control_namespace = get_str(*ltbl, "ctrl_namespace", ctx, out.control_namespace, err, ok);

      if (!ok) {
        return false;
      }

      out.control_domain =
          static_cast<uint32_t>(get_i64_range(*ltbl, "ctrl_domain", out.control_domain, 0, kMaxU32, err, ok));

      if (!ok) {
        return false;
      }

      out.security_key = get_str(*ltbl, "key", ctx, out.security_key, err, ok);

      if (!ok) {
        return false;
      }

      out.shutdown_timeout_ms = static_cast<uint32_t>(
          get_i64_range(*ltbl, "shutdown_timeout_ms", out.shutdown_timeout_ms, 500, kMaxU32, err, ok));

      if (!ok) {
        return false;
      }
    } else {
      const std::string sub_ns = get_str(*ltbl, "ns", ctx, "", err, ok);

      if (!ok) {
        return false;
      }

      if (!sub_ns.empty()) {
        ctx.default_ns = join_ns(ctx.default_ns, sub_ns);
      }
    }

    const auto pdirs = get_str_list(*ltbl, "plugin_dirs", ctx, err, ok);

    if (!ok) {
      return false;
    }

    for (const auto& d : pdirs) {
      std::filesystem::path p(d);

      if (p.is_relative()) {
        p = std::filesystem::path(ctx.this_dir) / p;
      }

      out.plugin_dirs.push_back(Helpers::path_to_string(p.lexically_normal()));
    }

    launcher_includes = get_str_list(*ltbl, "includes", ctx, err, ok);

    if (!ok) {
      return false;
    }
  }

  for (const auto& inc : launcher_includes) {
    if (!load_include_file(inc, ctx, "", "", "", {}, {}, out, err)) {
      return false;
    }
  }

  const auto* inode = root.get("include");

  if (inode != nullptr) {
    const auto* iarr = inode->as_array();

    if (iarr == nullptr) {
      err = "[[include]] must be an array of tables.";
      return false;
    }

    for (const auto& item : *iarr) {
      const auto* tbl = item.as_table();

      if (tbl == nullptr) {
        err = "[[include]] entries must be tables.";
        return false;
      }

      if (!validate_allowed_keys(*tbl, "[[include]]", {"file", "ns", "group", "prefix", "args", "remaps", "enabled"},
                                 err)) {
        return false;
      }

      bool inc_enabled = true;

      if (!parse_enabled_flag(*tbl, ctx, inc_enabled, err)) {
        return false;
      }

      if (!inc_enabled) {
        continue;
      }

      bool ok = true;
      const std::string inc_file = get_str(*tbl, "file", ctx, "", err, ok);
      const std::string inc_ns = get_str(*tbl, "ns", ctx, "", err, ok);
      const std::string inc_group = get_str(*tbl, "group", ctx, "", err, ok);
      const std::string inc_prefix = get_str(*tbl, "prefix", ctx, "", err, ok);

      if (!ok) {
        return false;
      }

      std::unordered_map<std::string, std::string> inc_args;
      const auto* ianode = tbl->get("args");

      if (ianode != nullptr) {
        const auto* iatbl = ianode->as_table();

        if (iatbl == nullptr) {
          err = "[[include]] 'args' must be a table.";
          return false;
        }

        if (!parse_args_table(*iatbl, ctx, inc_args, true, err)) {
          return false;
        }
      }

      std::unordered_map<std::string, std::string> inc_remaps;
      const auto* irnode = tbl->get("remaps");

      if (irnode != nullptr) {
        const auto* irtbl = irnode->as_table();

        if (irtbl == nullptr) {
          err = "[[include]] 'remaps' must be a table.";
          return false;
        }

        if (!parse_remaps_table(*irtbl, ctx, inc_remaps, err)) {
          return false;
        }
      }

      if (!load_include_file(inc_file, ctx, inc_ns, inc_group, inc_prefix, inc_args, inc_remaps, out, err)) {
        return false;
      }
    }
  }

  const auto* pnode_root = root.get("process");

  if (pnode_root != nullptr) {
    const auto* parr = pnode_root->as_array();

    if (parr == nullptr) {
      err = "[[process]] must be an array of tables.";
      return false;
    }

    for (const auto& item : *parr) {
      const auto* tbl = item.as_table();

      if (tbl == nullptr) {
        err = "[[process]] entries must be tables.";
        return false;
      }

      if (!validate_allowed_keys(*tbl, "[[process]]",
                                 {"name",           "ns",          "group",           "cmd",
                                  "args",           "cwd",         "inherit_env",     "env",
                                  "params",         "params_file", "remaps",          "scheduler",
                                  "threads",        "queue",       "queue_strategy",  "enabled",
                                  "autostart",      "required",    "delay_ms",        "stop_timeout_ms",
                                  "watchdog_ms",    "output",      "restart",         "restart_delay_ms",
                                  "restart_max_ms", "start_burst", "start_window_ms", "depends_on",
                                  "after",          "ready"},
                                 err)) {
        return false;
      }

      bool proc_enabled = true;

      if (!parse_enabled_flag(*tbl, ctx, proc_enabled, err)) {
        return false;
      }

      if (!proc_enabled) {
        continue;
      }

      bool ok = true;
      ProcessSpec ps;
      ps.name = get_str(*tbl, "name", ctx, "", err, ok);

      if (!ok) {
        return false;
      }

      ps.ns = join_ns(ctx.default_ns, get_str(*tbl, "ns", ctx, "", err, ok));
      ps.group = get_str(*tbl, "group", ctx, ctx.default_group, err, ok);
      ps.program = get_str(*tbl, "cmd", ctx, "", err, ok);
      ps.args = get_str_list(*tbl, "args", ctx, err, ok);
      ps.cwd = get_str(*tbl, "cwd", ctx, "", err, ok);
      ps.inherit_env = get_bool(*tbl, "inherit_env", true, err, ok);
      ps.autostart = get_bool(*tbl, "autostart", true, err, ok);
      ps.required = get_bool(*tbl, "required", false, err, ok);
      ps.delay_ms = static_cast<uint32_t>(get_i64_range(*tbl, "delay_ms", 0, 0, kMaxU32, err, ok));
      ps.stop_timeout_ms = static_cast<uint32_t>(get_i64_range(*tbl, "stop_timeout_ms", 5000, 200, kMaxU32, err, ok));
      ps.watchdog_ms = static_cast<uint32_t>(get_i64_range(*tbl, "watchdog_ms", 0, 0, kMaxU32, err, ok));
      ps.depends_on = get_str_list(*tbl, "depends_on", ctx, err, ok);
      ps.after = get_str_list(*tbl, "after", ctx, err, ok);

      if (!ok) {
        return false;
      }

      const std::string out_s = to_lower_ascii(get_str(*tbl, "output", ctx, "screen", err, ok));

      if (!ok) {
        return false;
      }

      if (out_s == "screen" || out_s.empty()) {
        ps.output = OutputMode::kScreen;
      } else if (out_s == "none") {
        ps.output = OutputMode::kNone;
      } else {
        err = "Process '" + ps.name + "': unknown output mode '" + out_s + "' (expected screen|none).";
        return false;
      }

      if (!parse_respawn(*tbl, ctx, ps.respawn, err) || !parse_ready_probe(*tbl, ctx, ps.ready, err) ||
          !parse_scheduler_and_queue(*tbl, ctx, false, ps.scheduler, ps.threads, ps.queue_type, ps.queue_strategy,
                                     err)) {
        return false;
      }

      const auto* enode = tbl->get("env");

      if (enode != nullptr) {
        const auto* etbl = enode->as_table();

        if (etbl == nullptr || !parse_env_table(*etbl, ctx, ps.env, err)) {
          if (etbl == nullptr) {
            err = "Process '" + ps.name + "': 'env' must be a table.";
          }

          return false;
        }
      }

      if (!load_params_files(*tbl, ctx, ps.params, err)) {
        return false;
      }

      const auto* pnode = tbl->get("params");

      if (pnode != nullptr) {
        const auto* ptbl = pnode->as_table();

        if (ptbl == nullptr || !parse_params_table(*ptbl, ctx, ps.params, err)) {
          if (ptbl == nullptr) {
            err = "Process '" + ps.name + "': 'params' must be a table.";
          }

          return false;
        }
      }

      ps.remaps = ctx.default_remaps;
      const auto* rnode = tbl->get("remaps");

      if (rnode != nullptr) {
        const auto* rtbl = rnode->as_table();

        if (rtbl == nullptr || !parse_remaps_table(*rtbl, ctx, ps.remaps, err)) {
          if (rtbl == nullptr) {
            err = "Process '" + ps.name + "': 'remaps' must be a table.";
          }

          return false;
        }
      }

      out.processes.push_back(std::move(ps));
    }
  }

  const auto* cnode_root = root.get("component");

  if (cnode_root != nullptr) {
    const auto* carr = cnode_root->as_array();

    if (carr == nullptr) {
      err = "[[component]] must be an array of tables.";
      return false;
    }

    for (const auto& item : *carr) {
      const auto* tbl = item.as_table();

      if (tbl == nullptr) {
        err = "[[component]] entries must be tables.";
        return false;
      }

      if (!validate_allowed_keys(*tbl, "[[component]]",
                                 {"name",
                                  "ns",
                                  "group",
                                  "lib",
                                  "class",
                                  "dedicated",
                                  "params",
                                  "params_file",
                                  "remaps",
                                  "scheduler",
                                  "threads",
                                  "queue",
                                  "queue_strategy",
                                  "enabled",
                                  "autostart",
                                  "required",
                                  "delay_ms",
                                  "stop_timeout_ms",
                                  "watchdog_ms",
                                  "restart",
                                  "restart_delay_ms",
                                  "restart_max_ms",
                                  "start_burst",
                                  "start_window_ms",
                                  "depends_on",
                                  "after",
                                  "ready"},
                                 err)) {
        return false;
      }

      bool comp_enabled = true;

      if (!parse_enabled_flag(*tbl, ctx, comp_enabled, err)) {
        return false;
      }

      if (!comp_enabled) {
        continue;
      }

      bool ok = true;
      ComponentSpec cs;
      cs.name = get_str(*tbl, "name", ctx, "", err, ok);

      if (!ok) {
        return false;
      }

      cs.ns = join_ns(ctx.default_ns, get_str(*tbl, "ns", ctx, "", err, ok));
      cs.group = get_str(*tbl, "group", ctx, ctx.default_group, err, ok);
      cs.library = get_str(*tbl, "lib", ctx, "", err, ok);
      cs.class_name = get_str(*tbl, "class", ctx, "", err, ok);
      cs.autostart = get_bool(*tbl, "autostart", true, err, ok);
      cs.required = get_bool(*tbl, "required", false, err, ok);
      cs.delay_ms = static_cast<uint32_t>(get_i64_range(*tbl, "delay_ms", 0, 0, kMaxU32, err, ok));
      cs.stop_timeout_ms = static_cast<uint32_t>(get_i64_range(*tbl, "stop_timeout_ms", 5000, 200, kMaxU32, err, ok));
      cs.watchdog_ms = static_cast<uint32_t>(get_i64_range(*tbl, "watchdog_ms", 0, 0, kMaxU32, err, ok));
      cs.depends_on = get_str_list(*tbl, "depends_on", ctx, err, ok);
      cs.after = get_str_list(*tbl, "after", ctx, err, ok);

      if (!ok) {
        return false;
      }

      if (!parse_respawn(*tbl, ctx, cs.respawn, err) || !parse_ready_probe(*tbl, ctx, cs.ready, err) ||
          !parse_scheduler_and_queue(*tbl, ctx, true, cs.scheduler, cs.threads, cs.queue_type, cs.queue_strategy,
                                     err)) {
        return false;
      }

      cs.dedicated = (cs.scheduler != SchedulerMode::kShared);

      if (!load_params_files(*tbl, ctx, cs.params, err)) {
        return false;
      }

      const auto* pnode = tbl->get("params");

      if (pnode != nullptr) {
        const auto* ptbl = pnode->as_table();

        if (ptbl == nullptr || !parse_params_table(*ptbl, ctx, cs.params, err)) {
          if (ptbl == nullptr) {
            err = "Component '" + cs.name + "': 'params' must be a table.";
          }

          return false;
        }
      }

      cs.remaps = ctx.default_remaps;
      const auto* rnode = tbl->get("remaps");

      if (rnode != nullptr) {
        const auto* rtbl = rnode->as_table();

        if (rtbl == nullptr || !parse_remaps_table(*rtbl, ctx, cs.remaps, err)) {
          if (rtbl == nullptr) {
            err = "Component '" + cs.name + "': 'remaps' must be a table.";
          }

          return false;
        }
      }

      out.components.push_back(std::move(cs));
    }
  }

  return true;
}

static bool is_executable_file(const std::filesystem::path& p) {
  std::error_code ec;

  if (!std::filesystem::is_regular_file(p, ec)) {
    return false;
  }

#if defined(_WIN32) || defined(_WIN64)
  return true;
#else
  return ::access(Helpers::path_to_string(p).c_str(), X_OK) == 0;
#endif
}

static bool exists_with_pathext(const std::filesystem::path& base) {
  if (is_executable_file(base)) {
    return true;
  }

#if defined(_WIN32) || defined(_WIN64)
  if (!base.has_extension()) {
    std::string pathext = Utils::get_env("PATHEXT");

    if (pathext.empty()) {
      pathext = ".EXE;.CMD;.BAT;.COM";
    }

    std::istringstream pss(pathext);
    std::string ext;

    while (std::getline(pss, ext, ';')) {
      if (ext.empty()) {
        continue;
      }

      std::filesystem::path cand = base;
      cand += ext;

      if (is_executable_file(cand)) {
        return true;
      }
    }
  }
#endif

  return false;
}

static bool program_exists(const std::string& prog, const std::string& cwd) {
  if (prog.empty()) {
    return false;
  }

  std::filesystem::path p(prog);

  if (p.is_absolute() || prog.find('/') != std::string::npos || prog.find('\\') != std::string::npos) {
    if (p.is_relative() && !cwd.empty()) {
      p = std::filesystem::path(cwd) / p;
    }

    return exists_with_pathext(p);
  }

  const std::string path_env = Utils::get_env("PATH");
#if defined(_WIN32) || defined(_WIN64)
  constexpr char kSep = ';';
#else
  constexpr char kSep = ':';
#endif
  std::istringstream iss(path_env);
  std::string dir;

  while (std::getline(iss, dir, kSep)) {
#if defined(_WIN32) || defined(_WIN64)
    if (dir.size() >= 2U && dir.front() == '"' && dir.back() == '"') {
      dir = dir.substr(1U, dir.size() - 2U);
    }
#endif
    if (dir.empty()) {
      continue;
    }

    if (exists_with_pathext(std::filesystem::path(dir) / prog)) {
      return true;
    }
  }

  return false;
}

std::string ValidationReport::format() const {
  std::string out;

  for (const auto& e : errors) {
    if (!out.empty()) {
      out.push_back('\n');
    }

    out.append(e);
  }

  return out;
}

bool load_manifest_file(const std::string& file_path, LaunchManifest& out, std::string& error_out,
                        const std::unordered_map<std::string, std::string>& args_override) {
  out = LaunchManifest{};
  out.source_file = file_path;

  try {
    const auto doc = toml::parse_file(file_path);
    const std::filesystem::path fp(file_path);
    const std::string dir = Helpers::path_to_string(fp.parent_path());
    std::vector<std::string> stack = {canonical_include_key(fp)};

    ParseContext ctx;
    ctx.this_dir = dir.empty() ? "." : dir;
    ctx.args = args_override;
    ctx.is_root = true;
    ctx.depth = 0U;
    ctx.active_stack = &stack;
    return parse_manifest_table(doc, std::move(ctx), out, error_out);
  } catch (const toml::parse_error& e) {
    error_out = "TOML parse error: " + std::string(e.description());
    return false;
  } catch (const std::exception& e) {
    error_out = "Failed to load manifest '" + file_path + "': " + e.what();
    return false;
  }
}

bool load_manifest_string(const std::string& toml_content, const std::string& base_dir, LaunchManifest& out,
                          std::string& error_out, const std::unordered_map<std::string, std::string>& args_override) {
  out = LaunchManifest{};

  try {
    const auto doc = toml::parse(toml_content);
    std::vector<std::string> stack;

    ParseContext ctx;
    ctx.this_dir = base_dir.empty() ? "." : base_dir;
    ctx.args = args_override;
    ctx.is_root = true;
    ctx.depth = 0U;
    ctx.active_stack = &stack;
    return parse_manifest_table(doc, std::move(ctx), out, error_out);
  } catch (const toml::parse_error& e) {
    error_out = "TOML parse error: " + std::string(e.description());
    return false;
  } catch (const std::exception& e) {
    error_out = "Failed to parse manifest string: " + std::string(e.what());
    return false;
  }
}

ValidationReport validate_manifest(const LaunchManifest& manifest, bool check_programs) {
  ValidationReport rep;
  std::unordered_set<std::string> names;
  std::unordered_map<std::string, std::vector<std::string>> preds;

  auto register_name = [&](const std::string& name) -> bool {
    if (name.empty()) {
      rep.errors.emplace_back("Unit name cannot be empty.");
      return false;
    }

    if (name.front() == '@') {
      rep.errors.push_back("Unit name '" + name + "' cannot start with '@' (reserved for groups).");
      return false;
    }

    if (!names.insert(name).second) {
      rep.errors.push_back("Duplicate unit name '" + name + "'.");
      return false;
    }

    return true;
  };

  for (const auto& ps : manifest.processes) {
    const bool valid_name = register_name(ps.name);

    if (ps.program.empty()) {
      rep.errors.push_back("Process '" + ps.name + "' has empty cmd.");
    } else if (check_programs && !program_exists(ps.program, ps.cwd)) {
      rep.errors.push_back("Process '" + ps.name + "' cmd '" + ps.program + "' not found.");
    }

    if (ps.ready.type == ReadyProbeType::kComponent) {
      rep.errors.push_back("Process '" + ps.name + "' cannot use ready.type='component'.");
    }

    if (ps.ready.type == ReadyProbeType::kStdout && ps.ready.pattern.empty()) {
      rep.errors.push_back("Process '" + ps.name + "' ready.type='stdout' requires non-empty pattern.");
    }

    if (valid_name) {
      auto& p = preds[ps.name];
      p.insert(p.end(), ps.depends_on.begin(), ps.depends_on.end());
      p.insert(p.end(), ps.after.begin(), ps.after.end());
    }
  }

  for (const auto& cs : manifest.components) {
    const bool valid_name = register_name(cs.name);

    if (cs.library.empty()) {
      rep.errors.push_back("Component '" + cs.name + "' has empty lib.");
    }

    if (cs.ready.type == ReadyProbeType::kNotify || cs.ready.type == ReadyProbeType::kStdout) {
      rep.errors.push_back("Component '" + cs.name + "' cannot use ready.type='notify' or 'stdout'.");
    }

    if (valid_name) {
      auto& p = preds[cs.name];
      p.insert(p.end(), cs.depends_on.begin(), cs.depends_on.end());
      p.insert(p.end(), cs.after.begin(), cs.after.end());
    }
  }

  if (names.size() > kMaxUnitCount) {
    rep.errors.push_back("Manifest exceeds maximum unit count (" + std::to_string(kMaxUnitCount) + ").");
  }

  std::unordered_map<std::string, size_t> indeg;
  std::unordered_map<std::string, std::vector<std::string>> adj;

  for (const auto& n : names) {
    indeg[n] = 0U;
  }

  for (const auto& [unit, deps] : preds) {
    std::set<std::string> uniq(deps.begin(), deps.end());

    for (const auto& dep : uniq) {
      if (dep == unit) {
        rep.errors.push_back("Unit '" + unit + "' cannot depend on itself.");
      } else if (names.count(dep) == 0U) {
        std::string msg = "Unit '";
        msg.append(unit).append("' references unknown dependency '").append(dep).append("'.");
        rep.errors.push_back(std::move(msg));
      } else {
        adj[dep].push_back(unit);
        ++indeg[unit];
      }
    }
  }

  std::queue<std::string> q;

  for (const auto& [n, d] : indeg) {
    if (d == 0U) {
      q.push(n);
    }
  }

  size_t visited = 0U;

  while (!q.empty()) {
    const std::string cur = q.front();
    q.pop();
    ++visited;

    for (const auto& nxt : adj[cur]) {
      if (--indeg[nxt] == 0U) {
        q.push(nxt);
      }
    }
  }

  if (visited != names.size()) {
    rep.errors.emplace_back("Dependency cycle detected in manifest.");
  }

  return rep;
}

std::string params_map_to_json(const LauncherParamMap& params) {
  if (params.empty()) {
    return {};
  }

  try {
    nlohmann::json obj = nlohmann::json::object();

    for (const auto& kv : params) {
      const std::string& key = kv.first;
      std::visit(
          [&obj, &key](const auto& v) {
            using T = std::decay_t<decltype(v)>;

            if constexpr (std::is_same_v<T, std::vector<bool>>) {
              nlohmann::json arr = nlohmann::json::array();

              for (const bool item : v) {
                arr.push_back(item);
              }

              obj[key] = std::move(arr);
            } else {
              obj[key] = v;
            }
          },
          kv.second);
    }

    return obj.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
  } catch (...) {
    return {};
  }
}

std::string string_map_to_json(const std::unordered_map<std::string, std::string>& kv) {
  if (kv.empty()) {
    return {};
  }

  try {
    nlohmann::json obj = nlohmann::json::object();

    for (const auto& entry : kv) {
      obj[entry.first] = entry.second;
    }

    return obj.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
  } catch (...) {
    return {};
  }
}

}  // namespace vlink::launcher
