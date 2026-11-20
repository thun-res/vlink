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

#include <osg/BlendFunc>
#include <osg/ShapeDrawable>
#include <osgViewer/Viewer>

#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic pop
#endif

#include <atomic>
#include <functional>

class OsgSelectHandler : public osgGA::GUIEventHandler {
 public:
  using SelectCallback = std::function<void(double xMin, double xMax, double yMin, double yMax)>;
  using CtrlCallback = std::function<bool()>;

  OsgSelectHandler(osg::ref_ptr<osg::Camera> spHudCamera);

  ~OsgSelectHandler();

  void registerSelectCallback(SelectCallback&& selectCallback);

  void registerCtrlCallback(CtrlCallback&& ctrlCallback);

  bool isSelecting() const;

  void setSelecting(bool selecting);

  void setRatio(double ratio);

 protected:
  virtual bool handle(const osgGA::GUIEventAdapter& ea, osgGA::GUIActionAdapter& aa, osg::Object* obj,
                      osg::NodeVisitor* nv);

 private:
  osg::Geode* createSelectBox(double fStartPosX, double fStartPosY, double fEndPosX, double fEndPosY);

  double m_fStartPosX{0.0};
  double m_fStartPosY{0.0};
  double m_fEndPosX{0.0};
  double m_fEndPosY{0.0};
  double m_ratio{1.0};

  bool m_bPush{false};
  std::atomic_bool m_selecting{false};

  osgViewer::Viewer* m_pViewer{nullptr};
  osg::ref_ptr<osg::Camera> m_spHudCamera;
  osg::ref_ptr<osg::Node> m_spOldNode;
  SelectCallback m_selectCallback;
  CtrlCallback m_ctrlCallback;
};

#endif

// NOLINTEND
