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

#include "./osggraphicsview.h"

#ifdef VLINK_ENABLE_VIEWER_OSG

#include <QGuiApplication>
#include <QOpenGLFunctions>
#include <QPaintEngine>
#include <QPainter>
#include <QSurfaceFormat>

class OsgGraphicsScene : public QGraphicsScene {
 public:
  OsgGraphicsScene(OsgGraphicsView* view) : QGraphicsScene(view), m_view(view) {}

 protected:
  void drawBackground(QPainter* painter, const QRectF& rect) override {
    (void)rect;

    painter->save();
    painter->beginNativePainting();

    m_view->paintGL();

    painter->endNativePainting();
    painter->restore();
  }

 private:
  OsgGraphicsView* m_view{nullptr};
};

OsgGraphicsView::OsgGraphicsView(QWidget* parent) : QGraphicsView{parent} {
  this->setRenderHints(QPainter::Antialiasing | QPainter::TextAntialiasing | QPainter::SmoothPixmapTransform);
  this->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  this->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);

  m_scene = new OsgGraphicsScene(this);
  m_scene->setStickyFocus(true);
  m_scene->setSceneRect(0, 0, this->width(), this->height());
  this->setScene(m_scene);

  m_glwidget = new QOpenGLWidget(this);
  this->setViewport(m_glwidget);
  this->setViewportUpdateMode(QGraphicsView::FullViewportUpdate);

  // QSurfaceFormat format = QSurfaceFormat::defaultFormat();
  // format.setRenderableType(QSurfaceFormat::OpenGL);
  // format.setProfile(QSurfaceFormat::CompatibilityProfile);
  // format.setSamples(4);
  // m_glwidget->setFormat(format);

  m_frameTimerId = this->startTimer(10, Qt::PreciseTimer);
  m_fpsTimer.start();

  init();
}

OsgGraphicsView::~OsgGraphicsView() {
  if (m_viewer) {
    m_viewer->setDone(true);
  }
}

osgViewer::Viewer* OsgGraphicsView::getViewer() const { return m_viewer.get(); }

int OsgGraphicsView::fpsRate() const { return m_fpsRate; }

