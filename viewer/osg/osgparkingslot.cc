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

#include "./osgparkingslot.h"

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

#include <cmath>

namespace OsgParkingSlot {

static osg::Vec4d color_from_rgb(uint32_t rgb, double alpha = 1.0) {
  return osg::Vec4d(((rgb >> 16) & 0xFF) / 255.0, ((rgb >> 8) & 0xFF) / 255.0, (rgb & 0xFF) / 255.0, alpha);
}

uint32_t get_slot_color(uint32_t slot_type) {
  if (slot_type == 1) {
    return 0x00FF88;
  }

  if (slot_type == 2) {
    return 0xFFAA00;
  }

  return 0x00AAFF;
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
    geometry->addPrimitiveSet(new osg::DrawArrays(osg::PrimitiveSet::TRIANGLES, 0, 0));

    osg::StateSet* ss = geometry->getOrCreateStateSet();
    ss->setMode(GL_BLEND, osg::StateAttribute::ON);
    ss->setMode(GL_LIGHTING, osg::StateAttribute::OFF);
    ss->setMode(GL_CULL_FACE, osg::StateAttribute::OFF);
    ss->setAttributeAndModes(new osg::BlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA), osg::StateAttribute::ON);
    ss->setAttributeAndModes(new osg::Depth(osg::Depth::LESS, 0.0, 1.0, false), osg::StateAttribute::ON);
    ss->setAttributeAndModes(new osg::PolygonOffset(-1.0f, -1.0f), osg::StateAttribute::ON);
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

    osg::ref_ptr<osg::LineWidth> lw = new osg::LineWidth(4.0f);
    osg::StateSet* ss = geometry->getOrCreateStateSet();
    ss->setAttribute(lw);
    ss->setMode(GL_BLEND, osg::StateAttribute::ON);
    ss->setMode(GL_LIGHTING, osg::StateAttribute::OFF);
    ss->setMode(GL_LINE_SMOOTH, osg::StateAttribute::ON);
    ss->setAttributeAndModes(new osg::PolygonOffset(-1.0f, -1.0f), osg::StateAttribute::ON);

    geode->addDrawable(geometry);
  }

  return geode;
}

