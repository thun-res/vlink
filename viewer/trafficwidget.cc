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

#include "./trafficwidget.h"

#include <vlink/base/logger.h>

TrafficWidget::TrafficWidget(QWidget* parent) : QWidget{parent} {}

TrafficWidget::~TrafficWidget() {}

void TrafficWidget::set_value_range(double min, double max) {
  min_value_ = min;
  max_value_ = max;
}

void TrafficWidget::set_max_count(int64_t count) { max_count_ = count; }

void TrafficWidget::set_total_max_count(int64_t count) { total_max_count_ = count; }

void TrafficWidget::set_unit(double unit) {
  if (unit <= 0) {
    unit = 1;
  }

  unit_ = unit;
}

void TrafficWidget::set_suffix(int suffix) { suffix_ = suffix; }

void TrafficWidget::set_color(const QColor& color) { color_ = color; }

void TrafficWidget::set_unit_text(const QString& text) { unit_text_ = text; }

void TrafficWidget::set_count_text(const QString& text) { count_text_ = text; }

void TrafficWidget::set_header_size(QSizeF size) { header_size_ = size; }

void TrafficWidget::add_value(double value) {
  while (value_list_.size() > total_max_count_) {
    value_list_.removeFirst();
  }

  value_list_.append(value);
}

double TrafficWidget::get_real_max_value() {
  if (value_list_.isEmpty()) {
    return 0.0;
  }

  auto offset = value_list_.size() - max_count_;

  if (offset > 0) {
    return *std::max_element(value_list_.begin() + offset, value_list_.end());
  } else {
    return *std::max_element(value_list_.begin(), value_list_.end());
  }
}

double TrafficWidget::get_real_min_value() {
  if (value_list_.isEmpty()) {
    return 0.0;
  }

  auto offset = value_list_.size() - max_count_;

  if (offset > 0) {
    return *std::min_element(value_list_.begin() + offset, value_list_.end());
  } else {
    return *std::min_element(value_list_.begin(), value_list_.end());
  }
}

void TrafficWidget::set_block(bool block) { is_blocked_ = block; }

void TrafficWidget::clear() {
  value_list_.clear();

  this->update();
}

void TrafficWidget::resizeEvent(QResizeEvent* event) {
  QWidget::resizeEvent(event);
  this->update();
}

