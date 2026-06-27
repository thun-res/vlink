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

#include "./osgprediction.h"

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

#include <cmath>

namespace OsgPrediction {

static constexpr uint32_t kPredColors[] = {
    0xFF8800, 0x00AAFF, 0xFF44FF, 0xFFFF00, 0x00FF88, 0xFF4444, 0x88FFFF, 0xFFAA44,
    0xAA88FF, 0x88FF44, 0xFF88AA, 0x44AAFF, 0xFFCC00, 0x00FFCC, 0xCC44FF, 0xFF6644,
};
static constexpr size_t kPredColorCount = sizeof(kPredColors) / sizeof(kPredColors[0]);

[[maybe_unused]] static osg::Vec4d color_from_rgb(uint32_t rgb, double alpha = 1.0) {
  return osg::Vec4d(((rgb >> 16) & 0xFF) / 255.0, ((rgb >> 8) & 0xFF) / 255.0, (rgb & 0xFF) / 255.0, alpha);
}

uint32_t get_prediction_color(uint32_t track_id) { return kPredColors[track_id % kPredColorCount]; }

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
    ss->setAttributeAndModes(new osg::BlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA), osg::StateAttribute::ON);
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

  {
    osg::ref_ptr<osg::Geometry> geometry = new osg::Geometry();
    osg::ref_ptr<osg::Vec3dArray> vertices = new osg::Vec3dArray();
    osg::ref_ptr<osg::Vec4dArray> colors = new osg::Vec4dArray();

    geometry->setUseVertexBufferObjects(true);
    geometry->setVertexArray(vertices);
    geometry->setColorArray(colors, osg::Array::BIND_PER_VERTEX);
    geometry->addPrimitiveSet(new osg::DrawArrays(osg::PrimitiveSet::POINTS, 0, 0));

    osg::ref_ptr<osg::Point> point = new osg::Point(6.0f);
    osg::StateSet* ss = geometry->getOrCreateStateSet();
    ss->setAttribute(point);
    ss->setMode(GL_BLEND, osg::StateAttribute::ON);
    ss->setMode(GL_LIGHTING, osg::StateAttribute::OFF);
    ss->setMode(GL_POINT_SMOOTH, osg::StateAttribute::ON);
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
    geometry->addPrimitiveSet(new osg::DrawArrays(osg::PrimitiveSet::POINTS, 0, 0));

    osg::ref_ptr<osg::Point> point = new osg::Point(4.0f);
    osg::StateSet* ss = geometry->getOrCreateStateSet();
    ss->setAttribute(point);
    ss->setMode(GL_BLEND, osg::StateAttribute::ON);
    ss->setMode(GL_LIGHTING, osg::StateAttribute::OFF);
    ss->setMode(GL_POINT_SMOOTH, osg::StateAttribute::ON);
    ss->setRenderingHint(osg::StateSet::TRANSPARENT_BIN);

    geode->addDrawable(geometry);
  }

  return geode;
}

