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

#include "./perception_config.h"

#include <QFileInfo>
#include <algorithm>
#include <fstream>
#include <nlohmann/json.hpp>
#include <utility>

using Json = nlohmann::json;

namespace perception_config_detail {

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

QStringList normalized_schema_list(const QStringList& values) {
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

bool match_spec_matches(const PerceptionConfig::MatchSpec& spec, const QString& url_text, const QString& ser_text,
                        const QString& combined_text, const QString& schema_text) {
  if (!schema_matches(spec.schema_types, schema_text)) {
    return false;
  }

  if (!contains_any_or_empty(ser_text, spec.serializer_contains)) {
    return false;
  }

  if (!equals_any_or_empty(ser_text, spec.serializer_equals)) {
    return false;
  }

  if (!regex_any_or_empty(ser_text, spec.serializer_regex)) {
    return false;
  }

  if (!contains_any_or_empty(url_text, spec.url_contains)) {
    return false;
  }

  if (!equals_any_url_or_empty(url_text, spec.url_equals)) {
    return false;
  }

  if (!regex_any_or_empty(url_text, spec.url_regex)) {
    return false;
  }

  if (!contains_any_or_empty(combined_text, spec.any_contains)) {
    return false;
  }

  if (!regex_any_or_empty(combined_text, spec.any_regex)) {
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

PerceptionConfig::MatchSpec load_match_spec(const Json& object) {
  const auto* match = json_find_any(object, {"match", "when"});
  const auto& match_object = (match && match->is_object()) ? *match : object;

  PerceptionConfig::MatchSpec spec;
  spec.schema_types =
      normalized_schema_list(json_string_list_any(match_object, {"schema", "schemas", "schema_type", "schema_types"}));
  spec.serializer_contains = normalized_serializer_list(
      json_string_list_any(match_object, {"ser_contains", "serializer_contains", "type_contains"}));
  spec.serializer_equals = normalized_serializer_list(
      json_string_list_any(match_object, {"ser_equals", "serializer_equals", "type_equals"}));
  spec.serializer_regex =
      compiled_regex_list(json_string_list_any(match_object, {"ser_regex", "serializer_regex", "type_regex"}));
  spec.url_contains = normalized_url_list(json_string_list_any(match_object, {"url_contains", "topic_contains"}));
  spec.url_equals = normalized_url_list(json_string_list_any(match_object, {"url_equals", "topic_equals"}));
  spec.url_regex = compiled_regex_list(json_string_list_any(match_object, {"url_regex", "topic_regex"}));
  spec.any_contains =
      normalized_any_match_list(json_string_list_any(match_object, {"any_contains", "combined_contains"}));
  spec.any_regex = compiled_regex_list(json_string_list_any(match_object, {"any_regex", "combined_regex"}));

  if (const auto* url = json_find_any(match_object, {"url"})) {
    if (url->is_object()) {
      append_unique(spec.url_contains, json_string_list_any(*url, {"whitelist"}));
    } else {
      append_unique(spec.url_contains, json_string_list(*url));
    }
  }

  return spec;
}

void load_field_mappings(const Json& object, std::vector<perception::FieldMapping>& out) {
  const auto* value = json_find_any(object, {"field_mappings", "fields"});

  if (!value || !value->is_array()) {
    return;
  }

  out.reserve(out.size() + value->size());

  for (const auto& fm : *value) {
    if (!fm.is_object()) {
      continue;
    }

    perception::FieldMapping field;
    field.source = fm.value("source", std::string());
    field.target = fm.value("target", std::string());
    field.expression = fm.value("expression", std::string());

    if (fm.contains("default_value")) {
      field.has_default_value = true;

      if (fm["default_value"].is_string()) {
        field.default_value = fm["default_value"].get<std::string>();
        field.default_value_is_string = true;
      } else {
        field.default_value = fm["default_value"].dump();
      }
    }

    if (field.target.empty()) {
      continue;
    }

    if (field.source.empty() && field.expression.empty() && !field.has_default_value) {
      continue;
    }

    out.emplace_back(std::move(field));
  }
}

Json dump_string_list(const QStringList& values) {
  Json out = Json::array();
  for (const auto& value : values) {
    out.emplace_back(value.toStdString());
  }
  return out;
}

Json dump_regex_list(const std::vector<QRegularExpression>& values) {
  Json out = Json::array();
  for (const auto& value : values) {
    out.emplace_back(value.pattern().toStdString());
  }
  return out;
}

Json dump_match_spec(const PerceptionConfig::MatchSpec& spec) {
  Json out = Json::object();

  if (!spec.schema_types.empty()) {
    out["schemas"] = dump_string_list(spec.schema_types);
  }
  if (!spec.serializer_contains.empty()) {
    out["ser_contains"] = dump_string_list(spec.serializer_contains);
  }
  if (!spec.serializer_equals.empty()) {
    out["ser_equals"] = dump_string_list(spec.serializer_equals);
  }
  if (!spec.serializer_regex.empty()) {
    out["ser_regex"] = dump_regex_list(spec.serializer_regex);
  }
  if (!spec.url_contains.empty()) {
    out["url_contains"] = dump_string_list(spec.url_contains);
  }
  if (!spec.url_equals.empty()) {
    out["url_equals"] = dump_string_list(spec.url_equals);
  }
  if (!spec.url_regex.empty()) {
    out["url_regex"] = dump_regex_list(spec.url_regex);
  }
  if (!spec.any_contains.empty()) {
    out["any_contains"] = dump_string_list(spec.any_contains);
  }
  if (!spec.any_regex.empty()) {
    out["any_regex"] = dump_regex_list(spec.any_regex);
  }

  return out;
}

Json dump_field_mappings(const std::vector<perception::FieldMapping>& mappings) {
  Json out = Json::array();

  for (const auto& field : mappings) {
    Json fm = Json::object();

    if (!field.source.empty()) {
      fm["source"] = field.source;
    }

    fm["target"] = field.target;

    if (!field.expression.empty()) {
      fm["expression"] = field.expression;
    }

    if (field.has_default_value) {
      if (field.default_value_is_string) {
        fm["default_value"] = field.default_value;
      } else {
        fm["default_value"] = Json::parse(field.default_value, nullptr, false);
      }
    }

    out.emplace_back(std::move(fm));
  }

  return out;
}

}  // namespace perception_config_detail

namespace pcd = perception_config_detail;

QString PerceptionConfig::render_type_to_string(perception::RenderType type) {
  switch (type) {
    case perception::RenderType::kPointCloud:
      return "point_cloud";
    case perception::RenderType::kObjectDetection:
      return "object_detection";
    case perception::RenderType::kLaneLine:
      return "lane_line";
    case perception::RenderType::kPrediction:
      return "prediction";
    case perception::RenderType::kTrafficLight:
      return "traffic_light";
    case perception::RenderType::kStopLine:
      return "stop_line";
    case perception::RenderType::kTrafficSign:
      return "traffic_sign";
    case perception::RenderType::kFreespace:
      return "freespace";
    case perception::RenderType::kOccupancyGrid:
      return "occupancy_grid";
    case perception::RenderType::kParkingSlot:
      return "parking_slot";
    case perception::RenderType::kEgoTrajectory:
      return "ego_trajectory";
    case perception::RenderType::kHdMap:
      return "hdmap";
    case perception::RenderType::kCameraFrustum:
      return "camera_frustum";
    case perception::RenderType::kCovarianceEllipse:
      return "covariance_ellipse";
    default:
      return "point_cloud";
  }
}

bool PerceptionConfig::render_type_from_string(QString value, perception::RenderType& type) {
  value = pcd::normalized_serializer(value);

  if (value == "pointcloud" || value == "pcl") {
    type = perception::RenderType::kPointCloud;
  } else if (value == "objectdetection" || value == "object" || value == "objects" || value == "od") {
    type = perception::RenderType::kObjectDetection;
  } else if (value == "laneline" || value == "lane" || value == "lanes" || value == "polyline") {
    type = perception::RenderType::kLaneLine;
  } else if (value == "prediction" || value == "trajectory" || value == "predictedpath") {
    type = perception::RenderType::kPrediction;
  } else if (value == "trafficlight" || value == "light") {
    type = perception::RenderType::kTrafficLight;
  } else if (value == "stopline" || value == "crosswalk") {
    type = perception::RenderType::kStopLine;
  } else if (value == "trafficsign" || value == "sign") {
    type = perception::RenderType::kTrafficSign;
  } else if (value == "freespace" || value == "drivablearea") {
    type = perception::RenderType::kFreespace;
  } else if (value == "occupancygrid" || value == "grid" || value == "costmap") {
    type = perception::RenderType::kOccupancyGrid;
  } else if (value == "parkingslot" || value == "slot") {
    type = perception::RenderType::kParkingSlot;
  } else if (value == "egotrajectory" || value == "egopath" || value == "motionplan") {
    type = perception::RenderType::kEgoTrajectory;
  } else if (value == "hdmap" || value == "map") {
    type = perception::RenderType::kHdMap;
  } else if (value == "camerafrustum" || value == "camerainfo" || value == "cameracalib") {
    type = perception::RenderType::kCameraFrustum;
  } else if (value == "covarianceellipse" || value == "covariance") {
    type = perception::RenderType::kCovarianceEllipse;
  } else {
    return false;
  }

  return true;
}

QString PerceptionConfig::encoding_to_string(perception::Encoding encoding) {
  switch (encoding) {
    case perception::Encoding::kProtobuf:
      return "protobuf";
    case perception::Encoding::kFlatbuffers:
      return "flatbuffers";
    case perception::Encoding::kZeroCopy:
      return "zero_copy";
    default:
      return "any";
  }
}

perception::Encoding PerceptionConfig::encoding_from_string(QString value) {
  value = pcd::schema_alias(value);

  if (value == "protobuf") {
    return perception::Encoding::kProtobuf;
  }

  if (value == "flatbuffers") {
    return perception::Encoding::kFlatbuffers;
  }

  if (value == "zerocopy") {
    return perception::Encoding::kZeroCopy;
  }

  return perception::Encoding::kUnknown;
}

const QStringList& PerceptionConfig::target_slots_for(perception::RenderType type) {
  static const QStringList kObject{"x",        "y",        "z",  "length", "width", "height", "yaw",  "score",
                                   "class_id", "track_id", "vx", "vy",     "vz",    "label",  "color"};
  static const QStringList kPolyline{"x", "y", "z", "color", "type", "label"};
  static const QStringList kPrediction{"x", "y", "z", "color", "type", "label", "track_id", "confidence"};
  static const QStringList kEgoTrajectory{"x", "y", "z", "yaw", "speed", "timestamp", "color", "type"};
  static const QStringList kTrafficLight{"x", "y", "z", "color_state", "confidence", "countdown", "label", "color"};
  static const QStringList kTrafficSign{"x", "y", "z", "type_id", "marker_size", "color", "label"};
  static const QStringList kParkingSlot{"corner0_x", "corner0_y", "corner0_z", "corner1_x", "corner1_y", "corner1_z",
                                        "corner2_x", "corner2_y", "corner2_z", "corner3_x", "corner3_y", "corner3_z",
                                        "slot_id",   "slot_type", "color",     "confidence"};
  static const QStringList kCameraFrustum{"x",  "y",     "z",     "qx",   "qy",  "qz",
                                          "qw", "fov_h", "fov_v", "near", "far", "color"};
  static const QStringList kCovariance{"x", "y", "z", "cov_xx", "cov_xy", "cov_yy", "color"};
  static const QStringList kGrid{"origin_x", "origin_y", "origin_z", "resolution", "width", "height", "cells"};
  static const QStringList kPointCloud{"x", "y", "z", "intensity"};
  static const QStringList kEmpty;

  switch (type) {
    case perception::RenderType::kPointCloud:
      return kPointCloud;
    case perception::RenderType::kObjectDetection:
      return kObject;
    case perception::RenderType::kLaneLine:
    case perception::RenderType::kStopLine:
    case perception::RenderType::kFreespace:
    case perception::RenderType::kHdMap:
      return kPolyline;
    case perception::RenderType::kPrediction:
      return kPrediction;
    case perception::RenderType::kEgoTrajectory:
      return kEgoTrajectory;
    case perception::RenderType::kTrafficLight:
      return kTrafficLight;
    case perception::RenderType::kTrafficSign:
      return kTrafficSign;
    case perception::RenderType::kParkingSlot:
      return kParkingSlot;
    case perception::RenderType::kCameraFrustum:
      return kCameraFrustum;
    case perception::RenderType::kCovarianceEllipse:
      return kCovariance;
    case perception::RenderType::kOccupancyGrid:
      return kGrid;
    default:
      return kEmpty;
  }
}

QString PerceptionConfig::schema_type_to_string(vlink::SchemaType schema_type) {
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

void PerceptionConfig::finalize_mapping(MappingRule& rule) {
  rule.ser_normalized = pcd::normalized_serializer(rule.ser);
}

PerceptionConfig PerceptionConfig::default_config() {
  PerceptionConfig config;

  config.skip_url_contains_ = pcd::normalized_url_list({"info/", "/status", "/diagnostic", "/heartbeat"});
  config.skip_serializer_contains_ = pcd::normalized_serializer_list(
      {"rawimage", "imagemsg", "compressed", "compressedimage", "video", "h264", "h265", "jpg", "png", "jpeg",
       "cameraframe", "audio", "sound", "wav", "log", "diagnostic"});
  config.skip_serializer_equals_ = pcd::normalized_serializer_list({"string"});

  using perception::RenderType;

  config.add_render_rule(RenderType::kObjectDetection, {"zero_copy"}, {"ObjectArray"}, {}, 10000);
  config.add_render_rule(RenderType::kPointCloud, {"zero_copy"}, {"PointCloud"}, {}, 9990);
  config.add_render_rule(RenderType::kOccupancyGrid, {"zero_copy"}, {"OccupancyGrid"}, {}, 9980);

  config.add_render_rule(
      RenderType::kObjectDetection, {},
      {"objectarray",   "detectedobject",      "trackedobject", "radarobject",    "obstacle",      "perceptionobstacle",
       "modelobstacle", "bevobstacle",         "objectlist",    "objlist",        "detectionlist", "detectionpoint",
       "fusionobject",  "objectdata",          "radartrack",    "radarscan",      "odfusion",      "acctarget",
       "radardataod",   "closestinpathobject", "clusterbbox",   "invasionobject", "objectbbox",    "radardataobject",
       "perceptionbox", "ultrasonicstamp"},
      {}, 9000);
  config.add_render_rule(
      RenderType::kEgoTrajectory, {},
      {"egotrajectory", "motionplan", "lateralplan", "longitudinalplan", "egoplanning", "adasresult"}, {}, 8990);
  config.add_render_rule(RenderType::kPrediction, {},
                         {"predictedobject", "predictedpath", "trajectory", "trajectories", "planningpath",
                          "navigationresult", "planningresult", "pathwithlaneid", "hparoutepoints", "factorarray",
                          "posearray", "velocityfactor", "steeringfactor", "safetyfactor", "navigationvisualization"},
                         {}, 8980);
  config.add_render_rule(RenderType::kLaneLine, {},
                         {"lanesegment",    "laneboundary",   "roadmarking",        "lanemarking",    "lanemark",
                          "laneline",       "extendedlane",   "perceptionlane",     "modellane",      "modelstatic",
                          "modeledge",      "visuallane",     "centerline",         "referenceline",  "guideline",
                          "linestring",     "roadinstance",   "roadstructure",      "roadcognition",  "roadedge",
                          "perceptionroad", "lanelet",        "polyline",           "debugline",      "llrrlane",
                          "lrlane",         "trianglelist",   "perceptionboundary", "perceptionline", "boundarydet",
                          "localroute",     "perceptiongate", "modelarea"},
                         {}, 8970);
  config.add_render_rule(RenderType::kTrafficLight, {}, {"trafficlight", "trafficsignal"}, {}, 8960);
  config.add_render_rule(
      RenderType::kFreespace, {},
      {"freespace", "drivablearea", "perceptioncell", "polygonstamped", "fspointsmessage", "perceptionarea"}, {}, 8950);
  config.add_render_rule(RenderType::kOccupancyGrid, {},
                         {"occupancygrid", "costmap", "voxelgrid", "gridmap", "occupiedgrid", "semanticimage",
                          "elevationmap", "localmap", "semanticmap", "hpalocalmap", "submap"},
                         {}, 8940);
  config.add_render_rule(RenderType::kTrafficSign, {}, {"trafficsign", "hdmapprimitive"}, {}, 8930);
  config.add_render_rule(RenderType::kStopLine, {},
                         {"stopline", "crosswalk", "stopreason", "areamarking", "arrowmarking", "modelarrow"}, {},
                         8920);
  config.add_render_rule(
      RenderType::kParkingSlot, {},
      {"parkingslot", "slotdetection", "parkingspace", "visualparkingslot", "modelslot", "slotpoint", "slotline"}, {},
      8910);
  config.add_render_rule(RenderType::kEgoTrajectory, {}, {"planningvisualization", "adasvisualization"}, {}, 8900);
  config.add_render_rule(RenderType::kHdMap, {}, {"hdmap", "laneletmap", "hdmapsegment", "laneletroute", "mapdata"}, {},
                         8890);
  config.add_render_rule(
      RenderType::kCameraFrustum, {},
      {"camerainfo", "cameracalib", "camerafrustum", "cameraintrinsic", "cameracalibration", "cameracalibparam"}, {},
      8880);
  config.add_render_rule(RenderType::kCovarianceEllipse, {},
                         {"covarianceellipse", "posecovariance", "posewithcovariance", "kinematicstate"}, {}, 8870);
  config.add_render_rule(RenderType::kObjectDetection, {},
                         {"detectedobject3d", "trackedobject3d", "objecttracking", "semanticobject", "instanceobject",
                          "boundingbox3d", "bbox3d", "orientedbox", "graspobject", "detectedperson", "humanobject"},
                         {}, 8860);
  config.add_render_rule(
      RenderType::kPrediction, {},
      {"navpath", "robotpath", "plannedpath", "referencepath", "goalpath", "motiontrajectory", "robottrajectory",
       "end_effectortrajectory", "endeffectortrajectory", "manipulatortrajectory", "armtrajectory"},
      {}, 8850);
  config.add_render_rule(RenderType::kEgoTrajectory, {},
                         {"odometry", "robotpose", "basepose", "basefootprint", "localization", "navstate",
                          "robotstate", "posewithtwist", "twistwithcovariance"},
                         {}, 8840);
  config.add_render_rule(RenderType::kLaneLine, {},
                         {"markerarray", "linestrip", "linelist", "skeleton", "skeleton3d", "keypoint", "keypoints",
                          "handkeypoint", "bodykeypoint"},
                         {}, 8830);
  config.add_render_rule(
      RenderType::kFreespace, {},
      {"workspace", "reachability", "footprint", "keepoutzone", "safetyzone", "costregion", "drivablepolygon"}, {},
      8820);
  config.add_render_rule(
      RenderType::kOccupancyGrid, {},
      {"octomap", "voxelmap", "distancefield", "esdf", "tsdf", "semanticgrid", "heightmap", "elevationgrid"}, {}, 8810);
  config.add_render_rule(
      RenderType::kHdMap, {},
      {"scenegraph", "topologicalmap", "navigationmap", "floorplan", "semanticmap3d", "semanticworld"}, {}, 8800);
  config.add_render_rule(RenderType::kCameraFrustum, {},
                         {"pinholecamera", "cameramodel", "depthcamera", "rgbdcamera", "sensorcalibration",
                          "extrinsiccalibration", "intrinsiccalibration"},
                         {}, 8790);
  config.add_render_rule(RenderType::kCovarianceEllipse, {},
                         {"statecovariance", "uncertaintyellipse", "uncertaintyellipsoid", "gaussianpose"}, {}, 8780);
  config.add_render_rule(
      RenderType::kObjectDetection, {},
      {"dynamicobject", "boundingboxarray", "cloudcluster", "pointobjectarray", "srrfullod", "ultrasonicinfo"}, {},
      8770);
  config.add_render_rule(RenderType::kPrediction, {},
                         {"decisionresult", "decisiontrajectory", "scenetrajectoryset", "planningfactor"}, {}, 8760);
  config.add_render_rule(RenderType::kEgoTrajectory, {},
                         {"ekfstate", "liteadasresult", "initialpose", "missiongoal", "locomotionstate",
                          "vehiclekinematics", "endeffectorstate"},
                         {}, 8750);
  config.add_render_rule(
      RenderType::kHdMap, {},
      {"hadmapbin", "hadmaproute", "routinginfo", "routinginfosd", "sdroute", "hparoutinginfo", "navigationsd"}, {},
      8740);
  config.add_render_rule(RenderType::kCameraFrustum, {},
                         {"sensorcalibs", "radarcalib", "radarcalibparam", "lidarcalib", "lidarcalibparam"}, {}, 8730);
  config.add_render_rule(RenderType::kPointCloud, {},
                         {"pointcluster", "pointclusters", "laserscan", "multiecholaserscan"}, {}, 8720);

  config.add_render_rule(RenderType::kObjectDetection, {}, {},
                         {"/od", "/detected_object", "/tracked_object", "/detection", "/obstacle", "/radar_object",
                          "/fusion_object", "/bev_object", "/object_list", "/perception_result", "/cone"},
                         8000);
  config.add_render_rule(RenderType::kPrediction, {}, {},
                         {"/predicted_object", "/prediction", "/trajectory", "/forecast", "/predicted_path",
                          "/planning_path", "/planned_trajectory"},
                         7990);
  config.add_render_rule(RenderType::kLaneLine, {}, {},
                         {"/lane", "/boundary", "/road_marking", "/centerline", "/lane_line", "/road_edge",
                          "/reference_line", "/road_structure"},
                         7980);
  config.add_render_rule(RenderType::kTrafficLight, {}, {}, {"/traffic_light", "/traffic_signal"}, 7970);
  config.add_render_rule(RenderType::kStopLine, {}, {}, {"/stop_line", "/crosswalk"}, 7960);
  config.add_render_rule(RenderType::kTrafficSign, {}, {},
                         {"/traffic_sign", "/sign", "/pole", "/signal_pole", "/lamp_post"}, 7950);
  config.add_render_rule(RenderType::kStopLine, {}, {},
                         {"/zebra", "/arrow", "/road_text", "/diamond", "/yield", "/speed_bump"}, 7940);
  config.add_render_rule(RenderType::kFreespace, {}, {}, {"/freespace", "/free_space", "/drivable_area"}, 7930);
  config.add_render_rule(RenderType::kOccupancyGrid, {}, {}, {"/occupancy", "/costmap", "/grid_map", "/voxel"}, 7920);
  config.add_render_rule(RenderType::kLaneLine, {}, {},
                         {"/virtual_wall", "/geofence", "/no_go_zone", "/restricted", "/gate", "/guardrail"}, 7910);
  config.add_render_rule(RenderType::kParkingSlot, {}, {}, {"/parking_slot", "/slot", "/parking_space"}, 7900);
  config.add_render_rule(RenderType::kEgoTrajectory, {}, {},
                         {"/ego_trajectory", "/motion_plan", "/ego_path", "/planned_path"}, 7890);
  config.add_render_rule(RenderType::kHdMap, {}, {}, {"/hd_map", "/lanelet_map", "/map_segment"}, 7880);
  config.add_render_rule(RenderType::kCameraFrustum, {}, {}, {"/camera_info", "/camera_calib"}, 7870);
  config.add_render_rule(RenderType::kObjectDetection, {}, {},
                         {"/bbox", "/bbox3d", "/bounding_box", "/semantic_object", "/tracked_person", "/grasp"}, 7860);
  config.add_render_rule(RenderType::kPrediction, {}, {},
                         {"/nav_path", "/robot_path", "/goal_path", "/reference_path", "/arm_trajectory",
                          "/manipulator_trajectory", "/end_effector_trajectory"},
                         7850);
  config.add_render_rule(RenderType::kEgoTrajectory, {}, {},
                         {"/odom", "/odometry", "/robot_pose", "/base_pose", "/localization", "/nav_state"}, 7840);
  config.add_render_rule(
      RenderType::kLaneLine, {}, {},
      {"/marker_array", "/line_strip", "/line_list", "/skeleton", "/keypoints", "/body_keypoints", "/hand_keypoints"},
      7830);
  config.add_render_rule(RenderType::kFreespace, {}, {},
                         {"/workspace", "/reachability", "/footprint", "/keepout", "/safety_zone"}, 7820);
  config.add_render_rule(RenderType::kOccupancyGrid, {}, {},
                         {"/octomap", "/voxel_map", "/distance_field", "/esdf", "/tsdf", "/height_map"}, 7810);
  config.add_render_rule(RenderType::kHdMap, {}, {},
                         {"/scene_graph", "/topological_map", "/navigation_map", "/floorplan"}, 7800);
  config.add_render_rule(RenderType::kPrediction, {}, {},
                         {"/decision_result", "/decision_trajectory", "/scene_trajectory"}, 7790);
  config.add_render_rule(RenderType::kHdMap, {}, {},
                         {"/routing_info", "/routing_result", "/sd_route", "/navigation_sd", "/had_map"}, 7780);
  config.add_render_rule(RenderType::kCameraFrustum, {}, {}, {"/sensor_calib", "/radar_calib", "/lidar_calib"}, 7770);
  config.add_render_rule(RenderType::kEgoTrajectory, {}, {},
                         {"/ekf_state", "/vehicle_pose", "/initial_pose", "/mission_goal"}, 7760);

  return config;
}

bool PerceptionConfig::load_from_file(const QString& path, QString* error) {
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
      *error = "Invalid perception JSON config";
    }
    return false;
  }

  PerceptionConfig next =
      pcd::json_bool_value(root, {"inherit_default", "extend_default"}, true) ? default_config() : PerceptionConfig();

  if (const auto* skip = pcd::json_find_any(root, {"skip", "ignore"}); skip && skip->is_object()) {
    if (pcd::json_bool_value(*skip, {"replace", "override"}, false)) {
      next.skip_url_contains_.clear();
      next.skip_serializer_contains_.clear();
      next.skip_url_equals_.clear();
      next.skip_serializer_equals_.clear();
    }

    pcd::append_unique(next.skip_url_contains_, pcd::json_string_list_any(*skip, {"url_contains", "urls"}));
    pcd::append_unique(
        next.skip_serializer_contains_,
        pcd::json_string_list_any(*skip, {"ser_contains", "serializer_contains", "type_contains", "serializers"}),
        true);
    pcd::append_unique(next.skip_url_equals_, pcd::json_string_list_any(*skip, {"url_equals", "topic_equals"}));
    pcd::append_unique(next.skip_serializer_equals_,
                       pcd::json_string_list_any(*skip, {"ser_equals", "serializer_equals", "type_equals_exact"}),
                       true);
  }

  if (const auto* rules = pcd::json_find_any(root, {"rules", "render_rules"}); rules && rules->is_array()) {
    int index = 0;

    for (const auto& object : *rules) {
      if (!object.is_object()) {
        ++index;
        continue;
      }

      perception::RenderType type;

      if (!render_type_from_string(pcd::json_string_value(object, {"type", "render_type"}), type)) {
        ++index;
        continue;
      }

      RenderRule rule;
      rule.name = pcd::json_string_value(object, {"name"});
      rule.domain = pcd::json_string_value(object, {"domain"});
      rule.type = type;
      rule.match = pcd::load_match_spec(object);
      rule.priority = pcd::json_int_value(object, {"priority"}, 20000 - index);
      next.render_rules_.emplace_back(std::move(rule));
      ++index;
    }
  }

  if (const auto* mappings = pcd::json_find_any(root, {"mappings", "parser_rules", "schema_rules", "decode_rules"});
      mappings && mappings->is_array()) {
    int index = 0;

    for (const auto& object : *mappings) {
      if (!object.is_object()) {
        ++index;
        continue;
      }

      perception::RenderType type;

      if (!render_type_from_string(pcd::json_string_value(object, {"type", "render_type"}), type)) {
        ++index;
        continue;
      }

      MappingRule rule;
      rule.name = pcd::json_string_value(object, {"name"});
      rule.ser = pcd::json_string_value(object, {"ser", "serializer", "source_type", "type_name"});
      rule.type = type;
      rule.encoding = encoding_from_string(pcd::json_string_value(object, {"encoding", "schema_encoding"}));
      rule.match = pcd::load_match_spec(object);
      rule.priority = pcd::json_int_value(object, {"priority"}, 20000 - index);
      rule.collection = pcd::json_path_string_value(
          object, {"collection", "collection_path", "repeated", "repeated_path", "objects", "outer"});
      rule.inner_collection =
          pcd::json_path_string_value(object, {"inner_collection", "inner_collection_path", "inner_repeated",
                                               "inner_repeated_path", "points", "point_collection"});
      pcd::load_field_mappings(object, rule.field_mappings);

      next.add_mapping_rule(std::move(rule));
      ++index;
    }
  }

  if (const auto* bindings = pcd::json_find_any(root, {"hud_bindings", "hud", "dashboard", "vehicle_bindings"});
      bindings && bindings->is_array()) {
    int index = 0;

    for (const auto& object : *bindings) {
      if (!object.is_object()) {
        ++index;
        continue;
      }

      MappingRule rule;
      rule.name = pcd::json_string_value(object, {"name"});
      rule.ser = pcd::json_string_value(object, {"ser", "serializer", "source_type", "type_name"});
      rule.encoding = encoding_from_string(pcd::json_string_value(object, {"encoding", "schema_encoding"}));
      rule.match = pcd::load_match_spec(object);
      rule.priority = pcd::json_int_value(object, {"priority"}, 20000 - index);
      pcd::load_field_mappings(object, rule.field_mappings);

      next.add_hud_binding(std::move(rule));
      ++index;
    }
  }

  next.source_path_ = path;
  *this = std::move(next);
  return true;
}

std::string PerceptionConfig::to_json_string() const {
  Json root = Json::object();
  root["inherit_default"] = false;

  Json skip = Json::object();
  if (!skip_url_contains_.empty()) {
    skip["url_contains"] = pcd::dump_string_list(skip_url_contains_);
  }
  if (!skip_serializer_contains_.empty()) {
    skip["ser_contains"] = pcd::dump_string_list(skip_serializer_contains_);
  }
  if (!skip_url_equals_.empty()) {
    skip["url_equals"] = pcd::dump_string_list(skip_url_equals_);
  }
  if (!skip_serializer_equals_.empty()) {
    skip["ser_equals"] = pcd::dump_string_list(skip_serializer_equals_);
  }
  if (!skip.empty()) {
    root["skip"] = std::move(skip);
  }

  Json rules = Json::array();
  for (const auto& rule : render_rules_) {
    Json object = Json::object();
    object["type"] = render_type_to_string(rule.type).toStdString();
    object["priority"] = rule.priority;

    if (!rule.name.isEmpty()) {
      object["name"] = rule.name.toStdString();
    }
    if (!rule.domain.isEmpty()) {
      object["domain"] = rule.domain.toStdString();
    }

    object["match"] = pcd::dump_match_spec(rule.match);
    rules.emplace_back(std::move(object));
  }
  root["rules"] = std::move(rules);

  Json mappings = Json::array();
  for (const auto& rule : mappings_) {
    Json object = Json::object();
    object["type"] = render_type_to_string(rule.type).toStdString();
    object["priority"] = rule.priority;

    if (!rule.name.isEmpty()) {
      object["name"] = rule.name.toStdString();
    }
    if (!rule.ser.isEmpty()) {
      object["ser"] = rule.ser.toStdString();
    }
    if (rule.encoding != perception::Encoding::kUnknown) {
      object["encoding"] = encoding_to_string(rule.encoding).toStdString();
    }
    if (!rule.collection.isEmpty()) {
      object["collection"] = rule.collection.toStdString();
    }
    if (!rule.inner_collection.isEmpty()) {
      object["inner_collection"] = rule.inner_collection.toStdString();
    }

    const Json match = pcd::dump_match_spec(rule.match);
    if (!match.empty()) {
      object["match"] = match;
    }

    object["field_mappings"] = pcd::dump_field_mappings(rule.field_mappings);
    mappings.emplace_back(std::move(object));
  }
  root["mappings"] = std::move(mappings);

  Json hud = Json::array();
  for (const auto& rule : hud_bindings_) {
    Json object = Json::object();
    object["priority"] = rule.priority;

    if (!rule.name.isEmpty()) {
      object["name"] = rule.name.toStdString();
    }
    if (!rule.ser.isEmpty()) {
      object["ser"] = rule.ser.toStdString();
    }
    if (rule.encoding != perception::Encoding::kUnknown) {
      object["encoding"] = encoding_to_string(rule.encoding).toStdString();
    }

    const Json match = pcd::dump_match_spec(rule.match);
    if (!match.empty()) {
      object["match"] = match;
    }

    object["field_mappings"] = pcd::dump_field_mappings(rule.field_mappings);
    hud.emplace_back(std::move(object));
  }
  root["hud_bindings"] = std::move(hud);

  return root.dump(2);
}

bool PerceptionConfig::save_to_file(const QString& path, QString* error) const {
  if (error) {
    error->clear();
  }

  std::ofstream file(path.toStdString(), std::ios::binary | std::ios::trunc);

  if (!file.is_open()) {
    if (error) {
      *error = "Can not open config file for writing";
    }
    return false;
  }

  const auto text = to_json_string();
  file.write(text.data(), static_cast<std::streamsize>(text.size()));

  if (!file.good()) {
    if (error) {
      *error = "Failed to write config file";
    }
    return false;
  }

  return true;
}

perception::RenderType PerceptionConfig::detect_render_type(const std::string& url, const std::string& ser,
                                                            vlink::SchemaType schema_type) const {
  const QString url_text = pcd::normalized_url(QString::fromStdString(url));
  const QString ser_text = pcd::normalized_serializer(QString::fromStdString(ser));
  const QString combined_text = ser_text + " " + url_text;
  const QString schema_text = schema_type_to_string(schema_type);

  const RenderRule* best_rule = nullptr;

  for (const auto& rule : render_rules_) {
    if (!pcd::match_spec_matches(rule.match, url_text, ser_text, combined_text, schema_text)) {
      continue;
    }

    if (!best_rule || rule.priority > best_rule->priority) {
      best_rule = &rule;
    }
  }

  const MappingRule* best_mapping = nullptr;

  for (const auto& rule : mappings_) {
    if (!rule.ser_normalized.isEmpty() && !ser_text.contains(rule.ser_normalized)) {
      continue;
    }

    if (!pcd::match_spec_matches(rule.match, url_text, ser_text, combined_text, schema_text)) {
      continue;
    }

    if (!best_mapping || rule.priority > best_mapping->priority) {
      best_mapping = &rule;
    }
  }

  if (best_mapping && (!best_rule || best_mapping->priority > best_rule->priority)) {
    return best_mapping->type;
  }

  return best_rule ? best_rule->type : perception::RenderType::kPointCloud;
}

const PerceptionConfig::MappingRule* PerceptionConfig::mapping_rule_for(const std::string& url, const std::string& ser,
                                                                        vlink::SchemaType schema_type,
                                                                        perception::RenderType type) const {
  const QString url_text = pcd::normalized_url(QString::fromStdString(url));
  const QString ser_text = pcd::normalized_serializer(QString::fromStdString(ser));
  const QString combined_text = ser_text + " " + url_text;
  const QString schema_text = schema_type_to_string(schema_type);

  const MappingRule* best_rule = nullptr;

  for (const auto& rule : mappings_) {
    if (rule.type != type) {
      continue;
    }

    if (!rule.ser_normalized.isEmpty() && !ser_text.contains(rule.ser_normalized)) {
      continue;
    }

    if (!pcd::match_spec_matches(rule.match, url_text, ser_text, combined_text, schema_text)) {
      continue;
    }

    if (!best_rule || rule.priority > best_rule->priority) {
      best_rule = &rule;
    }
  }

  return best_rule;
}

std::vector<const PerceptionConfig::MappingRule*> PerceptionConfig::mappings_for(const std::string& url,
                                                                                 const std::string& ser,
                                                                                 vlink::SchemaType schema_type) const {
  const QString url_text = pcd::normalized_url(QString::fromStdString(url));
  const QString ser_text = pcd::normalized_serializer(QString::fromStdString(ser));
  const QString combined_text = ser_text + " " + url_text;
  const QString schema_text = schema_type_to_string(schema_type);

  std::vector<const MappingRule*> out;

  for (const auto& rule : mappings_) {
    if (!rule.ser_normalized.isEmpty() && !ser_text.contains(rule.ser_normalized)) {
      continue;
    }

    if (!pcd::match_spec_matches(rule.match, url_text, ser_text, combined_text, schema_text)) {
      continue;
    }

    out.push_back(&rule);
  }

  std::sort(out.begin(), out.end(),
            [](const MappingRule* a, const MappingRule* b) { return a->priority > b->priority; });
  return out;
}

std::vector<const PerceptionConfig::MappingRule*> PerceptionConfig::hud_bindings_for(
    const std::string& url, const std::string& ser, vlink::SchemaType schema_type) const {
  const QString url_text = pcd::normalized_url(QString::fromStdString(url));
  const QString ser_text = pcd::normalized_serializer(QString::fromStdString(ser));
  const QString combined_text = ser_text + " " + url_text;
  const QString schema_text = schema_type_to_string(schema_type);

  std::vector<const MappingRule*> out;

  for (const auto& rule : hud_bindings_) {
    if (!rule.ser_normalized.isEmpty() && !ser_text.contains(rule.ser_normalized)) {
      continue;
    }

    if (!pcd::match_spec_matches(rule.match, url_text, ser_text, combined_text, schema_text)) {
      continue;
    }

    out.push_back(&rule);
  }

  std::sort(out.begin(), out.end(),
            [](const MappingRule* a, const MappingRule* b) { return a->priority > b->priority; });
  return out;
}

const QStringList& PerceptionConfig::hud_target_slots() {
  static const QStringList kSlots = {"speed",      "throttle",  "brake",     "steering_angle", "accel_lon",
                                     "accel_lat",  "yaw_rate",  "rpm",       "gear",           "turn_signal",
                                     "drive_mode", "turn_left", "turn_right"};
  return kSlots;
}

bool PerceptionConfig::should_skip(const QString& url, const QString& ser) const {
  return pcd::contains_any_token(pcd::normalized_url(url), skip_url_contains_) ||
         pcd::contains_any_token(pcd::normalized_serializer(ser), skip_serializer_contains_) ||
         pcd::equals_any_url(pcd::normalized_url(url), skip_url_equals_) ||
         skip_serializer_equals_.contains(pcd::normalized_serializer(ser));
}

QString PerceptionConfig::source_label() const {
  if (source_path_.isEmpty()) {
    return "Default";
  }

  return QFileInfo(source_path_).fileName();
}

void PerceptionConfig::add_render_rule(perception::RenderType type, QStringList schemas,
                                       QStringList serializer_contains, QStringList url_contains, int priority,
                                       QString name, QString domain, QStringList serializer_equals,
                                       QStringList url_equals, QStringList any_contains, QStringList serializer_regex,
                                       QStringList url_regex, QStringList any_regex) {
  RenderRule rule;
  rule.name = std::move(name);
  rule.domain = std::move(domain);
  rule.type = type;
  rule.match.schema_types = pcd::normalized_schema_list(schemas);
  rule.match.serializer_contains = pcd::normalized_serializer_list(serializer_contains);
  rule.match.serializer_equals = pcd::normalized_serializer_list(serializer_equals);
  rule.match.serializer_regex = pcd::compiled_regex_list(serializer_regex);
  rule.match.url_contains = pcd::normalized_url_list(url_contains);
  rule.match.url_equals = pcd::normalized_url_list(url_equals);
  rule.match.url_regex = pcd::compiled_regex_list(url_regex);
  rule.match.any_contains = pcd::normalized_any_match_list(any_contains);
  rule.match.any_regex = pcd::compiled_regex_list(any_regex);
  rule.priority = priority;
  render_rules_.emplace_back(std::move(rule));
}

void PerceptionConfig::add_mapping_rule(MappingRule rule) {
  if (rule.collection.isEmpty() && rule.inner_collection.isEmpty() && rule.field_mappings.empty()) {
    return;
  }

  finalize_mapping(rule);
  mappings_.emplace_back(std::move(rule));
}

void PerceptionConfig::add_hud_binding(MappingRule rule) {
  if (rule.field_mappings.empty()) {
    return;
  }

  finalize_mapping(rule);
  hud_bindings_.emplace_back(std::move(rule));
}

// NOLINTEND
