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

// NOLINTBEGIN

#include <vlink/base/logger_plugin_interface.h>
#include <vlink/base/utils.h>
//
#include <fstream>
#include <mutex>
#include <string>
#include <string_view>

class TestLogdPlugin final : public vlink::LoggerPluginInterface {
  VLINK_PLUGIN_REGISTER(vlink::LoggerPluginInterface)

 public:
  TestLogdPlugin() = default;

  ~TestLogdPlugin() override { flush(); }

  bool init(std::string_view app_name) noexcept override {
    try {
      if (vlink::Utils::get_env("VLINK_TEST_LOGD_FAIL_INIT") == "1") {
        return false;
      }

      std::lock_guard lock(mtx_);
      app_name_.assign(app_name.data(), app_name.size());
      out_path_ = vlink::Utils::get_env("VLINK_TEST_LOGD_OUT");

      if (!out_path_.empty()) {
        std::ofstream ofs(out_path_, std::ios::out | std::ios::app);

        if (!ofs.is_open()) {
          return false;
        }

        ofs << "INIT pid=" << vlink::Utils::get_pid_str() << " app=" << app_name_ << "\n";
        ofs.flush();
      }

      return true;
    } catch (...) {
      return false;
    }
  }

  bool log(int level, std::string_view str) noexcept override {
    try {
      std::lock_guard lock(mtx_);

      if (out_path_.empty()) {
        return true;
      }

      std::ofstream ofs(out_path_, std::ios::out | std::ios::app);

      if (!ofs.is_open()) {
        return false;
      }

      ofs << "LOG pid=" << vlink::Utils::get_pid_str() << " app=" << app_name_ << " lv=" << level << " " << str << "\n";
      return true;
    } catch (...) {
      return false;
    }
  }

  void flush() noexcept override {
    try {
      std::lock_guard lock(mtx_);

      if (out_path_.empty()) {
        return;
      }

      std::ofstream ofs(out_path_, std::ios::out | std::ios::app);

      if (ofs.is_open()) {
        ofs << "FLUSH pid=" << vlink::Utils::get_pid_str() << " app=" << app_name_ << "\n";
        ofs.flush();
      }
    } catch (...) {
    }
  }

 private:
  std::mutex mtx_;
  std::string app_name_;
  std::string out_path_;
};

VLINK_PLUGIN_DECLARE(TestLogdPlugin, 1, 0)

// NOLINTEND
