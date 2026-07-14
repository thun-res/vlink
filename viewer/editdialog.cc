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

#include "./editdialog.h"

#include <vlink/base/helpers.h>

#include <QCheckBox>
#include <QComboBox>
#include <QItemDelegate>
#include <QLineEdit>
#include <QMessageBox>
#include <QResizeEvent>
#include <QTimer>

#include "./mainwindow.h"
#include "./ui_editdialog.h"
#include "./ui_mainwindow.h"

[[maybe_unused]] static QString fbs_default_for_type(reflection::BaseType t) {
  if (t == reflection::Bool) {
    return "false";
  }

  if (t == reflection::String) {
    return "";
  }

  return "0";
}

[[maybe_unused]] static std::string escape_json_string(const std::string& s) {
  std::string out;
  out.reserve(s.size());
  for (char c : s) {
    switch (c) {
      case '"':
        out += "\\\"";
        break;
      case '\\':
        out += "\\\\";
        break;
      case '\n':
        out += "\\n";
        break;
      case '\r':
        out += "\\r";
        break;
      case '\t':
        out += "\\t";
        break;
      default:
        out += c;
        break;
    }
  }
  return out;
}

class EditItemDelegate : public QItemDelegate {
 public:
  using QItemDelegate::QItemDelegate;

  QWidget* createEditor(QWidget* parent, const QStyleOptionViewItem&, const QModelIndex& index) const override {
    QLineEdit* edit = nullptr;

    if (index.column() != 3) {
      return edit;
    }

    auto type = index.model()->index(index.row(), 0, index.parent()).data(Qt::UserRole).toInt();
    auto value_kind = index.model()->index(index.row(), 0, index.parent()).data(EditDialog::kEditValueKindRole).toInt();

    switch (type) {
      case google::protobuf::FieldDescriptor::CPPTYPE_INT32:
        edit = new QLineEdit(parent);
        edit->setValidator(MainWindow::get_instance()->validator_int32_);
        break;
      case google::protobuf::FieldDescriptor::CPPTYPE_INT64:
        edit = new QLineEdit(parent);
        edit->setValidator(MainWindow::get_instance()->validator_int64_);
        break;
      case google::protobuf::FieldDescriptor::CPPTYPE_UINT32:
        edit = new QLineEdit(parent);
        edit->setValidator(MainWindow::get_instance()->validator_uint32_);
        break;
      case google::protobuf::FieldDescriptor::CPPTYPE_UINT64:
        edit = new QLineEdit(parent);
        edit->setValidator(MainWindow::get_instance()->validator_uint64_);
        break;
      case google::protobuf::FieldDescriptor::CPPTYPE_DOUBLE:
        edit = new QLineEdit(parent);
        edit->setValidator(MainWindow::get_instance()->validator_double_);
        break;
      case google::protobuf::FieldDescriptor::CPPTYPE_FLOAT:
        edit = new QLineEdit(parent);
        edit->setValidator(MainWindow::get_instance()->validator_float_);
        break;
      case google::protobuf::FieldDescriptor::CPPTYPE_BOOL:
        edit = new QLineEdit(parent);
        edit->setValidator(MainWindow::get_instance()->validator_bool_);
        break;
      case google::protobuf::FieldDescriptor::CPPTYPE_ENUM: {
        edit = new QLineEdit(parent);
        edit->setValidator(MainWindow::get_instance()->validator_enum_);
      } break;
      case google::protobuf::FieldDescriptor::CPPTYPE_STRING:
        if (value_kind != static_cast<int>(EditDialog::EditValueKind::kString)) {
          break;
        }

        edit = new QLineEdit(parent);
        edit->setValidator(MainWindow::get_instance()->validator_string_);
        break;
      default:
        break;
    }

    return edit;
  }

  void setModelData(QWidget* editor, QAbstractItemModel* model, const QModelIndex& index) const override {
    QLineEdit* edit = qobject_cast<QLineEdit*>(editor);

    if (!edit) {
      return;
    }

    model->setData(index, edit->text());
  }

