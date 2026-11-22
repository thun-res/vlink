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

#include <QDialog>
#include <QFile>
#include <QPointer>

namespace Ui {
class PointDialog;
}

class PointDialog : public QDialog {
  Q_OBJECT

 public:
  explicit PointDialog(QWidget* parent = nullptr);

  ~PointDialog();

  void reload_file(const QString& bag_path);

  void save_file();

  void set_date_time(int64_t date_time = 0, bool is_relative = true);

 protected:
  void showEvent(class QShowEvent* event) override;

  void hideEvent(class QHideEvent* event) override;

  void closeEvent(class QCloseEvent* event) override;

  void resizeEvent(class QResizeEvent* event) override;

 private slots:
  void on_toolButton_jump_clicked();

  void on_toolButton_rename_clicked();

  void on_toolButton_add_clicked();

  void on_toolButton_del_clicked();

  void on_pushButton_close_clicked();

 private:
  void clear_inline_editor(bool commit = false);

 private:
  Ui::PointDialog* ui;

  QPointer<class QLineEdit> line_edit_;

  QFile txt_file_;

  int64_t date_time_{0};

  bool is_relative_time_{true};

 signals:
  void jump_point(int64_t timestamp);
};

// NOLINTEND
