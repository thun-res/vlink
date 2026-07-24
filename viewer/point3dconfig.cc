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

#include "./point3dconfig.h"

#include <QFileInfo>
#include <QRegularExpression>
#include <algorithm>
#include <fstream>
#include <nlohmann/json.hpp>
#include <utility>

using Json = nlohmann::json;

namespace point3d_config_detail {

QString normalized_serializer(QString value) {
  value = value.toLower();
  value.remove('_');
  value.remove(' ');
  value.remove('.');
  value.remove('-');
  value.remove(':');
  return value;
}

QString normalized_url(QString value) { return value.toLower(); }

QStringList normalized_url_exact_candidates(QString value) {
  QStringList out;

  auto append = [&out](const QString& candidate) {
    if (!candidate.isEmpty() && !out.contains(candidate)) {
      out << candidate;
    }
  };

  value = normalized_url(value);
  append(value);

  QString without_suffix = value;
  const auto query_pos = without_suffix.indexOf('?');
  const auto fragment_pos = without_suffix.indexOf('#');
  auto suffix_pos = decltype(query_pos){-1};

  if (query_pos >= 0 && fragment_pos >= 0) {
    suffix_pos = std::min(query_pos, fragment_pos);
  } else {
    suffix_pos = std::max(query_pos, fragment_pos);
  }

  if (suffix_pos >= 0) {
    without_suffix = without_suffix.left(suffix_pos);
    append(without_suffix);
  }

  QString content = without_suffix;
  const auto scheme_pos = content.indexOf("://");

  if (scheme_pos >= 0) {
    content = content.mid(scheme_pos + 3);
  } else {
    const auto colon_pos = content.indexOf(':');
    if (colon_pos >= 0) {
      content = content.mid(colon_pos + 1);
    }
  }

  append(content);

  if (!content.isEmpty()) {
    append(content.startsWith('/') ? content.mid(1) : QString("/") + content);
  }

  const auto slash_pos = content.indexOf('/');
  if (slash_pos >= 0) {
    const auto path = content.mid(slash_pos);
    append(path);

    if (path.startsWith('/')) {
      append(path.mid(1));
    }
  }

  return out;
}

QStringList json_string_list(const Json& value) {
  QStringList out;

  if (value.is_string()) {
    const auto text = QString::fromStdString(value.get<std::string>()).trimmed();
    if (!text.isEmpty()) {
      out << text;
    }
    return out;
  }

  if (!value.is_array()) {
    return out;
  }

  for (const auto& item : value) {
    if (!item.is_string()) {
      continue;
    }

    const auto text = QString::fromStdString(item.get<std::string>()).trimmed();
    if (!text.isEmpty()) {
      out << text;
    }
  }

  return out;
}

QString json_path_value(const Json& value) {
  if (value.is_string()) {
    return QString::fromStdString(value.get<std::string>()).trimmed();
  }

  if (!value.is_array()) {
    return {};
  }

  QStringList parts;

  for (const auto& item : value) {
    if (!item.is_string()) {
      continue;
    }

    const auto text = QString::fromStdString(item.get<std::string>()).trimmed();
    if (!text.isEmpty()) {
      parts << text;
    }
  }

  return parts.join('.');
}

const Json* json_find_any(const Json& object, std::initializer_list<const char*> keys) {
  if (!object.is_object()) {
    return nullptr;
  }

  for (const auto* key : keys) {
    if (!key) {
      continue;
    }

    auto it = object.find(key);
    if (it != object.end()) {
      return &(*it);
    }
  }

  return nullptr;
}

QString json_string_value(const Json& object, std::initializer_list<const char*> keys, QString fallback = {}) {
  const auto* value = json_find_any(object, keys);

  if (!value || !value->is_string()) {
    return fallback;
  }

  return QString::fromStdString(value->get<std::string>());
}

QString json_path_string_value(const Json& object, std::initializer_list<const char*> keys, QString fallback = {}) {
  const auto* value = json_find_any(object, keys);

  if (!value) {
    return fallback;
  }

  const auto path = json_path_value(*value);
  return path.isEmpty() ? fallback : path;
}

bool json_bool_value(const Json& object, std::initializer_list<const char*> keys, bool fallback) {
  const auto* value = json_find_any(object, keys);

  if (!value || !value->is_boolean()) {
    return fallback;
  }

  return value->get<bool>();
}

int json_int_value(const Json& object, std::initializer_list<const char*> keys, int fallback) {
  const auto* value = json_find_any(object, keys);

  if (!value || !value->is_number_integer()) {
    return fallback;
  }

  return value->get<int>();
}

QStringList json_string_list_any(const Json& object, std::initializer_list<const char*> keys) {
  const auto* value = json_find_any(object, keys);
  return value ? json_string_list(*value) : QStringList{};
}

QStringList normalized_serializer_list(const QStringList& values) {
  QStringList out;
  out.reserve(values.size());

  for (const auto& item : values) {
    const auto normalized = normalized_serializer(item);
    if (!normalized.isEmpty()) {
      out << normalized;
    }
  }

  return out;
}

QStringList normalized_url_list(const QStringList& values) {
  QStringList out;
  out.reserve(values.size());

  for (const auto& item : values) {
    const auto normalized = normalized_url(item);
    if (!normalized.isEmpty()) {
      out << normalized;
    }
  }

  return out;
}

QStringList normalized_any_match_list(const QStringList& values) {
  QStringList out;

  for (const auto& item : values) {
    const auto url_value = normalized_url(item);
    if (!url_value.isEmpty() && !out.contains(url_value)) {
      out << url_value;
    }

    const auto serializer_value = normalized_serializer(item);
    if (!serializer_value.isEmpty() && !out.contains(serializer_value)) {
      out << serializer_value;
    }
  }

  return out;
}

QString schema_alias(QString value);

QStringList normalized_exact_list(const QStringList& values) {
  QStringList out;
  out.reserve(values.size());

  for (const auto& item : values) {
    const auto normalized = schema_alias(item);
    if (!normalized.isEmpty()) {
      out << normalized;
    }
  }

  return out;
}

bool contains_any_or_empty(const QString& text, const QStringList& tokens) {
  if (tokens.empty()) {
    return true;
  }

  for (const auto& token : tokens) {
    if (!token.isEmpty() && text.contains(token)) {
      return true;
    }
  }

  return false;
}

bool contains_any_token(const QString& text, const QStringList& tokens) {
  for (const auto& token : tokens) {
    if (!token.isEmpty() && text.contains(token)) {
      return true;
    }
  }

  return false;
}

bool equals_any_or_empty(const QString& text, const QStringList& tokens) {
  return tokens.empty() || tokens.contains(text);
}

bool equals_any_url(const QString& text, const QStringList& tokens) {
  for (const auto& candidate : normalized_url_exact_candidates(text)) {
    if (tokens.contains(candidate)) {
      return true;
    }
  }

  return false;
}

bool equals_any_url_or_empty(const QString& text, const QStringList& tokens) {
  return tokens.empty() || equals_any_url(text, tokens);
}

std::vector<QRegularExpression> compiled_regex_list(const QStringList& patterns) {
  std::vector<QRegularExpression> out;
  out.reserve(patterns.size());

  for (const auto& pattern : patterns) {
    if (pattern.isEmpty()) {
      continue;
    }

    QRegularExpression regex(pattern, QRegularExpression::CaseInsensitiveOption);

    if (regex.isValid()) {
      out.emplace_back(std::move(regex));
    }
  }

  return out;
}

bool regex_any_or_empty(const QString& text, const std::vector<QRegularExpression>& patterns) {
  if (patterns.empty()) {
    return true;
  }

  for (const auto& regex : patterns) {
    if (regex.match(text).hasMatch()) {
      return true;
    }
  }

  return false;
}

bool schema_matches(const QStringList& schemas, const QString& schema) {
  if (schemas.empty()) {
    return true;
  }

  for (const auto& item : schemas) {
    const auto normalized = normalized_serializer(item);
    if (normalized == "any" || normalized == schema) {
      return true;
    }
  }

  return false;
}

template <typename RuleT>
bool rule_matches(const RuleT& rule, const QString& url_text, const QString& ser_text, const QString& combined_text,
                  const QString& schema_text) {
  if (!schema_matches(rule.schema_types, schema_text)) {
    return false;
  }

  if (!contains_any_or_empty(ser_text, rule.serializer_contains)) {
    return false;
  }

  if (!equals_any_or_empty(ser_text, rule.serializer_equals)) {
    return false;
  }

  if (!regex_any_or_empty(ser_text, rule.serializer_regex)) {
    return false;
  }

  if (!contains_any_or_empty(url_text, rule.url_contains)) {
    return false;
  }

  if (!equals_any_url_or_empty(url_text, rule.url_equals)) {
    return false;
  }

  if (!regex_any_or_empty(url_text, rule.url_regex)) {
    return false;
  }

  if (!contains_any_or_empty(combined_text, rule.any_contains)) {
    return false;
  }

  if (!regex_any_or_empty(combined_text, rule.any_regex)) {
    return false;
  }

  return true;
}

void append_unique(QStringList& target, const QStringList& values, bool normalize_for_serializer = false) {
  for (const auto& value : values) {
    auto normalized = normalize_for_serializer ? normalized_serializer(value) : normalized_url(value);
    if (normalized.isEmpty() || target.contains(normalized)) {
      continue;
    }
    target << normalized;
  }
}

QString schema_alias(QString value) {
  value = normalized_serializer(value);

  if (value == "zerocopy" || value == "zcopy") {
    return "zerocopy";
  }

  if (value == "proto" || value == "pb") {
    return "protobuf";
  }

  if (value == "fbs" || value == "flatbuffer") {
    return "flatbuffers";
  }

  return value;
}

void load_parser_field_paths(const Json& object, QHash<QString, QString>& fields) {
  if (!object.is_object()) {
    return;
  }

  for (auto it = object.begin(); it != object.end(); ++it) {
    const auto path = json_path_value(it.value());

    if (path.isEmpty()) {
      continue;
    }

    const auto key = normalized_serializer(QString::fromStdString(it.key()));

    if (!key.isEmpty()) {
      fields.insert(key, path);
    }
  }
}

}  // namespace point3d_config_detail