  void updateEditorGeometry(QWidget* editor, const QStyleOptionViewItem& option, const QModelIndex&) const override {
    editor->setGeometry(option.rect);
  }
};

EditDialog::EditDialog(QWidget* parent) : QDialog(parent), ui(new Ui::EditDialog) {
  setWindowFlags(Qt::Dialog | Qt::WindowMaximizeButtonHint | Qt::WindowCloseButtonHint);

  ui->setupUi(this);

  window_ = MainWindow::get_instance();

  if (!window_->desc_ && !window_->is_flatbuffers_types_) {
    QMessageBox::critical(this, tr("Critical"), tr("Logical error."));
    return;
  }

  QStringList property_headers = {
      tr("FIELD"),
      tr("TYPE"),
      tr("PROPERTY"),
      tr("VALUE"),
  };

  delegate_ = new EditItemDelegate();

  send_timer_ = new QTimer(this);

  {
    const auto schema_iter = window_->schema_type_map_.find(window_->current_url_);
    const auto schema_type =
        schema_iter != window_->schema_type_map_.end() ? schema_iter->second : vlink::SchemaType::kUnknown;

    vlink::ProxyAPI::Control control;
    control.mode = vlink::ProxyAPI::kEdit;
    control.url_meta_list.emplace_back(
        vlink::ProxyAPI::UrlMeta{window_->current_url_, window_->current_ser_, schema_type, vlink::kPublisher});

    if (window_->proxy_->get_current_config().role == vlink::ProxyAPI::kController) {
      if (!window_->proxy_->send_control(control)) {
        QMessageBox::critical(this, tr("Critical"), tr("Failed to configure proxy edit route."));
        return;
      }
    }
  }

  connect(send_timer_, &QTimer::timeout, this, [this]() { publish_data(); });

  connect(
      window_, &MainWindow::connect_changed, this,
      [this](bool connected) {
        if (!connected) {
          QMessageBox::warning(this, tr("Warning"), tr("Proxy is disconnected."));
          this->close();
        }
      },
      Qt::QueuedConnection);

  // ui->treeWidget_property->setRootIsDecorated(false);
  ui->treeWidget_property->setItemDelegate(delegate_);
  ui->treeWidget_property->setContextMenuPolicy(Qt::CustomContextMenu);
  ui->treeWidget_property->setHeaderLabels(property_headers);
  ui->treeWidget_property->setColumnWidth(0, 110);
  ui->treeWidget_property->setColumnWidth(1, 110);
  ui->treeWidget_property->setColumnWidth(2, 110);
  ui->treeWidget_property->setColumnWidth(3, 115);

  ui->spinBox_times->setValue(1);

  connect(ui->treeWidget_property, &QTreeWidget::customContextMenuRequested, this, [this](const QPoint& pos) {
    auto* item = ui->treeWidget_property->itemAt(pos);

    if (!item) {
      return;
    }

    int type = item->data(0, Qt::UserRole).toInt();

    if (type >= 100) {
      QMenu menu;
      menu.addAction(tr("Add New Element"), this, [this]() { on_pushButton_add_clicked(); });
      if (item->childCount() > 0) {
        menu.addAction(tr("Remove Last Element"), this, [this]() { on_pushButton_del_clicked(); });
      }
      menu.exec(QCursor::pos());
    }
  });

  connect(ui->treeWidget_property, &QTreeWidget::currentItemChanged, this,
          [this](QTreeWidgetItem*, QTreeWidgetItem*) { update_status(); });

  if (window_->is_flatbuffers_types_ && window_->current_fbs_context_ && window_->current_fbs_context_->valid()) {
    is_fbs_mode_ = true;
    fbs_context_ = window_->current_fbs_context_;
    init_fbs_tree();
  } else if (window_->desc_) {
    target_msg_ = window_->factory_->GetPrototype(window_->desc_)->New();
    window_->set_property_list(ui->treeWidget_property, "", target_msg_);
  }

  ui->pushButton_add->setEnabled(false);
  ui->pushButton_del->setEnabled(false);

  if (ui->treeWidget_property->topLevelItemCount() == 0) {
    ui->pushButton_send->setEnabled(false);
  } else {
    ui->pushButton_send->setEnabled(true);
  }

  ui->pushButton_stop->setEnabled(false);

  ui->treeWidget_property->expandAll();

  on_pushButton_stop_clicked();

  ui->pushButton_close->setFocusPolicy(Qt::NoFocus);
  setFocus();
}

