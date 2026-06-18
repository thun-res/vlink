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

#include "./osgobjectarray.h"

#ifdef VLINK_ENABLE_VIEWER_OSG

#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Woverloaded-virtual"
#pragma GCC diagnostic ignored "-Wdeprecated-copy"
#endif

#include <osg/BlendFunc>
#include <osg/CullFace>
#include <osg/Depth>
#include <osg/Geometry>
#include <osg/LineWidth>
#include <osgText/Text>

#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic pop
#endif

#include <cctype>
#include <cmath>
#include <cstring>

namespace OsgObjectArray {

static constexpr uint32_t kClassColors[] = {
    0x00FFFF, 0x00FF00, 0xFFFF00, 0x0088FF, 0xFF4444, 0xFF00FF, 0xFF8800, 0xFFFFFF,
    0x88FF88, 0x8888FF, 0xFF88FF, 0xFFFF88, 0x88FFFF, 0xAAAA00, 0x00AAAA, 0xAA00AA,
};
static constexpr size_t kClassColorCount = sizeof(kClassColors) / sizeof(kClassColors[0]);

static osg::Vec4d color_from_rgb(uint32_t rgb, double alpha = 1.0) {
  return osg::Vec4d(((rgb >> 16) & 0xFF) / 255.0, ((rgb >> 8) & 0xFF) / 255.0, (rgb & 0xFF) / 255.0, alpha);
}

static osg::Vec4d brighten_color(const osg::Vec4d& c, double factor) {
  return osg::Vec4d(std::min(c.x() * factor, 1.0), std::min(c.y() * factor, 1.0), std::min(c.z() * factor, 1.0), c.w());
}

static void compute_box_corners(const ObjectData& obj, osg::Vec3d corners[8], double cos_yaw, double sin_yaw) {
  double hl = obj.size[0] / 2.0;
  double hw = obj.size[1] / 2.0;
  double hh = obj.size[2] / 2.0;

  double local[8][3] = {
      {hl, hw, hh},  {hl, -hw, hh},  {-hl, -hw, hh},  {-hl, hw, hh},
      {hl, hw, -hh}, {hl, -hw, -hh}, {-hl, -hw, -hh}, {-hl, hw, -hh},
  };

  for (int i = 0; i < 8; ++i) {
    double rx = local[i][0] * cos_yaw - local[i][1] * sin_yaw + obj.position[0];
    double ry = local[i][0] * sin_yaw + local[i][1] * cos_yaw + obj.position[1];
    double rz = local[i][2] + obj.position[2];
    corners[i] = osg::Vec3d(rx, ry, rz);
  }
}

uint32_t get_class_color(uint32_t class_id) { return kClassColors[class_id % kClassColorCount]; }

const char* get_class_name(uint32_t class_id) {
  static constexpr const char* kClassNames[] = {
      "unknown",
      "car",
      "truck",
      "bus",
      "trailer",
      "motorcycle",
      "bicycle",
      "pedestrian",
      "animal",
      "cone",
      "barrier",
      "debris",
      "vehicle",
      "large_vehicle",
      "train",
      "emergency",
      "construction",
      "tractor",
      "van",
      "pickup",
      "minibus",
      "suv",
      "tricycle",
      "moped",
      "stroller",
      "wheelchair",
      "cart",
      "dog",
      "cat",
      "bird",
      "dummy",
      "other",
      "sedan",
      "hatchback",
      "coupe",
      "convertible",
      "sports_car",
      "wagon",
      "mpv",
      "crossover",
      "rv",
      "ambulance",
      "fire_truck",
      "police",
      "taxi",
      "school_bus",
      "concrete_mixer",
      "dump_truck",
      "tanker",
      "flatbed",
      "semi_trailer",
      "forklift",
      "excavator",
      "bulldozer",
      "crane",
      "road_roller",
      "sweeper",
      "sprinkler",
      "garbage_truck",
      "delivery",
      "scooter",
      "skateboard",
      "segway",
      "e_bike",
      "e_scooter",
      "child",
      "adult",
      "cyclist",
      "rider",
      "runner",
      "crowd",
      "sitting",
      "lying",
      "standing",
      "crouching",
      "horse",
      "cow",
      "sheep",
      "deer",
      "rabbit",
      "squirrel",
      "traffic_cone",
      "bollard",
      "water_barrier",
      "jersey_wall",
      "guardrail",
      "curb",
      "fence",
      "wall",
      "pole",
      "sign",
      "signal",
      "light",
      "fire_hydrant",
      "mailbox",
      "trash_can",
      "bench",
      "vegetation",
      "tree",
      "bush",
      "rock",
      "pothole",
      "speed_bump",
      "manhole",
      "puddle",
      "road_crack",
      "tire",
      "box",
      "pallet",
      "container",
      "noise_barrier",
      "overpass",
      "bridge",
      "tunnel",
      "building",
      "parking_lot",
      "crosswalk",
      "stop_line",
      "arrow",
      "zebra",
  };
  static constexpr size_t kClassNameCount = sizeof(kClassNames) / sizeof(kClassNames[0]);

  if (class_id < kClassNameCount) {
    return kClassNames[class_id];
  }

  return "obj";
}

static std::string normalize_class_str(const std::string& input) {
  std::string result;
  result.reserve(input.size());

  for (char ch : input) {
    if (ch == '_' || ch == '-' || ch == ' ' || ch == '.') {
      continue;
    }

    result += static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
  }

  static constexpr const char* kStripPrefixes[] = {"label", "class", "type", "category", "obj", "obstacle"};

  for (const auto* prefix : kStripPrefixes) {
    size_t plen = std::strlen(prefix);

    if (result.size() > plen && result.compare(0, plen, prefix) == 0) {
      result = result.substr(plen);
      break;
    }
  }

  return result;
}

uint32_t find_class_id(const std::string& name) {
  if (name.empty()) {
    return 0;
  }

  std::string norm = normalize_class_str(name);

  for (uint32_t i = 0; i < 120; ++i) {
    const char* cn = get_class_name(i);

    if (!cn) {
      break;
    }

    if (normalize_class_str(cn) == norm) {
      return i;
    }
  }

  if (norm.find("car") != std::string::npos || norm.find("sedan") != std::string::npos) {
    return 1;
  }

  if (norm.find("truck") != std::string::npos) {
    return 2;
  }

  if (norm.find("bus") != std::string::npos) {
    return 3;
  }

  if (norm.find("motor") != std::string::npos || norm.find("moto") != std::string::npos) {
    return 5;
  }

  if (norm.find("bike") != std::string::npos || norm.find("cycl") != std::string::npos) {
    return 6;
  }

  if (norm.find("ped") != std::string::npos || norm.find("person") != std::string::npos ||
      norm.find("human") != std::string::npos || norm.find("people") != std::string::npos) {
    return 7;
  }

  if (norm.find("animal") != std::string::npos) {
    return 8;
  }

  if (norm.find("cone") != std::string::npos) {
    return 9;
  }

  if (norm.find("barrier") != std::string::npos) {
    return 10;
  }

  return 0;
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

    osg::ref_ptr<osg::LineWidth> lw = new osg::LineWidth(2.5f);
    osg::StateSet* ss = geometry->getOrCreateStateSet();
    ss->setAttribute(lw);
    ss->setMode(GL_BLEND, osg::StateAttribute::ON);
    ss->setMode(GL_LIGHTING, osg::StateAttribute::OFF);
    ss->setMode(GL_LINE_SMOOTH, osg::StateAttribute::ON);
    ss->setAttributeAndModes(new osg::BlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA), osg::StateAttribute::ON);

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
    geometry->addPrimitiveSet(new osg::DrawArrays(osg::PrimitiveSet::LINES, 0, 0));

