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
#include <vlink/vlink.h>

#include "../flatbuffers_runtime_compat.h"

#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wcpp"
#endif
#include <google/protobuf/compiler/importer.h>
#include <google/protobuf/descriptor.h>
#include <google/protobuf/dynamic_message.h>
#include <google/protobuf/io/zero_copy_stream_impl.h>
#include <google/protobuf/message.h>
#include <google/protobuf/reflection.h>
#include <google/protobuf/stubs/common.h>
#include <google/protobuf/text_format.h>
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic pop
#endif

#include <QColor>
#include <QHash>
#include <QMainWindow>
#include <QProcess>
#include <QTimer>
#include <memory>
#include <optional>
#include <unordered_map>
#include <variant>

#include "./qcustomplot.h"

namespace Ui {
class AnalyzerWindow;
}

struct PlotUnit {
  int index{-1};
  std::string label;
  std::string url;
  std::string url_filter;
  std::vector<std::string> expressions;
  QColor color;

  bool ext_operation_pro{false};
  int ext_sample_interval{0};
  bool ext_zero_start_x{false};
  bool ext_zero_start_y{false};
  double ext_limit_max_x{QCPRange::maxRange};
  double ext_limit_min_x{-QCPRange::maxRange};
  double ext_limit_max_y{QCPRange::maxRange};
  double ext_limit_min_y{-QCPRange::maxRange};
  std::string ext_operation_x;
  std::string ext_operation_y;

  QVector<double> x_values;
  QVector<double> y_values;
  std::optional<double> x_start;
  std::optional<double> y_start;
  int64_t sample_timestamp{0};
  std::vector<std::vector<std::string>> condition_lists;
  QCPGraph* graph{nullptr};
  std::optional<vlink::SchemaType> schema_type_override;
  std::string cached_ser;
  std::unique_ptr<google::protobuf::Message> root_msg;
  std::shared_ptr<FlatbuffersSchemaContext> fbs_context;
};

using VariantType = std::variant<int64_t, double, std::string, vlink::Bytes>;

class AnalyzerWindow : public QMainWindow {
  Q_OBJECT

 public:
  enum Type : uint8_t {
    kUnknownType = 0,
    kFrequencyType = 1,
    kValueType = 2,
    kCustomType = 3,
  };

  explicit AnalyzerWindow(const QString& bag_path, bool enable_timeline, QWidget* parent = nullptr);

  ~AnalyzerWindow();

 protected:
  void showEvent(class QShowEvent* event) override;

  void hideEvent(class QHideEvent* event) override;

  void closeEvent(class QCloseEvent* event) override;

  void resizeEvent(class QResizeEvent* event) override;

  void dragEnterEvent(class QDragEnterEvent* event) override;

  void dropEvent(class QDropEvent* event) override;

  void moveEvent(class QMoveEvent* event) override;

  void keyPressEvent(class QKeyEvent* event) override;

  void keyReleaseEvent(class QKeyEvent* event) override;

 private slots:
  void on_groupBox_time_clicked(bool checked);

  void on_toolButton_path_clicked();

  void on_toolButton_config_clicked();

  void on_toolButton_proto_clicked();

  void on_toolButton_fbs_clicked();

  void on_pushButton_gen_clicked();

  void on_pushButton_interrupt_clicked();

  void on_pushButton_export_clicked();

  void on_pushButton_close_clicked();

  void on_horizontalSlider_time_valueChanged(int value);

  void on_checkBox_time_clicked(bool checked);

  void on_checkBox_legend_clicked(bool checked);

  void on_checkBox_grid_clicked(bool checked);

  void on_checkBox_point_clicked(bool checked);

  void on_checkBox_timeline_clicked(bool checked);

  void on_checkBox_tracking_clicked(bool checked);

  void on_comboBox_zoom_currentIndexChanged(int index);

  void on_comboBox_line_currentIndexChanged(int index);

  void on_comboBox_count_currentIndexChanged(int index);

  void set_progress(double value);

  bool load_bag(const QString& path);

  bool load_config(const QString& path);

  bool load_proto(const QString& proto);

  bool load_fbs(const QString& fbs_dir);

  void update_progress();

  void update_status();

  void create_plot();

  void clear_plot();

  void reset_plot();

  void adjust_x_range(double value);

  void move_timeline(double time);

 private:
  Ui::AnalyzerWindow* ui;

  QTimer* progress_timer_{nullptr};
  QCPTextElement* title_element_{nullptr};
  QList<QCPItemText*> text_items_;
  QCPItemLine* timeline_{nullptr};

  bool enable_timeline_{false};

  std::shared_ptr<vlink::BagReader> player_;

  std::unordered_set<std::string> filter_urls_;

  QString default_bag_path_;

  QString file_path_;

  QString config_path_;
  QString proto_dir_;
  QString fbs_dir_;

  std::atomic_bool quit_flag_{false};

  std::atomic<int> status_{-1};
  std::atomic<int> last_zoom_index_{0};

  std::atomic_bool ready_to_start_{false};
  std::atomic_bool interrupted_{false};
  std::atomic_bool finished_{false};
  std::atomic_bool mouse_pressed_{false};

  QString title_;
  std::atomic<Type> type_{kUnknownType};
  std::unordered_map<std::string, std::vector<PlotUnit>> unit_map_;
  std::vector<std::string> url_list_;

  QCPRange x_range_{-1.0, 61.0};
  QCPRange y_range_{-1.0, 61.0};

  QCPRange x_limit_range_{-1.0, 61.0};
  QCPRange y_limit_range_{-1.0, 61.0};

  double x_min_value_{QCPRange::maxRange};
  double x_max_value_{-QCPRange::maxRange};

  double y_min_value_{QCPRange::maxRange};
  double y_max_value_{-QCPRange::maxRange};

  std::string label_x_;
  std::string label_y_;

  std::shared_ptr<google::protobuf::compiler::DiskSourceTree> source_tree_;
  std::shared_ptr<google::protobuf::compiler::Importer> importer_;
  std::shared_ptr<google::protobuf::DynamicMessageFactory> factory_;
  google::protobuf::DescriptorPool* des_pool_{nullptr};
  FlatbuffersRuntime flatbuffers_runtime_;

  std::atomic<double> current_time_{0};
  class IpcChannel* ipc_{nullptr};

  QSharedPointer<QCPAxisTicker> time_ticker_;
  QSharedPointer<QCPAxisTicker> default_ticker_;

  bool points_is_empty_{true};
};

// NOLINTEND
