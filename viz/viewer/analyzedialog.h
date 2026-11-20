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

#include <vlink/base/bytes.h>

#include <QCloseEvent>
#include <QDialog>
#include <QResizeEvent>
#include <string>

namespace Ui {
class AnalyzeDialog;
}

class AnalyzeDialog : public QDialog {
  Q_OBJECT

 public:
  enum Type : uint8_t {
    kUnknownType = 0,
    kNumberType,
    kStringType,
    kRawType,
  };

  explicit AnalyzeDialog(QWidget* parent = nullptr);

  ~AnalyzeDialog();

  void init(Type type);

  bool is_number_type() const;

  bool is_string_type() const;

  bool is_raw_type() const;

  void add_number(double number);

  void add_string(const std::string& str);

  void add_raw(const std::string& str);

 protected:
  void closeEvent(QCloseEvent* event) override;

  void resizeEvent(class QResizeEvent* event) override;

 private slots:
  void on_pushButton_pause_clicked();

  void on_pushButton_resume_clicked();

  void on_pushButton_close_clicked();

 private:
  Ui::AnalyzeDialog* ui;
  Type current_type_{kUnknownType};
  bool is_paused{false};

  class MainWindow* window_{nullptr};
};

// NOLINTEND
