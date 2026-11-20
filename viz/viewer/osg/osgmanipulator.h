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

#include <osg/AnimationPath>
#include <osgGA/OrbitManipulator>
#include <osgViewer/Viewer>

#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic pop
#endif

#include <functional>
#include <tuple>
#include <utility>
#include <vector>

class OsgManipulator : public osgGA::OrbitManipulator {
 public:
  using FlyCompletedCallback = std::function<void(int)>;

  OsgManipulator();

  ~OsgManipulator();

 public:
  bool enablePick() const;

  void setEnablePick(bool enablePick);

  inline std::vector<osg::ref_ptr<osg::AnimationPath>> flyList() { return m_flyList; }

  inline void setFlyList(const std::vector<osg::ref_ptr<osg::AnimationPath>>& flyList) { m_flyList = flyList; }

  inline int flyIndex() const { return m_flyIndex; }

  inline void setFlyFinishedCallback(FlyCompletedCallback&& callback) { m_flyFinishedCallback = std::move(callback); }

  void setLimit(double maxPosition, double maxDistance, double minDistance);

  std::tuple<osg::Vec3d, osg::Vec3d, osg::Vec3d> getCurrentPoint() const;

  std::tuple<osg::Vec3d, osg::Quat, osg::Vec3d> getFlyPoint() const;

  bool playFly(int index);

  bool stopFly();

  void moveToPoint(const osg::Vec3d& moveEye, const osg::Vec3d& moveCenter, const osg::Vec3d& moveUp);

 protected:
  inline void setByInverseMatrix(const osg::Matrixd& matrix) override { setByMatrix(osg::Matrixd::inverse(matrix)); }

  inline osg::Matrixd getMatrix() const override { return m_matrix; }

  inline osg::Matrixd getInverseMatrix() const override { return osg::Matrixd::inverse(m_matrix); }

  void setByMatrix(const osg::Matrixd& matrix) override;

  void home(const osgGA::GUIEventAdapter& ea, osgGA::GUIActionAdapter& us) override;

  bool handle(const osgGA::GUIEventAdapter& ea, osgGA::GUIActionAdapter& us) override;

  bool handleFrame(const osgGA::GUIEventAdapter& ea, osgGA::GUIActionAdapter& us) override;

  bool handleKeyDown(const osgGA::GUIEventAdapter& ea, osgGA::GUIActionAdapter& us) override;

  bool performMovementLeftMouseButton(const double eventTimeDelta, const double dx, const double dy) override;

  bool performMovementMiddleMouseButton(const double eventTimeDelta, const double dx, const double dy) override;

  bool performMovementRightMouseButton(const double eventTimeDelta, const double dx, const double dy) override;

  bool handleMouseWheel(const osgGA::GUIEventAdapter& ea, osgGA::GUIActionAdapter& us) override;

 private:
  osg::ref_ptr<osg::AnimationPath> getAnimationPath(int index = -1) const;

  void resetAnimation();

  bool processFlyFrame(const osg::ref_ptr<osg::AnimationPath>& path, double time);

  std::vector<osg::ref_ptr<osg::AnimationPath>> m_flyList;
  osg::Matrixd m_matrix;
  FlyCompletedCallback m_flyFinishedCallback = nullptr;
  bool m_enablePick{false};
  int m_flyIndex{-1};
  int m_timeNum{-1};
  double m_eaTime{0};
  double m_timeOffset{0};
  double m_timeScale{1.0};
  double m_realTime{0};
  double m_animTime{0};
  double m_maxPosition{0};
  double m_maxDistance{0};
  double m_minDistance{0};

  enum class DragType { None, Pan, Rotate };
  DragType m_lastDragType{DragType::None};
  bool m_inertiaActive{false};
  double m_inertiaStartTime{0};
  double m_inertiaDuration{0.5};
  osg::Vec2d m_inertiaDelta{0, 0};
  double m_lastDragTime{-1.0};
  double m_minDragInterval{0.05};

  bool m_zoomActive{false};
  double m_zoomStartTime{0};
  double m_zoomDuration{0.5};
  double m_zoomStartDistance{0};
  double m_zoomTargetDistance{0};

  double m_lastScrollTime{-1};
  int m_scrollDirection{0};
  int m_scrollCount{0};

  bool m_moveActive{false};
  double m_moveStartTime{0.0};
  double m_moveDuration{1.0};
  osg::Vec3d m_moveStartCenter;
  osg::Quat m_moveStartRotation;
  double m_moveStartDistance{0.0};

  osg::Vec3d m_moveTargetCenter;
  osg::Quat m_moveTargetRotation;
  double m_moveTargetDistance{0.0};
};

#endif

// NOLINTEND
