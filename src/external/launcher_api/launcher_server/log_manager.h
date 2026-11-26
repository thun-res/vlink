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

#include <vlink/base/functional.h>
#include <vlink/base/logger.h>
#include <vlink/base/logger_backend.h>
#include <vlink/base/logger_plugin_interface.h>
#include <vlink/base/macros.h>
#include <vlink/base/plugin.h>
#include <vlink/external/launcher_api.h>
//
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "./launcher_config.h"

namespace vlink {

namespace launcher {

class ScopedComponentLogTag final {
 public:
  explicit ScopedComponentLogTag(std::string_view tag) noexcept;

  ~ScopedComponentLogTag() noexcept;

  [[nodiscard]] static std::string_view current() noexcept;

  static void push_tag(std::string_view tag) noexcept;

  static void pop_tag() noexcept;

 private:
  VLINK_DISALLOW_COPY_AND_ASSIGN(ScopedComponentLogTag)
};

class LogManager final {
 public:
  static constexpr const char* kBuiltinLogdLib = "vlink-launcher_logd";

  using StreamCallback = Function<void(const launcher::LogRecord&)>;

  [[nodiscard]] static bool is_builtin_logd_name(std::string_view name) noexcept;

  LogManager();

  ~LogManager();

  bool init(const std::string& session_id, const LoggerConfig& config, const std::string& log_dir_override,
            bool manage_console, bool manage_file_logger, bool color_output,
            const std::vector<std::string>& plugin_dirs = {}, const std::string& app_name = "vlink-launcher",
            bool builtin_logd_collector = false);

  void shutdown();

  void set_stream_callback(StreamCallback&& callback);

  void set_logd_endpoint(const std::string& url, const std::string& scheme, const std::string& bind_ip,
                         bool native_mode, const std::string& security_key);

  [[nodiscard]] std::string session_dir() const;

  [[nodiscard]] std::string tmp_dir() const;

  [[nodiscard]] bool is_plugin_active() const noexcept;

  [[nodiscard]] bool is_builtin_logd() const noexcept;

  [[nodiscard]] std::string effective_plugin() const;

  [[nodiscard]] std::string unit_log_dir(std::string_view unit_name, bool is_container) const;

  [[nodiscard]] std::unordered_map<std::string, std::string> build_child_log_env(std::string_view unit_name,
                                                                                 bool is_container,
                                                                                 bool inherit_env = true) const;

  void emit_unit_output(std::string_view unit_name, std::string_view raw_line, bool is_stderr, OutputMode mode,
                        bool is_container = false);

  void emit_launcher_message(std::string_view unit_name, Logger::Level level, std::string_view text);

  void ingest_logd_record(const launcher::LogRecord& rec, bool unwrap_bracket_unit = false,
                          bool record_to_history = true);

  void close_unit_backend(std::string_view unit_name);

  [[nodiscard]] std::vector<launcher::LogRecord> query_history(
      const launcher::LogQuery& query, const std::unordered_set<std::string>& allowed_units) const;

  [[nodiscard]] static std::string strip_ansi(std::string_view input);

 private:
  void write_console_line(std::string_view unit_name, Logger::Level level, bool is_stderr, std::string_view clean_text);

  void write_unit_file_line(std::string_view unit_name, Logger::Level level, std::string_view clean_text,
                            bool is_container);

  void write_plugin_line(std::string_view unit_name, Logger::Level level, std::string_view clean_text);

  void record_and_notify(std::string_view unit_name, Logger::Level level, bool is_stderr, std::string_view clean_text);

  static const char* color_for_unit(std::string_view unit_name) noexcept;

  static const char* level_to_env_string(Logger::Level level) noexcept;

  LoggerConfig config_;
  std::string session_dir_;
  std::string tmp_dir_;
  std::vector<std::string> plugin_dirs_;
  std::string effective_child_plugin_;
  std::string logd_url_;
  std::string logd_scheme_;
  std::string logd_bind_ip_;
  std::string logd_security_key_;
  bool logd_native_mode_{false};
  bool manage_console_{false};
  bool manage_file_logger_{false};
  bool registered_file_handler_{false};
  bool color_output_{true};
  bool initialized_{false};
  bool plugin_active_{false};
  bool builtin_logd_active_{false};

  std::mutex console_mtx_;
  mutable std::mutex backend_mtx_;
  Plugin log_plugin_loader_;
  std::shared_ptr<LoggerPluginInterface> log_plugin_;
  std::shared_ptr<LoggerBackend> logd_backend_;
  std::unordered_map<std::string, std::shared_ptr<LoggerBackend>> unit_backends_;

  mutable std::mutex history_mtx_;
  uint64_t next_seq_{1};
  std::deque<launcher::LogRecord> history_;

  std::mutex cb_mtx_;
  StreamCallback stream_cb_;

  VLINK_DISALLOW_COPY_AND_ASSIGN(LogManager)
};

}  // namespace launcher

}  // namespace vlink
