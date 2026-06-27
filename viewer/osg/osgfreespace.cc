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

#include "./osgfreespace.h"

#ifdef VLINK_ENABLE_VIEWER_OSG

#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Woverloaded-virtual"
#pragma GCC diagnostic ignored "-Wdeprecated-copy"
#endif

#include <osg/BlendFunc>
#include <osg/Depth>
#include <osg/Geometry>
#include <osg/LineWidth>
#include <osg/Point>
#include <osg/PolygonOffset>

#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic pop
#endif

namespace OsgFreespace {

static osg::Vec4d color_from_rgb(uint32_t rgb, double alpha = 1.0) {
  return osg::Vec4d(((rgb >> 16) & 0xFF) / 255.0, ((rgb >> 8) & 0xFF) / 255.0, (rgb & 0xFF) / 255.0, alpha);
}

osg::ref_ptr<osg::Geode> create() {
  osg::ref_ptr<osg::Geode> geode = new osg::Geode();

  {
    osg::ref_ptr<osg::Geometry> geometry = new osg::Geometry();
    osg::ref_ptr<osg::Vec3dArray> vertices = new osg::Vec3dArray();
    osg::ref_ptr<osg::Vec4dArray> colors = new osg::Vec4dArray();

    geometry->setUseVertexBufferObjects(true);
    geometry->setVertexArray(vertices);
    geometry->setColorArray(colors, osg::Array::BIND_PER_VERTEX);
    geometry->addPrimitiveSet(new osg::DrawArrays(osg::PrimitiveSet::TRIANGLES, 0, 0));

    osg::StateSet* ss = geometry->getOrCreateStateSet();
    ss->setMode(GL_BLEND, osg::StateAttribute::ON);
    ss->setMode(GL_LIGHTING, osg::StateAttribute::OFF);
    ss->setMode(GL_CULL_FACE, osg::StateAttribute::OFF);
    ss->setAttributeAndModes(new osg::BlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA), osg::StateAttribute::ON);
    ss->setAttributeAndModes(new osg::Depth(osg::Depth::LESS, 0.0, 1.0, false), osg::StateAttribute::ON);
    ss->setAttributeAndModes(new osg::PolygonOffset(-2.0f, -2.0f), osg::StateAttribute::ON);
    ss->setRenderingHint(osg::StateSet::TRANSPARENT_BIN);

    geode->addDrawable(geometry);
  }

  {
    osg::ref_ptr<osg::Geometry> geometry = new osg::Geometry();
    osg::ref_ptr<osg::Vec3dArray> vertices = new osg::Vec3dArray();
    osg::ref_ptr<osg::Vec4dArray> colors = new osg::Vec4dArray();

    geometry->setUseVertexBufferObjects(true);
    geometry->setVertexArray(vertices);
    geometry->setColorArray(colors, osg::Array::BIND_PER_VERTEX);
    geometry->addPrimitiveSet(new osg::DrawArrays(osg::PrimitiveSet::LINES, 0, 0));

    osg::ref_ptr<osg::LineWidth> lw = new osg::LineWidth(2.5f);
    osg::StateSet* ss = geometry->getOrCreateStateSet();
    ss->setAttribute(lw);
    ss->setMode(GL_BLEND, osg::StateAttribute::ON);
    ss->setMode(GL_LIGHTING, osg::StateAttribute::OFF);
    ss->setMode(GL_LINE_SMOOTH, osg::StateAttribute::ON);
    ss->setAttributeAndModes(new osg::PolygonOffset(-1.0f, -1.0f), osg::StateAttribute::ON);

    geode->addDrawable(geometry);
  }

  {
    osg::ref_ptr<osg::Geometry> geometry = new osg::Geometry();
    osg::ref_ptr<osg::Vec3dArray> vertices = new osg::Vec3dArray();
    osg::ref_ptr<osg::Vec4dArray> colors = new osg::Vec4dArray();

    geometry->setUseVertexBufferObjects(true);
    geometry->setVertexArray(vertices);
    geometry->setColorArray(colors, osg::Array::BIND_PER_VERTEX);
    geometry->addPrimitiveSet(new osg::DrawArrays(osg::PrimitiveSet::POINTS, 0, 0));

    osg::ref_ptr<osg::Point> pt = new osg::Point(3.0f);
    osg::StateSet* ss = geometry->getOrCreateStateSet();
    ss->setAttribute(pt);
    ss->setMode(GL_BLEND, osg::StateAttribute::ON);
    ss->setMode(GL_LIGHTING, osg::StateAttribute::OFF);
    ss->setMode(GL_POINT_SMOOTH, osg::StateAttribute::ON);
    ss->setRenderingHint(osg::StateSet::TRANSPARENT_BIN);

    geode->addDrawable(geometry);
  }

  return geode;
}

