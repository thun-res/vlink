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

#include "./sliderwidget.h"

#include <QKeyEvent>
#include <QMouseEvent>
#include <QProxyStyle>
#include <QWheelEvent>

class SliderStyle : public QProxyStyle {
 public:
  SliderStyle(QStyle* style = nullptr) : QProxyStyle(style) {}

  int styleHint(StyleHint hint, const QStyleOption* option = nullptr, const QWidget* widget = nullptr,
                QStyleHintReturn* returnData = nullptr) const override {
    if (hint == QStyle::SH_Slider_AbsoluteSetButtons) {
      return Qt::LeftButton;
    }

    return QProxyStyle::styleHint(hint, option, widget, returnData);
  }
};

SliderWidget::SliderWidget(QWidget* parent) : QSlider(parent), style_(new SliderStyle) { setStyle(style_); }

SliderWidget::~SliderWidget() {
  delete style_;
  style_ = nullptr;
}

bool SliderWidget::is_mouse_pressed() const { return is_mouse_pressed_; }

void SliderWidget::mousePressEvent(QMouseEvent* event) {
  is_mouse_pressed_ = true;
  QSlider::mousePressEvent(event);
}

void SliderWidget::mouseReleaseEvent(QMouseEvent* event) {
  QSlider::mouseReleaseEvent(event);
  is_mouse_pressed_ = false;
}

void SliderWidget::mouseMoveEvent(QMouseEvent* event) { QSlider::mouseMoveEvent(event); }

void SliderWidget::wheelEvent(QWheelEvent* event) { event->ignore(); }

void SliderWidget::keyPressEvent(QKeyEvent* event) { event->ignore(); }

void SliderWidget::keyReleaseEvent(QKeyEvent* event) { event->ignore(); }

// NOLINTEND
