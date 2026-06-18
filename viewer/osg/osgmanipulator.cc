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

#include "./osgmanipulator.h"

#ifdef VLINK_ENABLE_VIEWER_OSG

#include <iostream>

#include "./osgcommon.h"

#define FIRST_CONTROLPOINT_TIME 1

OsgManipulator::OsgManipulator() : osgGA::OrbitManipulator(DEFAULT_SETTINGS) {
  setAllowThrow(false);
  setVerticalAxisFixed(false);
  setWheelZoomFactor(0.1);
}

OsgManipulator::~OsgManipulator() {}

bool OsgManipulator::enablePick() const { return m_enablePick; }

void OsgManipulator::setEnablePick(bool enablePick) { m_enablePick = enablePick; }

void OsgManipulator::setLimit(double maxPosition, double maxDistance, double minDistance) {
  m_maxPosition = maxPosition;
  m_maxDistance = maxDistance;
  m_minDistance = minDistance;

  setMinimumDistance(m_minDistance);
}

std::tuple<osg::Vec3d, osg::Vec3d, osg::Vec3d> OsgManipulator::getCurrentPoint() const {
  std::tuple<osg::Vec3d, osg::Vec3d, osg::Vec3d> homePoint;
  getTransformation(std::get<0>(homePoint), std::get<1>(homePoint), std::get<2>(homePoint));

  return homePoint;
}

std::tuple<osg::Vec3d, osg::Quat, osg::Vec3d> OsgManipulator::getFlyPoint() const {
  std::tuple<osg::Vec3d, osg::Quat, osg::Vec3d> flyPoint;

  getTransformation(std::get<0>(flyPoint), std::get<1>(flyPoint));
  std::get<2>(flyPoint) = osg::Vec3d(1, 1, 1);

  return flyPoint;
}

bool OsgManipulator::playFly(int index) {
  if (!getAnimationPath(index).valid()) {
    return false;
  }

  m_flyIndex = index;

  const auto& p = getFlyPoint();
  osg::AnimationPath::ControlPoint point;
  point.setPosition(std::get<0>(p));
  point.setRotation(std::get<1>(p));
  point.setScale(std::get<2>(p));
  auto it = getAnimationPath()->getTimeControlPointMap().find(FIRST_CONTROLPOINT_TIME);

  if (it != getAnimationPath()->getTimeControlPointMap().end()) {
    getAnimationPath()->getTimeControlPointMap().erase(it);
  }

  getAnimationPath()->insert(FIRST_CONTROLPOINT_TIME, point);
  resetAnimation();

  return true;
}

bool OsgManipulator::stopFly() {
  if (!getAnimationPath().valid()) {
    m_flyIndex = -1;
    return false;
  }

  m_flyIndex = -1;

  return true;
}

void OsgManipulator::moveToPoint(const osg::Vec3d& moveEye, const osg::Vec3d& moveCenter, const osg::Vec3d& moveUp) {
  resetAnimation();

  osg::Vec3d oldEye, oldCenter, oldUp;
  getTransformation(oldEye, oldCenter, oldUp);

  m_moveStartCenter = oldCenter;
  m_moveStartRotation = _rotation;
  m_moveStartDistance = (oldEye - oldCenter).length();

  m_moveTargetCenter = moveCenter;
  m_moveTargetDistance = (moveEye - moveCenter).length();

  osg::Vec3d viewDir = moveCenter - moveEye;
  viewDir.normalize();

  osg::Quat baseRotation;
  baseRotation.makeRotate(osg::Vec3d(0, 0, -1), viewDir);

  osg::Vec3d side = viewDir ^ moveUp;

  if (side.length2() < 1e-6) {
    side = viewDir ^ osg::Vec3d(0, 0, 1);
  }

  side.normalize();

  osg::Vec3d up = side ^ viewDir;
  up.normalize();

  osg::Quat upCorrection;
  upCorrection.makeRotate(baseRotation * osg::Vec3d(0, 1, 0), up);

  m_moveTargetRotation = baseRotation * upCorrection;

  m_moveStartTime = -1.0;
  m_moveActive = true;
}

