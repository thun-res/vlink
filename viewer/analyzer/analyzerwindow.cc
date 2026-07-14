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

#include "./analyzerwindow.h"

#include <vlink/base/helpers.h>
#include <vlink/base/logger.h>
#include <vlink/zerocopy/message_parser.h>

#include <QCloseEvent>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFileDialog>
#include <QHideEvent>
#include <QKeyEvent>
#include <QMessageBox>
#include <QMimeData>
#include <QMoveEvent>
#include <QResizeEvent>
#include <QSettings>
#include <QShortcut>
#include <QShowEvent>
#include <QStandardPaths>
#include <QTimer>
#include <algorithm>
#include <atomic>
#include <bit>
#include <cctype>
#include <charconv>
#include <cstring>
#include <fstream>
#include <nlohmann/json.hpp>

#include "./exprtk_parser.h"
#include "./ipcchannel.h"
#include "./ui_analyzerwindow.h"

#ifdef _WIN32
#ifdef GetMessage
#undef GetMessage
#endif
#endif

QString global_proto_dir_config = QStandardPaths::writableLocation(QStandardPaths::HomeLocation) + "/.vlink_proto_dir";
QString global_fbs_dir_config = QStandardPaths::writableLocation(QStandardPaths::HomeLocation) + "/.vlink_fbs_dir";

[[maybe_unused]] static int parse_sub_index(const std::string& field, std::string& base_out) {
  base_out.clear();

  size_t pos_left = field.find('[');
  size_t pos_right = field.find(']');

  if (pos_left == std::string::npos || pos_right == std::string::npos) {
    base_out = field;
    return -1;
  }

  if (pos_right <= pos_left + 1) {
    base_out = field;
    return -1;
  }

  if (pos_right + 1 != field.size()) {
    base_out = field;
    return -1;
  }

  std::string num_str = field.substr(pos_left + 1, pos_right - pos_left - 1);
  int index = -1;
  auto result = std::from_chars(num_str.data(), num_str.data() + num_str.size(), index);

  if (result.ec != std::errc{} || result.ptr != num_str.data() + num_str.size()) {
    base_out = field;
    return -1;
  }

  base_out = field.substr(0, pos_left);

  return index;
}

[[maybe_unused]] static QColor get_rich_color(int count, int index) {
  if (count <= 0) {
    return QColor();
  }

  if (index < 0 || index >= count) {
    return QColor();
  }

  double golden_angle = 137.508;
  double hue = std::fmod(index * golden_angle, 360.0);

  if (hue <= 10) {
    hue = std::fmod(hue + 30.0, 360.0);
  }

  double saturation = 0.8;
  double lightness = 0.5;

  return QColor::fromHslF(hue / 360.0, saturation, lightness);
}

[[maybe_unused]] QCPScatterStyle::ScatterShape get_shape_for_index(int index) {
  switch (index) {
    case 0:
      return QCPScatterStyle::ssCircle;
    case 1:
      return QCPScatterStyle::ssDisc;
    case 2:
      return QCPScatterStyle::ssSquare;
    case 3:
      return QCPScatterStyle::ssDiamond;
    case 4:
      return QCPScatterStyle::ssStar;
    case 5:
      return QCPScatterStyle::ssTriangle;
    case 6:
      return QCPScatterStyle::ssCross;
    case 7:
      return QCPScatterStyle::ssTriangleInverted;
    case 8:
      return QCPScatterStyle::ssCrossSquare;
    case 9:
      return QCPScatterStyle::ssPlusSquare;
    case 10:
      return QCPScatterStyle::ssCrossCircle;
    case 11:
      return QCPScatterStyle::ssPlusCircle;
    case 12:
      return QCPScatterStyle::ssPeace;
    case 13:
      return QCPScatterStyle::ssPlus;
    case 14:
      return QCPScatterStyle::ssDot;
    default:
      return QCPScatterStyle::ssNone;
  }
}

[[maybe_unused]] static void import_protos(google::protobuf::compiler::Importer* importer,
                                           const std::filesystem::path& root_dir, const std::filesystem::path& sub_dir,
                                           bool& has_import, int depth = 0) {
  if VUNLIKELY (depth >= 100) {
    return;
  }

  std::vector<std::filesystem::directory_entry> file_list;

  try {
    for (const auto& entry : std::filesystem::directory_iterator(sub_dir)) {
      file_list.emplace_back(entry);
    }
  } catch (std::filesystem::filesystem_error&) {
    return;
  }

  if VUNLIKELY (file_list.empty() || file_list.size() > 1000) {
    return;
  }

  for (const auto& file : file_list) {
    if (file.is_regular_file() && file.path().extension() == ".proto") {
      try {
#ifdef _WIN32
        auto relative_path = vlink::Helpers::path_to_string(std::filesystem::relative(file.path(), root_dir));
        std::replace(relative_path.begin(), relative_path.end(), '\\', '/');
#else
        auto relative_path = std::filesystem::relative(file.path(), root_dir).string();
#endif
        const auto* ptr = importer->Import(relative_path);

        if (ptr) {
          has_import = true;
        }
      } catch (std::filesystem::filesystem_error&) {
        continue;
      }
    } else if (file.is_directory()) {
      import_protos(importer, root_dir, file.path(), has_import, depth + 1);
    }
  }
}

[[maybe_unused]] static bool get_proto_value(const google::protobuf::Message& message,
                                             const std::vector<std::string>& condition_list, int& depth,
                                             VariantType& result) {
  const google::protobuf::Descriptor* descriptor = message.GetDescriptor();
  const google::protobuf::Reflection* reflection = message.GetReflection();

  if (static_cast<size_t>(depth) >= condition_list.size()) {
    return false;
  }

  std::string condition = condition_list.at(depth);

  int array_pos = -1;

  size_t pos_left = condition.find('[');
  size_t pos_right = condition.find(']');

  if (pos_left != std::string::npos && pos_right != std::string::npos) {
    if (pos_right < condition.size() && pos_right > pos_left) {
      std::string num_str = condition.substr(pos_left + 1, pos_right - pos_left - 1);
      std::from_chars(num_str.data(), num_str.data() + num_str.size(), array_pos);

      condition = condition.substr(0, pos_left);
    }
  }

  for (int i = 0; i < descriptor->field_count(); ++i) {
    const google::protobuf::FieldDescriptor* field = descriptor->field(i);

    if (field->name() != condition) {
      continue;
    }

    if (field->is_repeated()) {
      if (array_pos < 0) {
        continue;
      }

      int count = reflection->FieldSize(message, field);

      for (int j = 0; j < count; ++j) {
        if (j != array_pos) {
          continue;
        }

        if (field->cpp_type() == google::protobuf::FieldDescriptor::CPPTYPE_MESSAGE) {
          if (get_proto_value(reflection->GetRepeatedMessage(message, field, j), condition_list, ++depth, result)) {
            return true;
          } else {
            --depth;
            continue;
          }
        }

        switch (field->cpp_type()) {
          case google::protobuf::FieldDescriptor::CPPTYPE_INT32:
            result = static_cast<int64_t>(reflection->GetRepeatedInt32(message, field, j));
            return true;
          case google::protobuf::FieldDescriptor::CPPTYPE_INT64:
            result = reflection->GetRepeatedInt64(message, field, j);
            return true;
          case google::protobuf::FieldDescriptor::CPPTYPE_UINT32:
            result = static_cast<int64_t>(reflection->GetRepeatedUInt32(message, field, j));
            return true;
          case google::protobuf::FieldDescriptor::CPPTYPE_UINT64:
            result = static_cast<int64_t>(reflection->GetRepeatedUInt64(message, field, j));
            return true;
          case google::protobuf::FieldDescriptor::CPPTYPE_DOUBLE:
            result = reflection->GetRepeatedDouble(message, field, j);
            return true;
          case google::protobuf::FieldDescriptor::CPPTYPE_FLOAT:
            result = static_cast<double>(reflection->GetRepeatedFloat(message, field, j));
            return true;
          case google::protobuf::FieldDescriptor::CPPTYPE_BOOL:
            result = static_cast<int64_t>(reflection->GetRepeatedBool(message, field, j));
            return true;
          case google::protobuf::FieldDescriptor::CPPTYPE_ENUM:
            result = static_cast<int64_t>(reflection->GetRepeatedEnum(message, field, j)->number());
            return true;
          case google::protobuf::FieldDescriptor::CPPTYPE_STRING:
            if (field->type() == google::protobuf::FieldDescriptor::TYPE_BYTES) {
              result = vlink::Bytes::from_string(reflection->GetRepeatedString(message, field, j));
            } else {
              result = reflection->GetRepeatedString(message, field, j);
            }
            return true;
          case google::protobuf::FieldDescriptor::CPPTYPE_MESSAGE:
            return true;
        }
      }
    } else {
      if (field->cpp_type() == google::protobuf::FieldDescriptor::CPPTYPE_MESSAGE) {
        if (get_proto_value(reflection->GetMessage(message, field), condition_list, ++depth, result)) {
          return true;
        } else {
          --depth;
          continue;
        }
      }

      switch (field->cpp_type()) {
        case google::protobuf::FieldDescriptor::CPPTYPE_INT32:
          result = static_cast<int64_t>(reflection->GetInt32(message, field));
          return true;
        case google::protobuf::FieldDescriptor::CPPTYPE_INT64:
          result = reflection->GetInt64(message, field);
          return true;
        case google::protobuf::FieldDescriptor::CPPTYPE_UINT32:
          result = static_cast<int64_t>(reflection->GetUInt32(message, field));
          return true;
        case google::protobuf::FieldDescriptor::CPPTYPE_UINT64:
          result = static_cast<int64_t>(reflection->GetUInt64(message, field));
          return true;
        case google::protobuf::FieldDescriptor::CPPTYPE_DOUBLE:
          result = reflection->GetDouble(message, field);
          return true;
        case google::protobuf::FieldDescriptor::CPPTYPE_FLOAT:
          result = static_cast<double>(reflection->GetFloat(message, field));
          return true;
        case google::protobuf::FieldDescriptor::CPPTYPE_BOOL:
          result = static_cast<int64_t>(reflection->GetBool(message, field));
          return true;
        case google::protobuf::FieldDescriptor::CPPTYPE_ENUM:
          result = static_cast<int64_t>(reflection->GetEnum(message, field)->number());
          return true;
        case google::protobuf::FieldDescriptor::CPPTYPE_STRING:
          if (field->type() == google::protobuf::FieldDescriptor::TYPE_BYTES) {
            result = vlink::Bytes::from_string(reflection->GetString(message, field));
          } else {
            result = reflection->GetString(message, field);
          }
          return true;
        case google::protobuf::FieldDescriptor::CPPTYPE_MESSAGE:
          return true;
      }
    }
  }

  return false;
}

[[maybe_unused]] static bool get_flatbuffers_value(const FlatbuffersObjectView& view, const reflection::Schema& schema,
                                                   const std::vector<std::string>& condition_list, int& depth,
                                                   VariantType& result) {
  if (!view.valid() || static_cast<size_t>(depth) >= condition_list.size()) {
    return false;
  }

  std::string token_name;
  int array_index = -1;
  const auto& token = condition_list.at(depth);
  const bool has_index = split_indexed_token(token, token_name, array_index);

  if (!has_index || token_name.empty()) {
    token_name = token;
  }

  const auto* field = find_field(*view.object, token_name);

  if (!field) {
    return false;
  }

  const auto base_type = field->type()->base_type();

  if (base_type == reflection::Obj) {
    if (array_index >= 0) {
      return false;
    }

    FlatbuffersObjectView child_view;
    if (!get_child_view(view, *field, schema, child_view)) {
      return false;
    }

    ++depth;
    const bool ok = get_flatbuffers_value(child_view, schema, condition_list, depth, result);
    if (!ok) {
      --depth;
    }
    return ok;
  }

  if (base_type == reflection::Vector || base_type == reflection::Vector64) {
    const auto vector_size = get_vector_size(view, *field);

    if (field->type()->element() == reflection::Obj) {
      if (array_index < 0 || static_cast<size_t>(array_index) >= vector_size) {
        return false;
      }

      FlatbuffersObjectView child_view;
      if (!get_vector_elem_view(view, *field, static_cast<size_t>(array_index), schema, child_view)) {
        return false;
      }

      ++depth;
      const bool ok = get_flatbuffers_value(child_view, schema, condition_list, depth, result);
      if (!ok) {
        --depth;
      }
      return ok;
    }

    if (field->type()->element() == reflection::String) {
      if (array_index < 0 || static_cast<size_t>(array_index) >= vector_size) {
        return false;
      }

      result = get_vector_string(view, *field, static_cast<size_t>(array_index));
      return true;
    }

    if (field->type()->element() == reflection::Byte || field->type()->element() == reflection::UByte) {
      if (array_index >= 0) {
        const auto numeric = get_vector_numeric(view, *field, static_cast<size_t>(array_index));
        if (!numeric.has_value()) {
          return false;
        }

        result = static_cast<int64_t>(numeric.value());
        return true;
      }

      vlink::Bytes bytes;
      if (!get_bytes(view, *field, bytes)) {
        return false;
      }

      result = bytes;
      return true;
    }

    if (array_index < 0 || static_cast<size_t>(array_index) >= vector_size) {
      return false;
    }

    const auto numeric = get_vector_numeric(view, *field, static_cast<size_t>(array_index));
    if (!numeric.has_value()) {
      return false;
    }

    if (field->type()->element() == reflection::Float || field->type()->element() == reflection::Double) {
      result = numeric.value();
    } else {
      result = static_cast<int64_t>(numeric.value());
    }

    return true;
  }

  if (array_index >= 0) {
    return false;
  }

  if (base_type == reflection::String) {
    result = get_string(view, *field, &schema);
    return true;
  }

  const auto numeric = get_numeric(view, *field);

  if (!numeric.has_value()) {
    return false;
  }

  if (base_type == reflection::Float || base_type == reflection::Double) {
    result = numeric.value();
  } else {
    result = static_cast<int64_t>(numeric.value());
  }

  return true;
}