QString Point3DConfig::render_type_to_string(Point3DRenderType type) {
  switch (type) {
    case Point3DRenderType::kPointCloud:
      return "point_cloud";
    case Point3DRenderType::kObjectDetection:
      return "object_detection";
    case Point3DRenderType::kLaneLine:
      return "lane_line";
    case Point3DRenderType::kPrediction:
      return "prediction";
    case Point3DRenderType::kTrafficLight:
      return "traffic_light";
    case Point3DRenderType::kStopLine:
      return "stop_line";
    case Point3DRenderType::kTrafficSign:
      return "traffic_sign";
    case Point3DRenderType::kFreespace:
      return "freespace";
    case Point3DRenderType::kOccupancyGrid:
      return "occupancy_grid";
    case Point3DRenderType::kParkingSlot:
      return "parking_slot";
    case Point3DRenderType::kEgoTrajectory:
      return "ego_trajectory";
    case Point3DRenderType::kHdMap:
      return "hdmap";
    case Point3DRenderType::kCameraFrustum:
      return "camera_frustum";
    case Point3DRenderType::kCovarianceEllipse:
      return "covariance_ellipse";
    default:
      return "point_cloud";
  }
}

bool Point3DConfig::render_type_from_string(QString value, Point3DRenderType& type) {
  value = point3d_config_detail::normalized_serializer(value);

  if (value == "pointcloud" || value == "pcl") {
    type = Point3DRenderType::kPointCloud;
  } else if (value == "objectdetection" || value == "object" || value == "objects" || value == "od") {
    type = Point3DRenderType::kObjectDetection;
  } else if (value == "laneline" || value == "lane" || value == "lanes" || value == "polyline") {
    type = Point3DRenderType::kLaneLine;
  } else if (value == "prediction" || value == "trajectory" || value == "predictedpath") {
    type = Point3DRenderType::kPrediction;
  } else if (value == "trafficlight" || value == "light") {
    type = Point3DRenderType::kTrafficLight;
  } else if (value == "stopline" || value == "crosswalk") {
    type = Point3DRenderType::kStopLine;
  } else if (value == "trafficsign" || value == "sign") {
    type = Point3DRenderType::kTrafficSign;
  } else if (value == "freespace" || value == "drivablearea") {
    type = Point3DRenderType::kFreespace;
  } else if (value == "occupancygrid" || value == "grid" || value == "costmap") {
    type = Point3DRenderType::kOccupancyGrid;
  } else if (value == "parkingslot" || value == "slot") {
    type = Point3DRenderType::kParkingSlot;
  } else if (value == "egotrajectory" || value == "egopath" || value == "motionplan") {
    type = Point3DRenderType::kEgoTrajectory;
  } else if (value == "hdmap" || value == "map") {
    type = Point3DRenderType::kHdMap;
  } else if (value == "camerafrustum" || value == "camerainfo" || value == "cameracalib") {
    type = Point3DRenderType::kCameraFrustum;
  } else if (value == "covarianceellipse" || value == "covariance") {
    type = Point3DRenderType::kCovarianceEllipse;
  } else {
    return false;
  }

  return true;
}

