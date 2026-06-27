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

#include "./osgcovarianceellipse.h"

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

namespace OsgCovarianceEllipse {

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

    osg::ref_ptr<osg::LineWidth> lw = new osg::LineWidth(1.5f);
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

  return geode;
}

void update(osg::Geode* geode, const std::vector<EllipseData>& ellipses, float line_width) {
  if (!geode || geode->getNumDrawables() < 2) {
    return;
  }

  auto* outline_geo = static_cast<osg::Geometry*>(geode->getDrawable(0));
  auto* outline_verts = static_cast<osg::Vec3dArray*>(outline_geo->getVertexArray());
  auto* outline_colors = static_cast<osg::Vec4dArray*>(outline_geo->getColorArray());

  auto* fill_geo = static_cast<osg::Geometry*>(geode->getDrawable(1));
  auto* fill_verts = static_cast<osg::Vec3dArray*>(fill_geo->getVertexArray());
  auto* fill_colors = static_cast<osg::Vec4dArray*>(fill_geo->getColorArray());

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

  static constexpr int kSegments = 48;
  static constexpr double kTwoPi = 2.0 * 3.14159265358979323846;
  static constexpr double kGroundOffset = 0.03;
  static constexpr double kScaleFactor = 2.4477;
  static constexpr double kCrossHalf = 0.1;
  static constexpr double kArrowLen = 0.25;
  static constexpr double kArrowHalfW = 0.08;
  static constexpr double kElongationThreshold = 2.0;

  outline_verts->reserve(ellipses.size() * (kSegments * 2 + 12));
  outline_colors->reserve(ellipses.size() * (kSegments * 2 + 12));
  fill_verts->reserve(ellipses.size() * (kSegments * 3 + 3));
  fill_colors->reserve(ellipses.size() * (kSegments * 3 + 3));

  for (const auto& ellipse : ellipses) {
    double xx = ellipse.covariance[0];
    double xy = ellipse.covariance[1];
    double yy = ellipse.covariance[3];

    double trace = xx + yy;
    double det = xx * yy - xy * xy;
    double disc = trace * trace * 0.25 - det;

    if (disc < 0.0) {
      disc = 0.0;
    }

    double sqrt_disc = std::sqrt(disc);
    double lambda1 = trace * 0.5 + sqrt_disc;
    double lambda2 = trace * 0.5 - sqrt_disc;

    if (lambda1 < 0.0) {
      lambda1 = 0.0;
    }

    if (lambda2 < 0.0) {
      lambda2 = 0.0;
    }

    double a = std::sqrt(lambda1) * kScaleFactor;
    double b = std::sqrt(lambda2) * kScaleFactor;

    double theta = 0.0;

    if (std::abs(xy) > 1e-12) {
      theta = std::atan2(lambda1 - xx, xy);
    } else if (yy > xx) {
      theta = 3.14159265358979323846 * 0.5;
    }

    double cos_theta = std::cos(theta);
    double sin_theta = std::sin(theta);

    double alpha_val = static_cast<double>(ellipse.alpha);
    osg::Vec4d line_color = color_from_rgb(ellipse.color, 0.9);
    osg::Vec4d axis_color = color_from_rgb(ellipse.color, 1.0);
    osg::Vec4d center_fill_color = color_from_rgb(ellipse.color, alpha_val * 0.1);
    osg::Vec4d edge_fill_color = color_from_rgb(ellipse.color, alpha_val * 0.4);

    double pz = ellipse.position[2] + kGroundOffset;
    osg::Vec3d center(ellipse.position[0], ellipse.position[1], pz);

    {
      outline_verts->push_back(osg::Vec3d(center.x() - kCrossHalf, center.y(), pz));
      outline_verts->push_back(osg::Vec3d(center.x() + kCrossHalf, center.y(), pz));
      outline_colors->push_back(axis_color);
      outline_colors->push_back(axis_color);

      outline_verts->push_back(osg::Vec3d(center.x(), center.y() - kCrossHalf, pz));
      outline_verts->push_back(osg::Vec3d(center.x(), center.y() + kCrossHalf, pz));
      outline_colors->push_back(axis_color);
      outline_colors->push_back(axis_color);
    }

    {
      osg::Vec3d major_end1(center.x() + a * cos_theta, center.y() + a * sin_theta, pz);
      osg::Vec3d major_end2(center.x() - a * cos_theta, center.y() - a * sin_theta, pz);

      outline_verts->push_back(major_end1);
      outline_verts->push_back(major_end2);
      outline_colors->push_back(axis_color);
      outline_colors->push_back(axis_color);

      osg::Vec3d minor_end1(center.x() - b * sin_theta, center.y() + b * cos_theta, pz);
      osg::Vec3d minor_end2(center.x() + b * sin_theta, center.y() - b * cos_theta, pz);

      outline_verts->push_back(minor_end1);
      outline_verts->push_back(minor_end2);
      outline_colors->push_back(axis_color);
      outline_colors->push_back(axis_color);
    }

    double prev_x = a * cos_theta;
    double prev_y = a * sin_theta;

    for (int i = 1; i <= kSegments; ++i) {
      double t = kTwoPi * static_cast<double>(i) / static_cast<double>(kSegments);
      double cos_t = std::cos(t);
      double sin_t = std::sin(t);
      double cur_x = a * cos_t * cos_theta - b * sin_t * sin_theta;
      double cur_y = a * cos_t * sin_theta + b * sin_t * cos_theta;

      osg::Vec3d p0(ellipse.position[0] + prev_x, ellipse.position[1] + prev_y, pz);
      osg::Vec3d p1(ellipse.position[0] + cur_x, ellipse.position[1] + cur_y, pz);

      outline_verts->push_back(p0);
      outline_verts->push_back(p1);
      outline_colors->push_back(line_color);
      outline_colors->push_back(line_color);

      fill_verts->push_back(center);
      fill_verts->push_back(p0);
      fill_verts->push_back(p1);
      fill_colors->push_back(center_fill_color);
      fill_colors->push_back(edge_fill_color);
      fill_colors->push_back(edge_fill_color);

      prev_x = cur_x;
      prev_y = cur_y;
    }

    {
      double ratio = (b > 1e-9) ? (a / b) : 999.0;

      if (ratio > kElongationThreshold) {
        osg::Vec3d arrow_tip(center.x() + cos_theta * (a + kArrowLen), center.y() + sin_theta * (a + kArrowLen), pz);
        osg::Vec3d arrow_base_left(center.x() + cos_theta * a + (-sin_theta) * kArrowHalfW,
                                   center.y() + sin_theta * a + cos_theta * kArrowHalfW, pz);
        osg::Vec3d arrow_base_right(center.x() + cos_theta * a - (-sin_theta) * kArrowHalfW,
                                    center.y() + sin_theta * a - cos_theta * kArrowHalfW, pz);

        fill_verts->push_back(arrow_tip);
        fill_verts->push_back(arrow_base_left);
        fill_verts->push_back(arrow_base_right);
        fill_colors->push_back(axis_color);
        fill_colors->push_back(axis_color);
        fill_colors->push_back(axis_color);
      }
    }
  }

  outline_verts->dirty();
  outline_colors->dirty();
  fill_verts->dirty();
  fill_colors->dirty();

  auto* outline_da = static_cast<osg::DrawArrays*>(outline_geo->getPrimitiveSet(0));

  if (outline_da) {
    outline_da->setCount(outline_verts->size());
  }

  auto* fill_da = static_cast<osg::DrawArrays*>(fill_geo->getPrimitiveSet(0));

  if (fill_da) {
    fill_da->setCount(fill_verts->size());
  }
}

}  // namespace OsgCovarianceEllipse

#endif

// NOLINTEND
