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

#include "./osgpointcloud.h"

#ifdef VLINK_ENABLE_VIEWER_OSG

#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Woverloaded-virtual"
#pragma GCC diagnostic ignored "-Wdeprecated-copy"
#endif

#include <osg/Geometry>
#include <osg/Point>

#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic pop
#endif

namespace OsgPointCloud {

osg::ref_ptr<osg::Geode> create(float point_size, float ratio) {
  osg::ref_ptr<osg::Geode> geode = new osg::Geode();

  {
    osg::ref_ptr<osg::Geometry> geometry = new osg::Geometry();
    osg::ref_ptr<osg::Vec3dArray> vertex_array = new osg::Vec3dArray();
    osg::ref_ptr<osg::Vec4dArray> color_array = new osg::Vec4dArray();

    geometry->setUseVertexBufferObjects(true);
    geometry->setVertexArray(vertex_array);
    geometry->setColorArray(color_array, osg::Array::BIND_PER_VERTEX);
    geometry->addPrimitiveSet(new osg::DrawArrays(osg::PrimitiveSet::POINTS, 0, 0));

    osg::ref_ptr<osg::StateSet> state_set = geometry->getOrCreateStateSet();
    osg::ref_ptr<osg::Point> point = new osg::Point();
    point->setSize(point_size * ratio);
    state_set->setAttribute(point);
    state_set->setMode(GL_POINT_SMOOTH, osg::StateAttribute::ON);

    geode->addDrawable(geometry);
  }

  {
    osg::ref_ptr<osg::Geometry> geometry_select = new osg::Geometry();
    osg::ref_ptr<osg::Vec3dArray> vertex_array_select = new osg::Vec3dArray();
    osg::ref_ptr<osg::Vec4dArray> color_array_select = new osg::Vec4dArray();

    geometry_select->setUseVertexBufferObjects(true);
    geometry_select->setVertexArray(vertex_array_select);
    geometry_select->setColorArray(color_array_select, osg::Array::BIND_PER_VERTEX);
    geometry_select->addPrimitiveSet(new osg::DrawArrays(osg::PrimitiveSet::POINTS, 0, 0));

    osg::ref_ptr<osg::StateSet> state_set_select = geometry_select->getOrCreateStateSet();
    osg::ref_ptr<osg::Point> point_select = new osg::Point();
    point_select->setSize(std::min(point_size * 3, 15.0f) * ratio);
    state_set_select->setAttribute(point_select);
    state_set_select->setMode(GL_POINT_SMOOTH, osg::StateAttribute::ON);

    geode->addDrawable(geometry_select);
  }

  return geode;
}

void update_point_size(osg::Geode* geode, float point_size, float select_size, float ratio) {
  if (!geode || geode->getNumDrawables() < 2) {
    return;
  }

  auto* geometry = static_cast<osg::Geometry*>(geode->getDrawable(0));

  if (geometry && geometry->getStateSet()) {
    auto* point = dynamic_cast<osg::Point*>(geometry->getStateSet()->getAttribute(osg::StateAttribute::POINT));

    if (point && point->getSize() != point_size * ratio) {
      point->setSize(point_size * ratio);
    }
  }

  auto* geometry_select = static_cast<osg::Geometry*>(geode->getDrawable(1));

  if (geometry_select && geometry_select->getStateSet()) {
    auto* point = dynamic_cast<osg::Point*>(geometry_select->getStateSet()->getAttribute(osg::StateAttribute::POINT));

    if (point && point->getSize() != select_size * ratio) {
      point->setSize(select_size * ratio);
    }
  }
}

void clear_arrays(osg::Geode* geode) {
  if (!geode || geode->getNumDrawables() < 2) {
    return;
  }

  auto* geometry = static_cast<osg::Geometry*>(geode->getDrawable(0));

  if (geometry) {
    if (auto* va = static_cast<osg::Vec3dArray*>(geometry->getVertexArray())) {
      va->clear();
    }

    if (auto* ca = static_cast<osg::Vec4dArray*>(geometry->getColorArray())) {
      ca->clear();
    }
  }

  auto* geometry_select = static_cast<osg::Geometry*>(geode->getDrawable(1));

  if (geometry_select) {
    if (auto* va = static_cast<osg::Vec3dArray*>(geometry_select->getVertexArray())) {
      va->clear();
    }

    if (auto* ca = static_cast<osg::Vec4dArray*>(geometry_select->getColorArray())) {
      ca->clear();
    }
  }

  if (geometry) {
    static_cast<osg::Vec3dArray*>(geometry->getVertexArray())->dirty();
    static_cast<osg::Vec4dArray*>(geometry->getColorArray())->dirty();
    auto* da0 = static_cast<osg::DrawArrays*>(geometry->getPrimitiveSet(0));

    if (da0) {
      da0->setCount(0);
    }
  }

  if (geometry_select) {
    static_cast<osg::Vec3dArray*>(geometry_select->getVertexArray())->dirty();
    static_cast<osg::Vec4dArray*>(geometry_select->getColorArray())->dirty();
    auto* da1 = static_cast<osg::DrawArrays*>(geometry_select->getPrimitiveSet(0));

    if (da1) {
      da1->setCount(0);
    }
  }
}

void finalize_arrays(osg::Geode* geode) {
  if (!geode || geode->getNumDrawables() < 2) {
    return;
  }

  auto* geometry = static_cast<osg::Geometry*>(geode->getDrawable(0));
  auto* vertex_array = static_cast<osg::Vec3dArray*>(geometry->getVertexArray());
  auto* color_array = static_cast<osg::Vec4dArray*>(geometry->getColorArray());

  vertex_array->dirty();
  color_array->dirty();

  auto* draw_arrays = static_cast<osg::DrawArrays*>(geometry->getPrimitiveSet(0));

  if (draw_arrays) {
    draw_arrays->setCount(vertex_array->size());
  }

  auto* geometry_select = static_cast<osg::Geometry*>(geode->getDrawable(1));
  auto* vertex_array_select = static_cast<osg::Vec3dArray*>(geometry_select->getVertexArray());
  auto* color_array_select = static_cast<osg::Vec4dArray*>(geometry_select->getColorArray());

  vertex_array_select->dirty();
  color_array_select->dirty();

  auto* draw_arrays_select = static_cast<osg::DrawArrays*>(geometry_select->getPrimitiveSet(0));

  if (draw_arrays_select) {
    draw_arrays_select->setCount(vertex_array_select->size());
  }
}

}  // namespace OsgPointCloud

#endif

// NOLINTEND
