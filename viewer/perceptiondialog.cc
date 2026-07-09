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

#include "./perceptiondialog.h"

#include <vlink/base/elapsed_timer.h>

#include <QApplication>
#include <QCheckBox>
#include <QColor>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFont>
#include <QFontMetricsF>
#include <QGraphicsPathItem>
#include <QGraphicsPixmapItem>
#include <QGraphicsProxyWidget>
#include <QGraphicsScene>
#include <QGraphicsTextItem>
#include <QIcon>
#include <QKeyEvent>
#include <QMessageBox>
#include <QPainter>
#include <QPixmap>
#include <QPolygonF>
#include <QResizeEvent>
#include <QSettings>
#include <QShowEvent>
#include <QStandardPaths>
#include <QToolButton>
#include <QVBoxLayout>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <sstream>
#include <vector>

#include "./cameradialog.h"
#include "./mainwindow.h"
#include "./perception/perception_decode.h"
#include "./perception/perception_editor.h"
#include "./ui_mainwindow.h"

#ifdef VLINK_ENABLE_VIEWER_OSG
#include <flatbuffers/flatbuffers.h>

#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Woverloaded-virtual"
#pragma GCC diagnostic ignored "-Wdeprecated-copy"
#endif
#include <osg/CullFace>
#include <osg/CullStack>
#include <osgDB/ReadFile>
#include <osgDB/Registry>
#include <osgText/Font>
#include <osgUtil/CullVisitor>
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic pop
#endif

#include "./osg/osgcamerafrustum.h"
#include "./osg/osgcoord.h"
#include "./osg/osgcovarianceellipse.h"
#include "./osg/osgegotrajectory.h"
#include "./osg/osgfreespace.h"
#include "./osg/osggraphicsview.h"
#include "./osg/osghdmap.h"
#include "./osg/osglaneline.h"
#include "./osg/osglight.h"
#include "./osg/osgmanipulator.h"
#include "./osg/osgobjectarray.h"
#include "./osg/osgoccupancygrid.h"
#include "./osg/osgparkingslot.h"
#include "./osg/osgplatform.h"
#include "./osg/osgpointcloud.h"
#include "./osg/osgprediction.h"
#include "./osg/osgstopline.h"
#include "./osg/osgtrafficlight.h"
#include "./osg/osgtrafficsign.h"
#include "./osg/osgwidget.h"
#endif

#include "ui_perceptiondialog.h"

#define VLINK_PERCEPTION_PLATFORM_SIZE 500.0
#define VLINK_PERCEPTION_PLATFORM_GRID_COUNT 100
#define USE_GRAPHICS_VIEW 1

class CustomCheckBox : public QCheckBox {
 public:
  explicit CustomCheckBox(QWidget* parent = nullptr) : QCheckBox(parent) {
    this->setStyleSheet("QCheckBox { background: transparent; color: white; font-size: 14px; font-weight: bold; }");
  }

 protected:
  void keyPressEvent(QKeyEvent* event) override { event->ignore(); }

  void keyReleaseEvent(QKeyEvent* event) override { event->ignore(); }
};

static bool perception_dispatch_expired(uint64_t dispatch_start_ms) {
  const auto now_ms = vlink::ElapsedTimer::get_cpu_timestamp(vlink::ElapsedTimer::kMilli);
  return now_ms > dispatch_start_ms && now_ms - dispatch_start_ms > 1000;
}

#ifdef VLINK_ENABLE_VIEWER_OSG

static osg::Vec4d perception_value_color(double value, double min_value, double max_value) {
  if (!(max_value > min_value)) {
    return osg::Vec4d(1.0, 0.33, 1.0, 1.0);
  }

  double t = (value - min_value) / (max_value - min_value);
  t = std::clamp(t, 0.0, 1.0);

  const double r = std::clamp(1.5 - std::abs(4.0 * t - 3.0), 0.0, 1.0);
  const double g = std::clamp(1.5 - std::abs(4.0 * t - 2.0), 0.0, 1.0);
  const double b = std::clamp(1.5 - std::abs(4.0 * t - 1.0), 0.0, 1.0);
  return osg::Vec4d(r, g, b, 1.0);
}

#endif