void update(osg::Geode* geode, const std::vector<SlotData>& slots, float line_width) {
  if (!geode || geode->getNumDrawables() < 3) {
    return;
  }

  auto* outline_geo = static_cast<osg::Geometry*>(geode->getDrawable(0));
  auto* outline_verts = static_cast<osg::Vec3dArray*>(outline_geo->getVertexArray());
  auto* outline_colors = static_cast<osg::Vec4dArray*>(outline_geo->getColorArray());

  auto* fill_geo = static_cast<osg::Geometry*>(geode->getDrawable(1));
  auto* fill_verts = static_cast<osg::Vec3dArray*>(fill_geo->getVertexArray());
  auto* fill_colors = static_cast<osg::Vec4dArray*>(fill_geo->getColorArray());

  auto* entry_geo = static_cast<osg::Geometry*>(geode->getDrawable(2));
  auto* entry_verts = static_cast<osg::Vec3dArray*>(entry_geo->getVertexArray());
  auto* entry_colors = static_cast<osg::Vec4dArray*>(entry_geo->getColorArray());

  {
    auto* lw =
        dynamic_cast<osg::LineWidth*>(outline_geo->getOrCreateStateSet()->getAttribute(osg::StateAttribute::LINEWIDTH));

    if (lw && lw->getWidth() != line_width) {
      lw->setWidth(line_width);
    }
  }

  outline_verts->clear();
  outline_colors->clear();
  fill_verts->clear();
  fill_colors->clear();
  entry_verts->clear();
  entry_colors->clear();

  static constexpr int kCornerDotSegs = 8;
  static constexpr double kCornerDotRadius = 0.08;
  static constexpr double kCentroidDotRadius = 0.12;
  static constexpr double kArrowLen = 0.4;
  static constexpr double kArrowHalfW = 0.12;
  static constexpr double kDashLen = 1.0;
  static constexpr double kGapLen = 0.5;
  static constexpr double kPi = 3.14159265358979323846;

  outline_verts->reserve(slots.size() * 48);
  outline_colors->reserve(slots.size() * 48);
  fill_verts->reserve(slots.size() * (6 + kCornerDotSegs * 3 * 5 + 3));
  fill_colors->reserve(slots.size() * (6 + kCornerDotSegs * 3 * 5 + 3));
  entry_verts->reserve(slots.size() * 2);
  entry_colors->reserve(slots.size() * 2);

  static constexpr double kGroundOffset = 0.02;

  for (const auto& slot : slots) {
    uint32_t c = slot.color != 0 ? slot.color : get_slot_color(slot.slot_type);
    double conf_alpha = 0.3 + 0.7 * static_cast<double>(slot.confidence);
    osg::Vec4d line_color = color_from_rgb(c, conf_alpha);
    osg::Vec4d fill_color = color_from_rgb(c, 0.2 * conf_alpha);
    osg::Vec4d bright_color = color_from_rgb(c, 1.0);
    osg::Vec4d dot_color = color_from_rgb(0xFFFFFF, 0.9);

    osg::Vec3d c0(slot.corners[0][0], slot.corners[0][1], slot.corners[0][2] + kGroundOffset);
    osg::Vec3d c1(slot.corners[1][0], slot.corners[1][1], slot.corners[1][2] + kGroundOffset);
    osg::Vec3d c2(slot.corners[2][0], slot.corners[2][1], slot.corners[2][2] + kGroundOffset);
    osg::Vec3d c3(slot.corners[3][0], slot.corners[3][1], slot.corners[3][2] + kGroundOffset);

    entry_verts->push_back(c0);
    entry_verts->push_back(c1);
    entry_colors->push_back(bright_color);
    entry_colors->push_back(bright_color);

    osg::Vec3d edges_from[3] = {c1, c2, c3};
    osg::Vec3d edges_to[3] = {c2, c3, c0};

    for (int e = 0; e < 3; ++e) {
      double dx = edges_to[e].x() - edges_from[e].x();
      double dy = edges_to[e].y() - edges_from[e].y();
      double dz = edges_to[e].z() - edges_from[e].z();
      double seg_len = std::sqrt(dx * dx + dy * dy + dz * dz);

      if (seg_len < 1e-9) {
        continue;
      }

      double dash_accum = 0.0;
      bool dash_on = true;
      double remaining = seg_len;
      double t_start = 0.0;

      while (remaining > 1e-9) {
        double budget = dash_on ? (kDashLen - dash_accum) : (kGapLen - dash_accum);
        double consume = std::min(budget, remaining);

        if (dash_on) {
          double t_end = t_start + consume / seg_len;
          osg::Vec3d p0(edges_from[e].x() + dx * t_start, edges_from[e].y() + dy * t_start,
                        edges_from[e].z() + dz * t_start);
          osg::Vec3d p1(edges_from[e].x() + dx * t_end, edges_from[e].y() + dy * t_end, edges_from[e].z() + dz * t_end);
          outline_verts->push_back(p0);
          outline_verts->push_back(p1);
          outline_colors->push_back(line_color);
          outline_colors->push_back(line_color);
        }

        t_start += consume / seg_len;
        remaining -= consume;
        dash_accum += consume;

        if (dash_on && dash_accum >= kDashLen) {
          dash_on = false;
          dash_accum = 0.0;
        } else if (!dash_on && dash_accum >= kGapLen) {
          dash_on = true;
          dash_accum = 0.0;
        }
      }
    }

    fill_verts->push_back(c0);
    fill_verts->push_back(c1);
    fill_verts->push_back(c2);
    fill_colors->push_back(fill_color);
    fill_colors->push_back(fill_color);
    fill_colors->push_back(fill_color);

    fill_verts->push_back(c0);
    fill_verts->push_back(c2);
    fill_verts->push_back(c3);
    fill_colors->push_back(fill_color);
    fill_colors->push_back(fill_color);
    fill_colors->push_back(fill_color);

    osg::Vec3d centroid = (c0 + c1 + c2 + c3) * 0.25;
    osg::Vec4d centroid_color = color_from_rgb(c, 1.0);

    for (int i = 0; i < kCornerDotSegs; ++i) {
      double a0 = kPi * 2.0 * static_cast<double>(i) / static_cast<double>(kCornerDotSegs);
      double a1 = kPi * 2.0 * static_cast<double>(i + 1) / static_cast<double>(kCornerDotSegs);

      fill_verts->push_back(centroid);
      fill_verts->push_back(osg::Vec3d(centroid.x() + std::cos(a0) * kCentroidDotRadius,
                                       centroid.y() + std::sin(a0) * kCentroidDotRadius, centroid.z()));
      fill_verts->push_back(osg::Vec3d(centroid.x() + std::cos(a1) * kCentroidDotRadius,
                                       centroid.y() + std::sin(a1) * kCentroidDotRadius, centroid.z()));
      fill_colors->push_back(centroid_color);
      fill_colors->push_back(centroid_color);
      fill_colors->push_back(centroid_color);
    }

    osg::Vec3d corners[4] = {c0, c1, c2, c3};

    for (int ci = 0; ci < 4; ++ci) {
      for (int i = 0; i < kCornerDotSegs; ++i) {
        double a0 = kPi * 2.0 * static_cast<double>(i) / static_cast<double>(kCornerDotSegs);
        double a1 = kPi * 2.0 * static_cast<double>(i + 1) / static_cast<double>(kCornerDotSegs);

        fill_verts->push_back(corners[ci]);
        fill_verts->push_back(osg::Vec3d(corners[ci].x() + std::cos(a0) * kCornerDotRadius,
                                         corners[ci].y() + std::sin(a0) * kCornerDotRadius, corners[ci].z()));
        fill_verts->push_back(osg::Vec3d(corners[ci].x() + std::cos(a1) * kCornerDotRadius,
                                         corners[ci].y() + std::sin(a1) * kCornerDotRadius, corners[ci].z()));
        fill_colors->push_back(dot_color);
        fill_colors->push_back(dot_color);
        fill_colors->push_back(dot_color);
      }
    }

    osg::Vec3d entry_mid = (c0 + c1) * 0.5;
    osg::Vec3d inward = centroid - entry_mid;
    double inward_len = inward.length();

    if (inward_len > 1e-9) {
      osg::Vec3d dir = inward * (1.0 / inward_len);
      osg::Vec3d perp(-dir.y(), dir.x(), 0.0);

      osg::Vec3d arrow_tip = entry_mid + dir * kArrowLen;
      osg::Vec3d arrow_left = entry_mid + perp * kArrowHalfW;
      osg::Vec3d arrow_right = entry_mid - perp * kArrowHalfW;

      fill_verts->push_back(arrow_tip);
      fill_verts->push_back(arrow_left);
      fill_verts->push_back(arrow_right);
      fill_colors->push_back(bright_color);
      fill_colors->push_back(bright_color);
      fill_colors->push_back(bright_color);
    }
  }

  outline_verts->dirty();
  outline_colors->dirty();
  fill_verts->dirty();
  fill_colors->dirty();
  entry_verts->dirty();
  entry_colors->dirty();

  auto* outline_da = static_cast<osg::DrawArrays*>(outline_geo->getPrimitiveSet(0));

  if (outline_da) {
    outline_da->setCount(outline_verts->size());
  }

  auto* fill_da = static_cast<osg::DrawArrays*>(fill_geo->getPrimitiveSet(0));

  if (fill_da) {
    fill_da->setCount(fill_verts->size());
  }

  auto* entry_da = static_cast<osg::DrawArrays*>(entry_geo->getPrimitiveSet(0));

  if (entry_da) {
    entry_da->setCount(entry_verts->size());
  }
}

}  // namespace OsgParkingSlot

#endif

// NOLINTEND
