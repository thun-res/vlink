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

#include "./osgtrafficsign.h"

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

#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic pop
#endif

namespace OsgTrafficSign {

static constexpr uint32_t kSignColors[] = {
    0x00FFFF, 0x00FF00, 0xFFFF00, 0x0088FF, 0xFF4444, 0xFF00FF, 0xFF8800, 0xFFFFFF,
    0x88FF88, 0x8888FF, 0xFF88FF, 0xFFFF88, 0x88FFFF, 0xAAAA00, 0x00AAAA, 0xAA00AA,
};
static constexpr size_t kSignColorCount = sizeof(kSignColors) / sizeof(kSignColors[0]);

static osg::Vec4d color_from_rgb(uint32_t rgb, double alpha = 1.0) {
  return osg::Vec4d(((rgb >> 16) & 0xFF) / 255.0, ((rgb >> 8) & 0xFF) / 255.0, (rgb & 0xFF) / 255.0, alpha);
}

uint32_t get_sign_color(uint32_t type_id) { return kSignColors[type_id % kSignColorCount]; }

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
    geometry->addPrimitiveSet(new osg::DrawArrays(osg::PrimitiveSet::LINES, 0, 0));

    osg::ref_ptr<osg::LineWidth> lw = new osg::LineWidth(2.5f);
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
    geometry->addPrimitiveSet(new osg::DrawArrays(osg::PrimitiveSet::TRIANGLES, 0, 0));

    osg::StateSet* ss = geometry->getOrCreateStateSet();
    ss->setMode(GL_BLEND, osg::StateAttribute::ON);
    ss->setMode(GL_LIGHTING, osg::StateAttribute::OFF);
    ss->setMode(GL_CULL_FACE, osg::StateAttribute::OFF);
    ss->setAttributeAndModes(new osg::BlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA), osg::StateAttribute::ON);
    ss->setAttributeAndModes(new osg::Depth(osg::Depth::LESS, 0.0, 1.0, false), osg::StateAttribute::ON);
    ss->setRenderingHint(osg::StateSet::TRANSPARENT_BIN);

    geode->addDrawable(geometry);
  }

  return geode;
}

