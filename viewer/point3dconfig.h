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

#include <vlink/impl/types.h>

#include <QHash>
#include <QRegularExpression>
#include <QString>
#include <QStringList>
#include <string>
#include <vector>

enum class Point3DRenderType {
  kPointCloud,
  kObjectDetection,
  kLaneLine,
  kPrediction,
  kTrafficLight,
  kStopLine,
  kTrafficSign,
  kFreespace,
  kOccupancyGrid,
  kParkingSlot,
  kEgoTrajectory,
  kHdMap,
  kCameraFrustum,
  kCovarianceEllipse,
};

class Point3DConfig final {
 public:
  struct RenderRule final {
    QString name;
    QString domain;
    Point3DRenderType type{Point3DRenderType::kPointCloud};
    QStringList schema_types;
    QStringList serializer_contains;
    QStringList serializer_equals;
    std::vector<QRegularExpression> serializer_regex;
    QStringList url_contains;
    QStringList url_equals;
    std::vector<QRegularExpression> url_regex;
    QStringList any_contains;
    std::vector<QRegularExpression> any_regex;
    int priority{0};
  };

  struct CollectionHint final {
    Point3DRenderType type{Point3DRenderType::kPointCloud};
    QStringList field_names;
  };

  struct ParserRule final {
    QString name;
    QString domain;
    Point3DRenderType type{Point3DRenderType::kPointCloud};
    QStringList schema_types;
    QStringList serializer_contains;
    QStringList serializer_equals;
    std::vector<QRegularExpression> serializer_regex;
    QStringList url_contains;
    QStringList url_equals;
    std::vector<QRegularExpression> url_regex;
    QStringList any_contains;
    std::vector<QRegularExpression> any_regex;
    int priority{0};
    QString collection_path;
    QString inner_collection_path;
    QHash<QString, QString> fields;
  };

  static Point3DConfig default_config();

  static QString render_type_to_string(Point3DRenderType type);

  static bool render_type_from_string(QString value, Point3DRenderType& type);

  [[nodiscard]] bool load_from_file(const QString& path, QString* error = nullptr);

  [[nodiscard]] Point3DRenderType detect_render_type(const std::string& url, const std::string& ser,
                                                     vlink::SchemaType schema_type) const;

  [[nodiscard]] Point3DRenderType collection_render_type(const std::string& normalized_name,
                                                         Point3DRenderType fallback) const;

  [[nodiscard]] const ParserRule* parser_rule_for(const std::string& url, const std::string& ser,
                                                  vlink::SchemaType schema_type, Point3DRenderType type) const;

  [[nodiscard]] bool should_skip(const QString& url, const QString& ser) const;

  [[nodiscard]] bool is_default() const noexcept { return source_path_.isEmpty(); }

  [[nodiscard]] const QString& source_path() const noexcept { return source_path_; }

  [[nodiscard]] QString source_label() const;

 private:
  void add_render_rule(Point3DRenderType type, QStringList schemas, QStringList serializer_contains,
                       QStringList url_contains, int priority, QString name = {}, QString domain = {},
                       QStringList serializer_equals = {}, QStringList url_equals = {}, QStringList any_contains = {},
                       QStringList serializer_regex = {}, QStringList url_regex = {}, QStringList any_regex = {});

  void add_collection_hint(Point3DRenderType type, QStringList field_names);

  void add_parser_rule(ParserRule rule);

  static QString schema_type_to_string(vlink::SchemaType schema_type);

  std::vector<RenderRule> render_rules_;
  std::vector<CollectionHint> collection_hints_;
  std::vector<ParserRule> parser_rules_;
  QStringList skip_url_contains_;
  QStringList skip_serializer_contains_;
  QStringList skip_url_equals_;
  QStringList skip_serializer_equals_;
  QString source_path_;
};

// NOLINTEND
