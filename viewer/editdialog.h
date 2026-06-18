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

#include <vlink/base/bytes.h>

#include "./flatbuffers_runtime_compat.h"

#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wcpp"
#endif
#include <google/protobuf/descriptor.h>
#include <google/protobuf/reflection.h>
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic pop
#endif

#include <QCloseEvent>
#include <QDialog>
#include <QResizeEvent>
#include <QTreeWidgetItem>
#include <string>
#include <unordered_map>

namespace Ui {
class EditDialog;
}

class EditDialog : public QDialog {
  Q_OBJECT

 public:
  enum class EditValueKind : int {
    kUnknown = 0,
    kString = 1,
    kBytes = 2,
  };

  static constexpr int kEditValueKindRole = Qt::UserRole + 1;

  explicit EditDialog(QWidget* parent = nullptr);

  ~EditDialog();

 protected:
  void closeEvent(QCloseEvent* event) override;

  void resizeEvent(class QResizeEvent* event) override;

 private slots:
  void on_pushButton_add_clicked();

  void on_pushButton_del_clicked();

  void on_pushButton_send_clicked();

  void on_pushButton_stop_clicked();

  void on_pushButton_close_clicked();

 private:
  void set_child_items(QTreeWidgetItem* item);

  void publish_data();

  void update_status();

  // FlatBuffers editing support
  void init_fbs_tree();
  void init_fbs_tree_object(QTreeWidgetItem* parent_item, const std::string& parent_id,
                            const reflection::Object* object, const FlatbuffersObjectView* view);
  void add_fbs_vector_element(QTreeWidgetItem* array_item, const reflection::Field* field,
                              const FlatbuffersObjectView* view, int index);
  std::string build_fbs_json();
  std::string build_fbs_json_object(QTreeWidgetItem* item);
  std::string build_fbs_json_value(QTreeWidgetItem* item);

 private:
  Ui::EditDialog* ui;
  class MainWindow* window_{nullptr};
  class EditItemDelegate* delegate_{nullptr};
  class QTimer* send_timer_{nullptr};

  google::protobuf::Message* target_msg_{nullptr};

  vlink::Bytes raw_;
  int progress_index_{0};

  // FlatBuffers editing state
  bool is_fbs_mode_{false};
  std::shared_ptr<FlatbuffersSchemaContext> fbs_context_;
  std::unordered_map<QTreeWidgetItem*, const reflection::Field*> item_to_fbs_field_map_;
  std::unordered_map<QTreeWidgetItem*, const reflection::Object*> item_to_fbs_object_map_;
};

// NOLINTEND