class TimeTicker : public QCPAxisTicker {
 protected:
  virtual QString getTickLabel(double tick, const QLocale& locale, QChar formatChar, int precision) override {
    return QCPAxisTicker::getTickLabel(tick, locale, formatChar, precision) + "s";
  }
};

AnalyzerWindow::AnalyzerWindow(const QString& bag_path, bool enable_timeline, QWidget* parent)
    : QMainWindow(parent), ui(new Ui::AnalyzerWindow) {
  default_bag_path_ = bag_path;

  enable_timeline_ = enable_timeline;

  setWindowFlags(Qt::Window | Qt::WindowMinimizeButtonHint | Qt::WindowMaximizeButtonHint | Qt::WindowCloseButtonHint);

  ui->setupUi(this);

  this->setAcceptDrops(true);

  setWindowTitle(tr("VLink Analyzer ") + VLINK_VERSION);

  ui->checkBox_timeline->setEnabled(enable_timeline_);
  ui->checkBox_timeline->setChecked(enable_timeline_);

  if (enable_timeline_) {
    ipc_ = new IpcChannel(this);
    connect(
        ipc_, &IpcChannel::timestamp_changed, this,
        [this](int64_t timestamp) {
          if (ui->checkBox_timeline->isChecked()) {
            current_time_ = timestamp / 1000.0;
            move_timeline(current_time_);
          }
        },
        Qt::QueuedConnection);
  }

  {
    ui->checkBox_time->adjustSize();
    ui->checkBox_time->setMinimumWidth(ui->checkBox_time->width());
    ui->checkBox_time->setText(tr("Limit(%1s):").arg(QString::number(1 * 60)));
  }

  // {
  //   QLinearGradient plot_gradient;
  //   plot_gradient.setStart(0, 0);
  //   plot_gradient.setFinalStop(0, 1);
  //   plot_gradient.setColorAt(0, QColor(240, 240, 240));
  //   plot_gradient.setColorAt(1, QColor(220, 220, 220));

  //   QRadialGradient radial_gradient(0.5, 0.5, 0.5, 0.5, 0.5);
  //   radial_gradient.setCoordinateMode(QGradient::ObjectBoundingMode);
  //   radial_gradient.setColorAt(0, QColor(240, 240, 245));
  //   radial_gradient.setColorAt(1, QColor(220, 220, 230));

  //   ui->plot_widget->setBackground(radial_gradient);
  // }

  {
    QFont font = this->font();
    font.setPixelSize(16);
    font.setBold(true);

    ui->plot_widget->setNotAntialiasedElements(QCP::aeAll);
    ui->plot_widget->setInteractions(QCP::iRangeDrag | QCP::iRangeZoom);
    ui->plot_widget->axisRect()->setRangeDrag(Qt::Horizontal | Qt::Vertical);

    ui->plot_widget->plotLayout()->insertRow(0);
    title_element_ = new QCPTextElement(ui->plot_widget, title_, font);
    ui->plot_widget->plotLayout()->addElement(title_element_);

    default_ticker_ = QSharedPointer<QCPAxisTicker>(new QCPAxisTicker);

    time_ticker_ = QSharedPointer<TimeTicker>(new TimeTicker);
    time_ticker_->setTickCount(6);

    if (type_ == kCustomType) {
      ui->plot_widget->xAxis->setTicker(default_ticker_);
      ui->plot_widget->xAxis->setNumberFormat("gb");
      ui->plot_widget->xAxis->setNumberPrecision(8);

      ui->plot_widget->xAxis2->setVisible(true);
      ui->plot_widget->xAxis2->setTicker(default_ticker_);
      ui->plot_widget->xAxis2->setNumberFormat("gb");
      ui->plot_widget->xAxis2->setNumberPrecision(8);
    } else {
      ui->plot_widget->xAxis->setTicker(time_ticker_);
      ui->plot_widget->xAxis->setNumberFormat("f");
      ui->plot_widget->xAxis->setNumberPrecision(3);

      ui->plot_widget->xAxis2->setVisible(true);
      ui->plot_widget->xAxis2->setTicker(time_ticker_);
      ui->plot_widget->xAxis2->setNumberFormat("f");
      ui->plot_widget->xAxis2->setNumberPrecision(3);
    }

    ui->plot_widget->xAxis->grid()->setVisible(true);
    ui->plot_widget->yAxis->grid()->setVisible(true);
    ui->plot_widget->xAxis->grid()->setSubGridVisible(true);
    ui->plot_widget->yAxis->grid()->setSubGridVisible(true);

    {
      QPen pen = ui->plot_widget->xAxis->grid()->pen();
      pen.setWidth(1);
      pen.setStyle(Qt::DashLine);

#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
      pen.setColor(QColor(0x888888));
#else
      pen.setColor("#888888");
#endif

      ui->plot_widget->xAxis->grid()->setZeroLinePen(pen);
      ui->plot_widget->yAxis->grid()->setZeroLinePen(pen);
    }

    ui->plot_widget->yAxis->setNumberFormat("gb");
    ui->plot_widget->yAxis->setNumberPrecision(8);

    ui->plot_widget->legend->setWrap(16);

    connect(ui->plot_widget->xAxis, QOverload<const QCPRange&, const QCPRange&>::of(&QCPAxis::rangeChanged), this,
            [this](const QCPRange& new_range, const QCPRange& old_range) {
              if (timeline_) {
                timeline_->start->setCoords(current_time_, ui->plot_widget->yAxis->range().lower);
                timeline_->end->setCoords(current_time_, ui->plot_widget->yAxis->range().upper);
              }

              if (ui->checkBox_time->isEnabled() && ui->checkBox_time->isChecked()) {
                if (new_range.upper - new_range.lower > x_range_.upper - x_range_.lower) {
                  ui->plot_widget->xAxis->blockSignals(true);
                  ui->plot_widget->xAxis->setRange(old_range);
                  ui->plot_widget->xAxis2->setRange(old_range);
                  ui->plot_widget->xAxis->blockSignals(false);
                  return;
                }
              }

              QCPRange bounded_range = new_range.bounded(x_limit_range_.lower, x_limit_range_.upper);
              ui->plot_widget->xAxis->setRange(bounded_range);
              ui->plot_widget->xAxis2->setRange(bounded_range);
            });

    connect(ui->plot_widget->yAxis, QOverload<const QCPRange&, const QCPRange&>::of(&QCPAxis::rangeChanged), this,
            [this](const QCPRange& new_range, const QCPRange&) {
              if (timeline_) {
                timeline_->start->setCoords(current_time_, ui->plot_widget->yAxis->range().lower);
                timeline_->end->setCoords(current_time_, ui->plot_widget->yAxis->range().upper);
              }

              QCPRange bounded_range = new_range.bounded(y_limit_range_.lower, y_limit_range_.upper);
              ui->plot_widget->yAxis->setRange(bounded_range);
            });

    connect(ui->plot_widget, &QCustomPlot::mouseDoubleClick, this, [this](QMouseEvent* event) {
      if (!ui->groupBox_view->isEnabled() || !ui->checkBox_timeline->isChecked()) {
        return;
      }

      if (event->button() != Qt::LeftButton) {
        return;
      }

      bool x_axis_clicked = ui->plot_widget->xAxis->selectTest(event->pos(), false) >= 0;
      bool x2_axis_clicked = ui->plot_widget->xAxis2->selectTest(event->pos(), false) >= 0;

      if (x_axis_clicked) {
        double value = ui->plot_widget->xAxis->pixelToCoord(event->pos().x());

        current_time_ = value;
        move_timeline(current_time_);

        if (ipc_ && (type_ == kFrequencyType || type_ == kValueType)) {
          ipc_->send_timestamp(current_time_ * 1000);
        }

        event->ignore();
        return;
      }

      if (x2_axis_clicked) {
        double value = ui->plot_widget->xAxis2->pixelToCoord(event->pos().x());

        current_time_ = value;
        move_timeline(current_time_);

        if (ipc_ && (type_ == kFrequencyType || type_ == kValueType)) {
          ipc_->send_timestamp(current_time_ * 1000);
        }

        event->ignore();
        return;
      }
    });

    connect(ui->plot_widget, &QCustomPlot::itemDoubleClick, this, [this](QCPAbstractItem* item, QMouseEvent* event) {
      if (event->button() != Qt::LeftButton) {
        return;
      }

      if (timeline_ == item) {
        return;
      }

      auto* target_item = static_cast<QCPItemText*>(item);

      if (target_item) {
        if (text_items_.removeOne(target_item)) {
          ui->plot_widget->removeItem(item);

          ui->plot_widget->replot();
        }
      }
    });

    connect(ui->plot_widget, &QCustomPlot::plottableDoubleClick, this,
            [this](QCPAbstractPlottable* plottable, int dataIndex, QMouseEvent* event) {
              if (event->button() != Qt::LeftButton) {
                return;
              }

              QCPGraph* graph = qobject_cast<QCPGraph*>(plottable);

              if (!graph) {
                return;
              }

              double x = graph->data()->at(dataIndex)->key;
              double y = graph->data()->at(dataIndex)->value;

              for (auto* item : std::as_const(text_items_)) {
                if (item->position->key() == x && item->position->value() == y) {
                  return;
                }
              }

              auto* item = new QCPItemText(ui->plot_widget);
              text_items_.append(item);

              item->setTextAlignment(Qt::AlignVCenter | Qt::AlignLeft);
              item->setPositionAlignment(Qt::AlignTop | Qt::AlignLeft);
              item->position->setCoords(x, y);

              QString y_str;

              switch (ui->comboBox_count->currentIndex()) {
                case 0:
                  y_str = QString::number(y, 'g', 8);
                  break;
                case 1:
                  y_str = QString::number(y, 'f', 2);
                  break;
                case 2:
                  y_str = QString::number(y, 'f', 4);
                  break;
                case 3:
                  y_str = QString::number(y, 'f', 6);
                  break;
                case 4:
                  y_str = QString::number(y, 'f', 8);
                  break;
                case 5:
                  y_str = QString::number(y, 'e', 2);
                  break;
                case 6:
                  y_str = QString::number(y, 'e', 4);
                  break;
                case 7:
                  y_str = QString::number(y, 'e', 6);
                  break;
                case 8:
                  y_str = QString::number(y, 'e', 8);
                  break;
                case 9:
                  y_str = QString::number(y, 'f', 0);
                  break;
                default:
                  break;
              }

              if (type_ == kFrequencyType) {
                item->setText(QString(" %1s ").arg(QString::number(x, 'f', 3)));
              } else if (type_ == kValueType) {
                item->setText(QString(" %1 \n %2s ").arg(y_str, QString::number(x, 'f', 3)));
              } else if (type_ == kCustomType) {
                item->setText(QString(" x: %2 \n y: %1 ").arg(y_str, QString::number(x, 'f', 3)));
              }

              QFont font = ui->plot_widget->font();
              font.setPixelSize(12);

              item->setFont(font);
              item->setPen(graph->pen());
              item->setBrush(QBrush(Qt::white));

              ui->plot_widget->replot();
            });

    connect(ui->plot_widget, &QCustomPlot::legendClick, this,
            [this](QCPLegend*, QCPAbstractLegendItem* item, QMouseEvent*) {
              QCPPlottableLegendItem* legend_item = qobject_cast<QCPPlottableLegendItem*>(item);

              if (legend_item) {
                QCPGraph* graph = qobject_cast<QCPGraph*>(legend_item->plottable());
                if (graph) {
                  graph->setVisible(!graph->visible());

                  QFont font = legend_item->font();
                  if (graph->visible()) {
                    font.setStrikeOut(false);
                  } else {
                    font.setStrikeOut(true);
                  }

                  legend_item->setFont(font);

                  ui->plot_widget->replot();
                }
              }
            });

    connect(ui->plot_widget, &QCustomPlot::mousePress, this, [this](QMouseEvent* event) {
      (void)event;
      mouse_pressed_ = true;
    });

    connect(ui->plot_widget, &QCustomPlot::mouseRelease, this, [this](QMouseEvent* event) {
      (void)event;
      mouse_pressed_ = false;
    });

    connect(ui->plot_widget, &QCustomPlot::mouseMove, this, [this](QMouseEvent* event) {
      if (mouse_pressed_ || !ui->checkBox_tracking->isChecked()) {
        return;
      }

      double x = ui->plot_widget->xAxis->pixelToCoord(event->pos().x());
      double y = ui->plot_widget->yAxis->pixelToCoord(event->pos().y());

      QString y_str;

      switch (ui->comboBox_count->currentIndex()) {
        case 0:
          y_str = QString::number(y, 'g', 8);
          break;
        case 1:
          y_str = QString::number(y, 'f', 2);
          break;
        case 2:
          y_str = QString::number(y, 'f', 4);
          break;
        case 3:
          y_str = QString::number(y, 'f', 6);
          break;
        case 4:
          y_str = QString::number(y, 'f', 8);
          break;
        case 5:
          y_str = QString::number(y, 'e', 2);
          break;
        case 6:
          y_str = QString::number(y, 'e', 4);
          break;
        case 7:
          y_str = QString::number(y, 'e', 6);
          break;
        case 8:
          y_str = QString::number(y, 'e', 8);
          break;
        case 9:
          y_str = QString::number(y, 'f', 0);
          break;
        default:
          break;
      }

      QString tooltip_text;

      if (type_ == kFrequencyType) {
        tooltip_text = QString(" %1s ").arg(QString::number(x, 'f', 3));
      } else if (type_ == kValueType) {
        tooltip_text = QString(" %1 \n %2s ").arg(y_str, QString::number(x, 'f', 3));
      } else if (type_ == kCustomType) {
        tooltip_text = QString(" x: %2 \n y: %1 ").arg(y_str, QString::number(x, 'f', 3));
      }

#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
      QToolTip::showText(event->globalPosition().toPoint(), tooltip_text, this, QRect{}, 3000);
#else
      QToolTip::showText(event->globalPos(), tooltip_text, this, QRect{}, 3000);
#endif
    });

    connect(ui->plot_widget, &QCustomPlot::axisClick, this,
            [this](QCPAxis* axis, QCPAxis::SelectablePart part, QMouseEvent* event) {
              if (axis != ui->plot_widget->yAxis || part != QCPAxis::spTickLabels) {
                return;
              }

              if (event->button() != Qt::RightButton) {
                return;
              }

              if (type_ == kFrequencyType) {
                auto ticker = qSharedPointerDynamicCast<QCPAxisTickerText>(axis->ticker());

                if (!ticker) {
                  return;
                }

                const auto& ticks = ticker->ticks();
                double clicked_value = axis->pixelToCoord(event->pos().y());

                QString closest_label;
                double min_distance = std::numeric_limits<double>::max();

                const auto& keys = ticks.keys();
                for (const auto& key : keys) {
                  double distance = std::abs(key - clicked_value);
                  if (distance < min_distance) {
                    min_distance = distance;
                    closest_label = ticks[key];
                  }
                }

                QMenu menu;
                menu.addAction(tr("Copy Url"), this, [closest_label]() { qApp->clipboard()->setText(closest_label); });
                menu.exec(QCursor::pos());
              }
            });

    clear_plot();

    ui->plot_widget->xAxis->setRange(x_range_);
    ui->plot_widget->xAxis2->setRange(x_range_);
    ui->plot_widget->yAxis->setRange(y_range_);
  }

  progress_timer_ = new QTimer(this);
  progress_timer_->setInterval(100);

  connect(progress_timer_, &QTimer::timeout, this, &AnalyzerWindow::update_progress);

  ui->progressBar->setTextVisible(true);

  {
    proto_dir_ = QStandardPaths::writableLocation(QStandardPaths::HomeLocation);
    fbs_dir_ = QStandardPaths::writableLocation(QStandardPaths::HomeLocation);
    QFile proto_file(global_proto_dir_config);
    QFile fbs_file(global_fbs_dir_config);

    if (proto_file.exists() && proto_file.open(QFile::ReadOnly)) {
      proto_dir_ = proto_file.readAll().simplified();
      proto_file.close();
    }

    if (fbs_file.exists() && fbs_file.open(QFile::ReadOnly)) {
      fbs_dir_ = fbs_file.readAll().simplified();
      fbs_file.close();
    }

    ui->lineEdit_proto->setText(proto_dir_);
    ui->lineEdit_fbs->setText(fbs_dir_);

    QSettings settings(QStandardPaths::writableLocation(QStandardPaths::CacheLocation) + "/settings.ini",
                       QSettings::IniFormat);
    settings.beginGroup("AnalyzerWindow");

    file_path_ = settings.value("bag_path", "").toString();

    config_path_ = settings.value("config_path", "").toString();
    ui->lineEdit_config->setText(config_path_);

    auto geometry = settings.value("geometry", this->geometry()).toByteArray();

    settings.endGroup();

    restoreGeometry(geometry);
  }

  if (!bag_path.isEmpty()) {
    load_bag(bag_path);
  }

  // if (!proto_dir_) {
  //   load_proto(proto_dir_);
  // }

  update_status();
}

