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
#include <vlink/external/proxy_api.h>
#include <vlink/zerocopy/point_cloud.h>

#include "./flatbuffers_runtime_compat.h"

#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wcpp"
#endif
#include <google/protobuf/descriptor.h>
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic pop
#endif

#include <QCloseEvent>
#include <QDialog>
#include <QElapsedTimer>
#include <QHash>
#include <QResizeEvent>
#include <QVector3D>
#include <mutex>
#include <string>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "./osg/osgselecthandler.h"

namespace Ui {
class Point3DDialog;
}

struct ProtoPointValueField final {
  std::string display_name;
  std::string leaf_name;
  int value_type{vlink::zerocopy::PointCloud::kUnknownType};
  std::vector<const google::protobuf::FieldDescriptor*> path;
};

struct ProtoPointCandidate final {
  const google::protobuf::FieldDescriptor* repeated_field{nullptr};
  std::vector<const google::protobuf::FieldDescriptor*> x_path;
  std::vector<const google::protobuf::FieldDescriptor*> y_path;
  std::vector<const google::protobuf::FieldDescriptor*> z_path;
  std::vector<ProtoPointValueField> value_fields;
};

#ifdef VLINK_ENABLE_VIEWER_FLATBUFFERS
struct FlatbuffersPointValueField final {
  std::string display_name;
  std::string leaf_name;
  int value_type{vlink::zerocopy::PointCloud::kUnknownType};
  std::vector<const reflection::Field*> path;
};

struct FlatbuffersPointCandidate final {
  const reflection::Field* repeated_field{nullptr};
  std::vector<const reflection::Field*> x_path;
  std::vector<const reflection::Field*> y_path;
  std::vector<const reflection::Field*> z_path;
  std::vector<FlatbuffersPointValueField> value_fields;
};
#endif

class Point3DDialog : public QDialog {
  Q_OBJECT

 public:
  using PointValueList = std::vector<std::tuple<std::string, int, double>>;
  using Point3dMap = std::unordered_map<
      std::string,
      std::vector<std::tuple<double, double, double, uint32_t, uint32_t, uint32_t, float, PointValueList>>>;

  struct ASTNode {
    enum NodeType {
      kTypeValue,
      kTypeAnd,
      kTypeOr,
      kTypeNot,
      kTypeExpression,
    };

    NodeType type;
    vlink::Function<bool(int, const PointValueList&)> eval;
    std::vector<std::shared_ptr<ASTNode>> children;

    ASTNode(NodeType t) : type(t) {}
  };

  explicit Point3DDialog(QWidget* parent = nullptr, bool disable_osg = false);

  ~Point3DDialog();

  void init_osg();

  inline Point3dMap& get_point3d_map() noexcept { return point3d_map_; };

 protected:
  void showEvent(class QShowEvent* event) override;

  void hideEvent(class QHideEvent* event) override;

  void mousePressEvent(class QMouseEvent* event) override;

  void mouseMoveEvent(class QMouseEvent* event) override;

  void mouseReleaseEvent(class QMouseEvent* event) override;

  void keyPressEvent(class QKeyEvent* event) override;

  void keyReleaseEvent(class QKeyEvent* event) override;

  void paintEvent(class QPaintEvent* event) override;

  void closeEvent(class QCloseEvent* event) override;

  void resizeEvent(class QResizeEvent* event) override;

 private slots:
  void on_pushButton_close_clicked();

  void on_checkBox_platform_clicked(bool checked);

  void on_checkBox_car_clicked(bool checked);

  void on_pushButton_camera_clicked();

  void on_checkBox_select_clicked(bool checked);

  void on_doubleSpinBox_point_valueChanged(double arg1);

  void on_doubleSpinBox_color_valueChanged(double arg1);

  void on_doubleSpinBox_min_valueChanged(double arg1);

  void on_doubleSpinBox_max_valueChanged(double arg1);

  void on_toolButton_inversion_clicked(bool checked);

  void on_groupBox_range_clicked(bool checked);

  void on_checkBox_exp_clicked(bool checked);

  void on_lineEdit_exp_editingFinished();

  void refresh_sence();

#ifdef VLINK_ENABLE_VIEWER_OSG
  void update_ui_for_proto(const QVariant& variant, bool cache, const QElapsedTimer& timer);

  void update_ui_for_flatbuffers(const QVariant& variant, bool cache, const QElapsedTimer& timer);

  void update_ui_for_zero_copy_types(const QVariant& variant, bool cache, const QElapsedTimer& timer);

  void update_points();
#endif

 private:
  bool evaluate_expression(int index, const QString& exp_str, const PointValueList& value_list);

  bool check_expression(int index, const PointValueList& value_list);

  std::shared_ptr<ASTNode> parse_expression_to_ast(const QString& expression);

  Ui::Point3DDialog* ui;
  class MainWindow* window_{nullptr};

  friend class CameraDialog;

  bool multi_mode_{false};

  QTimer* timer_{nullptr};
  float last_frame_count_{0};
  float frame_count_{0};
  int64_t total_point_count_{0};

  double point_min_{std::numeric_limits<double>::max()};
  double point_max_{std::numeric_limits<double>::lowest()};

  google::protobuf::Message* target_msg_{nullptr};
  std::vector<ProtoPointCandidate> msg_list_;
  Point3dMap point3d_map_;
  float point_size_{1};
  double average_value_{0};

  std::unordered_set<std::string> select_urls_;

  std::shared_ptr<FlatbuffersSchemaContext> target_fbs_context_;
  std::vector<FlatbuffersPointCandidate> fbs_msg_list_;

  class CameraDialog* camera_dialog_{nullptr};
  vlink::ProxyAPI::DataCallback data_callback_;
  std::mutex data_mutex_;

  uint64_t last_key_num_{0};
  std::string last_key_str_;

  std::unordered_map<std::string, vlink::ProxyAPI::Data> proxy_data_cache_;
  std::mutex cache_mtx_;

  QString current_expr_;
  std::shared_ptr<ASTNode> cached_ast_;
  QHash<QString, vlink::Function<bool(int, const PointValueList&)>> expression_cache_;
  bool has_expr_finished_{false};

#ifdef VLINK_ENABLE_VIEWER_OSG
  friend class UpdateCallback;
  class QVBoxLayout* osg_layout_;
  class OsgWidget* osg_widget_{nullptr};
  class OsgGraphicsView* osg_view_{nullptr};
  osg::ref_ptr<osg::Group> root_group_;
  osg::ref_ptr<osg::Node> platform_node_;
  osg::ref_ptr<osg::Node> car_node_;
  osg::ref_ptr<class OsgManipulator> manipulator_;
  std::unordered_map<std::string, osg::Geode*> geo_node_map_;
  osg::ref_ptr<OsgSelectHandler> select_handler_;
  std::atomic_bool osg_inited_{false};
  std::map<int, std::tuple<osg::Vec3d, osg::Vec3d, osg::Vec3d>> move_point_map_;
#endif

 signals:
  void point3d_map_changed();
};

// NOLINTEND