EditDialog::~EditDialog() {
  delete delegate_;

  delete ui;

  if (target_msg_) {
    delete target_msg_;
  }
}

void EditDialog::closeEvent(QCloseEvent* event) {
  window_->clear_all_property_item(ui->treeWidget_property);
  QDialog::closeEvent(event);
}

void EditDialog::resizeEvent(QResizeEvent* event) {
  ui->treeWidget_property->setColumnWidth(0, 110);
  ui->treeWidget_property->setColumnWidth(1, (ui->treeWidget_property->size().width() - 110 - 25) / 3);
  ui->treeWidget_property->setColumnWidth(2, (ui->treeWidget_property->size().width() - 110 - 25) / 3);
  ui->treeWidget_property->setColumnWidth(3, (ui->treeWidget_property->size().width() - 110 - 25) / 3);

  QDialog::resizeEvent(event);
}

void EditDialog::on_pushButton_add_clicked() {
  QTreeWidgetItem* item = ui->treeWidget_property->currentItem();

  if (!item) {
    return;
  }

  int type = item->data(0, Qt::UserRole).toInt();

  if (type < 100) {
    return;
  }

  if (is_fbs_mode_) {
    const auto* field = item_to_fbs_field_map_[item];
    if (!field) {
      return;
    }

    int new_index = item->childCount();
    add_fbs_vector_element(item, field, nullptr, new_index);

    for (int i = 0; i < item->childCount(); ++i) {
      item->child(i)->setText(1, QString("[%1]").arg(i));
    }

    update_status();
    return;
  }

  auto* msg = window_->item_to_msg_map_[item];

  if (!msg) {
    return;
  }

  auto* field = window_->item_to_field_map_[item];

  if (!field) {
    return;
  }

  const auto* ref = msg->GetReflection();

  switch (field->cpp_type()) {
    case google::protobuf::FieldDescriptor::CPPTYPE_INT32:
      ref->AddInt32(msg, field, 0);
      break;
    case google::protobuf::FieldDescriptor::CPPTYPE_INT64:
      ref->AddInt64(msg, field, 0);
      break;
    case google::protobuf::FieldDescriptor::CPPTYPE_UINT32:
      ref->AddUInt32(msg, field, 0);
      break;
    case google::protobuf::FieldDescriptor::CPPTYPE_UINT64:
      ref->AddUInt64(msg, field, 0);
      break;
    case google::protobuf::FieldDescriptor::CPPTYPE_DOUBLE:
      ref->AddDouble(msg, field, 0);
      break;
    case google::protobuf::FieldDescriptor::CPPTYPE_FLOAT:
      ref->AddFloat(msg, field, 0);
      break;
    case google::protobuf::FieldDescriptor::CPPTYPE_BOOL:
      ref->AddBool(msg, field, false);
      break;
    case google::protobuf::FieldDescriptor::CPPTYPE_ENUM: {
      ref->AddEnumValue(msg, field, 0);
    } break;
    case google::protobuf::FieldDescriptor::CPPTYPE_STRING:
      ref->AddString(msg, field, "");
      break;
    case google::protobuf::FieldDescriptor::CPPTYPE_MESSAGE:
      ref->AddMessage(msg, field);
      break;
    default:
      break;
  }

  window_->set_property_list(ui->treeWidget_property, "", target_msg_);

  update_status();
}

