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

#include "./log_manager.h"

#include <vlink/base/elapsed_timer.h>
#include <vlink/base/helpers.h>
#include <vlink/base/terminal_stream.h>
#include <vlink/base/utils.h>
//
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "../launcher_common.h"

namespace vlink {

namespace launcher {

static constexpr size_t kMaxLogTagDepth = 16U;
static thread_local std::array<std::string_view, kMaxLogTagDepth>
    g_component_log_tag_stack{};  // NOLINT(cppcoreguidelines-avoid-non-const-global-variables)
static thread_local size_t g_component_log_tag_depth =
    0U;                                                 // NOLINT(cppcoreguidelines-avoid-non-const-global-variables)
static thread_local bool g_in_log_dispatch = false;     // NOLINT(cppcoreguidelines-avoid-non-const-global-variables)
static thread_local bool g_in_plugin_dispatch = false;  // NOLINT(cppcoreguidelines-avoid-non-const-global-variables)

static std::string resolve_plugin_file_path(const std::string& plugin_name, const std::deque<std::string>& dirs,
                                            bool* found_out = nullptr) {
  if (found_out != nullptr) {
    *found_out = false;
  }

  if (plugin_name.empty()) {
    return {};
  }

#if defined(_WIN32) || defined(_WIN64)
  static constexpr const char* kPrefix = "";
  static constexpr const char* kSuffix = ".dll";
#elif defined(__APPLE__)
  static constexpr const char* kPrefix = "lib";
  static constexpr const char* kSuffix = ".dylib";
#else
  static constexpr const char* kPrefix = "lib";
  static constexpr const char* kSuffix = ".so";
#endif

  std::error_code ec;
  const bool has_sep = plugin_name.find_first_of("/\\") != std::string::npos;
  const bool has_ext = Helpers::has_endwith(plugin_name, kSuffix);

  if (has_sep || has_ext) {
    const std::filesystem::path p(plugin_name);

    if (std::filesystem::is_regular_file(p, ec)) {
      if (found_out != nullptr) {
        *found_out = true;
      }

      const auto abs_p = std::filesystem::absolute(p, ec);
      return Helpers::path_to_string(ec ? p : abs_p);
    }

    return plugin_name;
  }

  const std::string lib_file = std::string(kPrefix) + plugin_name + kSuffix;

  for (const auto& dir : dirs) {
    if (dir.empty()) {
      continue;
    }

    const std::filesystem::path candidate = std::filesystem::path(dir) / lib_file;

    if (std::filesystem::is_regular_file(candidate, ec)) {
      if (found_out != nullptr) {
        *found_out = true;
      }

      const auto abs_p = std::filesystem::absolute(candidate, ec);
      return Helpers::path_to_string(ec ? candidate : abs_p);
    }
  }

  return plugin_name;
}

ScopedComponentLogTag::ScopedComponentLogTag(std::string_view tag) noexcept { push_tag(tag); }

ScopedComponentLogTag::~ScopedComponentLogTag() noexcept { pop_tag(); }

std::string_view ScopedComponentLogTag::current() noexcept {
  if (g_component_log_tag_depth == 0U) {
    return {};
  }

  const size_t idx = std::min(g_component_log_tag_depth, kMaxLogTagDepth) - 1U;
  return g_component_log_tag_stack[idx];
}

void ScopedComponentLogTag::push_tag(std::string_view tag) noexcept {
  if VLIKELY (g_component_log_tag_depth < kMaxLogTagDepth) {
    g_component_log_tag_stack[g_component_log_tag_depth] = tag;
  }

  ++g_component_log_tag_depth;
}

void ScopedComponentLogTag::pop_tag() noexcept {
  if VLIKELY (g_component_log_tag_depth > 0U) {
    --g_component_log_tag_depth;

    if (g_component_log_tag_depth < kMaxLogTagDepth) {
      g_component_log_tag_stack[g_component_log_tag_depth] = {};
    }
  }
}

bool LogManager::is_builtin_logd_name(std::string_view name) noexcept {
  return name == "logd" || name == kBuiltinLogdLib;
}

LogManager::LogManager() = default;

LogManager::~LogManager() { shutdown(); }

bool LogManager::init(const std::string& session_id, const LoggerConfig& config, const std::string& log_dir_override,
                      bool manage_console, bool manage_file_logger, bool color_output,
                      const std::vector<std::string>& plugin_dirs, const std::string& app_name,
                      bool builtin_logd_collector) {
  {
    std::lock_guard lock(backend_mtx_);

    if (initialized_) {
      return true;
    }
  }

  config_ = config;
  plugin_dirs_ = plugin_dirs;
  manage_console_ = manage_console;
  manage_file_logger_ = manage_file_logger;
  registered_file_handler_ = false;
  color_output_ = color_output;
  plugin_active_ = false;
  builtin_logd_active_ = false;
  effective_child_plugin_.clear();
  logd_url_.clear();
  logd_scheme_.clear();
  logd_bind_ip_.clear();
  logd_security_key_.clear();
  logd_native_mode_ = false;

  std::string base_dir = !log_dir_override.empty() ? log_dir_override : config_.dir;

  if (base_dir.empty()) {
    const std::string env_dir = Utils::get_env("VLINK_LOG_DIR");
    base_dir = env_dir.empty() ? (Utils::get_tmp_dir() + "/vlink-launcher") : env_dir;
  }

  std::error_code ec;
  const auto abs_base = std::filesystem::absolute(std::filesystem::path(base_dir), ec);

  if (!ec && !abs_base.empty()) {
    base_dir = Helpers::path_to_string(abs_base);
  }

  {
    std::lock_guard lock(backend_mtx_);
    session_dir_ = base_dir + "/" + session_id;
    tmp_dir_ = session_dir_ + "/tmp";
  }

  std::filesystem::create_directories(session_dir_ + "/launcher", ec);
  std::filesystem::create_directories(session_dir_ + "/nodes", ec);
  std::filesystem::create_directories(session_dir_ + "/containers", ec);
  std::filesystem::create_directories(tmp_dir_, ec);

  {
    std::lock_guard lock(history_mtx_);
    history_.clear();
    next_seq_ = 1U;
  }

  std::string plugin_warn;
  const std::string effective_app = app_name.empty() ? std::string("vlink-launcher") : app_name;

  if (!config_.plugin.empty() && config_.file_level < Logger::kOff) {
    std::deque<std::string> search_paths;

    for (const auto& dir : plugin_dirs_) {
      if (!dir.empty()) {
        search_paths.push_back(dir);
      }
    }

    for (auto& def_dir : Plugin::default_search_path()) {
      search_paths.push_back(std::move(def_dir));
    }

    if (builtin_logd_collector && is_builtin_logd_name(config_.plugin)) {
      bool found = false;
      const std::string resolved = resolve_plugin_file_path(kBuiltinLogdLib, search_paths, &found);
      bool abi_ok = false;

      if (found) {
        Plugin probe;
        probe.set_log_level(Logger::kOff);
        auto probed = probe.load<LoggerPluginInterface>(resolved, 1, 0);
        abi_ok = static_cast<bool>(probed);
        probed.reset();
        probe.clear();
      }

      if (found && abi_ok) {
        LoggerBackend::Config lcfg;
        lcfg.app_name = "logd";
        lcfg.log_path = session_dir_ + "/logd";
        lcfg.max_file_size = config_.max_file_size;
        lcfg.max_files = config_.max_files;
        lcfg.queue_size = 4096U;
        lcfg.flush_interval_ms = config_.flush_delay_ms;
        lcfg.fixed_filename = config_.fixed_filename;
        lcfg.append = config_.append;
        lcfg.block_when_full = false;
        lcfg.use_utc = config_.utc;

        std::filesystem::create_directories(lcfg.log_path, ec);
        auto backend = std::make_shared<LoggerBackend>(std::move(lcfg), [](std::string_view) {});

        if (backend && !backend->has_error()) {
          {
            std::lock_guard lock(backend_mtx_);
            logd_backend_ = std::move(backend);
            builtin_logd_active_ = true;
            plugin_active_ = true;
            effective_child_plugin_ = resolved;
          }
        } else {
          plugin_warn = "LogManager: Failed to initialize built-in logd sink at '" + session_dir_ + "/logd'";

          if (config_.plugin_strict) {
            std::fprintf(stderr, "%s (plugin_strict=true).\n", plugin_warn.c_str());
            std::fflush(stderr);
            return false;
          }

          plugin_warn.append("; falling back to per-unit file logger.");
        }
      } else {
        plugin_warn =
            found ? ("LogManager: Built-in logd plugin '" + resolved + "' failed ABI load check")
                  : ("LogManager: Built-in logd plugin library '" + std::string(kBuiltinLogdLib) + "' not found");

        if (config_.plugin_strict) {
          std::fprintf(stderr, "%s (plugin_strict=true).\n", plugin_warn.c_str());
          std::fflush(stderr);
          return false;
        }

        plugin_warn.append("; falling back to per-unit file logger.");
      }
    } else {
      const std::string load_target =
          is_builtin_logd_name(config_.plugin) ? std::string(kBuiltinLogdLib) : config_.plugin;
      log_plugin_loader_.set_log_level(Logger::kOff);
      auto loaded = log_plugin_loader_.load<LoggerPluginInterface>(load_target, 1, 0, "", search_paths);
      bool plugin_ok = false;

      if (loaded) {
        if (manage_file_logger_) {
          Logger::set_file_level(config_.file_level);
          Logger::register_file_handler([this](Logger::Level level, std::string_view msg) {
            if VUNLIKELY (g_in_plugin_dispatch) {
              return;
            }

            const std::string_view tag = ScopedComponentLogTag::current();
            const std::string_view unit = tag.empty() ? std::string_view("launcher") : tag;
            const std::string clean = strip_ansi(msg);
            write_plugin_line(unit, level, clean);
          });
          registered_file_handler_ = true;
        }

        struct PluginDispatchGuard final {
          PluginDispatchGuard() noexcept { g_in_plugin_dispatch = true; }
          ~PluginDispatchGuard() noexcept { g_in_plugin_dispatch = false; }
        } guard;
        (void)guard;
        plugin_ok = loaded->init(effective_app);
      }

      if (plugin_ok) {
        {
          std::lock_guard lock(backend_mtx_);
          log_plugin_ = std::move(loaded);
          plugin_active_ = true;
          effective_child_plugin_ = resolve_plugin_file_path(load_target, search_paths);
        }
      } else {
        if (registered_file_handler_) {
          Logger::register_file_handler(nullptr);
          registered_file_handler_ = false;
        }

        loaded.reset();
        log_plugin_loader_.clear();
        plugin_warn = "LogManager: Failed to load or initialize logger plugin '" + config_.plugin + "'";

        if (config_.plugin_strict) {
          std::fprintf(stderr, "%s (plugin_strict=true).\n", plugin_warn.c_str());
          std::fflush(stderr);
          return false;
        }

        plugin_warn.append("; falling back to built-in file logger.");
      }
    }
  }

  if (plugin_active_ && manage_file_logger_ && !registered_file_handler_) {
    Logger::set_file_level(config_.file_level);
    Logger::register_file_handler([this](Logger::Level level, std::string_view msg) {
      if VUNLIKELY (g_in_plugin_dispatch) {
        return;
      }

      const std::string_view tag = ScopedComponentLogTag::current();
      const std::string_view unit = tag.empty() ? std::string_view("launcher") : tag;
      const std::string clean = strip_ansi(msg);
      write_plugin_line(unit, level, clean);
    });
    registered_file_handler_ = true;
  }

  if (manage_console_) {
    VLINK_TERM_OUT.init();
    Logger::set_console_level(config_.console_level);
    Logger::set_file_level(config_.file_level);
    Logger::init(effective_app, session_dir_ + "/launcher");

    Logger::register_console_handler([this](Logger::Level level, std::string_view msg) {
      if VUNLIKELY (g_in_log_dispatch) {
        return;
      }

      struct DispatchGuard final {
        DispatchGuard() noexcept { g_in_log_dispatch = true; }
        ~DispatchGuard() noexcept { g_in_log_dispatch = false; }
      } guard;
      (void)guard;

      const std::string_view tag = ScopedComponentLogTag::current();
      const std::string_view unit = tag.empty() ? std::string_view("launcher") : tag;
      const std::string clean = strip_ansi(msg);
      write_console_line(unit, level, level >= Logger::kWarn, clean);
      record_and_notify(unit, level, level >= Logger::kWarn, clean);
    });
  }

  {
    std::lock_guard lock(backend_mtx_);
    initialized_ = true;
  }

  if (!plugin_warn.empty()) {
    emit_launcher_message("launcher", Logger::kWarn, plugin_warn);
  }

  return true;
}

void LogManager::shutdown() {
  std::unordered_map<std::string, std::shared_ptr<LoggerBackend>> backends;
  std::shared_ptr<LoggerBackend> logd_sink;
  std::shared_ptr<LoggerPluginInterface> plugin_to_close;
  bool should_restore_console = false;
  bool should_restore_file = false;

  {
    std::lock_guard lock(backend_mtx_);

    if (!initialized_) {
      return;
    }

    initialized_ = false;
    plugin_active_ = false;
    builtin_logd_active_ = false;
    should_restore_console = manage_console_;
    should_restore_file = registered_file_handler_;
    registered_file_handler_ = false;
    backends.swap(unit_backends_);
    logd_sink.swap(logd_backend_);
    plugin_to_close.swap(log_plugin_);
  }

  if (should_restore_console) {
    Logger::register_console_handler(nullptr);
    Logger::flush();
  }

  if (should_restore_file) {
    Logger::set_file_level(Logger::kOff);
    Logger::register_file_handler(nullptr);
  }

  if (plugin_to_close) {
    {
      struct PluginDispatchGuard final {
        PluginDispatchGuard() noexcept { g_in_plugin_dispatch = true; }
        ~PluginDispatchGuard() noexcept { g_in_plugin_dispatch = false; }
      } guard;
      (void)guard;
      plugin_to_close->flush();
    }

    plugin_to_close.reset();
    log_plugin_loader_.clear();
  }

  if (logd_sink) {
    logd_sink->flush();
    logd_sink.reset();
  }

  for (auto& entry : backends) {
    auto& backend = entry.second;

    if (backend) {
      backend->flush();
      backend.reset();
    }
  }
}

void LogManager::set_stream_callback(StreamCallback&& callback) {
  std::lock_guard lock(cb_mtx_);
  stream_cb_ = std::move(callback);
}

void LogManager::set_logd_endpoint(const std::string& url, const std::string& scheme, const std::string& bind_ip,
                                   bool native_mode, const std::string& security_key) {
  std::lock_guard lock(backend_mtx_);
  logd_url_ = url;
  logd_scheme_ = scheme;
  logd_bind_ip_ = bind_ip;
  logd_native_mode_ = native_mode;
  logd_security_key_ = security_key;
}

std::string LogManager::session_dir() const {
  std::lock_guard lock(backend_mtx_);
  return session_dir_;
}

std::string LogManager::tmp_dir() const {
  std::lock_guard lock(backend_mtx_);
  return tmp_dir_;
}

bool LogManager::is_plugin_active() const noexcept {
  std::lock_guard lock(backend_mtx_);
  return plugin_active_;
}

bool LogManager::is_builtin_logd() const noexcept {
  std::lock_guard lock(backend_mtx_);
  return builtin_logd_active_;
}

std::string LogManager::effective_plugin() const {
  std::lock_guard lock(backend_mtx_);
  return plugin_active_ ? effective_child_plugin_ : std::string();
}

std::string LogManager::unit_log_dir(std::string_view unit_name, bool is_container) const {
  const std::string subdir = is_container ? "/containers/" : "/nodes/";
  const std::string dir = session_dir_ + subdir + sanitize_identifier(unit_name, true);
  std::error_code ec;
  std::filesystem::create_directories(dir, ec);
  return dir;
}

std::unordered_map<std::string, std::string> LogManager::build_child_log_env(std::string_view unit_name,
                                                                             bool is_container,
                                                                             bool inherit_env) const {
  std::unordered_map<std::string, std::string> env;
  env["VLINK_LOG_DIR"] = unit_log_dir(unit_name, is_container);
  env["VLINK_LOG_PID_ENABLE"] = "1";
  env["VLINK_LOG_CONSOLE_LEVEL"] = level_to_env_string(config_.console_level);
  env["VLINK_LOG_FILE_LEVEL"] = level_to_env_string(config_.file_level);
  env["VLINK_LOG_MAX_SIZE"] = std::to_string(config_.max_file_size);
  env["VLINK_LOG_MAX_COUNT"] = std::to_string(config_.max_files);
  env["VLINK_LOG_FLUSH_DELAY"] = std::to_string(config_.flush_delay_ms);
  env["VLINK_LOG_STORE_STRATEGY"] = config_.fixed_filename ? "1" : "0";
  env["VLINK_LOG_OPEN_APPEND"] = config_.append ? "1" : "0";
  env["VLINK_LOG_UTC_ENABLE"] = config_.utc ? "1" : "0";
  env["VLINK_LOG_CONSOLE_FMT"] = config_.console_fmt ? "1" : "0";

  std::string merged_plugin_dirs;
  std::unordered_set<std::string> seen_dirs;

  auto append_plugin_dir = [&](std::string_view dir) {
    const std::string_view trimmed = Helpers::trim_string_view(dir);

    if (trimmed.empty()) {
      return;
    }

    const std::string key(trimmed);

    if (seen_dirs.insert(key).second) {
      if (!merged_plugin_dirs.empty()) {
        merged_plugin_dirs.push_back(',');
      }

      merged_plugin_dirs.append(key);
    }
  };

  for (const auto& dir : plugin_dirs_) {
    append_plugin_dir(dir);
  }

  if (inherit_env) {
    const std::string existing_dirs = Utils::get_env("VLINK_PLUGIN_DIR");

    for (const auto& dir : Helpers::split_any(existing_dirs)) {
      append_plugin_dir(dir);
    }
  }

  if (!merged_plugin_dirs.empty()) {
    env["VLINK_PLUGIN_DIR"] = std::move(merged_plugin_dirs);
  }

  std::string active_plugin;
  std::string logd_url;
  std::string logd_scheme;
  std::string logd_bind_ip;
  std::string logd_key;
  bool logd_active = false;
  bool logd_native = false;

  {
    std::lock_guard lock(backend_mtx_);

    if (plugin_active_) {
      active_plugin = effective_child_plugin_;
    }

    logd_active = builtin_logd_active_;
    logd_url = logd_url_;
    logd_scheme = logd_scheme_;
    logd_bind_ip = logd_bind_ip_;
    logd_key = logd_security_key_;
    logd_native = logd_native_mode_;
  }

  if (logd_active && !logd_url.empty() && logd_scheme != "intra") {
    env["VLINK_LOGD_URL"] = logd_url;
    env["VLINK_LOGD_UNIT"] = std::string(unit_name);

    if (!logd_scheme.empty()) {
      env["VLINK_LOGD_SCHEME"] = logd_scheme;
    }

    if (!logd_bind_ip.empty()) {
      env["VLINK_LOGD_BIND_IP"] = logd_bind_ip;
    }

    if (logd_native) {
      env["VLINK_LOGD_NATIVE"] = "1";
    }

    if (!logd_key.empty()) {
      env["VLINK_LOGD_KEY"] = logd_key;
    }
  }

  if (is_container) {
    env["VLINK_LOG_PLUGIN"] = "";
  } else if (!active_plugin.empty() && (!logd_active || (!logd_url.empty() && logd_scheme != "intra"))) {
    env["VLINK_LOG_PLUGIN"] = active_plugin;
  }

  return env;
}

void LogManager::emit_unit_output(std::string_view unit_name, std::string_view raw_line, bool is_stderr,
                                  OutputMode mode, bool is_container) {
  const std::string clean = config_.strip_ansi ? strip_ansi(raw_line) : std::string(raw_line);

  if (clean.empty()) {
    return;
  }

  const Logger::Level level = is_stderr ? Logger::kWarn : Logger::kInfo;

  if ((mode == OutputMode::kScreen || mode == OutputMode::kBoth) && level >= config_.console_level) {
    write_console_line(unit_name, level, is_stderr, clean);
  }

  if ((mode == OutputMode::kLog || mode == OutputMode::kBoth) && config_.capture_to_file &&
      level >= config_.file_level) {
    if (is_plugin_active() && config_.plugin_capture && !is_container) {
      write_plugin_line(unit_name, level, clean);
    } else {
      write_unit_file_line(unit_name, level, clean, is_container);
    }
  }

  if (mode != OutputMode::kNone) {
    record_and_notify(unit_name, level, is_stderr, clean);
  }
}

void LogManager::emit_launcher_message(std::string_view unit_name, Logger::Level level, std::string_view text) {
  if (level >= config_.console_level) {
    write_console_line(unit_name, level, level >= Logger::kWarn, text);
  }

  if (level >= config_.file_level) {
    if (is_plugin_active()) {
      write_plugin_line(unit_name, level, text);
    } else if (config_.capture_to_file) {
      write_unit_file_line(unit_name, level, text, false);
    }
  }

  record_and_notify(unit_name, level, level >= Logger::kWarn, text);
}

void LogManager::ingest_logd_record(const launcher::LogRecord& rec, bool unwrap_bracket_unit, bool record_to_history) {
  std::string unit = rec.unit_name.empty() ? std::string("external") : rec.unit_name;
  std::string text = config_.strip_ansi ? strip_ansi(rec.text) : rec.text;

  if (text.empty()) {
    return;
  }

  if (unwrap_bracket_unit && text.size() > 3U && text[0] == '[') {
    const size_t close_pos = text.find("] ");

    if (close_pos != std::string::npos && close_pos > 1U) {
      unit = text.substr(1U, close_pos - 1U);
      text.erase(0U, close_pos + 2U);
    }
  }

  const int clamped =
      std::clamp(static_cast<int>(rec.level), static_cast<int>(Logger::kTrace), static_cast<int>(Logger::kFatal));
  const auto level = static_cast<Logger::Level>(clamped);

  if (level >= config_.file_level) {
    write_plugin_line(unit, level, text);
  }

  if (record_to_history || level >= Logger::kError || level < config_.console_level) {
    record_and_notify(unit, level, rec.is_stderr || level >= Logger::kWarn, text);
  }
}

void LogManager::close_unit_backend(std::string_view unit_name) {
  std::shared_ptr<LoggerBackend> backend;

  {
    std::lock_guard lock(backend_mtx_);
    auto it = unit_backends_.find(std::string(unit_name));

    if (it == unit_backends_.end()) {
      return;
    }

    backend = std::move(it->second);
    unit_backends_.erase(it);
  }

  if (backend) {
    backend->flush();
  }
}

std::vector<launcher::LogRecord> LogManager::query_history(const launcher::LogQuery& query,
                                                           const std::unordered_set<std::string>& allowed_units) const {
  const size_t limit = std::min<size_t>(kMaxLogBatch, query.limit == 0U ? 100U : query.limit);
  std::vector<launcher::LogRecord> matched;
  std::lock_guard lock(history_mtx_);

  if (query.after_seq > 0U) {
    matched.reserve(std::min(limit, history_.size()));

    for (const auto& rec : history_) {
      if (rec.seq <= query.after_seq) {
        continue;
      }

      if (rec.level < query.min_level) {
        continue;
      }

      if (query.stderr_only && !rec.is_stderr) {
        continue;
      }

      if (!allowed_units.empty() && allowed_units.count(rec.unit_name) == 0U) {
        continue;
      }

      if (!query.grep.empty() && rec.text.find(query.grep) == std::string::npos) {
        continue;
      }

      matched.push_back(rec);

      if (matched.size() >= limit) {
        break;
      }
    }

    return matched;
  }

  std::vector<launcher::LogRecord> reversed;
  reversed.reserve(std::min(limit, history_.size()));

  for (size_t idx = history_.size(); idx > 0U; --idx) {
    const auto& rec = history_[idx - 1U];

    if (rec.level < query.min_level) {
      continue;
    }

    if (query.stderr_only && !rec.is_stderr) {
      continue;
    }

    if (!allowed_units.empty() && allowed_units.count(rec.unit_name) == 0U) {
      continue;
    }

    if (!query.grep.empty() && rec.text.find(query.grep) == std::string::npos) {
      continue;
    }

    reversed.push_back(rec);

    if (reversed.size() >= limit) {
      break;
    }
  }

  std::reverse(reversed.begin(), reversed.end());
  return reversed;
}

std::string LogManager::strip_ansi(std::string_view input) {
  std::string out;
  out.reserve(input.size());
  size_t i = 0;

  while (i < input.size()) {
    if (input[i] == '\033' && (i + 1U) < input.size() && input[i + 1U] == '[') {
      i += 2U;

      while (i < input.size()) {
        const auto c = static_cast<unsigned char>(input[i]);
        ++i;

        if (c >= 0x40U && c <= 0x7EU) {
          break;
        }
      }

      continue;
    }

    if (input[i] != '\r') {
      out.push_back(input[i]);
    }

    ++i;
  }

  while (!out.empty() && out.back() == '\n') {
    out.pop_back();
  }

  return out;
}

void LogManager::write_console_line(std::string_view unit_name, Logger::Level level, bool is_stderr,
                                    std::string_view clean_text) {
  std::string line;
  line.reserve(unit_name.size() + clean_text.size() + 32U);

  const bool use_color = color_output_ && VLINK_TERM_OUT.is_initialized() && VLINK_TERM_OUT.is_tty();

  if (use_color) {
    line.append(color_for_unit(unit_name)).append("[").append(unit_name).append("]\033[0m ");

    if (level >= Logger::kError) {
      line.append("\033[31m").append(clean_text).append("\033[0m\n");
    } else if (is_stderr || level == Logger::kWarn) {
      line.append("\033[33m").append(clean_text).append("\033[0m\n");
    } else {
      line.append(clean_text).append("\n");
    }
  } else {
    line.append("[").append(unit_name).append("] ").append(clean_text).append("\n");
  }

  std::lock_guard lock(console_mtx_);

  if (VLINK_TERM_OUT.is_initialized()) {
    VLINK_TERM_OUT.write_raw(line.data(), line.size());
    VLINK_TERM_OUT.flush();
  } else {
    std::fwrite(line.data(), 1U, line.size(), is_stderr ? stderr : stdout);
    std::fflush(is_stderr ? stderr : stdout);
  }
}

void LogManager::write_unit_file_line(std::string_view unit_name, Logger::Level level, std::string_view clean_text,
                                      bool is_container) {
  std::shared_ptr<LoggerBackend> sink;

  {
    std::lock_guard lock(backend_mtx_);

    if VUNLIKELY (!initialized_) {
      return;
    }

    const std::string key(unit_name);
    auto it = unit_backends_.find(key);

    if (it == unit_backends_.end()) {
      LoggerBackend::Config cfg;
      cfg.app_name = sanitize_identifier(unit_name, true);
      cfg.log_path = unit_log_dir(unit_name, is_container) + "/captured";
      cfg.max_file_size = config_.max_file_size;
      cfg.max_files = config_.max_files;
      cfg.queue_size = 1024U;
      cfg.flush_interval_ms = config_.flush_delay_ms;
      cfg.fixed_filename = config_.fixed_filename;
      cfg.append = config_.append;
      cfg.block_when_full = false;
      cfg.use_utc = config_.utc;

      std::error_code ec;
      std::filesystem::create_directories(cfg.log_path, ec);

      auto backend = std::make_shared<LoggerBackend>(std::move(cfg), [](std::string_view) {});
      auto inserted = unit_backends_.emplace(key, std::move(backend));
      it = inserted.first;
    }

    sink = it->second;
  }

  if (sink && !sink->has_error()) {
    struct PluginDispatchGuard final {
      PluginDispatchGuard() noexcept { g_in_plugin_dispatch = true; }
      ~PluginDispatchGuard() noexcept { g_in_plugin_dispatch = false; }
    } guard;
    (void)guard;
    (void)sink->log(level, clean_text);
  }
}

void LogManager::write_plugin_line(std::string_view unit_name, Logger::Level level, std::string_view clean_text) {
  if VUNLIKELY (g_in_plugin_dispatch) {
    return;
  }

  std::string line;
  line.reserve(unit_name.size() + clean_text.size() + 4U);
  line.append("[").append(unit_name).append("] ").append(clean_text);

  std::shared_ptr<LoggerBackend> logd_sink;
  std::shared_ptr<LoggerPluginInterface> plugin;

  {
    std::lock_guard lock(backend_mtx_);

    if (!plugin_active_) {
      return;
    }

    if (builtin_logd_active_ && logd_backend_) {
      logd_sink = logd_backend_;
    } else if (log_plugin_) {
      plugin = log_plugin_;
    } else {
      return;
    }
  }

  struct PluginDispatchGuard final {
    PluginDispatchGuard() noexcept { g_in_plugin_dispatch = true; }
    ~PluginDispatchGuard() noexcept { g_in_plugin_dispatch = false; }
  } guard;
  (void)guard;

  if (logd_sink) {
    if (!logd_sink->has_error()) {
      (void)logd_sink->log(level, line);

      if VUNLIKELY (level == Logger::kFatal) {
        logd_sink->flush();
      }
    }

    return;
  }

  (void)plugin->log(static_cast<int>(level), line);

  if VUNLIKELY (level == Logger::kFatal) {
    plugin->flush();
  }
}

void LogManager::record_and_notify(std::string_view unit_name, Logger::Level level, bool is_stderr,
                                   std::string_view clean_text) {
  launcher::LogRecord rec;
  rec.timestamp_ms = ElapsedTimer::get_sys_timestamp(ElapsedTimer::kMilli);
  rec.unit_name = std::string(unit_name);
  rec.level = static_cast<uint8_t>(level);
  rec.is_stderr = is_stderr;

  if (clean_text.size() > kMaxLogStr) {
    rec.text.assign(clean_text.data(), kMaxLogStr - 16U);
    rec.text.append("...[truncated]");
  } else {
    rec.text.assign(clean_text.data(), clean_text.size());
  }

  {
    std::lock_guard lock(history_mtx_);
    rec.seq = next_seq_++;

    if (config_.history_max_records > 0U) {
      if (history_.size() >= config_.history_max_records) {
        history_.pop_front();
      }

      history_.push_back(rec);
    }
  }

  StreamCallback cb;

  {
    std::lock_guard lock(cb_mtx_);
    cb = stream_cb_;
  }

  if (cb) {
    cb(rec);
  }
}

const char* LogManager::color_for_unit(std::string_view unit_name) noexcept {
  static constexpr const char* kPalette[] = {
      "\033[36m", "\033[32m", "\033[33m", "\033[35m", "\033[34m", "\033[96m",
  };
  static constexpr size_t kPaletteSize = sizeof(kPalette) / sizeof(kPalette[0]);
  const uint32_t hash = Helpers::get_hash_code(std::string(unit_name));
  return kPalette[hash % kPaletteSize];
}

const char* LogManager::level_to_env_string(Logger::Level level) noexcept {
  switch (level) {
    case Logger::kTrace:
      return "Trace";
    case Logger::kDebug:
      return "Debug";
    case Logger::kInfo:
      return "Info";
    case Logger::kWarn:
      return "Warn";
    case Logger::kError:
      return "Error";
    case Logger::kFatal:
      return "Fatal";
    default:
      return "Off";
  }
}

}  // namespace launcher

}  // namespace vlink