AnalyzerWindow::~AnalyzerWindow() {
  quit_flag_ = true;

  {
    QSettings settings(QStandardPaths::writableLocation(QStandardPaths::CacheLocation) + "/settings.ini",
                       QSettings::IniFormat);
    settings.beginGroup("AnalyzerWindow");
    settings.setValue("geometry", saveGeometry());
    settings.endGroup();
    settings.sync();
  }

  player_.reset();

  delete ui;
}

void AnalyzerWindow::showEvent(QShowEvent* event) { QMainWindow::showEvent(event); }

void AnalyzerWindow::hideEvent(QHideEvent* event) { QMainWindow::hideEvent(event); }

void AnalyzerWindow::closeEvent(QCloseEvent* event) { QMainWindow::closeEvent(event); }

void AnalyzerWindow::resizeEvent(QResizeEvent* event) { QMainWindow::resizeEvent(event); }

void AnalyzerWindow::dragEnterEvent(QDragEnterEvent* event) {
  if (!ui->toolButton_path->isEnabled()) {
    event->ignore();
    return;
  }

  if (event->mimeData()->hasUrls()) {
    auto urls = event->mimeData()->urls();

    if (urls.size() != 1) {
      event->ignore();
      return;
    }

    QString file_path = urls.first().toLocalFile();
    QFileInfo file_info(file_path);

    if (file_info.suffix() != "json" && file_info.suffix() != "vdb" && file_info.suffix() != "vdbx" &&
        file_info.suffix() != "vcap" && file_info.suffix() != "vcapx") {
      event->ignore();
      return;
    }

    event->acceptProposedAction();
  } else {
    event->ignore();
  }
}

void AnalyzerWindow::dropEvent(QDropEvent* event) {
  if (!ui->toolButton_path->isEnabled()) {
    event->ignore();
    return;
  }

  if (event->mimeData()->hasUrls()) {
    auto urls = event->mimeData()->urls();

    if (urls.size() != 1) {
      event->ignore();
      return;
    }

    QString file_path = urls.first().toLocalFile();
    QFileInfo file_info(file_path);

    if (file_info.suffix() == "vdb" || file_info.suffix() == "vdbx" || file_info.suffix() == "vcap" ||
        file_info.suffix() == "vcapx") {
      load_bag(file_info.absoluteFilePath());

      event->acceptProposedAction();
    } else if (file_info.suffix() == "json") {
      config_path_ = file_info.absoluteFilePath();
      ui->lineEdit_config->setText(config_path_);

      update_status();

      event->acceptProposedAction();
    } else {
      event->ignore();
    }

  } else {
    event->ignore();
  }
}

void AnalyzerWindow::moveEvent(QMoveEvent* event) { QMainWindow::moveEvent(event); }

void AnalyzerWindow::keyPressEvent(QKeyEvent* event) {
  if (event->key() == Qt::Key_Space) {
    reset_plot();
    return;
  }

  if (!event->isAutoRepeat() && (type_ == kValueType || type_ == kCustomType)) {
    if (event->key() == Qt::Key_Control || event->key() == Qt::Key_X) {
      last_zoom_index_ = ui->comboBox_zoom->currentIndex();
      ui->comboBox_zoom->setCurrentIndex(1);
    } else if (event->key() == Qt::Key_Shift || event->key() == Qt::Key_Y) {
      last_zoom_index_ = ui->comboBox_zoom->currentIndex();
      ui->comboBox_zoom->setCurrentIndex(2);
    }
  }

  QMainWindow::keyPressEvent(event);
}

void AnalyzerWindow::keyReleaseEvent(QKeyEvent* event) {
  if (!event->isAutoRepeat() && (type_ == kValueType || type_ == kCustomType)) {
    if (event->key() == Qt::Key_Control || event->key() == Qt::Key_Shift || event->key() == Qt::Key_X ||
        event->key() == Qt::Key_Y) {
      ui->comboBox_zoom->setCurrentIndex(last_zoom_index_);
    }
  }

  QMainWindow::keyReleaseEvent(event);
}

void AnalyzerWindow::on_groupBox_time_clicked(bool checked) {
  (void)checked;

  if (player_) {
    ui->doubleSpinBox_begin->setMaximum(player_->get_info().total_duration / 1000.0);
    ui->doubleSpinBox_end->setMaximum(player_->get_info().total_duration / 1000.0);

    ui->doubleSpinBox_begin->setMinimum(player_->get_info().blank_duration / 1000.0);
    ui->doubleSpinBox_end->setMinimum(player_->get_info().blank_duration / 1000.0);

    ui->doubleSpinBox_begin->setValue(player_->get_info().blank_duration / 1000.0);
    ui->doubleSpinBox_end->setValue(player_->get_info().total_duration / 1000.0);
  }
}

void AnalyzerWindow::on_toolButton_path_clicked() {
  QFileInfo file_info(file_path_);

  if (file_info.isFile() && file_info.exists()) {
    file_path_ = file_info.absoluteFilePath();
  } else {
    file_path_ = qApp->applicationDirPath();
  }

  QFileDialog dialog(this, tr("Select bag file"), file_path_, "Bag files (*.vdb *.vdbx *.vcap *.vcapx)");

#if QT_VERSION < QT_VERSION_CHECK(5, 15, 0)
  dialog.setOption(QFileDialog::DontUseNativeDialog, true);
#endif

  dialog.setFileMode(QFileDialog::ExistingFile);
  dialog.setAcceptMode(QFileDialog::AcceptOpen);
  dialog.setDefaultSuffix("vdb");

  if (dialog.exec() != QDialog::Accepted) {
    return;
  }

  load_bag(dialog.selectedFiles().constFirst());
}

void AnalyzerWindow::on_toolButton_config_clicked() {
  QFileInfo file_info(config_path_);

  if (file_info.isFile() && file_info.exists()) {
    config_path_ = file_info.absoluteFilePath();
  } else {
    config_path_ = qApp->applicationDirPath();
  }

  QFileDialog dialog(this, tr("Select config file"), config_path_, "Config files (*.json)");

#if QT_VERSION < QT_VERSION_CHECK(5, 15, 0)
  dialog.setOption(QFileDialog::DontUseNativeDialog, true);
#endif

  dialog.setFileMode(QFileDialog::ExistingFile);
  dialog.setAcceptMode(QFileDialog::AcceptOpen);
  dialog.setDefaultSuffix("json");

  if (dialog.exec() != QDialog::Accepted) {
    return;
  }

  config_path_ = dialog.selectedFiles().constFirst();
  ui->lineEdit_config->setText(config_path_);

  {
    QSettings settings(QStandardPaths::writableLocation(QStandardPaths::CacheLocation) + "/settings.ini",
                       QSettings::IniFormat);
    settings.beginGroup("AnalyzerWindow");
    settings.setValue("config_path", config_path_);
    settings.endGroup();
    settings.sync();
  }

  update_status();
}

void AnalyzerWindow::on_toolButton_proto_clicked() {
  QFileInfo file_info(proto_dir_);

  if (file_info.isDir() && file_info.exists()) {
    proto_dir_ = file_info.absoluteFilePath();
  } else {
    proto_dir_ = QStandardPaths::writableLocation(QStandardPaths::HomeLocation);
  }

  QFileDialog dialog(this, tr("Select proto directory"), proto_dir_);

#if QT_VERSION < QT_VERSION_CHECK(5, 15, 0) || defined(Q_OS_LINUX)
  dialog.setOption(QFileDialog::DontUseNativeDialog, true);
#endif

  dialog.setFileMode(QFileDialog::Directory);
  dialog.setAcceptMode(QFileDialog::AcceptOpen);

  if (dialog.exec() != QDialog::Accepted) {
    return;
  }

  proto_dir_ = dialog.selectedFiles().constFirst();
  ui->lineEdit_proto->setText(proto_dir_);

  QFile file(global_proto_dir_config);

  if (file.open(QFile::WriteOnly | QFile::Truncate)) {
    file.write(proto_dir_.toUtf8());
    file.close();
  }

  update_status();
}