void EditDialog::on_pushButton_del_clicked() {
  QTreeWidgetItem* item = ui->treeWidget_property->currentItem();

  if (!item) {
    return;
  }

  int type = item->data(0, Qt::UserRole).toInt();

  if (type < 100) {
    return;
  }

  if (is_fbs_mode_) {
    if (item->childCount() == 0) {
      return;
    }

    auto* last_child = item->takeChild(item->childCount() - 1);

    vlink::MoveFunction<void(QTreeWidgetItem*)> cleanup = [&](QTreeWidgetItem* it) {
      item_to_fbs_field_map_.erase(it);
      item_to_fbs_object_map_.erase(it);

      for (int i = 0; i < it->childCount(); ++i) {
        cleanup(it->child(i));
      }
    };

    cleanup(last_child);
    delete last_child;

    for (int i = 0; i < item->childCount(); ++i) {
      item->child(i)->setText(1, QString("[%1]").arg(i));
    }

    update_status();
    return;
  }

  auto* msg = window_->item_to_msg_map_[item];

  if (!msg) {
    return;
  }

  auto* field = window_->item_to_field_map_[item];

  if (!field) {
    return;
  }

  const auto* ref = msg->GetReflection();

  ref->RemoveLast(msg, field);

  window_->set_property_list(ui->treeWidget_property, "", target_msg_);

  update_status();
}

void EditDialog::on_pushButton_send_clicked() {
  if (is_fbs_mode_) {
    std::string json = build_fbs_json();

    fbs_context_->parser->builder_.Clear();
    if (!fbs_context_->parser->ParseJson(json.c_str())) {
      QMessageBox::warning(this, tr("Warning"),
                           tr("Failed to build FlatBuffer from edited data.\n%1")
                               .arg(QString::fromStdString(fbs_context_->parser->error_)));
      return;
    }

    raw_ = vlink::Bytes::create(fbs_context_->parser->builder_.GetSize());
    std::memcpy(raw_.data(), fbs_context_->parser->builder_.GetBufferPointer(), raw_.size());
  } else {
    window_->set_property_list(ui->treeWidget_property, "", target_msg_);

    raw_ = vlink::Bytes::create(target_msg_->ByteSizeLong());
    if (!target_msg_->SerializeToArray(raw_.data(), raw_.size())) {
      return;
    }
  }

  progress_index_ = 0;
  send_timer_->setInterval(ui->spinBox_interval->value());
  ui->progressBar->setValue(0);
  ui->progressBar->setMaximum(ui->spinBox_times->value());
  ui->progressBar->setEnabled(true);
  ui->pushButton_send->setEnabled(false);
  ui->pushButton_stop->setEnabled(true);
  ui->treeWidget_property->setEnabled(false);

  ui->label_status2->setText(tr("Sending"));
  ui->label_status2->setStyleSheet("QLabel { color: green; }");

  publish_data();

  if (ui->groupBox_config->isChecked() && ui->spinBox_times->value() != 1) {
    send_timer_->stop();
    send_timer_->start();
  }
}

void EditDialog::on_pushButton_stop_clicked() {
  send_timer_->stop();
  progress_index_ = 0;
  send_timer_->setInterval(0);
  ui->progressBar->setValue(0);
  ui->progressBar->setMaximum(1);
  ui->progressBar->setEnabled(false);
  ui->pushButton_send->setEnabled(true);
  ui->pushButton_stop->setEnabled(false);
  ui->treeWidget_property->setEnabled(true);

  ui->label_status2->setText(tr("Stopped"));
  ui->label_status2->setStyleSheet("QLabel { color: red; }");
}

void EditDialog::on_pushButton_close_clicked() { this->close(); }

void EditDialog::set_child_items(QTreeWidgetItem* item) {
  for (int i = 0; i < item->childCount(); ++i) {
    auto* pitem = item->child(i);
    pitem->setFlags(pitem->flags() | Qt::ItemIsSelectable | Qt::ItemIsEditable);
    pitem->setExpanded(true);
    set_child_items(pitem);
  }
}

