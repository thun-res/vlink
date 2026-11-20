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

#ifdef VLINK_ENABLE_VIEWER_OSG

#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Woverloaded-virtual"
#pragma GCC diagnostic ignored "-Wdeprecated-copy"
#endif

#include <osgViewer/Viewer>

#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic pop
#endif

#include <QElapsedTimer>
#include <QInputEvent>
#include <QOpenGLWidget>
#include <QPainter>
#include <atomic>
#include <map>

class OsgWidget : public QOpenGLWidget {
  Q_OBJECT
  Q_DISABLE_COPY(OsgWidget)

  Q_PROPERTY(int fpsRate READ fpsRate NOTIFY fpsRateChanged FINAL)

 public:
  explicit OsgWidget(QWidget* parent = nullptr);

  ~OsgWidget();

  osgViewer::Viewer* getViewer() const;

  int fpsRate() const;

 protected:
  void init();

  void resizeGL(int w, int h) override;

  void paintGL() override;

  void showEvent(QShowEvent* event) override;

  void hideEvent(QHideEvent* event) override;

  void keyPressEvent(QKeyEvent* event) override;

  void keyReleaseEvent(QKeyEvent* event) override;

  void mousePressEvent(QMouseEvent* event) override;

  void mouseReleaseEvent(QMouseEvent* event) override;

  void mouseDoubleClickEvent(QMouseEvent* event) override;

  void mouseMoveEvent(QMouseEvent* event) override;

  void wheelEvent(QWheelEvent* event) override;

  void timerEvent(QTimerEvent* event) override;

  bool needDoFrame();

 private:
  int getOsgKey(QKeyEvent* event);

  void setKeyboardModifiers(QInputEvent* event);

  void updateOsgSize(double w, double h);

  std::map<unsigned int, int> m_keyMap;
  osg::ref_ptr<osgViewer::Viewer> m_viewer;
  osg::ref_ptr<osgViewer::GraphicsWindowEmbedded> m_gw;
  int m_frameTimerId{-1};
  int m_fpsRate{0};
  int m_fpsCount{0};
  QElapsedTimer m_fpsTimer;

 signals:
  void fpsRateChanged(int fpsRate);
};

#endif

// NOLINTEND
