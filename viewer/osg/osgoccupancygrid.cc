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

#include "./osgoccupancygrid.h"

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
#include <osg/PolygonOffset>

#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic pop
#endif

#include <algorithm>
#include <cmath>

namespace OsgOccupancyGrid {

static osg::Vec4d color_from_rgb(uint32_t rgb, double alpha = 1.0) {
  return osg::Vec4d(((rgb >> 16) & 0xFF) / 255.0, ((rgb >> 8) & 0xFF) / 255.0, (rgb & 0xFF) / 255.0, alpha);
}

static uint32_t compute_cell_color(int8_t value) {
  if (value <= 20) {
    return 0x000000;
  }

  if (value <= 40) {
    double t = std::clamp(static_cast<double>(value - 21) / 19.0, 0.0, 1.0);
    uint8_t r = 0;
    uint8_t g = static_cast<uint8_t>(120 + t * 100);
    uint8_t b = static_cast<uint8_t>(220 - t * 60);
    return (static_cast<uint32_t>(r) << 16) | (static_cast<uint32_t>(g) << 8) | b;
  }

  if (value <= 60) {
    double t = std::clamp(static_cast<double>(value - 41) / 19.0, 0.0, 1.0);
    uint8_t r = static_cast<uint8_t>(t * 255);
    uint8_t g = static_cast<uint8_t>(220 - t * 30);
    uint8_t b = static_cast<uint8_t>(160 - t * 160);
    return (static_cast<uint32_t>(r) << 16) | (static_cast<uint32_t>(g) << 8) | b;
  }

  if (value <= 80) {
    double t = std::clamp(static_cast<double>(value - 61) / 19.0, 0.0, 1.0);
    uint8_t r = 255;
    uint8_t g = static_cast<uint8_t>(190 - t * 140);
    uint8_t b = static_cast<uint8_t>(t * 30);
    return (static_cast<uint32_t>(r) << 16) | (static_cast<uint32_t>(g) << 8) | b;
  }

  double t = std::clamp(static_cast<double>(value - 81) / 19.0, 0.0, 1.0);
  uint8_t r = static_cast<uint8_t>(255 - t * 60);
  uint8_t g = static_cast<uint8_t>(50 - t * 50);
  uint8_t b = static_cast<uint8_t>(30 + t * 80);
  return (static_cast<uint32_t>(r) << 16) | (static_cast<uint32_t>(g) << 8) | b;
}

static const uint32_t* get_color_table() {
  static uint32_t table[256];
  static bool initialized = false;

  if (!initialized) {
    for (int i = 0; i < 256; ++i) {
      table[i] = compute_cell_color(static_cast<int8_t>(i));
    }

    initialized = true;
  }

  return table;
}

uint32_t get_cell_color(int8_t value) { return get_color_table()[static_cast<uint8_t>(value)]; }

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
    ss->setAttributeAndModes(new osg::BlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA), osg::StateAttribute::ON);
    ss->setAttributeAndModes(new osg::PolygonOffset(-2.0f, -2.0f), osg::StateAttribute::ON);
    ss->setAttributeAndModes(new osg::Depth(osg::Depth::LESS, 0.0, 1.0, false), osg::StateAttribute::ON);
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

    osg::ref_ptr<osg::LineWidth> lw = new osg::LineWidth(0.5f);
    osg::StateSet* ss = geometry->getOrCreateStateSet();
    ss->setAttribute(lw);
    ss->setMode(GL_BLEND, osg::StateAttribute::ON);
    ss->setMode(GL_LIGHTING, osg::StateAttribute::OFF);
    ss->setMode(GL_LINE_SMOOTH, osg::StateAttribute::ON);
    ss->setAttributeAndModes(new osg::BlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA), osg::StateAttribute::ON);
    ss->setAttributeAndModes(new osg::PolygonOffset(-1.0f, -1.0f), osg::StateAttribute::ON);
    ss->setRenderingHint(osg::StateSet::TRANSPARENT_BIN);

    geode->addDrawable(geometry);
  }

  return geode;
}

