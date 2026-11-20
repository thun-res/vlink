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

#pragma once

#include <vlink/impl/types.h>

#include <QRegularExpression>
#include <QString>
#include <QStringList>
#include <string>
#include <vector>

#include "./perception_model.h"

class PerceptionConfig final {
 public:
  struct MatchSpec final {
    QStringList schema_types;
    QStringList serializer_contains;
    QStringList serializer_equals;
    std::vector<QRegularExpression> serializer_regex;
    QStringList url_contains;
    QStringList url_equals;
    std::vector<QRegularExpression> url_regex;
    QStringList any_contains;
    std::vector<QRegularExpression> any_regex;
  };

  struct RenderRule final {
    QString name;
    QString domain;
    perception::RenderType type{perception::RenderType::kPointCloud};
    MatchSpec match;
    int priority{0};
  };

  struct MappingRule final {
    QString name;
    QString ser;
    QString ser_normalized;
    perception::RenderType type{perception::RenderType::kObjectDetection};
    perception::Encoding encoding{perception::Encoding::kUnknown};
    MatchSpec match;
    int priority{0};
    QString collection;
    QString inner_collection;
    std::vector<perception::FieldMapping> field_mappings;
  };

  static PerceptionConfig default_config();

  static QString render_type_to_string(perception::RenderType type);

  static bool render_type_from_string(QString value, perception::RenderType& type);

  static QString encoding_to_string(perception::Encoding encoding);

  static perception::Encoding encoding_from_string(QString value);

  static const QStringList& target_slots_for(perception::RenderType type);

  [[nodiscard]] bool load_from_file(const QString& path, QString* error = nullptr);

  [[nodiscard]] bool save_to_file(const QString& path, QString* error = nullptr) const;

  [[nodiscard]] std::string to_json_string() const;

  [[nodiscard]] perception::RenderType detect_render_type(const std::string& url, const std::string& ser,
                                                          vlink::SchemaType schema_type) const;

  [[nodiscard]] const MappingRule* mapping_rule_for(const std::string& url, const std::string& ser,
                                                    vlink::SchemaType schema_type, perception::RenderType type) const;

  [[nodiscard]] std::vector<const MappingRule*> mappings_for(const std::string& url, const std::string& ser,
                                                             vlink::SchemaType schema_type) const;

  [[nodiscard]] std::vector<const MappingRule*> hud_bindings_for(const std::string& url, const std::string& ser,
                                                                 vlink::SchemaType schema_type) const;

  static const QStringList& hud_target_slots();

  [[nodiscard]] bool should_skip(const QString& url, const QString& ser) const;

  [[nodiscard]] bool is_default() const noexcept { return source_path_.isEmpty(); }

  [[nodiscard]] const QString& source_path() const noexcept { return source_path_; }

  [[nodiscard]] QString source_label() const;

  [[nodiscard]] std::vector<MappingRule>& mappings() noexcept { return mappings_; }

  [[nodiscard]] const std::vector<MappingRule>& mappings() const noexcept { return mappings_; }

  [[nodiscard]] std::vector<MappingRule>& hud_bindings() noexcept { return hud_bindings_; }

  [[nodiscard]] const std::vector<MappingRule>& hud_bindings() const noexcept { return hud_bindings_; }

  [[nodiscard]] std::vector<RenderRule>& render_rules() noexcept { return render_rules_; }

  [[nodiscard]] const std::vector<RenderRule>& render_rules() const noexcept { return render_rules_; }

  void set_source_path(const QString& path) { source_path_ = path; }

  static void finalize_mapping(MappingRule& rule);

  static QString schema_type_to_string(vlink::SchemaType schema_type);

 private:
  void add_render_rule(perception::RenderType type, QStringList schemas, QStringList serializer_contains,
                       QStringList url_contains, int priority, QString name = {}, QString domain = {},
                       QStringList serializer_equals = {}, QStringList url_equals = {}, QStringList any_contains = {},
                       QStringList serializer_regex = {}, QStringList url_regex = {}, QStringList any_regex = {});

  void add_mapping_rule(MappingRule rule);

  void add_hud_binding(MappingRule rule);

  std::vector<RenderRule> render_rules_;
  std::vector<MappingRule> mappings_;
  std::vector<MappingRule> hud_bindings_;
  QStringList skip_url_contains_;
  QStringList skip_serializer_contains_;
  QStringList skip_url_equals_;
  QStringList skip_serializer_equals_;
  QString source_path_;
};
