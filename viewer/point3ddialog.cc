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

#include "./point3ddialog.h"

#include <vlink/base/helpers.h>
#include <vlink/zerocopy/point_cloud.h>

#include <QDateTime>
#include <QFile>
#include <QGraphicsProxyWidget>
#include <QGraphicsTextItem>
#include <QHideEvent>
#include <QKeyEvent>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPaintEvent>
#include <QSettings>
#include <QShowEvent>
#include <QStack>
#include <QStandardPaths>
#include <QTimeZone>

#include "./cameradialog.h"
#include "./mainwindow.h"
#include "./osg/osgcommon.h"
#include "./osg/osgcoord.h"
#include "./osg/osggraphicsview.h"
#include "./osg/osglight.h"
#include "./osg/osgmanipulator.h"
#include "./osg/osgplatform.h"
#include "./osg/osgpointcloud.h"
#include "./osg/osgwidget.h"
#include "./ui_mainwindow.h"
#include "./ui_point3ddialog.h"

#define VLINK_VIEWER_PLATFORM_SIZE 500.0
#define VLINK_VIEWER_PLATFORM_GRID_COUNT 100

#ifdef VLINK_ENABLE_VIEWER_OSG

#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Woverloaded-virtual"
#pragma GCC diagnostic ignored "-Wdeprecated-copy"
#endif

#include <osg/BlendFunc>
#include <osg/Callback>
#include <osg/ComputeBoundsVisitor>
#include <osg/CullFace>
#include <osg/Depth>
#include <osg/Fog>
#include <osg/Material>
#include <osg/NodeVisitor>
#include <osg/Point>
#include <osg/PointSprite>
#include <osg/PolygonMode>
#include <osg/ShapeDrawable>
#include <osgDB/ReadFile>
#include <osgGA/StateSetManipulator>
#include <osgGA/TrackballManipulator>
#include <osgViewer/ViewerEventHandlers>
#include <sstream>

#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic pop
#endif

#ifdef _WIN32
#ifdef GetMessage
#undef GetMessage
#endif
#endif

#define USE_USER_CONDITION 1

#define USE_GRAPHICS_VIEW 1

#endif

class CustomCheckBox : public QCheckBox {
 public:
  explicit CustomCheckBox(QWidget* parent = nullptr) : QCheckBox(parent) {
    this->setStyleSheet("QCheckBox { background: transparent; color: white; font-size: 14px; font-weight: bold; }");
  }

 protected:
  void keyPressEvent(QKeyEvent* event) override { event->ignore(); }

  void keyReleaseEvent(QKeyEvent* event) override { event->ignore(); }
};

[[maybe_unused]] static uint32_t get_point3d_color(double value, double min_value, double max_value, bool inversion) {
  const double range = max_value - min_value;
  double normalized = 0.0;

  if (range > 0 && std::isfinite(range) && std::isfinite(value) && std::isfinite(min_value) &&
      std::isfinite(max_value)) {
    normalized = (value - min_value) / range;
  }

  normalized = std::clamp(normalized, 0.0, 1.0);

  uint8_t r = 0;
  uint8_t g = 0;
  uint8_t b = 0;

  if (inversion) {
    if (normalized < 0.25) {
      double t = normalized / 0.25;
      r = 255;
      g = static_cast<uint8_t>(t * 165);
      b = 0;
    } else if (normalized < 0.5) {
      double t = (normalized - 0.25) / 0.25;
      r = 255;
      g = 165 + static_cast<uint8_t>(t * (255 - 165));
      b = 0;
    } else if (normalized < 0.75) {
      double t = (normalized - 0.5) / 0.25;
      r = static_cast<uint8_t>((1.0 - t) * 255);
      g = 255;
      b = 0;
    } else {
      double t = (normalized - 0.75) / 0.25;
      r = 0;
      g = 255;
      b = static_cast<uint8_t>(t * 255);
    }
  } else {
    if (normalized < 0.25) {
      double t = normalized / 0.25;
      r = 0;
      g = static_cast<uint8_t>(t * 255);
      b = 255;
    } else if (normalized < 0.5) {
      double t = (normalized - 0.25) / 0.25;
      r = 0;
      g = 255;
      b = 255 - static_cast<uint8_t>(t * 255);
    } else if (normalized < 0.75) {
      double t = (normalized - 0.5) / 0.25;
      r = static_cast<uint8_t>(t * 255);
      g = 255;
      b = 0;
    } else {
      double t = (normalized - 0.75) / 0.25;
      r = 255;
      g = 255 - static_cast<uint8_t>(t * 255);
      b = 0;
    }
  }

  return (static_cast<uint32_t>(r) << 16) | (static_cast<uint32_t>(g) << 8) | static_cast<uint32_t>(b);
}

[[maybe_unused]] static double get_point3d_distance(double x, double y, double z) {
  if (std::isnan(x) || std::isnan(y) || std::isnan(z)) {
    return 0;
  }

  return std::sqrt(x * x + y * y + z * z);
}

[[maybe_unused]] static bool is_fbs_numeric_type(reflection::BaseType type) {
  switch (type) {
    case reflection::Bool:
    case reflection::Byte:
    case reflection::UByte:
    case reflection::Short:
    case reflection::UShort:
    case reflection::Int:
    case reflection::UInt:
    case reflection::Long:
    case reflection::ULong:
    case reflection::Float:
    case reflection::Double:
      return true;
    default:
      return false;
  }
}

[[maybe_unused]] static int get_point_value_type(reflection::BaseType type) {
  switch (type) {
    case reflection::Bool:
      return vlink::zerocopy::PointCloud::kBoolType;
    case reflection::Byte:
      return vlink::zerocopy::PointCloud::kInt8Type;
    case reflection::UByte:
      return vlink::zerocopy::PointCloud::kUint8Type;
    case reflection::Short:
      return vlink::zerocopy::PointCloud::kInt16Type;
    case reflection::UShort:
      return vlink::zerocopy::PointCloud::kUint16Type;
    case reflection::Int:
      return vlink::zerocopy::PointCloud::kInt32Type;
    case reflection::UInt:
      return vlink::zerocopy::PointCloud::kUint32Type;
    case reflection::Long:
      return vlink::zerocopy::PointCloud::kInt64Type;
    case reflection::ULong:
      return vlink::zerocopy::PointCloud::kUint64Type;
    case reflection::Float:
      return vlink::zerocopy::PointCloud::kFloatType;
    case reflection::Double:
      return vlink::zerocopy::PointCloud::kDoubleType;
    default:
      return vlink::zerocopy::PointCloud::kUnknownType;
  }
}

static bool is_proto_numeric_type(google::protobuf::FieldDescriptor::CppType type) {
  switch (type) {
    case google::protobuf::FieldDescriptor::CPPTYPE_INT32:
    case google::protobuf::FieldDescriptor::CPPTYPE_INT64:
    case google::protobuf::FieldDescriptor::CPPTYPE_UINT32:
    case google::protobuf::FieldDescriptor::CPPTYPE_UINT64:
    case google::protobuf::FieldDescriptor::CPPTYPE_DOUBLE:
    case google::protobuf::FieldDescriptor::CPPTYPE_FLOAT:
    case google::protobuf::FieldDescriptor::CPPTYPE_BOOL:
    case google::protobuf::FieldDescriptor::CPPTYPE_ENUM:
      return true;
    default:
      return false;
  }
}

static int get_point_value_type(google::protobuf::FieldDescriptor::CppType type) {
  switch (type) {
    case google::protobuf::FieldDescriptor::CPPTYPE_INT32:
      return vlink::zerocopy::PointCloud::kInt32Type;
    case google::protobuf::FieldDescriptor::CPPTYPE_INT64:
      return vlink::zerocopy::PointCloud::kInt64Type;
    case google::protobuf::FieldDescriptor::CPPTYPE_UINT32:
      return vlink::zerocopy::PointCloud::kUint32Type;
    case google::protobuf::FieldDescriptor::CPPTYPE_UINT64:
      return vlink::zerocopy::PointCloud::kUint64Type;
    case google::protobuf::FieldDescriptor::CPPTYPE_DOUBLE:
      return vlink::zerocopy::PointCloud::kDoubleType;
    case google::protobuf::FieldDescriptor::CPPTYPE_FLOAT:
      return vlink::zerocopy::PointCloud::kFloatType;
    case google::protobuf::FieldDescriptor::CPPTYPE_BOOL:
      return vlink::zerocopy::PointCloud::kBoolType;
    case google::protobuf::FieldDescriptor::CPPTYPE_ENUM:
      return vlink::zerocopy::PointCloud::kInt32Type;
    default:
      return vlink::zerocopy::PointCloud::kUnknownType;
  }
}

static bool read_proto_numeric(const google::protobuf::Message& message, const google::protobuf::FieldDescriptor& field,
                               double& value) {
  const auto* reflection = message.GetReflection();

  if (!reflection || field.is_repeated()) {
    return false;
  }

  switch (field.cpp_type()) {
    case google::protobuf::FieldDescriptor::CPPTYPE_INT32:
      value = reflection->GetInt32(message, &field);
      return true;
    case google::protobuf::FieldDescriptor::CPPTYPE_INT64:
      value = static_cast<double>(reflection->GetInt64(message, &field));
      return true;
    case google::protobuf::FieldDescriptor::CPPTYPE_UINT32:
      value = reflection->GetUInt32(message, &field);
      return true;
    case google::protobuf::FieldDescriptor::CPPTYPE_UINT64:
      value = static_cast<double>(reflection->GetUInt64(message, &field));
      return true;
    case google::protobuf::FieldDescriptor::CPPTYPE_DOUBLE:
      value = reflection->GetDouble(message, &field);
      return true;
    case google::protobuf::FieldDescriptor::CPPTYPE_FLOAT:
      value = reflection->GetFloat(message, &field);
      return true;
    case google::protobuf::FieldDescriptor::CPPTYPE_BOOL:
      value = reflection->GetBool(message, &field);
      return true;
    case google::protobuf::FieldDescriptor::CPPTYPE_ENUM:
      value = reflection->GetEnumValue(message, &field);
      return true;
    default:
      return false;
  }
}

static std::string join_proto_path(const std::vector<const google::protobuf::FieldDescriptor*>& path) {
  std::string result;

  for (size_t i = 0; i < path.size(); ++i) {
    if (i > 0) {
      result += ".";
    }

    result += path.at(i)->name();
  }

  return result;
}

static bool is_same_proto_path(const std::vector<const google::protobuf::FieldDescriptor*>& lhs,
                               const std::vector<const google::protobuf::FieldDescriptor*>& rhs) {
  if (lhs.size() != rhs.size()) {
    return false;
  }

  for (size_t i = 0; i < lhs.size(); ++i) {
    if (lhs.at(i) != rhs.at(i)) {
      return false;
    }
  }

  return true;
}

[[maybe_unused]] static bool get_proto_numeric_by_path(
    const google::protobuf::Message& message, const std::vector<const google::protobuf::FieldDescriptor*>& path,
    double& value) {
  if (path.empty()) {
    return false;
  }

  const google::protobuf::Message* current = &message;

  for (size_t i = 0; i + 1 < path.size(); ++i) {
    const auto* field = path.at(i);

    if (!field || field->is_repeated() || field->cpp_type() != google::protobuf::FieldDescriptor::CPPTYPE_MESSAGE) {
      return false;
    }

    const auto* reflection = current->GetReflection();

    if (!reflection) {
      return false;
    }

    if (field->has_presence() && !reflection->HasField(*current, field)) {
      return false;
    }

    current = &reflection->GetMessage(*current, field);
  }

  const auto* leaf = path.back();

  if (!leaf) {
    return false;
  }

  return read_proto_numeric(*current, *leaf, value);
}

static bool contains_any_keyword(const std::string& name, std::initializer_list<const char*> keywords) {
  for (const auto* keyword : keywords) {
    if (keyword && name.find(keyword) != std::string::npos) {
      return true;
    }
  }

  return false;
}

#if GOOGLE_PROTOBUF_VERSION >= 6030000
static std::string proto_field_name_string(const google::protobuf::FieldDescriptor* field) {
  if (!field) {
    return {};
  }

  return std::string(field->name().data(), field->name().size());
}

static bool proto_field_name_equal(const google::protobuf::FieldDescriptor* field, const char* expected) {
  if (!field || !expected) {
    return false;
  }

  return field->name() == expected;
}
#else
static std::string proto_field_name_string(const google::protobuf::FieldDescriptor* field) {
  if (!field) {
    return {};
  }

  return field->name();
}

static bool proto_field_name_equal(const google::protobuf::FieldDescriptor* field, const char* expected) {
  if (!field || !expected) {
    return false;
  }

  return field->name() == expected;
}
#endif