void OsgManipulator::setByMatrix(const osg::Matrixd& matrix) {
  _center = osg::Vec3d(0, 0, -_distance) * matrix;
  _rotation = matrix.getRotate();
  _distance = matrix.getTrans().length();
}

void OsgManipulator::home(const osgGA::GUIEventAdapter& ea, osgGA::GUIActionAdapter& us) {
  resetAnimation();

  osgGA::OrbitManipulator::home(ea, us);
}

bool OsgManipulator::handle(const osgGA::GUIEventAdapter& ea, osgGA::GUIActionAdapter& us) {
  osgViewer::Viewer* viewer = dynamic_cast<osgViewer::Viewer*>(&us);

  if (m_enablePick && ea.getEventType() == osgGA::GUIEventAdapter::DOUBLECLICK) {
    m_moveActive = false;

    if (viewer) {
      osgUtil::LineSegmentIntersector::Intersections intersections;
      viewer->computeIntersections(ea.getX(), ea.getY(), intersections);

      if (intersections.size() > 0) {
        const auto& p = *(intersections.begin());
        OsgCommon::printVec3d("pick-point", p.getWorldIntersectPoint());
      }
    }
  }

  osg::Vec3d oldCenter = _center;
  osg::Quat oldRotation = _rotation;
  double oldDistance = _distance;

  static osg::Vec2d lastMousePos;
  static osg::Vec2d dragDelta;

  if (ea.getEventType() == osgGA::GUIEventAdapter::PUSH) {
    lastMousePos = osg::Vec2d(ea.getXnormalized(), ea.getYnormalized());
    dragDelta.set(0.0, 0.0);
    m_lastDragType = DragType::None;
    m_inertiaActive = false;
  }

  if (ea.getEventType() == osgGA::GUIEventAdapter::DRAG) {
    m_moveActive = false;

    osg::Vec2d currentMouse(ea.getXnormalized(), ea.getYnormalized());
    dragDelta = currentMouse - lastMousePos;
    lastMousePos = currentMouse;

    switch (ea.getButtonMask()) {
      case osgGA::GUIEventAdapter::LEFT_MOUSE_BUTTON:
        m_lastDragType = DragType::Pan;
        break;
      case osgGA::GUIEventAdapter::RIGHT_MOUSE_BUTTON:
        m_lastDragType = DragType::Rotate;
        break;
      default:
        m_lastDragType = DragType::None;
        break;
    }

    m_inertiaActive = false;

    m_lastDragTime = ea.getTime();
  }

  if (ea.getEventType() == osgGA::GUIEventAdapter::RELEASE) {
    double releaseTime = ea.getTime();

    bool isCtrlDown = (ea.getModKeyMask() & osgGA::GUIEventAdapter::MODKEY_CTRL) != 0;

    if (!isCtrlDown && dragDelta.length2() > 1e-6 && (releaseTime - m_lastDragTime) < m_minDragInterval) {
      m_inertiaDelta = dragDelta;
      m_inertiaStartTime = releaseTime;
      m_inertiaActive = true;
    }
  }

  bool handled = osgGA::OrbitManipulator::handle(ea, us);

  if (_center.length() > m_maxPosition || _distance > m_maxDistance || _distance < m_minDistance) {
    _center = oldCenter;
    _rotation = oldRotation;
    _distance = oldDistance;
  }

  return handled;
}

