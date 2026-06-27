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

#include "./osgselecthandler.h"

#ifdef VLINK_ENABLE_VIEWER_OSG

#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Woverloaded-virtual"
#pragma GCC diagnostic ignored "-Wdeprecated-copy"
#endif

#include <osg/Material>
#include <osg/PolygonMode>

#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic pop
#endif

OsgSelectHandler::OsgSelectHandler(osg::ref_ptr<osg::Camera> spHudCamera) : m_spHudCamera(spHudCamera) {}

OsgSelectHandler::~OsgSelectHandler() {}

void OsgSelectHandler::registerSelectCallback(SelectCallback&& selectCallback) {
  m_selectCallback = std::move(selectCallback);
}

void OsgSelectHandler::registerCtrlCallback(CtrlCallback&& ctrlCallback) { m_ctrlCallback = std::move(ctrlCallback); }

bool OsgSelectHandler::isSelecting() const { return m_selecting; }

void OsgSelectHandler::setSelecting(bool selecting) { m_selecting = selecting; }

void OsgSelectHandler::setRatio(double ratio) { m_ratio = ratio; }

bool OsgSelectHandler::handle(const osgGA::GUIEventAdapter& ea, osgGA::GUIActionAdapter& aa, osg::Object* obj,
                              osg::NodeVisitor* nv) {
  (void)aa;
  (void)obj;
  (void)nv;

  m_pViewer = static_cast<osgViewer::Viewer*>(&aa);

  if (m_pViewer == nullptr) {
    return false;
  }

  if (!m_selecting) {
    return false;
  }

  auto width = m_pViewer->getCamera()->getViewport()->width();
  auto height = m_pViewer->getCamera()->getViewport()->height();

  m_spHudCamera->setProjectionMatrix(osg::Matrix::ortho2D(0, width, 0, height));

  auto eventType = ea.getEventType();

  switch (eventType) {
    case osgGA::GUIEventAdapter::PUSH: {
      if (m_ctrlCallback && m_ctrlCallback()) {
        auto buttonMask = ea.getButtonMask();
        auto bIsMouseBtn = buttonMask & osgGA::GUIEventAdapter::LEFT_MOUSE_BUTTON;

        if (bIsMouseBtn) {
          m_fStartPosX = ea.getX() * m_ratio;
          m_fStartPosY = ea.getY() * m_ratio;
          m_bPush = true;
        }
      }
    } break;
    case osgGA::GUIEventAdapter::RELEASE: {
      if (m_bPush && m_selecting && m_selectCallback) {
        if (m_spOldNode != nullptr) {
          double startX = std::min(m_fStartPosX, m_fEndPosX);
          double endX = std::max(m_fStartPosX, m_fEndPosX);
          double startY = std::min(height * m_ratio - m_fStartPosY, height * m_ratio - m_fEndPosY);
          double endY = std::max(height * m_ratio - m_fStartPosY, height * m_ratio - m_fEndPosY);

          m_selectCallback(startX, endX, startY, endY);
          m_spHudCamera->removeChild(m_spOldNode);
          m_spOldNode = nullptr;
        }

        return true;
      }

      m_bPush = false;

    } break;
    case osgGA::GUIEventAdapter::DRAG: {
      if (m_ctrlCallback && m_ctrlCallback()) {
        auto buttonMask = ea.getButtonMask();
        auto bIsMouseBtn = buttonMask & osgGA::GUIEventAdapter::LEFT_MOUSE_BUTTON;

        if (bIsMouseBtn && m_bPush) {
          m_fEndPosX = ea.getX() * m_ratio;
          m_fEndPosY = ea.getY() * m_ratio;

          osg::Geode* pSelectBox = createSelectBox(m_fStartPosX, m_fStartPosY - height * (m_ratio - 1), m_fEndPosX,
                                                   m_fEndPosY - height * (m_ratio - 1));
          if (m_spOldNode != nullptr) {
            m_spHudCamera->removeChild(m_spOldNode);
            m_spOldNode = nullptr;
          }

          m_spHudCamera->addChild(pSelectBox);
          m_spOldNode = pSelectBox;
          m_selecting = true;

          return true;
        }
      }
    } break;
    default:
      return false;
  }

  return false;
}

osg::Geode* OsgSelectHandler::createSelectBox(double fStartPosX, double fStartPosY, double fEndPosX, double fEndPosY) {
  osg::Geode* pGeode = new osg::Geode();
  osg::Geometry* pQuardGeomerty = new osg::Geometry();

  osg::Vec3dArray* pVertArray = new osg::Vec3dArray;
  pVertArray->push_back(osg::Vec3d(fStartPosX, fStartPosY, 0.0));
  pVertArray->push_back(osg::Vec3d(fStartPosX, fEndPosY, 0.0));
  pVertArray->push_back(osg::Vec3d(fEndPosX, fEndPosY, 0.0));
  pVertArray->push_back(osg::Vec3d(fEndPosX, fStartPosY, 0.0));
  pQuardGeomerty->setVertexArray(pVertArray);

  osg::Vec4dArray* pColorArray = new osg::Vec4dArray;
  pColorArray->push_back(osg::Vec4d(1.0, 1.0, 1.0, 0.4));
  pQuardGeomerty->setColorArray(pColorArray);
  pQuardGeomerty->setColorBinding(osg::Geometry::AttributeBinding::BIND_OVERALL);

  pQuardGeomerty->addPrimitiveSet(new osg::DrawArrays(GL_QUADS, 0, 4));
  pQuardGeomerty->setUseVertexBufferObjects(true);

  pGeode->getOrCreateStateSet()->setMode(GL_LIGHTING, osg::StateAttribute::OFF | osg::StateAttribute::PROTECTED);
  pGeode->getOrCreateStateSet()->setMode(GL_BLEND, osg::StateAttribute::ON);

  pGeode->addChild(pQuardGeomerty);
  pGeode->addDrawable(pQuardGeomerty);

  return pGeode;
}

#endif

// NOLINTEND