Point3DConfig Point3DConfig::default_config() {
  Point3DConfig config;

  config.skip_url_contains_ =
      point3d_config_detail::normalized_url_list({"info/", "/status", "/diagnostic", "/heartbeat"});
  config.skip_serializer_contains_ = point3d_config_detail::normalized_serializer_list(
      {"rawimage", "imagemsg", "compressed", "compressedimage", "video", "h264", "h265", "jpg", "png", "jpeg", "audio",
       "sound", "wav", "log", "diagnostic"});
  config.skip_serializer_equals_ = point3d_config_detail::normalized_serializer_list({"string"});

  config.add_render_rule(Point3DRenderType::kObjectDetection, {"zero_copy"}, {"ObjectArray"}, {}, 10000);
  config.add_render_rule(Point3DRenderType::kPointCloud, {"zero_copy"}, {"PointCloud"}, {}, 9990);
  config.add_render_rule(Point3DRenderType::kOccupancyGrid, {"zero_copy"}, {"OccupancyGrid"}, {}, 9980);

  config.add_render_rule(
      Point3DRenderType::kObjectDetection, {},
      {"objectarray",   "detectedobject",      "trackedobject", "radarobject",    "obstacle",      "perceptionobstacle",
       "modelobstacle", "bevobstacle",         "objectlist",    "objlist",        "detectionlist", "detectionpoint",
       "fusionobject",  "objectdata",          "radartrack",    "radarscan",      "odfusion",      "acctarget",
       "radardataod",   "closestinpathobject", "clusterbbox",   "invasionobject", "objectbbox",    "radardataobject",
       "perceptionbox", "ultrasonicstamp"},
      {}, 9000);
  config.add_render_rule(
      Point3DRenderType::kEgoTrajectory, {},
      {"egotrajectory", "motionplan", "lateralplan", "longitudinalplan", "egoplanning", "adasresult"}, {}, 8990);
  config.add_render_rule(Point3DRenderType::kPrediction, {},
                         {"predictedobject", "predictedpath", "trajectory", "trajectories", "planningpath",
                          "navigationresult", "planningresult", "pathwithlaneid", "hparoutepoints", "factorarray",
                          "posearray", "velocityfactor", "steeringfactor", "safetyfactor", "navigationvisualization"},
                         {}, 8980);
  config.add_render_rule(Point3DRenderType::kLaneLine, {},
                         {"lanesegment",    "laneboundary",   "roadmarking",        "lanemarking",    "lanemark",
                          "laneline",       "extendedlane",   "perceptionlane",     "modellane",      "modelstatic",
                          "modeledge",      "visuallane",     "centerline",         "referenceline",  "guideline",
                          "linestring",     "roadinstance",   "roadstructure",      "roadcognition",  "roadedge",
                          "perceptionroad", "lanelet",        "polyline",           "debugline",      "llrrlane",
                          "lrlane",         "trianglelist",   "perceptionboundary", "perceptionline", "boundarydet",
                          "localroute",     "perceptiongate", "modelarea"},
                         {}, 8970);
  config.add_render_rule(Point3DRenderType::kTrafficLight, {}, {"trafficlight", "trafficsignal"}, {}, 8960);
  config.add_render_rule(
      Point3DRenderType::kFreespace, {},
      {"freespace", "drivablearea", "perceptioncell", "polygonstamped", "fspointsmessage", "perceptionarea"}, {}, 8950);
  config.add_render_rule(Point3DRenderType::kOccupancyGrid, {},
                         {"occupancygrid", "costmap", "voxelgrid", "gridmap", "occupiedgrid", "semanticimage",
                          "elevationmap", "localmap", "semanticmap", "hpalocalmap", "submap"},
                         {}, 8940);
  config.add_render_rule(Point3DRenderType::kTrafficSign, {}, {"trafficsign", "hdmapprimitive"}, {}, 8930);
  config.add_render_rule(Point3DRenderType::kStopLine, {},
                         {"stopline", "crosswalk", "stopreason", "areamarking", "arrowmarking", "modelarrow"}, {},
                         8920);
  config.add_render_rule(
      Point3DRenderType::kParkingSlot, {},
      {"parkingslot", "slotdetection", "parkingspace", "visualparkingslot", "modelslot", "slotpoint", "slotline"}, {},
      8910);
  config.add_render_rule(Point3DRenderType::kEgoTrajectory, {}, {"planningvisualization", "adasvisualization"}, {},
                         8900);
  config.add_render_rule(Point3DRenderType::kHdMap, {},
                         {"hdmap", "laneletmap", "hdmapsegment", "laneletroute", "mapdata"}, {}, 8890);
  config.add_render_rule(
      Point3DRenderType::kCameraFrustum, {},
      {"camerainfo", "cameracalib", "camerafrustum", "cameraintrinsic", "cameracalibration", "cameracalibparam"}, {},
      8880);
  config.add_render_rule(Point3DRenderType::kCovarianceEllipse, {},
                         {"covarianceellipse", "posecovariance", "posewithcovariance", "kinematicstate"}, {}, 8870);
  config.add_render_rule(Point3DRenderType::kObjectDetection, {},
                         {"detectedobject3d", "trackedobject3d", "objecttracking", "semanticobject", "instanceobject",
                          "boundingbox3d", "bbox3d", "orientedbox", "graspobject", "detectedperson", "humanobject"},
                         {}, 8860);
  config.add_render_rule(
      Point3DRenderType::kPrediction, {},
      {"navpath", "robotpath", "plannedpath", "referencepath", "goalpath", "motiontrajectory", "robottrajectory",
       "end_effectortrajectory", "endeffectortrajectory", "manipulatortrajectory", "armtrajectory"},
      {}, 8850);
  config.add_render_rule(Point3DRenderType::kEgoTrajectory, {},
                         {"odometry", "robotpose", "basepose", "basefootprint", "localization", "navstate",
                          "robotstate", "posewithtwist", "twistwithcovariance"},
                         {}, 8840);
  config.add_render_rule(Point3DRenderType::kLaneLine, {},
                         {"markerarray", "linestrip", "linelist", "skeleton", "skeleton3d", "keypoint", "keypoints",
                          "handkeypoint", "bodykeypoint"},
                         {}, 8830);
  config.add_render_rule(
      Point3DRenderType::kFreespace, {},
      {"workspace", "reachability", "footprint", "keepoutzone", "safetyzone", "costregion", "drivablepolygon"}, {},
      8820);
  config.add_render_rule(
      Point3DRenderType::kOccupancyGrid, {},
      {"octomap", "voxelmap", "distancefield", "esdf", "tsdf", "semanticgrid", "heightmap", "elevationgrid"}, {}, 8810);
  config.add_render_rule(
      Point3DRenderType::kHdMap, {},
      {"scenegraph", "topologicalmap", "navigationmap", "floorplan", "semanticmap3d", "semanticworld"}, {}, 8800);
  config.add_render_rule(Point3DRenderType::kCameraFrustum, {},
                         {"pinholecamera", "cameramodel", "depthcamera", "rgbdcamera", "sensorcalibration",
                          "extrinsiccalibration", "intrinsiccalibration"},
                         {}, 8790);
  config.add_render_rule(Point3DRenderType::kCovarianceEllipse, {},
                         {"statecovariance", "uncertaintyellipse", "uncertaintyellipsoid", "gaussianpose"}, {}, 8780);

  config.add_render_rule(
      Point3DRenderType::kObjectDetection, {},
      {"dynamicobject", "boundingboxarray", "cloudcluster", "pointobjectarray", "srrfullod", "ultrasonicinfo"}, {},
      8770);
  config.add_render_rule(Point3DRenderType::kPrediction, {},
                         {"decisionresult", "decisiontrajectory", "scenetrajectoryset", "planningfactor"}, {}, 8760);
  config.add_render_rule(Point3DRenderType::kEgoTrajectory, {},
                         {"ekfstate", "liteadasresult", "initialpose", "missiongoal", "locomotionstate",
                          "vehiclekinematics", "endeffectorstate"},
                         {}, 8750);
  config.add_render_rule(
      Point3DRenderType::kHdMap, {},
      {"hadmapbin", "hadmaproute", "routinginfo", "routinginfosd", "sdroute", "hparoutinginfo", "navigationsd"}, {},
      8740);
  config.add_render_rule(Point3DRenderType::kCameraFrustum, {},
                         {"sensorcalibs", "radarcalib", "radarcalibparam", "lidarcalib", "lidarcalibparam"}, {}, 8730);
  config.add_render_rule(Point3DRenderType::kPointCloud, {},
                         {"pointcluster", "pointclusters", "laserscan", "multiecholaserscan"}, {}, 8720);

  config.add_render_rule(Point3DRenderType::kObjectDetection, {}, {},
                         {"/od", "/detected_object", "/tracked_object", "/detection", "/obstacle", "/radar_object",
                          "/fusion_object", "/bev_object", "/object_list", "/perception_result", "/cone"},
                         8000);
  config.add_render_rule(Point3DRenderType::kPrediction, {}, {},
                         {"/predicted_object", "/prediction", "/trajectory", "/forecast", "/predicted_path",
                          "/planning_path", "/planned_trajectory"},
                         7990);
  config.add_render_rule(Point3DRenderType::kLaneLine, {}, {},
                         {"/lane", "/boundary", "/road_marking", "/centerline", "/lane_line", "/road_edge",
                          "/reference_line", "/road_structure"},
                         7980);
  config.add_render_rule(Point3DRenderType::kTrafficLight, {}, {}, {"/traffic_light", "/traffic_signal"}, 7970);
  config.add_render_rule(Point3DRenderType::kStopLine, {}, {}, {"/stop_line", "/crosswalk"}, 7960);
  config.add_render_rule(Point3DRenderType::kTrafficSign, {}, {},
                         {"/traffic_sign", "/sign", "/pole", "/signal_pole", "/lamp_post"}, 7950);
  config.add_render_rule(Point3DRenderType::kStopLine, {}, {},
                         {"/zebra", "/arrow", "/road_text", "/diamond", "/yield", "/speed_bump"}, 7940);
  config.add_render_rule(Point3DRenderType::kFreespace, {}, {}, {"/freespace", "/free_space", "/drivable_area"}, 7930);
  config.add_render_rule(Point3DRenderType::kOccupancyGrid, {}, {}, {"/occupancy", "/costmap", "/grid_map", "/voxel"},
                         7920);
  config.add_render_rule(Point3DRenderType::kLaneLine, {}, {},
                         {"/virtual_wall", "/geofence", "/no_go_zone", "/restricted", "/gate", "/guardrail"}, 7910);
  config.add_render_rule(Point3DRenderType::kParkingSlot, {}, {}, {"/parking_slot", "/slot", "/parking_space"}, 7900);
  config.add_render_rule(Point3DRenderType::kEgoTrajectory, {}, {},
                         {"/ego_trajectory", "/motion_plan", "/ego_path", "/planned_path"}, 7890);
  config.add_render_rule(Point3DRenderType::kHdMap, {}, {}, {"/hd_map", "/lanelet_map", "/map_segment"}, 7880);
  config.add_render_rule(Point3DRenderType::kCameraFrustum, {}, {}, {"/camera_info", "/camera_calib"}, 7870);
  config.add_render_rule(Point3DRenderType::kObjectDetection, {}, {},
                         {"/bbox", "/bbox3d", "/bounding_box", "/semantic_object", "/tracked_person", "/grasp"}, 7860);
  config.add_render_rule(Point3DRenderType::kPrediction, {}, {},
                         {"/nav_path", "/robot_path", "/goal_path", "/reference_path", "/arm_trajectory",
                          "/manipulator_trajectory", "/end_effector_trajectory"},
                         7850);
  config.add_render_rule(Point3DRenderType::kEgoTrajectory, {}, {},
                         {"/odom", "/odometry", "/robot_pose", "/base_pose", "/localization", "/nav_state"}, 7840);
  config.add_render_rule(
      Point3DRenderType::kLaneLine, {}, {},
      {"/marker_array", "/line_strip", "/line_list", "/skeleton", "/keypoints", "/body_keypoints", "/hand_keypoints"},
      7830);
  config.add_render_rule(Point3DRenderType::kFreespace, {}, {},
                         {"/workspace", "/reachability", "/footprint", "/keepout", "/safety_zone"}, 7820);
  config.add_render_rule(Point3DRenderType::kOccupancyGrid, {}, {},
                         {"/octomap", "/voxel_map", "/distance_field", "/esdf", "/tsdf", "/height_map"}, 7810);
  config.add_render_rule(Point3DRenderType::kHdMap, {}, {},
                         {"/scene_graph", "/topological_map", "/navigation_map", "/floorplan"}, 7800);
  config.add_render_rule(Point3DRenderType::kPrediction, {}, {},
                         {"/decision_result", "/decision_trajectory", "/scene_trajectory"}, 7790);
  config.add_render_rule(Point3DRenderType::kHdMap, {}, {},
                         {"/routing_info", "/routing_result", "/sd_route", "/navigation_sd", "/had_map"}, 7780);
  config.add_render_rule(Point3DRenderType::kCameraFrustum, {}, {}, {"/sensor_calib", "/radar_calib", "/lidar_calib"},
                         7770);
  config.add_render_rule(Point3DRenderType::kEgoTrajectory, {}, {},
                         {"/ekf_state", "/vehicle_pose", "/initial_pose", "/mission_goal"}, 7760);
  config.add_collection_hint(Point3DRenderType::kObjectDetection,
                             {"obstacles", "objects", "detections", "targets", "tracks", "boxes", "bboxes",
                              "boundingboxes", "instances", "graspobjects"});
  config.add_collection_hint(Point3DRenderType::kLaneLine,
                             {"lanes", "markings", "boundaries", "lines", "segments", "centerlines", "polylines",
                              "edges", "referencelines", "referencepoints", "markers", "keypoints", "skeletons"});
  config.add_collection_hint(Point3DRenderType::kPrediction, {"predictions", "trajectories", "predictedpaths",
                                                              "waypoints", "pathpoints", "paths", "navpaths", "goals"});
  config.add_collection_hint(Point3DRenderType::kParkingSlot, {"slots", "parkingslots", "corners"});
  config.add_collection_hint(Point3DRenderType::kTrafficLight, {"lights", "trafficlights", "signals", "elements"});
  config.add_collection_hint(Point3DRenderType::kFreespace,
                             {"areas", "regions", "zones", "workspaces", "footprints", "keepoutzones"});
  config.add_collection_hint(Point3DRenderType::kStopLine, {"stoplinesarr", "stoplines", "crosswalks"});
  config.add_collection_hint(Point3DRenderType::kTrafficSign, {"signs", "trafficsigns"});
  config.add_collection_hint(Point3DRenderType::kOccupancyGrid, {"gridmap", "cells", "grid"});
  config.add_collection_hint(Point3DRenderType::kObjectDetection, {"dynamicobjects", "clusters", "cloudclusters"});
  config.add_collection_hint(Point3DRenderType::kHdMap, {"lanelets", "primitives", "routesegments"});

  return config;
}

