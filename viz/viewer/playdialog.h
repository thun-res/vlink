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

#include <vlink/base/elapsed_timer.h>
#include <vlink/extension/bag_reader.h>

#include <QCloseEvent>
#include <QDialog>
#include <QResizeEvent>
#include <atomic>
#include <memory>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace Ui {
class PlayDialog;
}

class PlayDialog : public QDialog {
  Q_OBJECT

 public:
  explicit PlayDialog(QWidget* parent = nullptr);

  ~PlayDialog();

  enum Status : uint8_t {
    kDisable,
    kStopped,
    kPlaying,
    kPaused,
  };

 private slots:
  void on_pushButton_all_clicked();

  void on_pushButton_unall_clicked();

  void on_pushButton_select_clicked();

  void on_pushButton_start_clicked();

  void on_pushButton_pause_clicked();

  void on_pushButton_resume_clicked();

  void on_pushButton_stop_clicked();

  void on_pushButton_close_clicked();

  void on_checkBox_blank_toggled(bool checked);

  void update_time_label();

  void update_status_for_player(int status);

  void update_progress_for_player(double progress);

 private:
  void update_status();

 protected:
  void closeEvent(QCloseEvent* event) override;

 private:
  Ui::PlayDialog* ui;
  class MainWindow* window_{nullptr};
  std::shared_ptr<vlink::BagReader> player_;
  std::atomic<Status> status_{kDisable};
  std::unordered_set<std::string> url_list_;
  std::unordered_map<std::string, std::string> ser_map_;
  std::unordered_map<std::string, vlink::SchemaType> schema_type_map_;
  std::shared_mutex url_mtx_;

  QTimer* label_timer_{nullptr};
  std::atomic_bool data_has_changed{false};
};

// NOLINTEND
