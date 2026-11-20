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

#pragma once

#include <QCloseEvent>
#include <QDialog>
#include <QResizeEvent>
#include <string>

namespace Ui {
class SettingsDialog;
}

class SettingsDialog : public QDialog {
  Q_OBJECT

 public:
  enum Mode : uint8_t {
    kUnknown = 0,
    kController = 1,
    kListener = 2,
  };

  static bool do_select();

  static Mode get_mode();

  static int get_domain_id();

  static std::string get_dds_impl();

  static std::string get_security_key();

  static bool get_detect_upgrade();

  static bool get_match_version();

  static bool get_native_mode();

  static bool get_reliable_mode();

  static bool get_tcp_mode();

  static bool get_direct_mode();

  static std::string get_allow_ip();

  static std::string get_peer_ip();

  static int get_buf_size();

  static int get_mtu_size();

 private slots:
  void on_checkBox_native_clicked(bool checked);

  void on_pushButton_controller_clicked();

  void on_pushButton_listener_clicked();

  void on_toolButton_show_clicked();

 private:
  explicit SettingsDialog(QWidget* parent = nullptr);

  ~SettingsDialog();

  void save_profile();

  inline static Mode mode_{Mode::kUnknown};
  inline static int domain_id_{0};
  inline static std::string dds_impl_;
  inline static std::string security_key_;
  inline static bool detect_upgrade_{false};
  inline static bool match_version_{false};
  inline static bool native_mode_{false};
  inline static bool reliable_mode_{false};
  inline static bool tcp_mode_{false};
  inline static bool direct_mode_{false};
  inline static std::string allow_ip_;
  inline static std::string peer_ip_;
  inline static int buf_size_{0};
  inline static int mtu_size_{0};
  Ui::SettingsDialog* ui;
  class QRegularExpressionValidator* validator_ip_{nullptr};
  class MainWindow* window_{nullptr};
};

// NOLINTEND
