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

#include "./osgtrafficlight.h"

#ifdef VLINK_ENABLE_VIEWER_OSG

#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Woverloaded-virtual"
#pragma GCC diagnostic ignored "-Wdeprecated-copy"
#endif

#include <osg/Geometry>
#include <osg/LineWidth>
#include <osg/Point>

#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic pop
#endif

namespace OsgTrafficLight {

static osg::Vec4d color_from_rgb(uint32_t rgb, double alpha = 1.0) {
  return osg::Vec4d(((rgb >> 16) & 0xFF) / 255.0, ((rgb >> 8) & 0xFF) / 255.0, (rgb & 0xFF) / 255.0, alpha);
}

uint32_t get_state_color(uint8_t state) {
  switch (state) {
    case 1:
      return 0xFF0000;
    case 2:
      return 0xFFFF00;
    case 3:
      return 0x00FF00;
    case 4:
      return 0xFFFFFF;
    default:
      return 0x888888;
  }
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
    geometry->addPrimitiveSet(new osg::DrawArrays(osg::PrimitiveSet::LINES, 0, 0));

    osg::ref_ptr<osg::LineWidth> lw = new osg::LineWidth(2.0f);
    osg::StateSet* ss = geometry->getOrCreateStateSet();
    ss->setAttribute(lw);
    ss->setMode(GL_BLEND, osg::StateAttribute::ON);
    ss->setMode(GL_LIGHTING, osg::StateAttribute::OFF);
    ss->setMode(GL_LINE_SMOOTH, osg::StateAttribute::ON);

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

    osg::ref_ptr<osg::Point> pt = new osg::Point(10.0f);
    osg::StateSet* ss = geometry->getOrCreateStateSet();
    ss->setAttribute(pt);
    ss->setMode(GL_BLEND, osg::StateAttribute::ON);
    ss->setMode(GL_LIGHTING, osg::StateAttribute::OFF);
    ss->setMode(GL_POINT_SMOOTH, osg::StateAttribute::ON);

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

    osg::ref_ptr<osg::Point> pt = new osg::Point(16.0f);
    osg::StateSet* ss = geometry->getOrCreateStateSet();
    ss->setAttribute(pt);
    ss->setMode(GL_BLEND, osg::StateAttribute::ON);
    ss->setMode(GL_LIGHTING, osg::StateAttribute::OFF);
    ss->setMode(GL_POINT_SMOOTH, osg::StateAttribute::ON);

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

    osg::ref_ptr<osg::Point> pt = new osg::Point(30.0f);
    osg::StateSet* ss = geometry->getOrCreateStateSet();
    ss->setAttribute(pt);
    ss->setMode(GL_BLEND, osg::StateAttribute::ON);
    ss->setMode(GL_LIGHTING, osg::StateAttribute::OFF);
    ss->setMode(GL_POINT_SMOOTH, osg::StateAttribute::ON);

    geode->addDrawable(geometry);
  }

  return geode;
}

