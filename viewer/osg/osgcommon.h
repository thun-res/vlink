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

#pragma once

#ifdef VLINK_ENABLE_VIEWER_OSG

#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Woverloaded-virtual"
#pragma GCC diagnostic ignored "-Wdeprecated-copy"
#endif

#include <osg/AnimationPath>
#include <osg/Camera>
#include <osg/Matrix>
#include <osg/Quat>
#include <osg/Vec3d>

#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic pop
#endif

#include <QVariantMap>
#include <string>
#include <tuple>
#include <vector>

namespace OsgCommon {

osg::Vec3d worldToScreen(const osg::Vec3d& worldPoint, osg::Camera* camera);

void printVec3d(const std::string& name, const osg::Vec3d& vec3d);

void printVec4d(const std::string& name, const osg::Vec4d& vec4d);

osg::Matrix getMatrix(const QVariantMap& value);

std::tuple<osg::Vec3d, osg::Vec3d, osg::Vec3d> getHomePos(const QVariantMap& value);

std::vector<osg::ref_ptr<osg::AnimationPath>> getFlyList(const QVariantList& value);

}  // namespace OsgCommon

#endif

// NOLINTEND