void update(osg::Geode* geode, const std::vector<TrafficSignData>& signs, float line_width) {
  if (!geode || geode->getNumDrawables() < 3) {
    return;
  }

  auto* pole_geo = static_cast<osg::Geometry*>(geode->getDrawable(0));
  auto* pole_verts = static_cast<osg::Vec3dArray*>(pole_geo->getVertexArray());
  auto* pole_colors = static_cast<osg::Vec4dArray*>(pole_geo->getColorArray());

  auto* outline_geo = static_cast<osg::Geometry*>(geode->getDrawable(1));
  auto* outline_verts = static_cast<osg::Vec3dArray*>(outline_geo->getVertexArray());
  auto* outline_colors = static_cast<osg::Vec4dArray*>(outline_geo->getColorArray());

  auto* fill_geo = static_cast<osg::Geometry*>(geode->getDrawable(2));
  auto* fill_verts = static_cast<osg::Vec3dArray*>(fill_geo->getVertexArray());
  auto* fill_colors = static_cast<osg::Vec4dArray*>(fill_geo->getColorArray());

  {
    auto* lw =
        dynamic_cast<osg::LineWidth*>(pole_geo->getOrCreateStateSet()->getAttribute(osg::StateAttribute::LINEWIDTH));

    if (lw && lw->getWidth() != line_width) {
      lw->setWidth(line_width);
    }
  }

  pole_verts->clear();
  pole_colors->clear();
  outline_verts->clear();
  outline_colors->clear();
  fill_verts->clear();
  fill_colors->clear();

  size_t est = signs.size();
  pole_verts->reserve(est * 4);
  pole_colors->reserve(est * 4);
  outline_verts->reserve(est * 8);
  outline_colors->reserve(est * 8);
  fill_verts->reserve(est * 12);
  fill_colors->reserve(est * 12);

  static constexpr double kPoleHeight = 2.0;
  static constexpr double kDefaultBoardHalf = 0.3;
  static constexpr double kPoleSpread = 0.015;

  for (const auto& sign : signs) {
    uint32_t c = sign.color != 0 ? sign.color : get_sign_color(sign.type_id);
    osg::Vec4d pole_color(0.5, 0.5, 0.5, 1.0);
    osg::Vec4d wire_color = color_from_rgb(c, 1.0);
    osg::Vec4d fill_color = color_from_rgb(c, 0.4);

    double px = sign.position[0];
    double py = sign.position[1];
    double pz = sign.position[2];
    double board_half = (sign.marker_size > 0.1) ? (sign.marker_size / 2.0) : kDefaultBoardHalf;
    double board_z = pz + kPoleHeight;

    pole_verts->push_back(osg::Vec3d(px, py - kPoleSpread, pz));
    pole_verts->push_back(osg::Vec3d(px, py - kPoleSpread, board_z));
    pole_colors->push_back(pole_color);
    pole_colors->push_back(pole_color);

    pole_verts->push_back(osg::Vec3d(px, py + kPoleSpread, pz));
    pole_verts->push_back(osg::Vec3d(px, py + kPoleSpread, board_z));
    pole_colors->push_back(pole_color);
    pole_colors->push_back(pole_color);

    osg::Vec3d tl(px, py - board_half, board_z + board_half);
    osg::Vec3d tr(px, py + board_half, board_z + board_half);
    osg::Vec3d br(px, py + board_half, board_z - board_half);
    osg::Vec3d bl(px, py - board_half, board_z - board_half);

    outline_verts->push_back(tl);
    outline_verts->push_back(tr);
    outline_colors->push_back(wire_color);
    outline_colors->push_back(wire_color);

    outline_verts->push_back(tr);
    outline_verts->push_back(br);
    outline_colors->push_back(wire_color);
    outline_colors->push_back(wire_color);

    outline_verts->push_back(br);
    outline_verts->push_back(bl);
    outline_colors->push_back(wire_color);
    outline_colors->push_back(wire_color);

    outline_verts->push_back(bl);
    outline_verts->push_back(tl);
    outline_colors->push_back(wire_color);
    outline_colors->push_back(wire_color);

    fill_verts->push_back(tl);
    fill_verts->push_back(tr);
    fill_verts->push_back(br);
    fill_colors->push_back(fill_color);
    fill_colors->push_back(fill_color);
    fill_colors->push_back(fill_color);

    fill_verts->push_back(tl);
    fill_verts->push_back(br);
    fill_verts->push_back(bl);
    fill_colors->push_back(fill_color);
    fill_colors->push_back(fill_color);
    fill_colors->push_back(fill_color);

    static constexpr double kInnerMargin = 0.04;
    osg::Vec4d inner_border = color_from_rgb(c, 0.15);

    osg::Vec3d itl(px, py - board_half + kInnerMargin, board_z + board_half - kInnerMargin);
    osg::Vec3d itr(px, py + board_half - kInnerMargin, board_z + board_half - kInnerMargin);
    osg::Vec3d ibr(px, py + board_half - kInnerMargin, board_z - board_half + kInnerMargin);
    osg::Vec3d ibl(px, py - board_half + kInnerMargin, board_z - board_half + kInnerMargin);

    fill_verts->push_back(itl);
    fill_verts->push_back(itr);
    fill_verts->push_back(ibr);
    fill_colors->push_back(inner_border);
    fill_colors->push_back(inner_border);
    fill_colors->push_back(inner_border);

    fill_verts->push_back(itl);
    fill_verts->push_back(ibr);
    fill_verts->push_back(ibl);
    fill_colors->push_back(inner_border);
    fill_colors->push_back(inner_border);
    fill_colors->push_back(inner_border);
  }

  pole_verts->dirty();
  pole_colors->dirty();
  outline_verts->dirty();
  outline_colors->dirty();
  fill_verts->dirty();
  fill_colors->dirty();

  auto* pole_da = static_cast<osg::DrawArrays*>(pole_geo->getPrimitiveSet(0));

  if (pole_da) {
    pole_da->setCount(pole_verts->size());
  }

  auto* outline_da = static_cast<osg::DrawArrays*>(outline_geo->getPrimitiveSet(0));

  if (outline_da) {
    outline_da->setCount(outline_verts->size());
  }

  auto* fill_da = static_cast<osg::DrawArrays*>(fill_geo->getPrimitiveSet(0));

  if (fill_da) {
    fill_da->setCount(fill_verts->size());
  }
}

}  // namespace OsgTrafficSign

#endif

// NOLINTEND