void OsgGraphicsView::init() {
  if (QGuiApplication::platformName() == QLatin1String("wayland")) {
    QGuiApplication::setAttribute(Qt::AA_DontCreateNativeWidgetSiblings);
    setAttribute(Qt::WA_NativeWindow);
  }

  m_viewer = new osgViewer::Viewer;

  osg::DisplaySettings* display_settings = osg::DisplaySettings::instance();
  display_settings->setNvOptimusEnablement(1);
  display_settings->setStereo(false);

  if (display_settings->getVertexBufferHint() == osg::DisplaySettings::NO_PREFERENCE) {
    display_settings->setVertexBufferHint(osg::DisplaySettings::VERTEX_BUFFER_OBJECT);
  }

  osg::ref_ptr<osg::GraphicsContext::Traits> traits = new osg::GraphicsContext::Traits(display_settings);
  traits->x = 0;
  traits->y = 0;
  traits->width = this->width();
  traits->height = this->height();
  traits->samples = 4;
  traits->doubleBuffer = true;
  traits->windowDecoration = false;
  traits->useCursor = false;

  m_gw = new osgViewer::GraphicsWindowEmbedded(traits);

  osg::Camera* camera = m_viewer->getCamera();
  camera->setGraphicsContext(m_gw);
  camera->setViewport(0, 0, 1920.0, 1080.0);
  camera->setProjectionMatrixAsPerspective(30.0, 1920.0 / 1080.0, 0.01f, 1000.0f);

  m_viewer->setRunFrameScheme(osgViewer::Viewer::ON_DEMAND);
  m_viewer->setThreadingModel(osgViewer::Viewer::SingleThreaded);
  m_viewer->setKeyEventSetsDone(0);

  m_keyMap[Qt::Key_Escape] = osgGA::GUIEventAdapter::KEY_Escape;
  m_keyMap[Qt::Key_Delete] = osgGA::GUIEventAdapter::KEY_Delete;
  m_keyMap[Qt::Key_Home] = osgGA::GUIEventAdapter::KEY_Home;
  m_keyMap[Qt::Key_Enter] = osgGA::GUIEventAdapter::KEY_KP_Enter;
  m_keyMap[Qt::Key_End] = osgGA::GUIEventAdapter::KEY_End;
  m_keyMap[Qt::Key_Return] = osgGA::GUIEventAdapter::KEY_Return;
  m_keyMap[Qt::Key_PageUp] = osgGA::GUIEventAdapter::KEY_Page_Up;
  m_keyMap[Qt::Key_PageDown] = osgGA::GUIEventAdapter::KEY_Page_Down;
  m_keyMap[Qt::Key_Left] = osgGA::GUIEventAdapter::KEY_Left;
  m_keyMap[Qt::Key_Right] = osgGA::GUIEventAdapter::KEY_Right;
  m_keyMap[Qt::Key_Up] = osgGA::GUIEventAdapter::KEY_Up;
  m_keyMap[Qt::Key_Down] = osgGA::GUIEventAdapter::KEY_Down;
  m_keyMap[Qt::Key_Backspace] = osgGA::GUIEventAdapter::KEY_BackSpace;
  m_keyMap[Qt::Key_Tab] = osgGA::GUIEventAdapter::KEY_Tab;
  m_keyMap[Qt::Key_Space] = osgGA::GUIEventAdapter::KEY_Space;
  m_keyMap[Qt::Key_Delete] = osgGA::GUIEventAdapter::KEY_Delete;
  m_keyMap[Qt::Key_Alt] = osgGA::GUIEventAdapter::KEY_Alt_L;
  m_keyMap[Qt::Key_Shift] = osgGA::GUIEventAdapter::KEY_Shift_L;
  m_keyMap[Qt::Key_Control] = osgGA::GUIEventAdapter::KEY_Control_L;
  m_keyMap[Qt::Key_Meta] = osgGA::GUIEventAdapter::KEY_Meta_L;
  m_keyMap[Qt::Key_F1] = osgGA::GUIEventAdapter::KEY_F1;
  m_keyMap[Qt::Key_F2] = osgGA::GUIEventAdapter::KEY_F2;
  m_keyMap[Qt::Key_F3] = osgGA::GUIEventAdapter::KEY_F3;
  m_keyMap[Qt::Key_F4] = osgGA::GUIEventAdapter::KEY_F4;
  m_keyMap[Qt::Key_F5] = osgGA::GUIEventAdapter::KEY_F5;
  m_keyMap[Qt::Key_F6] = osgGA::GUIEventAdapter::KEY_F6;
  m_keyMap[Qt::Key_F7] = osgGA::GUIEventAdapter::KEY_F7;
  m_keyMap[Qt::Key_F8] = osgGA::GUIEventAdapter::KEY_F8;
  m_keyMap[Qt::Key_F9] = osgGA::GUIEventAdapter::KEY_F9;
  m_keyMap[Qt::Key_F10] = osgGA::GUIEventAdapter::KEY_F10;
  m_keyMap[Qt::Key_F11] = osgGA::GUIEventAdapter::KEY_F11;
  m_keyMap[Qt::Key_F12] = osgGA::GUIEventAdapter::KEY_F12;
  m_keyMap[Qt::Key_F13] = osgGA::GUIEventAdapter::KEY_F13;
  m_keyMap[Qt::Key_F14] = osgGA::GUIEventAdapter::KEY_F14;
  m_keyMap[Qt::Key_F15] = osgGA::GUIEventAdapter::KEY_F15;
  m_keyMap[Qt::Key_F16] = osgGA::GUIEventAdapter::KEY_F16;
  m_keyMap[Qt::Key_F17] = osgGA::GUIEventAdapter::KEY_F17;
  m_keyMap[Qt::Key_F18] = osgGA::GUIEventAdapter::KEY_F18;
  m_keyMap[Qt::Key_F19] = osgGA::GUIEventAdapter::KEY_F19;
  m_keyMap[Qt::Key_F20] = osgGA::GUIEventAdapter::KEY_F20;
  m_keyMap[Qt::Key_hyphen] = '-';
  m_keyMap[Qt::Key_Equal] = '=';
  m_keyMap[Qt::Key_division] = osgGA::GUIEventAdapter::KEY_KP_Divide;
  m_keyMap[Qt::Key_multiply] = osgGA::GUIEventAdapter::KEY_KP_Multiply;
  m_keyMap[Qt::Key_Minus] = '-';
  m_keyMap[Qt::Key_Plus] = '+';
  m_keyMap[Qt::Key_Insert] = osgGA::GUIEventAdapter::KEY_KP_Insert;
}