    osg::ref_ptr<osg::LineWidth> lw = new osg::LineWidth(4.0f);
    osg::StateSet* ss = geometry->getOrCreateStateSet();
    ss->setAttribute(lw);
    ss->setMode(GL_BLEND, osg::StateAttribute::ON);
    ss->setMode(GL_LIGHTING, osg::StateAttribute::OFF);
    ss->setMode(GL_LINE_SMOOTH, osg::StateAttribute::ON);
    ss->setAttributeAndModes(new osg::BlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA), osg::StateAttribute::ON);

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
    geometry->addPrimitiveSet(new osg::DrawArrays(osg::PrimitiveSet::LINES, 0, 0));

    osg::ref_ptr<osg::LineWidth> lw = new osg::LineWidth(1.5f);
    osg::StateSet* ss = geometry->getOrCreateStateSet();
    ss->setAttribute(lw);
    ss->setMode(GL_BLEND, osg::StateAttribute::ON);
    ss->setMode(GL_LIGHTING, osg::StateAttribute::OFF);
    ss->setMode(GL_LINE_SMOOTH, osg::StateAttribute::ON);
    ss->setAttributeAndModes(new osg::BlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA), osg::StateAttribute::ON);

    geode->addDrawable(geometry);
  }

  return geode;
}