PerceptionDialog::PerceptionDialog(QWidget* parent) : QDialog(parent), ui(new Ui::PerceptionDialog) {
  window_ = MainWindow::get_instance();

  setWindowFlags(Qt::Window | Qt::WindowMaximizeButtonHint | Qt::WindowCloseButtonHint);

  ui->setupUi(this);

  {
    QSettings settings(QStandardPaths::writableLocation(QStandardPaths::CacheLocation) + "/settings.ini",
                       QSettings::IniFormat);

    settings.beginGroup("PerceptionDialog");
    const auto saved_path = settings.value("perception_config_path").toString();
    settings.endGroup();

    if (!saved_path.isEmpty()) {
      PerceptionConfig loaded;
      QString error;

      if (loaded.load_from_file(saved_path, &error)) {
        config_ = std::move(loaded);
        config_path_ = saved_path;
      }
    }

    ui->lineEdit_config_path->setText(config_path_);
    ui->toolButton_config_path->setEnabled(true);
  }

  render_size_ = static_cast<float>(ui->doubleSpinBox_size->value());

  ui->checkBox_car->setEnabled(false);
  ui->checkBox_car->setChecked(false);

#ifdef VLINK_ENABLE_VIEWER_OSG
  url_ctx_ = build_contexts();

  ui->label_osg->setStyleSheet("");

  ui->label_osg->setText("");

  osg_layout_ = new QVBoxLayout(ui->label_osg);
  osg_layout_->setContentsMargins(0, 0, 0, 0);
  osg_layout_->setSpacing(0);
  ui->label_osg->setLayout(osg_layout_);

#if USE_GRAPHICS_VIEW
  osg_view_ = new OsgGraphicsView(ui->label_osg);
  osg_layout_->addWidget(osg_view_);

  auto* logo_item = new QGraphicsPixmapItem;
  auto* fps_item = new QGraphicsTextItem;
  auto* home_item = new QGraphicsProxyWidget;
  auto* left_item = new QGraphicsProxyWidget;
  auto* right_item = new QGraphicsProxyWidget;
  auto* front_item = new QGraphicsProxyWidget;
  auto* behind_item = new QGraphicsProxyWidget;

  {
    logo_item->setCacheMode(QGraphicsTextItem::ItemCoordinateCache);
    logo_item->setOpacity(0.2);
    logo_item->setPixmap(QPixmap(":/resource/vlink.svg"));
    osg_view_->scene()->addItem(logo_item);
  }

  {
    fps_item->setCacheMode(QGraphicsTextItem::ItemCoordinateCache);
    fps_item->setPlainText("FPS: ---");
    fps_item->setDefaultTextColor(qRgb(50, 255, 50));
    fps_item->setOpacity(0.8);
    QFont font = fps_item->font();
    font.setPixelSize(15);
    fps_item->setFont(font);
    osg_view_->scene()->addItem(fps_item);
  }

  {
    auto* button = new QToolButton();
    button->setFocusPolicy(Qt::NoFocus);
    button->setAutoRaise(true);
    button->setIconSize(QSize(24, 24));
    button->setIcon(QIcon(":/resource/home.png"));
    button->setToolTip(tr("Home"));
    connect(button, &QToolButton::clicked, this, [this](bool) {
      osg::Vec3d home_eye;
      osg::Vec3d home_center;
      osg::Vec3d home_up;

      manipulator_->getHomePosition(home_eye, home_center, home_up);

      manipulator_->moveToPoint(home_eye, home_center, home_up);
    });

    home_item->setCacheMode(QGraphicsTextItem::ItemCoordinateCache);
    home_item->setWidget(button);
    home_item->setOpacity(0.8);
    home_item->setFocusPolicy(Qt::NoFocus);

    osg_view_->scene()->addItem(home_item);
  }

  {
    auto* button = new QToolButton();
    button->setFocusPolicy(Qt::NoFocus);
    button->setAutoRaise(true);
    button->setIconSize(QSize(24, 24));
    button->setIcon(QIcon(":/resource/left.png"));
    button->setToolTip(tr("Left"));
    connect(button, &QToolButton::clicked, this, [this](bool) {
      manipulator_->moveToPoint(std::get<0>(move_point_map_[0]), std::get<1>(move_point_map_[0]),
                                std::get<2>(move_point_map_[0]));
    });

    left_item->setCacheMode(QGraphicsTextItem::ItemCoordinateCache);
    left_item->setWidget(button);
    left_item->setOpacity(0.8);
    left_item->setFocusPolicy(Qt::NoFocus);

    osg_view_->scene()->addItem(left_item);
  }

  {
    auto* button = new QToolButton();
    button->setFocusPolicy(Qt::NoFocus);
    button->setAutoRaise(true);
    button->setIconSize(QSize(24, 24));
    button->setIcon(QIcon(":/resource/right.png"));
    button->setToolTip(tr("Right"));
    connect(button, &QToolButton::clicked, this, [this](bool) {
      manipulator_->moveToPoint(std::get<0>(move_point_map_[1]), std::get<1>(move_point_map_[1]),
                                std::get<2>(move_point_map_[1]));
    });

    right_item->setCacheMode(QGraphicsTextItem::ItemCoordinateCache);
    right_item->setWidget(button);
    right_item->setOpacity(0.8);
    right_item->setFocusPolicy(Qt::NoFocus);

    osg_view_->scene()->addItem(right_item);
  }

  {
    auto* button = new QToolButton();
    button->setFocusPolicy(Qt::NoFocus);
    button->setAutoRaise(true);
    button->setIconSize(QSize(24, 24));
    button->setIcon(QIcon(":/resource/front.png"));
    button->setToolTip(tr("Front"));
    connect(button, &QToolButton::clicked, this, [this](bool) {
      manipulator_->moveToPoint(std::get<0>(move_point_map_[2]), std::get<1>(move_point_map_[2]),
                                std::get<2>(move_point_map_[2]));
    });

    front_item->setCacheMode(QGraphicsTextItem::ItemCoordinateCache);
    front_item->setWidget(button);
    front_item->setOpacity(0.8);
    front_item->setFocusPolicy(Qt::NoFocus);

    osg_view_->scene()->addItem(front_item);
  }

  {
    auto* button = new QToolButton();
    button->setFocusPolicy(Qt::NoFocus);
    button->setAutoRaise(true);
    button->setIconSize(QSize(24, 24));
    button->setIcon(QIcon(":/resource/behind.png"));
    button->setToolTip(tr("Behind"));
    connect(button, &QToolButton::clicked, this, [this](bool) {
      manipulator_->moveToPoint(std::get<0>(move_point_map_[3]), std::get<1>(move_point_map_[3]),
                                std::get<2>(move_point_map_[3]));
    });

    behind_item->setCacheMode(QGraphicsTextItem::ItemCoordinateCache);
    behind_item->setWidget(button);
    behind_item->setOpacity(0.8);
    behind_item->setFocusPolicy(Qt::NoFocus);

    osg_view_->scene()->addItem(behind_item);
  }

  {
    hud_item_ = new QGraphicsPixmapItem;
    hud_item_->setZValue(50);
    hud_item_->setVisible(false);
    hud_item_->setTransformationMode(Qt::SmoothTransformation);
    osg_view_->scene()->addItem(hud_item_);
  }

  int index = 0;
  for (const auto& [url, context] : url_ctx_) {
    auto* check_box = new CustomCheckBox;
    check_box->setText(QString::fromStdString(url));
    check_box->setChecked(true);
    check_box->setFocusPolicy(Qt::NoFocus);
    connect(check_box, &CustomCheckBox::clicked, this, [this, check_box](bool checked) {
      const std::string url = check_box->text().toStdString();

      if (checked) {
        hidden_urls_.erase(url);
      } else {
        hidden_urls_.emplace(url);
      }

      for (const auto& [geode_key, geode] : geo_node_map_) {
        if (!geode) {
          continue;
        }

        if (geode_key.compare(0, url.size(), url) == 0 &&
            (geode_key.size() == url.size() || geode_key[url.size()] == '#')) {
          geode->setNodeMask(checked ? 0xFFFFFFFF : 0);
        }
      }
    });

    auto* proxy = new QGraphicsProxyWidget;

    proxy->setCacheMode(QGraphicsTextItem::ItemCoordinateCache);
    proxy->setWidget(check_box);
    proxy->setOpacity(0.8);

    osg_view_->scene()->addItem(proxy);

    proxy->setPos(5, (check_box->height() + 5) * index + 5);

    ++index;
  }

  connect(osg_view_, &OsgGraphicsView::fpsRateChanged, this,
          [fps_item](int fps_rate) { fps_item->setPlainText("FPS: " + QString::number(fps_rate)); });

  connect(osg_view_->scene(), &QGraphicsScene::sceneRectChanged, this,
          [this, logo_item, fps_item, home_item, left_item, right_item, front_item, behind_item](const QRectF& rect) {
            logo_item->setPos((rect.width() - logo_item->boundingRect().width()) / 2,
                              rect.height() - logo_item->boundingRect().height() - 10);

            fps_item->setPos(rect.width() - fps_item->boundingRect().width() - 30, 5);

            home_item->setPos(rect.width() - home_item->boundingRect().width() - 10,
                              rect.height() / 2 - (home_item->boundingRect().height() + 20) * 5 / 2);

            left_item->setPos(rect.width() - left_item->boundingRect().width() - 10,
                              rect.height() / 2 - (left_item->boundingRect().height() + 20) * 5 / 2 +
                                  (left_item->boundingRect().height() + 20) * 1);

            right_item->setPos(rect.width() - right_item->boundingRect().width() - 10,
                               rect.height() / 2 - (right_item->boundingRect().height() + 20) * 5 / 2 +
                                   (right_item->boundingRect().height() + 20) * 2);

            front_item->setPos(rect.width() - front_item->boundingRect().width() - 10,
                               rect.height() / 2 - (front_item->boundingRect().height() + 20) * 5 / 2 +
                                   (front_item->boundingRect().height() + 20) * 3);

            behind_item->setPos(rect.width() - behind_item->boundingRect().width() - 10,
                                rect.height() / 2 - (behind_item->boundingRect().height() + 20) * 5 / 2 +
                                    (behind_item->boundingRect().height() + 20) * 4);

            reposition_hud_overlay();
          });

#else
  osg_widget_ = new OsgWidget(ui->label_osg);
  osg_layout_->addWidget(osg_widget_);
#endif

  init_osg();

#if USE_GRAPHICS_VIEW
  osg_view_->setFocus();
#else
  osg_widget_->setFocus();
#endif
#else
  url_ctx_ = build_contexts();
#endif

  auto data_callback = [this](const vlink::ProxyAPI::Data& proxy_data) {
    {
      std::lock_guard lock(data_mutex_);

      if (data_callback_) {
        data_callback_(proxy_data);
      }
    }

    bool owned = false;

    {
      std::lock_guard lock(cache_mtx_);

      if (url_ctx_.find(proxy_data.url) != url_ctx_.end()) {
        proxy_data_cache_[proxy_data.url] = proxy_data;
        owned = true;
      }
    }

    if (owned) {
      QMetaObject::invokeMethod(this, "render_url", Qt::QueuedConnection,
                                Q_ARG(QString, QString::fromStdString(proxy_data.url)));
    }
  };

  if (window_) {
    std::lock_guard lock(window_->data_mutex_);
    window_->data_callback_ = std::move(data_callback);
  }
}

