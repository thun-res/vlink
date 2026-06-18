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
#include <vlink/extension/url_remap.h>
#include <vlink/vlink.h>

#include <QMainWindow>
#include <QProcess>
#include <QTimer>
#include <atomic>
#include <memory>
#include <shared_mutex>

QT_BEGIN_NAMESPACE
namespace Ui {
class PlayerWindow;
}
QT_END_NAMESPACE

class PlayerWindow : public QMainWindow {
  Q_OBJECT

 public:
  enum class TimeMethod : uint8_t {
    kTimeRel = 0,
    kTimeLocal = 1,
    kTimeUtc = 2,
  };

  using RawPub = vlink::Publisher<vlink::Bytes>;
  using RawPubPtr = std::shared_ptr<RawPub>;

  PlayerWindow(const QString& bag_path = "", QWidget* parent = nullptr);

  ~PlayerWindow();

  static PlayerWindow* get_instance();

  bool has_error() const;

  int64_t get_current_timestamp() const;

 protected:
  void showEvent(class QShowEvent* event) override;

  void hideEvent(class QHideEvent* event) override;

  void closeEvent(class QCloseEvent* event) override;

  void dragEnterEvent(class QDragEnterEvent* event) override;

  void dropEvent(class QDropEvent* event) override;

  void resizeEvent(class QResizeEvent* event) override;

  void moveEvent(class QMoveEvent* event) override;

  void mousePressEvent(class QMouseEvent* event) override;

  void mouseReleaseEvent(class QMouseEvent* event) override;

  void mouseMoveEvent(class QMouseEvent* event) override;

 private slots:
  void on_toolButton_select_clicked();

  void on_toolButton_close_clicked();

  void on_toolButton_point_clicked();

  void on_toolButton_info_clicked();

  void on_toolButton_viewer_clicked();

  void on_toolButton_cmd_clicked();

  void on_toolButton_analyzer_clicked();

  void on_toolButton_play_clicked();

  void on_toolButton_pause_clicked();

  void on_toolButton_time_clicked();

  void on_toolButton_remap_clicked(bool checked);

  void on_toolButton_skip_clicked(bool checked);

  void on_toolButton_proxy_clicked(bool checked);

  void on_checkBox_black_clicked(bool checked);

  void on_checkBox_loop_clicked(bool checked);

  void on_checkBox_native_clicked(bool checked);

  void on_horizontalSlider_progress_sliderPressed();

  void on_horizontalSlider_progress_sliderReleased();

  void on_horizontalSlider_progress_sliderMoved(int position);

  void on_doubleSpinBox_rate_editingFinished();

  void ready_to_play(const QString& warn_string);

  void update_windows_title(bool show_percent = false);

  void update_timestamp(int64_t timestamp);

  void update_status();

  void process_ready();

  void update_proxy_icon();

 private:
  bool load_bag(const QString& path);

  void reload();

  bool check_has_shm();

  inline static PlayerWindow* instance_{nullptr};

  Ui::PlayerWindow* ui;

  class WaitWidget* wait_widget_{nullptr};
  class InfoDialog* info_dialog_{nullptr};
  class PointDialog* point_dialog_{nullptr};

  QTimer* main_timer_{nullptr};
  QTimer* play_timer_{nullptr};

  int fixed_height_{0};

  vlink::UrlRemap remap_;
  std::mutex remap_mtx_;

  std::shared_ptr<vlink::BagReader> player_;
  std::unordered_map<std::string, RawPubPtr> pub_urls_map_;

  std::unordered_set<std::string> filter_urls_;

  std::shared_mutex urls_mtx_;

  QString remap_path_;
  QString file_path_;

  std::atomic_bool is_changed_{false};
  std::atomic<size_t> bytes_size_{0};
  vlink::ElapsedTimer total_elapsed_timer;

  QProcess proxy_process_;
  QProcess viewer_process_;
  QProcess analyzer_process_;

  std::atomic_bool has_block_shm_{false};
  std::atomic_bool has_played_{false};
  std::atomic_bool has_error_{false};
  std::atomic_bool quit_flag_{false};

  std::atomic<int> status_{-1};
  std::atomic_bool press_and_paused_{false};
  std::atomic_bool pause_to_next_flag_{false};
  std::atomic_bool proxy_user_kill_{false};
  std::atomic<double> last_rate_{1.0};

  std::atomic<int64_t> curent_timestamp_{0};
  std::atomic<int64_t> date_timestamp_{0};

  std::atomic<TimeMethod> time_method_{TimeMethod::kTimeRel};
};

// NOLINTEND
