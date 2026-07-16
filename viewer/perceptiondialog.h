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

#include <vlink/external/proxy_api.h>

#include <QDialog>
#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "./flatbuffers_runtime_compat.h"
#include "./perception/perception_config.h"
#include "./perception/perception_model.h"

#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wcpp"
#endif
#include <google/protobuf/descriptor.h>
#include <google/protobuf/dynamic_message.h>
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic pop
#endif

#ifdef VLINK_ENABLE_VIEWER_OSG
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Woverloaded-virtual"
#pragma GCC diagnostic ignored "-Wdeprecated-copy"
#endif
#include <osg/Geode>
#include <osg/Group>
#include <osg/Vec3d>
#include <osgText/Font>
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic pop
#endif

#include <map>
#include <tuple>
#endif

namespace Ui {
class PerceptionDialog;
}

class PerceptionDialog : public QDialog {
  Q_OBJECT

 public:
  explicit PerceptionDialog(QWidget* parent = nullptr);

  ~PerceptionDialog() override;

  void init_osg();

 protected:
  void showEvent(class QShowEvent* event) override;

  void closeEvent(class QCloseEvent* event) override;

  void resizeEvent(class QResizeEvent* event) override;

 private slots:
  void on_pushButton_close_clicked();

  void on_checkBox_platform_clicked(bool checked);

  void on_checkBox_car_clicked(bool checked);

  void on_toolButton_config_path_clicked();

  void on_pushButton_edit_clicked();

  void on_doubleSpinBox_size_valueChanged(double value);

  void on_pushButton_camera_clicked();

  void render_url(const QString& url);

 private:
  friend class CameraDialog;

  struct UrlContext final {
    perception::RenderType type{perception::RenderType::kPointCloud};
    vlink::SchemaType schema{vlink::SchemaType::kUnknown};
    google::protobuf::Message* proto_prototype{nullptr};
    std::shared_ptr<FlatbuffersSchemaContext> fbs_context;
    std::vector<PerceptionConfig::MappingRule> mappings;
    std::vector<PerceptionConfig::MappingRule> hud_bindings;
  };

  std::unordered_map<std::string, UrlContext> build_contexts();

  void apply_config(const PerceptionConfig& config, const QString& path, bool persist);

  void enqueue_render_url(const QString& url);

#ifdef VLINK_ENABLE_VIEWER_OSG
  void rebuild_url_controls();

  void render_layer(const std::string& geode_key, const std::string& base_url, const perception::Layer& layer);

  osg::Geode* ensure_geode(const std::string& geode_key, perception::RenderType type);

  void render_point_cloud(osg::Geode* geode, const perception::Layer& layer);

  void update_hud_overlay();

  void reposition_hud_overlay();
#endif

  Ui::PerceptionDialog* ui;
  class MainWindow* window_{nullptr};

  vlink::ProxyAPI::DataCallback data_callback_;
  std::mutex data_mutex_;
  class CameraDialog* camera_dialog_{nullptr};

  std::unordered_map<std::string, UrlContext> url_ctx_;
  std::unordered_map<std::string, perception::RenderType> url_geode_type_;
  std::unordered_set<std::string> hidden_urls_;

  PerceptionConfig config_{PerceptionConfig::default_config()};
  QString config_path_;

  float render_size_{2.0f};

  std::mutex cache_mtx_;
  std::unordered_map<std::string, std::shared_ptr<const vlink::ProxyAPI::Data>> proxy_data_cache_;
  std::unordered_set<std::string> pending_render_urls_;

#ifdef VLINK_ENABLE_VIEWER_OSG
  class QVBoxLayout* osg_layout_{nullptr};
  class OsgWidget* osg_widget_{nullptr};
  class OsgGraphicsView* osg_view_{nullptr};
  std::map<int, std::tuple<osg::Vec3d, osg::Vec3d, osg::Vec3d>> move_point_map_;
  osg::ref_ptr<osg::Group> root_group_;
  osg::ref_ptr<osg::Node> platform_node_;
  osg::ref_ptr<osg::Node> car_node_;
  osg::ref_ptr<class OsgManipulator> manipulator_;
  osg::ref_ptr<osgText::Font> osg_font_;
  std::unordered_map<std::string, osg::ref_ptr<osg::Geode>> geo_node_map_;
  std::vector<class QGraphicsProxyWidget*> url_filter_items_;
  class QGraphicsPixmapItem* hud_item_{nullptr};
  std::map<std::string, perception::HudField> hud_values_;
  uint64_t hud_last_render_ms_{0};
  bool hud_render_pending_{false};
  std::atomic_bool osg_inited_{false};
#endif
};

// NOLINTEND