PerceptionDialog::~PerceptionDialog() {
  if (camera_dialog_) {
    delete camera_dialog_;
    camera_dialog_ = nullptr;
  }

  if (window_) {
    std::lock_guard lock(window_->data_mutex_);
    window_->data_callback_ = nullptr;
  }

  {
    std::lock_guard lock(data_mutex_);
    data_callback_ = nullptr;
  }

  for (auto& [url, context] : url_ctx_) {
    delete context.proto_prototype;
    context.proto_prototype = nullptr;
  }

  delete ui;
}

std::unordered_map<std::string, PerceptionDialog::UrlContext> PerceptionDialog::build_contexts() {
  std::unordered_map<std::string, UrlContext> contexts;

  if (!window_) {
    return contexts;
  }

  const auto& selected_items = window_->ui->treeWidget_url->selectedItems();

  std::lock_guard lock(window_->data_mutex_);

  for (const auto& item : selected_items) {
    const QString url = item->text(1);
    const QString ser = item->data(1, Qt::UserRole).toString();

    if (config_.should_skip(url, ser)) {
      continue;
    }

    const auto url_str = url.toStdString();
    const auto ser_str = ser.toStdString();

    const auto schema_iter = window_->schema_type_map_.find(url_str);
    const auto schema_type =
        schema_iter != window_->schema_type_map_.end() ? schema_iter->second : vlink::SchemaType::kUnknown;

    UrlContext context;
    context.schema = schema_type;
    context.type = config_.detect_render_type(url_str, ser_str, schema_type);

    if (schema_type == vlink::SchemaType::kProtobuf && window_->des_pool_ && window_->factory_ && !ser_str.empty()) {
      const auto* desc = window_->des_pool_->FindMessageTypeByName(ser_str);

      if (desc) {
        context.proto_prototype = window_->factory_->GetPrototype(desc)->New();
      }
    } else if (schema_type == vlink::SchemaType::kFlatbuffers && !ser_str.empty()) {
      auto fbs_context = window_->flatbuffers_runtime_.find_context(ser_str);

      if (fbs_context && fbs_context->valid()) {
        context.fbs_context = fbs_context;
      }
    }

    for (const auto* rule : config_.mappings_for(url_str, ser_str, schema_type)) {
      context.mappings.push_back(*rule);
    }

    for (const auto* rule : config_.hud_bindings_for(url_str, ser_str, schema_type)) {
      context.hud_bindings.push_back(*rule);
    }

    contexts.emplace(url_str, std::move(context));
  }

  return contexts;
}

void PerceptionDialog::render_url(const QString& url) {
  const auto url_str = url.toStdString();

  vlink::ProxyAPI::Data proxy_data;
  const UrlContext* context = nullptr;

  {
    std::lock_guard lock(cache_mtx_);

    const auto cache_iter = proxy_data_cache_.find(url_str);
    const auto ctx_iter = url_ctx_.find(url_str);

    if (cache_iter == proxy_data_cache_.end() || ctx_iter == url_ctx_.end()) {
      return;
    }

    proxy_data = cache_iter->second;
    context = &ctx_iter->second;
  }

  const auto dispatch_start_ms =
      static_cast<uint64_t>(vlink::ElapsedTimer::get_cpu_timestamp(vlink::ElapsedTimer::kMilli));

  if (perception_dispatch_expired(dispatch_start_ms)) {
    return;
  }

#ifdef VLINK_ENABLE_VIEWER_OSG
  if (context->schema == vlink::SchemaType::kZeroCopy) {
    perception::Layer layer;
    layer.type = context->type;

    switch (context->type) {
      case perception::RenderType::kObjectDetection:
        perception::decode::decode_zerocopy_object_array(proxy_data.raw, layer);
        break;
      case perception::RenderType::kOccupancyGrid:
        perception::decode::decode_zerocopy_occupancy_grid(proxy_data.raw, layer);
        break;
      case perception::RenderType::kPointCloud:
        perception::decode::decode_zerocopy_point_cloud(proxy_data.raw, layer);
        break;
      default:
        break;
    }

    render_layer(url_str, url_str, layer);
    return;
  }

  if (context->mappings.empty() && context->hud_bindings.empty()) {
    return;
  }

  if (context->schema == vlink::SchemaType::kProtobuf && context->proto_prototype) {
    if (!context->proto_prototype->ParseFromArray(proxy_data.raw.data(), static_cast<int>(proxy_data.raw.size()))) {
      return;
    }

    for (size_t i = 0; i < context->mappings.size(); ++i) {
      perception::Layer layer;
      perception::decode::decode_proto(*context->proto_prototype, context->mappings[i], layer);
      render_layer(url_str + "#" + std::to_string(i), url_str, layer);
    }

    for (const auto& binding : context->hud_bindings) {
      std::vector<perception::HudField> fields;
      perception::decode::decode_hud_proto(*context->proto_prototype, binding, fields);

      for (auto& field : fields) {
        hud_values_[field.slot] = std::move(field);
      }
    }

    if (!context->hud_bindings.empty()) {
      update_hud_overlay();
    }

    return;
  }

  if (context->schema == vlink::SchemaType::kFlatbuffers && context->fbs_context && context->fbs_context->schema &&
      context->fbs_context->root_object) {
    const auto* root_table = flatbuffers::GetAnyRoot(reinterpret_cast<const uint8_t*>(proxy_data.raw.data()));

    if (!root_table) {
      return;
    }

    for (size_t i = 0; i < context->mappings.size(); ++i) {
      perception::Layer layer;
      perception::decode::decode_fbs(*root_table, *context->fbs_context->schema, *context->fbs_context->root_object,
                                     context->mappings[i], layer);
      render_layer(url_str + "#" + std::to_string(i), url_str, layer);
    }

    for (const auto& binding : context->hud_bindings) {
      std::vector<perception::HudField> fields;
      perception::decode::decode_hud_fbs(*root_table, *context->fbs_context->schema, *context->fbs_context->root_object,
                                         binding, fields);

      for (auto& field : fields) {
        hud_values_[field.slot] = std::move(field);
      }
    }

    if (!context->hud_bindings.empty()) {
      update_hud_overlay();
    }
  }
#else
  (void)context;
#endif
}

