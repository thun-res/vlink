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

#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic pop
#endif

#include <cstdint>
#include <vector>

namespace OsgPrediction {

struct PredPoint {
  double x{0};
  double y{0};
  double z{0};
};

struct PredictionData {
  std::vector<PredPoint> points;
  uint32_t color{0};
  uint64_t track_id{0};
  float confidence{1.0f};
};

extern osg::ref_ptr<osg::Geode> create();

extern void update(osg::Geode* geode, const std::vector<PredictionData>& predictions, float line_width);

extern uint32_t get_prediction_color(uint64_t track_id);

}  // namespace OsgPrediction

#endif

// NOLINTEND