void TrafficWidget::paintEvent(QPaintEvent* event) {
  (void)event;

  if (is_blocked_) {
    return;
  }

  QPainter painter(this);

  painter.setRenderHint(QPainter::Antialiasing);

  QFont font = painter.font();
  font.setPixelSize(12);
  painter.setFont(font);

  double frame_width = rect().width();
  double frame_height = rect().height();

  QRadialGradient gradient(rect().center(), frame_width / 2);

  if (!isEnabled()) {
    gradient.setColorAt(0, QColor(200, 200, 200));
    gradient.setColorAt(1, QColor(180, 180, 180));
    painter.fillRect(rect(), gradient);
    return;
  }

  gradient.setColorAt(0, QColor(250, 250, 250));
  gradient.setColorAt(1, QColor(220, 220, 220));
  painter.fillRect(rect(), gradient);

  {
    QPen pen(QColor(100, 100, 100, 255), 1, Qt::DashLine);
    pen.setDashPattern({5, 5});
    painter.setPen(pen);
  }

  double p_width = static_cast<double>(frame_width - header_size_.width()) / max_count_;
  double p_height = static_cast<double>(frame_height - header_size_.height() - 3) / (max_value_ - min_value_);

  QPointF last_p;
  QPointF next_p;
  double x_offset = .0f;

  for (int i = 1; i < 4; ++i) {
    painter.drawLine(header_size_.width(), i * (frame_height - header_size_.height()) / 4, frame_width,
                     i * (frame_height - header_size_.height()) / 4);
  }

  for (int i = 1; i < 6; ++i) {
    painter.drawLine(header_size_.width() + i * (frame_width - header_size_.width()) / 6, 0,
                     header_size_.width() + i * (frame_width - header_size_.width()) / 6,
                     frame_height - header_size_.height());
  }

  double pen_witdh = 2.0;

  painter.setPen(QPen(color_, pen_witdh));

  int i = 1;
  int pp = 1;

  if (value_list_.size() > max_count_) {
    i = value_list_.size() - max_count_;
  }

  for (; i < value_list_.size(); ++i) {
    if (value_list_[(i - 1)] <= min_value_) {
      last_p = QPointF((pp - 1) * p_width + header_size_.width(),
                       frame_height - (value_list_[(i - 1)] - min_value_) * p_height - header_size_.height());
    } else {
      last_p = QPointF((pp - 1) * p_width + header_size_.width(),
                       frame_height - (value_list_[(i - 1)] - min_value_) * p_height - header_size_.height() - 2);
    }

    if (i < value_list_.size() - 1) {
      x_offset = pen_witdh / 3 * 2;
    } else {
      x_offset = .0f;
    }

    if (value_list_[i] <= min_value_) {
      next_p = QPointF(pp * p_width + header_size_.width() - x_offset,
                       frame_height - (value_list_[i] - min_value_) * p_height - header_size_.height());
    } else {
      next_p = QPointF(pp * p_width + header_size_.width() - x_offset,
                       frame_height - (value_list_[i] - min_value_) * p_height - header_size_.height() - 2);
    }

    painter.drawLine(last_p, next_p);

    ++pp;
  }

  painter.setPen(QPen(QColor(50, 50, 50, 255), pen_witdh));

  painter.drawLine(header_size_.width(), 0, header_size_.width(), frame_height);
  painter.drawLine(0, frame_height - header_size_.height(), frame_width, frame_height - header_size_.height());

  double max_show_value = max_value_ / unit_;

  double min_show_value = min_value_ / unit_;

  if (suffix_ > 0) {
    if (max_show_value >= 10000 || min_show_value <= -10000) {
      painter.drawText(QRectF(0, frame_height - header_size_.height() - 14 - 1, header_size_.width() - 3, 14),
                       Qt::AlignRight | Qt::AlignVCenter, QString::number(min_show_value / 1000, 'f', suffix_) + "K");
      painter.drawText(QRectF(0, (frame_height - header_size_.height() - 14) / 2, header_size_.width() - 3, 14),
                       Qt::AlignRight | Qt::AlignVCenter,
                       QString::number((max_show_value + min_show_value) / 2 / 1000, 'f', suffix_) + "K");
      painter.drawText(QRectF(0, 1, header_size_.width() - 3, 14), Qt::AlignRight | Qt::AlignVCenter,
                       QString::number(max_show_value / 1000, 'g', suffix_) + "K");
    } else {
      painter.drawText(QRectF(0, frame_height - header_size_.height() - 14 - 1, header_size_.width() - 3, 14),
                       Qt::AlignRight | Qt::AlignVCenter, QString::number(min_show_value, 'f', suffix_));
      painter.drawText(QRectF(0, (frame_height - header_size_.height() - 14) / 2, header_size_.width() - 3, 14),
                       Qt::AlignRight | Qt::AlignVCenter,
                       QString::number((max_show_value + min_show_value) / 2, 'f', suffix_));
      painter.drawText(QRectF(0, 1, header_size_.width() - 3, 14), Qt::AlignRight | Qt::AlignVCenter,
                       QString::number(max_show_value, 'f', suffix_));
    }

  } else {
    if (max_show_value >= 10000 || min_show_value <= -10000) {
      painter.drawText(QRectF(0, frame_height - header_size_.height() - 14 - 1, header_size_.width() - 3, 14),
                       Qt::AlignRight | Qt::AlignVCenter,
                       QString::number(static_cast<int64_t>(min_show_value / 1000)) + "K");
      painter.drawText(QRectF(0, (frame_height - header_size_.height() - 14) / 2, header_size_.width() - 3, 14),
                       Qt::AlignRight | Qt::AlignVCenter,
                       QString::number(static_cast<int64_t>((max_show_value + min_show_value) / 2 / 1000)) + "K");
      painter.drawText(QRectF(0, 1, header_size_.width() - 3, 14), Qt::AlignRight | Qt::AlignVCenter,
                       QString::number(static_cast<int64_t>(max_show_value / 1000)) + "K");
    } else {
      painter.drawText(QRectF(0, frame_height - header_size_.height() - 14 - 1, header_size_.width() - 3, 14),
                       Qt::AlignRight | Qt::AlignVCenter, QString::number(static_cast<int64_t>(min_show_value)));
      painter.drawText(QRectF(0, (frame_height - header_size_.height() - 14) / 2, header_size_.width() - 3, 14),
                       Qt::AlignRight | Qt::AlignVCenter,
                       QString::number(static_cast<int64_t>((max_show_value + min_show_value) / 2)));
      painter.drawText(QRectF(0, 1, header_size_.width() - 3, 14), Qt::AlignRight | Qt::AlignVCenter,
                       QString::number(static_cast<int64_t>(max_show_value)));
    }
  }

  painter.drawText(QRectF(header_size_.width() + 3, frame_height - header_size_.height(), header_size_.width(), 14),
                   Qt::AlignLeft | Qt::AlignVCenter, "0" + count_text_);

  painter.drawText(QRectF(frame_width / 2, frame_height - header_size_.height(), header_size_.width(), 14),
                   Qt::AlignCenter, QString::number(max_count_ / 2) + count_text_);

  painter.drawText(
      QRectF(frame_width - 3 - header_size_.width(), frame_height - header_size_.height(), header_size_.width(), 14),
      Qt::AlignRight | Qt::AlignVCenter, QString::number(max_count_) + count_text_);

  if (!unit_text_.isEmpty()) {
    painter.drawText(QRectF(0, frame_height - header_size_.height(), header_size_.width(), 14), Qt::AlignCenter,
                     unit_text_);
  }
}

// NOLINTEND
