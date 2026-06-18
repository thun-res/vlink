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

#include "./osgstopline.h"

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

namespace OsgStopLine {

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

    osg::ref_ptr<osg::LineWidth> lw = new osg::LineWidth(4.0f);
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
    geometry->addPrimitiveSet(new osg::DrawArrays(osg::PrimitiveSet::LINES, 0, 0));

    osg::ref_ptr<osg::LineWidth> lw = new osg::LineWidth(2.0f);
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
    ss->setAttributeAndModes(new osg::PolygonOffset(-2.0f, -2.0f), osg::StateAttribute::ON);
    ss->setRenderingHint(osg::StateSet::TRANSPARENT_BIN);

    geode->addDrawable(geometry);
  }

  return geode;
}

void update(osg::Geode* geode, const std::vector<StopLineData>& lines, float line_width) {
  if (!geode || geode->getNumDrawables() < 3) {
    return;
  }

  auto* main_geo = static_cast<osg::Geometry*>(geode->getDrawable(0));
  auto* main_verts = static_cast<osg::Vec3dArray*>(main_geo->getVertexArray());
  auto* main_colors = static_cast<osg::Vec4dArray*>(main_geo->getColorArray());

  auto* edge_geo = static_cast<osg::Geometry*>(geode->getDrawable(1));
  auto* edge_verts = static_cast<osg::Vec3dArray*>(edge_geo->getVertexArray());
  auto* edge_colors = static_cast<osg::Vec4dArray*>(edge_geo->getColorArray());

  auto* zebra_geo = static_cast<osg::Geometry*>(geode->getDrawable(2));
  auto* zebra_verts = static_cast<osg::Vec3dArray*>(zebra_geo->getVertexArray());
  auto* zebra_colors = static_cast<osg::Vec4dArray*>(zebra_geo->getColorArray());

  {
    auto* lw =
        dynamic_cast<osg::LineWidth*>(main_geo->getOrCreateStateSet()->getAttribute(osg::StateAttribute::LINEWIDTH));

    if (lw && lw->getWidth() != line_width) {
      lw->setWidth(line_width);
    }
  }

  main_verts->clear();
  main_colors->clear();
  edge_verts->clear();
  edge_colors->clear();
  zebra_verts->clear();
  zebra_colors->clear();

  size_t total_segs = 0;
  size_t total_stripes = 0;

  for (const auto& line : lines) {
    if (line.points.size() > 1) {
      total_segs += line.points.size() - 1;

      if (line.line_type == 1) {
        double path_len = 0.0;

        for (size_t i = 0; i + 1 < line.points.size(); ++i) {
          double dx = line.points[i + 1].x - line.points[i].x;
          double dy = line.points[i + 1].y - line.points[i].y;
          path_len += std::sqrt(dx * dx + dy * dy);
        }

        total_stripes += static_cast<size_t>(path_len / 0.65) + 1;
      }
    }
  }

  main_verts->reserve(total_segs * 4);
  main_colors->reserve(total_segs * 4);
  edge_verts->reserve(total_segs * 4);
  edge_colors->reserve(total_segs * 4);
  zebra_verts->reserve(total_stripes * 6);
  zebra_colors->reserve(total_stripes * 6);

  static constexpr double kGroundOffset = 0.04;
  static constexpr double kStripeWidth = 0.4;
  static constexpr double kStripeGap = 0.25;
  static constexpr double kStripeSpacing = kStripeWidth + kStripeGap;
  static constexpr double kCrosswalkHalf = 0.4;

  for (const auto& line : lines) {
    if (line.points.size() < 2) {
      continue;
    }

    osg::Vec4d main_color = color_from_rgb(0xFF4444, 0.8);
    osg::Vec4d white_edge(1.0, 1.0, 1.0, 0.9);
    osg::Vec4d stripe_color(1.0, 1.0, 1.0, 0.85);
    osg::Vec4d stripe_edge_color(1.0, 1.0, 1.0, 0.5);

    for (size_t i = 0; i + 1 < line.points.size(); ++i) {
      osg::Vec3d p0(line.points[i].x, line.points[i].y, line.points[i].z + kGroundOffset);
      osg::Vec3d p1(line.points[i + 1].x, line.points[i + 1].y, line.points[i + 1].z + kGroundOffset);

      main_verts->push_back(p0);
      main_verts->push_back(p1);
      main_colors->push_back(main_color);
      main_colors->push_back(main_color);

      double dx = line.points[i + 1].x - line.points[i].x;
      double dy = line.points[i + 1].y - line.points[i].y;
      double seg_len = std::sqrt(dx * dx + dy * dy);

      if (seg_len > 1e-9) {
        double perp_x = -(dy / seg_len) * 0.08;
        double perp_y = (dx / seg_len) * 0.08;

        main_verts->push_back(osg::Vec3d(p0.x() + perp_x, p0.y() + perp_y, p0.z()));
        main_verts->push_back(osg::Vec3d(p1.x() + perp_x, p1.y() + perp_y, p1.z()));
        main_colors->push_back(white_edge);
        main_colors->push_back(white_edge);

        edge_verts->push_back(osg::Vec3d(p0.x() - perp_x, p0.y() - perp_y, p0.z()));
        edge_verts->push_back(osg::Vec3d(p1.x() - perp_x, p1.y() - perp_y, p1.z()));
        edge_colors->push_back(white_edge);
        edge_colors->push_back(white_edge);
      }
    }

    if (line.line_type == 1) {
      double cumulative = 0.0;
      double next_stripe = 0.0;

      for (size_t i = 0; i + 1 < line.points.size(); ++i) {
        double dx = line.points[i + 1].x - line.points[i].x;
        double dy = line.points[i + 1].y - line.points[i].y;
        double dz = line.points[i + 1].z - line.points[i].z;
        double seg_len = std::sqrt(dx * dx + dy * dy + dz * dz);

        if (seg_len < 1e-9) {
          continue;
        }

        double ux = dx / seg_len;
        double uy = dy / seg_len;
        double perp_x = -uy;
        double perp_y = ux;

        double new_cumulative = cumulative + seg_len;

        while (next_stripe <= new_cumulative) {
          double t0 = (next_stripe - cumulative) / seg_len;
          double t1 = (next_stripe + kStripeWidth - cumulative) / seg_len;

          if (t1 > 1.0) {
            break;
          }

          double cx0 = line.points[i].x + dx * t0;
          double cy0 = line.points[i].y + dy * t0;
          double cz0 = line.points[i].z + dz * t0 + kGroundOffset;

          double cx1 = line.points[i].x + dx * t1;
          double cy1 = line.points[i].y + dy * t1;
          double cz1 = line.points[i].z + dz * t1 + kGroundOffset;

          osg::Vec3d s0(cx0 + perp_x * kCrosswalkHalf, cy0 + perp_y * kCrosswalkHalf, cz0);
          osg::Vec3d s1(cx0 - perp_x * kCrosswalkHalf, cy0 - perp_y * kCrosswalkHalf, cz0);
          osg::Vec3d s2(cx1 - perp_x * kCrosswalkHalf, cy1 - perp_y * kCrosswalkHalf, cz1);
          osg::Vec3d s3(cx1 + perp_x * kCrosswalkHalf, cy1 + perp_y * kCrosswalkHalf, cz1);

          zebra_verts->push_back(s0);
          zebra_verts->push_back(s1);
          zebra_verts->push_back(s2);
          zebra_colors->push_back(stripe_color);
          zebra_colors->push_back(stripe_edge_color);
          zebra_colors->push_back(stripe_edge_color);

          zebra_verts->push_back(s0);
          zebra_verts->push_back(s2);
          zebra_verts->push_back(s3);
          zebra_colors->push_back(stripe_color);
          zebra_colors->push_back(stripe_edge_color);
          zebra_colors->push_back(stripe_color);

          edge_verts->push_back(s0);
          edge_verts->push_back(s1);
          edge_colors->push_back(stripe_edge_color);
          edge_colors->push_back(stripe_edge_color);

          edge_verts->push_back(s2);
          edge_verts->push_back(s3);
          edge_colors->push_back(stripe_edge_color);
          edge_colors->push_back(stripe_edge_color);

          next_stripe += kStripeSpacing;
        }

        cumulative = new_cumulative;
      }
    }
  }

  main_verts->dirty();
  main_colors->dirty();
  edge_verts->dirty();
  edge_colors->dirty();
  zebra_verts->dirty();
  zebra_colors->dirty();

  auto* main_da = static_cast<osg::DrawArrays*>(main_geo->getPrimitiveSet(0));

  if (main_da) {
    main_da->setCount(main_verts->size());
  }

  auto* edge_da = static_cast<osg::DrawArrays*>(edge_geo->getPrimitiveSet(0));

  if (edge_da) {
    edge_da->setCount(edge_verts->size());
  }

  auto* zebra_da = static_cast<osg::DrawArrays*>(zebra_geo->getPrimitiveSet(0));

  if (zebra_da) {
    zebra_da->setCount(zebra_verts->size());
  }
}

}  // namespace OsgStopLine

#endif

// NOLINTEND
