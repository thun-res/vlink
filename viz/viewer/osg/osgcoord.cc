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

#include "./osgcoord.h"

#ifdef VLINK_ENABLE_VIEWER_OSG

#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Woverloaded-virtual"
#pragma GCC diagnostic ignored "-Wdeprecated-copy"
#endif

#include <osg/AutoTransform>
#include <osg/LineWidth>
#include <osg/MatrixTransform>
#include <osg/ShapeDrawable>
#include <osgText/Text>

#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic pop
#endif

static osg::ref_ptr<osg::Geode> createAxisArrow(const std::string& axis, float arrowLength, float arrowConeRadius,
                                                float arrowConeHeight, osg::Vec3& coneCenter, double ratio = 1.0) {
  osg::ref_ptr<osg::Geode> geode = new osg::Geode;
  osg::ref_ptr<osg::Geometry> lineGeometry = new osg::Geometry;
  osg::ref_ptr<osg::ShapeDrawable> coneGeometry = new osg::ShapeDrawable;
  osg::ref_ptr<osg::Vec3Array> vertices = new osg::Vec3Array;
  osg::ref_ptr<osg::Vec4Array> colors = new osg::Vec4Array;

  vertices->push_back(osg::Vec3(0.0f, 0.0f, 0.0f));

  osg::Vec4 coneColor;
  osg::ref_ptr<osg::Cone> cone;
  osg::Quat coneRotQuat;
  auto labelOffsetRatio = 2.0;

  if (axis == "X") {
    coneCenter = osg::Vec3(arrowLength * labelOffsetRatio * ratio, 0.0f, 0.0f);
    coneColor = osg::Vec4(0.8157f, 0.008f, 0.0745f, 1.0f);

    vertices->push_back(coneCenter);
    colors->push_back(coneColor);

    cone = new osg::Cone(coneCenter, arrowConeRadius * ratio, arrowConeHeight * ratio);

    coneRotQuat.makeRotate(osg::PI / 2, osg::Vec3(0.0f, 1.0f, 0.0f));
    cone->setRotation(coneRotQuat);
    coneGeometry->setShape(cone);
    coneGeometry->setColor(coneColor);

    geode->setName("X");
  } else if (axis == "Y") {
    coneCenter = osg::Vec3(0.0f, arrowLength * labelOffsetRatio * ratio, 0.0f);
    coneColor = osg::Vec4(0.494f, 0.827f, 0.129f, 1.0f);

    vertices->push_back(coneCenter);
    colors->push_back(coneColor);

    cone = new osg::Cone(coneCenter, arrowConeRadius * ratio, arrowConeHeight * ratio);

    coneRotQuat.makeRotate(-osg::PI / 2, osg::Vec3(1.0f, 0.0f, 0.0f));
    cone->setRotation(coneRotQuat);
    coneGeometry->setShape(cone);
    coneGeometry->setColor(coneColor);

    geode->setName("Y");
  } else {
    coneCenter = osg::Vec3(0.0f, 0.0f, arrowLength * labelOffsetRatio * ratio);
    coneColor = osg::Vec4(0.0f, 0.5216f, 1.0f, 1.0f);

    vertices->push_back(coneCenter);
    colors->push_back(coneColor);

    cone = new osg::Cone(coneCenter, arrowConeRadius * ratio, arrowConeHeight * ratio);

    coneGeometry->setShape(cone);
    coneGeometry->setColor(coneColor);

    geode->setName("Z");
  }

  osg::ref_ptr<osg::LineWidth> lw = new osg::LineWidth(2.0 * ratio);

  lineGeometry->getOrCreateStateSet()->setAttributeAndModes(lw);
  lineGeometry->setVertexArray(vertices);
  lineGeometry->setColorArray(colors, osg::Array::BIND_OVERALL);
  lineGeometry->addPrimitiveSet(new osg::DrawArrays(GL_LINES, 0, vertices->size()));
  lineGeometry->setUseVertexBufferObjects(true);
  coneGeometry->setUseVertexBufferObjects(true);

  geode->addDrawable(lineGeometry);
  geode->addDrawable(coneGeometry);

  osg::ref_ptr<osg::Uniform> hoveredUniform = new osg::Uniform("Hovered", false);

  geode->getOrCreateStateSet()->addUniform(hoveredUniform);
  geode->setUserData(hoveredUniform);

  coneCenter *= 1.5;

  return geode;
}

static osg::ref_ptr<osg::AutoTransform> createLabel(const std::string& txt, osg::Vec3 pos,
                                                    osg::ref_ptr<osgText::Font> font = nullptr, double ratio = 1.0) {
  osg::ref_ptr<osgText::Text> text = new osgText::Text;

  if (font.valid()) {
    text->setFont(font);
  }

  text->setCharacterSize(17 * ratio);
  text->setText(txt);
  text->setAlignment(osgText::Text::CENTER_CENTER);

  if (txt == "X") {
    text->setColor(osg::Vec4(0.8157f, 0.008f, 0.0745f, 1.0f));
  } else if (txt == "Y") {
    text->setColor(osg::Vec4(0.494f, 0.827f, 0.129f, 1.0f));
  } else {
    text->setColor(osg::Vec4(0.0f, 0.5216f, 1.0f, 1.0f));
  }

  osg::ref_ptr<osg::Geode> textGeode = new osg::Geode;

  textGeode->addDrawable(text);
  textGeode->getOrCreateStateSet()->setMode(GL_LIGHTING, osg::StateAttribute::OFF);

  osg::ref_ptr<osg::AutoTransform> at = new osg::AutoTransform;

  at->addChild(textGeode);
  at->setAutoRotateMode(osg::AutoTransform::ROTATE_TO_SCREEN);
  at->setAutoScaleToScreen(true);
  at->setPosition(pos);

  return at;
}