void update(osg::Geode* geode, const std::vector<TrafficLightData>& lights, float point_size) {
  (void)point_size;

  if (!geode || geode->getNumDrawables() < 4) {
    return;
  }

  auto* housing_geo = static_cast<osg::Geometry*>(geode->getDrawable(0));
  auto* housing_verts = static_cast<osg::Vec3dArray*>(housing_geo->getVertexArray());
  auto* housing_colors = static_cast<osg::Vec4dArray*>(housing_geo->getColorArray());

  auto* bg_geo = static_cast<osg::Geometry*>(geode->getDrawable(1));
  auto* bg_verts = static_cast<osg::Vec3dArray*>(bg_geo->getVertexArray());
  auto* bg_colors = static_cast<osg::Vec4dArray*>(bg_geo->getColorArray());

  auto* active_geo = static_cast<osg::Geometry*>(geode->getDrawable(2));
  auto* active_verts = static_cast<osg::Vec3dArray*>(active_geo->getVertexArray());
  auto* active_colors = static_cast<osg::Vec4dArray*>(active_geo->getColorArray());

  auto* glow_geo = static_cast<osg::Geometry*>(geode->getDrawable(3));
  auto* glow_verts = static_cast<osg::Vec3dArray*>(glow_geo->getVertexArray());
  auto* glow_colors = static_cast<osg::Vec4dArray*>(glow_geo->getColorArray());

  housing_verts->clear();
  housing_colors->clear();
  bg_verts->clear();
  bg_colors->clear();
  active_verts->clear();
  active_colors->clear();
  glow_verts->clear();
  glow_colors->clear();

  size_t est = lights.size();
  housing_verts->reserve(est * 4);
  housing_colors->reserve(est * 4);
  bg_verts->reserve(est * 3);
  bg_colors->reserve(est * 3);
  active_verts->reserve(est);
  active_colors->reserve(est);
  glow_verts->reserve(est);
  glow_colors->reserve(est);

  static constexpr double kPoleOffset = 0.02;
  static constexpr double kSlotRed = 0.9;
  static constexpr double kSlotYellow = 0.6;
  static constexpr double kSlotGreen = 0.3;
  static const osg::Vec4d kDarkSlot(0.3, 0.3, 0.3, 0.5);

  for (const auto& light : lights) {
    uint32_t c = get_state_color(light.color_state);
    double alpha = static_cast<double>(light.confidence);
    osg::Vec4d pole_color(0.5, 0.5, 0.5, alpha);
    osg::Vec4d indicator_color = color_from_rgb(c, alpha);
    osg::Vec4d glow_color = color_from_rgb(c, 0.25 * alpha);

    double px = light.position[0];
    double py = light.position[1];
    double pz = light.position[2];

    housing_verts->push_back(osg::Vec3d(px, py - kPoleOffset, pz));
    housing_verts->push_back(osg::Vec3d(px, py - kPoleOffset, pz + kSlotRed + 0.15));
    housing_colors->push_back(pole_color);
    housing_colors->push_back(pole_color);

    housing_verts->push_back(osg::Vec3d(px, py + kPoleOffset, pz));
    housing_verts->push_back(osg::Vec3d(px, py + kPoleOffset, pz + kSlotRed + 0.15));
    housing_colors->push_back(pole_color);
    housing_colors->push_back(pole_color);

    osg::Vec3d pos_green(px, py, pz + kSlotGreen);
    osg::Vec3d pos_yellow(px, py, pz + kSlotYellow);
    osg::Vec3d pos_red(px, py, pz + kSlotRed);

    bg_verts->push_back(pos_red);
    bg_verts->push_back(pos_yellow);
    bg_verts->push_back(pos_green);
    bg_colors->push_back(kDarkSlot);
    bg_colors->push_back(kDarkSlot);
    bg_colors->push_back(kDarkSlot);

    int active_slot = 0;

    switch (light.color_state) {
      case 1:
        active_slot = 0;
        break;
      case 2:
        active_slot = 1;
        break;
      case 3:
        active_slot = 2;
        break;
      default:
        active_slot = 1;
        break;
    }

    osg::Vec3d active_pos = (active_slot == 0) ? pos_red : (active_slot == 1) ? pos_yellow : pos_green;

    active_verts->push_back(active_pos);
    active_colors->push_back(indicator_color);

    glow_verts->push_back(active_pos);
    glow_colors->push_back(glow_color);
  }

  housing_verts->dirty();
  housing_colors->dirty();
  bg_verts->dirty();
  bg_colors->dirty();
  active_verts->dirty();
  active_colors->dirty();
  glow_verts->dirty();
  glow_colors->dirty();

  auto* housing_da = static_cast<osg::DrawArrays*>(housing_geo->getPrimitiveSet(0));

  if (housing_da) {
    housing_da->setCount(housing_verts->size());
  }

  auto* bg_da = static_cast<osg::DrawArrays*>(bg_geo->getPrimitiveSet(0));

  if (bg_da) {
    bg_da->setCount(bg_verts->size());
  }

  auto* active_da = static_cast<osg::DrawArrays*>(active_geo->getPrimitiveSet(0));

  if (active_da) {
    active_da->setCount(active_verts->size());
  }

  auto* glow_da = static_cast<osg::DrawArrays*>(glow_geo->getPrimitiveSet(0));

  if (glow_da) {
    glow_da->setCount(glow_verts->size());
  }
}

}  // namespace OsgTrafficLight

#endif

// NOLINTEND