void PerceptionDialog::apply_config(const PerceptionConfig& config, const QString& path, bool persist) {
  config_ = config;
  config_path_ = path;

  auto fresh = build_contexts();

  std::vector<google::protobuf::Message*> stale_prototypes;
  std::vector<QString> cached_urls;

  {
    std::lock_guard lock(cache_mtx_);

    for (auto& [url, context] : url_ctx_) {
      stale_prototypes.push_back(context.proto_prototype);
    }

    url_ctx_ = std::move(fresh);

    for (const auto& [url, data] : proxy_data_cache_) {
      if (url_ctx_.find(url) != url_ctx_.end()) {
        cached_urls.push_back(QString::fromStdString(url));
      }
    }
  }

  for (auto* prototype : stale_prototypes) {
    delete prototype;
  }

#ifdef VLINK_ENABLE_VIEWER_OSG
  if (root_group_) {
    for (const auto& [geode_key, geode] : geo_node_map_) {
      root_group_->removeChild(geode);
    }
  }

  geo_node_map_.clear();
  url_geode_type_.clear();

  hud_values_.clear();
  hud_last_render_ms_ = 0;

  if (hud_item_) {
    hud_item_->setVisible(false);
  }
#endif

  ui->lineEdit_config_path->setText(path);

  for (const auto& url : cached_urls) {
    render_url(url);
  }

  if (persist) {
    QSettings settings(QStandardPaths::writableLocation(QStandardPaths::CacheLocation) + "/settings.ini",
                       QSettings::IniFormat);

    settings.beginGroup("PerceptionDialog");
    settings.setValue("perception_config_path", path);
    settings.endGroup();
  }
}

void PerceptionDialog::on_toolButton_config_path_clicked() {
  const auto path = QFileDialog::getOpenFileName(this, "Select perception config", config_path_, "JSON (*.json)");

  if (path.isEmpty()) {
    return;
  }

  PerceptionConfig loaded;
  QString error;

  if (!loaded.load_from_file(path, &error)) {
    QMessageBox::warning(this, "Config error", error);
    return;
  }

  apply_config(loaded, path, true);
}

void PerceptionDialog::on_pushButton_edit_clicked() {
  std::vector<PerceptionEditorDialog::SourceMessage> sources;

  {
    std::lock_guard lock(cache_mtx_);

    for (const auto& [url, context] : url_ctx_) {
      PerceptionEditorDialog::SourceMessage source;
      source.schema = context.schema;

      if (context.proto_prototype) {
        source.proto_desc = context.proto_prototype->GetDescriptor();
        source.ser = QString::fromStdString(std::string(source.proto_desc->full_name()));
      } else if (context.fbs_context) {
        source.fbs_ctx = context.fbs_context;
        source.ser = QString::fromStdString(context.fbs_context->type_name);
      } else {
        continue;
      }

      sources.emplace_back(std::move(source));
    }
  }

  PerceptionEditorDialog editor(config_, config_path_, window_, std::move(sources), this);

  if (editor.exec() == QDialog::Accepted) {
    PerceptionConfig edited = editor.result_config();
    const QString path = editor.result_path();

    if (!path.isEmpty()) {
      QString error;

      if (edited.save_to_file(path, &error)) {
        apply_config(edited, path, true);
      } else {
        QMessageBox::warning(this, "Save failed", error);
        apply_config(edited, config_path_, false);
      }

      return;
    }

    apply_config(edited, config_path_, false);
  }
}

void PerceptionDialog::on_doubleSpinBox_size_valueChanged(double value) {
  render_size_ = static_cast<float>(value);

#ifdef VLINK_ENABLE_VIEWER_OSG
  const auto ratio = static_cast<float>(qApp->devicePixelRatio());

  for (auto& [url, geode] : geo_node_map_) {
    const auto type_iter = url_geode_type_.find(url);

    if (type_iter != url_geode_type_.end() && type_iter->second == perception::RenderType::kPointCloud) {
      OsgPointCloud::update_point_size(geode, render_size_, std::min(render_size_ * 3.0f, 15.0f), ratio);
    }
  }
#endif
}

void PerceptionDialog::on_pushButton_close_clicked() { close(); }

void PerceptionDialog::on_pushButton_camera_clicked() {
  if (camera_dialog_) {
    camera_dialog_->show();
    camera_dialog_->raise();
    camera_dialog_->activateWindow();
    return;
  }

  camera_dialog_ = new CameraDialog(this);
  camera_dialog_->show();
}

void PerceptionDialog::on_checkBox_platform_clicked(bool checked) {
#ifdef VLINK_ENABLE_VIEWER_OSG
  if (!osg_inited_) {
    ui->checkBox_platform->setChecked(false);
    return;
  }

  if (!platform_node_ || !platform_node_.valid()) {
    return;
  }

  platform_node_->setNodeMask(checked ? ~0U : 0U);
#else
  (void)checked;
#endif
}

void PerceptionDialog::on_checkBox_car_clicked(bool checked) {
#ifdef VLINK_ENABLE_VIEWER_OSG
  if (!osg_inited_) {
    ui->checkBox_car->setChecked(false);
    return;
  }

  if (!car_node_ || !car_node_.valid()) {
    return;
  }

  car_node_->setNodeMask(checked ? ~0U : 0U);
#else
  (void)checked;
#endif
}

void PerceptionDialog::showEvent(QShowEvent* event) { QDialog::showEvent(event); }

void PerceptionDialog::closeEvent(QCloseEvent* event) { QDialog::closeEvent(event); }

void PerceptionDialog::resizeEvent(QResizeEvent* event) {
  QDialog::resizeEvent(event);

#ifdef VLINK_ENABLE_VIEWER_OSG
#if USE_GRAPHICS_VIEW

  if (osg_view_) {
    osg_view_->resize(ui->label_osg->width(), ui->label_osg->height());
  }
#else

  if (osg_widget_) {
    osg_widget_->resize(ui->label_osg->width(), ui->label_osg->height());
  }
#endif
#endif
}

#ifndef VLINK_ENABLE_VIEWER_OSG
void PerceptionDialog::init_osg() {}
#endif

#ifdef VLINK_ENABLE_VIEWER_OSG