bool OsgManipulator::handleFrame(const osgGA::GUIEventAdapter& ea, osgGA::GUIActionAdapter& us) {
  m_eaTime = ea.getTime();

  if (getAnimationPath().valid()) {
    return processFlyFrame(getAnimationPath(), m_eaTime);
  }

  if (m_inertiaActive) {
    double t = m_eaTime - m_inertiaStartTime;
    if (t > m_inertiaDuration) {
      m_inertiaActive = false;
    } else {
      double progress = t / m_inertiaDuration;
      double ease = 1.0 - std::pow(1.0 - progress, 4.0);
      osg::Vec2d delta = m_inertiaDelta * (1.0 - ease);

      float scale = -0.3f * _distance * 0.5;

      if (m_lastDragType == DragType::Pan) {
        panModel(delta.x() * scale, delta.y() * scale, 0);
      } else if (m_lastDragType == DragType::Rotate) {
        osg::Quat newRotateX, newRotateY;
        osg::Vec3d localUp(0.0, 0.0, 1.0);
        osg::Matrix rotMat;
        rotMat.makeRotate(_rotation);
        osg::Vec3d side = getSideVector(rotMat);
        osg::Vec3d forward = localUp ^ side;
        side = forward ^ localUp;
        forward.normalize();
        side.normalize();

        newRotateX.makeRotate(delta.y(), side);
        newRotateY.makeRotate(-delta.x(), localUp);

        _rotation = _rotation * newRotateX * newRotateY;
        _center = newRotateX * newRotateY * _center;
      }
    }
  }

  if (!std::isnan(_distance)) {
    m_matrix =
        osg::Matrixd::translate(0, 0, _distance) * osg::Matrixd::rotate(_rotation) * osg::Matrixd::translate(_center);
  }

  if (m_zoomActive) {
    double t = m_eaTime - m_zoomStartTime;

    if (t > m_zoomDuration) {
      _distance = m_zoomTargetDistance;
      m_zoomActive = false;
    } else {
      double progress = t / m_zoomDuration;
      double ease = 1.0 - std::pow(1.0 - progress, 4.0);
      _distance = m_zoomStartDistance + (m_zoomTargetDistance - m_zoomStartDistance) * ease;
    }
  }

  if (m_moveActive) {
    if (m_moveStartTime < 0.0) {
      m_moveStartTime = m_eaTime;

      us.requestContinuousUpdate(true);
    }

    double t = m_eaTime - m_moveStartTime;
    if (t > m_moveDuration) {
      _center = m_moveTargetCenter;
      _rotation = m_moveTargetRotation;
      _distance = m_moveTargetDistance;
      m_moveActive = false;

      us.requestContinuousUpdate(false);
    } else {
      double progress = t / m_moveDuration;
      double ease = 1.0 - std::pow(1.0 - progress, 4.0);

      _center = m_moveStartCenter + (m_moveTargetCenter - m_moveStartCenter) * ease;
      _distance = m_moveStartDistance + (m_moveTargetDistance - m_moveStartDistance) * ease;
      _rotation.slerp(ease, m_moveStartRotation, m_moveTargetRotation);

      us.requestRedraw();
    }
  }

  return osgGA::OrbitManipulator::handleFrame(ea, us);
}

bool OsgManipulator::handleKeyDown(const osgGA::GUIEventAdapter& ea, osgGA::GUIActionAdapter& us) {
  if (ea.getKey() == osgGA::GUIEventAdapter::KEY_Space) {
    osg::Vec3d homeEye;
    osg::Vec3d homeCenter;
    osg::Vec3d homeUp;

    getHomePosition(homeEye, homeCenter, homeUp);
    moveToPoint(homeEye, homeCenter, homeUp);

    return true;
  } else if (ea.getKey() == osgGA::GUIEventAdapter::KEY_F5) {
    const auto& homePoint = getCurrentPoint();
    const auto& flyPoint = getFlyPoint();

    std::cout << "***********HomePoint***********" << std::endl;
    OsgCommon::printVec3d("eye", std::get<0>(homePoint));
    OsgCommon::printVec3d("center", std::get<1>(homePoint));
    OsgCommon::printVec3d("up", std::get<2>(homePoint));
    std::cout << "***********FlyPoint***********" << std::endl;
    OsgCommon::printVec3d("position", std::get<0>(flyPoint));
    OsgCommon::printVec4d("rotation", std::get<1>(flyPoint).asVec4());
    OsgCommon::printVec3d("scale", std::get<2>(flyPoint));

    return true;
  }

  return osgGA::OrbitManipulator::handleKeyDown(ea, us);
}

bool OsgManipulator::performMovementLeftMouseButton(const double eventTimeDelta, const double dx, const double dy) {
  float scale = -0.3f * _distance * getThrowScale(eventTimeDelta);
  panModel(dx * scale, dy * scale, 0);

  return true;
}

