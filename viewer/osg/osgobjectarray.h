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

#include <osg/Geode>
#include <osgText/Font>

#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic pop
#endif

#include <cstdint>
#include <string>
#include <vector>

namespace OsgObjectArray {

struct ObjectData {
  double position[3]{0};
  double size[3]{0};
  double yaw{0};
  double velocity[3]{0};
  double score{0};
  uint32_t class_id{0};
  uint64_t track_id{0};
  uint32_t color{0};
  std::string label;
};

extern osg::ref_ptr<osg::Geode> create();

extern void update(osg::Geode* geode, const std::vector<ObjectData>& objects, float line_width);

extern void update_labels(osg::Geode* geode, const std::vector<ObjectData>& objects, osg::ref_ptr<osgText::Font> font,
                          float ratio);

extern uint32_t get_class_color(uint32_t class_id);

extern const char* get_class_name(uint32_t class_id);

extern uint32_t find_class_id(const std::string& name);

}  // namespace OsgObjectArray

#endif

// NOLINTEND