void PerceptionDialog::init_osg() {
  if (osg_inited_) {
    return;
  }

  root_group_ = new osg::Group;

#if USE_GRAPHICS_VIEW
  auto* viewer = osg_view_->getViewer();
#else
  auto* viewer = osg_widget_->getViewer();
#endif
  viewer->setSceneData(root_group_);

  auto culling_mode = viewer->getCamera()->getCullingMode();
  culling_mode &= ~(osg::CullStack::SMALL_FEATURE_CULLING);
  viewer->getCamera()->setCullingMode(culling_mode);
  viewer->getCamera()->setComputeNearFarMode(osgUtil::CullVisitor::DO_NOT_COMPUTE_NEAR_FAR);
  viewer->getCamera()->setProjectionMatrixAsPerspective(30.0, 1920.0 / 1080.0, 1, VLINK_PERCEPTION_PLATFORM_SIZE * 4);
  viewer->getCamera()->setClearColor(osg::Vec4d(0.18, 0.18, 0.20, 1));

  manipulator_ = new OsgManipulator;
  manipulator_->setLimit(VLINK_PERCEPTION_PLATFORM_SIZE * 0.75, VLINK_PERCEPTION_PLATFORM_SIZE * 2,
                         VLINK_PERCEPTION_PLATFORM_SIZE * 0.01);
  manipulator_->setHomePosition(osg::Vec3d(0, 0, VLINK_PERCEPTION_PLATFORM_SIZE * 0.5), osg::Vec3d(1, 0, 0),
                                osg::Vec3d(0, 0, 1));
  viewer->setCameraManipulator(manipulator_);

  move_point_map_[0] =
      std::make_tuple(osg::Vec3d(0, VLINK_PERCEPTION_PLATFORM_SIZE * 0.1, VLINK_PERCEPTION_PLATFORM_SIZE * 0.01),
                      osg::Vec3d(1, 0, 0), osg::Vec3d(0, 0, 1));

  move_point_map_[1] =
      std::make_tuple(osg::Vec3d(0, -VLINK_PERCEPTION_PLATFORM_SIZE * 0.1, VLINK_PERCEPTION_PLATFORM_SIZE * 0.01),
                      osg::Vec3d(-1, 0, 0), osg::Vec3d(0, 0, 1));

  move_point_map_[2] =
      std::make_tuple(osg::Vec3d(VLINK_PERCEPTION_PLATFORM_SIZE * 0.1, 0, VLINK_PERCEPTION_PLATFORM_SIZE * 0.01),
                      osg::Vec3d(0, -1, 0), osg::Vec3d(0, 0, 1));

  move_point_map_[3] =
      std::make_tuple(osg::Vec3d(-VLINK_PERCEPTION_PLATFORM_SIZE * 0.1, 0, VLINK_PERCEPTION_PLATFORM_SIZE * 0.01),
                      osg::Vec3d(0, 1, 0), osg::Vec3d(0, 0, 1));

  {
    QFile font_file(":/resource/notomono.ttf");
    osg::ref_ptr<osgText::Font> font;

    if (font_file.open(QIODevice::ReadOnly)) {
      const QByteArray font_data = font_file.readAll();
      font_file.close();

      std::istringstream font_stream(font_data.toStdString());
      font = osgText::readRefFontStream(font_stream);
    }

    osg_font_ = font;

    const auto ratio = qApp->devicePixelRatio();
    root_group_->addChild(OsgCoord::create(viewer->getCamera(), font, ratio));
  }

  platform_node_ = OsgPlatform::create(VLINK_PERCEPTION_PLATFORM_SIZE, VLINK_PERCEPTION_PLATFORM_GRID_COUNT);
  platform_node_->getOrCreateStateSet()->setMode(GL_LIGHTING, osg::StateAttribute::OFF);
  root_group_->addChild(platform_node_);

  root_group_->addChild(OsgLight::create(viewer->getCamera(), VLINK_PERCEPTION_PLATFORM_SIZE * 3 / 4));

  {
    osg::ref_ptr<osg::CullFace> cullface = new osg::CullFace(osg::CullFace::BACK);
    root_group_->getOrCreateStateSet()->setAttribute(cullface);
    root_group_->getOrCreateStateSet()->setMode(GL_CULL_FACE, osg::StateAttribute::ON);
    root_group_->setCullingActive(true);
  }

  {
    root_group_->getOrCreateStateSet()->setMode(GL_MULTISAMPLE, osg::StateAttribute::ON);
    root_group_->getOrCreateStateSet()->setMode(GL_LINE_SMOOTH, osg::StateAttribute::ON);
    root_group_->getOrCreateStateSet()->setMode(GL_POINT_SMOOTH, osg::StateAttribute::ON);
    root_group_->getOrCreateStateSet()->setMode(GL_BLEND, osg::StateAttribute::ON);
  }

  {
    QFile car_file(":/resource/car.osgb");

    if (car_file.open(QIODevice::ReadOnly)) {
      const QByteArray car_data = car_file.readAll();
      car_file.close();

      std::istringstream car_stream(car_data.toStdString());
      osg::ref_ptr<osgDB::ReaderWriter> reader = osgDB::Registry::instance()->getReaderWriterForExtension("osgb");

      if (reader) {
        auto result = reader->readNode(car_stream);
        car_node_ = result.getNode();

        if (car_node_.valid()) {
          car_node_->setNodeMask(0U);
          root_group_->addChild(car_node_);
          ui->checkBox_car->setEnabled(true);
          ui->checkBox_car->setChecked(false);
        }
      }
    }
  }

  osg_inited_ = true;
}

osg::Geode* PerceptionDialog::ensure_geode(const std::string& geode_key, perception::RenderType type) {
  const auto existing = geo_node_map_.find(geode_key);
  const auto existing_type = url_geode_type_.find(geode_key);

  if (existing != geo_node_map_.end() && existing_type != url_geode_type_.end() && existing_type->second == type) {
    return existing->second;
  }

  if (existing != geo_node_map_.end()) {
    if (root_group_) {
      root_group_->removeChild(existing->second);
    }

    geo_node_map_.erase(existing);
  }

  osg::ref_ptr<osg::Geode> geode;

  switch (type) {
    case perception::RenderType::kObjectDetection:
    case perception::RenderType::kTrafficLight:
      geode = (type == perception::RenderType::kTrafficLight) ? OsgTrafficLight::create() : OsgObjectArray::create();
      break;
    case perception::RenderType::kTrafficSign:
      geode = OsgTrafficSign::create();
      break;
    case perception::RenderType::kCameraFrustum:
      geode = OsgCameraFrustum::create();
      break;
    case perception::RenderType::kCovarianceEllipse:
      geode = OsgCovarianceEllipse::create();
      break;
    case perception::RenderType::kLaneLine:
      geode = OsgLaneLine::create();
      break;
    case perception::RenderType::kPrediction:
      geode = OsgPrediction::create();
      break;
    case perception::RenderType::kStopLine:
      geode = OsgStopLine::create();
      break;
    case perception::RenderType::kFreespace:
      geode = OsgFreespace::create();
      break;
    case perception::RenderType::kHdMap:
      geode = OsgHdMap::create();
      break;
    case perception::RenderType::kEgoTrajectory:
      geode = OsgEgoTrajectory::create();
      break;
    case perception::RenderType::kParkingSlot:
      geode = OsgParkingSlot::create();
      break;
    case perception::RenderType::kOccupancyGrid:
      geode = OsgOccupancyGrid::create();
      break;
    case perception::RenderType::kPointCloud:
    default:
      geode = OsgPointCloud::create(render_size_, static_cast<float>(qApp->devicePixelRatio()));
      break;
  }

  if (root_group_) {
    root_group_->addChild(geode);
  }

  geo_node_map_[geode_key] = geode;
  url_geode_type_[geode_key] = type;
  return geode.get();
}