void update(osg::Geode* geode, const std::vector<ObjectData>& objects, float line_width) {
  if (!geode || geode->getNumDrawables() < 5) {
    return;
  }

  auto* box_geo = static_cast<osg::Geometry*>(geode->getDrawable(0));
  auto* box_verts = static_cast<osg::Vec3dArray*>(box_geo->getVertexArray());
  auto* box_colors = static_cast<osg::Vec4dArray*>(box_geo->getColorArray());

  auto* bottom_geo = static_cast<osg::Geometry*>(geode->getDrawable(1));
  auto* bottom_verts = static_cast<osg::Vec3dArray*>(bottom_geo->getVertexArray());
  auto* bottom_colors = static_cast<osg::Vec4dArray*>(bottom_geo->getColorArray());

  auto* front_geo = static_cast<osg::Geometry*>(geode->getDrawable(2));
  auto* front_verts = static_cast<osg::Vec3dArray*>(front_geo->getVertexArray());
  auto* front_colors = static_cast<osg::Vec4dArray*>(front_geo->getColorArray());

  auto* arrow_geo = static_cast<osg::Geometry*>(geode->getDrawable(3));
  auto* arrow_verts = static_cast<osg::Vec3dArray*>(arrow_geo->getVertexArray());
  auto* arrow_colors = static_cast<osg::Vec4dArray*>(arrow_geo->getColorArray());

  auto* cross_geo = static_cast<osg::Geometry*>(geode->getDrawable(4));
  auto* cross_verts = static_cast<osg::Vec3dArray*>(cross_geo->getVertexArray());
  auto* cross_colors = static_cast<osg::Vec4dArray*>(cross_geo->getColorArray());

  {
    auto* lw =
        dynamic_cast<osg::LineWidth*>(box_geo->getOrCreateStateSet()->getAttribute(osg::StateAttribute::LINEWIDTH));

    if (lw && lw->getWidth() != line_width) {
      lw->setWidth(line_width);
    }
  }

  box_verts->clear();
  box_colors->clear();
  bottom_verts->clear();
  bottom_colors->clear();
  front_verts->clear();
  front_colors->clear();
  arrow_verts->clear();
  arrow_colors->clear();
  cross_verts->clear();
  cross_colors->clear();

  size_t est = objects.size();
  box_verts->reserve(est * 24);
  box_colors->reserve(est * 24);
  bottom_verts->reserve(est * 36);
  bottom_colors->reserve(est * 36);
  front_verts->reserve(est * 8);
  front_colors->reserve(est * 8);
  arrow_verts->reserve(est * 9);
  arrow_colors->reserve(est * 9);
  cross_verts->reserve(est * 4);
  cross_colors->reserve(est * 4);

  static constexpr int kEdges[12][2] = {
      {0, 1}, {1, 2}, {2, 3}, {3, 0}, {4, 5}, {5, 6}, {6, 7}, {7, 4}, {0, 4}, {1, 5}, {2, 6}, {3, 7},
  };

  for (const auto& obj : objects) {
    uint32_t c = obj.color != 0 ? obj.color : get_class_color(obj.class_id);
    double alpha = (obj.score > 0.01 && obj.score <= 1.0) ? (0.5 + 0.5 * obj.score) : 1.0;
    osg::Vec4d color = color_from_rgb(c, alpha);
    osg::Vec4d front_color = brighten_color(color_from_rgb(c, alpha), 1.4);
    osg::Vec4d bottom_color = color_from_rgb(c, alpha * 0.12);

    double cos_yaw = std::cos(obj.yaw);
    double sin_yaw = std::sin(obj.yaw);

    osg::Vec3d corners[8];
    compute_box_corners(obj, corners, cos_yaw, sin_yaw);

    for (const auto& e : kEdges) {
      box_verts->push_back(corners[e[0]]);
      box_verts->push_back(corners[e[1]]);
      box_colors->push_back(color);
      box_colors->push_back(color);
    }

    osg::Vec4d side_color = color_from_rgb(c, alpha * 0.1);
    osg::Vec4d top_color = color_from_rgb(c, alpha * 0.08);
    osg::Vec4d front_fill = color_from_rgb(c, alpha * 0.15);

    static constexpr int kFaces[6][4] = {
        {4, 5, 6, 7}, {0, 3, 2, 1}, {0, 1, 5, 4}, {2, 3, 7, 6}, {0, 4, 7, 3}, {1, 2, 6, 5},
    };

    for (int fi = 0; fi < 6; ++fi) {
      osg::Vec4d face_color = (fi == 0) ? bottom_color : (fi == 1) ? top_color : (fi == 2) ? front_fill : side_color;

      bottom_verts->push_back(corners[kFaces[fi][0]]);
      bottom_verts->push_back(corners[kFaces[fi][1]]);
      bottom_verts->push_back(corners[kFaces[fi][2]]);
      bottom_colors->push_back(face_color);
      bottom_colors->push_back(face_color);
      bottom_colors->push_back(face_color);

      bottom_verts->push_back(corners[kFaces[fi][0]]);
      bottom_verts->push_back(corners[kFaces[fi][2]]);
      bottom_verts->push_back(corners[kFaces[fi][3]]);
      bottom_colors->push_back(face_color);
      bottom_colors->push_back(face_color);
      bottom_colors->push_back(face_color);
    }

    front_verts->push_back(corners[0]);
    front_verts->push_back(corners[1]);
    front_colors->push_back(front_color);
    front_colors->push_back(front_color);

    front_verts->push_back(corners[4]);
    front_verts->push_back(corners[5]);
    front_colors->push_back(front_color);
    front_colors->push_back(front_color);

    front_verts->push_back(corners[0]);
    front_verts->push_back(corners[4]);
    front_colors->push_back(front_color);
    front_colors->push_back(front_color);

    front_verts->push_back(corners[1]);
    front_verts->push_back(corners[5]);
    front_colors->push_back(front_color);
    front_colors->push_back(front_color);

    [[maybe_unused]] double hl = obj.size[0] / 2.0;
    [[maybe_unused]] double hw = obj.size[1] / 2.0;

    double arrow_len = std::max(obj.size[0] * 0.35, 0.4);
    double arrow_wid = std::max(obj.size[1] * 0.4, 0.3);
    double pz = obj.position[2];

    auto rotate_pt = [&](double lx, double ly, double lz) -> osg::Vec3d {
      return osg::Vec3d(lx * cos_yaw - ly * sin_yaw + obj.position[0], lx * sin_yaw + ly * cos_yaw + obj.position[1],
                        lz);
    };

    osg::Vec4d heading_color = brighten_color(color_from_rgb(c, 0.85), 1.2);
    arrow_verts->push_back(rotate_pt(hl + arrow_len * 0.3, 0, pz));
    arrow_verts->push_back(rotate_pt(hl - arrow_len * 0.7, arrow_wid / 2.0, pz));
    arrow_verts->push_back(rotate_pt(hl - arrow_len * 0.7, -arrow_wid / 2.0, pz));
    arrow_colors->push_back(heading_color);
    arrow_colors->push_back(heading_color);
    arrow_colors->push_back(heading_color);

    double vx = obj.velocity[0];
    double vy = obj.velocity[1];
    double vz = obj.velocity[2];
    double v_mag = std::sqrt(vx * vx + vy * vy + vz * vz);

    if (v_mag > 0.5) {
      static constexpr double kVelArrowLen = 0.3;
      static constexpr double kVelArrowWid = 0.15;
      osg::Vec4d vel_color(0.3, 1.0, 0.3, 0.95);

      osg::Vec3d tip(obj.position[0] + vx, obj.position[1] + vy, pz + vz);
      double vn = 1.0 / v_mag;
      double dvx = vx * vn;
      double dvy = vy * vn;
      double perp_x = -dvy;
      double perp_y = dvx;

      osg::Vec3d arrow_base(tip.x() - dvx * kVelArrowLen, tip.y() - dvy * kVelArrowLen, tip.z());
      osg::Vec3d arrow_left(arrow_base.x() + perp_x * kVelArrowWid, arrow_base.y() + perp_y * kVelArrowWid,
                            arrow_base.z());
      osg::Vec3d arrow_right(arrow_base.x() - perp_x * kVelArrowWid, arrow_base.y() - perp_y * kVelArrowWid,
                             arrow_base.z());

      arrow_verts->push_back(tip);
      arrow_verts->push_back(arrow_left);
      arrow_verts->push_back(arrow_right);
      arrow_colors->push_back(vel_color);
      arrow_colors->push_back(vel_color);
      arrow_colors->push_back(vel_color);
    }

    static constexpr double kCrossSize = 0.3;
    double bottom_z = obj.position[2] - obj.size[2] / 2.0;
    osg::Vec4d cross_color = color_from_rgb(c, 0.6);

    cross_verts->push_back(rotate_pt(kCrossSize, kCrossSize, bottom_z));
    cross_verts->push_back(rotate_pt(-kCrossSize, -kCrossSize, bottom_z));
    cross_colors->push_back(cross_color);
    cross_colors->push_back(cross_color);

    cross_verts->push_back(rotate_pt(kCrossSize, -kCrossSize, bottom_z));
    cross_verts->push_back(rotate_pt(-kCrossSize, kCrossSize, bottom_z));
    cross_colors->push_back(cross_color);
    cross_colors->push_back(cross_color);
  }

  box_verts->dirty();
  box_colors->dirty();
  bottom_verts->dirty();
  bottom_colors->dirty();
  front_verts->dirty();
  front_colors->dirty();
  arrow_verts->dirty();
  arrow_colors->dirty();
  cross_verts->dirty();
  cross_colors->dirty();

  auto* box_da = static_cast<osg::DrawArrays*>(box_geo->getPrimitiveSet(0));

  if (box_da) {
    box_da->setCount(box_verts->size());
  }

  auto* bottom_da = static_cast<osg::DrawArrays*>(bottom_geo->getPrimitiveSet(0));

  if (bottom_da) {
    bottom_da->setCount(bottom_verts->size());
  }

  auto* front_da = static_cast<osg::DrawArrays*>(front_geo->getPrimitiveSet(0));

  if (front_da) {
    front_da->setCount(front_verts->size());
  }

  auto* arrow_da = static_cast<osg::DrawArrays*>(arrow_geo->getPrimitiveSet(0));

  if (arrow_da) {
    arrow_da->setCount(arrow_verts->size());
  }

  auto* cross_da = static_cast<osg::DrawArrays*>(cross_geo->getPrimitiveSet(0));

  if (cross_da) {
    cross_da->setCount(cross_verts->size());
  }
}

