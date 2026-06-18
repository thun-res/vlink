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

#include "./osgcommon.h"

#ifdef VLINK_ENABLE_VIEWER_OSG

#include <iostream>

namespace OsgCommon {

osg::Vec3d worldToScreen(const osg::Vec3d& worldPoint, osg::Camera* camera) {
  osg::Viewport* viewport = camera->getViewport();
  osg::Matrixd viewMatrix = camera->getViewMatrix();
  osg::Matrixd projMatrix = camera->getProjectionMatrix();

  osg::Vec4d worldPointH(worldPoint.x(), worldPoint.y(), worldPoint.z(), 1.0);
  osg::Vec4d clipSpacePoint = worldPointH * viewMatrix * projMatrix;

  osg::Vec3d normalizedScreenPoint(clipSpacePoint.x() / clipSpacePoint.w(), clipSpacePoint.y() / clipSpacePoint.w(),
                                   clipSpacePoint.z() / clipSpacePoint.w());

  normalizedScreenPoint.x() = normalizedScreenPoint.x() * 0.5 + 0.5;
  normalizedScreenPoint.y() = normalizedScreenPoint.y() * 0.5 + 0.5;

  normalizedScreenPoint.x() = normalizedScreenPoint.x() * viewport->width() + viewport->x();
  normalizedScreenPoint.y() = normalizedScreenPoint.y() * viewport->height() + viewport->y();

  normalizedScreenPoint.y() = viewport->height() - normalizedScreenPoint.y();

  return normalizedScreenPoint;
}

void printVec3d(const std::string& name, const osg::Vec3d& vec3d) {
  std::cout << "\"" << name << "\""
            << ": "
            << "\"" << vec3d.x() << "," << vec3d.y() << "," << vec3d.z() << "\"" << std::endl;
}

void printVec4d(const std::string& name, const osg::Vec4d& vec4d) {
  std::cout << "\"" << name << "\""
            << ": "
            << "\"" << vec4d.x() << "," << vec4d.y() << "," << vec4d.z() << "," << vec4d.w() << "\"" << std::endl;
}
template <typename T>
T getVecForString(const QString& value) {
  T vec;

  auto list = value.split(",");

  if (list.length() != T::num_components) {
    return T();
  }

  bool ok = false;

  for (int i = 0; i < T::num_components; i++) {
    double t = list[i].trimmed().toDouble(&ok);

    if (!ok) {
      return T();
    }

    vec._v[i] = t;
  }

  return vec;
}

osg::Matrix getMatrix(const QVariantMap& value) {
  osg::Vec3d translate = getVecForString<osg::Vec3d>(value.value("translate", "0,0,0").toString());
  osg::Vec4d rotate = getVecForString<osg::Vec4d>(value.value("rotate", "0,0,0,1").toString());
  osg::Vec3d scale = getVecForString<osg::Vec3d>(value.value("scale", "1,1,1").toString());

  return osg::Matrix::scale(scale) * osg::Matrix::rotate(rotate) * osg::Matrix::translate(translate);
}

std::tuple<osg::Vec3d, osg::Vec3d, osg::Vec3d> getHomePos(const QVariantMap& value) {
  std::tuple<osg::Vec3d, osg::Vec3d, osg::Vec3d> pos;
  std::get<0>(pos) = getVecForString<osg::Vec3d>(value.value("eye", "0,0,0").toString());
  std::get<1>(pos) = getVecForString<osg::Vec3d>(value.value("center", "0,0,0").toString());
  std::get<2>(pos) = getVecForString<osg::Vec3d>(value.value("up", "0,0,0").toString());

  return pos;
}

std::vector<osg::ref_ptr<osg::AnimationPath>> getFlyList(const QVariantList& value) {
  std::vector<osg::ref_ptr<osg::AnimationPath>> pathList;

  for (const auto& pvalue : value) {
    osg::ref_ptr<osg::AnimationPath> path = new osg::AnimationPath;
    path->setLoopMode(osg::AnimationPath::NO_LOOPING);
    path->setName(pvalue.toMap().value("name").toString().toStdString());

    const auto& list = pvalue.toMap().value("path").toList();

    for (const auto& p : list) {
      osg::AnimationPath::ControlPoint pos;

      pos.setPosition(getVecForString<osg::Vec3d>(p.toMap().value("position", "0,0,0").toString()));
      pos.setRotation(getVecForString<osg::Vec4d>(p.toMap().value("rotation", "0,0,0,1").toString()));
      pos.setScale(getVecForString<osg::Vec3d>(p.toMap().value("scale", "1,1,1").toString()));
      path->insert(p.toMap().value("time", 0).toDouble(), pos);
    }

    pathList.emplace_back(path);
  }

  return pathList;
}

}  // namespace OsgCommon

#endif

// NOLINTEND