static std::string normalize_field_name(std::string name) {
  std::transform(name.begin(), name.end(), name.begin(),
                 [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
  return name;
}

static int score_proto_xyz_path(const std::vector<const google::protobuf::FieldDescriptor*>& prefix) {
  int score = static_cast<int>(prefix.size()) * 50;

  for (const auto* field : prefix) {
    if (!field) {
      continue;
    }

    const auto name = normalize_field_name(proto_field_name_string(field));

    if (name == "point" || name == "points" || name == "pt" || name == "position" || name == "pos" || name == "coord" ||
        name == "coords" || name == "coordinate" || name == "coordinates") {
      score -= 120;
    } else if (contains_any_keyword(name, {"point", "position", "coord", "loc", "xyz"})) {
      score -= 80;
    } else if (name == "center" || name == "centroid" || name == "translation" || name == "offset") {
      score -= 50;
    } else if (contains_any_keyword(name, {"center", "centroid", "trans", "offset"})) {
      score -= 30;
    }

    if (contains_any_keyword(name, {"orient", "rot", "quat", "euler", "angle", "scale", "normal", "direction"})) {
      score += 120;
    }
  }

  return score;
}

static void find_proto_xyz_candidate_recursive(const google::protobuf::Descriptor* descriptor,
                                               std::vector<const google::protobuf::FieldDescriptor*>& prefix,
                                               std::unordered_set<const google::protobuf::Descriptor*>& stack,
                                               ProtoPointCandidate& candidate, bool& found, int& best_score) {
  if (!descriptor) {
    return;
  }

  const google::protobuf::FieldDescriptor* x_field = nullptr;
  const google::protobuf::FieldDescriptor* y_field = nullptr;
  const google::protobuf::FieldDescriptor* z_field = nullptr;

  for (int i = 0; i < descriptor->field_count(); ++i) {
    const auto* field = descriptor->field(i);

    if (!field || field->is_repeated() || !is_proto_numeric_type(field->cpp_type())) {
      continue;
    }

    if (proto_field_name_equal(field, "x")) {
      x_field = field;
    } else if (proto_field_name_equal(field, "y")) {
      y_field = field;
    } else if (proto_field_name_equal(field, "z")) {
      z_field = field;
    }
  }

  if (x_field && y_field && z_field) {
    const int score = score_proto_xyz_path(prefix);
    if (!found || score < best_score) {
      candidate.x_path = prefix;
      candidate.y_path = prefix;
      candidate.z_path = prefix;
      candidate.x_path.emplace_back(x_field);
      candidate.y_path.emplace_back(y_field);
      candidate.z_path.emplace_back(z_field);
      best_score = score;
      found = true;
    }
  }

  if (!stack.insert(descriptor).second) {
    return;
  }

  for (int i = 0; i < descriptor->field_count(); ++i) {
    const auto* field = descriptor->field(i);

    if (!field || field->is_repeated() || field->cpp_type() != google::protobuf::FieldDescriptor::CPPTYPE_MESSAGE ||
        !field->message_type()) {
      continue;
    }

    prefix.emplace_back(field);
    find_proto_xyz_candidate_recursive(field->message_type(), prefix, stack, candidate, found, best_score);
    prefix.pop_back();
  }

  stack.erase(descriptor);
}

static void collect_proto_value_fields_recursive(const google::protobuf::Descriptor* descriptor,
                                                 std::vector<const google::protobuf::FieldDescriptor*>& prefix,
                                                 std::unordered_set<const google::protobuf::Descriptor*>& stack,
                                                 const ProtoPointCandidate& candidate,
                                                 std::unordered_set<std::string>& seen_names,
                                                 std::vector<ProtoPointValueField>& out) {
  if (!descriptor) {
    return;
  }

  if (!stack.insert(descriptor).second) {
    return;
  }

  for (int i = 0; i < descriptor->field_count(); ++i) {
    const auto* field = descriptor->field(i);

    if (!field || field->is_repeated()) {
      continue;
    }

    prefix.emplace_back(field);

    if (field->cpp_type() == google::protobuf::FieldDescriptor::CPPTYPE_MESSAGE && field->message_type()) {
      collect_proto_value_fields_recursive(field->message_type(), prefix, stack, candidate, seen_names, out);
    } else if (is_proto_numeric_type(field->cpp_type()) && !is_same_proto_path(prefix, candidate.x_path) &&
               !is_same_proto_path(prefix, candidate.y_path) && !is_same_proto_path(prefix, candidate.z_path)) {
      const auto display_name = join_proto_path(prefix);
      if (seen_names.emplace(display_name).second) {
        ProtoPointValueField item;
        item.display_name = display_name;
        item.leaf_name = proto_field_name_string(field);
        item.value_type = get_point_value_type(field->cpp_type());
        item.path = prefix;
        out.emplace_back(std::move(item));
      }
    }

    prefix.pop_back();
  }

  stack.erase(descriptor);
}

static std::string join_flatbuffers_path(const std::vector<const reflection::Field*>& path) {
  std::string result;

  for (size_t i = 0; i < path.size(); ++i) {
    if (i > 0) {
      result += ".";
    }

    result += path.at(i)->name()->str();
  }

  return result;
}

static bool is_same_flatbuffers_path(const std::vector<const reflection::Field*>& lhs,
                                     const std::vector<const reflection::Field*>& rhs) {
  if (lhs.size() != rhs.size()) {
    return false;
  }

  for (size_t i = 0; i < lhs.size(); ++i) {
    if (lhs.at(i) != rhs.at(i)) {
      return false;
    }
  }

  return true;
}

[[maybe_unused]] static bool get_flatbuffers_numeric_by_path(const FlatbuffersObjectView& view,
                                                             const std::vector<const reflection::Field*>& path,
                                                             const reflection::Schema& schema, double& value) {
  if (!view.valid() || path.empty()) {
    return false;
  }

  FlatbuffersObjectView current_view = view;

  for (size_t i = 0; i + 1 < path.size(); ++i) {
    const auto* field = path.at(i);

    if (!field || field->type()->base_type() != reflection::Obj) {
      return false;
    }

    FlatbuffersObjectView child_view;

    if (!get_child_view(current_view, *field, schema, child_view)) {
      return false;
    }

    current_view = child_view;
  }

  const auto* leaf = path.back();

  if (!leaf) {
    return false;
  }

  const auto numeric = get_numeric(current_view, *leaf);

  if (!numeric.has_value()) {
    return false;
  }

  value = numeric.value();
  return true;
}

static int score_flatbuffers_xyz_path(const std::vector<const reflection::Field*>& prefix) {
  int score = static_cast<int>(prefix.size()) * 50;

  for (const auto* field : prefix) {
    if (!field || !field->name()) {
      continue;
    }

    const auto name = normalize_field_name(field->name()->str());

    if (name == "point" || name == "points" || name == "pt" || name == "position" || name == "pos" || name == "coord" ||
        name == "coords" || name == "coordinate" || name == "coordinates") {
      score -= 120;
    } else if (contains_any_keyword(name, {"point", "position", "coord", "loc", "xyz"})) {
      score -= 80;
    } else if (name == "center" || name == "centroid" || name == "translation" || name == "offset") {
      score -= 50;
    } else if (contains_any_keyword(name, {"center", "centroid", "trans", "offset"})) {
      score -= 30;
    }

    if (contains_any_keyword(name, {"orient", "rot", "quat", "euler", "angle", "scale", "normal", "direction"})) {
      score += 120;
    }
  }

  return score;
}

static void find_flatbuffers_xyz_candidate_recursive(const reflection::Object* object, const reflection::Schema& schema,
                                                     std::vector<const reflection::Field*>& prefix,
                                                     std::unordered_set<const reflection::Object*>& stack,
                                                     FlatbuffersPointCandidate& candidate, bool& found,
                                                     int& best_score) {
  if (!object || !object->fields()) {
    return;
  }

  const reflection::Field* x_field = nullptr;
  const reflection::Field* y_field = nullptr;
  const reflection::Field* z_field = nullptr;

  for (uint32_t i = 0; i < object->fields()->size(); ++i) {
    const auto* field = object->fields()->Get(i);

    if (!field || !is_fbs_numeric_type(field->type()->base_type())) {
      continue;
    }

    const auto name = field->name()->str();

    if (name == "x") {
      x_field = field;
    } else if (name == "y") {
      y_field = field;
    } else if (name == "z") {
      z_field = field;
    }
  }

  if (x_field && y_field && z_field) {
    const int score = score_flatbuffers_xyz_path(prefix);
    if (!found || score < best_score) {
      candidate.x_path = prefix;
      candidate.y_path = prefix;
      candidate.z_path = prefix;
      candidate.x_path.emplace_back(x_field);
      candidate.y_path.emplace_back(y_field);
      candidate.z_path.emplace_back(z_field);
      best_score = score;
      found = true;
    }
  }

  if (!stack.insert(object).second) {
    return;
  }

  for (uint32_t i = 0; i < object->fields()->size(); ++i) {
    const auto* field = object->fields()->Get(i);

    if (!field || field->type()->base_type() != reflection::Obj || !schema.objects()) {
      continue;
    }

    const auto* child_object = schema.objects()->Get(static_cast<uint32_t>(field->type()->index()));

    if (!child_object) {
      continue;
    }

    prefix.emplace_back(field);
    find_flatbuffers_xyz_candidate_recursive(child_object, schema, prefix, stack, candidate, found, best_score);
    prefix.pop_back();
  }

  stack.erase(object);
}

static void collect_flatbuffers_value_fields_recursive(
    const reflection::Object* object, const reflection::Schema& schema, std::vector<const reflection::Field*>& prefix,
    std::unordered_set<const reflection::Object*>& stack, const FlatbuffersPointCandidate& candidate,
    std::unordered_set<std::string>& seen_names, std::vector<FlatbuffersPointValueField>& out) {
  if (!object || !object->fields()) {
    return;
  }

  if (!stack.insert(object).second) {
    return;
  }

  for (uint32_t i = 0; i < object->fields()->size(); ++i) {
    const auto* field = object->fields()->Get(i);

    if (!field) {
      continue;
    }

    prefix.emplace_back(field);

    if (field->type()->base_type() == reflection::Obj && schema.objects()) {
      const auto* child_object = schema.objects()->Get(static_cast<uint32_t>(field->type()->index()));
      if (child_object) {
        collect_flatbuffers_value_fields_recursive(child_object, schema, prefix, stack, candidate, seen_names, out);
      }
    } else if (is_fbs_numeric_type(field->type()->base_type()) && !is_same_flatbuffers_path(prefix, candidate.x_path) &&
               !is_same_flatbuffers_path(prefix, candidate.y_path) &&
               !is_same_flatbuffers_path(prefix, candidate.z_path)) {
      const auto display_name = join_flatbuffers_path(prefix);
      if (seen_names.emplace(display_name).second) {
        FlatbuffersPointValueField item;
        item.display_name = display_name;
        item.leaf_name = field->name()->str();
        item.value_type = get_point_value_type(field->type()->base_type());
        item.path = prefix;
        out.emplace_back(std::move(item));
      }
    }

    prefix.pop_back();
  }

  stack.erase(object);
}

Point3DDialog::Point3DDialog(QWidget* parent, bool disable_osg) : QDialog(parent), ui(new Ui::Point3DDialog) {
  window_ = MainWindow::get_instance();

  if (parent) {
    setWindowFlags(Qt::Window | Qt::WindowMaximizeButtonHint | Qt::WindowCloseButtonHint | Qt::WindowStaysOnTopHint);
  } else {
    setWindowFlags(Qt::Window | Qt::WindowMaximizeButtonHint | Qt::WindowCloseButtonHint);
  }

  ui->setupUi(this);

  ui->stackedWidget->setCurrentIndex(0);

  {
    QFont font = ui->textEdit->font();
    font.setFamily("Noto Mono");
    ui->textEdit->setFont(font);
  }

  ui->checkBox_car->setEnabled(false);
  ui->checkBox_car->setChecked(false);

  const auto& selected_items = window_->ui->treeWidget_url->selectedItems();

  multi_mode_ = selected_items.count() > 1;

  msg_list_.emplace_back();
  fbs_msg_list_.emplace_back();

  google::protobuf::Descriptor* target_desc = nullptr;

  {
    std::lock_guard lock(window_->data_mutex_);

    select_urls_.clear();

    if (selected_items.count() == 1) {
      QString url = selected_items.at(0)->text(1);
      QString ser = selected_items.at(0)->data(1, Qt::UserRole).toString();

      select_urls_.emplace(url.toStdString());

      const auto url_str = url.toStdString();
      const auto ser_str = ser.toStdString();
      const auto schema_iter = window_->schema_type_map_.find(url_str);
      const auto schema_type =
          schema_iter != window_->schema_type_map_.end() ? schema_iter->second : vlink::SchemaType::kUnknown;

      if (schema_type == vlink::SchemaType::kProtobuf && !target_desc && window_->des_pool_) {
        target_desc = const_cast<google::protobuf::Descriptor*>(window_->des_pool_->FindMessageTypeByName(ser_str));
      } else if (schema_type == vlink::SchemaType::kFlatbuffers && !target_fbs_context_) {
        target_fbs_context_ = window_->flatbuffers_runtime_.find_context(ser_str);
      }

      ui->pushButton_camera->setEnabled(false);
    } else {
      for (const auto& item : selected_items) {
        QString url = item->text(1);
        QString ser = item->data(1, Qt::UserRole).toString();

#if USE_USER_CONDITION
        QString url_ser = url.toLower();
        QString lower_ser = ser.toLower();

        if (url_ser.contains("/od") || url_ser.contains("info/")) {
          continue;
        }

        if (!lower_ser.contains("point") && !lower_ser.contains("radar") && !lower_ser.contains("lidar")) {
          continue;
        }
#endif

        select_urls_.emplace(url.toStdString());

        const auto url_str = url.toStdString();
        const auto ser_str = ser.toStdString();
        const auto schema_iter = window_->schema_type_map_.find(url_str);
        const auto schema_type =
            schema_iter != window_->schema_type_map_.end() ? schema_iter->second : vlink::SchemaType::kUnknown;

        if (schema_type == vlink::SchemaType::kProtobuf && !target_desc && window_->des_pool_) {
          target_desc = const_cast<google::protobuf::Descriptor*>(window_->des_pool_->FindMessageTypeByName(ser_str));
        } else if (schema_type == vlink::SchemaType::kFlatbuffers && !target_fbs_context_) {
          target_fbs_context_ = window_->flatbuffers_runtime_.find_context(ser_str);
        }
      }

      ui->pushButton_camera->setEnabled(true);
    }

    auto data_callback = [this](const vlink::ProxyAPI::Data& proxy_data) {
      if (select_urls_.count(proxy_data.url) == 0) {
        std::lock_guard lock(data_mutex_);
        if (data_callback_) {
          data_callback_(proxy_data);
        }

        return;
      }

#ifdef VLINK_ENABLE_VIEWER_OSG
      {
        std::lock_guard lock(cache_mtx_);
        proxy_data_cache_[proxy_data.url] = proxy_data;
      }

      if (select_handler_ && select_handler_->isSelecting()) {
        return;
      }

      QElapsedTimer timer;
      timer.start();

      const auto schema_type = proxy_data.schema;

      if (schema_type == vlink::SchemaType::kZeroCopy &&
          proxy_data.ser == vlink::Serializer::get_serialized_type<vlink::zerocopy::PointCloud>()) {
        QMetaObject::invokeMethod(this, "update_ui_for_zero_copy_types", Qt::QueuedConnection,
                                  Q_ARG(QVariant, QVariant::fromValue<vlink::ProxyAPI::Data>(proxy_data)),
                                  Q_ARG(bool, false), Q_ARG(QElapsedTimer, timer));
      } else if (schema_type == vlink::SchemaType::kProtobuf && target_msg_) {
        QMetaObject::invokeMethod(this, "update_ui_for_proto", Qt::QueuedConnection,
                                  Q_ARG(QVariant, QVariant::fromValue<vlink::ProxyAPI::Data>(proxy_data)),
                                  Q_ARG(bool, false), Q_ARG(QElapsedTimer, timer));
      } else if (schema_type == vlink::SchemaType::kFlatbuffers && target_fbs_context_) {
        QMetaObject::invokeMethod(this, "update_ui_for_flatbuffers", Qt::QueuedConnection,
                                  Q_ARG(QVariant, QVariant::fromValue<vlink::ProxyAPI::Data>(proxy_data)),
                                  Q_ARG(bool, false), Q_ARG(QElapsedTimer, timer));
      }

#endif
    };

    if (parent) {
      auto parent_dialog = qobject_cast<CameraDialog*>(parent);
      if (parent_dialog) {
        std::lock_guard plock(parent_dialog->data_mutex_);
        parent_dialog->data_callback_ = std::move(data_callback);
      }
    } else {
      window_->data_callback_ = std::move(data_callback);
    }
  }

  auto process_func = [this](const google::protobuf::FieldDescriptor* field) {
    if (!field || !field->is_repeated() || field->cpp_type() != google::protobuf::FieldDescriptor::CPPTYPE_MESSAGE ||
        !field->message_type()) {
      return;
    }

    ProtoPointCandidate candidate;
    candidate.repeated_field = field;

    std::vector<const google::protobuf::FieldDescriptor*> prefix;
    std::unordered_set<const google::protobuf::Descriptor*> stack;
    bool found = false;
    int best_score = std::numeric_limits<int>::max();
    find_proto_xyz_candidate_recursive(field->message_type(), prefix, stack, candidate, found, best_score);

    if (!found) {
      return;
    }

    std::unordered_set<std::string> seen_names;
    collect_proto_value_fields_recursive(field->message_type(), prefix, stack, candidate, seen_names,
                                         candidate.value_fields);

    msg_list_.emplace_back(candidate);

#if GOOGLE_PROTOBUF_VERSION >= 6030000
    ui->comboBox_proto->addItem(field->name().data());
#else
    ui->comboBox_proto->addItem(field->name().c_str());
#endif

    for (const auto& value_field : candidate.value_fields) {
      const auto item_name = QString::fromStdString(value_field.display_name);

      if (ui->comboBox_value->findText(item_name) < 0) {
        ui->comboBox_value->addItem(item_name);
      }
    }
  };

  auto process_fbs_func = [this](const reflection::Field* field) {
    if (!target_fbs_context_ || !target_fbs_context_->valid() || !field ||
        (field->type()->base_type() != reflection::Vector && field->type()->base_type() != reflection::Vector64) ||
        field->type()->element() != reflection::Obj || !target_fbs_context_->schema->objects()) {
      return;
    }

    const auto* element_object =
        target_fbs_context_->schema->objects()->Get(static_cast<uint32_t>(field->type()->index()));

    if (!element_object || !element_object->fields()) {
      return;
    }

    FlatbuffersPointCandidate candidate;
    candidate.repeated_field = field;

    std::vector<const reflection::Field*> prefix;
    std::unordered_set<const reflection::Object*> stack;
    bool found = false;
    int best_score = std::numeric_limits<int>::max();
    find_flatbuffers_xyz_candidate_recursive(element_object, *target_fbs_context_->schema, prefix, stack, candidate,
                                             found, best_score);

    if (!found) {
      return;
    }

    std::unordered_set<std::string> seen_names;
    collect_flatbuffers_value_fields_recursive(element_object, *target_fbs_context_->schema, prefix, stack, candidate,
                                               seen_names, candidate.value_fields);

    fbs_msg_list_.emplace_back(candidate);
    ui->comboBox_proto->addItem(QString::fromStdString(field->name()->str()));

    for (const auto& value_field : candidate.value_fields) {
      const auto item_name = QString::fromStdString(value_field.display_name);

      if (ui->comboBox_value->findText(item_name) < 0) {
        ui->comboBox_value->addItem(item_name);
      }
    }
  };

  if (target_desc) {
    target_msg_ = window_->factory_->GetPrototype(target_desc)->New();

    if (target_msg_) {
      for (int i = 0; i < target_msg_->GetDescriptor()->field_count(); ++i) {
        const auto* field = target_msg_->GetDescriptor()->field(i);

        if (field->cpp_type() != google::protobuf::FieldDescriptor::CPPTYPE_MESSAGE) {
          continue;
        }

        if (field->is_repeated()) {
          process_func(field);
        }
      }
    }

    ui->label_proto->setEnabled(true);
    ui->comboBox_proto->setEnabled(true);
  } else if (target_fbs_context_ && target_fbs_context_->valid()) {
    const auto* root_object = target_fbs_context_->root_object;

    if (root_object && root_object->fields()) {
      for (uint32_t i = 0; i < root_object->fields()->size(); ++i) {
        process_fbs_func(root_object->fields()->Get(i));
      }
    }

    ui->label_proto->setEnabled(true);
    ui->comboBox_proto->setEnabled(true);
  } else {
    ui->label_proto->setEnabled(false);
    ui->comboBox_proto->setEnabled(false);
  }

  if (ui->comboBox_proto->count() > 1) {
    ui->comboBox_proto->setCurrentIndex(1);
  }

  if (ui->comboBox_value->count() > 1) {
    ui->comboBox_value->setCurrentIndex(1);
  }

  timer_ = new QTimer(this);
  timer_->setInterval(1000);

  connect(timer_, &QTimer::timeout, this, [this]() {
    if (frame_count_ != 0) {
      float real_frame_count =
          (last_frame_count_ * 1 + frame_count_ * 2) / 3.0f / window_->ui->treeWidget_url->selectedItems().count();
      last_frame_count_ = frame_count_;

      ui->label_frame2->setText(QString::number(real_frame_count, 'f', 2) + " Hz");
      ui->label_count2->setText(QString::number(total_point_count_));

      if (point_min_ == std::numeric_limits<double>::max()) {
        ui->label_min2->setText("---");
        if (!ui->groupBox_range->isChecked()) {
          ui->doubleSpinBox_min->blockSignals(true);
          ui->doubleSpinBox_min->setValue(0);
          ui->doubleSpinBox_min->blockSignals(false);
        }
      } else {
        ui->label_min2->setText(QString::number(point_min_, 'f', 2));
        if (!ui->groupBox_range->isChecked()) {
          ui->doubleSpinBox_min->blockSignals(true);
          ui->doubleSpinBox_min->setValue(point_min_);
          ui->doubleSpinBox_min->blockSignals(false);
        }
      }

      if (point_max_ == std::numeric_limits<double>::max()) {
        ui->label_max2->setText("---");
        if (!ui->groupBox_range->isChecked()) {
          ui->doubleSpinBox_max->blockSignals(true);
          ui->doubleSpinBox_max->setValue(0);
          ui->doubleSpinBox_max->blockSignals(false);
        }
      } else {
        ui->label_max2->setText(QString::number(point_max_, 'f', 2));
        if (!ui->groupBox_range->isChecked()) {
          ui->doubleSpinBox_max->blockSignals(true);
          ui->doubleSpinBox_max->setValue(point_max_);
          ui->doubleSpinBox_max->blockSignals(false);
        }
      }

      if (average_value_ == std::numeric_limits<double>::max()) {
        ui->label_average2->setText("---");
      } else {
        ui->label_average2->setText(QString::number(average_value_, 'f', 2));
      }

      ui->groupBox_point->setEnabled(true);

    } else {
      ui->label_count2->setText("---");
      ui->label_frame2->setText("---");
      ui->label_min2->setText("---");
      ui->label_max2->setText("---");
      ui->label_average2->setText("---");
      ui->groupBox_point->setEnabled(false);
    }

    frame_count_ = 0;
    total_point_count_ = 0;
  });

  connect(ui->comboBox_proto, static_cast<void (QComboBox::*)(int)>(&QComboBox::currentIndexChanged), this,
          [](int index) {
            if (index == 0) {
              return;
            } else {
            }
          });

  connect(ui->comboBox_value, static_cast<void (QComboBox::*)(int)>(&QComboBox::currentIndexChanged), this,
          [this](int index) {
            (void)index;
            point_min_ = std::numeric_limits<double>::max();
            point_max_ = std::numeric_limits<double>::max();

            refresh_sence();
          });

  if (disable_osg) {
    return;
  }

#ifdef VLINK_ENABLE_VIEWER_OSG
  ui->label_osg->setStyleSheet("");

  ui->label_osg->setText("");

  osg_layout_ = new QVBoxLayout(ui->label_osg);
  osg_layout_->setContentsMargins(0, 0, 0, 0);
  osg_layout_->setSpacing(0);
  ui->label_osg->setLayout(osg_layout_);

#if USE_GRAPHICS_VIEW
  osg_view_ = new OsgGraphicsView(ui->label_osg);
  osg_layout_->addWidget(osg_view_);

  auto* logo_item = new QGraphicsPixmapItem;
  auto* fps_item = new QGraphicsTextItem;
  auto* selection_item = new QGraphicsTextItem;
  auto* home_item = new QGraphicsProxyWidget;
  auto* left_item = new QGraphicsProxyWidget;
  auto* right_item = new QGraphicsProxyWidget;
  auto* front_item = new QGraphicsProxyWidget;
  auto* behind_item = new QGraphicsProxyWidget;

  {
    logo_item->setCacheMode(QGraphicsTextItem::ItemCoordinateCache);
    logo_item->setOpacity(0.2);
    logo_item->setPixmap(QPixmap(":/resource/vlink.svg"));
    osg_view_->scene()->addItem(logo_item);
  }

  {
    fps_item->setCacheMode(QGraphicsTextItem::ItemCoordinateCache);
    fps_item->setPlainText("FPS: ---");
    fps_item->setDefaultTextColor(qRgb(50, 255, 50));
    fps_item->setOpacity(0.8);
    QFont font = fps_item->font();
    font.setPixelSize(15);
    fps_item->setFont(font);
    osg_view_->scene()->addItem(fps_item);
  }

  {
    selection_item->setCacheMode(QGraphicsTextItem::ItemCoordinateCache);
    selection_item->setPlainText("Selection Mode");
    selection_item->setDefaultTextColor(qRgb(240, 120, 40));
    selection_item->setOpacity(0.8);
    QFont font = selection_item->font();
    font.setPixelSize(30);
    font.setBold(true);
    selection_item->setFont(font);
    osg_view_->scene()->addItem(selection_item);
    selection_item->setVisible(false);
  }

  // home
  {
    auto* button = new QToolButton();
    button->setFocusPolicy(Qt::NoFocus);
    button->setAutoRaise(true);
    button->setIconSize(QSize(24, 24));
    button->setIcon(QIcon(":/resource/home.png"));
    button->setToolTip(tr("Home"));
    connect(button, &QToolButton::clicked, this, [this](bool) {
      osg::Vec3d home_eye;
      osg::Vec3d home_center;
      osg::Vec3d home_up;

      manipulator_->getHomePosition(home_eye, home_center, home_up);

      manipulator_->moveToPoint(home_eye, home_center, home_up);
    });

    home_item->setCacheMode(QGraphicsTextItem::ItemCoordinateCache);
    home_item->setWidget(button);
    home_item->setOpacity(0.8);
    home_item->setFocusPolicy(Qt::NoFocus);

    osg_view_->scene()->addItem(home_item);
  }

  // left
  {
    auto* button = new QToolButton();
    button->setFocusPolicy(Qt::NoFocus);
    button->setAutoRaise(true);
    button->setIconSize(QSize(24, 24));
    button->setIcon(QIcon(":/resource/left.png"));
    button->setToolTip(tr("Left"));
    connect(button, &QToolButton::clicked, this, [this](bool) {
      osg::Vec3d eye = std::get<0>(move_point_map_[0]);
      osg::Vec3d center = std::get<1>(move_point_map_[0]);
      osg::Vec3d up = std::get<2>(move_point_map_[0]);

      manipulator_->moveToPoint(eye, center, up);
    });

    left_item->setCacheMode(QGraphicsTextItem::ItemCoordinateCache);
    left_item->setWidget(button);
    left_item->setOpacity(0.8);
    left_item->setFocusPolicy(Qt::NoFocus);

    osg_view_->scene()->addItem(left_item);
  }

  // right
  {
    auto* button = new QToolButton();
    button->setFocusPolicy(Qt::NoFocus);
    button->setAutoRaise(true);
    button->setIconSize(QSize(24, 24));
    button->setIcon(QIcon(":/resource/right.png"));
    button->setToolTip(tr("Right"));
    connect(button, &QToolButton::clicked, this, [this](bool) {
      osg::Vec3d eye = std::get<0>(move_point_map_[1]);
      osg::Vec3d center = std::get<1>(move_point_map_[1]);
      osg::Vec3d up = std::get<2>(move_point_map_[1]);

      manipulator_->moveToPoint(eye, center, up);
    });

    right_item->setCacheMode(QGraphicsTextItem::ItemCoordinateCache);
    right_item->setWidget(button);
    right_item->setOpacity(0.8);
    right_item->setFocusPolicy(Qt::NoFocus);

    osg_view_->scene()->addItem(right_item);
  }

  // front
  {
    auto* button = new QToolButton();
    button->setFocusPolicy(Qt::NoFocus);
    button->setAutoRaise(true);
    button->setIconSize(QSize(24, 24));
    button->setIcon(QIcon(":/resource/front.png"));
    button->setToolTip(tr("Front"));
    connect(button, &QToolButton::clicked, this, [this](bool) {
      osg::Vec3d eye = std::get<0>(move_point_map_[2]);
      osg::Vec3d center = std::get<1>(move_point_map_[2]);
      osg::Vec3d up = std::get<2>(move_point_map_[2]);

      manipulator_->moveToPoint(eye, center, up);
    });

    front_item->setCacheMode(QGraphicsTextItem::ItemCoordinateCache);
    front_item->setWidget(button);
    front_item->setOpacity(0.8);
    front_item->setFocusPolicy(Qt::NoFocus);

    osg_view_->scene()->addItem(front_item);
  }

  // behind
  {
    auto* button = new QToolButton();
    button->setFocusPolicy(Qt::NoFocus);
    button->setAutoRaise(true);
    button->setIconSize(QSize(24, 24));
    button->setIcon(QIcon(":/resource/behind.png"));
    button->setToolTip(tr("Behind"));
    connect(button, &QToolButton::clicked, this, [this](bool) {
      osg::Vec3d eye = std::get<0>(move_point_map_[3]);
      osg::Vec3d center = std::get<1>(move_point_map_[3]);
      osg::Vec3d up = std::get<2>(move_point_map_[3]);

      manipulator_->moveToPoint(eye, center, up);
    });

    behind_item->setCacheMode(QGraphicsTextItem::ItemCoordinateCache);
    behind_item->setWidget(button);
    behind_item->setOpacity(0.8);
    behind_item->setFocusPolicy(Qt::NoFocus);

    osg_view_->scene()->addItem(behind_item);
  }

  int index = 0;
  for (const auto& url : select_urls_) {
    auto* check_box = new CustomCheckBox;
    check_box->setText(QString::fromStdString(url));
    check_box->setChecked(true);
    check_box->setFocusPolicy(Qt::NoFocus);
    connect(check_box, &CustomCheckBox::clicked, this, [this, check_box](bool checked) {
      std::lock_guard lock(window_->data_mutex_);

      std::string url = check_box->text().toStdString();

      auto* geode = geo_node_map_[url];

      if (!geode) {
        return;
      }

      if (checked) {
        select_urls_.emplace(url);
        geode->setNodeMask(0xFFFFFFFF);
      } else {
        select_urls_.erase(url);
        geode->setNodeMask(0);
      }

      update_points();
    });

    auto* proxy = new QGraphicsProxyWidget;

    proxy->setCacheMode(QGraphicsTextItem::ItemCoordinateCache);
    proxy->setWidget(check_box);
    proxy->setOpacity(0.8);

    osg_view_->scene()->addItem(proxy);

    proxy->setPos(5, (check_box->height() + 5) * index + 5);

    ++index;
  }

#if QT_VERSION >= QT_VERSION_CHECK(6, 7, 0)
  connect(ui->checkBox_select, &QCheckBox::checkStateChanged, this,
          [this, selection_item](Qt::CheckState) { selection_item->setVisible(ui->checkBox_select->isChecked()); });
#else
  connect(ui->checkBox_select, &QCheckBox::stateChanged, this,
          [this, selection_item](int) { selection_item->setVisible(ui->checkBox_select->isChecked()); });
#endif

  connect(osg_view_, &OsgGraphicsView::fpsRateChanged, this,
          [fps_item](int fpsRate) { fps_item->setPlainText("FPS: " + QString::number(fpsRate)); });

  connect(osg_view_->scene(), &QGraphicsScene::sceneRectChanged, this,
          [logo_item, fps_item, selection_item, home_item, left_item, right_item, front_item,
           behind_item](const QRectF& rect) {
            logo_item->setPos((rect.width() - logo_item->boundingRect().width()) / 2,
                              rect.height() - logo_item->boundingRect().height() - 10);

            fps_item->setPos(rect.width() - fps_item->boundingRect().width() - 30, 5);

            selection_item->setPos(rect.width() / 2 - selection_item->boundingRect().width() / 2,
                                   rect.height() - selection_item->boundingRect().height() - 10);

            home_item->setPos(rect.width() - home_item->boundingRect().width() - 10,
                              rect.height() / 2 - (home_item->boundingRect().height() + 20) * 5 / 2);

            left_item->setPos(rect.width() - left_item->boundingRect().width() - 10,
                              rect.height() / 2 - (left_item->boundingRect().height() + 20) * 5 / 2 +
                                  (left_item->boundingRect().height() + 20) * 1);

            right_item->setPos(rect.width() - right_item->boundingRect().width() - 10,
                               rect.height() / 2 - (right_item->boundingRect().height() + 20) * 5 / 2 +
                                   (right_item->boundingRect().height() + 20) * 2);

            front_item->setPos(rect.width() - front_item->boundingRect().width() - 10,
                               rect.height() / 2 - (front_item->boundingRect().height() + 20) * 5 / 2 +
                                   (front_item->boundingRect().height() + 20) * 3);

            behind_item->setPos(rect.width() - behind_item->boundingRect().width() - 10,
                                rect.height() / 2 - (behind_item->boundingRect().height() + 20) * 5 / 2 +
                                    (behind_item->boundingRect().height() + 20) * 4);
          });

#else
  osg_widget_ = new OsgWidget(ui->label_osg);
  osg_layout_->addWidget(osg_widget_);
#endif
#endif

  init_osg();

  timer_->start();

  ui->label_size2->setText(QString::number(VLINK_VIEWER_PLATFORM_SIZE) + "x" +
                           QString::number(VLINK_VIEWER_PLATFORM_SIZE));
  ui->label_grid2->setText(QString::number(VLINK_VIEWER_PLATFORM_SIZE / VLINK_VIEWER_PLATFORM_GRID_COUNT));
  ui->label_count2->setText("---");
  ui->label_frame2->setText("---");
  ui->label_min2->setText("---");
  ui->label_max2->setText("---");
  ui->label_average2->setText("---");
  ui->groupBox_point->setEnabled(false);

  QSettings settings(QStandardPaths::writableLocation(QStandardPaths::CacheLocation) + "/settings.ini",
                     QSettings::IniFormat);

  if (this->parent()) {
    settings.beginGroup("Point3DDialog_ext");
  } else {
    settings.beginGroup("Point3DDialog");
  }

  auto geometry = settings.value("geometry", this->geometry()).toByteArray();
  restoreGeometry(geometry);

  settings.endGroup();

  ui->pushButton_close->setFocusPolicy(Qt::NoFocus);

#ifdef VLINK_ENABLE_VIEWER_OSG
#if USE_GRAPHICS_VIEW
  osg_view_->setFocus();
#else
  osg_widget_->setFocus();
#endif
#else
  setFocus();
#endif
}

Point3DDialog::~Point3DDialog() {
  {
    QSettings settings(QStandardPaths::writableLocation(QStandardPaths::CacheLocation) + "/settings.ini",
                       QSettings::IniFormat);

    if (this->parent()) {
      settings.beginGroup("Point3DDialog_ext");
    } else {
      settings.beginGroup("Point3DDialog");
    }

    settings.setValue("geometry", saveGeometry());

    settings.endGroup();

    settings.sync();
  }

  timer_->stop();

  if (auto* parent_dialog = qobject_cast<CameraDialog*>(parent())) {
    std::lock_guard lock(parent_dialog->data_mutex_);
    parent_dialog->data_callback_ = nullptr;
  } else if (!parent() && window_) {
    std::lock_guard lock(window_->data_mutex_);
    window_->data_callback_ = nullptr;
  }

  {
    std::lock_guard lock(data_mutex_);

    data_callback_ = nullptr;
  }

  // if (camera_dialog_) {
  //   camera_dialog_->close();
  //   camera_dialog_->deleteLater();
  //   camera_dialog_ = nullptr;
  // }

  delete ui;
}

void Point3DDialog::init_osg() {
#ifdef VLINK_ENABLE_VIEWER_OSG

  if (osg_inited_) {
    return;
  }

  root_group_ = new osg::Group;

#if USE_GRAPHICS_VIEW
  auto* viewer = osg_view_->getViewer();
#else
  auto* viewer = osg_widget_->getViewer();
#endif

  viewer->setSceneData(root_group_);

  // Camera
  auto culling_mode = viewer->getCamera()->getCullingMode();
  culling_mode &= ~(osg::CullStack::SMALL_FEATURE_CULLING);
  viewer->getCamera()->setCullingMode(culling_mode);
  viewer->getCamera()->setComputeNearFarMode(osgUtil::CullVisitor::DO_NOT_COMPUTE_NEAR_FAR);
  viewer->getCamera()->setProjectionMatrixAsPerspective(30.0, 1920.0 / 1080.0, 1, VLINK_VIEWER_PLATFORM_SIZE * 4);
  viewer->getCamera()->setClearColor(osg::Vec4d(0.18, 0.18, 0.20, 1));

  // Manipulator
  manipulator_ = new OsgManipulator;
  manipulator_->setLimit(VLINK_VIEWER_PLATFORM_SIZE * 0.75, VLINK_VIEWER_PLATFORM_SIZE * 2,
                         VLINK_VIEWER_PLATFORM_SIZE * 0.01);
  manipulator_->setHomePosition(osg::Vec3d(0, 0, VLINK_VIEWER_PLATFORM_SIZE * 0.5), osg::Vec3d(1, 0, 0),
                                osg::Vec3d(0, 0, 1));
  viewer->setCameraManipulator(manipulator_);

  move_point_map_[0] =
      std::make_tuple(osg::Vec3d(0, VLINK_VIEWER_PLATFORM_SIZE * 0.1, VLINK_VIEWER_PLATFORM_SIZE * 0.01),
                      osg::Vec3d(1, 0, 0), osg::Vec3d(0, 0, 1));

  move_point_map_[1] =
      std::make_tuple(osg::Vec3d(0, -VLINK_VIEWER_PLATFORM_SIZE * 0.1, VLINK_VIEWER_PLATFORM_SIZE * 0.01),
                      osg::Vec3d(-1, 0, 0), osg::Vec3d(0, 0, 1));

  move_point_map_[2] =
      std::make_tuple(osg::Vec3d(VLINK_VIEWER_PLATFORM_SIZE * 0.1, 0, VLINK_VIEWER_PLATFORM_SIZE * 0.01),
                      osg::Vec3d(0, -1, 0), osg::Vec3d(0, 0, 1));

  move_point_map_[3] =
      std::make_tuple(osg::Vec3d(-VLINK_VIEWER_PLATFORM_SIZE * 0.1, 0, VLINK_VIEWER_PLATFORM_SIZE * 0.01),
                      osg::Vec3d(0, 1, 0), osg::Vec3d(0, 0, 1));

  // StatsHandler
  // viewer->addEventHandler(new osgViewer::StatsHandler);
  // viewer->addEventHandler(new osgGA::StateSetManipulator(viewer->getCamera()->getOrCreateStateSet()));

  // OsgCoord
  {
    QFile font_file(":/resource/notomono.ttf");

    osg::ref_ptr<osgText::Font> font;

    if (font_file.open(QIODevice::ReadOnly)) {
      QByteArray font_data = font_file.readAll();

      font_file.close();

      std::istringstream font_stream(font_data.toStdString());

      font = osgText::readRefFontStream(font_stream);

      // font = osgText::readRefFontFile("/work/vlink/middleware/vlink/viewer/resource/notomono.ttf");

      // VLOG_W(font.valid());
    }

    auto ratio = qApp->devicePixelRatio();

    root_group_->addChild(OsgCoord::create(viewer->getCamera(), font, ratio));
  }

  // OsgPlatform
  platform_node_ = OsgPlatform::create(VLINK_VIEWER_PLATFORM_SIZE, VLINK_VIEWER_PLATFORM_GRID_COUNT);
  root_group_->addChild(platform_node_);

  // OsgLight
  root_group_->addChild(OsgLight::create(viewer->getCamera(), VLINK_VIEWER_PLATFORM_SIZE * 3 / 4));

  // cullface
  {
    osg::ref_ptr<osg::CullFace> cullface = new osg::CullFace(osg::CullFace::BACK);
    root_group_->getOrCreateStateSet()->setAttribute(cullface);
    root_group_->getOrCreateStateSet()->setMode(GL_CULL_FACE, osg::StateAttribute::ON);
    root_group_->setCullingActive(true);
  }

  // // Fog
  // {
  //   osg::ref_ptr<osg::Fog> fog =
  //       static_cast<osg::Fog *>(root_group_->getOrCreateStateSet()->getAttribute(osg::StateAttribute::FOG));

  //   if (!fog.valid()) {
  //     fog = new osg::Fog;
  //     root_group_->getOrCreateStateSet()->setAttributeAndModes(fog);
  //   }

  //   fog->setMode(osg::Fog::LINEAR);
  //   fog->setStart(0.0);
  //   fog->setEnd(VLINK_VIEWER_PLATFORM_SIZE * 2.5);
  //   fog->setColor(osg::Vec4d(0.1, 0.1, 0.1, 1.0));
  //   fog->setDensity(1.0);
  //   fog->setUseRadialFog(true);
  // }

  // Points
  auto ratio = qApp->devicePixelRatio();

  point_size_ = ui->doubleSpinBox_point->value();

  for (const auto& url : select_urls_) {
    osg::ref_ptr<osg::Geode> geode = OsgPointCloud::create(point_size_, ratio);

    root_group_->addChild(geode);

    geo_node_map_.emplace(url, geode.get());
  }

  // Model
  {
    QFile car_file(":/resource/car.osgb");

    if (car_file.open(QIODevice::ReadOnly)) {
      QByteArray car_data = car_file.readAll();

      car_file.close();

      std::istringstream car_stream(car_data.toStdString());

      osg::ref_ptr<osgDB::ReaderWriter> reader = osgDB::Registry::instance()->getReaderWriterForExtension("osgb");

      if (reader) {
        auto result = reader->readNode(car_stream);

        car_node_ = result.getNode();

        if (car_node_.valid()) {
          root_group_->addChild(car_node_);
          ui->checkBox_car->setEnabled(true);
          ui->checkBox_car->setChecked(true);
        }
      }
    }
  }

  // Select Handler

  {
    osg::ref_ptr<osg::Camera> selectCamera = new osg::Camera;

    selectCamera->setClearMask(GL_DEPTH_BUFFER_BIT);
    selectCamera->getOrCreateStateSet()->setMode(GL_LIGHTING, osg::StateAttribute::OFF);
    selectCamera->setRenderOrder(osg::Camera::RenderOrder::POST_RENDER);
    selectCamera->setAllowEventFocus(false);
    selectCamera->setReferenceFrame(osg::Transform::ReferenceFrame::ABSOLUTE_RF);
    selectCamera->setViewMatrix(osg::Matrix::identity());

    root_group_->addChild(selectCamera);

    select_handler_ = new OsgSelectHandler(selectCamera);

    select_handler_->setRatio(qApp->devicePixelRatio());

    select_handler_->registerSelectCallback([this](double xMin, double xMax, double yMin, double yMax) {
#if USE_GRAPHICS_VIEW
      osgViewer::Viewer* viewer = osg_view_->getViewer();
#else
      osgViewer::Viewer* viewer = osg_widget_->getViewer();
#endif

      if (!viewer) {
        return;
      }

      int64_t point_index = 1;
      QString context;
      for (auto& [url, list] : point3d_map_) {
        for (auto& [x, y, z, index, c1, c2, intensity, value_list] : list) {
          osg::Vec3d screen_point = OsgCommon::worldToScreen(osg::Vec3d(x, y, z), viewer->getCamera());

          if (screen_point.x() >= xMin && screen_point.x() <= xMax && screen_point.y() >= yMin &&
              screen_point.y() <= yMax) {
            c2 = 0xFFFFFF;

            if (point_index > 5000) {
              if (point_index == 5000 + 1) {
                context += "{......}";
              }
            } else {
              context += "# NUM_" + QString::number(point_index) + QString(" IDX_") + QString::number(index) + "\n";
              for (const auto& [name, type, value] : value_list) {
                context += QString::fromStdString(name) + ": " + QString::number(value) + "\n";
              }

              context += "\n";
            }

            ++point_index;
          } else {
            c2 = c1;
          }
        }
      }

      ui->textEdit->setText(context);

      update_points();
    });

    select_handler_->registerCtrlCallback([]() { return (qApp->keyboardModifiers() & Qt::ControlModifier); });

    viewer->addEventHandler(select_handler_);
  }

  // // Fly Points
  // {
  //   QVariantMap fly_points = {{"name", "home"},
  //                             {"path", QVariantList{QVariantMap{{"position", "-58.0731,-0.339068,19.2924"},
  //                                                               {"rotation",
  //                                                               "0.434866,-0.432977,-0.557094,0.559525"},
  //                                                               {"scale", "1,1,1"},
  //                                                               {"time", 10}}}}};

  //   manipulator->setFlyList(OsgCommon::getFlyList(QVariantList() << fly_points));
  // }

  osg_inited_ = true;
#endif
}

void Point3DDialog::showEvent(QShowEvent* event) { QDialog::showEvent(event); }

void Point3DDialog::hideEvent(QHideEvent* event) { QDialog::hideEvent(event); }

void Point3DDialog::mousePressEvent(QMouseEvent* event) { QDialog::mousePressEvent(event); }

void Point3DDialog::mouseMoveEvent(QMouseEvent* event) { QDialog::mouseMoveEvent(event); }

void Point3DDialog::mouseReleaseEvent(QMouseEvent* event) { QDialog::mouseReleaseEvent(event); }

void Point3DDialog::keyPressEvent(QKeyEvent* event) {
#ifdef VLINK_ENABLE_VIEWER_OSG
#if USE_GRAPHICS_VIEW

  if (ui->lineEdit_exp->hasFocus()) {
    QDialog::keyPressEvent(event);
    return;
  }

  // if (!osg_view_->geometry().contains(osg_view_->mapFromGlobal(QCursor::pos()))) {
  //   QDialog::keyPressEvent(event);
  //   return;
  // }
#else

  if (!osg_widget_->geometry().contains(osg_widget_->mapFromGlobal(QCursor::pos()))) {
    QDialog::keyPressEvent(event);
    return;
  }
#endif

  if (event->key() == Qt::Key_Control) {
    if (!ui->checkBox_select->isChecked()) {
      ui->checkBox_select->setChecked(true);
      on_checkBox_select_clicked(true);
    }
  } else if (event->key() == Qt::Key_Q || event->key() == Qt::Key_Shift || event->key() == Qt::Key_Alt) {
    if (ui->checkBox_select->isChecked()) {
      ui->checkBox_select->setChecked(false);
      on_checkBox_select_clicked(false);
    }
  }

  QDialog::keyPressEvent(event);
#else
  QDialog::keyPressEvent(event);
#endif
}

void Point3DDialog::keyReleaseEvent(QKeyEvent* event) { QDialog::keyReleaseEvent(event); }

void Point3DDialog::paintEvent(QPaintEvent* event) { QDialog::paintEvent(event); }

void Point3DDialog::closeEvent(QCloseEvent* event) { QDialog::closeEvent(event); }

void Point3DDialog::resizeEvent(QResizeEvent* event) {
  QDialog::resizeEvent(event);

#ifdef VLINK_ENABLE_VIEWER_OSG
#if USE_GRAPHICS_VIEW

  if (osg_view_) {
    osg_view_->resize(ui->label_osg->width(), ui->label_osg->height());
  }
#else

  if (osg_widget_) {
    osg_widget_->resize(ui->label_osg->width(), ui->label_osg->height());
  }
#endif
#endif
}

void Point3DDialog::on_pushButton_close_clicked() { this->close(); }

void Point3DDialog::on_checkBox_platform_clicked(bool checked) {
#ifdef VLINK_ENABLE_VIEWER_OSG

  if (!osg_inited_) {
    ui->checkBox_platform->setChecked(false);
    return;
  }

  if (!platform_node_ || !platform_node_.valid()) {
    ui->checkBox_platform->setChecked(false);
    return;
  }

  if (checked) {
    platform_node_->setNodeMask(0xFFFFFFFF);
  } else {
    platform_node_->setNodeMask(0);
  }
#else
  (void)checked;
#endif
}

void Point3DDialog::on_checkBox_car_clicked(bool checked) {
#ifdef VLINK_ENABLE_VIEWER_OSG

  if (!osg_inited_) {
    ui->checkBox_car->setChecked(false);
    return;
  }

  if (!car_node_ || !car_node_.valid()) {
    ui->checkBox_car->setChecked(false);
    return;
  }

  if (checked) {
    car_node_->setNodeMask(0xFFFFFFFF);
  } else {
    car_node_->setNodeMask(0);
  }
#else
  (void)checked;
#endif
}

void Point3DDialog::on_pushButton_camera_clicked() {
  if (camera_dialog_) {
    camera_dialog_->show();
  } else {
    camera_dialog_ = new CameraDialog(this);
    camera_dialog_->show();
  }
}

void Point3DDialog::on_checkBox_select_clicked(bool checked) {
#ifdef VLINK_ENABLE_VIEWER_OSG

  if (select_handler_) {
    select_handler_->setSelecting(checked);
  }

  if (checked) {
    ui->stackedWidget->setCurrentIndex(1);
  } else {
    ui->stackedWidget->setCurrentIndex(0);
  }

  ui->textEdit->clear();

  refresh_sence();
#else
  (void)checked;
#endif
}

void Point3DDialog::on_doubleSpinBox_point_valueChanged(double arg1) {
  (void)arg1;
  refresh_sence();
}

void Point3DDialog::on_doubleSpinBox_color_valueChanged(double arg1) {
  (void)arg1;
  refresh_sence();
}

void Point3DDialog::on_doubleSpinBox_min_valueChanged(double arg1) {
  (void)arg1;
  refresh_sence();
}

void Point3DDialog::on_doubleSpinBox_max_valueChanged(double arg1) {
  (void)arg1;
  refresh_sence();
}

void Point3DDialog::on_toolButton_inversion_clicked(bool checked) {
  if (checked) {
    ui->toolButton_inversion->setIcon(QIcon(":/resource/change_red.png"));
  } else {
    ui->toolButton_inversion->setIcon(QIcon(":/resource/change.png"));
  }

  refresh_sence();
}

void Point3DDialog::on_groupBox_range_clicked(bool checked) {
  (void)checked;
  refresh_sence();
}

void Point3DDialog::on_checkBox_exp_clicked(bool checked) {
  ui->lineEdit_exp->setEnabled(checked);

  has_expr_finished_ = false;

  if (checked) {
    current_expr_ = ui->lineEdit_exp->text();
    ui->lineEdit_exp->setStyleSheet("QLineEdit { color: red; }");
  } else {
    current_expr_.clear();
    ui->lineEdit_exp->setStyleSheet("");
  }

  refresh_sence();
}

void Point3DDialog::on_lineEdit_exp_editingFinished() {
  current_expr_ = ui->lineEdit_exp->text().simplified().replace(" ", "").toLower();

  cached_ast_ = parse_expression_to_ast(current_expr_);

  has_expr_finished_ = false;

  ui->lineEdit_exp->setStyleSheet("QLineEdit { color: red; }");

  refresh_sence();
}

void Point3DDialog::refresh_sence() {
#ifdef VLINK_ENABLE_VIEWER_OSG
  std::lock_guard lock(cache_mtx_);
  for (const auto& [url, cache] : proxy_data_cache_) {
    if (!cache.raw.empty()) {
      QElapsedTimer timer;
      timer.start();

      if (cache.schema == vlink::SchemaType::kZeroCopy &&
          cache.ser == vlink::Serializer::get_serialized_type<vlink::zerocopy::PointCloud>()) {
        QMetaObject::invokeMethod(this, "update_ui_for_zero_copy_types", Qt::QueuedConnection,
                                  Q_ARG(QVariant, QVariant::fromValue<vlink::ProxyAPI::Data>(cache)), Q_ARG(bool, true),
                                  Q_ARG(QElapsedTimer, timer));
      } else if (cache.schema == vlink::SchemaType::kProtobuf && target_msg_) {
        QMetaObject::invokeMethod(this, "update_ui_for_proto", Qt::QueuedConnection,
                                  Q_ARG(QVariant, QVariant::fromValue<vlink::ProxyAPI::Data>(cache)), Q_ARG(bool, true),
                                  Q_ARG(QElapsedTimer, timer));
      } else if (cache.schema == vlink::SchemaType::kFlatbuffers && target_fbs_context_) {
        QMetaObject::invokeMethod(this, "update_ui_for_flatbuffers", Qt::QueuedConnection,
                                  Q_ARG(QVariant, QVariant::fromValue<vlink::ProxyAPI::Data>(cache)), Q_ARG(bool, true),
                                  Q_ARG(QElapsedTimer, timer));
      }
    }
  }
#endif
}

#ifdef VLINK_ENABLE_VIEWER_OSG
void Point3DDialog::update_ui_for_proto(const QVariant& variant, bool cache, const QElapsedTimer& timer) {
  if (!target_msg_) {
    return;
  }

  if (!cache && timer.elapsed() > 1000) {
    return;
  }

  const auto& proxy_data = variant.value<vlink::ProxyAPI::Data>();

  if (ui->comboBox_proto->currentIndex() < 0 ||
      static_cast<size_t>(ui->comboBox_proto->currentIndex()) >= msg_list_.size()) {
    return;
  }

  const auto& candidate = msg_list_.at(ui->comboBox_proto->currentIndex());
  const auto* field = candidate.repeated_field;

  point3d_map_[proxy_data.url].clear();

  if (!field) {
    return;
  }

  if (!target_msg_->ParseFromArray(proxy_data.raw.data(), proxy_data.raw.size())) {
    return;
  }

  point_size_ = ui->doubleSpinBox_point->value();

  int field_size = target_msg_->GetReflection()->FieldSize(*target_msg_, field);

  double percent = ui->doubleSpinBox_color->value() / 100.0;
  double total_value = 0;
  size_t total_cnt = 0;

  osg::Geode* geode = nullptr;
  osg::Vec3dArray* vertex_array = nullptr;
  osg::Vec4dArray* color_array = nullptr;

  if (osg_inited_) {
    geode = geo_node_map_[proxy_data.url];

    if (!geode) {
      return;
    }

    auto* geometry = static_cast<osg::Geometry*>(geode->getDrawable(0));
    vertex_array = static_cast<osg::Vec3dArray*>(geometry->getVertexArray());
    color_array = static_cast<osg::Vec4dArray*>(geometry->getColorArray());

    OsgPointCloud::update_point_size(geode, point_size_, std::min(point_size_ * 3, 15.0f), qApp->devicePixelRatio());
    OsgPointCloud::clear_arrays(geode);
  }

  bool has_expr_finished = false;
  const auto target_value_name = ui->comboBox_value->currentText().toStdString();
  const bool has_exp_value = ui->checkBox_select->isChecked() || ui->checkBox_exp->isChecked();

  auto update_value_stats = [this, &total_value, &total_cnt](double value) {
    if (value == std::numeric_limits<double>::max()) {
      return;
    }

    total_value += value;
    ++total_cnt;

    if (point_min_ == std::numeric_limits<double>::max()) {
      point_min_ = value;
    } else {
      point_min_ = std::min(point_min_, value);
    }

    if (point_max_ == std::numeric_limits<double>::max()) {
      point_max_ = value;
    } else {
      point_max_ = std::max(point_max_, value);
    }
  };

  for (int i = 0; i < field_size; ++i) {
    const auto* sub_msg = &target_msg_->GetReflection()->GetRepeatedMessage(*target_msg_, field, i);

    if (!sub_msg) {
      continue;
    }

    double x = 0;
    double y = 0;
    double z = 0;
    uint32_t color = 0xFF55FF;
    double p = std::numeric_limits<double>::max();

    if (!get_proto_numeric_by_path(*sub_msg, candidate.x_path, x) ||
        !get_proto_numeric_by_path(*sub_msg, candidate.y_path, y) ||
        !get_proto_numeric_by_path(*sub_msg, candidate.z_path, z) || std::isnan(x) || std::isnan(y) || std::isnan(z)) {
      continue;
    }

    PointValueList value_list;

    if (has_exp_value) {
      value_list.reserve(candidate.value_fields.size() + 3);
      value_list.emplace_back("x", vlink::zerocopy::PointCloud::kFloatType, x);
      value_list.emplace_back("y", vlink::zerocopy::PointCloud::kFloatType, y);
      value_list.emplace_back("z", vlink::zerocopy::PointCloud::kFloatType, z);
    }

    for (const auto& value_field : candidate.value_fields) {
      double value = 0;

      if (!get_proto_numeric_by_path(*sub_msg, value_field.path, value)) {
        continue;
      }

      if (has_exp_value) {
        value_list.emplace_back(value_field.display_name, value_field.value_type, value);
      }

      if (value_field.leaf_name == "color") {
        color = static_cast<uint32_t>(value);
      }

      if (ui->comboBox_value->currentIndex() > 1 && target_value_name == value_field.display_name) {
        p = value;
      }
    }

    if (ui->comboBox_value->currentIndex() == 1) {
      p = get_point3d_distance(x, y, z);
    }

    update_value_stats(p);

    if (!check_expression(i, value_list)) {
      continue;
    }

    has_expr_finished = true;

    if (ui->groupBox_range->isChecked()) {
      auto c = get_point3d_color(p, ui->doubleSpinBox_min->value() * percent, ui->doubleSpinBox_max->value() * percent,
                                 ui->toolButton_inversion->isChecked());
      point3d_map_[proxy_data.url].emplace_back(x, y, z, i, c, c, -1, std::move(value_list));

      if (vertex_array && color_array) {
        QColor tcolor(c);
        vertex_array->push_back(osg::Vec3d(x, y, z));
        color_array->push_back(osg::Vec4d(tcolor.redF(), tcolor.greenF(), tcolor.blueF(), tcolor.alphaF()));
      }
    } else {
      if (p == std::numeric_limits<double>::max() || point_min_ == std::numeric_limits<double>::max() ||
          point_max_ == std::numeric_limits<double>::max()) {
        point3d_map_[proxy_data.url].emplace_back(x, y, z, i, color, color, -1, std::move(value_list));

        if (vertex_array && color_array) {
          QColor tcolor(color);
          vertex_array->push_back(osg::Vec3d(x, y, z));
          color_array->push_back(osg::Vec4d(tcolor.redF(), tcolor.greenF(), tcolor.blueF(), tcolor.alphaF()));
        }
      } else {
        auto c =
            get_point3d_color(p, point_min_ * percent, point_max_ * percent, ui->toolButton_inversion->isChecked());
        point3d_map_[proxy_data.url].emplace_back(x, y, z, i, c, c, -1, std::move(value_list));

        if (vertex_array && color_array) {
          QColor tcolor(c);
          vertex_array->push_back(osg::Vec3d(x, y, z));
          color_array->push_back(osg::Vec4d(tcolor.redF(), tcolor.greenF(), tcolor.blueF(), tcolor.alphaF()));
        }
      }
    }

    ++total_point_count_;
  }

  if (osg_inited_) {
    OsgPointCloud::finalize_arrays(geode);
  }

  if (total_cnt == 0) {
    average_value_ = std::numeric_limits<double>::max();
  } else {
    average_value_ = static_cast<double>(total_value / total_cnt);
  }

  if (!cache) {
    ++frame_count_;
  }

  if (has_expr_finished == true && has_expr_finished_ == false && !current_expr_.isEmpty()) {
    has_expr_finished_ = true;
    ui->lineEdit_exp->setStyleSheet("QLineEdit { color: green; }");
  }

  if (!this->isVisible()) {
    emit point3d_map_changed();
  }
}

void Point3DDialog::update_ui_for_zero_copy_types(const QVariant& variant, bool cache, const QElapsedTimer& timer) {
  if (!cache && timer.elapsed() > 1000) {
    return;
  }

  const auto& proxy_data = variant.value<vlink::ProxyAPI::Data>();

  vlink::zerocopy::PointCloud pcl;
  pcl << proxy_data.raw;

  point3d_map_[proxy_data.url].clear();

  if (!pcl.is_valid()) {
    return;
  }

  vlink::zerocopy::PointCloud::KeyList key_list;

  auto key_map = pcl.get_key_map(&key_list);

  if (key_map.empty()) {
    return;
  }

  osg::Geode* geode = nullptr;
  osg::Vec3dArray* vertex_array = nullptr;
  osg::Vec4dArray* color_array = nullptr;

  if (osg_inited_) {
    geode = geo_node_map_[proxy_data.url];

    if (!geode) {
      return;
    }

    auto* geometry = static_cast<osg::Geometry*>(geode->getDrawable(0));
    vertex_array = static_cast<osg::Vec3dArray*>(geometry->getVertexArray());
    color_array = static_cast<osg::Vec4dArray*>(geometry->getColorArray());

    OsgPointCloud::update_point_size(geode, point_size_, std::min(point_size_ * 3, 15.0f), qApp->devicePixelRatio());
    OsgPointCloud::clear_arrays(geode);
  }

  point_size_ = ui->doubleSpinBox_point->value();
  point3d_map_[proxy_data.url].clear();

  if (last_key_num_ != pcl.get_protocol_size_num() || last_key_str_ != pcl.get_protocol_name_str()) {
    last_key_num_ = pcl.get_protocol_size_num();
    last_key_str_ = pcl.get_protocol_name_str();

    ui->comboBox_value->blockSignals(true);

    for (size_t i = 0; i < key_list.size(); ++i) {
      const auto& key = key_list.at(i);
      if (key.name == "x" || key.name == "y" || key.name == "z") {
        continue;
      }

      bool has_item = false;

      for (int j = 0; j < ui->comboBox_value->count(); ++j) {
        if (ui->comboBox_value->itemText(j).toStdString() == key.name) {
          has_item = true;
          break;
        }
      }

      if (!has_item) {
        ui->comboBox_value->addItem(QString::fromStdString(key.name));
      }
    }

    ui->comboBox_value->blockSignals(false);
  }

  vlink::zerocopy::PointCloud::Key current_key;

  auto iter = std::find_if(key_list.begin(), key_list.end(), [this](const auto& item) {
    if (item.name == ui->comboBox_value->currentText().toStdString()) {
      return true;
    }

    return false;
  });

  if (iter != key_list.end()) {
    current_key = *iter;
  }

  vlink::zerocopy::PointCloud::Vector3f v3f;

  bool has_compressed_xyz = (pcl.get_extent() != 0);

  uint32_t color = 0xFF55FF;
  double p = std::numeric_limits<double>::max();

  double percent = ui->doubleSpinBox_color->value() / 100.0;
  double total_value = 0;
  size_t total_cnt = 0;

  bool has_expr_finished = false;

  bool has_exp_value = false;

  if (ui->checkBox_select->isChecked() || ui->checkBox_exp->isChecked()) {
    has_exp_value = true;
  }

  for (size_t i = 0; i < pcl.size(); ++i) {
    if (!pcl.get_value_v3f(v3f, i)) {
      continue;
    }

    if (std::isnan(v3f.x) || std::isnan(v3f.y) || std::isnan(v3f.z)) {
      continue;
    }

    float intensity = -1;

    if (ui->comboBox_value->currentIndex() == 1) {
      p = get_point3d_distance(v3f.x, v3f.y, v3f.z);

      if (!this->isVisible()) {
        intensity = pcl.get_value<float>(i, key_map, "intensity");
      }

    } else if (ui->comboBox_value->currentIndex() > 1) {
      if (current_key.size > 0 && !current_key.name.empty()) {
        if (current_key.type == vlink::zerocopy::PointCloud::kUnknownType) {
          p = pcl.get_value<float>(i, key_map, current_key.name);
        } else {
          p = pcl.get_value_for_double_float(i, key_map, current_key.name, current_key.type);
        }
      }
    }

    if (p != std::numeric_limits<double>::max()) {
      total_value += p;
    }

    ++total_cnt;

    if (point_min_ == std::numeric_limits<double>::max()) {
      point_min_ = p;
    } else {
      point_min_ = std::min(point_min_, p);
    }

    if (point_max_ == std::numeric_limits<double>::max()) {
      point_max_ = p;
    } else {
      point_max_ = std::max(point_max_, p);
    }

    PointValueList value_list;

    if (has_exp_value) {
      value_list.reserve(key_list.size());

      double v = 0;
      for (size_t key_index = 0; key_index < key_list.size(); ++key_index) {
        const auto& key = key_list[key_index];

        if (has_compressed_xyz && key_index < 3) {
          if (key_index == 0) {
            v = v3f.x;
          } else if (key_index == 1) {
            v = v3f.y;
          } else {
            v = v3f.z;
          }
        } else if (key.type == vlink::zerocopy::PointCloud::kUnknownType) {
          if (key.size == 1) {
            v = pcl.get_value<uint8_t>(i, key_map, key.name);
          } else if (key.size == 2) {
            v = pcl.get_value<int16_t>(i, key_map, key.name);
          } else if (key.size == 4) {
            v = pcl.get_value<float>(i, key_map, key.name);
          } else if (key.size == 8) {
            v = pcl.get_value<double>(i, key_map, key.name);
          }
        } else {
          v = pcl.get_value_for_double_float(i, key_map, key.name, key.type);
        }

        value_list.emplace_back(key.name, key.type, v);
      }
    }

    if (!check_expression(static_cast<int>(i), value_list)) {
      continue;
    }

    has_expr_finished = true;

    if (ui->groupBox_range->isChecked()) {
      auto c = get_point3d_color(p, ui->doubleSpinBox_min->value() * percent, ui->doubleSpinBox_max->value() * percent,
                                 ui->toolButton_inversion->isChecked());
      point3d_map_[proxy_data.url].emplace_back(v3f.x, v3f.y, v3f.z, i, c, c, intensity, std::move(value_list));

      if (vertex_array && color_array) {
        QColor tcolor(c);
        vertex_array->push_back(osg::Vec3d(v3f.x, v3f.y, v3f.z));
        color_array->push_back(osg::Vec4d(tcolor.redF(), tcolor.greenF(), tcolor.blueF(), tcolor.alphaF()));
      }
    } else {
      if (p == std::numeric_limits<double>::max() || point_min_ == std::numeric_limits<double>::max() ||
          point_max_ == std::numeric_limits<double>::max()) {
        point3d_map_[proxy_data.url].emplace_back(v3f.x, v3f.y, v3f.z, i, color, color, intensity,
                                                  std::move(value_list));
        if (vertex_array && color_array) {
          QColor tcolor(color);
          vertex_array->push_back(osg::Vec3d(v3f.x, v3f.y, v3f.z));
          color_array->push_back(osg::Vec4d(tcolor.redF(), tcolor.greenF(), tcolor.blueF(), tcolor.alphaF()));
        }
      } else {
        auto c =
            get_point3d_color(p, point_min_ * percent, point_max_ * percent, ui->toolButton_inversion->isChecked());
        point3d_map_[proxy_data.url].emplace_back(v3f.x, v3f.y, v3f.z, i, c, c, intensity, std::move(value_list));
        if (vertex_array && color_array) {
          QColor tcolor(c);
          vertex_array->push_back(osg::Vec3d(v3f.x, v3f.y, v3f.z));
          color_array->push_back(osg::Vec4d(tcolor.redF(), tcolor.greenF(), tcolor.blueF(), tcolor.alphaF()));
        }
      }
    }

    ++total_point_count_;
  }

  if (osg_inited_) {
    OsgPointCloud::finalize_arrays(geode);
  }

  if (total_cnt == 0) {
    average_value_ = std::numeric_limits<double>::max();
  } else {
    average_value_ = static_cast<double>(total_value / total_cnt);
  }

  if (!cache) {
    ++frame_count_;
  }

  if (has_expr_finished == true && has_expr_finished_ == false && !current_expr_.isEmpty()) {
    has_expr_finished_ = true;
    ui->lineEdit_exp->setStyleSheet("QLineEdit { color: green; }");
  }

  if (!this->isVisible()) {
    emit point3d_map_changed();
  }
}

void Point3DDialog::update_ui_for_flatbuffers(const QVariant& variant, bool cache, const QElapsedTimer& timer) {
  if (!target_fbs_context_ || !target_fbs_context_->valid()) {
    return;
  }

  if (!cache && timer.elapsed() > 1000) {
    return;
  }

  const auto& proxy_data = variant.value<vlink::ProxyAPI::Data>();

  if (ui->comboBox_proto->currentIndex() < 0 ||
      static_cast<size_t>(ui->comboBox_proto->currentIndex()) >= fbs_msg_list_.size()) {
    return;
  }

  const auto& candidate = fbs_msg_list_.at(ui->comboBox_proto->currentIndex());
  point3d_map_[proxy_data.url].clear();

  if (!candidate.repeated_field) {
    return;
  }

  FlatbuffersObjectView root_view;

  if (!make_root_view(*target_fbs_context_, proxy_data.raw, root_view)) {
    return;
  }

  const auto* root_field = candidate.repeated_field;

  if (!root_field ||
      (root_field->type()->base_type() != reflection::Vector &&
       root_field->type()->base_type() != reflection::Vector64) ||
      root_field->type()->element() != reflection::Obj) {
    return;
  }

  osg::Geode* geode = nullptr;
  osg::Vec3dArray* vertex_array = nullptr;
  osg::Vec4dArray* color_array = nullptr;

  if (osg_inited_) {
    geode = geo_node_map_[proxy_data.url];

    if (!geode) {
      return;
    }

    auto* geometry = static_cast<osg::Geometry*>(geode->getDrawable(0));
    vertex_array = static_cast<osg::Vec3dArray*>(geometry->getVertexArray());
    color_array = static_cast<osg::Vec4dArray*>(geometry->getColorArray());

    OsgPointCloud::update_point_size(geode, point_size_, std::min(point_size_ * 3, 15.0f), qApp->devicePixelRatio());
    OsgPointCloud::clear_arrays(geode);
  }

  point_size_ = ui->doubleSpinBox_point->value();

  const auto vector_size = get_vector_size(root_view, *root_field);
  const auto target_value_name = ui->comboBox_value->currentText().toStdString();
  const bool has_exp_value = ui->checkBox_select->isChecked() || ui->checkBox_exp->isChecked();

  uint32_t default_color = 0xFF55FF;
  double percent = ui->doubleSpinBox_color->value() / 100.0;
  double total_value = 0;
  size_t total_cnt = 0;
  bool has_expr_finished = false;

  for (size_t i = 0; i < vector_size; ++i) {
    FlatbuffersObjectView element_view;

    if (!get_vector_elem_view(root_view, *root_field, i, *target_fbs_context_->schema, element_view)) {
      continue;
    }

    double x = 0;
    double y = 0;
    double z = 0;

    if (!get_flatbuffers_numeric_by_path(element_view, candidate.x_path, *target_fbs_context_->schema, x) ||
        !get_flatbuffers_numeric_by_path(element_view, candidate.y_path, *target_fbs_context_->schema, y) ||
        !get_flatbuffers_numeric_by_path(element_view, candidate.z_path, *target_fbs_context_->schema, z) ||
        std::isnan(x) || std::isnan(y) || std::isnan(z)) {
      continue;
    }

    PointValueList value_list;

    if (has_exp_value) {
      value_list.emplace_back("x", vlink::zerocopy::PointCloud::kFloatType, x);
      value_list.emplace_back("y", vlink::zerocopy::PointCloud::kFloatType, y);
      value_list.emplace_back("z", vlink::zerocopy::PointCloud::kFloatType, z);
    }

    float intensity = -1;
    double p = std::numeric_limits<double>::max();
    uint32_t color = default_color;

    if (ui->comboBox_value->currentIndex() == 1) {
      p = get_point3d_distance(x, y, z);
    }

    for (const auto& value_field : candidate.value_fields) {
      double value = 0;

      if (!get_flatbuffers_numeric_by_path(element_view, value_field.path, *target_fbs_context_->schema, value)) {
        continue;
      }

      if (has_exp_value) {
        value_list.emplace_back(value_field.display_name, value_field.value_type, value);
      }

      if (value_field.leaf_name == "intensity" && !this->isVisible()) {
        intensity = static_cast<float>(value);
      }

      if (value_field.leaf_name == "color") {
        color = static_cast<uint32_t>(value);
      }

      if (ui->comboBox_value->currentIndex() > 1 && target_value_name == value_field.display_name) {
        p = value;
      }
    }

    if (!check_expression(static_cast<int>(i), value_list)) {
      continue;
    }

    has_expr_finished = true;

    if (p != std::numeric_limits<double>::max()) {
      total_value += p;
      ++total_cnt;

      if (point_min_ == std::numeric_limits<double>::max()) {
        point_min_ = p;
      } else {
        point_min_ = std::min(point_min_, p);
      }

      if (point_max_ == std::numeric_limits<double>::max()) {
        point_max_ = p;
      } else {
        point_max_ = std::max(point_max_, p);
      }
    }

    if (ui->groupBox_range->isChecked()) {
      auto c = get_point3d_color(p, ui->doubleSpinBox_min->value() * percent, ui->doubleSpinBox_max->value() * percent,
                                 ui->toolButton_inversion->isChecked());
      point3d_map_[proxy_data.url].emplace_back(x, y, z, i, c, c, intensity, std::move(value_list));

      if (vertex_array && color_array) {
        QColor tcolor(c);
        vertex_array->push_back(osg::Vec3d(x, y, z));
        color_array->push_back(osg::Vec4d(tcolor.redF(), tcolor.greenF(), tcolor.blueF(), tcolor.alphaF()));
      }
    } else {
      if (p == std::numeric_limits<double>::max() || point_min_ == std::numeric_limits<double>::max() ||
          point_max_ == std::numeric_limits<double>::max()) {
        point3d_map_[proxy_data.url].emplace_back(x, y, z, i, color, color, intensity, std::move(value_list));

        if (vertex_array && color_array) {
          QColor tcolor(color);
          vertex_array->push_back(osg::Vec3d(x, y, z));
          color_array->push_back(osg::Vec4d(tcolor.redF(), tcolor.greenF(), tcolor.blueF(), tcolor.alphaF()));
        }
      } else {
        auto c =
            get_point3d_color(p, point_min_ * percent, point_max_ * percent, ui->toolButton_inversion->isChecked());
        point3d_map_[proxy_data.url].emplace_back(x, y, z, i, c, c, intensity, std::move(value_list));

        if (vertex_array && color_array) {
          QColor tcolor(c);
          vertex_array->push_back(osg::Vec3d(x, y, z));
          color_array->push_back(osg::Vec4d(tcolor.redF(), tcolor.greenF(), tcolor.blueF(), tcolor.alphaF()));
        }
      }
    }

    ++total_point_count_;
  }

  if (osg_inited_) {
    OsgPointCloud::finalize_arrays(geode);
  }

  if (total_cnt == 0) {
    average_value_ = std::numeric_limits<double>::max();
  } else {
    average_value_ = static_cast<double>(total_value / total_cnt);
  }

  if (!cache) {
    ++frame_count_;
  }

  if (has_expr_finished && has_expr_finished_ == false && !current_expr_.isEmpty()) {
    has_expr_finished_ = true;
    ui->lineEdit_exp->setStyleSheet("QLineEdit { color: green; }");
  }

  if (!this->isVisible()) {
    emit point3d_map_changed();
  }
}

void Point3DDialog::update_points() {
  if (!osg_inited_) {
    return;
  }

  for (const auto& [url, list] : point3d_map_) {
    osg::Geode* geode = geo_node_map_[url];

    if (!geode) {
      continue;
    }

    auto* geometry = static_cast<osg::Geometry*>(geode->getDrawable(0));
    auto* vertex_array = static_cast<osg::Vec3dArray*>(geometry->getVertexArray());
    auto* color_array = static_cast<osg::Vec4dArray*>(geometry->getColorArray());

    auto* geometry_select = static_cast<osg::Geometry*>(geode->getDrawable(1));
    auto* vertex_array_select = static_cast<osg::Vec3dArray*>(geometry_select->getVertexArray());
    auto* color_array_select = static_cast<osg::Vec4dArray*>(geometry_select->getColorArray());

    OsgPointCloud::update_point_size(geode, point_size_, std::min(point_size_ * 3, 15.0f), qApp->devicePixelRatio());
    OsgPointCloud::clear_arrays(geode);

    for (const auto& [x, y, z, index, c1, c2, intensity, value_list] : list) {
      QColor color;

      if (select_handler_ && select_handler_->isSelecting()) {
        color = c2;
        if (c2 == 0xFFFFFF) {
          vertex_array_select->push_back(osg::Vec3d(x, y, z));
          color_array_select->push_back(osg::Vec4d(color.redF(), color.greenF(), color.blueF(), color.alphaF()));
        } else {
          vertex_array->push_back(osg::Vec3d(x, y, z));
          color_array->push_back(osg::Vec4d(color.redF(), color.greenF(), color.blueF(), color.alphaF()));
        }
      } else {
        color = c1;
        vertex_array->push_back(osg::Vec3d(x, y, z));
        color_array->push_back(osg::Vec4d(color.redF(), color.greenF(), color.blueF(), color.alphaF()));
      }
    }

    OsgPointCloud::finalize_arrays(geode);
  }
}

#endif

bool Point3DDialog::evaluate_expression(int index, const QString& exp_str, const PointValueList& value_list) {
  if (expression_cache_.contains(exp_str)) {
    return expression_cache_[exp_str](index, value_list);
  }

  vlink::Function<bool(int, const PointValueList&)> eval_func;

  bool ok = false;

  if (exp_str.startsWith("index")) {
    QString right_str = exp_str.mid(5);
    if (right_str.startsWith("==")) {
      qint64 target_value = right_str.mid(2).toLongLong(&ok);
      eval_func = [target_value](int index, const PointValueList&) { return index == target_value; };
    } else if (right_str.startsWith("!=")) {
      qint64 target_value = right_str.mid(2).toLongLong(&ok);
      eval_func = [target_value](int index, const PointValueList&) { return index != target_value; };
    } else if (right_str.startsWith(">=")) {
      qint64 target_value = right_str.mid(2).toLongLong(&ok);
      eval_func = [target_value](int index, const PointValueList&) { return index >= target_value; };
    } else if (right_str.startsWith(">")) {
      qint64 target_value = right_str.mid(1).toLongLong(&ok);
      eval_func = [target_value](int index, const PointValueList&) { return index > target_value; };
    } else if (right_str.startsWith("<=")) {
      qint64 target_value = right_str.mid(2).toLongLong(&ok);
      eval_func = [target_value](int index, const PointValueList&) { return index <= target_value; };
    } else if (right_str.startsWith("<")) {
      qint64 target_value = right_str.mid(1).toLongLong(&ok);
      eval_func = [target_value](int index, const PointValueList&) { return index < target_value; };
    }

    if (!ok) {
      return false;
    }
  } else {
    int op_type = 0;
    int op_indexof = -1;
    int op_offset = 0;

    for (; op_type < 6; ++op_type) {
      switch (op_type) {
        case 0:
          op_indexof = exp_str.indexOf("==");
          op_offset = 2;
          break;
        case 1:
          op_indexof = exp_str.indexOf("!=");
          op_offset = 2;
          break;
        case 2:
          op_indexof = exp_str.indexOf(">=");
          op_offset = 2;
          break;
        case 3:
          op_indexof = exp_str.indexOf(">");
          op_offset = 1;
          break;
        case 4:
          op_indexof = exp_str.indexOf("<=");
          op_offset = 2;
          break;
        case 5:
          op_indexof = exp_str.indexOf("<");
          op_offset = 1;
          break;
        default:
          op_indexof = -1;
          op_offset = 0;
          break;
      }

      if (op_indexof >= 0) {
        break;
      }
    }

    if (op_indexof < 0 || op_offset <= 0) {
      return false;
    }

    QString left_str = exp_str.left(op_indexof);
    QString right_str = exp_str.mid(op_indexof + op_offset);

    for (const auto& [name, type, value] : value_list) {
      if (name == left_str.toStdString()) {
        if (op_type == 0) {
          eval_func = [name = name, right_str](int, const PointValueList& value_list) {
            bool ok = false;

            for (const auto& [v_name, v_type, v_value] : value_list) {
              if (v_name == name) {
                if (v_type == vlink::zerocopy::PointCloud::kFloatType ||
                    v_type == vlink::zerocopy::PointCloud::kDoubleType) {
                  double target_value = right_str.toDouble(&ok);

                  if (!ok) {
                    return false;
                  }

                  return v_value == target_value;
                } else {
                  qint64 target_value = right_str.toLongLong(&ok);

                  if (!ok) {
                    return false;
                  }

                  return v_value == target_value;
                }
              }
            }

            return false;
          };
        } else if (op_type == 1) {
          eval_func = [name = name, right_str](int, const PointValueList& value_list) {
            bool ok = false;

            for (const auto& [v_name, v_type, v_value] : value_list) {
              if (v_name == name) {
                if (v_type == vlink::zerocopy::PointCloud::kFloatType ||
                    v_type == vlink::zerocopy::PointCloud::kDoubleType) {
                  double target_value = right_str.toDouble(&ok);

                  if (!ok) {
                    return false;
                  }

                  return v_value != target_value;
                } else {
                  qint64 target_value = right_str.toLongLong(&ok);

                  if (!ok) {
                    return false;
                  }

                  return v_value != target_value;
                }
              }
            }

            return false;
          };
        } else if (op_type == 2) {
          eval_func = [name = name, right_str](int, const PointValueList& value_list) {
            bool ok = false;

            for (const auto& [v_name, v_type, v_value] : value_list) {
              if (v_name == name) {
                if (v_type == vlink::zerocopy::PointCloud::kFloatType ||
                    v_type == vlink::zerocopy::PointCloud::kDoubleType) {
                  double target_value = right_str.toDouble(&ok);

                  if (!ok) {
                    return false;
                  }

                  return v_value >= target_value;
                } else {
                  qint64 target_value = right_str.toLongLong(&ok);

                  if (!ok) {
                    return false;
                  }

                  return v_value >= target_value;
                }
              }
            }

            return false;
          };
        } else if (op_type == 3) {
          eval_func = [name = name, right_str](int, const PointValueList& value_list) {
            bool ok = false;

            for (const auto& [v_name, v_type, v_value] : value_list) {
              if (v_name == name) {
                if (v_type == vlink::zerocopy::PointCloud::kFloatType ||
                    v_type == vlink::zerocopy::PointCloud::kDoubleType) {
                  double target_value = right_str.toDouble(&ok);

                  if (!ok) {
                    return false;
                  }

                  return v_value > target_value;
                } else {
                  qint64 target_value = right_str.toLongLong(&ok);

                  if (!ok) {
                    return false;
                  }

                  return v_value > target_value;
                }
              }
            }

            return false;
          };
        } else if (op_type == 4) {
          eval_func = [name = name, right_str](int, const PointValueList& value_list) {
            bool ok = false;

            for (const auto& [v_name, v_type, v_value] : value_list) {
              if (v_name == name) {
                if (v_type == vlink::zerocopy::PointCloud::kFloatType ||
                    v_type == vlink::zerocopy::PointCloud::kDoubleType) {
                  double target_value = right_str.toDouble(&ok);

                  if (!ok) {
                    return false;
                  }

                  return v_value <= target_value;
                } else {
                  qint64 target_value = right_str.toLongLong(&ok);

                  if (!ok) {
                    return false;
                  }

                  return v_value <= target_value;
                }
              }
            }

            return false;
          };
        } else if (op_type == 5) {
          eval_func = [name = name, right_str](int, const PointValueList& value_list) {
            bool ok = false;

            for (const auto& [v_name, v_type, v_value] : value_list) {
              if (v_name == name) {
                if (v_type == vlink::zerocopy::PointCloud::kFloatType ||
                    v_type == vlink::zerocopy::PointCloud::kDoubleType) {
                  double target_value = right_str.toDouble(&ok);

                  if (!ok) {
                    return false;
                  }

                  return v_value < target_value;
                } else {
                  qint64 target_value = right_str.toLongLong(&ok);

                  if (!ok) {
                    return false;
                  }

                  return v_value < target_value;
                }
              }
            }

            return false;
          };
        }

        break;
      }
    }
  }

  if (!eval_func) {
    return false;
  }

  const bool result = eval_func(index, value_list);

  expression_cache_.insert(exp_str, std::move(eval_func));

  return result;
}

bool Point3DDialog::check_expression(int index, const PointValueList& value_list) {
  if (!ui->checkBox_exp->isChecked() || value_list.empty()) {
    return true;
  }

  if (current_expr_.isEmpty()) {
    return true;
  }

  if (!cached_ast_) {
    return false;
  }

  return cached_ast_->eval(index, value_list);
}

std::shared_ptr<Point3DDialog::ASTNode> Point3DDialog::parse_expression_to_ast(const QString& expression) {
  QStack<std::shared_ptr<ASTNode>> node_stack;
  QStack<QChar> operator_stack;

  auto apply_operator = [&node_stack, &operator_stack]() {
    if (node_stack.size() < 2 || operator_stack.isEmpty()) {
      operator_stack.clear();
      return;
    }

    QChar op = operator_stack.top();
    operator_stack.pop();

    auto right_node = node_stack.top();
    node_stack.pop();

    auto left_node = node_stack.top();
    node_stack.pop();

    if (op == '&') {
      auto and_node = std::make_shared<ASTNode>(ASTNode::kTypeAnd);
      and_node->children = {left_node, right_node};

      and_node->eval = [left_node, right_node](int index, const PointValueList& value_list) {
        return left_node->eval(index, value_list) && right_node->eval(index, value_list);
      };

      node_stack.push(and_node);
    } else if (op == '|') {
      auto or_node = std::make_shared<ASTNode>(ASTNode::kTypeOr);
      or_node->children = {left_node, right_node};

      or_node->eval = [left_node, right_node](int index, const PointValueList& value_list) {
        return left_node->eval(index, value_list) || right_node->eval(index, value_list);
      };

      node_stack.push(or_node);
    }
  };

  int i = 0;
  while (i < expression.length()) {
    if (expression[i] == '(') {
      operator_stack.push(expression[i]);
    } else if (expression[i] == ')') {
      while (!operator_stack.isEmpty() && operator_stack.top() != '(') {
        apply_operator();
      }

      if (!operator_stack.isEmpty() && operator_stack.top() == '(') {
        operator_stack.pop();
      }
    } else if (expression[i] == '&' || expression[i] == '|') {
      while (!operator_stack.isEmpty() && operator_stack.top() != '(') {
        QChar top_op = operator_stack.top();

        if ((top_op == '&' && expression[i] == '|') || top_op == '|') {
          apply_operator();
        } else {
          break;
        }
      }

      operator_stack.push(expression[i]);
    } else {
      int j = i;
      while (j < expression.length() && expression[j] != '&' && expression[j] != '|' && expression[j] != '(' &&
             expression[j] != ')') {
        j++;
      }

      QString sub_expr = expression.mid(i, j - i);
      auto value_node = std::make_shared<ASTNode>(ASTNode::kTypeValue);

      value_node->eval = [this, sub_expr](int index, const PointValueList& value_list) {
        return evaluate_expression(index, sub_expr, value_list);
      };

      node_stack.push(value_node);

      i = j - 1;
    }

    i++;
  }

  while (!operator_stack.isEmpty()) {
    apply_operator();
  }

  return node_stack.isEmpty() ? nullptr : node_stack.top();
}

// NOLINTEND