bool Point3DConfig::load_from_file(const QString& path, QString* error) {
  if (error) {
    error->clear();
  }

  std::ifstream file(path.toStdString());

  if (!file.is_open()) {
    if (error) {
      *error = "Can not open config file";
    }
    return false;
  }

  Json root = Json::parse(file, nullptr, false);

  if (root.is_discarded() || !root.is_object()) {
    if (error) {
      *error = "Invalid Point3D JSON config";
    }
    return false;
  }

  Point3DConfig next = point3d_config_detail::json_bool_value(root, {"inherit_default", "extend_default"}, true)
                           ? Point3DConfig::default_config()
                           : Point3DConfig();

  const auto* skip = point3d_config_detail::json_find_any(root, {"skip", "ignore"});
  if (skip && skip->is_object()) {
    if (point3d_config_detail::json_bool_value(*skip, {"replace", "override"}, false)) {
      next.skip_url_contains_.clear();
      next.skip_serializer_contains_.clear();
      next.skip_url_equals_.clear();
      next.skip_serializer_equals_.clear();
    }

    point3d_config_detail::append_unique(next.skip_url_contains_,
                                         point3d_config_detail::json_string_list_any(*skip, {"url_contains", "urls"}));
    point3d_config_detail::append_unique(
        next.skip_serializer_contains_,
        point3d_config_detail::json_string_list_any(
            *skip, {"ser_contains", "serializer_contains", "type_contains", "serializers"}),
        true);
    point3d_config_detail::append_unique(
        next.skip_url_equals_, point3d_config_detail::json_string_list_any(*skip, {"url_equals", "topic_equals"}));
    point3d_config_detail::append_unique(
        next.skip_serializer_equals_,
        point3d_config_detail::json_string_list_any(*skip, {"ser_equals", "serializer_equals", "type_equals_exact"}),
        true);
  }

  auto load_rule_array = [&next](const Json& render_rules) {
    if (!render_rules.is_array()) {
      return;
    }

    int index = 0;
    for (const auto& object : render_rules) {
      if (!object.is_object()) {
        ++index;
        continue;
      }

      const auto* match = point3d_config_detail::json_find_any(object, {"match", "when"});
      const auto& match_object = (match && match->is_object()) ? *match : object;

      Point3DRenderType type;

      if (!Point3DConfig::render_type_from_string(
              point3d_config_detail::json_string_value(object, {"type", "render_type"}), type)) {
        ++index;
        continue;
      }

      const int priority = point3d_config_detail::json_int_value(object, {"priority"}, 20000 - index);
      auto schema = point3d_config_detail::json_string_list_any(match_object,
                                                                {"schema", "schemas", "schema_type", "schema_types"});
      auto ser_contains = point3d_config_detail::json_string_list_any(
          match_object, {"ser_contains", "serializer_contains", "type_contains"});
      auto url_contains = point3d_config_detail::json_string_list_any(match_object, {"url_contains", "topic_contains"});
      auto ser_equals =
          point3d_config_detail::json_string_list_any(match_object, {"ser_equals", "serializer_equals", "type_equals"});
      auto url_equals = point3d_config_detail::json_string_list_any(match_object, {"url_equals", "topic_equals"});
      auto any_contains =
          point3d_config_detail::json_string_list_any(match_object, {"any_contains", "combined_contains"});
      auto ser_regex =
          point3d_config_detail::json_string_list_any(match_object, {"ser_regex", "serializer_regex", "type_regex"});
      auto url_regex = point3d_config_detail::json_string_list_any(match_object, {"url_regex", "topic_regex"});
      auto any_regex = point3d_config_detail::json_string_list_any(match_object, {"any_regex", "combined_regex"});

      next.add_render_rule(type, schema, ser_contains, url_contains, priority,
                           point3d_config_detail::json_string_value(object, {"name"}),
                           point3d_config_detail::json_string_value(object, {"domain"}), ser_equals, url_equals,
                           any_contains, ser_regex, url_regex, any_regex);
      ++index;
    }
  };

  if (const auto* rules = point3d_config_detail::json_find_any(root, {"rules", "render_rules"})) {
    load_rule_array(*rules);
  }

  if (const auto* parser_rules =
          point3d_config_detail::json_find_any(root, {"parser_rules", "schema_rules", "decode_rules"})) {
    if (parser_rules->is_array()) {
      int index = 0;

      for (const auto& object : *parser_rules) {
        if (!object.is_object()) {
          ++index;
          continue;
        }

        Point3DRenderType type;

        if (!Point3DConfig::render_type_from_string(
                point3d_config_detail::json_string_value(object, {"type", "render_type"}), type)) {
          ++index;
          continue;
        }

        const auto* match = point3d_config_detail::json_find_any(object, {"match", "when"});
        const auto& match_object = (match && match->is_object()) ? *match : object;
        const auto* fields_object = point3d_config_detail::json_find_any(object, {"fields", "field_paths", "paths"});

        Point3DConfig::ParserRule rule;
        rule.name = point3d_config_detail::json_string_value(object, {"name"});
        rule.domain = point3d_config_detail::json_string_value(object, {"domain"});
        rule.type = type;
        rule.priority = point3d_config_detail::json_int_value(object, {"priority"}, 20000 - index);
        rule.schema_types = point3d_config_detail::normalized_exact_list(point3d_config_detail::json_string_list_any(
            match_object, {"schema", "schemas", "schema_type", "schema_types"}));
        rule.serializer_contains =
            point3d_config_detail::normalized_serializer_list(point3d_config_detail::json_string_list_any(
                match_object, {"ser_contains", "serializer_contains", "type_contains"}));
        rule.serializer_equals =
            point3d_config_detail::normalized_serializer_list(point3d_config_detail::json_string_list_any(
                match_object, {"ser_equals", "serializer_equals", "type_equals"}));
        rule.serializer_regex = point3d_config_detail::compiled_regex_list(
            point3d_config_detail::json_string_list_any(match_object, {"ser_regex", "serializer_regex", "type_regex"}));
        rule.url_contains = point3d_config_detail::normalized_url_list(
            point3d_config_detail::json_string_list_any(match_object, {"url_contains", "topic_contains"}));
        rule.url_equals = point3d_config_detail::normalized_url_list(
            point3d_config_detail::json_string_list_any(match_object, {"url_equals", "topic_equals"}));
        rule.url_regex = point3d_config_detail::compiled_regex_list(
            point3d_config_detail::json_string_list_any(match_object, {"url_regex", "topic_regex"}));
        rule.any_contains = point3d_config_detail::normalized_any_match_list(
            point3d_config_detail::json_string_list_any(match_object, {"any_contains", "combined_contains"}));
        rule.any_regex = point3d_config_detail::compiled_regex_list(
            point3d_config_detail::json_string_list_any(match_object, {"any_regex", "combined_regex"}));

        rule.collection_path = point3d_config_detail::json_path_string_value(
            object, {"collection", "collection_path", "repeated", "repeated_path", "objects", "outer"});
        rule.inner_collection_path = point3d_config_detail::json_path_string_value(
            object, {"inner_collection", "inner_collection_path", "inner_repeated", "inner_repeated_path", "points",
                     "point_collection"});

        point3d_config_detail::load_parser_field_paths(object, rule.fields);

        if (fields_object && fields_object->is_object()) {
          rule.collection_path = point3d_config_detail::json_path_string_value(
              *fields_object, {"collection", "collection_path", "repeated", "repeated_path", "objects", "outer"},
              rule.collection_path);
          rule.inner_collection_path = point3d_config_detail::json_path_string_value(
              *fields_object,
              {"inner_collection", "inner_collection_path", "inner_repeated", "inner_repeated_path", "points",
               "point_collection"},
              rule.inner_collection_path);
          point3d_config_detail::load_parser_field_paths(*fields_object, rule.fields);
        }
        rule.fields.remove("match");
        rule.fields.remove("when");
        rule.fields.remove("fields");
        rule.fields.remove("fieldpaths");
        rule.fields.remove("paths");
        rule.fields.remove("collection");
        rule.fields.remove("collectionpath");
        rule.fields.remove("repeated");
        rule.fields.remove("repeatedpath");
        rule.fields.remove("objects");
        rule.fields.remove("outer");
        rule.fields.remove("innercollection");
        rule.fields.remove("innercollectionpath");
        rule.fields.remove("innerrepeated");
        rule.fields.remove("innerrepeatedpath");
        rule.fields.remove("points");
        rule.fields.remove("pointcollection");
        rule.fields.remove("name");
        rule.fields.remove("domain");
        rule.fields.remove("type");
        rule.fields.remove("rendertype");
        rule.fields.remove("priority");
        rule.fields.remove("schema");
        rule.fields.remove("schemas");
        rule.fields.remove("schematype");
        rule.fields.remove("schematypes");
        rule.fields.remove("sercontains");
        rule.fields.remove("serializercontains");
        rule.fields.remove("typecontains");
        rule.fields.remove("serequals");
        rule.fields.remove("serializerequals");
        rule.fields.remove("typeequals");
        rule.fields.remove("serregex");
        rule.fields.remove("serializerregex");
        rule.fields.remove("typeregex");
        rule.fields.remove("urlcontains");
        rule.fields.remove("topiccontains");
        rule.fields.remove("urlequals");
        rule.fields.remove("topicequals");
        rule.fields.remove("urlregex");
        rule.fields.remove("topicregex");
        rule.fields.remove("anycontains");
        rule.fields.remove("combinedcontains");
        rule.fields.remove("anyregex");
        rule.fields.remove("combinedregex");

        next.add_parser_rule(std::move(rule));
        ++index;
      }
    }
  }

  const auto* field_hints =
      point3d_config_detail::json_find_any(root, {"field_hints", "collection_hints", "collections"});
  if (field_hints && field_hints->is_object()) {
    if (point3d_config_detail::json_bool_value(*field_hints, {"replace", "override"}, false)) {
      next.collection_hints_.clear();
    }

    for (auto it = field_hints->begin(); it != field_hints->end(); ++it) {
      if (it.key() == "replace" || it.key() == "override") {
        continue;
      }

      Point3DRenderType type;
      if (!render_type_from_string(QString::fromStdString(it.key()), type)) {
        continue;
      }

      next.add_collection_hint(type, point3d_config_detail::json_string_list(it.value()));
    }
  } else if (field_hints && field_hints->is_array()) {
    for (const auto& object : *field_hints) {
      if (!object.is_object()) {
        continue;
      }

      Point3DRenderType type;
      if (!render_type_from_string(point3d_config_detail::json_string_value(object, {"type", "render_type"}), type)) {
        continue;
      }

      next.add_collection_hint(type,
                               point3d_config_detail::json_string_list_any(object, {"names", "field_names", "fields"}));
    }
  }

  next.source_path_ = path;
  *this = std::move(next);
  return true;
}