void EditDialog::publish_data() {
  if (!window_->ui->treeWidget_url->currentItem()) {
    return;
  }

  ++progress_index_;

  ui->progressBar->setValue(progress_index_);

  vlink::ProxyAPI::Data proxy_data;
  proxy_data.timestamp = 0;
  proxy_data.url = window_->ui->treeWidget_url->currentItem()->text(1).toStdString();
  proxy_data.ser = window_->ui->treeWidget_url->currentItem()->data(1, Qt::UserRole).toString().toStdString();
  {
    const auto schema_iter = window_->schema_type_map_.find(proxy_data.url);
    proxy_data.schema =
        schema_iter != window_->schema_type_map_.end() ? schema_iter->second : vlink::SchemaType::kUnknown;
  }
  proxy_data.raw = raw_;

  if (!window_->proxy_->send_data(proxy_data)) {
    on_pushButton_stop_clicked();
    QMessageBox::warning(this, tr("Warning"), tr("Failed to send edited data. Check schema metadata and proxy route."));
    return;
  }

  if (!ui->groupBox_config->isChecked()) {
    on_pushButton_stop_clicked();
    return;
  }

  if (ui->progressBar->maximum() > 0) {
    if (ui->progressBar->value() >= ui->progressBar->maximum()) {
      on_pushButton_stop_clicked();
    }
  }
}

void EditDialog::update_status() {
  QTreeWidgetItem* item = ui->treeWidget_property->currentItem();

  if (!item) {
    ui->pushButton_add->setEnabled(false);
    ui->pushButton_del->setEnabled(false);
    return;
  }

  int type = item->data(0, Qt::UserRole).toInt();

  if (type >= 100) {
    ui->pushButton_add->setEnabled(true);
    if (item->childCount() > 0) {
      ui->pushButton_del->setEnabled(true);
    } else {
      ui->pushButton_del->setEnabled(false);
    }
  } else {
    ui->pushButton_add->setEnabled(false);
    ui->pushButton_del->setEnabled(false);
  }
}

void EditDialog::init_fbs_tree() {
  const auto* root_object = fbs_context_->root_object;

  if (!root_object) {
    return;
  }

  FlatbuffersObjectView root_view;
  bool has_initial_data = false;
  auto iter = window_->last_data_map_.find(window_->current_url_);

  if (iter != window_->last_data_map_.end() && !iter->second.empty()) {
    has_initial_data = make_root_view(*fbs_context_, iter->second, root_view);
  }

  init_fbs_tree_object(nullptr, "", root_object, has_initial_data ? &root_view : nullptr);
}

