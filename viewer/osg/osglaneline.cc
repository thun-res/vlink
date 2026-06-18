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

#include "./osglaneline.h"

#ifdef VLINK_ENABLE_VIEWER_OSG

#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Woverloaded-virtual"
#pragma GCC diagnostic ignored "-Wdeprecated-copy"
#endif

#include <osg/BlendFunc>
#include <osg/Geometry>
#include <osg/LineWidth>
#include <osg/PolygonOffset>

#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic pop
#endif

#include <cmath>

namespace OsgLaneLine {

static constexpr uint32_t kLaneColors[] = {
    0x00FF00, 0x00FFFF, 0xFFFF00, 0xFF00FF, 0xFF8800, 0x8888FF, 0xFF4488, 0x88FF88,
    0xFFAA00, 0x00AAFF, 0xAAFF00, 0xFF44FF, 0x44FFAA, 0xAA44FF, 0xFFAA88, 0x88AAFF,
};
static constexpr size_t kLaneColorCount = sizeof(kLaneColors) / sizeof(kLaneColors[0]);

static osg::Vec4d color_from_rgb(uint32_t rgb, double alpha = 1.0) {
  return osg::Vec4d(((rgb >> 16) & 0xFF) / 255.0, ((rgb >> 8) & 0xFF) / 255.0, (rgb & 0xFF) / 255.0, alpha);
}

uint32_t get_lane_color(size_t index) { return kLaneColors[index % kLaneColorCount]; }

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
    ss->setAttributeAndModes(new osg::PolygonOffset(-1.0f, -1.0f), osg::StateAttribute::ON);

    geode->addDrawable(geometry);
  }

  return geode;
}