Point3DRenderType Point3DConfig::detect_render_type(const std::string& url, const std::string& ser,
                                                    vlink::SchemaType schema_type) const {
  const QString url_text = point3d_config_detail::normalized_url(QString::fromStdString(url));
  const QString ser_text = point3d_config_detail::normalized_serializer(QString::fromStdString(ser));
  const QString combined_text = ser_text + " " + url_text;
  const QString schema_text = schema_type_to_string(schema_type);

  const RenderRule* best_rule = nullptr;

  for (const auto& rule : render_rules_) {
    if (!point3d_config_detail::rule_matches(rule, url_text, ser_text, combined_text, schema_text)) {
      continue;
    }

    if (!best_rule || rule.priority > best_rule->priority) {
      best_rule = &rule;
    }
  }

  const ParserRule* best_parser_rule = nullptr;

  for (const auto& rule : parser_rules_) {
    if (!point3d_config_detail::rule_matches(rule, url_text, ser_text, combined_text, schema_text)) {
      continue;
    }

    if (!best_parser_rule || rule.priority > best_parser_rule->priority) {
      best_parser_rule = &rule;
    }
  }

  if (best_parser_rule && (!best_rule || best_parser_rule->priority > best_rule->priority)) {
    return best_parser_rule->type;
  }

  return best_rule ? best_rule->type : Point3DRenderType::kPointCloud;
}

