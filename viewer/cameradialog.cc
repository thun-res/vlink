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

#include "./cameradialog.h"

#include <vlink/base/helpers.h>
#include <vlink/external/proxy_api.h>
#include <vlink/zerocopy/camera_frame.h>

#include <QDateTime>
#include <QFileDialog>
#include <QFileInfo>
#include <QHideEvent>
#include <QMessageBox>
#include <QPainter>
#include <QSettings>
#include <QShowEvent>
#include <QStandardPaths>
#include <QTimeZone>
#include <cmath>

#include "./mainwindow.h"
#include "./perceptiondialog.h"
#include "./point3ddialog.h"
#include "./settingsdialog.h"
#include "./ui_cameradialog.h"
#include "./ui_mainwindow.h"

#ifdef _WIN32
#ifdef GetMessage
#undef GetMessage
#endif
#endif

#ifdef _WIN32
#pragma warning(push)
#pragma warning(disable : 4127)
#endif

#include <Eigen/Dense>

#ifdef _WIN32
#pragma warning(pop)
#endif

#define USE_USER_CONDITION 1

[[maybe_unused]] static uint32_t get_point3d_color(double value, double min_value, double max_value, bool inversion) {
  double normalized = (value - min_value) / (max_value - min_value);

  normalized = std::clamp(normalized, 0.0, 1.0);

  uint8_t r = 0;
  uint8_t g = 0;
  uint8_t b = 0;

  if (inversion) {
    if (normalized < 0.25) {
      double t = normalized / 0.25;
      r = 255;
      g = static_cast<uint8_t>(t * 165);
      b = 0;
    } else if (normalized < 0.5) {
      double t = (normalized - 0.25) / 0.25;
      r = 255;
      g = 165 + static_cast<uint8_t>(t * (255 - 165));
      b = 0;
    } else if (normalized < 0.75) {
      double t = (normalized - 0.5) / 0.25;
      r = static_cast<uint8_t>((1.0 - t) * 255);
      g = 255;
      b = 0;
    } else {
      double t = (normalized - 0.75) / 0.25;
      r = 0;
      g = 255;
      b = static_cast<uint8_t>(t * 255);
    }
  } else {
    if (normalized < 0.25) {
      double t = normalized / 0.25;
      r = 0;
      g = static_cast<uint8_t>(t * 255);
      b = 255;
    } else if (normalized < 0.5) {
      double t = (normalized - 0.25) / 0.25;
      r = 0;
      g = 255;
      b = 255 - static_cast<uint8_t>(t * 255);
    } else if (normalized < 0.75) {
      double t = (normalized - 0.5) / 0.25;
      r = static_cast<uint8_t>(t * 255);
      g = 255;
      b = 0;
    } else {
      double t = (normalized - 0.75) / 0.25;
      r = 255;
      g = 255 - static_cast<uint8_t>(t * 255);
      b = 0;
    }
  }

  return (static_cast<uint32_t>(r) << 16) | (static_cast<uint32_t>(g) << 8) | static_cast<uint32_t>(b);
}

static bool resolve_flatbuffers_field_path(const FlatbuffersObjectView& root_view, const reflection::Schema& schema,
                                           const std::string& path, FlatbuffersObjectView& parent_view,
                                           const reflection::Field*& field_out) {
  parent_view = {};
  field_out = nullptr;

  if (!root_view.valid() || path.empty()) {
    return false;
  }

  auto current_view = root_view;
  size_t begin = 0;

  while (begin < path.size()) {
    const auto pos = path.find('.', begin);
    const auto token = path.substr(begin, pos == std::string::npos ? std::string::npos : pos - begin);
    const auto* field = find_field(*current_view.object, token);

    if (!field) {
      return false;
    }

    if (pos == std::string::npos) {
      parent_view = current_view;
      field_out = field;
      return true;
    }

    FlatbuffersObjectView child_view;

    if (!get_child_view(current_view, *field, schema, child_view)) {
      return false;
    }

    current_view = child_view;
    begin = pos + 1;
  }

  return false;
}

class CameraLabel : public QLabel {
 public:
  using PathCallback = vlink::MoveFunction<void(const std::string& path, bool whole_label)>;
  using SizeCallback = vlink::MoveFunction<void(int w, int h)>;

  explicit CameraLabel(const QString& title, CameraDialog* camera_dialog, QWidget* parent = nullptr)
      : QLabel(parent), title_(title), camera_dialog_(camera_dialog) {
    setStyleSheet("background-color: rgb(25, 25, 25); color: rgb(225, 225, 225);");
    setAlignment(Qt::AlignCenter);

    QFont font = this->font();
    font.setBold(true);
    font.setPixelSize(30);

    setFont(font);
  }

  ~CameraLabel() {}

  void set_show_info(bool show_info) { show_info_ = show_info; }

  void set_camera_size(QSize size) { size_ = size; }

  void set_timestamp(uint64_t timestamp) { timestamp_ = timestamp; }

  void set_error(bool error) { has_error_ = error; }

  void set_update_points(bool update_points) {
    update_points_ = update_points;
    update();
  };

  void update_info(float fps, float time) {
    fps_ = fps;
    time_ = time;
    update();
  }

  std::string get_title() const { return title_.toStdString(); }

  void register_path_callback(PathCallback&& path_callback) { path_callback_ = std::move(path_callback); }

  void register_size_callback(SizeCallback&& size_callback) { size_callback_ = std::move(size_callback); }

