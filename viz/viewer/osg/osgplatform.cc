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

#include "./osgplatform.h"

#ifdef VLINK_ENABLE_VIEWER_OSG

#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Woverloaded-virtual"
#pragma GCC diagnostic ignored "-Wdeprecated-copy"
#endif

#include <osg/Geode>
#include <osg/Geometry>
#include <osg/LineWidth>
#include <osg/Multisample>

#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic pop
#endif

namespace OsgPlatform {

osg::ref_ptr<osg::Node> create(double size, int count) {
  double pm = size / count;
  double min = size / 50000.0;

  osg::ref_ptr<osg::Geode> geode = new osg::Geode();

  if (size <= 0 || count <= 0) {
    return geode;
  }

  {
    osg::ref_ptr<osg::Geometry> lineGeo = new osg::Geometry();
    osg::ref_ptr<osg::Vec3Array> lineVertex = new osg::Vec3Array();
    osg::ref_ptr<osg::LineWidth> lineLength = new osg::LineWidth(1);
    osg::ref_ptr<osg::DrawArrays> linePrimitive = new osg::DrawArrays(osg::PrimitiveSet::LINES, 0, (count + 1) * 4);
    osg::ref_ptr<osg::Vec4dArray> lineColor = new osg::Vec4dArray();
    osg::ref_ptr<osg::Vec3Array> normals = new osg::Vec3Array;

    for (int x = 0; x <= count; x++) {
      lineVertex->push_back(osg::Vec3(-size / 2 + x * pm, -size / 2, 0));
      lineVertex->push_back(osg::Vec3(-size / 2 + x * pm, size / 2, 0));
    }

    for (int y = 0; y <= count; y++) {
      lineVertex->push_back(osg::Vec3(-size / 2, -size / 2 + y * pm, 0));
      lineVertex->push_back(osg::Vec3(size / 2, -size / 2 + y * pm, 0));
    }

    lineColor->push_back(osg::Vec4d(1.0, 1.0, 1.0, 0.1));
    normals->push_back(osg::Vec3(0.0f, 0.0f, 1.0f));

    lineGeo->setVertexArray(lineVertex);
    lineGeo->getOrCreateStateSet()->setAttribute(lineLength, osg::StateAttribute::ON);
    lineGeo->addPrimitiveSet(linePrimitive);
    lineGeo->setColorArray(lineColor);
    lineGeo->setColorBinding(osg::Geometry::BIND_OVERALL);
    lineGeo->setNormalArray(normals);
    lineGeo->setNormalBinding(osg::Geometry::BIND_OVERALL);
    lineGeo->getOrCreateStateSet()->setMode(GL_BLEND, osg::StateAttribute::ON);
    lineGeo->setUseVertexBufferObjects(true);

    geode->addDrawable(lineGeo);
  }
  {
    osg::ref_ptr<osg::Geometry> crossGeo = new osg::Geometry();
    osg::ref_ptr<osg::Vec3Array> crossVertex = new osg::Vec3Array();
    osg::ref_ptr<osg::LineWidth> crossWidth = new osg::LineWidth(3);
    osg::ref_ptr<osg::DrawArrays> crossPrimitive = new osg::DrawArrays(osg::PrimitiveSet::LINES, 0, 4);
    osg::ref_ptr<osg::Vec4dArray> crossColor = new osg::Vec4dArray();
    osg::ref_ptr<osg::Vec3Array> normals = new osg::Vec3Array;

    crossVertex->push_back(osg::Vec3(-pm / 10, 0, min));
    crossVertex->push_back(osg::Vec3(pm / 10, 0, min));
    crossVertex->push_back(osg::Vec3(0, -pm / 10, min));
    crossVertex->push_back(osg::Vec3(0, pm / 10, min));
    crossColor->push_back(osg::Vec4d(1.0, 1.0, 1.0, 0.5));
    normals->push_back(osg::Vec3(0.0f, 0.0f, 1.0f));

    crossGeo->setVertexArray(crossVertex);
    crossGeo->getOrCreateStateSet()->setAttribute(crossWidth, osg::StateAttribute::ON);
    crossGeo->addPrimitiveSet(crossPrimitive);
    crossGeo->setColorArray(crossColor);
    crossGeo->setColorBinding(osg::Geometry::BIND_OVERALL);
    crossGeo->setNormalArray(normals);
    crossGeo->setNormalBinding(osg::Geometry::BIND_OVERALL);
    crossGeo->getOrCreateStateSet()->setMode(GL_BLEND, osg::StateAttribute::ON);
    crossGeo->setUseVertexBufferObjects(true);

    geode->addDrawable(crossGeo);
  }
  {
    osg::ref_ptr<osg::Geometry> rectGeo = new osg::Geometry();
    osg::ref_ptr<osg::Vec3Array> rectVertex = new osg::Vec3Array();
    osg::ref_ptr<osg::DrawArrays> rectPrimitive = new osg::DrawArrays(osg::PrimitiveSet::QUADS, 0, 4);
    osg::ref_ptr<osg::Vec4dArray> rectColor = new osg::Vec4dArray();
    osg::ref_ptr<osg::Vec3Array> normals = new osg::Vec3Array;

    rectVertex->push_back(osg::Vec3(-size / 2, -size / 2, min));
    rectVertex->push_back(osg::Vec3(-size / 2, size / 2, min));
    rectVertex->push_back(osg::Vec3(size / 2, size / 2, min));
    rectVertex->push_back(osg::Vec3(size / 2, -size / 2, min));
    rectColor->push_back(osg::Vec4d(0.05, 0.05, 0, 0.5));
    normals->push_back(osg::Vec3(0.0f, 0.0f, 1.0f));

    rectGeo->setVertexArray(rectVertex);
    rectGeo->addPrimitiveSet(rectPrimitive);
    rectGeo->setColorArray(rectColor);
    rectGeo->setColorBinding(osg::Geometry::BIND_OVERALL);
    rectGeo->setNormalArray(normals);
    rectGeo->setNormalBinding(osg::Geometry::BIND_OVERALL);
    rectGeo->getOrCreateStateSet()->setMode(GL_BLEND, osg::StateAttribute::ON);
    rectGeo->getOrCreateStateSet()->setRenderBinDetails(10, "RenderBin");
    rectGeo->setUseVertexBufferObjects(true);

    geode->addDrawable(rectGeo);
  }
  {
    osg::ref_ptr<osg::Geometry> arrowGeo = new osg::Geometry();
    osg::ref_ptr<osg::Vec3Array> arrowVertex = new osg::Vec3Array();
    osg::ref_ptr<osg::DrawArrays> arrowPrimitive = new osg::DrawArrays(osg::PrimitiveSet::TRIANGLES, 0, 3);
    osg::ref_ptr<osg::Vec4dArray> arrowColor = new osg::Vec4dArray();
    osg::ref_ptr<osg::Vec3Array> normals = new osg::Vec3Array;

    arrowVertex->push_back(osg::Vec3(-size / 2, -pm, min + size * 0.001));
    arrowVertex->push_back(osg::Vec3(-size / 2 + pm * 2, 0, min + size * 0.001));
    arrowVertex->push_back(osg::Vec3(-size / 2, pm, min + size * 0.001));
    arrowColor->push_back(osg::Vec4d(1.0, 1.0, 1.0, 0.5));
    normals->push_back(osg::Vec3(0.0f, 0.0f, 1.0f));

    arrowGeo->setVertexArray(arrowVertex);
    arrowGeo->addPrimitiveSet(arrowPrimitive);
    arrowGeo->setColorArray(arrowColor);
    arrowGeo->setColorBinding(osg::Geometry::BIND_OVERALL);
    arrowGeo->setNormalArray(normals);
    arrowGeo->setNormalBinding(osg::Geometry::BIND_OVERALL);
    arrowGeo->setUseVertexBufferObjects(true);

    geode->addDrawable(arrowGeo);
  }

  return geode;
}

};  // namespace OsgPlatform

#endif

// NOLINTEND