void EditDialog::init_fbs_tree_object(QTreeWidgetItem* parent_item, const std::string& parent_id,
                                      const reflection::Object* object, const FlatbuffersObjectView* view) {
  if (!object || !object->fields()) {
    return;
  }

  std::vector<const reflection::Field*> ordered_fields;
  ordered_fields.reserve(object->fields()->size());
  for (unsigned i = 0; i < object->fields()->size(); ++i) {
    const auto* field = object->fields()->Get(i);

    if (field) {
      ordered_fields.push_back(field);
    }
  }
  std::sort(ordered_fields.begin(), ordered_fields.end(),
            [](const reflection::Field* a, const reflection::Field* b) { return a->id() < b->id(); });

  for (const auto* field : ordered_fields) {
    if (!field || !field->name()) {
      continue;
    }

    const std::string field_name = field->name()->str();
    const std::string current_id = parent_id + "." + std::to_string(static_cast<int>(field->id()));
    const auto base_type = field->type()->base_type();

    bool is_enum = is_enum_field(*field, *fbs_context_->schema);
    int edit_type = is_enum ? 8 : get_fbs_edit_type(base_type);

    if (base_type == reflection::Vector || base_type == reflection::Vector64) {
      auto* array_item = new QTreeWidgetItem;
      if (parent_item) {
        parent_item->addChild(array_item);
      } else {
        ui->treeWidget_property->addTopLevelItem(array_item);
      }

      const auto element_type = field->type()->element();
      bool is_elem_enum = is_enum_field(*field, *fbs_context_->schema);
      int elem_edit_type = is_elem_enum ? 8 : get_fbs_edit_type(element_type);

      array_item->setText(0, QString::number(static_cast<int>(field->id())));
      array_item->setText(1, QString::fromStdString(get_field_type_name(*field, *fbs_context_->schema)));
      array_item->setText(2, QString::fromStdString(field_name));
      array_item->setText(3, "");
      array_item->setData(0, Qt::UserRole, 100 + elem_edit_type);
      array_item->setFlags(array_item->flags() | Qt::ItemIsSelectable);
      array_item->setExpanded(true);

      item_to_fbs_field_map_[array_item] = field;

      size_t vec_size = 0;
      if (view) {
        vec_size = get_vector_size(*view, *field);
      }

      for (size_t j = 0; j < vec_size; ++j) {
        add_fbs_vector_element(array_item, field, view, static_cast<int>(j));
      }

      continue;
    }

    if (base_type == reflection::Obj && fbs_context_->schema->objects()) {
      const auto* child_object = fbs_context_->schema->objects()->Get(static_cast<uint32_t>(field->type()->index()));
      if (!child_object) {
        continue;
      }

      auto* item = new QTreeWidgetItem;
      if (parent_item) {
        parent_item->addChild(item);
      } else {
        ui->treeWidget_property->addTopLevelItem(item);
      }

      item->setText(0, QString::number(static_cast<int>(field->id())));
      item->setText(1, QString::fromStdString(get_field_type_name(*field, *fbs_context_->schema)));
      item->setText(2, QString::fromStdString(field_name));
      item->setText(3, "");
      item->setData(0, Qt::UserRole, 0);
      item->setFlags(item->flags() | Qt::ItemIsSelectable);
      item->setExpanded(true);

      item_to_fbs_field_map_[item] = field;
      item_to_fbs_object_map_[item] = child_object;

      FlatbuffersObjectView child_view;
      bool has_child = false;
      if (view) {
        has_child = get_child_view(*view, *field, *fbs_context_->schema, child_view);
      }

      init_fbs_tree_object(item, current_id, child_object, has_child ? &child_view : nullptr);
      continue;
    }

    auto* item = new QTreeWidgetItem;

    if (parent_item) {
      parent_item->addChild(item);
    } else {
      ui->treeWidget_property->addTopLevelItem(item);
    }

    item->setText(0, QString::number(static_cast<int>(field->id())));
    item->setText(1, QString::fromStdString(get_field_type_name(*field, *fbs_context_->schema)));
    item->setText(2, QString::fromStdString(field_name));
    item->setData(0, Qt::UserRole, edit_type);
    item->setData(0, kEditValueKindRole,
                  base_type == reflection::String ? static_cast<int>(EditValueKind::kString)
                                                  : static_cast<int>(EditValueKind::kUnknown));
    item->setFlags(item->flags() | Qt::ItemIsSelectable | Qt::ItemIsEditable);

    item_to_fbs_field_map_[item] = field;

    if (view) {
      if (base_type == reflection::String) {
        item->setText(3, QString::fromStdString(get_string(*view, *field)));
      } else if (base_type == reflection::Bool) {
        auto val = get_numeric(*view, *field);
        item->setText(3, (val.has_value() && val.value() != 0) ? "true" : "false");
      } else {
        auto val = get_numeric(*view, *field);
        if (val.has_value()) {
          if (base_type == reflection::Float) {
            item->setText(3, QString::number(val.value(), 'g', 8));
          } else if (base_type == reflection::Double) {
            item->setText(3, QString::number(val.value(), 'g', 16));
          } else {
            item->setText(3, QString::number(static_cast<qlonglong>(val.value())));
          }
        } else {
          item->setText(3, fbs_default_for_type(base_type));
        }
      }
    } else {
      item->setText(3, fbs_default_for_type(base_type));
    }
  }
}