void PerceptionDialog::render_point_cloud(osg::Geode* geode, const perception::Layer& layer) {
  OsgPointCloud::clear_arrays(geode);

  if (geode->getNumDrawables() < 1) {
    return;
  }

  auto* geometry = static_cast<osg::Geometry*>(geode->getDrawable(0));
  auto* vertex_array = static_cast<osg::Vec3dArray*>(geometry->getVertexArray());
  auto* color_array = static_cast<osg::Vec4dArray*>(geometry->getColorArray());

  vertex_array->reserve(layer.cloud.size());
  color_array->reserve(layer.cloud.size());

  double min_value = std::numeric_limits<double>::max();
  double max_value = std::numeric_limits<double>::lowest();

  for (const auto& point : layer.cloud) {
    const double channel =
        layer.has_value_channel ? point.value : std::sqrt(point.x * point.x + point.y * point.y + point.z * point.z);
    min_value = std::min(min_value, channel);
    max_value = std::max(max_value, channel);
  }

  for (const auto& point : layer.cloud) {
    const double channel =
        layer.has_value_channel ? point.value : std::sqrt(point.x * point.x + point.y * point.y + point.z * point.z);
    vertex_array->push_back(osg::Vec3d(point.x, point.y, point.z));
    color_array->push_back(perception_value_color(channel, min_value, max_value));
  }

  OsgPointCloud::finalize_arrays(geode);
}

void PerceptionDialog::update_hud_overlay() {
  if (!hud_item_) {
    return;
  }

  const auto now_ms = static_cast<uint64_t>(vlink::ElapsedTimer::get_cpu_timestamp(vlink::ElapsedTimer::kMilli));

  if (hud_item_->isVisible() && now_ms - hud_last_render_ms_ < 100) {
    return;
  }

  hud_last_render_ms_ = now_ms;

  struct HudSlotStyle {
    const char* slot;
    const char* label;
    const char* unit;
    QColor color;
    int decimals;
  };

  static const HudSlotStyle kHudSlots[] = {
      {"speed", "SPEED", "km/h", QColor(0x4D, 0xE3, 0xA2), 1},
      {"throttle", "THROTTLE", "%", QColor(0x7F, 0xD1, 0xFF), 0},
      {"brake", "BRAKE", "%", QColor(0xFF, 0x6B, 0x6B), 0},
      {"steering_angle", "STEERING", "deg", QColor(0xFF, 0xD1, 0x66), 1},
      {"yaw_rate", "YAW RATE", "rad/s", QColor(0xC2, 0x9F, 0xFF), 3},
      {"accel_lon", "ACCEL X", "m/s2", QColor(0x9F, 0xE8, 0xFF), 2},
      {"accel_lat", "ACCEL Y", "m/s2", QColor(0x9F, 0xE8, 0xFF), 2},
      {"rpm", "RPM", "", QColor(0xB6, 0xC2, 0xD0), 0},
      {"gear", "GEAR", "", QColor(0xFF, 0xFF, 0xFF), 0},
      {"turn_signal", "TURN", "", QColor(0xFF, 0xD1, 0x66), 0},
      {"drive_mode", "MODE", "", QColor(0x4D, 0xE3, 0xA2), 0},
  };

  struct HudRow {
    QString label;
    QString value;
    QString unit;
    QColor color;
  };

  std::vector<HudRow> rows;

  for (const auto& style : kHudSlots) {
    const auto value_iter = hud_values_.find(style.slot);

    if (value_iter == hud_values_.end()) {
      continue;
    }

    QString value_text;

    if (value_iter->second.is_text) {
      value_text = QString::fromStdString(value_iter->second.text).trimmed();

      if (value_text.isEmpty()) {
        value_text = "--";
      }
    } else {
      value_text = QString::number(value_iter->second.value, 'f', style.decimals);
    }

    rows.push_back({QString(style.label), value_text, QString(style.unit), style.color});
  }

  const auto steering_iter = hud_values_.find("steering_angle");
  const auto turn_left_iter = hud_values_.find("turn_left");
  const auto turn_right_iter = hud_values_.find("turn_right");

  const bool has_wheel = steering_iter != hud_values_.end();
  const bool has_turn = turn_left_iter != hud_values_.end() || turn_right_iter != hud_values_.end();
  const bool show_indicators = has_wheel || has_turn;

  if (rows.empty() && !show_indicators) {
    hud_item_->setVisible(false);
    return;
  }

  static const QFont label_font = [] {
    QFont font("Noto Mono");
    font.setStyleHint(QFont::Monospace);
    font.setPixelSize(13);
    return font;
  }();

  static const QFont value_font = [] {
    QFont font("Noto Mono");
    font.setStyleHint(QFont::Monospace);
    font.setPixelSize(18);
    font.setBold(true);
    return font;
  }();

  static const QFont unit_font = [] {
    QFont font("Noto Mono");
    font.setStyleHint(QFont::Monospace);
    font.setPixelSize(12);
    return font;
  }();

  static const QFont title_font = [] {
    QFont font("Noto Mono");
    font.setStyleHint(QFont::Monospace);
    font.setPixelSize(13);
    font.setBold(true);
    return font;
  }();

  static const QFontMetricsF label_metrics(label_font);
  static const QFontMetricsF value_metrics(value_font);
  static const QFontMetricsF unit_metrics(unit_font);
  static const QFontMetricsF title_metrics(title_font);

  qreal label_w = 0;
  qreal value_w = 0;
  qreal unit_w = 0;

  for (const auto& row : rows) {
    label_w = std::max(label_w, label_metrics.horizontalAdvance(row.label));
    value_w = std::max(value_w, value_metrics.horizontalAdvance(row.value));
    unit_w = std::max(unit_w, unit_metrics.horizontalAdvance(row.unit));
  }

  static constexpr qreal kPad = 13.0;
  static constexpr qreal kPadRight = 22.0;
  static constexpr qreal kColGap = 14.0;
  static constexpr qreal kUnitGap = 6.0;

  static constexpr qreal kIndicatorH = 54.0;
  static constexpr qreal kIndicatorMinW = 150.0;

  const qreal row_h = value_metrics.height() + 5.0;
  const qreal title_h = title_metrics.height() + 8.0;
  const qreal indicator_h = show_indicators ? kIndicatorH : 0.0;
  const qreal content_w =
      std::max(label_w + kColGap + value_w + kUnitGap + unit_w, show_indicators ? kIndicatorMinW : 0.0);
  const qreal panel_w = content_w + kPad + kPadRight;
  const qreal panel_h = title_h + static_cast<qreal>(rows.size()) * row_h + indicator_h + kPad * 2;

  const qreal dpr = std::max(1.0, static_cast<double>(qApp->devicePixelRatio()));

  QPixmap pixmap(qRound(panel_w * dpr), qRound(panel_h * dpr));
  pixmap.setDevicePixelRatio(dpr);
  pixmap.fill(Qt::transparent);

  QPainter painter(&pixmap);
  painter.setRenderHint(QPainter::Antialiasing, true);
  painter.setRenderHint(QPainter::TextAntialiasing, true);

  painter.setBrush(QColor(16, 18, 24, 210));
  painter.setPen(QPen(QColor(110, 183, 255, 90), 1.2));
  painter.drawRoundedRect(QRectF(0.6, 0.6, panel_w - 1.2, panel_h - 1.2), 12, 12);

  painter.setFont(title_font);
  painter.setPen(QColor(111, 183, 255));
  painter.drawText(QRectF(kPad, kPad - 2.0, content_w, title_h), Qt::AlignLeft | Qt::AlignVCenter, "VEHICLE");

  qreal y = kPad + title_h;

  for (const auto& row : rows) {
    painter.setFont(label_font);
    painter.setPen(QColor(138, 147, 166));
    painter.drawText(QRectF(kPad, y, label_w, row_h), Qt::AlignLeft | Qt::AlignVCenter, row.label);

    painter.setFont(value_font);
    painter.setPen(row.color);
    painter.drawText(QRectF(kPad + label_w + kColGap, y, value_w, row_h), Qt::AlignRight | Qt::AlignVCenter, row.value);

    if (!row.unit.isEmpty()) {
      painter.setFont(unit_font);
      painter.setPen(QColor(92, 102, 119));
      painter.drawText(QRectF(kPad + label_w + kColGap + value_w + kUnitGap, y, unit_w, row_h),
                       Qt::AlignLeft | Qt::AlignVCenter, row.unit);
    }

    y += row_h;
  }

  if (show_indicators) {
    painter.setPen(QPen(QColor(110, 183, 255, 55), 1.0));
    painter.drawLine(QPointF(kPad, y + 1.0), QPointF(panel_w - kPadRight, y + 1.0));

    const qreal center_y = y + 1.0 + indicator_h / 2.0;

    static constexpr qreal kWheelRadius = 15.0;
    const qreal wheel_cx = kPad + kWheelRadius + 1.0;

    if (has_wheel) {
      const double steering_deg = std::isfinite(steering_iter->second.value) ? steering_iter->second.value : 0.0;

      painter.save();
      painter.translate(wheel_cx, center_y);
      painter.rotate(steering_deg);

      painter.setBrush(Qt::NoBrush);
      painter.setPen(QPen(QColor(0xE6, 0xEC, 0xF5), 2.4));
      painter.drawEllipse(QPointF(0, 0), kWheelRadius, kWheelRadius);
      painter.drawLine(QPointF(-kWheelRadius, 0), QPointF(kWheelRadius, 0));
      painter.drawLine(QPointF(0, 0), QPointF(0, kWheelRadius));

      painter.setPen(Qt::NoPen);
      painter.setBrush(QColor(0xE6, 0xEC, 0xF5));
      painter.drawEllipse(QPointF(0, 0), 3.0, 3.0);

      painter.setBrush(QColor(0x6F, 0xB7, 0xFF));
      painter.drawEllipse(QPointF(0, -kWheelRadius), 2.8, 2.8);
      painter.restore();
    }

    const bool left_on = turn_left_iter != hud_values_.end() && turn_left_iter->second.value > 0.5;
    const bool right_on = turn_right_iter != hud_values_.end() && turn_right_iter->second.value > 0.5;

    const qreal arrow_left_cx = wheel_cx + kWheelRadius + 26.0;
    const qreal arrow_right_cx = arrow_left_cx + 34.0;

    const auto draw_arrow = [&painter, center_y](qreal cx, bool point_left, bool on) {
      static constexpr qreal kSize = 9.0;

      QPolygonF triangle;

      if (point_left) {
        triangle << QPointF(cx - kSize, center_y) << QPointF(cx + kSize, center_y - kSize)
                 << QPointF(cx + kSize, center_y + kSize);
      } else {
        triangle << QPointF(cx + kSize, center_y) << QPointF(cx - kSize, center_y - kSize)
                 << QPointF(cx - kSize, center_y + kSize);
      }

      painter.setPen(Qt::NoPen);
      painter.setBrush(on ? QColor(0x4D, 0xE3, 0xA2) : QColor(0x39, 0x40, 0x4C));
      painter.drawPolygon(triangle);
    };

    draw_arrow(arrow_left_cx, true, left_on);
    draw_arrow(arrow_right_cx, false, right_on);
  }

  painter.end();

  hud_item_->setPixmap(pixmap);
  hud_item_->setVisible(true);
  reposition_hud_overlay();
}

