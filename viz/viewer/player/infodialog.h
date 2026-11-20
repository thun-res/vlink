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

#include <vlink/extension/bag_reader.h>

#include <QDialog>

namespace Ui {
class InfoDialog;
}

class InfoDialog : public QDialog {
  Q_OBJECT

 public:
  explicit InfoDialog(QWidget* parent = nullptr);

  ~InfoDialog();

  void show_information(const vlink::BagReader::Info& info);

 protected:
  void showEvent(class QShowEvent* event) override;

  void hideEvent(class QHideEvent* event) override;

  void closeEvent(class QCloseEvent* event) override;

  void resizeEvent(class QResizeEvent* event) override;

 private slots:
  void on_pushButton_close_clicked();

 private:
  Ui::InfoDialog* ui;
};

// NOLINTEND