void EditDialog::add_fbs_vector_element(QTreeWidgetItem* array_item, const reflection::Field* field,
                                        const FlatbuffersObjectView* view, int index) {
  const auto element_type = field->type()->element();
  bool is_elem_enum = is_enum_field(*field, *fbs_context_->schema);
  int elem_edit_type = is_elem_enum ? 8 : get_fbs_edit_type(element_type);

  auto* item = new QTreeWidgetItem;
  array_item->addChild(item);

  item->setText(0, "");
  item->setText(1, QString("[%1]").arg(index));
  item->setText(2, "");

  if (element_type == reflection::Obj && fbs_context_->schema->objects()) {
    const auto* child_object = fbs_context_->schema->objects()->Get(static_cast<uint32_t>(field->type()->index()));

    item->setData(0, Qt::UserRole, 0);
    item->setFlags(item->flags() | Qt::ItemIsSelectable);
    item->setExpanded(true);
    item_to_fbs_object_map_[item] = child_object;

    FlatbuffersObjectView child_view;
    bool has_child = false;
    if (view) {
      has_child = get_vector_elem_view(*view, *field, static_cast<size_t>(index), *fbs_context_->schema, child_view);
    }

    init_fbs_tree_object(item, "", child_object, has_child ? &child_view : nullptr);
  } else {
    item->setData(0, Qt::UserRole, elem_edit_type);
    item->setData(0, kEditValueKindRole,
                  element_type == reflection::String ? static_cast<int>(EditValueKind::kString)
                                                     : static_cast<int>(EditValueKind::kUnknown));
    item->setFlags(item->flags() | Qt::ItemIsSelectable | Qt::ItemIsEditable);

    if (view) {
      if (element_type == reflection::String) {
        item->setText(3, QString::fromStdString(get_vector_string(*view, *field, static_cast<size_t>(index))));
      } else if (element_type == reflection::Bool) {
        auto val = get_vector_numeric(*view, *field, static_cast<size_t>(index));
        item->setText(3, (val.has_value() && val.value() != 0) ? "true" : "false");
      } else {
        auto val = get_vector_numeric(*view, *field, static_cast<size_t>(index));
        if (val.has_value()) {
          if (element_type == reflection::Float) {
            item->setText(3, QString::number(val.value(), 'g', 8));
          } else if (element_type == reflection::Double) {
            item->setText(3, QString::number(val.value(), 'g', 16));
          } else {
            item->setText(3, QString::number(static_cast<qlonglong>(val.value())));
          }
        } else {
          item->setText(3, fbs_default_for_type(element_type));
        }
      }
    } else {
      item->setText(3, fbs_default_for_type(element_type));
    }
  }
}

std::string EditDialog::build_fbs_json() {
  std::string json = "{";
  bool first = true;

  for (int i = 0; i < ui->treeWidget_property->topLevelItemCount(); ++i) {
    auto* item = ui->treeWidget_property->topLevelItem(i);
    const auto* field = item_to_fbs_field_map_[item];

    if (!field || !field->name()) {
      continue;
    }

    if (!first) {
      json += ",";
    }

    first = false;
    json += "\"" + field->name()->str() + "\":";
    json += build_fbs_json_value(item);
  }

  json += "}";
  return json;
}

std::string EditDialog::build_fbs_json_object(QTreeWidgetItem* item) {
  std::string json = "{";
  bool first = true;

  for (int i = 0; i < item->childCount(); ++i) {
    auto* child = item->child(i);
    const auto* field = item_to_fbs_field_map_[child];

    if (!field || !field->name()) {
      continue;
    }

    if (!first) {
      json += ",";
    }

    first = false;
    json += "\"" + field->name()->str() + "\":";
    json += build_fbs_json_value(child);
  }

  json += "}";
  return json;
}

std::string EditDialog::build_fbs_json_value(QTreeWidgetItem* item) {
  int type = item->data(0, Qt::UserRole).toInt();

  if (type >= 100) {
    std::string arr = "[";
    for (int i = 0; i < item->childCount(); ++i) {
      if (i > 0) {
        arr += ",";
      }
      arr += build_fbs_json_value(item->child(i));
    }
    arr += "]";
    return arr;
  }

  if (type == 0 && item->childCount() > 0) {
    return build_fbs_json_object(item);
  }

  QString text = item->text(3);

  switch (type) {
    case 7:
      return (text == "true") ? "true" : "false";
    case 9:
      return "\"" + escape_json_string(text.toStdString()) + "\"";
    default:
      return text.isEmpty() ? "0" : text.toStdString();
  }
}

// NOLINTEND