void update(osg::Geode* geode, const std::vector<FreespaceData>& areas, float alpha) {
  if (!geode || geode->getNumDrawables() < 3) {
    return;
  }

  auto* fill_geo = static_cast<osg::Geometry*>(geode->getDrawable(0));
  auto* fill_verts = static_cast<osg::Vec3dArray*>(fill_geo->getVertexArray());
  auto* fill_colors = static_cast<osg::Vec4dArray*>(fill_geo->getColorArray());

  auto* line_geo = static_cast<osg::Geometry*>(geode->getDrawable(1));
  auto* line_verts = static_cast<osg::Vec3dArray*>(line_geo->getVertexArray());
  auto* line_colors = static_cast<osg::Vec4dArray*>(line_geo->getColorArray());

  auto* point_geo = static_cast<osg::Geometry*>(geode->getDrawable(2));
  auto* point_verts = static_cast<osg::Vec3dArray*>(point_geo->getVertexArray());
  auto* point_colors = static_cast<osg::Vec4dArray*>(point_geo->getColorArray());

  fill_verts->clear();
  fill_colors->clear();
  line_verts->clear();
  line_colors->clear();
  point_verts->clear();
  point_colors->clear();

  size_t total_fill = 0;
  size_t total_outline = 0;
  size_t total_points = 0;

  for (const auto& area : areas) {
    if (area.polygon.size() >= 3) {
      total_fill += area.polygon.size() * 3;
      total_outline += area.polygon.size() * 2;
      total_points += area.polygon.size();
    }
  }

  fill_verts->reserve(total_fill);
  fill_colors->reserve(total_fill);
  line_verts->reserve(total_outline);
  line_colors->reserve(total_outline);
  point_verts->reserve(total_points);
  point_colors->reserve(total_points);

  static constexpr double kFillOffset = 0.01;
  static constexpr double kOutlineOffset = 0.02;

  for (const auto& area : areas) {
    if (area.polygon.size() < 3) {
      continue;
    }

    uint32_t c = area.color != 0 ? area.color : 0x00CC00;
    osg::Vec4d center_color = color_from_rgb(c, static_cast<double>(alpha) * 0.35);
    osg::Vec4d edge_color = color_from_rgb(c, static_cast<double>(alpha) * 0.55);
    osg::Vec4d outline_color = color_from_rgb(c, 0.9);
    osg::Vec4d caution_color(1.0, 1.0, 0.0, 0.5);

    double cx = 0.0;
    double cy = 0.0;
    double cz = 0.0;

    for (const auto& pt : area.polygon) {
      cx += pt.x;
      cy += pt.y;
      cz += pt.z;
    }

    cx /= static_cast<double>(area.polygon.size());
    cy /= static_cast<double>(area.polygon.size());
    cz /= static_cast<double>(area.polygon.size());

    osg::Vec3d centroid(cx, cy, cz + kFillOffset);

    for (size_t i = 0; i < area.polygon.size(); ++i) {
      size_t next = (i + 1) % area.polygon.size();
      osg::Vec3d v0(area.polygon[i].x, area.polygon[i].y, area.polygon[i].z + kFillOffset);
      osg::Vec3d v1(area.polygon[next].x, area.polygon[next].y, area.polygon[next].z + kFillOffset);

      fill_verts->push_back(centroid);
      fill_verts->push_back(v0);
      fill_verts->push_back(v1);
      fill_colors->push_back(center_color);
      fill_colors->push_back(edge_color);
      fill_colors->push_back(edge_color);

      osg::Vec3d o0(area.polygon[i].x, area.polygon[i].y, area.polygon[i].z + kOutlineOffset);
      osg::Vec3d o1(area.polygon[next].x, area.polygon[next].y, area.polygon[next].z + kOutlineOffset);

      line_verts->push_back(o0);
      line_verts->push_back(o1);
      line_colors->push_back(outline_color);
      line_colors->push_back(outline_color);

      point_verts->push_back(o0);
      point_colors->push_back(caution_color);
    }
  }

  fill_verts->dirty();
  fill_colors->dirty();
  line_verts->dirty();
  line_colors->dirty();
  point_verts->dirty();
  point_colors->dirty();

  auto* fill_da = static_cast<osg::DrawArrays*>(fill_geo->getPrimitiveSet(0));

  if (fill_da) {
    fill_da->setCount(fill_verts->size());
  }

  auto* line_da = static_cast<osg::DrawArrays*>(line_geo->getPrimitiveSet(0));

  if (line_da) {
    line_da->setCount(line_verts->size());
  }

  auto* point_da = static_cast<osg::DrawArrays*>(point_geo->getPrimitiveSet(0));

  if (point_da) {
    point_da->setCount(point_verts->size());
  }
}

}  // namespace OsgFreespace

#endif

// NOLINTEND