 protected:
  void paintEvent(QPaintEvent* event) override {
    QLabel::paintEvent(event);

    QPainter painter(this);

    QFont font = painter.font();

    if (camera_dialog_->proj_params_.is_valid) {
      if (update_points_) {
        if (points_pixmap_.isNull() || points_pixmap_.width() != width() || points_pixmap_.height() != height()) {
          points_pixmap_ = QPixmap(width(), height());
        }

        points_pixmap_.fill(Qt::transparent);

        QPainter points_painter(&points_pixmap_);

        font.setPixelSize(12);
        points_painter.setFont(font);

        QFontMetrics top_metrics(font);
        QRect proj_rect = top_metrics.boundingRect(0, 0, 0, 0, Qt::AlignLeft | Qt::AlignVCenter, "Projection");

        QRect bottom_rect(width() - proj_rect.width() - 10, 0, proj_rect.width() + 10, proj_rect.height() + 10);
        points_painter.setBrush(QBrush(QColor(255, 255, 0, 100)));
        points_painter.setPen(QColor(0, 0, 0, 0));
        points_painter.drawRect(bottom_rect);

        points_painter.setPen(QColor(255, 255, 255, 200));
        points_painter.drawText(width() - proj_rect.width() - 5, 5, proj_rect.width(), proj_rect.height(),
                                Qt::AlignLeft | Qt::AlignVCenter, "Projection");

#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)

        if (pixmap().width() > 0 && pixmap().height() > 0) {
          double aspect_ratio_pixmap = static_cast<double>(pixmap().width()) / pixmap().height();
#else

        if (pixmap()->width() > 0 && pixmap()->height() > 0) {
          double aspect_ratio_pixmap = static_cast<double>(pixmap()->width()) / pixmap()->height();
#endif
          double aspect_ratio_label = static_cast<double>(width()) / height();

          QRect pixmap_rect;

          if (aspect_ratio_pixmap > aspect_ratio_label) {
            int new_width = width();
            int new_height = new_width / aspect_ratio_pixmap;
            pixmap_rect.setRect(0, 0 + (height() - new_height) / 2, new_width, new_height);
          } else {
            int new_height = height();
            int new_width = new_height * aspect_ratio_pixmap;
            pixmap_rect.setRect(0 + (width() - new_width) / 2, 0, new_width, new_height);
          }

          for (const auto& p : std::as_const(camera_dialog_->projection_points_)) {
            if (p.z() < 0) {
              points_painter.setPen(QPen(QBrush(0xFF55FFFF), camera_dialog_->proj_params_.point_size));
            } else {
              points_painter.setPen(
                  QPen(QBrush(get_point3d_color(p.z(), 0, 255 * camera_dialog_->proj_params_.color_percent,
                                                camera_dialog_->proj_params_.inversion)),
                       camera_dialog_->proj_params_.point_size));
            }

            float px = pixmap_rect.width() / camera_dialog_->proj_params_.img_width * p.x() + pixmap_rect.x();

            float py = pixmap_rect.height() / camera_dialog_->proj_params_.img_height * p.y() + pixmap_rect.y();

            points_painter.drawPoint(px, py);
          }
        }

        points_painter.end();

        update_points_ = false;
      }

      if (!points_pixmap_.isNull()) {
        painter.drawPixmap(0, 0, points_pixmap_);
      }
    }

    if (show_info_) {
      font.setPixelSize(12);
      painter.setFont(font);

      QFontMetrics top_metrics(font);
      QRect text_rect = top_metrics.boundingRect(0, 0, 0, 0, Qt::AlignLeft | Qt::AlignVCenter, title_);

      QRect top_rect(0, 0, text_rect.width() + 10, text_rect.height() + 10);
      painter.setBrush(QBrush(QColor(0, 0, 0, 100)));
      painter.setPen(QColor(0, 0, 0, 0));
      painter.drawRect(top_rect);

      painter.setPen(QColor(255, 255, 255, 200));
      painter.drawText(5, 5, text_rect.width(), text_rect.height(), Qt::AlignLeft | Qt::AlignVCenter, title_);

      if (!has_error_) {
        font.setPixelSize(10);
        painter.setFont(font);

        QString decoding_time_str = time_ >= 0 ? (QString::number(time_, 'f', 2) + "ms") : "---";
        QString bottom_text = "SIZE: " + QString::number(size_.width()) + "x" + QString::number(size_.height()) + "  " +
                              "FPS: " + QString::number(fps_, 'f', 2) + "  " + "ADT: " + decoding_time_str;

        if (timestamp_ > 0) {
          bottom_text += "  TS: ";
          bottom_text += QString::fromStdString(vlink::ProxyAPI::get_format_sys_time(timestamp_));
        }

        QFontMetrics bottom_metrics(font);
        QRect bottom_text_rect = bottom_metrics.boundingRect(0, 0, 0, 0, Qt::AlignLeft | Qt::AlignVCenter, bottom_text);

        QRect bottom_rect(0, height() - bottom_text_rect.height() - 10, bottom_text_rect.width() + 10,
                          bottom_text_rect.height() + 10);
        painter.setBrush(QBrush(QColor(0, 0, 0, 100)));
        painter.setPen(QColor(0, 0, 0, 0));
        painter.drawRect(bottom_rect);

        painter.setPen(QColor(50, 255, 50, 200));
        painter.drawText(5, height() - bottom_text_rect.height() - 5, bottom_text_rect.width(),
                         bottom_text_rect.height(), Qt::AlignLeft | Qt::AlignVCenter, bottom_text);
      }
    }
  }

  void mousePressEvent(QMouseEvent* event) override {
    if (event->button() == Qt::RightButton) {
      right_pressed_ = true;
    } else {
      right_pressed_ = false;
    }

    QLabel::mousePressEvent(event);
  }

  void mouseReleaseEvent(QMouseEvent* event) override {
    if (event->button() == Qt::RightButton && right_pressed_) {
      QMenu menu(this);

      QAction* save_only_action = menu.addAction(tr("Save only image"));
      QAction* save_whole_action = menu.addAction(tr("Save whole label"));

#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
      QAction* selected_action = menu.exec(event->globalPosition().toPoint());
#else
      QAction* selected_action = menu.exec(event->globalPos());
#endif

      if (selected_action == save_only_action || selected_action == save_whole_action) {
        QSettings settings(QStandardPaths::writableLocation(QStandardPaths::CacheLocation) + "/settings.ini",
                           QSettings::IniFormat);

        settings.beginGroup("CameraDialog");

        QFileDialog dialog(MainWindow::get_instance(), tr("Save image file"),
                           settings.value("image_dir", qApp->applicationDirPath()).toString(),
                           "Image files (*.jpg *.png)");

        settings.endGroup();

#if QT_VERSION < QT_VERSION_CHECK(5, 15, 0)
        dialog.setOption(QFileDialog::DontUseNativeDialog, true);
#endif

        dialog.setFileMode(QFileDialog::AnyFile);
        dialog.setAcceptMode(QFileDialog::AcceptSave);
        dialog.setDefaultSuffix("jpg");

        if (dialog.exec() == QDialog::Accepted) {
          QString file_path = dialog.selectedFiles().constFirst();
          if (!file_path.isEmpty()) {
            settings.beginGroup("CameraDialog");
            settings.setValue("image_dir", QFileInfo(file_path).dir().path());
            settings.endGroup();
            settings.sync();

            if (path_callback_) {
              path_callback_(file_path.toStdString(), (selected_action == save_whole_action));
            }
          }
        }
      }
    }

    right_pressed_ = false;

    QLabel::mouseReleaseEvent(event);
  }

  void resizeEvent(QResizeEvent* event) override {
    update_points_ = true;

    if (size_callback_) {
      size_callback_(event->size().width(), event->size().height());
    }

    QLabel::resizeEvent(event);
  }

 private:
  QString title_;
  CameraDialog* camera_dialog_{nullptr};
  bool show_info_{true};
  bool has_error_{false};
  QSize size_{0, 0};
  float fps_{0};
  float time_{0};
  uint64_t timestamp_{0};
  bool right_pressed_{false};
  bool update_points_{false};
  PathCallback path_callback_;
  SizeCallback size_callback_;
  QPixmap points_pixmap_;
};

CameraDialog::CameraDialog(QWidget* parent) : QDialog(parent), ui(new Ui::CameraDialog) {
  window_ = MainWindow::get_instance();

  if (parent) {
    setWindowFlags(Qt::Window | Qt::WindowMaximizeButtonHint | Qt::WindowCloseButtonHint | Qt::WindowStaysOnTopHint);
  } else {
    setWindowFlags(Qt::Window | Qt::WindowMaximizeButtonHint | Qt::WindowCloseButtonHint);
  }

  ui->setupUi(this);

  ui->stackedWidget->setCurrentIndex(0);

  {
    QFont font = ui->textEdit->font();
    font.setFamily("Noto Mono");
    ui->textEdit->setFont(font);
  }

  ui->pushButton_point3d->setEnabled(false);

  ui->label_pause->setVisible(false);

  camera_layout_ = new QGridLayout(ui->widget);

  msg_list_.emplace_back(std::pair{nullptr, nullptr});

  const auto& selected_items = window_->ui->treeWidget_url->selectedItems();

  if (selected_items.count() > 8) {
    ui->comboBox_quality->setCurrentIndex(3);
    display_quality_ = 0.25;
  } else if (selected_items.count() > 4) {
    ui->comboBox_quality->setCurrentIndex(2);
    display_quality_ = 0.5;
  } else if (selected_items.count() > 1) {
    ui->comboBox_quality->setCurrentIndex(1);
    display_quality_ = 0.75;
  } else {
    ui->comboBox_quality->setCurrentIndex(0);
    display_quality_ = 1.0;
  }

  multi_mode_ = selected_items.count() > 1;

  if (multi_mode_) {
    ui->groupBox_info->hide();
  }

  ui->checkBox_cache->setEnabled(FFmpegDecoder::is_valid());
  ui->checkBox_hard->setEnabled(FFmpegDecoder::is_valid());
  ui->label_quality->setEnabled(FFmpegDecoder::is_valid());
  ui->comboBox_quality->setEnabled(FFmpegDecoder::is_valid());

  fbs_field_list_.emplace_back();

  google::protobuf::Descriptor* target_desc = nullptr;

  {
    std::lock_guard lock(window_->data_mutex_);

    select_urls_.clear();

    if (selected_items.count() == 1) {
      QString url = selected_items.at(0)->text(1);
      QString ser = selected_items.at(0)->data(1, Qt::UserRole).toString();

      select_urls_.emplace(url.toStdString());

      if (current_url_.empty()) {
        current_url_ = url.toStdString();
      }

      const auto url_str = url.toStdString();
      const auto ser_str = ser.toStdString();
      const auto schema_iter = window_->schema_type_map_.find(url_str);
      const auto schema_type =
          schema_iter != window_->schema_type_map_.end() ? schema_iter->second : vlink::SchemaType::kUnknown;

      if (schema_type == vlink::SchemaType::kProtobuf && !target_desc && window_->des_pool_) {
        target_desc = const_cast<google::protobuf::Descriptor*>(window_->des_pool_->FindMessageTypeByName(ser_str));
      } else if (schema_type == vlink::SchemaType::kFlatbuffers && !target_fbs_context_) {
        target_fbs_context_ = window_->flatbuffers_runtime_.find_context(ser_str);
      }

      ui->pushButton_point3d->setEnabled(false);
    } else {
      for (const auto& item : selected_items) {
        QString url = item->text(1);
        QString ser = item->data(1, Qt::UserRole).toString();

#if USE_USER_CONDITION
        QString lower_url = url.toLower();
        QString lower_ser = ser.toLower();

        if (lower_url.contains("calib")) {
          continue;
        }

        if (!lower_ser.contains("cam")) {
          continue;
        }
#endif

        select_urls_.emplace(url.toStdString());

        if (current_url_.empty()) {
          current_url_ = url.toStdString();
        }

        const auto url_str = url.toStdString();
        const auto ser_str = ser.toStdString();
        const auto schema_iter = window_->schema_type_map_.find(url_str);
        const auto schema_type =
            schema_iter != window_->schema_type_map_.end() ? schema_iter->second : vlink::SchemaType::kUnknown;

        if (schema_type == vlink::SchemaType::kProtobuf && !target_desc && window_->des_pool_) {
          target_desc = const_cast<google::protobuf::Descriptor*>(window_->des_pool_->FindMessageTypeByName(ser_str));
        } else if (schema_type == vlink::SchemaType::kFlatbuffers && !target_fbs_context_) {
          target_fbs_context_ = window_->flatbuffers_runtime_.find_context(ser_str);
        }
      }

      ui->pushButton_point3d->setEnabled(false);
    }

    auto data_callback = [this](const vlink::ProxyAPI::Data& proxy_data) {
      if (select_urls_.count(proxy_data.url) == 0) {
        std::lock_guard lock(data_mutex_);
        if (data_callback_) {
          data_callback_(proxy_data);
        }

        return;
      }

      if (pause_flag_) {
        return;
      }

      QElapsedTimer timer;
      timer.start();

      const auto schema_type = proxy_data.schema;

      if (schema_type == vlink::SchemaType::kZeroCopy &&
          proxy_data.ser == vlink::Serializer::get_serialized_type<vlink::zerocopy::CameraFrame>()) {
        QMetaObject::invokeMethod(this, "update_ui_for_zero_copy_types", Qt::QueuedConnection,
                                  Q_ARG(QVariant, QVariant::fromValue<vlink::ProxyAPI::Data>(proxy_data)),
                                  Q_ARG(QElapsedTimer, timer));
      } else if (schema_type == vlink::SchemaType::kProtobuf && target_msg_) {
        QMetaObject::invokeMethod(this, "update_ui_for_proto", Qt::QueuedConnection,
                                  Q_ARG(QVariant, QVariant::fromValue<vlink::ProxyAPI::Data>(proxy_data)),
                                  Q_ARG(QElapsedTimer, timer));
      } else if (schema_type == vlink::SchemaType::kFlatbuffers && target_fbs_context_) {
        QMetaObject::invokeMethod(this, "update_ui_for_flatbuffers", Qt::QueuedConnection,
                                  Q_ARG(QVariant, QVariant::fromValue<vlink::ProxyAPI::Data>(proxy_data)),
                                  Q_ARG(QElapsedTimer, timer));
      } else {
        QMetaObject::invokeMethod(this, "update_ui_for_unknown_types", Qt::QueuedConnection,
                                  Q_ARG(QVariant, QVariant::fromValue<vlink::ProxyAPI::Data>(proxy_data)),
                                  Q_ARG(QElapsedTimer, timer));
      }
    };

    if (parent) {
      if (auto* parent_dialog = qobject_cast<Point3DDialog*>(parent)) {
        std::lock_guard plock(parent_dialog->data_mutex_);
        parent_dialog->data_callback_ = std::move(data_callback);
      } else if (auto* perception_dialog = qobject_cast<PerceptionDialog*>(parent)) {
        std::lock_guard plock(perception_dialog->data_mutex_);
        perception_dialog->data_callback_ = std::move(data_callback);
      }
    } else {
      window_->data_callback_ = std::move(data_callback);
    }
  }

  int num_cols = std::ceil(std::sqrt(select_urls_.size()));
  int index = 0;

  for (const auto& url : select_urls_) {
    int row = index / num_cols;
    int col = index % num_cols;

    CameraLabel* label = new CameraLabel(QString::fromStdString(url), this, ui->widget);
    label->setText(tr("No Image"));
    camera_layout_->addWidget(label, row, col);

    Detail detail;
    detail.channel = index;
    detail.label = label;
    detail.frame_count = 0;
    detail.last_frame_count = 0;
    detail.total_rate = 0;
    detail.state = kNoImage;

    Detail& t_detail = camera_detail_map_.emplace(label->get_title(), std::move(detail)).first->second;
    channel_map_.emplace(index, label->get_title());

    label->register_path_callback([&t_detail](const std::string& path, bool whole_label) {
      QFileInfo file_info(QString::fromStdString(path));

      if (whole_label) {
        if (file_info.suffix().toLower() == "jpg") {
          t_detail.label->grab().save(file_info.filePath(), "jpg", 100);
        } else if (file_info.suffix().toLower() == "png") {
          t_detail.label->grab().save(file_info.filePath(), "png", 100);
        }
      } else {
        if (file_info.suffix().toLower() == "jpg") {
          t_detail.img.save(file_info.filePath(), "jpg", 100);
        } else if (file_info.suffix().toLower() == "png") {
          t_detail.img.save(file_info.filePath(), "png", 100);
        }
      }
    });

    label->register_size_callback([label, &t_detail](int w, int h) {
      if (!t_detail.img.isNull()) {
        QPixmap pixmap;

        if (!pixmap.convertFromImage(t_detail.img)) {
          return;
        }

        pixmap =
            pixmap.scaled(w, h, Qt::AspectRatioMode::KeepAspectRatio, Qt::TransformationMode::SmoothTransformation);

        label->setPixmap(pixmap);
      }
    });

    ++index;
  }

  if (select_urls_.size() == 1 && !parent) {
    ui->pushButton_projection->setEnabled(true);
  } else {
    ui->pushButton_projection->setEnabled(false);
  }

  ui->widget->setLayout(camera_layout_);

  if (target_desc) {
    ui->label_proto->setEnabled(true);
    ui->comboBox_proto->setEnabled(true);
    ui->label_offset->setEnabled(false);
    ui->spinBox_offset->setEnabled(false);

    target_msg_ = window_->factory_->GetPrototype(target_desc)->New();

    if (target_msg_) {
      for (int i = 0; i < target_msg_->GetDescriptor()->field_count(); ++i) {
        const auto* field = target_msg_->GetDescriptor()->field(i);

        if (!field->is_repeated() && field->type() == google::protobuf::FieldDescriptor::TYPE_BYTES) {
#if GOOGLE_PROTOBUF_VERSION >= 6030000
          ui->comboBox_proto->addItem(field->name().data());
#else
          ui->comboBox_proto->addItem(field->name().c_str());
#endif
          msg_list_.emplace_back(target_msg_, field);
        } else if (!field->is_repeated() && field->cpp_type() == google::protobuf::FieldDescriptor::CPPTYPE_MESSAGE) {
          auto* sub_msg = &target_msg_->GetReflection()->GetMessage(*target_msg_, field);
          for (int j = 0; j < sub_msg->GetDescriptor()->field_count(); ++j) {
            const auto* sub_field = sub_msg->GetDescriptor()->field(j);

            if (!sub_field->is_repeated() && sub_field->type() == google::protobuf::FieldDescriptor::TYPE_BYTES) {
#if GOOGLE_PROTOBUF_VERSION >= 6030000
              ui->comboBox_proto->addItem(sub_field->name().data());
#else
              ui->comboBox_proto->addItem(sub_field->name().c_str());
#endif
              msg_list_.emplace_back(sub_msg, sub_field);
            }
          }
        }
      }
    }

    if (ui->comboBox_proto->count() > 1) {
      ui->comboBox_proto->setCurrentIndex(1);
      ui->groupBox_config->setEnabled(true);
      ui->groupBox_info->setEnabled(true);
    } else {
      ui->groupBox_config->setEnabled(false);
      ui->groupBox_info->setEnabled(false);
    }
  } else if (target_fbs_context_ && target_fbs_context_->valid()) {
    ui->label_proto->setEnabled(true);
    ui->comboBox_proto->setEnabled(true);
    ui->label_offset->setEnabled(false);
    ui->spinBox_offset->setEnabled(false);

    const auto* root_object = target_fbs_context_->root_object;
    const auto* schema = target_fbs_context_->schema;

    if (root_object && root_object->fields() && schema) {
      for (uint32_t i = 0; i < root_object->fields()->size(); ++i) {
        const auto* field = root_object->fields()->Get(i);

        if (!field) {
          continue;
        }

        if (is_bytes_field(*field)) {
          const auto field_name = field->name()->str();
          ui->comboBox_proto->addItem(QString::fromStdString(field_name));
          fbs_field_list_.emplace_back(field_name);
          continue;
        }

        if (field->type()->base_type() != reflection::Obj || !schema->objects()) {
          continue;
        }

        const auto* child_object = schema->objects()->Get(static_cast<uint32_t>(field->type()->index()));
        if (!child_object || !child_object->fields()) {
          continue;
        }

        for (uint32_t j = 0; j < child_object->fields()->size(); ++j) {
          const auto* child_field = child_object->fields()->Get(j);

          if (!child_field || !is_bytes_field(*child_field)) {
            continue;
          }

          const auto path = field->name()->str() + "." + child_field->name()->str();
          ui->comboBox_proto->addItem(QString::fromStdString(path));
          fbs_field_list_.emplace_back(path);
        }
      }
    }

    if (ui->comboBox_proto->count() > 1) {
      ui->comboBox_proto->setCurrentIndex(1);
      ui->groupBox_config->setEnabled(true);
      ui->groupBox_info->setEnabled(true);
    } else {
      ui->groupBox_config->setEnabled(false);
      ui->groupBox_info->setEnabled(false);
    }
  } else {
    ui->label_proto->setEnabled(false);
    ui->comboBox_proto->setEnabled(false);
    ui->label_offset->setEnabled(true);
    ui->spinBox_offset->setEnabled(true);

    ui->groupBox_config->setEnabled(true);
    ui->groupBox_info->setEnabled(true);
  }

  {
    QSettings settings(QStandardPaths::writableLocation(QStandardPaths::CacheLocation) + "/settings.ini",
                       QSettings::IniFormat);

    const bool use_ext_settings = this->parent() != nullptr;
    QVariant camera_type_value;
    QByteArray geometry;

    settings.beginGroup(use_ext_settings ? "CameraDialog_ext" : "CameraDialog");
    camera_type_value = settings.value("camera_type");
    geometry = settings.value("geometry").toByteArray();
    settings.endGroup();

    if (!camera_type_value.isValid() && use_ext_settings) {
      settings.beginGroup("CameraDialog");
      camera_type_value = settings.value("camera_type");
      settings.endGroup();
    }

    const int camera_type = camera_type_value.isValid() ? camera_type_value.toInt() : 0;
    if (camera_type >= 0 && camera_type < ui->comboBox_type->count()) {
      ui->comboBox_type->setCurrentIndex(camera_type);
    }

    if (!geometry.isEmpty()) {
      restoreGeometry(geometry);
    }
  }

  timer_ = new QTimer(this);
  timer_->setTimerType(Qt::PreciseTimer);
  timer_->setInterval(1000);

  connect(timer_, &QTimer::timeout, this, [this]() {
    for (auto& [url, detail] : camera_detail_map_) {
      if (!detail.label) {
        return;
      }

#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)

      if (!detail.label->pixmap().isNull()) {
#elif QT_VERSION >= QT_VERSION_CHECK(5, 15, 0)

    if (!detail.label->pixmap(Qt::ReturnByValue).isNull()) {
#else

    if (detail.label->pixmap()) {
#endif
        if (detail.frame_count != 0) {
          float real_frame_count = (detail.last_frame_count * 1 + detail.frame_count * 2) / 3.0f;
          detail.last_frame_count = detail.frame_count;
          if (!multi_mode_) {
            ui->label_frame2->setText(QString::number(real_frame_count, 'f', 2));
          }

          if (FFmpegDecoder::is_valid()) {
            detail.label->update_info(real_frame_count, detail.decoder->get_average_decode_cost());
          } else {
            detail.label->update_info(real_frame_count, -1);
          }

          QString total_rate_str;
          if (detail.total_rate < 1024) {
            total_rate_str = QString::number(detail.total_rate) + "B/S";
          } else if (detail.total_rate < 1024 * 1024) {
            total_rate_str = QString::number(detail.total_rate / 1024.0F, 'f', 2) + "KB/S";
          } else {
            total_rate_str = QString::number(detail.total_rate / 1024 / 1024.0F, 'f', 2) + "MB/S";
          }
          if (!multi_mode_) {
            ui->label_transfer2->setText(total_rate_str);
          }

        } else {
          if (detail.state != kNoImage) {
            if (!multi_mode_) {
              ui->label_transfer2->setText("---");
              ui->label_frame2->setText("---");
              ui->label_size2->setText("---");
            }

            // ui->label_img->setPixmap(QPixmap());
            // ui->label_img->setText(tr("No Image"));
            detail.state = kNoImage;
          }

          if (FFmpegDecoder::is_valid()) {
            detail.label->update_info(0, detail.decoder->get_average_decode_cost());
          } else {
            detail.label->update_info(0, -1);
          }
        }
      }

      detail.frame_count = 0;
      detail.total_rate = 0;
    }
  });

  connect(ui->comboBox_proto, static_cast<void (QComboBox::*)(int)>(&QComboBox::currentIndexChanged), this,
          [this](int index) {
            if (index == 0) {
              ui->groupBox_config->setEnabled(false);
              ui->groupBox_info->setEnabled(false);
              ui->groupBox_yuv->setEnabled(false);

              for (auto& [url, detail] : camera_detail_map_) {
                detail.decoder.reset();
              }
            } else {
              ui->groupBox_config->setEnabled(true);
              ui->groupBox_info->setEnabled(true);
              ui->groupBox_yuv->setEnabled(has_yuv_format());

              for (auto& [url, detail] : camera_detail_map_) {
                create_decoder(url, get_decoder_type());
              }
            }
          });

  connect(ui->comboBox_type, static_cast<void (QComboBox::*)(int)>(&QComboBox::currentIndexChanged), this,
          [this](int index) {
            QSettings settings(QStandardPaths::writableLocation(QStandardPaths::CacheLocation) + "/settings.ini",
                               QSettings::IniFormat);

            if (this->parent()) {
              settings.beginGroup("CameraDialog_ext");
            } else {
              settings.beginGroup("CameraDialog");
            }

            settings.setValue("camera_type", index);
            settings.endGroup();
            settings.sync();

            ui->groupBox_yuv->setEnabled(has_yuv_format());

            for (auto& [url, detail] : camera_detail_map_) {
              create_decoder(url, get_decoder_type());
            }
          });

  connect(ui->comboBox_quality, static_cast<void (QComboBox::*)(int)>(&QComboBox::currentIndexChanged), this,
          [this](int index) {
            switch (index) {
              case 0:
                display_quality_ = 1.0;
                break;
              case 1:
                display_quality_ = 0.75;
                break;
              case 2:
                display_quality_ = 0.5;
                break;
              case 3:
                display_quality_ = 0.25;
                break;
              default:
                break;
            }

            (void)index;
            ui->groupBox_yuv->setEnabled(has_yuv_format());

            for (auto& [url, detail] : camera_detail_map_) {
              create_decoder(url, get_decoder_type());
            }
          });

  connect(ui->comboBox_elapsed, static_cast<void (QComboBox::*)(int)>(&QComboBox::currentIndexChanged), this,
          [this](int index) {
            (void)index;

            for (auto& [url, detail] : camera_detail_map_) {
              create_decoder(url, get_decoder_type());
            }
          });

  timer_->start();

  for (auto& [url, detail] : camera_detail_map_) {
    create_decoder(url, get_decoder_type());
  }

  ui->label_transfer2->setText("---");
  ui->label_frame2->setText("---");
  ui->label_size2->setText("---");

  ui->pushButton_close->setFocusPolicy(Qt::NoFocus);
  setFocus();
}

CameraDialog::~CameraDialog() {
  quit_flag_ = true;

  {
    QSettings settings(QStandardPaths::writableLocation(QStandardPaths::CacheLocation) + "/settings.ini",
                       QSettings::IniFormat);

    if (this->parent()) {
      settings.beginGroup("CameraDialog_ext");
    } else {
      settings.beginGroup("CameraDialog");
    }

    settings.setValue("geometry", saveGeometry());

    settings.endGroup();

    settings.sync();
  }

  if (auto* parent_dialog = qobject_cast<Point3DDialog*>(parent())) {
    std::lock_guard lock(parent_dialog->data_mutex_);
    parent_dialog->data_callback_ = nullptr;
  } else if (auto* perception_dialog = qobject_cast<PerceptionDialog*>(parent())) {
    std::lock_guard lock(perception_dialog->data_mutex_);
    perception_dialog->data_callback_ = nullptr;
  } else if (!parent() && window_) {
    std::lock_guard lock(window_->data_mutex_);
    window_->data_callback_ = nullptr;
  }

  {
    std::lock_guard lock(data_mutex_);

    data_callback_ = nullptr;
  }

  for (auto& [url, detail] : camera_detail_map_) {
    detail.decoder.reset();
  }

  if (point3d_dialog_) {
    point3d_dialog_->close();
    point3d_dialog_->deleteLater();
    point3d_dialog_ = nullptr;
  }

  delete ui;

  if (target_msg_) {
    delete target_msg_;
  }

  camera_detail_map_.clear();
}

void CameraDialog::showEvent(QShowEvent* event) {
  if (timer_ && !timer_->isActive()) {
    timer_->start();
  }

  QDialog::showEvent(event);
}

void CameraDialog::hideEvent(QHideEvent* event) { QDialog::hideEvent(event); }

void CameraDialog::closeEvent(QCloseEvent* event) {
  timer_->stop();
  QDialog::closeEvent(event);
}

void CameraDialog::resizeEvent(QResizeEvent* event) { QDialog::resizeEvent(event); }

void CameraDialog::keyPressEvent(QKeyEvent* event) {
  if (event->key() == Qt::Key_Space) {
    on_pushButton_pause_clicked();
  }

  QDialog::keyPressEvent(event);
}

void CameraDialog::keyReleaseEvent(QKeyEvent* event) { QDialog::keyReleaseEvent(event); }

void CameraDialog::update_ui_for_proto(const QVariant& variant, const QElapsedTimer& timer) {
  if (!target_msg_) {
    return;
  }

  if (pause_flag_) {
    return;
  }

  if (timer.elapsed() > 1000) {
    return;
  }

  const auto& proxy_data = variant.value<vlink::ProxyAPI::Data>();
  auto decoder_type = get_decoder_type();

  auto& detail = camera_detail_map_[proxy_data.url];

  if (!detail.label) {
    return;
  }

  auto& [msg, field] = msg_list_.at(ui->comboBox_proto->currentIndex());

  if (!msg || !field) {
    if (detail.state != kNoImage) {
      if (!multi_mode_) {
        ui->label_transfer2->setText("---");
        ui->label_frame2->setText("---");
        ui->label_size2->setText("---");
      }

      detail.label->set_error(true);
      detail.label->setPixmap(QPixmap());
      detail.label->setText(tr("No Image"));
      detail.state = kNoImage;
    }
    return;
  }

  if (!target_msg_->ParseFromArray(proxy_data.raw.data(), proxy_data.raw.size())) {
    if (detail.state != kParseFailed) {
      if (!multi_mode_) {
        ui->label_transfer2->setText("---");
        ui->label_frame2->setText("---");
        ui->label_size2->setText("---");
      }

      detail.label->set_error(true);
      detail.label->setPixmap(QPixmap());
      detail.label->setText(tr("Parse failed"));
      detail.state = kParseFailed;
    }
    return;
  }

  ui->spinBox_offset->setEnabled(false);
  ui->label_offset->setEnabled(false);

  const std::string& raw_str = msg->GetReflection()->GetString(*msg, field);

  const auto& raw_data = vlink::Bytes::shallow_copy(reinterpret_cast<const uint8_t*>(raw_str.c_str()), raw_str.size());

  if (raw_data.empty()) {
    if (detail.state != kParseFailed) {
      if (!multi_mode_) {
        ui->label_transfer2->setText("---");
        ui->label_frame2->setText("---");
        ui->label_size2->setText("---");
      }

      detail.label->set_error(true);
      detail.label->setPixmap(QPixmap());
      detail.label->setText(tr("Parse failed"));
      detail.state = kParseFailed;
    }
    return;
  }

  if (FFmpegDecoder::is_valid()) {
    detail.decoder->post_data(detail.channel, 0, raw_data);
  } else if (decoder_type == FFmpegDecoder::InType::kJPG) {
    process_image(QString::fromStdString(proxy_data.url), 0, 0,
                  QByteArray(reinterpret_cast<const char*>(raw_data.data()), raw_data.size()), false);
  }

  detail.total_rate += raw_data.size();
}

void CameraDialog::update_ui_for_flatbuffers(const QVariant& variant, const QElapsedTimer& timer) {
  if (!target_fbs_context_ || !target_fbs_context_->valid()) {
    return;
  }

  if (pause_flag_) {
    return;
  }

  if (timer.elapsed() > 1000) {
    return;
  }

  const auto& proxy_data = variant.value<vlink::ProxyAPI::Data>();
  auto decoder_type = get_decoder_type();

  auto& detail = camera_detail_map_[proxy_data.url];

  if (!detail.label) {
    return;
  }

  if (ui->comboBox_proto->currentIndex() < 0 ||
      static_cast<size_t>(ui->comboBox_proto->currentIndex()) >= fbs_field_list_.size()) {
    return;
  }

  const auto& field_path = fbs_field_list_.at(ui->comboBox_proto->currentIndex());

  if (field_path.empty()) {
    return;
  }

  FlatbuffersObjectView root_view;

  if (!make_root_view(*target_fbs_context_, proxy_data.raw, root_view)) {
    if (detail.state != kParseFailed) {
      if (!multi_mode_) {
        ui->label_transfer2->setText("---");
        ui->label_frame2->setText("---");
        ui->label_size2->setText("---");
      }

      detail.label->set_error(true);
      detail.label->setPixmap(QPixmap());
      detail.label->setText(tr("Parse failed"));
      detail.state = kParseFailed;
    }
    return;
  }

  FlatbuffersObjectView parent_view;
  const reflection::Field* field = nullptr;

  if (!resolve_flatbuffers_field_path(root_view, *target_fbs_context_->schema, field_path, parent_view, field) ||
      !field) {
    if (detail.state != kParseFailed) {
      if (!multi_mode_) {
        ui->label_transfer2->setText("---");
        ui->label_frame2->setText("---");
        ui->label_size2->setText("---");
      }

      detail.label->set_error(true);
      detail.label->setPixmap(QPixmap());
      detail.label->setText(tr("Parse failed"));
      detail.state = kParseFailed;
    }
    return;
  }

  vlink::Bytes raw_data;

  if (!get_bytes(parent_view, *field, raw_data) || raw_data.empty()) {
    if (detail.state != kParseFailed) {
      if (!multi_mode_) {
        ui->label_transfer2->setText("---");
        ui->label_frame2->setText("---");
        ui->label_size2->setText("---");
      }

      detail.label->set_error(true);
      detail.label->setPixmap(QPixmap());
      detail.label->setText(tr("Parse failed"));
      detail.state = kParseFailed;
    }
    return;
  }

  ui->spinBox_offset->setEnabled(false);
  ui->label_offset->setEnabled(false);

  if (FFmpegDecoder::is_valid()) {
    detail.decoder->post_data(detail.channel, 0, raw_data);
  } else if (decoder_type == FFmpegDecoder::InType::kJPG) {
    process_image(QString::fromStdString(proxy_data.url), 0, 0,
                  QByteArray(reinterpret_cast<const char*>(raw_data.data()), raw_data.size()), false);
  }

  detail.total_rate += raw_data.size();
}

void CameraDialog::update_ui_for_zero_copy_types(const QVariant& variant, const QElapsedTimer& timer) {
  if (pause_flag_) {
    return;
  }

  if (timer.elapsed() > 1000) {
    return;
  }

  const auto& proxy_data = variant.value<vlink::ProxyAPI::Data>();
  auto decoder_type = get_decoder_type();

  auto& detail = camera_detail_map_[proxy_data.url];

  if (!detail.label) {
    return;
  }

  vlink::zerocopy::CameraFrame camera_frame;
  camera_frame << proxy_data.raw;

  if (!camera_frame.data() || camera_frame.size() == 0) {
    if (detail.state != kParseFailed) {
      if (!multi_mode_) {
        ui->label_transfer2->setText("---");
        ui->label_frame2->setText("---");
        ui->label_size2->setText("---");
      }

      detail.label->set_error(true);
      detail.label->setPixmap(QPixmap());
      detail.label->setText(tr("Parse failed"));
      detail.label->set_timestamp(0);
      detail.state = kParseFailed;
    }
    return;
  }

  if (camera_frame.size() > proxy_data.raw.size() - sizeof(camera_frame.header)) {
    if (detail.state != kParseFailed) {
      if (!multi_mode_) {
        ui->label_transfer2->setText("---");
        ui->label_frame2->setText("---");
        ui->label_size2->setText("---");
      }

      detail.label->set_error(true);
      detail.label->setPixmap(QPixmap());
      detail.label->setText(tr("Parse failed"));
      detail.label->set_timestamp(0);
      detail.state = kParseFailed;
    }
    return;
  }

  detail.label->set_timestamp(camera_frame.header.time_meas / 1000);

  if (camera_detail_map_.size() == 1) {
    ui->spinBox_yuv_width->setValue(camera_frame.width());
    ui->spinBox_yuv_height->setValue(camera_frame.height());
  }

  ui->spinBox_offset->setEnabled(false);
  ui->label_offset->setEnabled(false);

  vlink::Bytes raw_data = vlink::Bytes::shallow_copy(camera_frame.data(), camera_frame.size());

  if (FFmpegDecoder::is_valid()) {
    detail.decoder->post_data(detail.channel, 0, raw_data);
  } else if (decoder_type == FFmpegDecoder::InType::kJPG) {
    process_image(QString::fromStdString(proxy_data.url), 0, 0,
                  QByteArray(reinterpret_cast<const char*>(raw_data.data()), raw_data.size()), false);
  }

  detail.total_rate += raw_data.size();
}

void CameraDialog::update_ui_for_unknown_types(const QVariant& variant, const QElapsedTimer& timer) {
  if (pause_flag_) {
    return;
  }

  if (timer.elapsed() > 3000) {
    return;
  }

  const auto& proxy_data = variant.value<vlink::ProxyAPI::Data>();
  auto decoder_type = get_decoder_type();

  auto& detail = camera_detail_map_[proxy_data.url];

  if (!detail.label) {
    return;
  }

  ui->spinBox_offset->setEnabled(true);
  ui->label_offset->setEnabled(true);

  if (static_cast<size_t>(ui->spinBox_offset->value()) >= proxy_data.raw.size()) {
    return;
  }

  const auto& raw_data = vlink::Bytes::shallow_copy(proxy_data.raw.data() + ui->spinBox_offset->value(),
                                                    proxy_data.raw.size() - ui->spinBox_offset->value());

  if (raw_data.empty()) {
    if (detail.state != kParseFailed) {
      if (!multi_mode_) {
        ui->label_transfer2->setText("---");
        ui->label_frame2->setText("---");
        ui->label_size2->setText("---");
      }

      detail.label->set_error(true);
      detail.label->setPixmap(QPixmap());
      detail.label->setText(tr("Parse failed"));
      detail.state = kParseFailed;
    }
    return;
  }

  if (FFmpegDecoder::is_valid()) {
    detail.decoder->post_data(detail.channel, 0, raw_data);
  } else if (decoder_type == FFmpegDecoder::InType::kJPG) {
    process_image(QString::fromStdString(proxy_data.url), 0, 0,
                  QByteArray(reinterpret_cast<const char*>(raw_data.data()), raw_data.size()), false);
  }

  detail.total_rate += raw_data.size();
}

void CameraDialog::on_checkBox_display_clicked(bool checked) {
  for (auto& [url, detail] : camera_detail_map_) {
    if (detail.label) {
      detail.label->set_show_info(checked);
      detail.label->update();
    }
  }
}

void CameraDialog::on_pushButton_close_clicked() { this->close(); }

void CameraDialog::on_pushButton_yuv_clicked() {
  auto decoder_type = get_decoder_type();

  if ((target_msg_ || target_fbs_context_) && ui->comboBox_proto->currentIndex() == 0) {
    for (auto& [url, detail] : camera_detail_map_) {
      detail.decoder.reset();
    }
  } else {
    for (auto& [url, detail] : camera_detail_map_) {
      create_decoder(url, decoder_type);
    }
  }
}

void CameraDialog::on_checkBox_cache_clicked(bool checked) {
  (void)checked;

  for (auto& [url, detail] : camera_detail_map_) {
    create_decoder(url, get_decoder_type());
  }
}

void CameraDialog::on_checkBox_hard_toggled(bool checked) {
  (void)checked;

  for (auto& [url, detail] : camera_detail_map_) {
    create_decoder(url, get_decoder_type());
  }
}

void CameraDialog::on_pushButton_point3d_clicked() {
  if (point3d_dialog_) {
    point3d_dialog_->show();
  } else {
    point3d_dialog_ = new Point3DDialog(this, false);
    point3d_dialog_->show();
  }
}

void CameraDialog::on_pushButton_projection_clicked() {
  if (!projection_dialog_) {
    projection_dialog_ = new ProjectionDialog(this);
  }

  proj_params_ = projection_dialog_->process();

  if (!proj_params_.is_valid) {
    return;
  }

  {
    Eigen::Matrix4f extrinsic_matrix = Eigen::Matrix4f::Identity();

    if (proj_params_.ext_rvec.norm() < 1e-6) {
      extrinsic_matrix.block<3, 3>(0, 0) = Eigen::Matrix3f::Identity();
    } else {
      extrinsic_matrix.block<3, 3>(0, 0) =
          Eigen::AngleAxisf(proj_params_.ext_rvec.norm(), proj_params_.ext_rvec.normalized()).toRotationMatrix();
    }

    extrinsic_matrix.block<3, 1>(0, 3) = proj_params_.ext_tvec;

    projection_matrix_ = proj_params_.in_mat * extrinsic_matrix.block<3, 4>(0, 0);
  }

  if (!point3d_dialog_) {
    point3d_dialog_ = new Point3DDialog(this, true);
    connect(point3d_dialog_, &Point3DDialog::point3d_map_changed, this, [this]() { process_projection(); });
  }

  process_projection();
}

void CameraDialog::on_pushButton_pause_clicked() {
  pause_flag_ = !pause_flag_.load();

  if (pause_flag_) {
    ui->label_pause->setVisible(true);
    ui->pushButton_pause->setText(tr("Resume"));
    ui->pushButton_pause->setIcon(QIcon(":/resource/resume.png"));
  } else {
    ui->label_pause->setVisible(false);
    ui->pushButton_pause->setText(tr("Pause"));
    ui->pushButton_pause->setIcon(QIcon(":/resource/pause.png"));
  }
}

void CameraDialog::process_image(const QString& url, int width, int height, const QByteArray& img_data,
                                 bool use_codec) {
  if (quit_flag_) {
    return;
  }

  auto& detail = camera_detail_map_[url.toStdString()];

  if (!detail.label) {
    return;
  }

  if (img_data.isEmpty()) {
    if (detail.state != kLoadFailed) {
      if (!multi_mode_) {
        ui->label_transfer2->setText("---");
        ui->label_frame2->setText("---");
        ui->label_size2->setText("---");
      }

      detail.label->set_error(true);
      detail.label->setPixmap(QPixmap());
      detail.label->setText(tr("Load failed"));
      detail.state = kLoadFailed;
    }
    return;
  }

  bool ok = false;
  QPixmap pixmap;

  if (use_codec) {
    QImage image(reinterpret_cast<const uint8_t*>(img_data.data()), width, height, QImage::Format_RGB888);
    ok = pixmap.convertFromImage(image);
    detail.img = image.copy();
  } else {
    (void)width;
    (void)height;
    QImage image = QImage::fromData(img_data, "JPG");
    ok = pixmap.convertFromImage(image);
    detail.img = image.copy();
  }

  if (!ok) {
    if (detail.state != kLoadFailed) {
      if (!multi_mode_) {
        ui->label_transfer2->setText("---");
        ui->label_frame2->setText("---");
        ui->label_size2->setText("---");
      }

      detail.label->set_error(true);
      detail.label->setPixmap(QPixmap());
      detail.label->setText(tr("Load failed"));
      detail.state = kLoadFailed;
    }
    return;
  }

  if (FFmpegDecoder::is_valid()) {
    if (!multi_mode_) {
      ui->label_size2->setText(QString::number(pixmap.width() / display_quality_) + "x" +
                               QString::number(pixmap.height() / display_quality_));
    }

    detail.label->set_camera_size(QSize(pixmap.width() / display_quality_, pixmap.height() / display_quality_));
  } else {
    if (!multi_mode_) {
      ui->label_size2->setText(QString::number(pixmap.width()) + "x" + QString::number(pixmap.height()));
    }

    detail.label->set_camera_size(pixmap.size());
  }

  if (detail.state != kLoadSucceed) {
    detail.label->setText("");
  }

  pixmap = pixmap.scaled(detail.label->size(), Qt::AspectRatioMode::KeepAspectRatio,
                         Qt::TransformationMode::SmoothTransformation);

  if (quit_flag_) {
    return;
  }

  detail.label->set_error(false);

  detail.label->setPixmap(pixmap);

  detail.label->update();

  detail.state = kLoadSucceed;

  detail.frame_count += 1;
}

void CameraDialog::process_error(void* label) {
  auto* target_label = static_cast<CameraLabel*>(label);

  if (!target_label) {
    return;
  }

  target_label->set_error(true);
  target_label->setPixmap(QPixmap());
  target_label->setText(tr("Load failed"));
}

void CameraDialog::create_decoder(const std::string& url, FFmpegDecoder::InType type) {
  if (type == FFmpegDecoder::InType::kUnknown) {
    return;
  }

  int width = 0;
  int height = 0;

  if (ui->groupBox_yuv->isEnabled()) {
    width = ui->spinBox_yuv_width->value();
    height = ui->spinBox_yuv_height->value();
  }

  auto& detail = camera_detail_map_[url];

  int max_elapsed_time = 0;
  switch (ui->comboBox_elapsed->currentIndex()) {
    case 0:
      max_elapsed_time = 50;
      break;
    case 1:
      max_elapsed_time = 100;
      break;
    case 2:
      max_elapsed_time = 200;
      break;
    case 3:
      max_elapsed_time = 400;
      break;
    case 4:
      max_elapsed_time = 800;
      break;
    default:
      max_elapsed_time = 0;
      break;
  }

  FFmpegDecoder::Config config;
  config.in_type = type;
  config.out_type = FFmpegDecoder::OutType::kRGB888;
  config.width = width;
  config.height = height;
  config.scale = display_quality_;
  config.cache_frame = ui->checkBox_cache->isChecked();
  config.use_hard_codec = ui->checkBox_hard->isChecked();
  config.max_elapsed_time = max_elapsed_time;
  config.max_codec_time = 0;

  detail.decoder.emplace(config);

  if (!FFmpegDecoder::is_valid()) {
    if (detail.state != kNoSupport) {
      if (!multi_mode_) {
        ui->label_transfer2->setText("---");
        ui->label_frame2->setText("---");
        ui->label_size2->setText("---");
      }

      detail.label->set_error(true);
      detail.label->setPixmap(QPixmap());
      detail.label->setText(tr("Not support"));
      detail.state = kNoSupport;
    }
    return;
  }

  detail.decoder->register_handler([this](int channel, int seq, int width, int height, const vlink::Bytes& img_data) {
    (void)seq;
    QString url = QString::fromStdString(channel_map_[channel]);
    QMetaObject::invokeMethod(
        this, "process_image", Qt::QueuedConnection, Q_ARG(QString, url), Q_ARG(int, width), Q_ARG(int, height),
        Q_ARG(QByteArray, QByteArray(reinterpret_cast<const char*>(img_data.data()), img_data.size())),
        Q_ARG(bool, true));
  });

  detail.decoder->register_error_handler([this, &detail](int channel, int seq) {
    (void)seq;
    (void)channel;
    QMetaObject::invokeMethod(this, "process_error", Qt::QueuedConnection, Q_ARG(void*, detail.label));
  });
}

FFmpegDecoder::InType CameraDialog::get_decoder_type() const {
  switch (ui->comboBox_type->currentIndex()) {
    case 0:
      return FFmpegDecoder::InType::kJPG;
    case 1:
      return FFmpegDecoder::InType::kH264;
    case 2:
      return FFmpegDecoder::InType::kH265;
    case 3:
      return FFmpegDecoder::InType::kMPEG4;
    case 4:
      return FFmpegDecoder::InType::kYUV420;
    case 5:
      return FFmpegDecoder::InType::kYUV422;
    case 6:
      return FFmpegDecoder::InType::kYUV444;
    case 7:
      return FFmpegDecoder::InType::kNV12;
    case 8:
      return FFmpegDecoder::InType::kYUYV;
    case 9:
      return FFmpegDecoder::InType::kYVYU;
    case 10:
      return FFmpegDecoder::InType::kUYVY;
    case 11:
      return FFmpegDecoder::InType::kBGR888;
    case 12:
      return FFmpegDecoder::InType::kRGB888;
    default:
      return FFmpegDecoder::InType::kUnknown;
  }
}

bool CameraDialog::has_yuv_format() const { return get_decoder_type() >= FFmpegDecoder::InType::kYUV420; }

int CameraDialog::get_number_for_msg(const google::protobuf::Message* msg,
                                     const google::protobuf::FieldDescriptor* field) {
  switch (field->cpp_type()) {
    case google::protobuf::FieldDescriptor::CPPTYPE_INT32: {
      return msg->GetReflection()->GetInt32(*msg, field);
    } break;
    case google::protobuf::FieldDescriptor::CPPTYPE_INT64: {
      return msg->GetReflection()->GetInt64(*msg, field);
    } break;
    case google::protobuf::FieldDescriptor::CPPTYPE_UINT32: {
      return msg->GetReflection()->GetUInt32(*msg, field);
    } break;
    case google::protobuf::FieldDescriptor::CPPTYPE_UINT64: {
      return msg->GetReflection()->GetUInt64(*msg, field);
    }
    default:
      return 0;
  }
}

void CameraDialog::process_projection() {
  if (!proj_params_.is_valid) {
    return;
  }

  if (pause_flag_) {
    return;
  }

  projection_points_.clear();

  float k1 = proj_params_.distortion_mat[0];
  float k2 = proj_params_.distortion_mat[1];
  float p1 = proj_params_.distortion_mat[2];
  float p2 = proj_params_.distortion_mat[3];
  float k3 = proj_params_.distortion_mat.size() > 4 ? proj_params_.distortion_mat[4] : 0;

  float proj_fx = proj_params_.in_mat(0, 0);
  float proj_fy = proj_params_.in_mat(1, 1);
  float proj_cx = proj_params_.in_mat(0, 2);
  float proj_cy = proj_params_.in_mat(1, 2);

  for (const auto& [url, list] : point3d_dialog_->get_point3d_map()) {
    for (const auto& [x, y, z, index, c1, c2, intensity, value_list] : list) {
      if (std::isnan(x) || std::isnan(y) || std::isnan(z)) {
        continue;
      }

      Eigen::Vector4f point_3d_homogeneous(x, y, z, 1.0f);

      Eigen::Vector3f point_2d_homogeneous = projection_matrix_ * point_3d_homogeneous;

      if (point_2d_homogeneous.z() <= 0) {
        continue;
      }

      float px = point_2d_homogeneous.x() / point_2d_homogeneous.z();
      float py = point_2d_homogeneous.y() / point_2d_homogeneous.z();

      if (proj_params_.enable_distortion_mat) {
        float cx = (px - proj_cx) / proj_fx;
        float cy = (py - proj_cy) / proj_fy;

        float r2 = cx * cx + cy * cy;
        float radial_distortion = 1 + k1 * r2 + k2 * r2 * r2 + k3 * r2 * r2 * r2;

        float cx_distorted = cx * radial_distortion;
        float cy_distorted = cy * radial_distortion;

        cx_distorted += 2 * p1 * cx * cy + p2 * (r2 + 2 * cx * cx);
        cy_distorted += p1 * (r2 + 2 * cy * cy) + 2 * p2 * cx * cy;

        px = proj_fx * cx_distorted + proj_cx;
        py = proj_fy * cy_distorted + proj_cy;
      }

      if (px < 0 || px > proj_params_.img_width || py < 0 || py > proj_params_.img_height) {
        continue;
      }

      projection_points_.append(QVector3D(px, py, intensity));
    }
  }

  if (!current_url_.empty()) {
    auto& detail = camera_detail_map_[current_url_];

    if (detail.label) {
      detail.label->set_update_points(true);
    }
  }
}

// NOLINTEND