Point3DRenderType Point3DConfig::collection_render_type(const std::string& normalized_name,
                                                        Point3DRenderType fallback) const {
  const auto name = point3d_config_detail::normalized_serializer(QString::fromStdString(normalized_name));

  for (const auto& hint : collection_hints_) {
    if (hint.field_names.contains(name)) {
      return hint.type;
    }
  }

  return fallback;
}

const Point3DConfig::ParserRule* Point3DConfig::parser_rule_for(const std::string& url, const std::string& ser,
                                                                vlink::SchemaType schema_type,
                                                                Point3DRenderType type) const {
  const QString url_text = point3d_config_detail::normalized_url(QString::fromStdString(url));
  const QString ser_text = point3d_config_detail::normalized_serializer(QString::fromStdString(ser));
  const QString combined_text = ser_text + " " + url_text;
  const QString schema_text = schema_type_to_string(schema_type);

  const ParserRule* best_rule = nullptr;

  for (const auto& rule : parser_rules_) {
    if (rule.type != type) {
      continue;
    }

    if (!point3d_config_detail::rule_matches(rule, url_text, ser_text, combined_text, schema_text)) {
      continue;
    }

    if (!best_rule || rule.priority > best_rule->priority) {
      best_rule = &rule;
    }
  }

  return best_rule;
}

