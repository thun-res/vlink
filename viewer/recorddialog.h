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
#include <vlink/extension/bag_writer.h>
#include <vlink/impl/calculate_sample.h>

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
class RecordDialog;
}

class RecordDialog : public QDialog {
  Q_OBJECT

 public:
  explicit RecordDialog(QWidget* parent = nullptr);

  ~RecordDialog();

  enum Status : uint8_t {
    kDisable,
    kStopped,
    kRecording,
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

  void update_time_label();

 private:
  void update_status();

  void set_record_loss();

 protected:
  void closeEvent(QCloseEvent* event) override;

 private:
  Ui::RecordDialog* ui;
  class MainWindow* window_{nullptr};
  std::shared_ptr<vlink::BagWriter> recorder_;
  std::unordered_set<std::string> url_set;
  std::atomic<Status> status_{kDisable};
  vlink::ElapsedTimer record_timer_;
  vlink::ElapsedTimer pause_timer_;
  int64_t pause_total_{0};
  int64_t last_timestamp_{0};
  std::atomic<int> progress_index_{0};
  std::unordered_map<std::string, vlink::CalculateSample> loss_map_;
  std::unordered_set<std::string> select_urls_;
  std::shared_mutex select_mtx_;
  int64_t dx_timestamp_{-1};

  std::string ser_type_;

  class QRegularExpressionValidator* validator_{nullptr};

  QTimer* label_timer_{nullptr};
  std::atomic_bool data_has_changed{false};
};

// NOLINTEND