void AnalyzerWindow::on_toolButton_fbs_clicked() {
  QFileInfo file_info(fbs_dir_);

  if (file_info.isDir() && file_info.exists()) {
    fbs_dir_ = file_info.absoluteFilePath();
  } else {
    fbs_dir_ = QStandardPaths::writableLocation(QStandardPaths::HomeLocation);
  }

  QFileDialog dialog(this, tr("Select FlatBuffers directory"), fbs_dir_);

#if QT_VERSION < QT_VERSION_CHECK(5, 15, 0) || defined(Q_OS_LINUX)
  dialog.setOption(QFileDialog::DontUseNativeDialog, true);
#endif

  dialog.setFileMode(QFileDialog::Directory);
  dialog.setAcceptMode(QFileDialog::AcceptOpen);

  if (dialog.exec() != QDialog::Accepted) {
    return;
  }

  fbs_dir_ = dialog.selectedFiles().constFirst();
  ui->lineEdit_fbs->setText(fbs_dir_);

  QFile file(global_fbs_dir_config);

  if (file.open(QFile::WriteOnly | QFile::Truncate)) {
    file.write(fbs_dir_.toUtf8());
    file.close();
  }

  update_status();
}

void AnalyzerWindow::on_pushButton_gen_clicked() {
  if (!player_) {
    return;
  }

  if (!load_config(config_path_)) {
    return;
  }

  if (!load_proto(proto_dir_)) {
    return;
  }

  if (!load_fbs(fbs_dir_)) {
    return;
  }

  if (status_ == vlink::BagReader::kPlaying || status_ == vlink::BagReader::kPaused) {
    return;
  }

  ready_to_start_ = false;
  interrupted_ = false;
  finished_ = false;

  x_min_value_ = QCPRange::maxRange;
  x_max_value_ = -QCPRange::maxRange;

  y_min_value_ = QCPRange::maxRange;
  y_max_value_ = -QCPRange::maxRange;

  vlink::BagReader::Config config;

  if (ui->groupBox_time->isChecked()) {
    config.begin_time = ui->doubleSpinBox_begin->value() * 1000;
    config.end_time = ui->doubleSpinBox_end->value() * 1000;
  } else {
    config.begin_time = 0;
    config.end_time = 0;
  }

  config.times = 1;
  config.rate = 1;
  config.skip_blank = false;
  config.force_delay = 0;
  config.auto_pause = false;
  config.auto_quit = false;
  config.filter_urls = filter_urls_;

  status_ = vlink::BagReader::kPlaying;

  player_->play(config);

  progress_timer_->stop();
  progress_timer_->start();
}

void AnalyzerWindow::on_pushButton_interrupt_clicked() {
  if (!player_) {
    return;
  }

  if (status_ != vlink::BagReader::kPlaying && status_ != vlink::BagReader::kPaused) {
    return;
  }

  ready_to_start_ = false;
  interrupted_ = true;
  finished_ = false;

  player_->stop();
  progress_timer_->stop();

  player_->wait_for_idle();

  status_ = vlink::BagReader::kStopped;

  set_progress(0);
}

void AnalyzerWindow::on_pushButton_export_clicked() {
  QFileDialog dialog(this, tr("Export to image"), QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation),
                     "Image files (*.png)");

#if QT_VERSION < QT_VERSION_CHECK(5, 15, 0)
  dialog.setOption(QFileDialog::DontUseNativeDialog, true);
#endif

  dialog.setFileMode(QFileDialog::AnyFile);
  dialog.setAcceptMode(QFileDialog::AcceptSave);
  dialog.setDefaultSuffix("png");

  if (dialog.exec() != QDialog::Accepted) {
    return;
  }

  QString export_img_path = dialog.selectedFiles().constFirst();

  ui->plot_widget->savePng(export_img_path);
}

void AnalyzerWindow::on_pushButton_close_clicked() { this->close(); }

void AnalyzerWindow::on_horizontalSlider_time_valueChanged(int value) {
  if (ui->horizontalSlider_time->isSliderDown()) {
    return;
  }

  if (ui->checkBox_time->isChecked()) {
    ui->checkBox_time->setText(tr("Limit(%1s):").arg(QString::number(value * 60)));
  } else {
    ui->checkBox_time->setText(tr("Limit(~s):"));
  }

  if (player_ && (type_ == kFrequencyType || type_ == kValueType)) {
    adjust_x_range(value * 60.0);
  }

  reset_plot();
}

void AnalyzerWindow::on_checkBox_time_clicked(bool checked) {
  if (checked) {
    ui->checkBox_time->setText(tr("Limit(%1s):").arg(QString::number(ui->horizontalSlider_time->value() * 60)));
  } else {
    ui->checkBox_time->setText(tr("Limit(~s):"));
  }

  ui->horizontalSlider_time->setEnabled(checked);

  if (player_ && (type_ == kFrequencyType || type_ == kValueType)) {
    if (checked) {
      adjust_x_range(ui->horizontalSlider_time->value() * 60.0);
    } else {
      adjust_x_range(0);
    }
  }

  reset_plot();
}

void AnalyzerWindow::on_checkBox_legend_clicked(bool checked) {
  if (type_ == kFrequencyType) {
    //
  } else {
    ui->plot_widget->legend->setVisible(checked);

    ui->plot_widget->replot();
  }
}

void AnalyzerWindow::on_checkBox_grid_clicked(bool checked) {
  if (type_ == kFrequencyType) {
    ui->plot_widget->xAxis->grid()->setVisible(checked);
    ui->plot_widget->yAxis->grid()->setVisible(false);

    ui->plot_widget->xAxis->grid()->setSubGridVisible(checked);
    ui->plot_widget->yAxis->grid()->setSubGridVisible(false);

    ui->plot_widget->replot();
  } else {
    ui->plot_widget->xAxis->grid()->setVisible(checked);
    ui->plot_widget->yAxis->grid()->setVisible(checked);
    ui->plot_widget->xAxis->grid()->setSubGridVisible(checked);
    ui->plot_widget->yAxis->grid()->setSubGridVisible(checked);

    ui->plot_widget->replot();
  }
}

void AnalyzerWindow::on_checkBox_point_clicked(bool checked) {
  if (type_ == kFrequencyType) {
    for (int i = 0; i < ui->plot_widget->graphCount(); ++i) {
      ui->plot_widget->graph(i)->setScatterStyle(QCPScatterStyle::ssNone);
    }

    ui->plot_widget->replot();
  } else {
    for (int i = 0; i < ui->plot_widget->graphCount(); ++i) {
      if (checked) {
        ui->plot_widget->graph(i)->setScatterStyle(get_shape_for_index(i));
      } else {
        ui->plot_widget->graph(i)->setScatterStyle(QCPScatterStyle::ssNone);
      }
    }

    ui->plot_widget->replot();
  }
}

void AnalyzerWindow::on_checkBox_timeline_clicked(bool checked) {
  if (checked) {
    move_timeline(current_time_);
  } else {
    if (timeline_) {
      ui->plot_widget->removeItem(timeline_);
      timeline_ = nullptr;

      ui->plot_widget->replot();
    }
  }
}

void AnalyzerWindow::on_checkBox_tracking_clicked(bool checked) { (void)checked; }

void AnalyzerWindow::on_comboBox_zoom_currentIndexChanged(int index) {
  if (type_ == kFrequencyType) {
    ui->plot_widget->axisRect()->setRangeZoom(Qt::Horizontal);

    ui->plot_widget->replot();
  } else {
    if (index == 0) {
      ui->plot_widget->axisRect()->setRangeZoom(Qt::Horizontal | Qt::Vertical);
    } else if (index == 1) {
      ui->plot_widget->axisRect()->setRangeZoom(Qt::Horizontal);
    } else if (index == 2) {
      ui->plot_widget->axisRect()->setRangeZoom(Qt::Vertical);
    }

    ui->plot_widget->replot();
  }
}

void AnalyzerWindow::on_comboBox_line_currentIndexChanged(int index) {
  if (type_ == kFrequencyType) {
    for (int i = 0; i < ui->plot_widget->graphCount(); ++i) {
      ui->plot_widget->graph(i)->setLineStyle(QCPGraph::lsNone);
    }

    ui->plot_widget->replot();
  } else {
    for (int i = 0; i < ui->plot_widget->graphCount(); ++i) {
      switch (index) {
        case 0:
          ui->plot_widget->graph(i)->setLineStyle(QCPGraph::lsLine);
          break;
        case 1:
          ui->plot_widget->graph(i)->setLineStyle(QCPGraph::lsLine);
          break;
        case 2:
          ui->plot_widget->graph(i)->setLineStyle(QCPGraph::lsImpulse);
          break;
        case 3:
          ui->plot_widget->graph(i)->setLineStyle(QCPGraph::lsStepLeft);
          break;
        case 4:
          ui->plot_widget->graph(i)->setLineStyle(QCPGraph::lsStepRight);
          break;
        case 5:
          ui->plot_widget->graph(i)->setLineStyle(QCPGraph::lsStepCenter);
          break;
        case 6:
          ui->plot_widget->graph(i)->setLineStyle(QCPGraph::lsNone);
          break;
        default:
          ui->plot_widget->graph(i)->setLineStyle(QCPGraph::lsLine);
          break;
      }

      QPen pen = ui->plot_widget->graph(i)->pen();
      if (ui->comboBox_line->currentIndex() == 1) {
        pen.setStyle(Qt::DashLine);
      } else {
        pen.setStyle(Qt::SolidLine);
      }
      ui->plot_widget->graph(i)->setPen(pen);
    }

    ui->plot_widget->replot();
  }
}

void AnalyzerWindow::on_comboBox_count_currentIndexChanged(int index) {
  if (type_ == kFrequencyType) {
    ui->plot_widget->yAxis->setNumberFormat("gb");
    ui->plot_widget->yAxis->setNumberPrecision(8);

    ui->plot_widget->replot();
  } else {
    switch (index) {
      case 0:
        ui->plot_widget->yAxis->setNumberFormat("gb");
        ui->plot_widget->yAxis->setNumberPrecision(8);
        break;
      case 1:
        ui->plot_widget->yAxis->setNumberFormat("fb");
        ui->plot_widget->yAxis->setNumberPrecision(2);
        break;
      case 2:
        ui->plot_widget->yAxis->setNumberFormat("fb");
        ui->plot_widget->yAxis->setNumberPrecision(4);
        break;
      case 3:
        ui->plot_widget->yAxis->setNumberFormat("fb");
        ui->plot_widget->yAxis->setNumberPrecision(6);
        break;
      case 4:
        ui->plot_widget->yAxis->setNumberFormat("fb");
        ui->plot_widget->yAxis->setNumberPrecision(8);
        break;
      case 5:
        ui->plot_widget->yAxis->setNumberFormat("eb");
        ui->plot_widget->yAxis->setNumberPrecision(2);
        break;
      case 6:
        ui->plot_widget->yAxis->setNumberFormat("eb");
        ui->plot_widget->yAxis->setNumberPrecision(4);
        break;
      case 7:
        ui->plot_widget->yAxis->setNumberFormat("eb");
        ui->plot_widget->yAxis->setNumberPrecision(6);
        break;
      case 8:
        ui->plot_widget->yAxis->setNumberFormat("eb");
        ui->plot_widget->yAxis->setNumberPrecision(8);
        break;
      case 9:
        ui->plot_widget->yAxis->setNumberFormat("fb");
        ui->plot_widget->yAxis->setNumberPrecision(0);
        break;
      default:
        break;
    }

    if (type_ == kCustomType) {
      ui->plot_widget->xAxis->setNumberFormat(ui->plot_widget->yAxis->numberFormat());
      ui->plot_widget->xAxis->setNumberPrecision(ui->plot_widget->yAxis->numberPrecision());

      ui->plot_widget->xAxis2->setNumberFormat(ui->plot_widget->yAxis->numberFormat());
      ui->plot_widget->xAxis2->setNumberPrecision(ui->plot_widget->yAxis->numberPrecision());
    } else {
      ui->plot_widget->xAxis->setNumberFormat("f");
      ui->plot_widget->xAxis->setNumberPrecision(3);

      ui->plot_widget->xAxis2->setNumberFormat("f");
      ui->plot_widget->xAxis2->setNumberPrecision(3);
    }

    ui->plot_widget->replot();
  }
}

void AnalyzerWindow::set_progress(double value) {
  ui->progressBar->setValue(value * 1000);
  ui->progressBar->setFormat(QString::number(value, 'f', 2) + " %");
}