void update_labels(osg::Geode* geode, const std::vector<ObjectData>& objects, osg::ref_ptr<osgText::Font> font,
                   float ratio) {
  if (!geode || !font) {
    return;
  }

  static constexpr size_t kTextStart = 5;

  size_t needed = 0;

  for (const auto& obj : objects) {
    if (!obj.label.empty()) {
      ++needed;
    }
  }

  while (geode->getNumDrawables() > kTextStart + needed) {
    geode->removeDrawable(geode->getDrawable(geode->getNumDrawables() - 1));
  }

  size_t text_idx = 0;

  for (const auto& obj : objects) {
    if (obj.label.empty()) {
      continue;
    }

    osgText::Text* text;

    if (kTextStart + text_idx < geode->getNumDrawables()) {
      text = static_cast<osgText::Text*>(geode->getDrawable(kTextStart + text_idx));
    } else {
      auto* new_text = new osgText::Text;
      new_text->setFont(font);
      new_text->setCharacterSize(0.4f * ratio);
      new_text->setAxisAlignment(osgText::Text::SCREEN);
      new_text->setAlignment(osgText::Text::CENTER_BOTTOM);
      new_text->setBackdropType(osgText::Text::OUTLINE);
      new_text->setBackdropColor(osg::Vec4(0, 0, 0, 0.7f));
      geode->addDrawable(new_text);
      text = new_text;
    }

    text->setPosition(osg::Vec3d(obj.position[0], obj.position[1], obj.position[2] + obj.size[2] / 2.0 + 0.3));
    text->setText(obj.label);

    uint32_t c = obj.color != 0 ? obj.color : get_class_color(obj.class_id);
    text->setColor(osg::Vec4(((c >> 16) & 0xFF) / 255.0f, ((c >> 8) & 0xFF) / 255.0f, (c & 0xFF) / 255.0f, 1.0f));

    ++text_idx;
  }
}

}  // namespace OsgObjectArray

#endif

// NOLINTEND