void update(osg::Geode* geode, const GridData& grid, float alpha) {
  if (!geode || geode->getNumDrawables() < 2) {
    return;
  }

  auto* cell_geo = static_cast<osg::Geometry*>(geode->getDrawable(0));
  auto* cell_verts = static_cast<osg::Vec3dArray*>(cell_geo->getVertexArray());
  auto* cell_colors = static_cast<osg::Vec4dArray*>(cell_geo->getColorArray());

  auto* border_geo = static_cast<osg::Geometry*>(geode->getDrawable(1));
  auto* border_verts = static_cast<osg::Vec3dArray*>(border_geo->getVertexArray());
  auto* border_colors = static_cast<osg::Vec4dArray*>(border_geo->getColorArray());

  cell_verts->clear();
  cell_colors->clear();
  border_verts->clear();
  border_colors->clear();

  static constexpr double kCellOffset = 0.01;
  static constexpr double kBorderOffset = 0.02;

  double res = grid.resolution;
  double oz = grid.origin_z;

  static const osg::Vec4d kBorderColor(1.0, 1.0, 1.0, 0.3);

  const uint32_t* color_table = get_color_table();

  size_t active_cells = 0;
  size_t border_cells = 0;

  for (size_t i = 0; i < grid.cells.size(); ++i) {
    int8_t val = grid.cells[i];

    if (val > 20) {
      ++active_cells;

      if (val > 50) {
        ++border_cells;
      }
    }
  }

  cell_verts->resize(active_cells * 6);
  cell_colors->resize(active_cells * 6);
  border_verts->resize(border_cells * 8);
  border_colors->resize(border_cells * 8);

  size_t ci = 0;
  size_t bi = 0;

  for (uint32_t row = 0; row < grid.height; ++row) {
    double wy = grid.origin_y + row * res;

    for (uint32_t col = 0; col < grid.width; ++col) {
      size_t idx = static_cast<size_t>(row) * grid.width + col;

      if (idx >= grid.cells.size()) {
        break;
      }

      int8_t val = grid.cells[idx];

      if (val <= 20) {
        continue;
      }

      double cell_alpha = 0.0;

      if (val <= 40) {
        cell_alpha = 0.4;
      } else if (val <= 60) {
        cell_alpha = 0.55;
      } else if (val <= 80) {
        cell_alpha = 0.7;
      } else {
        cell_alpha = 0.85;
      }

      cell_alpha *= static_cast<double>(alpha);

      uint32_t c = color_table[static_cast<uint8_t>(val)];
      osg::Vec4d color = color_from_rgb(c, cell_alpha);

      double wx = grid.origin_x + col * res;

      osg::Vec3d v0(wx, wy, oz + kCellOffset);
      osg::Vec3d v1(wx + res, wy, oz + kCellOffset);
      osg::Vec3d v2(wx + res, wy + res, oz + kCellOffset);
      osg::Vec3d v3(wx, wy + res, oz + kCellOffset);

      (*cell_verts)[ci] = v0;
      (*cell_verts)[ci + 1] = v1;
      (*cell_verts)[ci + 2] = v2;
      (*cell_colors)[ci] = color;
      (*cell_colors)[ci + 1] = color;
      (*cell_colors)[ci + 2] = color;

      (*cell_verts)[ci + 3] = v0;
      (*cell_verts)[ci + 4] = v2;
      (*cell_verts)[ci + 5] = v3;
      (*cell_colors)[ci + 3] = color;
      (*cell_colors)[ci + 4] = color;
      (*cell_colors)[ci + 5] = color;

      ci += 6;

      if (val > 50) {
        osg::Vec3d b0(wx, wy, oz + kBorderOffset);
        osg::Vec3d b1(wx + res, wy, oz + kBorderOffset);
        osg::Vec3d b2(wx + res, wy + res, oz + kBorderOffset);
        osg::Vec3d b3(wx, wy + res, oz + kBorderOffset);

        (*border_verts)[bi] = b0;
        (*border_verts)[bi + 1] = b1;
        (*border_colors)[bi] = kBorderColor;
        (*border_colors)[bi + 1] = kBorderColor;

        (*border_verts)[bi + 2] = b1;
        (*border_verts)[bi + 3] = b2;
        (*border_colors)[bi + 2] = kBorderColor;
        (*border_colors)[bi + 3] = kBorderColor;

        (*border_verts)[bi + 4] = b2;
        (*border_verts)[bi + 5] = b3;
        (*border_colors)[bi + 4] = kBorderColor;
        (*border_colors)[bi + 5] = kBorderColor;

        (*border_verts)[bi + 6] = b3;
        (*border_verts)[bi + 7] = b0;
        (*border_colors)[bi + 6] = kBorderColor;
        (*border_colors)[bi + 7] = kBorderColor;

        bi += 8;
      }
    }
  }

  cell_verts->dirty();
  cell_colors->dirty();
  border_verts->dirty();
  border_colors->dirty();

  auto* cell_da = static_cast<osg::DrawArrays*>(cell_geo->getPrimitiveSet(0));

  if (cell_da) {
    cell_da->setCount(cell_verts->size());
  }

  auto* border_da = static_cast<osg::DrawArrays*>(border_geo->getPrimitiveSet(0));

  if (border_da) {
    border_da->setCount(border_verts->size());
  }
}

}  // namespace OsgOccupancyGrid

#endif

// NOLINTEND