void OsgGraphicsView::paintGL() {
  if (m_viewer) {
    QOpenGLContext::currentContext()->functions()->glUseProgram(0);
    m_viewer->frame();

    ++m_fpsCount;

    if (m_fpsTimer.elapsed() > 1000) {
      if (m_fpsRate != m_fpsCount) {
        m_fpsRate = m_fpsCount;
        emit fpsRateChanged(m_fpsRate);
      }

      m_fpsTimer.restart();
      m_fpsCount = 0;
    }
  }
}

void OsgGraphicsView::resizeGL(int w, int h) {
  auto ratio = qApp->devicePixelRatio();
  updateOsgSize(w * ratio, h * ratio);
}

void OsgGraphicsView::drawBackground(QPainter* painter, const QRectF& rect) {
  QGraphicsView::drawBackground(painter, rect);
}

void OsgGraphicsView::resizeEvent(QResizeEvent* event) {
  QGraphicsView::resizeEvent(event);

  m_scene->setSceneRect(0, 0, event->size().width(), event->size().height());

  resizeGL(event->size().width(), event->size().height());
}

void OsgGraphicsView::showEvent(QShowEvent* event) { QGraphicsView::showEvent(event); }

void OsgGraphicsView::hideEvent(QHideEvent* event) { QGraphicsView::hideEvent(event); }

void OsgGraphicsView::keyPressEvent(QKeyEvent* event) {
  QGraphicsView::keyPressEvent(event);

  if (event->key() == Qt::Key_Escape) {
    return;
  }

  // event->accept();

  setKeyboardModifiers(event);
  m_gw->getEventQueue()->keyPress(getOsgKey(event));
}

void OsgGraphicsView::keyReleaseEvent(QKeyEvent* event) {
  QGraphicsView::keyReleaseEvent(event);

  if (event->key() == Qt::Key_Escape) {
    return;
  }

  // event->accept();

  if (event->isAutoRepeat()) {
    event->ignore();
  } else {
    setKeyboardModifiers(event);
    m_gw->getEventQueue()->keyRelease(getOsgKey(event));
  }
}

void OsgGraphicsView::mousePressEvent(QMouseEvent* event) {
  setFocus();
  QGraphicsView::mousePressEvent(event);

  event->accept();

  int button = 0;
  switch (event->button()) {
    case Qt::LeftButton:
      button = 1;
      break;
    case Qt::MiddleButton:
      button = 2;
      break;
    case Qt::RightButton:
      button = 3;
      break;
    case Qt::NoButton:
      button = 0;
      break;
    default:
      button = 0;
      break;
  }

  setKeyboardModifiers(event);
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
  m_gw->getEventQueue()->mouseButtonPress(event->position().x(), event->position().y(), button);
#else
  m_gw->getEventQueue()->mouseButtonPress(event->localPos().x(), event->localPos().y(), button);
#endif
}

void OsgGraphicsView::mouseReleaseEvent(QMouseEvent* event) {
  QGraphicsView::mouseReleaseEvent(event);

  event->accept();

  int button = 0;
  switch (event->button()) {
    case Qt::LeftButton:
      button = 1;
      break;
    case Qt::MiddleButton:
      button = 2;
      break;
    case Qt::RightButton:
      button = 3;
      break;
    case Qt::NoButton:
      button = 0;
      break;
    default:
      button = 0;
      break;
  }

  setKeyboardModifiers(event);
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
  m_gw->getEventQueue()->mouseButtonRelease(event->position().x(), event->position().y(), button);
#else
  m_gw->getEventQueue()->mouseButtonRelease(event->localPos().x(), event->localPos().y(), button);
#endif
}

void OsgGraphicsView::mouseDoubleClickEvent(QMouseEvent* event) {
  QGraphicsView::mouseDoubleClickEvent(event);

  event->accept();

  int button = 0;
  switch (event->button()) {
    case Qt::LeftButton:
      button = 1;
      break;
    case Qt::MiddleButton:
      button = 2;
      break;
    case Qt::RightButton:
      button = 3;
      break;
    case Qt::NoButton:
      button = 0;
      break;
    default:
      button = 0;
      break;
  }

  setKeyboardModifiers(event);
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
  m_gw->getEventQueue()->mouseDoubleButtonPress(event->position().x(), event->position().y(), button);
#else
  m_gw->getEventQueue()->mouseDoubleButtonPress(event->localPos().x(), event->localPos().y(), button);
#endif
}