bool AnalyzerWindow::load_bag(const QString& path) {
  if (!QFile::exists(path)) {
    QMessageBox::warning(this, tr("File Error"), tr("File not exists (%1).").arg(path));
    ui->statusBar->showMessage(tr("Bag load error"));

    ui->lineEdit_path->clear();

    update_status();

    return false;
  }

  file_path_ = path;

  ui->lineEdit_path->setText(file_path_);

  if (player_) {
    if (player_->is_running()) {
      player_->stop();
      player_->quit();
      player_->wait_for_quit(2000);
    }

    player_.reset();
  }

  try {
    player_ = vlink::BagReader::create(file_path_.toStdString(), true);
  } catch (vlink::Exception::RuntimeError& e) {
    QMessageBox::warning(this, tr("Warning"), QString::fromStdString(e.what()));
    ui->statusBar->showMessage(tr("Bag load error"));

    ui->lineEdit_path->clear();

    update_status();

    return false;
  }

  ui->doubleSpinBox_begin->setMaximum(player_->get_info().total_duration / 1000.0);
  ui->doubleSpinBox_end->setMaximum(player_->get_info().total_duration / 1000.0);

  ui->doubleSpinBox_begin->setMinimum(player_->get_info().blank_duration / 1000.0);
  ui->doubleSpinBox_end->setMinimum(player_->get_info().blank_duration / 1000.0);

  ui->doubleSpinBox_begin->setValue(player_->get_info().blank_duration / 1000.0);
  ui->doubleSpinBox_end->setValue(player_->get_info().total_duration / 1000.0);

  player_->register_begin_handler(
      [this]() { QMetaObject::invokeMethod(this, "update_progress", Qt::QueuedConnection); });

  player_->register_end_handler([this]() { QMetaObject::invokeMethod(this, "update_progress", Qt::QueuedConnection); });

  player_->register_output_callback([this](const vlink::Frame& frame) {
    int64_t timestamp = frame.timestamp;
    const std::string& url = frame.url;
    const vlink::ActionType action_type = frame.action_type;
    const vlink::Bytes& raw_data = frame.data;

    (void)action_type;

    timestamp /= 1000.0;

    double x_value = timestamp / 1000.0;
    double y_value = 0;

    auto& unit_list = unit_map_[url];

    for (auto& unit : unit_list) {
      if (unit.index < 0) {
        continue;
      }

      auto ser = frame.ser_type;

      if (type_ == kFrequencyType) {
        unit.x_values.append(x_value);
        unit.y_values.append(unit.index + 1);
      } else if (type_ == kValueType || type_ == kCustomType) {
        if (unit.ext_sample_interval != 0) {
          if (timestamp - unit.sample_timestamp > unit.ext_sample_interval) {
            unit.sample_timestamp = timestamp;
          } else {
            continue;
          }
        }

        std::unordered_map<std::string, std::optional<double>> pvalue_map;
        vlink::zerocopy::MessageParser zerocopy_parser;
        bool zerocopy_parse_attempted = false;
        bool zerocopy_parse_succeeded = false;
        const auto schema_type = vlink::SchemaData::resolve_type(
            unit.schema_type_override.has_value() ? *unit.schema_type_override : frame.schema_type, ser);

        for (size_t i = 0; i < unit.expressions.size(); ++i) {
          const auto& expression = unit.expressions[i];
          const auto& condition_list = unit.condition_lists[i];

          auto& pvalue = pvalue_map[expression];

          if (schema_type == vlink::SchemaType::kZeroCopy) {
            if (!zerocopy_parse_attempted) {
              zerocopy_parse_attempted = true;
              zerocopy_parse_succeeded = zerocopy_parser.parse(ser, raw_data);
            }

            if VUNLIKELY (!zerocopy_parse_succeeded) {
              continue;
            }

            double value = 0.0;
            bool precision_loss = false;

            if (zerocopy_parser.numeric(expression, value, &precision_loss)) {
              static std::atomic_bool warned_integer_precision{false};

              if VUNLIKELY (precision_loss && !warned_integer_precision.exchange(true, std::memory_order_relaxed)) {
                MLOG_W("Analyzer expression input exceeds the exact integer range of ExprTk double values (2^53)");
              }

              pvalue.emplace(value);
            }
          } else if (schema_type == vlink::SchemaType::kProtobuf) {
            if (des_pool_ == nullptr || factory_ == nullptr) {
              continue;
            }

            if (unit.cached_ser != ser) {
              unit.cached_ser = ser;
              unit.root_msg.reset();
              unit.fbs_context.reset();
            }

            if (!unit.root_msg) {
              const auto* descriptor = des_pool_->FindMessageTypeByName(ser);

              if (descriptor == nullptr) {
                continue;
              }

              const auto* prototype = factory_->GetPrototype(descriptor);

              if (prototype == nullptr) {
                continue;
              }

              unit.root_msg.reset(prototype->New());
            }

            if (unit.root_msg && unit.root_msg->ParseFromArray(raw_data.data(), static_cast<int>(raw_data.size()))) {
              int depth = 0;
              VariantType value;

              if (get_proto_value(*unit.root_msg, condition_list, depth, value)) {
                if (std::holds_alternative<int64_t>(value)) {
                  pvalue.emplace(std::get<int64_t>(value));
                } else if (std::holds_alternative<double>(value)) {
                  pvalue.emplace(std::get<double>(value));
                }
              }
            }
          } else if (schema_type == vlink::SchemaType::kFlatbuffers) {
            if (flatbuffers_runtime_.empty()) {
              continue;
            }

            if (unit.cached_ser != ser) {
              unit.cached_ser = ser;
              unit.root_msg.reset();
              unit.fbs_context.reset();
            }

            if (!unit.fbs_context) {
              unit.fbs_context = flatbuffers_runtime_.find_context(ser);
            }

            if (!unit.fbs_context || !unit.fbs_context->valid()) {
              continue;
            }

            FlatbuffersObjectView root_view;
            if (make_root_view(*unit.fbs_context, raw_data, root_view)) {
              int depth = 0;
              VariantType value;

              if (get_flatbuffers_value(root_view, *unit.fbs_context->schema, condition_list, depth, value)) {
                if (std::holds_alternative<int64_t>(value)) {
                  pvalue.emplace(std::get<int64_t>(value));
                } else if (std::holds_alternative<double>(value)) {
                  pvalue.emplace(std::get<double>(value));
                }
              }
            }
          }

          if (!unit.ext_operation_pro && !unit.ext_operation_y.empty()) {
            std::string content(unit.ext_operation_y.data() + 1, unit.ext_operation_y.size() - 1);

            try {
              double content_value = std::stod(content);

              if (unit.ext_operation_y[0] == '+') {
                pvalue.emplace(pvalue.value() + content_value);
              } else if (unit.ext_operation_y[0] == '-') {
                pvalue.emplace(pvalue.value() - content_value);
              } else if (unit.ext_operation_y[0] == '*') {
                pvalue.emplace(pvalue.value() * content_value);
              } else if (unit.ext_operation_y[0] == '/') {
                pvalue.emplace(pvalue.value() / content_value);
              } else if (unit.ext_operation_y[0] == '^') {
                pvalue.emplace(std::pow(pvalue.value(), content_value));
              }
            } catch (std::exception&) {
            }
          }
        }

        {
          vlink::Exprtk::VariableList variable_list;

          y_value = 0;
          bool y_valid = false;

          for (const auto& [expression, pvalue] : pvalue_map) {
            if (!pvalue.has_value()) {
              y_valid = false;
              break;
            }

            y_value = pvalue.value();
            y_valid = true;

            if (unit.ext_operation_pro) {
              variable_list.emplace_back(expression, pvalue.value());
            }
          }

          if (!y_valid) {
            continue;
          }

          if (unit.ext_operation_pro) {
            if (!unit.ext_operation_x.empty()) {
              auto result = vlink::Exprtk::parse(unit.ext_operation_x, variable_list);

              if (!result.has_value()) {
                continue;
              }

              x_value = result.value();
            }

            if (!unit.ext_operation_y.empty()) {
              auto result = vlink::Exprtk::parse(unit.ext_operation_y, variable_list);

              if (!result.has_value()) {
                continue;
              }

              y_value = result.value();
            }
          }

          if (unit.ext_zero_start_x) {
            if (!unit.x_start.has_value()) {
              unit.x_start.emplace(x_value);
            }

            x_value = x_value - unit.x_start.value();
          }

          if (unit.ext_zero_start_y) {
            if (!unit.y_start.has_value()) {
              unit.y_start.emplace(y_value);
            }

            y_value = y_value - unit.y_start.value();
          }

          x_value = std::min(std::max(x_value, unit.ext_limit_min_x), unit.ext_limit_max_x);
          y_value = std::min(std::max(y_value, unit.ext_limit_min_y), unit.ext_limit_max_y);

          x_min_value_ = std::min(x_min_value_, x_value);
          x_max_value_ = std::max(x_max_value_, x_value);

          y_min_value_ = std::min(y_min_value_, y_value);
          y_max_value_ = std::max(y_max_value_, y_value);

          unit.x_values.append(x_value);
          unit.y_values.append(y_value);
        }
      }
    }
  });

  player_->register_status_callback([this](vlink::BagReader::Status status) {
    status_ = status;
    QMetaObject::invokeMethod(this, "update_status", Qt::QueuedConnection);
  });

  player_->register_finish_callback([this](bool is_interrupted) {
    interrupted_ = is_interrupted;
    finished_ = true;
    QMetaObject::invokeMethod(this, "update_progress", Qt::QueuedConnection);
    QMetaObject::invokeMethod(this, "create_plot", Qt::QueuedConnection);
    QMetaObject::invokeMethod(this, "update_status", Qt::QueuedConnection);
  });

  ready_to_start_ = true;
  status_ = vlink::BagReader::kStopped;

  update_status();

  player_->async_run();

  {
    QSettings settings(QStandardPaths::writableLocation(QStandardPaths::CacheLocation) + "/settings.ini",
                       QSettings::IniFormat);
    settings.beginGroup("AnalyzerWindow");
    settings.setValue("bag_path", file_path_);
    settings.endGroup();
    settings.sync();
  }

  if (file_path_ != default_bag_path_) {
    enable_timeline_ = false;

    ui->checkBox_timeline->setEnabled(enable_timeline_);
    ui->checkBox_timeline->setChecked(enable_timeline_);
  }

  return true;
}

