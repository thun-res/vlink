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

namespace Ui {
class AboutDialog;
}

class AboutDialog : public QDialog {
  Q_OBJECT

 public:
  explicit AboutDialog(QWidget* parent = nullptr);

  ~AboutDialog();

 private slots:
  void on_pushButton_open_clicked();

  void on_pushButton_ok_clicked();

  void on_label_vlink_linkActivated(const QString& link);

  void on_label_github_linkActivated(const QString& link);

 private:
  Ui::AboutDialog* ui;
  class MainWindow* window_{nullptr};
};

// NOLINTEND