bool Point3DConfig::should_skip(const QString& url, const QString& ser) const {
  return point3d_config_detail::contains_any_token(point3d_config_detail::normalized_url(url), skip_url_contains_) ||
         point3d_config_detail::contains_any_token(point3d_config_detail::normalized_serializer(ser),
                                                   skip_serializer_contains_) ||
         point3d_config_detail::equals_any_url(point3d_config_detail::normalized_url(url), skip_url_equals_) ||
         skip_serializer_equals_.contains(point3d_config_detail::normalized_serializer(ser));
}

QString Point3DConfig::source_label() const {
  if (source_path_.isEmpty()) {
    return "Default";
  }

  return QFileInfo(source_path_).fileName();
}

void Point3DConfig::add_render_rule(Point3DRenderType type, QStringList schemas, QStringList serializer_contains,
                                    QStringList url_contains, int priority, QString name, QString domain,
                                    QStringList serializer_equals, QStringList url_equals, QStringList any_contains,
                                    QStringList serializer_regex, QStringList url_regex, QStringList any_regex) {
  RenderRule rule;
  rule.name = std::move(name);
  rule.domain = std::move(domain);
  rule.type = type;
  rule.schema_types = point3d_config_detail::normalized_exact_list(schemas);
  rule.serializer_contains = point3d_config_detail::normalized_serializer_list(serializer_contains);
  rule.serializer_equals = point3d_config_detail::normalized_serializer_list(serializer_equals);
  rule.serializer_regex = point3d_config_detail::compiled_regex_list(serializer_regex);
  rule.url_contains = point3d_config_detail::normalized_url_list(url_contains);
  rule.url_equals = point3d_config_detail::normalized_url_list(url_equals);
  rule.url_regex = point3d_config_detail::compiled_regex_list(url_regex);
  rule.any_contains = point3d_config_detail::normalized_any_match_list(any_contains);
  rule.any_regex = point3d_config_detail::compiled_regex_list(any_regex);
  rule.priority = priority;
  render_rules_.emplace_back(std::move(rule));
}