bool AnalyzerWindow::load_config(const QString& path) {
  title_.clear();
  type_ = kUnknownType;
  url_list_.clear();
  unit_map_.clear();
  label_x_.clear();
  label_y_.clear();
  filter_urls_.clear();

  if (!QFile::exists(path) || !QFileInfo(path).isFile()) {
    QMessageBox::warning(this, tr("File Error"), tr("File not exists (%1).").arg(path));
    ui->statusBar->showMessage(tr("Config parse error"));

    ui->label_type->setText("Type: Unknown");
    ui->label_type->setStyleSheet("QLabel { background-color: red; color: white; }");

    return false;
  }

  config_path_ = path;

  ui->lineEdit_config->setText(config_path_);

  try {
    nlohmann::ordered_json root_json;

    {
      std::ifstream file(path.toLocal8Bit());

      file >> root_json;

      file.close();
    }

    if (root_json.empty()) {
      QMessageBox::warning(this, tr("Config Error"), tr("Content is empty."));
      ui->statusBar->showMessage(tr("Config parse error"));

      title_.clear();
      type_ = kUnknownType;
      url_list_.clear();
      unit_map_.clear();
      label_x_.clear();
      label_y_.clear();
      filter_urls_.clear();

      ui->label_type->setText("Type: Unknown");
      ui->label_type->setStyleSheet("QLabel { background-color: red; color: white; }");

      return false;
    }

    std::string title = root_json["title"];

    title_ = QString::fromUtf8(title.c_str());

    std::string type = root_json["type"];

    std::transform(type.begin(), type.end(), type.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

    if (type == "freq" || type == "frequency") {
      type_ = kFrequencyType;
      ui->label_type->setText("Type: Freq");
      ui->label_type->setStyleSheet("QLabel { background-color: green; color: white; }");
    } else if (type == "value") {
      type_ = kValueType;
      ui->label_type->setText("Type: Value");
      ui->label_type->setStyleSheet("QLabel { background-color: green; color: white; }");
    } else if (type == "custom") {
      type_ = kCustomType;
      ui->label_type->setText("Type: Custom");
      ui->label_type->setStyleSheet("QLabel { background-color: green; color: white; }");
    }

    if (root_json.contains("label_x")) {
      label_x_ = root_json["label_x"];
    }

    if (root_json.contains("label_y")) {
      label_y_ = root_json["label_y"];
    }

    std::string x_label = root_json["type"];

    if (root_json.contains("units")) {
      nlohmann::ordered_json::array_t units_json = root_json["units"];

      int index = 0;
      for (const auto& json : units_json) {
        if (json.contains("enable")) {
          if (json["enable"] == false) {
            continue;
          }
        }

        PlotUnit unit;

        unit.index = index;

        if (json.contains("url_filter")) {
          unit.url_filter = json["url_filter"];

          if (player_) {
            for (const auto& meta : player_->get_info().url_metas) {
              if (meta.url_type == "Method") {
                continue;
              }

              if (meta.url.find(unit.url_filter) != std::string::npos) {
                unit.url = meta.url;
                break;
              }
            }
          }

          if (unit.url.empty()) {
            continue;
          }

          if (json.contains("label")) {
            unit.label = json["label"];
          } else {
            unit.label = unit.url_filter;
          }
        } else {
          unit.url = json["url"];

          if (json.contains("label")) {
            unit.label = json["label"];
          } else {
            unit.label = unit.url;
          }
        }

        filter_urls_.emplace(unit.url);

        if (json.contains("color")) {
          std::string color_str = json["color"];
          unit.color = QColor(QString::fromStdString(color_str));
        } else {
          unit.color = get_rich_color(static_cast<int>(units_json.size()), index);
        }

        bool invalid_schema_type = json.contains("encoding") || json.contains("schema");

        if (!json.contains("schema_type")) {
          unit.schema_type_override.reset();
        } else {
          vlink::SchemaType schema_type = vlink::SchemaType::kUnknown;
          bool treat_as_unset = false;

          if (json["schema_type"].is_string()) {
            std::string normalized = json["schema_type"].get<std::string>();
            std::transform(normalized.begin(), normalized.end(), normalized.begin(),
                           [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });

            if (normalized.empty() || normalized == "unknown") {
              treat_as_unset = true;
            } else if (normalized == "protobuf") {
              schema_type = vlink::SchemaType::kProtobuf;
            } else if (normalized == "flatbuffers" || normalized == "fbs") {
              schema_type = vlink::SchemaType::kFlatbuffers;
            } else if (normalized == "raw") {
              schema_type = vlink::SchemaType::kRaw;
            } else if (normalized == "zerocopy") {
              schema_type = vlink::SchemaType::kZeroCopy;
            } else {
              schema_type = static_cast<vlink::SchemaType>(-1);
            }
          } else if (json["schema_type"].is_number_integer()) {
            schema_type = static_cast<vlink::SchemaType>(json["schema_type"].get<int>());
          } else {
            schema_type = static_cast<vlink::SchemaType>(-1);
          }

          if (treat_as_unset) {
            unit.schema_type_override.reset();
          } else {
            invalid_schema_type = !vlink::SchemaData::is_real_type(schema_type);
            unit.schema_type_override = schema_type;
          }
        }

        if VUNLIKELY (invalid_schema_type) {
          QMessageBox::warning(
              this, tr("Config Error"),
              tr("Invalid unit schema_type. Analyzer only supports protobuf, flatbuffers, raw or zerocopy."));
          ui->statusBar->showMessage(tr("Config parse error"));

          title_.clear();
          type_ = kUnknownType;
          url_list_.clear();
          unit_map_.clear();
          label_x_.clear();
          label_y_.clear();
          filter_urls_.clear();

          ui->label_type->setText("Type: Unknown");
          ui->label_type->setStyleSheet("QLabel { background-color: red; color: white; }");

          return false;
        }

        bool has_rewrite_x = false;

        if (type_ != kFrequencyType) {
          if (json.contains("expressions")) {
            unit.expressions = json["expressions"];
            unit.condition_lists.clear();

            for (const auto& expression : unit.expressions) {
              auto ss = vlink::Helpers::split(expression, '.');

              if (ss.empty()) {
                ss.emplace_back(expression);
              }

              unit.condition_lists.emplace_back(ss);
            }

            unit.ext_operation_pro = true;
          } else {
            unit.expressions.emplace_back(json["expression"]);
            unit.condition_lists.clear();

            auto ss = vlink::Helpers::split(unit.expressions[0], '.');

            if (ss.empty()) {
              ss.emplace_back(unit.expressions[0]);
            }

            unit.condition_lists.emplace_back(ss);
          }

          if (json.contains("ext_sample_interval")) {
            unit.ext_sample_interval = json["ext_sample_interval"];
          }

          if (json.contains("ext_zero_start")) {
            unit.ext_zero_start_y = json["ext_zero_start"];
          }

          if (json.contains("ext_zero_start_x")) {
            unit.ext_zero_start_x = json["ext_zero_start_x"];
            has_rewrite_x = true;
          }

          if (json.contains("ext_zero_start_y")) {
            unit.ext_zero_start_y = json["ext_zero_start_y"];
          }

          if (json.contains("ext_limit_max") && !json["ext_limit_max"].is_null()) {
            unit.ext_limit_max_y = json["ext_limit_max"];
          }

          if (json.contains("ext_limit_max_x") && !json["ext_limit_max_x"].is_null()) {
            unit.ext_limit_max_x = json["ext_limit_max_x"];
            has_rewrite_x = true;
          }

          if (json.contains("ext_limit_max_y") && !json["ext_limit_max_y"].is_null()) {
            unit.ext_limit_max_y = json["ext_limit_max_y"];
          }

          if (json.contains("ext_limit_min") && !json["ext_limit_min"].is_null()) {
            unit.ext_limit_min_y = json["ext_limit_min"];
          }

          if (json.contains("ext_limit_min_x") && !json["ext_limit_min_x"].is_null()) {
            unit.ext_limit_min_x = json["ext_limit_min_x"];
            has_rewrite_x = true;
          }

          if (json.contains("ext_limit_min_y") && !json["ext_limit_min_y"].is_null()) {
            unit.ext_limit_min_y = json["ext_limit_min_y"];
          }

          if (json.contains("ext_operation")) {
            unit.ext_operation_y = json["ext_operation"];
          }

          if (json.contains("ext_operation_x")) {
            unit.ext_operation_x = json["ext_operation_x"];
            has_rewrite_x = true;
          }

          if (json.contains("ext_operation_y")) {
            unit.ext_operation_y = json["ext_operation_y"];
          }
        }

        if (type_ == kCustomType && (unit.ext_operation_x.empty() || unit.ext_operation_y.empty())) {
          QMessageBox::warning(this, tr("Config Error"), tr("Rewrite X-Asix operation(x && y) can not ne empty."));
          ui->statusBar->showMessage(tr("Config parse error"));

          title_.clear();
          type_ = kUnknownType;
          url_list_.clear();
          unit_map_.clear();
          label_x_.clear();
          label_y_.clear();
          filter_urls_.clear();

          ui->label_type->setText("Type: Unknown");
          ui->label_type->setStyleSheet("QLabel { background-color: red; color: white; }");

          return false;
        }

        if (has_rewrite_x != (type_ == kCustomType)) {
          QMessageBox::warning(this, tr("Config Error"), tr("Rewrite X-Asix must be custom type."));
          ui->statusBar->showMessage(tr("Config parse error"));

          title_.clear();
          type_ = kUnknownType;
          url_list_.clear();
          unit_map_.clear();
          label_x_.clear();
          label_y_.clear();
          filter_urls_.clear();

          ui->label_type->setText("Type: Unknown");
          ui->label_type->setStyleSheet("QLabel { background-color: red; color: white; }");

          return false;
        }

        if (type_ != kFrequencyType &&
            (unit.expressions.empty() || unit.expressions.size() != unit.condition_lists.size())) {
          QMessageBox::warning(this, tr("Config Error"), tr("Expressions error."));
          ui->statusBar->showMessage(tr("Config parse error"));

          title_.clear();
          type_ = kUnknownType;
          url_list_.clear();
          unit_map_.clear();
          label_x_.clear();
          label_y_.clear();
          filter_urls_.clear();

          ui->label_type->setText("Type: Unknown");
          ui->label_type->setStyleSheet("QLabel { background-color: red; color: white; }");

          return false;
        }

        if (type_ != kFrequencyType && index >= 15) {
          QMessageBox::warning(this, tr("Config Error"), tr("Units out of range(Limit 15)."));
          ui->statusBar->showMessage(tr("Config parse error"));

          title_.clear();
          type_ = kUnknownType;
          url_list_.clear();
          unit_map_.clear();
          label_x_.clear();
          label_y_.clear();
          filter_urls_.clear();

          ui->label_type->setText("Type: Unknown");
          ui->label_type->setStyleSheet("QLabel { background-color: red; color: white; }");

          return false;
        }

        auto& unit_list = unit_map_[unit.url];

        if (std::find(url_list_.begin(), url_list_.end(), unit.url) == url_list_.end()) {
          url_list_.emplace_back(unit.url);
        }

        unit_list.emplace_back(std::move(unit));

        ++index;
      }
    } else {
      if (type_ != kFrequencyType) {
        QMessageBox::warning(this, tr("Config Error"), tr("Can not foind units."));
        ui->statusBar->showMessage(tr("Config parse error"));

        title_.clear();
        type_ = kUnknownType;
        url_list_.clear();
        unit_map_.clear();
        label_x_.clear();
        label_y_.clear();
        filter_urls_.clear();

        ui->label_type->setText("Type: Unknown");
        ui->label_type->setStyleSheet("QLabel { background-color: red; color: white; }");

        return false;
      }
    }
  } catch (nlohmann::ordered_json::exception& e) {
    QMessageBox::warning(this, tr("Config Error"), e.what());
    ui->statusBar->showMessage(tr("Config parse error"));

    title_.clear();
    type_ = kUnknownType;
    url_list_.clear();
    unit_map_.clear();
    label_x_.clear();
    label_y_.clear();
    filter_urls_.clear();

    ui->label_type->setText("Type: Unknown");
    ui->label_type->setStyleSheet("QLabel { background-color: red; color: white; }");

    return false;
  }

  if (player_ && type_ == kFrequencyType && unit_map_.empty()) {
    int index = 0;

    for (const auto& meta : player_->get_info().url_metas) {
      if (meta.url_type == "Method") {
        continue;
      }

      PlotUnit punit;
      punit.index = index;
      punit.label = meta.url;
      punit.url = meta.url;
      punit.color = get_rich_color(static_cast<int>(player_->get_info().url_metas.size()), index);

      auto& unit_list = unit_map_[punit.url];

      if (std::find(url_list_.begin(), url_list_.end(), punit.url) == url_list_.end()) {
        url_list_.emplace_back(punit.url);
      }

      unit_list.emplace_back(std::move(punit));

      ++index;
    }
  }

  ui->plot_widget->xAxis->setLabel(QString::fromUtf8(label_x_.c_str()));
  ui->plot_widget->yAxis->setLabel(QString::fromUtf8(label_y_.c_str()));

  return true;
}

bool AnalyzerWindow::load_proto(const QString& proto) {
  if (proto.trimmed().isEmpty()) {
    factory_.reset();
    source_tree_.reset();
    importer_.reset();
    des_pool_ = nullptr;
    proto_dir_.clear();
    return true;
  }

  factory_ = std::make_shared<google::protobuf::DynamicMessageFactory>();
  source_tree_ = std::make_shared<google::protobuf::compiler::DiskSourceTree>();
  importer_ = std::make_shared<google::protobuf::compiler::Importer>(source_tree_.get(), nullptr);

  proto_dir_ = proto;

  std::string proto_dir = proto.toStdString();

  bool has_import = false;

  try {
#ifdef _WIN32
    auto proto_path = std::filesystem::path(vlink::Helpers::string_to_wstring(proto_dir));
    source_tree_->MapPath("", vlink::Helpers::path_to_string(proto_path));
#else
    auto proto_path = std::filesystem::path(proto_dir);
    source_tree_->MapPath("", proto_path.string());
#endif

    import_protos(importer_.get(), proto_path, proto_path, has_import);
  } catch (std::filesystem::filesystem_error& e) {
    QMessageBox::critical(this, tr("File Error"), e.what());

    return false;
  }

  if (!has_import) {
    QMessageBox::warning(this, tr("Proto Error"), tr("Load proto fialed."));
    ui->statusBar->showMessage(tr("Proto parse error"));

    return false;
  }

  des_pool_ = const_cast<google::protobuf::DescriptorPool*>(importer_->pool());

  if (!des_pool_) {
    QMessageBox::warning(this, tr("Proto Error"), tr("Load proto pool fialed."));
    ui->statusBar->showMessage(tr("Load proto pool error"));

    return false;
  }

  return true;
}

bool AnalyzerWindow::load_fbs(const QString& fbs_dir) {
  if (fbs_dir.trimmed().isEmpty()) {
    flatbuffers_runtime_.clear();
    fbs_dir_.clear();
    return true;
  }

  std::string error;

  if (!flatbuffers_runtime_.load_dir(fbs_dir.toStdString(), &error)) {
    QMessageBox::warning(this, tr("FlatBuffers Error"),
                         error.empty() ? tr("Load flatbuffers directory failed.") : QString::fromStdString(error));
    ui->statusBar->showMessage(tr("Load flatbuffers directory error"));
    return false;
  }

  fbs_dir_ = fbs_dir;
  return true;
}

void AnalyzerWindow::update_progress() {
  if (!player_) {
    set_progress(0);
    return;
  }

  if (interrupted_) {
    return;
  }

  if (finished_) {
    set_progress(100);
    return;
  }

  int64_t start_time = 0;
  int64_t end_time = 0;

  if (ui->groupBox_time->isChecked()) {
    start_time = player_->get_real_timestamp() - ui->doubleSpinBox_begin->value() * 1000;
    end_time = ui->doubleSpinBox_end->value() * 1000 - ui->doubleSpinBox_begin->value() * 1000;
  } else {
    start_time = player_->get_real_timestamp() - player_->get_info().blank_duration;
    end_time = player_->get_info().total_duration - player_->get_info().blank_duration;
  }

  if (start_time < 0) {
    start_time = 0;
  }

  if (end_time <= 0) {
    set_progress(0);
    return;
  }

  set_progress(100.0 * start_time / end_time);
}

void AnalyzerWindow::update_status() {
  if (ui->lineEdit_config->text().isEmpty() ||
      (ui->lineEdit_proto->text().isEmpty() && ui->lineEdit_fbs->text().isEmpty())) {
    ui->label_path->setEnabled(true);
    ui->label_config->setEnabled(true);
    ui->label_proto->setEnabled(true);
    ui->label_fbs->setEnabled(true);
    ui->lineEdit_path->setEnabled(true);
    ui->lineEdit_config->setEnabled(true);
    ui->lineEdit_proto->setEnabled(true);
    ui->lineEdit_fbs->setEnabled(true);
    ui->toolButton_path->setEnabled(true);
    ui->toolButton_config->setEnabled(true);
    ui->toolButton_proto->setEnabled(true);
    ui->toolButton_fbs->setEnabled(true);

    ui->groupBox_time->setEnabled(false);

    ui->groupBox_progress->setEnabled(false);
    ui->progressBar->setEnabled(false);
    ui->pushButton_gen->setEnabled(false);
    ui->pushButton_interrupt->setEnabled(false);

    ui->groupBox_view->setEnabled(false);

    ui->pushButton_export->setEnabled(false);

    ui->statusBar->showMessage(tr(""));

    set_progress(0);

    return;
  }

  if (status_ == vlink::BagReader::kPlaying || status_ == vlink::BagReader::kPaused) {
    ui->label_path->setEnabled(false);
    ui->label_config->setEnabled(false);
    ui->label_proto->setEnabled(false);
    ui->label_fbs->setEnabled(false);
    ui->lineEdit_path->setEnabled(false);
    ui->lineEdit_config->setEnabled(false);
    ui->lineEdit_proto->setEnabled(false);
    ui->lineEdit_fbs->setEnabled(false);
    ui->toolButton_path->setEnabled(false);
    ui->toolButton_config->setEnabled(false);
    ui->toolButton_proto->setEnabled(false);
    ui->toolButton_fbs->setEnabled(false);

    ui->groupBox_time->setEnabled(false);

    ui->groupBox_progress->setEnabled(true);
    ui->progressBar->setEnabled(true);
    ui->pushButton_gen->setEnabled(false);
    ui->pushButton_interrupt->setEnabled(true);

    ui->groupBox_view->setEnabled(false);

    ui->pushButton_export->setEnabled(false);

    ui->statusBar->showMessage(tr("In progressing..."));
  } else if (status_ == vlink::BagReader::kStopped) {
    ui->label_path->setEnabled(true);
    ui->label_config->setEnabled(true);
    ui->label_proto->setEnabled(true);
    ui->label_fbs->setEnabled(true);
    ui->lineEdit_path->setEnabled(true);
    ui->lineEdit_config->setEnabled(true);
    ui->lineEdit_proto->setEnabled(true);
    ui->lineEdit_fbs->setEnabled(true);
    ui->toolButton_path->setEnabled(true);
    ui->toolButton_config->setEnabled(true);
    ui->toolButton_proto->setEnabled(true);
    ui->toolButton_fbs->setEnabled(true);

    ui->groupBox_time->setEnabled(true);

    ui->groupBox_progress->setEnabled(true);
    ui->progressBar->setEnabled(true);
    ui->pushButton_gen->setEnabled(true);
    ui->pushButton_interrupt->setEnabled(false);

    if (finished_) {
      ui->groupBox_view->setEnabled(true);
    } else {
      ui->groupBox_view->setEnabled(false);
    }

    ui->pushButton_export->setEnabled(true);

    if (ready_to_start_) {
      ui->statusBar->showMessage(tr("Ready!"));
    } else {
      if (points_is_empty_) {
        if (interrupted_) {
          ui->statusBar->showMessage(tr("Interrupted (Empty datas)!"));
        } else if (finished_) {
          ui->statusBar->showMessage(tr("Finished (Empty datas)!"));
        }
      } else {
        if (interrupted_) {
          ui->statusBar->showMessage(tr("Interrupted!"));
        } else if (finished_) {
          ui->statusBar->showMessage(tr("Finished!"));
        }
      }
    }
  } else {
    ui->label_path->setEnabled(true);
    ui->label_config->setEnabled(true);
    ui->label_proto->setEnabled(true);
    ui->label_fbs->setEnabled(true);
    ui->lineEdit_path->setEnabled(true);
    ui->lineEdit_config->setEnabled(true);
    ui->lineEdit_proto->setEnabled(true);
    ui->lineEdit_fbs->setEnabled(true);
    ui->toolButton_path->setEnabled(true);
    ui->toolButton_config->setEnabled(true);
    ui->toolButton_proto->setEnabled(true);
    ui->toolButton_fbs->setEnabled(true);

    ui->groupBox_time->setEnabled(false);

    ui->groupBox_progress->setEnabled(false);
    ui->progressBar->setEnabled(false);
    ui->pushButton_gen->setEnabled(false);
    ui->pushButton_interrupt->setEnabled(false);

    ui->groupBox_view->setEnabled(false);

    ui->pushButton_export->setEnabled(false);

    ui->statusBar->showMessage(tr(""));

    set_progress(0);
  }
}

void AnalyzerWindow::create_plot() {
  points_is_empty_ = true;

  clear_plot();

  if (type_ == kFrequencyType) {
    QSharedPointer<QCPAxisTickerText> text_ticker(new QCPAxisTickerText);

    QVector<double> ticks;
    QVector<QString> labels;

    for (const auto& url : url_list_) {
      auto& unit_list = unit_map_[url];

      for (auto& unit : unit_list) {
        if (unit.index < 0) {
          continue;
        }

        ticks.append(unit.index + 1);
        labels.append(QString::fromUtf8(unit.label.c_str()));

        QCPGraph* graph = ui->plot_widget->addGraph();
        unit.graph = graph;
        graph->setName(QString::fromUtf8(unit.label.c_str()));
        graph->setLineStyle(QCPGraph::lsNone);
        graph->setScatterStyle(QCPScatterStyle::ssPlus);
        QPen pen(unit.color, 1);
        graph->setPen(pen);
        graph->setData(unit.x_values, unit.y_values);

        if (!unit.x_values.empty() && !unit.y_values.empty()) {
          points_is_empty_ = false;
        }
      }
    }

    ui->plot_widget->xAxis->grid()->setVisible(ui->checkBox_grid->isChecked());
    ui->plot_widget->yAxis->grid()->setVisible(false);
    ui->plot_widget->xAxis->grid()->setSubGridVisible(ui->checkBox_grid->isChecked());
    ui->plot_widget->yAxis->grid()->setSubGridVisible(false);

    ui->plot_widget->yAxis->setNumberFormat("fb");
    ui->plot_widget->yAxis->setNumberPrecision(2);

    ui->plot_widget->xAxis->setNumberFormat("f");
    ui->plot_widget->xAxis->setNumberPrecision(3);

    ui->plot_widget->xAxis2->setNumberFormat("f");
    ui->plot_widget->xAxis2->setNumberPrecision(3);

    text_ticker->addTicks(ticks, labels);

    ui->plot_widget->xAxis->setTicker(default_ticker_);
    ui->plot_widget->xAxis2->setTicker(default_ticker_);
    ui->plot_widget->yAxis->setTicker(text_ticker);

    ui->plot_widget->axisRect()->setRangeZoom(Qt::Horizontal);

    if (player_) {
      title_element_->setText(title_ + "   (" + QString::fromUtf8(player_->get_info().file_name.c_str()) + ")");
    } else {
      title_element_->setText(title_);
    }

    ui->plot_widget->legend->setVisible(false);

    if (player_) {
      adjust_x_range(ui->horizontalSlider_time->value() * 60.0);

      x_limit_range_.lower = 0;
      x_limit_range_.upper = player_->get_info().total_duration / 1000.0;
      --x_limit_range_.lower;
      ++x_limit_range_.upper;

      y_range_.lower = 0.0;
      y_range_.upper = std::min(static_cast<double>(url_list_.size() + 1.0), 30.0);
      y_limit_range_.lower = -y_range_.upper + 1.0 + 0.5;
      y_limit_range_.upper = url_list_.size() + y_range_.upper - 0.5;
    }

    ui->checkBox_legend->setChecked(false);
    ui->checkBox_legend->setEnabled(false);
    ui->checkBox_point->setChecked(false);
    ui->checkBox_point->setEnabled(false);
    ui->label_zoom->setEnabled(false);
    ui->comboBox_zoom->setCurrentIndex(0);
    ui->comboBox_zoom->setEnabled(false);
    ui->label_line->setEnabled(false);
    ui->comboBox_line->setCurrentIndex(0);
    ui->comboBox_line->setEnabled(false);
    ui->label_count->setEnabled(false);
    ui->comboBox_count->setCurrentIndex(0);
    ui->comboBox_count->setEnabled(false);

    ui->checkBox_timeline->setEnabled(enable_timeline_);
    ui->checkBox_time->setEnabled(true);
    ui->horizontalSlider_time->setEnabled(ui->checkBox_time->isChecked());
  } else if (type_ == kValueType) {
    ui->plot_widget->xAxis->setTicker(time_ticker_);
    ui->plot_widget->xAxis2->setTicker(time_ticker_);
    ui->plot_widget->yAxis->setTicker(default_ticker_);

    for (auto& url : url_list_) {
      auto& unit_list = unit_map_[url];

      for (auto& unit : unit_list) {
        if (unit.index < 0) {
          continue;
        }

        QCPGraph* graph = ui->plot_widget->addGraph();
        unit.graph = graph;
        graph->setName(QString::fromUtf8(unit.label.c_str()));

        switch (ui->comboBox_line->currentIndex()) {
          case 0:
            graph->setLineStyle(QCPGraph::lsLine);
            break;
          case 1:
            graph->setLineStyle(QCPGraph::lsLine);
            break;
          case 2:
            graph->setLineStyle(QCPGraph::lsImpulse);
            break;
          case 3:
            graph->setLineStyle(QCPGraph::lsStepLeft);
            break;
          case 4:
            graph->setLineStyle(QCPGraph::lsStepRight);
            break;
          case 5:
            graph->setLineStyle(QCPGraph::lsStepCenter);
            break;
          case 6:
            graph->setLineStyle(QCPGraph::lsNone);
            break;
          default:
            graph->setLineStyle(QCPGraph::lsLine);
            break;
        }

        if (ui->checkBox_point->isChecked()) {
          graph->setScatterStyle(get_shape_for_index(unit.index));
        } else {
          graph->setScatterStyle(QCPScatterStyle::ssNone);
        }

        QPen pen(unit.color, 1);
        if (ui->comboBox_line->currentIndex() == 1) {
          pen.setStyle(Qt::DashLine);
        } else {
          pen.setStyle(Qt::SolidLine);
        }
        graph->setPen(pen);

        graph->setData(unit.x_values, unit.y_values);

        if (!unit.x_values.empty() && !unit.y_values.empty()) {
          points_is_empty_ = false;
        }
      }
    }

    ui->plot_widget->xAxis->grid()->setVisible(ui->checkBox_grid->isChecked());
    ui->plot_widget->yAxis->grid()->setVisible(ui->checkBox_grid->isChecked());
    ui->plot_widget->xAxis->grid()->setSubGridVisible(ui->checkBox_grid->isChecked());
    ui->plot_widget->yAxis->grid()->setSubGridVisible(ui->checkBox_grid->isChecked());

    switch (ui->comboBox_count->currentIndex()) {
      case 0:
        ui->plot_widget->yAxis->setNumberFormat("gb");
        ui->plot_widget->yAxis->setNumberPrecision(8);
        break;
      case 1:
        ui->plot_widget->yAxis->setNumberFormat("fb");
        ui->plot_widget->yAxis->setNumberPrecision(2);
        break;
      case 2:
        ui->plot_widget->yAxis->setNumberFormat("fb");
        ui->plot_widget->yAxis->setNumberPrecision(4);
        break;
      case 3:
        ui->plot_widget->yAxis->setNumberFormat("fb");
        ui->plot_widget->yAxis->setNumberPrecision(6);
        break;
      case 4:
        ui->plot_widget->yAxis->setNumberFormat("fb");
        ui->plot_widget->yAxis->setNumberPrecision(8);
        break;
      case 5:
        ui->plot_widget->yAxis->setNumberFormat("eb");
        ui->plot_widget->yAxis->setNumberPrecision(2);
        break;
      case 6:
        ui->plot_widget->yAxis->setNumberFormat("eb");
        ui->plot_widget->yAxis->setNumberPrecision(4);
        break;
      case 7:
        ui->plot_widget->yAxis->setNumberFormat("eb");
        ui->plot_widget->yAxis->setNumberPrecision(6);
        break;
      case 8:
        ui->plot_widget->yAxis->setNumberFormat("eb");
        ui->plot_widget->yAxis->setNumberPrecision(8);
        break;
      case 9:
        ui->plot_widget->yAxis->setNumberFormat("fb");
        ui->plot_widget->yAxis->setNumberPrecision(0);
        break;
      default:
        break;
    }

    ui->plot_widget->xAxis->setNumberFormat("f");
    ui->plot_widget->xAxis->setNumberPrecision(3);

    ui->plot_widget->xAxis2->setNumberFormat("f");
    ui->plot_widget->xAxis2->setNumberPrecision(3);

    ui->plot_widget->axisRect()->setRangeZoom(Qt::Horizontal | Qt::Vertical);

    if (player_) {
      title_element_->setText(title_ + "   (" + QString::fromUtf8(player_->get_info().file_name.c_str()) + ")");
    } else {
      title_element_->setText(title_);
    }

    ui->plot_widget->legend->setVisible(true);

    if (player_) {
      adjust_x_range(ui->horizontalSlider_time->value() * 60.0);

      x_limit_range_.lower = 0;
      x_limit_range_.upper = player_->get_info().total_duration / 1000.0;
      --x_limit_range_.lower;
      ++x_limit_range_.upper;

      if (y_min_value_ == QCPRange::maxRange || y_max_value_ == -QCPRange::maxRange) {
        y_range_ = {-1.0, 61.0};
      } else {
        double p = std::abs(y_max_value_ - y_min_value_) * 0.05;
        y_range_ = {y_min_value_ - p, y_max_value_ + p};
      }

      y_limit_range_ = y_range_;
    }

    ui->checkBox_legend->setChecked(true);
    ui->checkBox_legend->setEnabled(true);
    ui->checkBox_point->setEnabled(true);
    ui->label_zoom->setEnabled(true);
    ui->comboBox_zoom->setEnabled(true);
    ui->label_line->setEnabled(true);
    ui->comboBox_line->setEnabled(true);
    ui->label_count->setEnabled(true);
    ui->comboBox_count->setEnabled(true);

    ui->checkBox_timeline->setEnabled(enable_timeline_);
    ui->checkBox_time->setEnabled(true);
    ui->horizontalSlider_time->setEnabled(ui->checkBox_time->isChecked());
  } else if (type_ == kCustomType) {
    ui->plot_widget->xAxis->setTicker(default_ticker_);
    ui->plot_widget->xAxis2->setTicker(default_ticker_);
    ui->plot_widget->yAxis->setTicker(default_ticker_);

    for (auto& url : url_list_) {
      auto& unit_list = unit_map_[url];

      for (auto& unit : unit_list) {
        if (unit.index < 0) {
          continue;
        }

        QCPGraph* graph = ui->plot_widget->addGraph();
        unit.graph = graph;
        graph->setName(QString::fromUtf8(unit.label.c_str()));

        switch (ui->comboBox_line->currentIndex()) {
          case 0:
            graph->setLineStyle(QCPGraph::lsLine);
            break;
          case 1:
            graph->setLineStyle(QCPGraph::lsLine);
            break;
          case 2:
            graph->setLineStyle(QCPGraph::lsImpulse);
            break;
          case 3:
            graph->setLineStyle(QCPGraph::lsStepLeft);
            break;
          case 4:
            graph->setLineStyle(QCPGraph::lsStepRight);
            break;
          case 5:
            graph->setLineStyle(QCPGraph::lsStepCenter);
            break;
          case 6:
            graph->setLineStyle(QCPGraph::lsNone);
            break;
          default:
            graph->setLineStyle(QCPGraph::lsLine);
            break;
        }

        if (ui->checkBox_point->isChecked()) {
          graph->setScatterStyle(get_shape_for_index(unit.index));
        } else {
          graph->setScatterStyle(QCPScatterStyle::ssNone);
        }

        QPen pen(unit.color, 1);
        if (ui->comboBox_line->currentIndex() == 1) {
          pen.setStyle(Qt::DashLine);
        } else {
          pen.setStyle(Qt::SolidLine);
        }
        graph->setPen(pen);

        graph->setData(unit.x_values, unit.y_values);

        if (!unit.x_values.empty() && !unit.y_values.empty()) {
          points_is_empty_ = false;
        }
      }
    }

    ui->plot_widget->xAxis->grid()->setVisible(ui->checkBox_grid->isChecked());
    ui->plot_widget->yAxis->grid()->setVisible(ui->checkBox_grid->isChecked());
    ui->plot_widget->xAxis->grid()->setSubGridVisible(ui->checkBox_grid->isChecked());
    ui->plot_widget->yAxis->grid()->setSubGridVisible(ui->checkBox_grid->isChecked());

    switch (ui->comboBox_count->currentIndex()) {
      case 0:
        ui->plot_widget->yAxis->setNumberFormat("gb");
        ui->plot_widget->yAxis->setNumberPrecision(8);
        break;
      case 1:
        ui->plot_widget->yAxis->setNumberFormat("fb");
        ui->plot_widget->yAxis->setNumberPrecision(2);
        break;
      case 2:
        ui->plot_widget->yAxis->setNumberFormat("fb");
        ui->plot_widget->yAxis->setNumberPrecision(4);
        break;
      case 3:
        ui->plot_widget->yAxis->setNumberFormat("fb");
        ui->plot_widget->yAxis->setNumberPrecision(6);
        break;
      case 4:
        ui->plot_widget->yAxis->setNumberFormat("fb");
        ui->plot_widget->yAxis->setNumberPrecision(8);
        break;
      case 5:
        ui->plot_widget->yAxis->setNumberFormat("eb");
        ui->plot_widget->yAxis->setNumberPrecision(2);
        break;
      case 6:
        ui->plot_widget->yAxis->setNumberFormat("eb");
        ui->plot_widget->yAxis->setNumberPrecision(4);
        break;
      case 7:
        ui->plot_widget->yAxis->setNumberFormat("eb");
        ui->plot_widget->yAxis->setNumberPrecision(6);
        break;
      case 8:
        ui->plot_widget->yAxis->setNumberFormat("eb");
        ui->plot_widget->yAxis->setNumberPrecision(8);
        break;
      case 9:
        ui->plot_widget->yAxis->setNumberFormat("fb");
        ui->plot_widget->yAxis->setNumberPrecision(0);
        break;
      default:
        break;
    }

    ui->plot_widget->xAxis->setNumberFormat(ui->plot_widget->yAxis->numberFormat());
    ui->plot_widget->xAxis->setNumberPrecision(ui->plot_widget->yAxis->numberPrecision());

    ui->plot_widget->xAxis2->setNumberFormat(ui->plot_widget->yAxis->numberFormat());
    ui->plot_widget->xAxis2->setNumberPrecision(ui->plot_widget->yAxis->numberPrecision());

    ui->plot_widget->axisRect()->setRangeZoom(Qt::Horizontal | Qt::Vertical);

    if (player_) {
      title_element_->setText(title_ + "   (" + QString::fromUtf8(player_->get_info().file_name.c_str()) + ")");
    } else {
      title_element_->setText(title_);
    }

    ui->plot_widget->legend->setVisible(true);

    if (player_) {
      if (x_min_value_ == QCPRange::maxRange || x_max_value_ == -QCPRange::maxRange) {
        x_range_ = {-1.0, 61.0};
      } else {
        double p = std::abs(x_max_value_ - x_min_value_) * 0.05;
        x_range_ = {x_min_value_ - p, x_max_value_ + p};
      }

      x_limit_range_ = x_range_;

      if (y_min_value_ == QCPRange::maxRange || y_max_value_ == -QCPRange::maxRange) {
        y_range_ = {-1.0, 61.0};
      } else {
        double p = std::abs(y_max_value_ - y_min_value_) * 0.05;
        y_range_ = {y_min_value_ - p, y_max_value_ + p};
      }

      y_limit_range_ = y_range_;
    }

    ui->checkBox_legend->setChecked(true);
    ui->checkBox_legend->setEnabled(true);
    ui->checkBox_point->setEnabled(true);
    ui->label_zoom->setEnabled(true);
    ui->comboBox_zoom->setEnabled(true);
    ui->label_line->setEnabled(true);
    ui->comboBox_line->setEnabled(true);
    ui->label_count->setEnabled(true);
    ui->comboBox_count->setEnabled(true);

    ui->checkBox_timeline->setEnabled(false);
    ui->checkBox_time->setEnabled(false);
    ui->horizontalSlider_time->setEnabled(false);
  }

  if (player_) {
    double max_min = player_->get_info().total_duration / 1000.0 / 60.0;
    max_min += 0.9999;

    ui->horizontalSlider_time->setMaximum(std::min(max_min, 60.0));

    if (ui->checkBox_time->isChecked()) {
      ui->checkBox_time->setText(tr("Limit(%1s):").arg(QString::number(ui->horizontalSlider_time->value() * 60)));
    } else {
      ui->checkBox_time->setText(tr("Limit(~s):"));
    }
  }

  reset_plot();
}

void AnalyzerWindow::clear_plot() {
  ui->plot_widget->clearGraphs();
  ui->plot_widget->clearItems();
  ui->plot_widget->clearPlottables();

  text_items_.clear();

  timeline_ = nullptr;

  title_element_->setText("Empty");

  ui->plot_widget->setInteraction(QCP::iRangeDrag, true);
  ui->plot_widget->setInteraction(QCP::iRangeZoom, true);

  ui->plot_widget->axisRect()->setRangeZoom(Qt::Horizontal | Qt::Vertical);
  ui->plot_widget->axisRect()->setRangeDrag(Qt::Horizontal | Qt::Vertical);

  x_range_ = {-1.0, 61.0};
  y_range_ = {-1.0, 61.0};

  y_limit_range_ = {-1.0, 61.0};
  y_limit_range_ = {-1.0, 61.0};
}

void AnalyzerWindow::reset_plot() {
  ui->plot_widget->xAxis->setRange(x_range_);
  ui->plot_widget->xAxis2->setRange(x_range_);
  ui->plot_widget->yAxis->setRange(y_range_);

  ui->plot_widget->replot();
}

void AnalyzerWindow::adjust_x_range(double value) {
  if (!player_) {
    return;
  }

  if (value <= 0) {
    x_range_ = x_limit_range_;
  } else {
    if (ui->groupBox_time->isChecked()) {
      x_range_.lower =
          std::max(static_cast<double>(player_->get_info().blank_duration / 1000.0), ui->doubleSpinBox_begin->value());
      x_range_.upper =
          x_range_.lower + std::min((std::min(static_cast<double>(player_->get_info().total_duration / 1000.0),
                                              ui->doubleSpinBox_end->value()) -
                                     player_->get_info().blank_duration / 1000.0),
                                    value);
    } else {
      x_range_.lower = player_->get_info().blank_duration / 1000.0;
      x_range_.upper =
          x_range_.lower +
          std::min((player_->get_info().total_duration - player_->get_info().blank_duration) / 1000.0, value);
    }

    --x_range_.lower;
    ++x_range_.upper;
  }
}

void AnalyzerWindow::move_timeline(double time) {
  if (type_ == kCustomType) {
    return;
  }

  if (!ui->groupBox_view->isEnabled()) {
    return;
  }

  if (!timeline_) {
    timeline_ = new QCPItemLine(ui->plot_widget);

    QPen pen(Qt::red);
    pen.setWidth(2);
    timeline_->setPen(pen);

    QCPLineEnding arrow(QCPLineEnding::esDiamond, 10, 20);
    timeline_->setTail(arrow);
    timeline_->setHead(arrow);
  }

  timeline_->start->setCoords(time, ui->plot_widget->yAxis->range().lower);
  timeline_->end->setCoords(time, ui->plot_widget->yAxis->range().upper);

  if (ui->checkBox_tracking->isChecked()) {
    QCPRange current_range = ui->plot_widget->xAxis->range();

    if (!current_range.contains(time)) {
      double width = current_range.size();
      current_range.lower = time - width / 2.0;
      current_range.upper = time + width / 2.0;

      ui->plot_widget->xAxis->setRange(current_range);
      ui->plot_widget->xAxis2->setRange(current_range);
    }
  }

  ui->plot_widget->replot();

  // reset_plot();
}

// NOLINTEND