static osg::ref_ptr<osg::MatrixTransform> createCoord(float size, osg::ref_ptr<osgText::Font> font = nullptr,
                                                      double ratio = 1.0) {
  float arrowLength = size;
  float arrowConeRadius = size / 8.0;
  float arrowConeHeight = size / 2.0;

  osg::ref_ptr<osg::MatrixTransform> datum = new osg::MatrixTransform;
  osg::ref_ptr<osg::Group> arrowsGroup = new osg::Group;
  osg::ref_ptr<osg::Group> labelGroup = new osg::Group;

  osg::Vec3 labelPos;
  arrowsGroup->addChild(createAxisArrow("X", arrowLength, arrowConeRadius, arrowConeHeight, labelPos, ratio));

  auto labelX = createLabel("X", labelPos, font, ratio);
  osg::ref_ptr<osg::Uniform> textColor = new osg::Uniform("MaterialColor", osg::Vec4(0.313, 0.313, 0.313, 1.0));
  osg::ref_ptr<osg::Uniform> textTexture = new osg::Uniform("GlyphTexture", 0);

  auto stateSet = labelX->getOrCreateStateSet();

  stateSet->addUniform(textColor);
  stateSet->addUniform(textTexture);
  labelGroup->addChild(labelX);
  arrowsGroup->addChild(createAxisArrow("Y", arrowLength, arrowConeRadius, arrowConeHeight, labelPos, ratio));

  auto labelY = createLabel("Y", labelPos, font, ratio);

  textTexture = new osg::Uniform("GlyphTexture", 0);

  stateSet = labelY->getOrCreateStateSet();
  stateSet->addUniform(textColor);
  stateSet->addUniform(textTexture);
  labelGroup->addChild(labelY);
  arrowsGroup->addChild(createAxisArrow("Z", arrowLength, arrowConeRadius, arrowConeHeight, labelPos, ratio));

  auto labelZ = createLabel("Z", labelPos, font, ratio);

  textTexture = new osg::Uniform("GlyphTexture", 0);

  stateSet = labelZ->getOrCreateStateSet();
  stateSet->addUniform(textColor);
  stateSet->addUniform(textTexture);
  labelGroup->addChild(labelZ);

  osg::ref_ptr<osg::LineWidth> lineWidth = new osg::LineWidth;

  lineWidth->setWidth(3.0f * ratio);
  stateSet = arrowsGroup->getOrCreateStateSet();
  stateSet->setAttributeAndModes(lineWidth);

  datum->addChild(arrowsGroup);
  datum->addChild(labelGroup);

  return datum;
}

OsgCoord::OsgCoord(osg::Camera* camera, osg::ref_ptr<osgText::Font> font, double ratio)
    : m_camera(camera), m_font(font) {
  setNodeMask(0xfffffffe);
  setViewport(0.0, 0.0, 150.0 * ratio, 150.0 * ratio);
  setProjectionMatrixAsPerspective(45, 1.0, 0.001, 100000);
  setComputeNearFarMode(osg::CullSettings::COMPUTE_NEAR_FAR_USING_BOUNDING_VOLUMES);
  setViewMatrixAsLookAt(osg::Vec3d(0, 0, 20 * ratio), osg::Vec3d(0, 0, 0), osg::Vec3d(0, 1, 0));
  setRenderOrder(osg::Camera::POST_RENDER, 1000);
  setAllowEventFocus(false);
  setReferenceFrame(osg::Transform::ABSOLUTE_RF);
  getOrCreateStateSet()->setMode(GL_LIGHTING, osg::StateAttribute::OFF);
  getOrCreateStateSet()->setMode(GL_BLEND, osg::StateAttribute::ON);

  setClearMask(GL_DEPTH_BUFFER_BIT);

  m_axis = createCoord(2.2, m_font, ratio);
}

OsgCoord::~OsgCoord() {}

OsgCoord* OsgCoord::create(osg::Camera* camera, osg::ref_ptr<osgText::Font> font, double ratio) {
  return new OsgCoord(camera, font, ratio);
}

void OsgCoord::traverse(osg::NodeVisitor& nv) {
  if (m_camera.valid() && nv.getVisitorType() == osg::NodeVisitor::CULL_VISITOR) {
    osg::Matrix matrix = m_camera->getViewMatrix();

    if (matrix.valid()) {
      matrix.setTrans(osg::Vec3(0, 0, 0));
      m_axis->setMatrix(matrix);
    }
  }

  m_axis->accept(nv);

  osg::Camera::traverse(nv);
}

#endif

// NOLINTEND