void update(osg::Geode* geode, const std::vector<PredictionData>& predictions, float line_width) {
  if (!geode || geode->getNumDrawables() < 4) {
    return;
  }

  auto* line_geo = static_cast<osg::Geometry*>(geode->getDrawable(0));
  auto* line_verts = static_cast<osg::Vec3dArray*>(line_geo->getVertexArray());
  auto* line_colors = static_cast<osg::Vec4dArray*>(line_geo->getColorArray());

  auto* ribbon_geo = static_cast<osg::Geometry*>(geode->getDrawable(1));
  auto* ribbon_verts = static_cast<osg::Vec3dArray*>(ribbon_geo->getVertexArray());
  auto* ribbon_colors = static_cast<osg::Vec4dArray*>(ribbon_geo->getColorArray());

  auto* point_geo = static_cast<osg::Geometry*>(geode->getDrawable(2));
  auto* point_verts = static_cast<osg::Vec3dArray*>(point_geo->getVertexArray());
  auto* point_colors = static_cast<osg::Vec4dArray*>(point_geo->getColorArray());

  auto* end_point_geo = static_cast<osg::Geometry*>(geode->getDrawable(3));
  auto* end_point_verts = static_cast<osg::Vec3dArray*>(end_point_geo->getVertexArray());
  auto* end_point_colors = static_cast<osg::Vec4dArray*>(end_point_geo->getColorArray());

  {
    auto* lw =
        dynamic_cast<osg::LineWidth*>(line_geo->getOrCreateStateSet()->getAttribute(osg::StateAttribute::LINEWIDTH));

    if (lw && lw->getWidth() != line_width) {
      lw->setWidth(line_width);
    }
  }

  line_verts->clear();
  line_colors->clear();
  ribbon_verts->clear();
  ribbon_colors->clear();
  point_verts->clear();
  point_colors->clear();
  end_point_verts->clear();
  end_point_colors->clear();

  size_t total_segs = 0;
  size_t total_preds = 0;

  for (const auto& pred : predictions) {
    if (pred.points.size() > 1) {
      total_segs += pred.points.size() - 1;
      ++total_preds;
    }
  }

  line_verts->reserve(total_segs * 2);
  line_colors->reserve(total_segs * 2);
  ribbon_verts->reserve(total_segs * 6);
  ribbon_colors->reserve(total_segs * 6);
  point_verts->reserve(total_preds);
  point_colors->reserve(total_preds);
  end_point_verts->reserve(total_preds);
  end_point_colors->reserve(total_preds);

  static constexpr double kGroundOffset = 0.05;
  static constexpr double kBaseWidth = 0.3;

  for (const auto& pred : predictions) {
    if (pred.points.size() < 2) {
      continue;
    }

    uint32_t c = pred.color != 0 ? pred.color : get_prediction_color(pred.track_id);
    double conf = static_cast<double>(pred.confidence);
    double n = static_cast<double>(pred.points.size() - 1);

    double base_r = ((c >> 16) & 0xFF) / 255.0;
    double base_g = ((c >> 8) & 0xFF) / 255.0;
    double base_b = (c & 0xFF) / 255.0;

    double cumul_dist = 0.0;
    bool is_low_conf = (conf <= 0.5);

    {
      osg::Vec3d start_pt(pred.points[0].x, pred.points[0].y, pred.points[0].z + kGroundOffset);
      osg::Vec4d start_color(std::min(base_r * 1.3, 1.0), std::min(base_g * 1.3, 1.0), std::min(base_b * 1.3, 1.0),
                             std::min(conf * 1.2, 1.0));
      point_verts->push_back(start_pt);
      point_colors->push_back(start_color);

      osg::Vec3d end_pt(pred.points.back().x, pred.points.back().y, pred.points.back().z + kGroundOffset);
      double end_alpha = conf * 0.2;
      osg::Vec4d end_color(std::min(base_r * 1.3, 1.0), std::min(base_g * 1.3, 1.0), std::min(base_b * 1.3, 1.0),
                           std::min(end_alpha * 1.2, 1.0));
      end_point_verts->push_back(end_pt);
      end_point_colors->push_back(end_color);
    }

    for (size_t i = 0; i + 1 < pred.points.size(); ++i) {
      double t1 = static_cast<double>(i) / n;
      double t2 = static_cast<double>(i + 1) / n;

      double alpha1 = conf * (1.0 - t1 * 0.8);
      double alpha2 = conf * (1.0 - t2 * 0.8);

      double r1 = base_r * (1.0 - t1 * 0.3);
      double g1 = base_g * (1.0 - t1 * 0.2);
      double b1 = base_b * (1.0 + t1 * 0.3);
      double r2 = base_r * (1.0 - t2 * 0.3);
      double g2 = base_g * (1.0 - t2 * 0.2);
      double b2 = base_b * (1.0 + t2 * 0.3);

      osg::Vec4d c1(std::min(r1, 1.0), std::min(g1, 1.0), std::min(b1, 1.0), alpha1);
      osg::Vec4d c2(std::min(r2, 1.0), std::min(g2, 1.0), std::min(b2, 1.0), alpha2);

      osg::Vec3d p0(pred.points[i].x, pred.points[i].y, pred.points[i].z + kGroundOffset);
      osg::Vec3d p1(pred.points[i + 1].x, pred.points[i + 1].y, pred.points[i + 1].z + kGroundOffset);

      double dx = p1.x() - p0.x();
      double dy = p1.y() - p0.y();
      double seg_len = std::sqrt(dx * dx + dy * dy);
      cumul_dist += seg_len;

      bool in_dash = (static_cast<int>(cumul_dist / 1.0) % 2 == 0);

      if (is_low_conf && !in_dash) {
        continue;
      }

      line_verts->push_back(p0);
      line_verts->push_back(p1);
      line_colors->push_back(c1);
      line_colors->push_back(c2);

      if (seg_len > 1e-9) {
        double nx = -dy / seg_len;
        double ny = dx / seg_len;

        double w1 = kBaseWidth * (1.0 + t1);
        double w2 = kBaseWidth * (1.0 + t2);

        osg::Vec3d l0(p0.x() + nx * w1, p0.y() + ny * w1, p0.z());
        osg::Vec3d r0(p0.x() - nx * w1, p0.y() - ny * w1, p0.z());
        osg::Vec3d l1(p1.x() + nx * w2, p1.y() + ny * w2, p1.z());
        osg::Vec3d r1_pt(p1.x() - nx * w2, p1.y() - ny * w2, p1.z());

        osg::Vec4d rc1(c1.x(), c1.y(), c1.z(), alpha1 * 0.15);
        osg::Vec4d rc2(c2.x(), c2.y(), c2.z(), alpha2 * 0.15);

        ribbon_verts->push_back(l0);
        ribbon_verts->push_back(r0);
        ribbon_verts->push_back(l1);
        ribbon_colors->push_back(rc1);
        ribbon_colors->push_back(rc1);
        ribbon_colors->push_back(rc2);

        ribbon_verts->push_back(r0);
        ribbon_verts->push_back(r1_pt);
        ribbon_verts->push_back(l1);
        ribbon_colors->push_back(rc1);
        ribbon_colors->push_back(rc2);
        ribbon_colors->push_back(rc2);
      }
    }
  }

  line_verts->dirty();
  line_colors->dirty();
  ribbon_verts->dirty();
  ribbon_colors->dirty();
  point_verts->dirty();
  point_colors->dirty();
  end_point_verts->dirty();
  end_point_colors->dirty();

  auto* line_da = static_cast<osg::DrawArrays*>(line_geo->getPrimitiveSet(0));

  if (line_da) {
    line_da->setCount(line_verts->size());
  }

  auto* ribbon_da = static_cast<osg::DrawArrays*>(ribbon_geo->getPrimitiveSet(0));

  if (ribbon_da) {
    ribbon_da->setCount(ribbon_verts->size());
  }

  auto* point_da = static_cast<osg::DrawArrays*>(point_geo->getPrimitiveSet(0));

  if (point_da) {
    point_da->setCount(point_verts->size());
  }

  auto* end_point_da = static_cast<osg::DrawArrays*>(end_point_geo->getPrimitiveSet(0));

  if (end_point_da) {
    end_point_da->setCount(end_point_verts->size());
  }
}

}  // namespace OsgPrediction

#endif

// NOLINTEND