bool OsgManipulator::performMovementMiddleMouseButton(const double eventTimeDelta, const double dx, const double dy) {
  (void)dx;
  zoomModel(dy * getThrowScale(eventTimeDelta), false);

  return true;
}

bool OsgManipulator::performMovementRightMouseButton(const double eventTimeDelta, const double dx, const double dy) {
  (void)eventTimeDelta;

  osg::Matrix rotation_matrix;
  rotation_matrix.makeRotate(_rotation);

  osg::Vec3d sideVector = getSideVector(rotation_matrix);
  osg::Vec3d localUp(0.0, 0.0, 1.0);
  osg::Vec3d forwardVector = localUp ^ sideVector;

  sideVector = forwardVector ^ localUp;
  forwardVector.normalize();
  sideVector.normalize();

  osg::Quat newRotatex;
  newRotatex.makeRotate(dy, sideVector);

  osg::Quat newRotatey;
  newRotatey.makeRotate(-dx, localUp);
  _rotation = _rotation * newRotatex * newRotatey;
  _center = newRotatex * newRotatey * _center;

  return true;
}

bool OsgManipulator::handleMouseWheel(const osgGA::GUIEventAdapter& ea, osgGA::GUIActionAdapter& us) {
  m_moveActive = false;

  auto sm = ea.getScrollingMotion();

  int currentDirection = 0;
  switch (sm) {
    case osgGA::GUIEventAdapter::SCROLL_UP:
      currentDirection = -1;
      break;
    case osgGA::GUIEventAdapter::SCROLL_DOWN:
      currentDirection = 1;
      break;
    default:
      return false;
  }

  if (m_lastScrollTime >= 0 && currentDirection == m_scrollDirection && (m_eaTime - m_lastScrollTime) < 0.3) {
    m_scrollCount++;
  } else {
    m_scrollCount = 1;
    m_scrollDirection = currentDirection;
  }

  m_lastScrollTime = m_eaTime;

  double baseZoomAmount = currentDirection * _wheelZoomFactor;
  double dynamicScale = std::min(1.0 + 0.1 * m_scrollCount, 3.0);
  double zoomAmount = baseZoomAmount * dynamicScale;

  double zoomScale = std::exp(-zoomAmount);

  double currentDistance = m_zoomActive ? m_zoomTargetDistance : _distance;

  double newTargetDistance = currentDistance * zoomScale;

  newTargetDistance = std::clamp(newTargetDistance, m_minDistance, m_maxDistance);

  m_zoomStartTime = m_eaTime;
  m_zoomStartDistance = _distance;
  m_zoomTargetDistance = newTargetDistance;
  m_zoomActive = true;

  us.requestRedraw();
  us.requestContinuousUpdate(true);

  return true;
}

osg::ref_ptr<osg::AnimationPath> OsgManipulator::getAnimationPath(int index) const {
  if (index < 0) {
    index = m_flyIndex;
  }

  if (index >= 0 && index < static_cast<int>(m_flyList.size())) {
    return m_flyList.at(index);
  }

  return osg::ref_ptr<osg::AnimationPath>();
}

void OsgManipulator::resetAnimation() {
  if (getAnimationPath().valid()) {
    m_timeOffset = getAnimationPath()->getFirstTime() - m_eaTime;
  } else {
    m_timeOffset = 0;
  }

  m_timeNum = -1;
}

bool OsgManipulator::processFlyFrame(const osg::ref_ptr<osg::AnimationPath>& path, double time) {
  osg::AnimationPath::ControlPoint cp;

  double animTime = (time + m_timeOffset) * m_timeScale;
  path->getInterpolatedControlPoint(animTime, cp);

  if (m_timeNum == -1) {
    m_realTime = time;
    m_animTime = animTime;
  }

  m_timeNum++;

  double animDelta = (animTime - m_animTime);

  if (animDelta >= path->getPeriod()) {
    m_flyIndex = -1;

    if (m_flyFinishedCallback) {
      m_flyFinishedCallback(m_flyIndex);
    }

    m_realTime = time;
    m_animTime = animTime;
    m_timeNum = 0;
  }

  cp.getMatrix(m_matrix);
  setByMatrix(m_matrix);

  return false;
}

#endif

// NOLINTEND
