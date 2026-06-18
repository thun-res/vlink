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

#include "./osghdmap.h"

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

namespace OsgHdMap {

static osg::Vec4d color_from_rgb(uint32_t rgb, double alpha = 1.0) {
  return osg::Vec4d(((rgb >> 16) & 0xFF) / 255.0, ((rgb >> 8) & 0xFF) / 255.0, (rgb & 0xFF) / 255.0, alpha);
}

static uint32_t get_element_color(int element_type) {
  if (element_type == 1) {
    return 0xFFFF88;
  }

  if (element_type == 2) {
    return 0x884488;
  }

  if (element_type == 3) {
    return 0xFF8844;
  }

  if (element_type == 4) {
    return 0xFF4444;
  }

  return 0xFFFFFF;
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
    ss->setAttributeAndModes(new osg::PolygonOffset(-2.0f, -2.0f), osg::StateAttribute::ON);

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

  {
    osg::ref_ptr<osg::Geometry> geometry = new osg::Geometry();
    osg::ref_ptr<osg::Vec3dArray> vertices = new osg::Vec3dArray();
    osg::ref_ptr<osg::Vec4dArray> colors = new osg::Vec4dArray();

    geometry->setUseVertexBufferObjects(true);
    geometry->setVertexArray(vertices);
    geometry->setColorArray(colors, osg::Array::BIND_PER_VERTEX);
    geometry->addPrimitiveSet(new osg::DrawArrays(osg::PrimitiveSet::LINES, 0, 0));

    osg::ref_ptr<osg::LineWidth> lw = new osg::LineWidth(1.0f);
    osg::StateSet* ss = geometry->getOrCreateStateSet();
    ss->setAttribute(lw);
    ss->setMode(GL_BLEND, osg::StateAttribute::ON);
    ss->setMode(GL_LIGHTING, osg::StateAttribute::OFF);
    ss->setMode(GL_LINE_SMOOTH, osg::StateAttribute::ON);
    ss->setAttributeAndModes(new osg::PolygonOffset(-2.0f, -2.0f), osg::StateAttribute::ON);

    geode->addDrawable(geometry);
  }

  return geode;
}

void update(osg::Geode* geode, const std::vector<MapElement>& elements, float line_width) {
  if (!geode || geode->getNumDrawables() < 3) {
    return;
  }

  auto* line_geo = static_cast<osg::Geometry*>(geode->getDrawable(0));
  auto* line_verts = static_cast<osg::Vec3dArray*>(line_geo->getVertexArray());
  auto* line_colors = static_cast<osg::Vec4dArray*>(line_geo->getColorArray());

  auto* fill_geo = static_cast<osg::Geometry*>(geode->getDrawable(1));
  auto* fill_verts = static_cast<osg::Vec3dArray*>(fill_geo->getVertexArray());
  auto* fill_colors = static_cast<osg::Vec4dArray*>(fill_geo->getColorArray());

  auto* hatch_geo = static_cast<osg::Geometry*>(geode->getDrawable(2));
  auto* hatch_verts = static_cast<osg::Vec3dArray*>(hatch_geo->getVertexArray());
  auto* hatch_colors = static_cast<osg::Vec4dArray*>(hatch_geo->getColorArray());

  {
    auto* lw =
        dynamic_cast<osg::LineWidth*>(line_geo->getOrCreateStateSet()->getAttribute(osg::StateAttribute::LINEWIDTH));

    if (lw && lw->getWidth() != line_width) {
      lw->setWidth(line_width);
    }
  }

  line_verts->clear();
  line_colors->clear();
  fill_verts->clear();
  fill_colors->clear();
  hatch_verts->clear();
  hatch_colors->clear();

  size_t total_line_verts = 0;
  size_t total_fill_verts = 0;
  size_t total_hatch_verts = 0;

  for (const auto& elem : elements) {
    if (elem.points.size() < 2) {
      continue;
    }

    if (elem.element_type == 0) {
      total_line_verts += (elem.points.size() - 1) * 2;
    }

    if (elem.element_type == 3) {
      total_line_verts += (elem.points.size() - 1) * 2;
    }

    if (elem.element_type == 4) {
      total_line_verts += (elem.points.size() - 1) * 2;
      total_fill_verts += (elem.points.size() - 1) * 18;
    }

    if (elem.element_type == 1 && elem.points.size() >= 3) {
      total_line_verts += elem.points.size() * 2;
      total_fill_verts += (elem.points.size() - 1) * 60;
    }

    if (elem.element_type == 2 && elem.points.size() >= 3) {
      total_fill_verts += (elem.points.size() - 2) * 3;
      total_line_verts += elem.points.size() * 2;
      total_hatch_verts += 200;
    }
  }

  line_verts->reserve(total_line_verts);
  line_colors->reserve(total_line_verts);
  fill_verts->reserve(total_fill_verts);
  fill_colors->reserve(total_fill_verts);
  hatch_verts->reserve(total_hatch_verts);
  hatch_colors->reserve(total_hatch_verts);

  static constexpr double kGroundOffset = 0.01;
  static constexpr double kStripeWidth = 0.3;
  static constexpr double kStripeGap = 0.2;
  static constexpr double kStripeSpacing = kStripeWidth + kStripeGap;
  static constexpr double kCrosswalkHalf = 0.4;
  static constexpr double kCrossHatchSpacing = 1.0;
  static constexpr double kBumpHeight = 0.15;
  static constexpr double kBumpSpacing = 0.4;
  static constexpr double kBumpHalfBase = 0.12;
  [[maybe_unused]] static constexpr double kPi = 3.14159265358979323846;

  for (const auto& elem : elements) {
    if (elem.points.size() < 2) {
      continue;
    }

    uint32_t c = elem.color != 0 ? elem.color : get_element_color(elem.element_type);

    if (elem.element_type == 0) {
      osg::Vec4d color = color_from_rgb(c, 0.6);

      for (size_t i = 0; i + 1 < elem.points.size(); ++i) {
        line_verts->push_back(osg::Vec3d(elem.points[i].x, elem.points[i].y, elem.points[i].z + kGroundOffset));
        line_verts->push_back(
            osg::Vec3d(elem.points[i + 1].x, elem.points[i + 1].y, elem.points[i + 1].z + kGroundOffset));
        line_colors->push_back(color);
        line_colors->push_back(color);
      }
    }

    if (elem.element_type == 3) {
      osg::Vec4d color = color_from_rgb(0xFF6633, 0.95);

      for (size_t i = 0; i + 1 < elem.points.size(); ++i) {
        line_verts->push_back(osg::Vec3d(elem.points[i].x, elem.points[i].y, elem.points[i].z + kGroundOffset + 0.005));
        line_verts->push_back(
            osg::Vec3d(elem.points[i + 1].x, elem.points[i + 1].y, elem.points[i + 1].z + kGroundOffset + 0.005));
        line_colors->push_back(color);
        line_colors->push_back(color);
      }
    }

    if (elem.element_type == 4) {
      osg::Vec4d color = color_from_rgb(c, 0.9);

      for (size_t i = 0; i + 1 < elem.points.size(); ++i) {
        double dx = elem.points[i + 1].x - elem.points[i].x;
        double dy = elem.points[i + 1].y - elem.points[i].y;
        double seg_len = std::sqrt(dx * dx + dy * dy);

        if (seg_len < 1e-9) {
          continue;
        }

        line_verts->push_back(osg::Vec3d(elem.points[i].x, elem.points[i].y, elem.points[i].z + kGroundOffset));
        line_verts->push_back(
            osg::Vec3d(elem.points[i + 1].x, elem.points[i + 1].y, elem.points[i + 1].z + kGroundOffset));
        line_colors->push_back(color);
        line_colors->push_back(color);

        double ux = dx / seg_len;
        double uy = dy / seg_len;
        double perp_x = -uy;
        double perp_y = ux;

        osg::Vec4d bump_color = color_from_rgb(0xFFCC00, 0.7);
        double dist = kBumpSpacing * 0.5;

        while (dist < seg_len) {
          double t = dist / seg_len;
          double bx = elem.points[i].x + dx * t;
          double by = elem.points[i].y + dy * t;
          double bz = elem.points[i].z + (elem.points[i + 1].z - elem.points[i].z) * t + kGroundOffset;

          osg::Vec3d base_left(bx + perp_x * kBumpHalfBase, by + perp_y * kBumpHalfBase, bz);
          osg::Vec3d base_right(bx - perp_x * kBumpHalfBase, by - perp_y * kBumpHalfBase, bz);
          osg::Vec3d peak(bx, by, bz + kBumpHeight);

          osg::Vec3d front_left(bx + ux * kBumpHalfBase + perp_x * kBumpHalfBase,
                                by + uy * kBumpHalfBase + perp_y * kBumpHalfBase, bz);
          osg::Vec3d front_right(bx + ux * kBumpHalfBase - perp_x * kBumpHalfBase,
                                 by + uy * kBumpHalfBase - perp_y * kBumpHalfBase, bz);
          osg::Vec3d back_left(bx - ux * kBumpHalfBase + perp_x * kBumpHalfBase,
                               by - uy * kBumpHalfBase + perp_y * kBumpHalfBase, bz);
          osg::Vec3d back_right(bx - ux * kBumpHalfBase - perp_x * kBumpHalfBase,
                                by - uy * kBumpHalfBase - perp_y * kBumpHalfBase, bz);

          fill_verts->push_back(front_left);
          fill_verts->push_back(front_right);
          fill_verts->push_back(peak);
          fill_colors->push_back(bump_color);
          fill_colors->push_back(bump_color);
          fill_colors->push_back(bump_color);

          fill_verts->push_back(back_left);
          fill_verts->push_back(back_right);
          fill_verts->push_back(peak);
          fill_colors->push_back(bump_color);
          fill_colors->push_back(bump_color);
          fill_colors->push_back(bump_color);

          fill_verts->push_back(front_left);
          fill_verts->push_back(back_left);
          fill_verts->push_back(peak);
          fill_colors->push_back(bump_color);
          fill_colors->push_back(bump_color);
          fill_colors->push_back(bump_color);

          fill_verts->push_back(front_right);
          fill_verts->push_back(back_right);
          fill_verts->push_back(peak);
          fill_colors->push_back(bump_color);
          fill_colors->push_back(bump_color);
          fill_colors->push_back(bump_color);

          fill_verts->push_back(front_left);
          fill_verts->push_back(front_right);
          fill_verts->push_back(back_right);
          fill_colors->push_back(bump_color);
          fill_colors->push_back(bump_color);
          fill_colors->push_back(bump_color);

          fill_verts->push_back(front_left);
          fill_verts->push_back(back_right);
          fill_verts->push_back(back_left);
          fill_colors->push_back(bump_color);
          fill_colors->push_back(bump_color);
          fill_colors->push_back(bump_color);

          dist += kBumpSpacing;
        }
      }
    }

    if (elem.element_type == 1 && elem.points.size() >= 3) {
      osg::Vec4d outline_color = color_from_rgb(c, 0.7);
      osg::Vec4d stripe_color(1.0, 1.0, 1.0, 0.85);
      osg::Vec4d stripe_edge_color(1.0, 1.0, 1.0, 0.5);

      for (size_t i = 0; i < elem.points.size(); ++i) {
        size_t next = (i + 1) % elem.points.size();
        line_verts->push_back(osg::Vec3d(elem.points[i].x, elem.points[i].y, elem.points[i].z + kGroundOffset));
        line_verts->push_back(
            osg::Vec3d(elem.points[next].x, elem.points[next].y, elem.points[next].z + kGroundOffset));
        line_colors->push_back(outline_color);
        line_colors->push_back(outline_color);
      }

      for (size_t i = 0; i + 1 < elem.points.size(); ++i) {
        double dx = elem.points[i + 1].x - elem.points[i].x;
        double dy = elem.points[i + 1].y - elem.points[i].y;
        double dz = elem.points[i + 1].z - elem.points[i].z;
        double seg_len = std::sqrt(dx * dx + dy * dy + dz * dz);

        if (seg_len < 1e-9) {
          continue;
        }

        double ux = dx / seg_len;
        double uy = dy / seg_len;
        double perp_x = -uy;
        double perp_y = ux;

        double next_stripe = 0.0;

        while (next_stripe <= seg_len) {
          double t0 = next_stripe / seg_len;
          double t1 = (next_stripe + kStripeWidth) / seg_len;

          if (t1 > 1.0) {
            break;
          }

          double cx0 = elem.points[i].x + dx * t0;
          double cy0 = elem.points[i].y + dy * t0;
          double cz0 = elem.points[i].z + dz * t0 + kGroundOffset;

          double cx1 = elem.points[i].x + dx * t1;
          double cy1 = elem.points[i].y + dy * t1;
          double cz1 = elem.points[i].z + dz * t1 + kGroundOffset;

          osg::Vec3d s0(cx0 + perp_x * kCrosswalkHalf, cy0 + perp_y * kCrosswalkHalf, cz0);
          osg::Vec3d s1(cx0 - perp_x * kCrosswalkHalf, cy0 - perp_y * kCrosswalkHalf, cz0);
          osg::Vec3d s2(cx1 - perp_x * kCrosswalkHalf, cy1 - perp_y * kCrosswalkHalf, cz1);
          osg::Vec3d s3(cx1 + perp_x * kCrosswalkHalf, cy1 + perp_y * kCrosswalkHalf, cz1);

          fill_verts->push_back(s0);
          fill_verts->push_back(s1);
          fill_verts->push_back(s2);
          fill_colors->push_back(stripe_color);
          fill_colors->push_back(stripe_edge_color);
          fill_colors->push_back(stripe_edge_color);

          fill_verts->push_back(s0);
          fill_verts->push_back(s2);
          fill_verts->push_back(s3);
          fill_colors->push_back(stripe_color);
          fill_colors->push_back(stripe_edge_color);
          fill_colors->push_back(stripe_color);

          hatch_verts->push_back(s0);
          hatch_verts->push_back(s1);
          hatch_colors->push_back(stripe_edge_color);
          hatch_colors->push_back(stripe_edge_color);

          hatch_verts->push_back(s2);
          hatch_verts->push_back(s3);
          hatch_colors->push_back(stripe_edge_color);
          hatch_colors->push_back(stripe_edge_color);

          next_stripe += kStripeSpacing;
        }
      }
    }

    if (elem.element_type == 2 && elem.points.size() >= 3) {
      osg::Vec4d fill_color = color_from_rgb(c, 0.15);
      osg::Vec4d outline_color = color_from_rgb(c, 0.7);
      osg::Vec4d hatch_color = color_from_rgb(c, 0.4);

      for (size_t i = 1; i + 1 < elem.points.size(); ++i) {
        fill_verts->push_back(osg::Vec3d(elem.points[0].x, elem.points[0].y, elem.points[0].z + kGroundOffset));
        fill_verts->push_back(osg::Vec3d(elem.points[i].x, elem.points[i].y, elem.points[i].z + kGroundOffset));
        fill_verts->push_back(
            osg::Vec3d(elem.points[i + 1].x, elem.points[i + 1].y, elem.points[i + 1].z + kGroundOffset));
        fill_colors->push_back(fill_color);
        fill_colors->push_back(fill_color);
        fill_colors->push_back(fill_color);
      }

      for (size_t i = 0; i < elem.points.size(); ++i) {
        size_t next = (i + 1) % elem.points.size();
        line_verts->push_back(osg::Vec3d(elem.points[i].x, elem.points[i].y, elem.points[i].z + kGroundOffset));
        line_verts->push_back(
            osg::Vec3d(elem.points[next].x, elem.points[next].y, elem.points[next].z + kGroundOffset));
        line_colors->push_back(outline_color);
        line_colors->push_back(outline_color);
      }

      double min_x = elem.points[0].x;
      double max_x = elem.points[0].x;
      double min_y = elem.points[0].y;
      double max_y = elem.points[0].y;
      double avg_z = 0.0;

      for (const auto& pt : elem.points) {
        if (pt.x < min_x) {
          min_x = pt.x;
        }

        if (pt.x > max_x) {
          max_x = pt.x;
        }

        if (pt.y < min_y) {
          min_y = pt.y;
        }

        if (pt.y > max_y) {
          max_y = pt.y;
        }

        avg_z += pt.z;
      }

      avg_z /= static_cast<double>(elem.points.size());
      double hz = avg_z + kGroundOffset + 0.002;
      double diag = std::sqrt((max_x - min_x) * (max_x - min_x) + (max_y - min_y) * (max_y - min_y));
      double cx = (min_x + max_x) * 0.5;
      double cy = (min_y + max_y) * 0.5;
      double half_diag = diag * 0.5 + kCrossHatchSpacing;

      static constexpr double kSin45 = 0.70710678118;
      static constexpr double kCos45 = 0.70710678118;

      for (double d = -half_diag; d <= half_diag; d += kCrossHatchSpacing) {
        double lx0 = cx + kCos45 * (-half_diag) - kSin45 * d;
        double ly0 = cy + kSin45 * (-half_diag) + kCos45 * d;
        double lx1 = cx + kCos45 * half_diag - kSin45 * d;
        double ly1 = cy + kSin45 * half_diag + kCos45 * d;

        hatch_verts->push_back(osg::Vec3d(lx0, ly0, hz));
        hatch_verts->push_back(osg::Vec3d(lx1, ly1, hz));
        hatch_colors->push_back(hatch_color);
        hatch_colors->push_back(hatch_color);
      }

      for (double d = -half_diag; d <= half_diag; d += kCrossHatchSpacing) {
        double lx0 = cx + kCos45 * d + kSin45 * (-half_diag);
        double ly0 = cy - kSin45 * d + kCos45 * (-half_diag);
        double lx1 = cx + kCos45 * d + kSin45 * half_diag;
        double ly1 = cy - kSin45 * d + kCos45 * half_diag;

        hatch_verts->push_back(osg::Vec3d(lx0, ly0, hz));
        hatch_verts->push_back(osg::Vec3d(lx1, ly1, hz));
        hatch_colors->push_back(hatch_color);
        hatch_colors->push_back(hatch_color);
      }
    }
  }

  line_verts->dirty();
  line_colors->dirty();
  fill_verts->dirty();
  fill_colors->dirty();
  hatch_verts->dirty();
  hatch_colors->dirty();

  auto* line_da = static_cast<osg::DrawArrays*>(line_geo->getPrimitiveSet(0));

  if (line_da) {
    line_da->setCount(line_verts->size());
  }

  auto* fill_da = static_cast<osg::DrawArrays*>(fill_geo->getPrimitiveSet(0));

  if (fill_da) {
    fill_da->setCount(fill_verts->size());
  }

  auto* hatch_da = static_cast<osg::DrawArrays*>(hatch_geo->getPrimitiveSet(0));

  if (hatch_da) {
    hatch_da->setCount(hatch_verts->size());
  }
}

}  // namespace OsgHdMap

#endif

// NOLINTEND
