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

#include <QPaintEvent>
#include <QPainter>
#include <QResizeEvent>
#include <QWidget>

class TrafficWidget : public QWidget {
  Q_OBJECT

 public:
  explicit TrafficWidget(QWidget* parent = nullptr);

  ~TrafficWidget();

 public:
  void set_value_range(double min, double max);

  void set_max_count(int64_t count);

  void set_total_max_count(int64_t count);

  void set_unit(double unit);

  void set_suffix(int suffix);

  void set_color(const QColor& color);

  void set_unit_text(const QString& text);

  void set_count_text(const QString& text);

  void set_header_size(QSizeF size);

  void add_value(double value);

  double get_real_max_value();

  double get_real_min_value();

  void set_block(bool block);

  void clear();

 protected:
  void resizeEvent(QResizeEvent* event) override;

  void paintEvent(QPaintEvent* event) override;

 private:
  bool is_blocked_{false};
  double min_value_{100};
  double max_value_{100};
  int64_t max_count_{60};
  int64_t total_max_count_{3000};
  double unit_{1};
  int suffix_{0};
  QColor color_{Qt::darkGreen};
  QList<double> value_list_;
  QString unit_text_;
  QString count_text_{"s"};
  QSizeF header_size_{45.0, 15.0};
};

// NOLINTEND
