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

#include <QSlider>
#include <atomic>

class SliderWidget : public QSlider {
  Q_OBJECT

 public:
  explicit SliderWidget(QWidget* parent = nullptr);

  ~SliderWidget();

  bool is_mouse_pressed() const;

 protected:
  void mousePressEvent(class QMouseEvent* event) override;

  void mouseReleaseEvent(class QMouseEvent* event) override;

  void mouseMoveEvent(class QMouseEvent* event) override;

  void wheelEvent(class QWheelEvent* event) override;

  void keyPressEvent(class QKeyEvent* event) override;

  void keyReleaseEvent(class QKeyEvent* event) override;

 private:
  std::atomic<bool> is_mouse_pressed_{false};
  class SliderStyle* style_{nullptr};
};

// NOLINTEND