void Point3DConfig::add_collection_hint(Point3DRenderType type, QStringList field_names) {
  CollectionHint hint;
  hint.type = type;
  hint.field_names = point3d_config_detail::normalized_serializer_list(field_names);

  if (hint.field_names.empty()) {
    return;
  }

  for (auto& existing : collection_hints_) {
    for (const auto& name : hint.field_names) {
      existing.field_names.removeAll(name);
    }
  }

  collection_hints_.erase(std::remove_if(collection_hints_.begin(), collection_hints_.end(),
                                         [](const CollectionHint& h) { return h.field_names.empty(); }),
                          collection_hints_.end());

  collection_hints_.emplace_back(std::move(hint));
}

void Point3DConfig::add_parser_rule(ParserRule rule) {
  if (rule.collection_path.isEmpty() && rule.inner_collection_path.isEmpty() && rule.fields.empty()) {
    return;
  }

  parser_rules_.emplace_back(std::move(rule));
}

QString Point3DConfig::schema_type_to_string(vlink::SchemaType schema_type) {
  switch (schema_type) {
    case vlink::SchemaType::kZeroCopy:
      return "zerocopy";
    case vlink::SchemaType::kProtobuf:
      return "protobuf";
    case vlink::SchemaType::kFlatbuffers:
      return "flatbuffers";
    case vlink::SchemaType::kRaw:
      return "raw";
    default:
      return "unknown";
  }
}

// NOLINTEND