void OsgGraphicsView::mouseMoveEvent(QMouseEvent* event) {
  QGraphicsView::mouseMoveEvent(event);

  event->accept();

  setKeyboardModifiers(event);
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
  m_gw->getEventQueue()->mouseMotion(event->position().x(), event->position().y());
#else
  m_gw->getEventQueue()->mouseMotion(event->localPos().x(), event->localPos().y());
#endif
}

void OsgGraphicsView::wheelEvent(QWheelEvent* event) {
  QGraphicsView::wheelEvent(event);

  event->accept();

  setKeyboardModifiers(event);

  int px = std::abs(event->angleDelta().x() / 120);
  int py = std::abs(event->angleDelta().y() / 120);

  if (event->angleDelta().x() < 0) {
    for (int i = 0; i < px; ++i) {
      m_gw->getEventQueue()->mouseScroll(osgGA::GUIEventAdapter::SCROLL_LEFT);
    }
  } else if (event->angleDelta().x() > 0) {
    for (int i = 0; i < px; ++i) {
      m_gw->getEventQueue()->mouseScroll(osgGA::GUIEventAdapter::SCROLL_RIGHT);
    }
  } else if (event->angleDelta().y() < 0) {
    for (int i = 0; i < py; ++i) {
      m_gw->getEventQueue()->mouseScroll(osgGA::GUIEventAdapter::SCROLL_UP);
    }
  } else if (event->angleDelta().y() > 0) {
    for (int i = 0; i < py; ++i) {
      m_gw->getEventQueue()->mouseScroll(osgGA::GUIEventAdapter::SCROLL_DOWN);
    }
  }
}

void OsgGraphicsView::timerEvent(QTimerEvent* event) {
  QGraphicsView::timerEvent(event);

  if (event->timerId() == m_frameTimerId) {
    if (needDoFrame()) {
      m_scene->update();
    }
  }
}

bool OsgGraphicsView::needDoFrame() {
  if (!m_viewer) {
    return false;
  }

  // if (m_viewer->getRequestRedraw()) {
  //   return true;
  // }

  // if (m_viewer->getRequestContinousUpdate()) {
  //   return true;
  // }

  // if (m_viewer->requiresUpdateSceneGraph()) {
  //   return true;
  // }

  // if (m_viewer->getDatabasePager()->requiresUpdateSceneGraph()) {
  //   return true;
  // }

  // if (m_viewer->getImagePager()->requiresUpdateSceneGraph()) {
  //   return true;
  // }

  // if (m_viewer->requiresRedraw()) {
  //   return true;
  // }

  // if (m_viewer->checkEvents()) {
  //   return true;
  // }

  // if (m_viewer->getRequestRedraw()) {
  //   return true;
  // }

  // if (m_viewer->getRequestContinousUpdate()) {
  //   return true;
  // }

  return true;
}

int OsgGraphicsView::getOsgKey(QKeyEvent* event) {
  auto iter = m_keyMap.find(event->key());
  return iter == m_keyMap.end() ? static_cast<int>(*(event->text().toLatin1().data())) : iter->second;
}

void OsgGraphicsView::setKeyboardModifiers(QInputEvent* event) {
  int modkey = event->modifiers() & (Qt::ShiftModifier | Qt::ControlModifier | Qt::AltModifier);

  unsigned int mask = 0;

  if (modkey & Qt::ShiftModifier) {
    mask |= osgGA::GUIEventAdapter::MODKEY_SHIFT;
  }

  if (modkey & Qt::ControlModifier) {
    mask |= osgGA::GUIEventAdapter::MODKEY_CTRL;
  }

  if (modkey & Qt::AltModifier) {
    mask |= osgGA::GUIEventAdapter::MODKEY_ALT;
  }

  m_gw->getEventQueue()->getCurrentEventState()->setModKeyMask(mask);
}

void OsgGraphicsView::updateOsgSize(double w, double h) {
  if (!m_viewer || !m_gw) {
    return;
  }

  m_gw->getEventQueue()->windowResize(0, 0, w, h);
  m_gw->resized(0, 0, w, h);

  m_viewer->getCamera()->setViewport(0, 0, w, h);
  m_viewer->getCamera()->resize(w, h);

  double fovy;
  double aspectRatio;
  double zNear;
  double zFar;

  m_viewer->getCamera()->getProjectionMatrixAsPerspective(fovy, aspectRatio, zNear, zFar);

  m_viewer->getCamera()->setProjectionMatrixAsPerspective(fovy, w / h, zNear, zFar);
}

#endif

// NOLINTEND
