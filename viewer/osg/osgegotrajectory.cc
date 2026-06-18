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

#include "./osgegotrajectory.h"

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

namespace OsgEgoTrajectory {

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
    geometry->addPrimitiveSet(new osg::DrawArrays(osg::PrimitiveSet::LINES, 0, 0));

    osg::ref_ptr<osg::LineWidth> lw = new osg::LineWidth(3.0f);
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

  return geode;
}

void update(osg::Geode* geode, const std::vector<TrajectoryData>& trajectories, float line_width) {
  if (!geode || geode->getNumDrawables() < 3) {
    return;
  }

  auto* line_geo = static_cast<osg::Geometry*>(geode->getDrawable(0));
  auto* line_verts = static_cast<osg::Vec3dArray*>(line_geo->getVertexArray());
  auto* line_colors = static_cast<osg::Vec4dArray*>(line_geo->getColorArray());

  auto* arrow_geo = static_cast<osg::Geometry*>(geode->getDrawable(1));
  auto* arrow_verts = static_cast<osg::Vec3dArray*>(arrow_geo->getVertexArray());
  auto* arrow_colors = static_cast<osg::Vec4dArray*>(arrow_geo->getColorArray());

  auto* ribbon_geo = static_cast<osg::Geometry*>(geode->getDrawable(2));
  auto* ribbon_verts = static_cast<osg::Vec3dArray*>(ribbon_geo->getVertexArray());
  auto* ribbon_colors = static_cast<osg::Vec4dArray*>(ribbon_geo->getColorArray());

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
  ribbon_verts->clear();
  ribbon_colors->clear();

  size_t total_segs = 0;

  for (const auto& traj : trajectories) {
    if (traj.points.size() > 1) {
      total_segs += traj.points.size() - 1;
    }
  }

  static constexpr int kStartCircleSegs = 10;
  static constexpr double kStartCircleRadius = 0.25;
  static constexpr double kEndCrossSize = 0.25;

  line_verts->reserve(total_segs * 4 + trajectories.size() * 8);
  line_colors->reserve(total_segs * 4 + trajectories.size() * 8);
  arrow_verts->reserve(total_segs * 3 + trajectories.size() * (kStartCircleSegs * 3 + 3));
  arrow_colors->reserve(total_segs * 3 + trajectories.size() * (kStartCircleSegs * 3 + 3));
  ribbon_verts->reserve(total_segs * 6);
  ribbon_colors->reserve(total_segs * 6);

  static constexpr double kGroundOffset = 0.04;
  static constexpr double kArrowInterval = 3.0;
  static constexpr double kArrowSize = 0.3;
  static constexpr double kDashLen = 2.0;
  static constexpr double kGapLen = 1.0;
  static constexpr double kMaxRibbonHalf = 0.5;
  static constexpr double kSpeedScale = 0.03;
  static constexpr double kSlowSpeed = 5.0;
  static constexpr double kFastSpeed = 15.0;
  static constexpr double kTimeMarkerInterval = 1.0;
  static constexpr double kPi = 3.14159265358979323846;

  for (const auto& traj : trajectories) {
    if (traj.points.size() < 2) {
      continue;
    }

    uint32_t c = traj.color;
    double line_alpha = 1.0;
    bool is_actual = (traj.trajectory_type == 1);

    if (traj.trajectory_type == 0) {
      if (c == 0) {
        c = 0x00FF88;
      }
    } else {
      if (c == 0) {
        c = 0x888888;
      }

      line_alpha = 0.7;
    }

    osg::Vec4d arrow_color = color_from_rgb(c, 0.9);

    double cumulative_dist = 0.0;
    double next_arrow_dist = kArrowInterval;
    double dash_accum = 0.0;
    bool dash_on = true;
    double cumulative_time = 0.0;
    double next_time_marker = kTimeMarkerInterval;

    {
      osg::Vec3d first_pt(traj.points[0].x, traj.points[0].y, traj.points[0].z + kGroundOffset);
      osg::Vec4d start_color = is_actual ? color_from_rgb(0x888888, 0.9) : color_from_rgb(0x00FF88, 0.9);

      for (int i = 0; i < kStartCircleSegs; ++i) {
        double a0 = kPi * 2.0 * static_cast<double>(i) / static_cast<double>(kStartCircleSegs);
        double a1 = kPi * 2.0 * static_cast<double>(i + 1) / static_cast<double>(kStartCircleSegs);

        arrow_verts->push_back(first_pt);
        arrow_verts->push_back(osg::Vec3d(first_pt.x() + std::cos(a0) * kStartCircleRadius,
                                          first_pt.y() + std::sin(a0) * kStartCircleRadius, first_pt.z()));
        arrow_verts->push_back(osg::Vec3d(first_pt.x() + std::cos(a1) * kStartCircleRadius,
                                          first_pt.y() + std::sin(a1) * kStartCircleRadius, first_pt.z()));
        arrow_colors->push_back(start_color);
        arrow_colors->push_back(start_color);
        arrow_colors->push_back(start_color);
      }
    }

    {
      const auto& lp = traj.points.back();
      osg::Vec3d last_pt(lp.x, lp.y, lp.z + kGroundOffset);
      osg::Vec4d end_color = color_from_rgb(0xFF4444, 0.9);

      double yaw_last = lp.yaw;
      double cos_y = std::cos(yaw_last);
      double sin_y = std::sin(yaw_last);

      osg::Vec3d d_top(last_pt.x() + sin_y * kEndCrossSize, last_pt.y() - cos_y * kEndCrossSize, last_pt.z());
      osg::Vec3d d_right(last_pt.x() + cos_y * kEndCrossSize, last_pt.y() + sin_y * kEndCrossSize, last_pt.z());
      osg::Vec3d d_bottom(last_pt.x() - sin_y * kEndCrossSize, last_pt.y() + cos_y * kEndCrossSize, last_pt.z());
      osg::Vec3d d_left(last_pt.x() - cos_y * kEndCrossSize, last_pt.y() - sin_y * kEndCrossSize, last_pt.z());

      arrow_verts->push_back(d_top);
      arrow_verts->push_back(d_right);
      arrow_verts->push_back(d_bottom);
      arrow_colors->push_back(end_color);
      arrow_colors->push_back(end_color);
      arrow_colors->push_back(end_color);

      arrow_verts->push_back(d_top);
      arrow_verts->push_back(d_bottom);
      arrow_verts->push_back(d_left);
      arrow_colors->push_back(end_color);
      arrow_colors->push_back(end_color);
      arrow_colors->push_back(end_color);
    }

    for (size_t i = 0; i + 1 < traj.points.size(); ++i) {
      double dx = traj.points[i + 1].x - traj.points[i].x;
      double dy = traj.points[i + 1].y - traj.points[i].y;
      double dz = traj.points[i + 1].z - traj.points[i].z;
      double seg_len = std::sqrt(dx * dx + dy * dy + dz * dz);

      if (seg_len < 1e-9) {
        continue;
      }

      double avg_speed = (std::abs(traj.points[i].speed) + std::abs(traj.points[i + 1].speed)) * 0.5;

      double r_val = 0.0;
      double g_val = 0.0;
      double b_val = 0.0;

      if (avg_speed < kSlowSpeed) {
        double t_spd = avg_speed / kSlowSpeed;
        r_val = t_spd;
        g_val = 1.0;
        b_val = 0.0;
      } else if (avg_speed < kFastSpeed) {
        double t_spd = (avg_speed - kSlowSpeed) / (kFastSpeed - kSlowSpeed);
        r_val = 1.0;
        g_val = 1.0 - t_spd;
        b_val = 0.0;
      } else {
        r_val = 1.0;
        g_val = 0.0;
        b_val = 0.0;
      }

      osg::Vec4d seg_color(r_val, g_val, b_val, line_alpha);

      if (is_actual) {
        double remaining = seg_len;
        double t_start = 0.0;

        while (remaining > 1e-9) {
          double budget = dash_on ? (kDashLen - dash_accum) : (kGapLen - dash_accum);
          double consume = std::min(budget, remaining);

          if (dash_on) {
            double t_end = t_start + consume / seg_len;
            osg::Vec3d p0(traj.points[i].x + dx * t_start, traj.points[i].y + dy * t_start,
                          traj.points[i].z + dz * t_start + kGroundOffset);
            osg::Vec3d p1(traj.points[i].x + dx * t_end, traj.points[i].y + dy * t_end,
                          traj.points[i].z + dz * t_end + kGroundOffset);
            line_verts->push_back(p0);
            line_verts->push_back(p1);
            line_colors->push_back(seg_color);
            line_colors->push_back(seg_color);
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
      } else {
        line_verts->push_back(osg::Vec3d(traj.points[i].x, traj.points[i].y, traj.points[i].z + kGroundOffset));
        line_verts->push_back(
            osg::Vec3d(traj.points[i + 1].x, traj.points[i + 1].y, traj.points[i + 1].z + kGroundOffset));
        line_colors->push_back(seg_color);
        line_colors->push_back(seg_color);
      }

      {
        double nx = dx / seg_len;
        double ny = dy / seg_len;
        double speed0 = std::abs(traj.points[i].speed);
        double speed1 = std::abs(traj.points[i + 1].speed);
        double half0 = std::min(speed0 * kSpeedScale, kMaxRibbonHalf);
        double half1 = std::min(speed1 * kSpeedScale, kMaxRibbonHalf);
        double px = -ny;
        double py = nx;
        double z0 = traj.points[i].z + kGroundOffset - 0.001;
        double z1 = traj.points[i + 1].z + kGroundOffset - 0.001;
        osg::Vec4d ribbon_color(r_val, g_val, b_val, 0.15);

        osg::Vec3d a(traj.points[i].x + px * half0, traj.points[i].y + py * half0, z0);
        osg::Vec3d b(traj.points[i].x - px * half0, traj.points[i].y - py * half0, z0);
        osg::Vec3d cc(traj.points[i + 1].x - px * half1, traj.points[i + 1].y - py * half1, z1);
        osg::Vec3d d(traj.points[i + 1].x + px * half1, traj.points[i + 1].y + py * half1, z1);

        ribbon_verts->push_back(a);
        ribbon_verts->push_back(b);
        ribbon_verts->push_back(cc);
        ribbon_colors->push_back(ribbon_color);
        ribbon_colors->push_back(ribbon_color);
        ribbon_colors->push_back(ribbon_color);

        ribbon_verts->push_back(a);
        ribbon_verts->push_back(cc);
        ribbon_verts->push_back(d);
        ribbon_colors->push_back(ribbon_color);
        ribbon_colors->push_back(ribbon_color);
        ribbon_colors->push_back(ribbon_color);
      }

      {
        double seg_speed = std::max(avg_speed, 0.1);
        double seg_time = seg_len / seg_speed;
        double new_time = cumulative_time + seg_time;

        while (next_time_marker <= new_time) {
          double dt = next_time_marker - cumulative_time;
          double t_frac = dt / seg_time;

          if (t_frac >= 0.0 && t_frac <= 1.0) {
            double mx = traj.points[i].x + dx * t_frac;
            double my = traj.points[i].y + dy * t_frac;
            double mz = traj.points[i].z + dz * t_frac + kGroundOffset + 0.001;
            osg::Vec4d marker_color(1.0, 1.0, 1.0, 0.9);

            line_verts->push_back(osg::Vec3d(mx - 0.08, my, mz));
            line_verts->push_back(osg::Vec3d(mx + 0.08, my, mz));
            line_colors->push_back(marker_color);
            line_colors->push_back(marker_color);

            line_verts->push_back(osg::Vec3d(mx, my - 0.08, mz));
            line_verts->push_back(osg::Vec3d(mx, my + 0.08, mz));
            line_colors->push_back(marker_color);
            line_colors->push_back(marker_color);
          }

          next_time_marker += kTimeMarkerInterval;
        }

        cumulative_time = new_time;
      }

      double new_cumulative = cumulative_dist + seg_len;

      while (next_arrow_dist <= new_cumulative) {
        double t = (next_arrow_dist - cumulative_dist) / seg_len;
        double ax = traj.points[i].x + dx * t;
        double ay = traj.points[i].y + dy * t;
        double az = traj.points[i].z + dz * t;

        double t_yaw = traj.points[i].yaw + (traj.points[i + 1].yaw - traj.points[i].yaw) * t;
        double cos_yaw = std::cos(t_yaw);
        double sin_yaw = std::sin(t_yaw);

        osg::Vec3d tip(ax + cos_yaw * kArrowSize * 0.5, ay + sin_yaw * kArrowSize * 0.5, az + kGroundOffset);
        osg::Vec3d left(ax - cos_yaw * kArrowSize * 0.5 + (-sin_yaw) * kArrowSize * 0.3,
                        ay - sin_yaw * kArrowSize * 0.5 + cos_yaw * kArrowSize * 0.3, az + kGroundOffset);
        osg::Vec3d right(ax - cos_yaw * kArrowSize * 0.5 - (-sin_yaw) * kArrowSize * 0.3,
                         ay - sin_yaw * kArrowSize * 0.5 - cos_yaw * kArrowSize * 0.3, az + kGroundOffset);

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
  ribbon_verts->dirty();
  ribbon_colors->dirty();

  auto* line_da = static_cast<osg::DrawArrays*>(line_geo->getPrimitiveSet(0));

  if (line_da) {
    line_da->setCount(line_verts->size());
  }

  auto* arrow_da = static_cast<osg::DrawArrays*>(arrow_geo->getPrimitiveSet(0));

  if (arrow_da) {
    arrow_da->setCount(arrow_verts->size());
  }

  auto* ribbon_da = static_cast<osg::DrawArrays*>(ribbon_geo->getPrimitiveSet(0));

  if (ribbon_da) {
    ribbon_da->setCount(ribbon_verts->size());
  }
}

}  // namespace OsgEgoTrajectory

#endif

// NOLINTEND