void update(osg::Geode* geode, const std::vector<LaneData>& lanes, float line_width) {
  if (!geode || geode->getNumDrawables() < 2) {
    return;
  }

  auto* line_geo = static_cast<osg::Geometry*>(geode->getDrawable(0));
  auto* line_verts = static_cast<osg::Vec3dArray*>(line_geo->getVertexArray());
  auto* line_colors = static_cast<osg::Vec4dArray*>(line_geo->getColorArray());

  auto* arrow_geo = static_cast<osg::Geometry*>(geode->getDrawable(1));
  auto* arrow_verts = static_cast<osg::Vec3dArray*>(arrow_geo->getVertexArray());
  auto* arrow_colors = static_cast<osg::Vec4dArray*>(arrow_geo->getColorArray());

  {
    auto* lw =
        dynamic_cast<osg::LineWidth*>(line_geo->getOrCreateStateSet()->getAttribute(osg::StateAttribute::LINEWIDTH));

    if (lw && lw->getWidth() != line_width) {
      lw->setWidth(line_width);
    }
  }

  line_verts->clear();
  line_colors->clear();
  arrow_verts->clear();
  arrow_colors->clear();

  size_t total_segs = 0;

  for (const auto& lane : lanes) {
    if (lane.points.size() > 1) {
      total_segs += lane.points.size() - 1;
    }
  }

  line_verts->reserve(total_segs * 2);
  line_colors->reserve(total_segs * 2);
  arrow_verts->reserve(total_segs);
  arrow_colors->reserve(total_segs);

  static constexpr double kGroundOffset = 0.03;
  static constexpr double kDashLen = 2.0;
  static constexpr double kGapLen = 2.0;
  static constexpr double kDotLen = 0.5;
  static constexpr double kDotGap = 0.5;
  static constexpr double kDoubleOffset = 0.15;
  static constexpr double kArrowInterval = 5.0;
  static constexpr double kArrowSize = 0.4;

  for (size_t lane_idx = 0; lane_idx < lanes.size(); ++lane_idx) {
    const auto& lane = lanes[lane_idx];

    if (lane.points.size() < 2) {
      continue;
    }

    uint32_t c = lane.color != 0 ? lane.color : get_lane_color(lane_idx);
    double line_alpha = 1.0;

    if (lane.lane_type == 1) {
      line_alpha = 0.85;
    } else if (lane.lane_type >= 3) {
      line_alpha = 0.7;
    }

    osg::Vec4d color = color_from_rgb(c, line_alpha);
    osg::Vec4d arrow_color = color_from_rgb(c, 0.8);

    double cumulative_dist = 0.0;
    double next_arrow_dist = kArrowInterval;
    double dash_accum = 0.0;
    bool dash_on = true;

    for (size_t i = 0; i + 1 < lane.points.size(); ++i) {
      double dx = lane.points[i + 1].x - lane.points[i].x;
      double dy = lane.points[i + 1].y - lane.points[i].y;
      double dz = lane.points[i + 1].z - lane.points[i].z;
      double seg_len = std::sqrt(dx * dx + dy * dy + dz * dz);

      if (seg_len < 1e-9) {
        continue;
      }

      if (lane.lane_type == 1) {
        double remaining = seg_len;
        double t_start = 0.0;

        while (remaining > 1e-9) {
          double budget = dash_on ? (kDashLen - dash_accum) : (kGapLen - dash_accum);
          double consume = std::min(budget, remaining);

          if (dash_on) {
            double t_end = t_start + consume / seg_len;
            osg::Vec3d p0(lane.points[i].x + dx * t_start, lane.points[i].y + dy * t_start,
                          lane.points[i].z + dz * t_start + kGroundOffset);
            osg::Vec3d p1(lane.points[i].x + dx * t_end, lane.points[i].y + dy * t_end,
                          lane.points[i].z + dz * t_end + kGroundOffset);
            line_verts->push_back(p0);
            line_verts->push_back(p1);
            line_colors->push_back(color);
            line_colors->push_back(color);
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
      } else if (lane.lane_type == 2) {
        double nx = dx / seg_len;
        double ny = dy / seg_len;
        double px = -ny * kDoubleOffset;
        double py = nx * kDoubleOffset;

        line_verts->push_back(
            osg::Vec3d(lane.points[i].x + px, lane.points[i].y + py, lane.points[i].z + kGroundOffset));
        line_verts->push_back(
            osg::Vec3d(lane.points[i + 1].x + px, lane.points[i + 1].y + py, lane.points[i + 1].z + kGroundOffset));
        line_colors->push_back(color);
        line_colors->push_back(color);

        line_verts->push_back(
            osg::Vec3d(lane.points[i].x - px, lane.points[i].y - py, lane.points[i].z + kGroundOffset));
        line_verts->push_back(
            osg::Vec3d(lane.points[i + 1].x - px, lane.points[i + 1].y - py, lane.points[i + 1].z + kGroundOffset));
        line_colors->push_back(color);
        line_colors->push_back(color);
      } else if (lane.lane_type >= 3) {
        double remaining = seg_len;
        double t_start = 0.0;

        while (remaining > 1e-9) {
          double budget = dash_on ? (kDotLen - dash_accum) : (kDotGap - dash_accum);
          double consume = std::min(budget, remaining);

          if (dash_on) {
            double t_end = t_start + consume / seg_len;
            osg::Vec3d p0(lane.points[i].x + dx * t_start, lane.points[i].y + dy * t_start,
                          lane.points[i].z + dz * t_start + kGroundOffset);
            osg::Vec3d p1(lane.points[i].x + dx * t_end, lane.points[i].y + dy * t_end,
                          lane.points[i].z + dz * t_end + kGroundOffset);
            line_verts->push_back(p0);
            line_verts->push_back(p1);
            line_colors->push_back(color);
            line_colors->push_back(color);
          }

          t_start += consume / seg_len;
          remaining -= consume;
          dash_accum += consume;

          if (dash_on && dash_accum >= kDotLen) {
            dash_on = false;
            dash_accum = 0.0;
          } else if (!dash_on && dash_accum >= kDotGap) {
            dash_on = true;
            dash_accum = 0.0;
          }
        }
      } else {
        line_verts->push_back(osg::Vec3d(lane.points[i].x, lane.points[i].y, lane.points[i].z + kGroundOffset));
        line_verts->push_back(
            osg::Vec3d(lane.points[i + 1].x, lane.points[i + 1].y, lane.points[i + 1].z + kGroundOffset));
        line_colors->push_back(color);
        line_colors->push_back(color);
      }

      {
        static constexpr double kRibbonHalf = 0.08;
        double nx = dx / seg_len;
        double ny = dy / seg_len;
        double px = -ny * kRibbonHalf;
        double py = nx * kRibbonHalf;
        double z0 = lane.points[i].z + kGroundOffset - 0.001;
        double z1 = lane.points[i + 1].z + kGroundOffset - 0.001;
        osg::Vec4d ribbon_color = color_from_rgb(c, line_alpha * 0.2);

        osg::Vec3d a(lane.points[i].x + px, lane.points[i].y + py, z0);
        osg::Vec3d b(lane.points[i].x - px, lane.points[i].y - py, z0);
        osg::Vec3d cc(lane.points[i + 1].x - px, lane.points[i + 1].y - py, z1);
        osg::Vec3d d(lane.points[i + 1].x + px, lane.points[i + 1].y + py, z1);

        arrow_verts->push_back(a);
        arrow_verts->push_back(b);
        arrow_verts->push_back(cc);
        arrow_colors->push_back(ribbon_color);
        arrow_colors->push_back(ribbon_color);
        arrow_colors->push_back(ribbon_color);

        arrow_verts->push_back(a);
        arrow_verts->push_back(cc);
        arrow_verts->push_back(d);
        arrow_colors->push_back(ribbon_color);
        arrow_colors->push_back(ribbon_color);
        arrow_colors->push_back(ribbon_color);
      }

      double new_cumulative = cumulative_dist + seg_len;

      while (next_arrow_dist <= new_cumulative) {
        double t = (next_arrow_dist - cumulative_dist) / seg_len;
        double ax = lane.points[i].x + dx * t;
        double ay = lane.points[i].y + dy * t;
        double az = lane.points[i].z + dz * t;

        double nx = dx / seg_len;
        double ny = dy / seg_len;
        double perp_x = -ny;
        double perp_y = nx;

        osg::Vec3d tip(ax + nx * kArrowSize * 0.5, ay + ny * kArrowSize * 0.5, az + kGroundOffset);
        osg::Vec3d left(ax - nx * kArrowSize * 0.5 + perp_x * kArrowSize * 0.3,
                        ay - ny * kArrowSize * 0.5 + perp_y * kArrowSize * 0.3, az + kGroundOffset);
        osg::Vec3d right(ax - nx * kArrowSize * 0.5 - perp_x * kArrowSize * 0.3,
                         ay - ny * kArrowSize * 0.5 - perp_y * kArrowSize * 0.3, az + kGroundOffset);

        arrow_verts->push_back(tip);
        arrow_verts->push_back(left);
        arrow_verts->push_back(right);
        arrow_colors->push_back(arrow_color);
        arrow_colors->push_back(arrow_color);
        arrow_colors->push_back(arrow_color);

        next_arrow_dist += kArrowInterval;
      }

      cumulative_dist = new_cumulative;
    }
  }

  line_verts->dirty();
  line_colors->dirty();
  arrow_verts->dirty();
  arrow_colors->dirty();

  auto* line_da = static_cast<osg::DrawArrays*>(line_geo->getPrimitiveSet(0));

  if (line_da) {
    line_da->setCount(line_verts->size());
  }

  auto* arrow_da = static_cast<osg::DrawArrays*>(arrow_geo->getPrimitiveSet(0));

  if (arrow_da) {
    arrow_da->setCount(arrow_verts->size());
  }
}

}  // namespace OsgLaneLine

#endif

// NOLINTEND