void PerceptionDialog::reposition_hud_overlay() {
  if (!hud_item_ || !osg_view_ || !hud_item_->isVisible()) {
    return;
  }

  static constexpr qreal kMargin = 14.0;

  const qreal dpr = std::max(1.0, static_cast<double>(qApp->devicePixelRatio()));
  const QRectF scene_rect = osg_view_->scene()->sceneRect();
  const qreal panel_w = hud_item_->pixmap().width() / dpr;
  const qreal panel_h = hud_item_->pixmap().height() / dpr;

  hud_item_->setPos(scene_rect.width() - panel_w - kMargin - 50, scene_rect.height() - panel_h - kMargin);
}

void PerceptionDialog::render_layer(const std::string& geode_key, const std::string& base_url,
                                    const perception::Layer& layer) {
  if (!osg_inited_) {
    return;
  }

  osg::Geode* geode = ensure_geode(geode_key, layer.type);

  if (!geode) {
    return;
  }

  geode->setNodeMask(hidden_urls_.count(base_url) ? 0 : 0xFFFFFFFF);

  const auto line_width = render_size_;
  const auto ratio = static_cast<float>(qApp->devicePixelRatio());

  switch (layer.type) {
    case perception::RenderType::kObjectDetection: {
      std::vector<OsgObjectArray::ObjectData> objects;
      objects.reserve(layer.boxes.size());

      for (const auto& box : layer.boxes) {
        OsgObjectArray::ObjectData data;
        data.position[0] = box.position[0];
        data.position[1] = box.position[1];
        data.position[2] = box.position[2];
        data.size[0] = box.size[0];
        data.size[1] = box.size[1];
        data.size[2] = box.size[2];
        data.yaw = box.yaw;
        data.velocity[0] = box.velocity[0];
        data.velocity[1] = box.velocity[1];
        data.velocity[2] = box.velocity[2];
        data.score = box.score;
        data.class_id = box.class_id;
        data.track_id = box.track_id;
        data.color = box.color;

        if (!box.label.empty()) {
          data.label = box.label + (box.track_id > 0 ? " #" + std::to_string(box.track_id) : std::string());
        } else {
          data.label = OsgObjectArray::get_class_name(box.class_id) +
                       (box.track_id > 0 ? std::string(" #") + std::to_string(box.track_id) : std::string());
        }

        objects.emplace_back(std::move(data));
      }

      OsgObjectArray::update(geode, objects, line_width);
      OsgObjectArray::update_labels(geode, objects, osg_font_, ratio);
      break;
    }

    case perception::RenderType::kTrafficLight: {
      std::vector<OsgTrafficLight::TrafficLightData> lights;
      lights.reserve(layer.boxes.size());

      for (const auto& box : layer.boxes) {
        OsgTrafficLight::TrafficLightData data;
        data.position[0] = box.position[0];
        data.position[1] = box.position[1];
        data.position[2] = box.position[2];
        data.color_state = box.color_state;
        data.confidence = box.confidence;
        data.countdown = box.countdown;
        data.label = box.label;
        lights.emplace_back(std::move(data));
      }

      OsgTrafficLight::update(geode, lights, line_width);
      break;
    }

    case perception::RenderType::kTrafficSign: {
      std::vector<OsgTrafficSign::TrafficSignData> signs;
      signs.reserve(layer.boxes.size());

      for (const auto& box : layer.boxes) {
        OsgTrafficSign::TrafficSignData data;
        data.position[0] = box.position[0];
        data.position[1] = box.position[1];
        data.position[2] = box.position[2];
        data.type_id = box.type_id;
        data.color = box.color;
        data.marker_size = box.marker_size;
        data.label = box.label;
        signs.emplace_back(std::move(data));
      }

      OsgTrafficSign::update(geode, signs, line_width);
      break;
    }

    case perception::RenderType::kCameraFrustum: {
      std::vector<OsgCameraFrustum::FrustumData> frustums;
      frustums.reserve(layer.boxes.size());

      for (const auto& box : layer.boxes) {
        OsgCameraFrustum::FrustumData data;
        data.position[0] = box.position[0];
        data.position[1] = box.position[1];
        data.position[2] = box.position[2];
        data.orientation[0] = box.orientation[0];
        data.orientation[1] = box.orientation[1];
        data.orientation[2] = box.orientation[2];
        data.orientation[3] = box.orientation[3];
        data.fov_h = box.fov_h;
        data.fov_v = box.fov_v;
        data.near_dist = box.near_dist;
        data.far_dist = box.far_dist;
        data.color = box.color != 0 ? box.color : 0x00AAFF;
        frustums.emplace_back(std::move(data));
      }

      OsgCameraFrustum::update(geode, frustums, line_width);
      break;
    }

    case perception::RenderType::kCovarianceEllipse: {
      std::vector<OsgCovarianceEllipse::EllipseData> ellipses;
      ellipses.reserve(layer.boxes.size());

      for (const auto& box : layer.boxes) {
        OsgCovarianceEllipse::EllipseData data;
        data.position[0] = box.position[0];
        data.position[1] = box.position[1];
        data.position[2] = box.position[2];
        data.covariance[0] = box.covariance[0];
        data.covariance[1] = box.covariance[1];
        data.covariance[2] = box.covariance[2];
        data.covariance[3] = box.covariance[3];
        data.color = box.color != 0 ? box.color : 0xFFFF00;
        data.alpha = box.ellipse_alpha;
        ellipses.emplace_back(std::move(data));
      }

      OsgCovarianceEllipse::update(geode, ellipses, line_width);
      break;
    }

    case perception::RenderType::kLaneLine: {
      std::vector<OsgLaneLine::LaneData> lanes;
      lanes.reserve(layer.polylines.size());

      for (const auto& polyline : layer.polylines) {
        OsgLaneLine::LaneData data;
        data.color = polyline.color;
        data.lane_type = polyline.type;
        data.points.reserve(polyline.points.size());

        for (const auto& point : polyline.points) {
          data.points.push_back(OsgLaneLine::LanePoint{point.x, point.y, point.z});
        }

        lanes.emplace_back(std::move(data));
      }

      OsgLaneLine::update(geode, lanes, line_width);
      break;
    }

    case perception::RenderType::kPrediction: {
      std::vector<OsgPrediction::PredictionData> predictions;
      predictions.reserve(layer.polylines.size());

      for (const auto& polyline : layer.polylines) {
        OsgPrediction::PredictionData data;
        data.color = polyline.color;
        data.track_id = polyline.track_id;
        data.confidence = polyline.confidence;
        data.points.reserve(polyline.points.size());

        for (const auto& point : polyline.points) {
          data.points.push_back(OsgPrediction::PredPoint{point.x, point.y, point.z});
        }

        predictions.emplace_back(std::move(data));
      }

      OsgPrediction::update(geode, predictions, line_width);
      break;
    }

    case perception::RenderType::kStopLine: {
      std::vector<OsgStopLine::StopLineData> lines;
      lines.reserve(layer.polylines.size());

      for (const auto& polyline : layer.polylines) {
        OsgStopLine::StopLineData data;
        data.color = polyline.color;
        data.line_type = polyline.type;
        data.points.reserve(polyline.points.size());

        for (const auto& point : polyline.points) {
          data.points.push_back(OsgStopLine::StopLinePoint{point.x, point.y, point.z});
        }

        lines.emplace_back(std::move(data));
      }

      OsgStopLine::update(geode, lines, line_width);
      break;
    }

    case perception::RenderType::kFreespace: {
      std::vector<OsgFreespace::FreespaceData> areas;
      areas.reserve(layer.polylines.size());

      for (const auto& polyline : layer.polylines) {
        OsgFreespace::FreespaceData data;
        data.color = polyline.color;
        data.polygon.reserve(polyline.points.size());

        for (const auto& point : polyline.points) {
          data.polygon.push_back(OsgFreespace::FreespacePoint{point.x, point.y, point.z});
        }

        areas.emplace_back(std::move(data));
      }

      OsgFreespace::update(geode, areas, 0.35f);
      break;
    }

    case perception::RenderType::kHdMap: {
      std::vector<OsgHdMap::MapElement> elements;
      elements.reserve(layer.polylines.size());

      for (const auto& polyline : layer.polylines) {
        OsgHdMap::MapElement data;
        data.color = polyline.color;
        data.element_type = polyline.type;
        data.label = polyline.label;
        data.points.reserve(polyline.points.size());

        for (const auto& point : polyline.points) {
          data.points.push_back(OsgHdMap::MapPoint{point.x, point.y, point.z});
        }

        elements.emplace_back(std::move(data));
      }

      OsgHdMap::update(geode, elements, line_width);
      break;
    }

    case perception::RenderType::kEgoTrajectory: {
      std::vector<OsgEgoTrajectory::TrajectoryData> trajectories;
      trajectories.reserve(layer.polylines.size());

      for (const auto& polyline : layer.polylines) {
        OsgEgoTrajectory::TrajectoryData data;
        data.color = polyline.color;
        data.trajectory_type = polyline.type;
        data.points.reserve(polyline.points.size());

        for (const auto& point : polyline.points) {
          OsgEgoTrajectory::TrajectoryPoint traj_point;
          traj_point.x = point.x;
          traj_point.y = point.y;
          traj_point.z = point.z;
          traj_point.yaw = point.yaw;
          traj_point.speed = point.speed;
          data.points.push_back(traj_point);
        }

        trajectories.emplace_back(std::move(data));
      }

      OsgEgoTrajectory::update(geode, trajectories, line_width);
      break;
    }

    case perception::RenderType::kParkingSlot: {
      std::vector<OsgParkingSlot::SlotData> slot_list;
      slot_list.reserve(layer.parking_slots.size());

      for (const auto& slot : layer.parking_slots) {
        OsgParkingSlot::SlotData data;

        for (int c = 0; c < 4; ++c) {
          for (int axis = 0; axis < 3; ++axis) {
            data.corners[c][axis] = slot.corners[c][axis];
          }
        }

        data.slot_id = slot.slot_id;
        data.slot_type = slot.slot_type;
        data.color = slot.color;
        data.confidence = slot.confidence;
        slot_list.emplace_back(std::move(data));
      }

      OsgParkingSlot::update(geode, slot_list, line_width);
      break;
    }

    case perception::RenderType::kOccupancyGrid: {
      OsgOccupancyGrid::GridData grid;

      if (layer.grid_valid) {
        grid.origin_x = layer.grid.origin_x;
        grid.origin_y = layer.grid.origin_y;
        grid.origin_z = layer.grid.origin_z;
        grid.resolution = layer.grid.resolution;
        grid.width = layer.grid.width;
        grid.height = layer.grid.height;
        grid.cells = layer.grid.cells;
      }

      OsgOccupancyGrid::update(geode, grid, 0.6f);
      break;
    }

    case perception::RenderType::kPointCloud:
    default: {
      render_point_cloud(geode, layer);
      break;
    }
  }
}

#endif  // VLINK_ENABLE_VIEWER_OSG

// NOLINTEND
