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

#include "./osgcamerafrustum.h"

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

#include <cmath>

namespace OsgCameraFrustum {

static osg::Vec4d color_from_rgb(uint32_t rgb, double alpha = 1.0) {
  return osg::Vec4d(((rgb >> 16) & 0xFF) / 255.0, ((rgb >> 8) & 0xFF) / 255.0, (rgb & 0xFF) / 255.0, alpha);
}

static osg::Vec3d rotate_by_quat(const osg::Vec3d& v, const double* q) {
  double qx = q[0];
  double qy = q[1];
  double qz = q[2];
  double qw = q[3];

  double tx = 2.0 * (qy * v.z() - qz * v.y());
  double ty = 2.0 * (qz * v.x() - qx * v.z());
  double tz = 2.0 * (qx * v.y() - qy * v.x());

  return osg::Vec3d(v.x() + qw * tx + qy * tz - qz * ty, v.y() + qw * ty + qz * tx - qx * tz,
                    v.z() + qw * tz + qx * ty - qy * tx);
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

void update(osg::Geode* geode, const std::vector<FrustumData>& frustums, float line_width) {
  if (!geode || geode->getNumDrawables() < 2) {
    return;
  }

  auto* wire_geo = static_cast<osg::Geometry*>(geode->getDrawable(0));
  auto* wire_verts = static_cast<osg::Vec3dArray*>(wire_geo->getVertexArray());
  auto* wire_colors = static_cast<osg::Vec4dArray*>(wire_geo->getColorArray());

  auto* face_geo = static_cast<osg::Geometry*>(geode->getDrawable(1));
  auto* face_verts = static_cast<osg::Vec3dArray*>(face_geo->getVertexArray());
  auto* face_colors = static_cast<osg::Vec4dArray*>(face_geo->getColorArray());

  {
    auto* lw =
        dynamic_cast<osg::LineWidth*>(wire_geo->getOrCreateStateSet()->getAttribute(osg::StateAttribute::LINEWIDTH));

    if (lw && lw->getWidth() != line_width) {
      lw->setWidth(line_width);
    }
  }

  wire_verts->clear();
  wire_colors->clear();
  face_verts->clear();
  face_colors->clear();

  static constexpr double kCubeHalf = 0.06;
  static constexpr double kCrossHalf = 0.04;

  wire_verts->reserve(frustums.size() * 60);
  wire_colors->reserve(frustums.size() * 60);
  face_verts->reserve(frustums.size() * 30);
  face_colors->reserve(frustums.size() * 30);

  static constexpr double kDegToRad = 3.14159265358979323846 / 180.0;

  for (const auto& frust : frustums) {
    [[maybe_unused]] osg::Vec4d wire_color = color_from_rgb(frust.color, 0.8);
    osg::Vec4d face_color = color_from_rgb(frust.color, 0.1);
    osg::Vec4d near_color = color_from_rgb(frust.color, 1.0);
    osg::Vec4d far_color = color_from_rgb(frust.color, 0.4);
    osg::Vec4d center_line_color(1.0, 1.0, 1.0, 0.9);
    osg::Vec4d cube_color = color_from_rgb(frust.color, 0.95);
    osg::Vec4d vert_fov_color(near_color.r() * 0.7, near_color.g() * 0.7, near_color.b() * 1.0, 0.7);

    double tan_h = std::tan(frust.fov_h * 0.5 * kDegToRad);
    double tan_v = std::tan(frust.fov_v * 0.5 * kDegToRad);

    double nh = frust.near_dist * tan_h;
    double nv = frust.near_dist * tan_v;
    double fh = frust.far_dist * tan_h;
    double fv = frust.far_dist * tan_v;

    osg::Vec3d local_near[4] = {
        osg::Vec3d(frust.near_dist, -nh, nv),
        osg::Vec3d(frust.near_dist, nh, nv),
        osg::Vec3d(frust.near_dist, nh, -nv),
        osg::Vec3d(frust.near_dist, -nh, -nv),
    };

    osg::Vec3d local_far[4] = {
        osg::Vec3d(frust.far_dist, -fh, fv),
        osg::Vec3d(frust.far_dist, fh, fv),
        osg::Vec3d(frust.far_dist, fh, -fv),
        osg::Vec3d(frust.far_dist, -fh, -fv),
    };

    osg::Vec3d pos(frust.position[0], frust.position[1], frust.position[2]);
    osg::Vec3d n[4];
    osg::Vec3d f[4];

    for (int i = 0; i < 4; ++i) {
      n[i] = pos + rotate_by_quat(local_near[i], frust.orientation);
      f[i] = pos + rotate_by_quat(local_far[i], frust.orientation);
    }

    for (int i = 0; i < 4; ++i) {
      int next = (i + 1) % 4;
      bool is_vert_edge = (i == 0 || i == 2);
      osg::Vec4d near_edge_color = is_vert_edge ? vert_fov_color : near_color;

      wire_verts->push_back(n[i]);
      wire_verts->push_back(n[next]);
      wire_colors->push_back(near_edge_color);
      wire_colors->push_back(near_edge_color);
    }

    for (int i = 0; i < 4; ++i) {
      int next = (i + 1) % 4;
      wire_verts->push_back(f[i]);
      wire_verts->push_back(f[next]);
      wire_colors->push_back(far_color);
      wire_colors->push_back(far_color);
    }

    for (int i = 0; i < 4; ++i) {
      wire_verts->push_back(n[i]);
      wire_verts->push_back(f[i]);
      wire_colors->push_back(near_color);
      wire_colors->push_back(far_color);
    }

    {
      osg::Vec3d near_center = (n[0] + n[1] + n[2] + n[3]) * 0.25;
      osg::Vec3d far_center = (f[0] + f[1] + f[2] + f[3]) * 0.25;

      wire_verts->push_back(pos);
      wire_verts->push_back(far_center);
      wire_colors->push_back(center_line_color);
      wire_colors->push_back(center_line_color);

      osg::Vec3d local_right = rotate_by_quat(osg::Vec3d(0, 1, 0), frust.orientation);
      osg::Vec3d local_up = rotate_by_quat(osg::Vec3d(0, 0, 1), frust.orientation);

      wire_verts->push_back(near_center - local_right * kCrossHalf);
      wire_verts->push_back(near_center + local_right * kCrossHalf);
      wire_colors->push_back(near_color);
      wire_colors->push_back(near_color);

      wire_verts->push_back(near_center - local_up * kCrossHalf);
      wire_verts->push_back(near_center + local_up * kCrossHalf);
      wire_colors->push_back(near_color);
      wire_colors->push_back(near_color);
    }

    {
      osg::Vec3d local_x = rotate_by_quat(osg::Vec3d(1, 0, 0), frust.orientation) * kCubeHalf;
      osg::Vec3d local_y = rotate_by_quat(osg::Vec3d(0, 1, 0), frust.orientation) * kCubeHalf;
      osg::Vec3d local_z = rotate_by_quat(osg::Vec3d(0, 0, 1), frust.orientation) * kCubeHalf;

      osg::Vec3d cube[8];
      cube[0] = pos - local_x - local_y - local_z;
      cube[1] = pos + local_x - local_y - local_z;
      cube[2] = pos + local_x + local_y - local_z;
      cube[3] = pos - local_x + local_y - local_z;
      cube[4] = pos - local_x - local_y + local_z;
      cube[5] = pos + local_x - local_y + local_z;
      cube[6] = pos + local_x + local_y + local_z;
      cube[7] = pos - local_x + local_y + local_z;

      static constexpr int kCubeEdges[12][2] = {
          {0, 1}, {1, 2}, {2, 3}, {3, 0}, {4, 5}, {5, 6}, {6, 7}, {7, 4}, {0, 4}, {1, 5}, {2, 6}, {3, 7},
      };

      for (int e = 0; e < 12; ++e) {
        wire_verts->push_back(cube[kCubeEdges[e][0]]);
        wire_verts->push_back(cube[kCubeEdges[e][1]]);
        wire_colors->push_back(cube_color);
        wire_colors->push_back(cube_color);
      }
    }

    for (int i = 0; i < 4; ++i) {
      int next = (i + 1) % 4;

      face_verts->push_back(n[i]);
      face_verts->push_back(f[i]);
      face_verts->push_back(f[next]);
      face_colors->push_back(face_color);
      face_colors->push_back(face_color);
      face_colors->push_back(face_color);

      face_verts->push_back(n[i]);
      face_verts->push_back(f[next]);
      face_verts->push_back(n[next]);
      face_colors->push_back(face_color);
      face_colors->push_back(face_color);
      face_colors->push_back(face_color);
    }

    {
      osg::Vec4d near_face_color = color_from_rgb(frust.color, 0.08);

      face_verts->push_back(n[0]);
      face_verts->push_back(n[1]);
      face_verts->push_back(n[2]);
      face_colors->push_back(near_face_color);
      face_colors->push_back(near_face_color);
      face_colors->push_back(near_face_color);

      face_verts->push_back(n[0]);
      face_verts->push_back(n[2]);
      face_verts->push_back(n[3]);
      face_colors->push_back(near_face_color);
      face_colors->push_back(near_face_color);
      face_colors->push_back(near_face_color);
    }
  }

  wire_verts->dirty();
  wire_colors->dirty();
  face_verts->dirty();
  face_colors->dirty();

  auto* wire_da = static_cast<osg::DrawArrays*>(wire_geo->getPrimitiveSet(0));

  if (wire_da) {
    wire_da->setCount(wire_verts->size());
  }

  auto* face_da = static_cast<osg::DrawArrays*>(face_geo->getPrimitiveSet(0));

  if (face_da) {
    face_da->setCount(face_verts->size());
  }
}

}  // namespace OsgCameraFrustum

#endif

// NOLINTEND
