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

#include <QDialog>
#include <memory>
#include <vector>

#include "./perception_config.h"

struct FlatbuffersSchemaContext;
class MainWindow;
class QListWidget;
class QLabel;
class QLineEdit;
class QComboBox;
class QTableWidget;
class QTreeWidget;
class QTreeWidgetItem;

namespace google {
namespace protobuf {
class Descriptor;
}  // namespace protobuf
}  // namespace google

namespace reflection {
struct Schema;
struct Object;
}  // namespace reflection

class PerceptionEditorDialog : public QDialog {
  Q_OBJECT

 public:
  struct SourceMessage final {
    QString ser;
    vlink::SchemaType schema{vlink::SchemaType::kUnknown};
    const google::protobuf::Descriptor* proto_desc{nullptr};
    std::shared_ptr<FlatbuffersSchemaContext> fbs_ctx;
  };

  PerceptionEditorDialog(const PerceptionConfig& config, QString config_path, MainWindow* window,
                         std::vector<SourceMessage> sources = {}, QWidget* parent = nullptr);

  ~PerceptionEditorDialog() override;

  [[nodiscard]] const PerceptionConfig& result_config() const { return config_; }

  [[nodiscard]] const QString& result_path() const { return config_path_; }

 private slots:
  void on_mapping_selection_changed();

  void on_add_mapping();

  void on_remove_mapping();

  void on_type_changed(int index);

  void on_import_clicked();

  void on_save_clicked();

  void on_export_clicked();

  void on_field_double_clicked(QTreeWidgetItem* item, int column);

  void accept() override;

 private:
  struct Entry final {
    PerceptionConfig::MappingRule rule;
    bool is_hud{false};
  };

  void load_entries_from_config();

  void apply_entries_to_config();

  bool save_config_to(const QString& path);

  void update_path_label();

  [[nodiscard]] bool current_is_hud() const;

  void rebuild_mapping_list();

  void load_mapping_to_form(int index);

  void commit_form_to_mapping(int index);

  void rebuild_target_table(const QStringList& slot_names, const PerceptionConfig::MappingRule* rule);

  void rebuild_field_tree(const QString& ser, vlink::SchemaType schema);

  void add_proto_fields(QTreeWidgetItem* parent, const google::protobuf::Descriptor* descriptor, const QString& prefix,
                        int depth);

  void add_fbs_fields(QTreeWidgetItem* parent, const reflection::Object* object, const reflection::Schema* schema,
                      const QString& prefix, int depth);

  PerceptionConfig config_;
  QString config_path_;
  MainWindow* window_{nullptr};
  std::vector<SourceMessage> sources_;
  QStringList scanned_types_;
  std::vector<Entry> entries_;
  int current_index_{-1};

  QListWidget* mapping_list_{nullptr};
  QLabel* path_label_{nullptr};
  QLineEdit* name_edit_{nullptr};
  QLineEdit* ser_edit_{nullptr};
  QComboBox* type_combo_{nullptr};
  QComboBox* encoding_combo_{nullptr};
  QLineEdit* collection_edit_{nullptr};
  QLineEdit* inner_collection_edit_{nullptr};
  QTableWidget* target_table_{nullptr};
  QTreeWidget* field_tree_{nullptr};
};

// NOLINTEND
