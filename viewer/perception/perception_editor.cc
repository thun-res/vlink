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

#include "./perception_editor.h"

#include <QComboBox>
#include <QCompleter>
#include <QFileDialog>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QIcon>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPushButton>
#include <QSplitter>
#include <QTableWidget>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <nlohmann/json.hpp>
#include <string>

#include "../flatbuffers_runtime_compat.h"
#include "../mainwindow.h"

#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wcpp"
#endif
#include <google/protobuf/descriptor.h>
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic pop
#endif

static constexpr int kFieldTreeMaxDepth = 5;
static constexpr int kPathRole = Qt::UserRole + 1;
static constexpr int kHudComboData = -1;

static QString normalized_serializer(QString value) {
  value = value.toLower();
  value.remove('_');
  value.remove(' ');
  value.remove('.');
  value.remove('-');
  value.remove(':');
  return value;
}

static const std::vector<perception::RenderType>& editor_render_types() {
  static const std::vector<perception::RenderType> kTypes{
      perception::RenderType::kPointCloud,    perception::RenderType::kObjectDetection,
      perception::RenderType::kLaneLine,      perception::RenderType::kPrediction,
      perception::RenderType::kTrafficLight,  perception::RenderType::kStopLine,
      perception::RenderType::kTrafficSign,   perception::RenderType::kFreespace,
      perception::RenderType::kOccupancyGrid, perception::RenderType::kParkingSlot,
      perception::RenderType::kEgoTrajectory, perception::RenderType::kHdMap,
      perception::RenderType::kCameraFrustum, perception::RenderType::kCovarianceEllipse,
  };
  return kTypes;
}

static QString proto_type_label(const google::protobuf::FieldDescriptor* field) {
  QString suffix = field->is_repeated() ? "[]" : "";
  return QString::fromStdString(std::string(field->cpp_type_name())) + suffix;
}

static const google::protobuf::Descriptor* resolve_collection_element(const google::protobuf::Descriptor* descriptor,
                                                                      const QString& path) {
  if (!descriptor || path.trimmed().isEmpty()) {
    return descriptor;
  }

  const auto segments = path.trimmed().split('.');

  for (const auto& raw_segment : segments) {
    QString segment = raw_segment;
    const int bracket = segment.indexOf('[');

    if (bracket >= 0) {
      segment = segment.left(bracket);
    }

    if (segment.isEmpty()) {
      continue;
    }

    const auto* field = descriptor->FindFieldByName(segment.toStdString());

    if (!field || field->cpp_type() != google::protobuf::FieldDescriptor::CPPTYPE_MESSAGE) {
      return nullptr;
    }

    descriptor = field->message_type();
  }

  return descriptor;
}

PerceptionEditorDialog::PerceptionEditorDialog(const PerceptionConfig& config, QString config_path, MainWindow* window,
                                               std::vector<SourceMessage> sources, QWidget* parent)
    : QDialog(parent),
      config_(config),
      config_path_(std::move(config_path)),
      window_(window),
      sources_(std::move(sources)) {
  setWindowTitle("Perception Config Editor");
  resize(1080, 680);

  mapping_list_ = new QListWidget(this);

  auto* add_button = new QPushButton(QIcon(":/resource/add.png"), "Add", this);
  auto* remove_button = new QPushButton(QIcon(":/resource/sub.png"), "Remove", this);

  add_button->setToolTip("Add a new rule (set its render type to Vehicle HUD for a HUD binding)");

  auto* list_buttons = new QHBoxLayout;
  list_buttons->addWidget(add_button);
  list_buttons->addWidget(remove_button);

  auto* left_layout = new QVBoxLayout;
  left_layout->addWidget(new QLabel("Rules (mappings + HUD)", this));
  left_layout->addWidget(mapping_list_);
  left_layout->addLayout(list_buttons);

  auto* left_widget = new QWidget(this);
  left_widget->setLayout(left_layout);

  name_edit_ = new QLineEdit(this);
  ser_edit_ = new QLineEdit(this);

  if (window_) {
    for (const auto& type : window_->scanned_message_types()) {
      scanned_types_ << type;
    }

    auto* completer = new QCompleter(scanned_types_, this);
    completer->setCaseSensitivity(Qt::CaseInsensitive);
    completer->setFilterMode(Qt::MatchContains);
    completer->setCompletionMode(QCompleter::PopupCompletion);
    ser_edit_->setCompleter(completer);
  }

  type_combo_ = new QComboBox(this);
  encoding_combo_ = new QComboBox(this);
  collection_edit_ = new QLineEdit(this);
  inner_collection_edit_ = new QLineEdit(this);

  for (const auto type : editor_render_types()) {
    type_combo_->addItem(PerceptionConfig::render_type_to_string(type), static_cast<int>(type));
  }

  type_combo_->addItem("vehicle_hud", kHudComboData);

  encoding_combo_->addItem("any", static_cast<int>(perception::Encoding::kUnknown));
  encoding_combo_->addItem("protobuf", static_cast<int>(perception::Encoding::kProtobuf));
  encoding_combo_->addItem("flatbuffers", static_cast<int>(perception::Encoding::kFlatbuffers));
  encoding_combo_->addItem("zero_copy", static_cast<int>(perception::Encoding::kZeroCopy));

  auto* form = new QFormLayout;
  form->addRow("Name", name_edit_);
  form->addRow("Serializer (ser)", ser_edit_);
  form->addRow("Render type", type_combo_);
  form->addRow("Encoding", encoding_combo_);
  form->addRow("Collection", collection_edit_);
  form->addRow("Inner collection", inner_collection_edit_);

  target_table_ = new QTableWidget(this);
  target_table_->setColumnCount(4);
  target_table_->setHorizontalHeaderLabels({"Target", "Source field", "Expression", "Default"});
  target_table_->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
  target_table_->verticalHeader()->setVisible(false);

  auto* middle_layout = new QVBoxLayout;
  middle_layout->addLayout(form);
  middle_layout->addWidget(
      new QLabel("Field mappings (double-click a field on the right to fill the selected row)", this));
  middle_layout->addWidget(target_table_);

  auto* middle_widget = new QWidget(this);
  middle_widget->setLayout(middle_layout);

  field_tree_ = new QTreeWidget(this);
  field_tree_->setHeaderLabels({"Field", "Type"});
  field_tree_->setColumnWidth(0, 200);

  auto* right_layout = new QVBoxLayout;
  right_layout->addWidget(new QLabel("Source fields", this));
  right_layout->addWidget(field_tree_);

  auto* right_widget = new QWidget(this);
  right_widget->setLayout(right_layout);

  auto* splitter = new QSplitter(Qt::Horizontal, this);
  splitter->addWidget(left_widget);
  splitter->addWidget(middle_widget);
  splitter->addWidget(right_widget);
  splitter->setStretchFactor(0, 1);
  splitter->setStretchFactor(1, 3);
  splitter->setStretchFactor(2, 2);

  path_label_ = new QLabel(this);
  path_label_->setStyleSheet("color: #888888;");
  path_label_->setTextInteractionFlags(Qt::TextSelectableByMouse);

  auto* import_button = new QPushButton(QIcon(":/resource/download.png"), "Import...", this);
  auto* save_button = new QPushButton(QIcon(":/resource/config.png"), "Save", this);
  auto* export_button = new QPushButton(QIcon(":/resource/export.png"), "Export As...", this);
  auto* ok_button = new QPushButton(QIcon(":/resource/ok.png"), "OK", this);
  auto* cancel_button = new QPushButton(QIcon(":/resource/cancel.png"), "Cancel", this);

  ok_button->setDefault(true);

  auto* bottom_layout = new QHBoxLayout;
  bottom_layout->addWidget(import_button);
  bottom_layout->addWidget(save_button);
  bottom_layout->addWidget(export_button);
  bottom_layout->addWidget(path_label_, 1);
  bottom_layout->addWidget(ok_button);
  bottom_layout->addWidget(cancel_button);

  auto* main_layout = new QVBoxLayout(this);
  main_layout->addWidget(splitter);
  main_layout->addLayout(bottom_layout);

  connect(mapping_list_, &QListWidget::currentRowChanged, this, &PerceptionEditorDialog::on_mapping_selection_changed);
  connect(add_button, &QPushButton::clicked, this, &PerceptionEditorDialog::on_add_mapping);
  connect(remove_button, &QPushButton::clicked, this, &PerceptionEditorDialog::on_remove_mapping);
  connect(type_combo_, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
          &PerceptionEditorDialog::on_type_changed);
  connect(import_button, &QPushButton::clicked, this, &PerceptionEditorDialog::on_import_clicked);
  connect(save_button, &QPushButton::clicked, this, &PerceptionEditorDialog::on_save_clicked);
  connect(export_button, &QPushButton::clicked, this, &PerceptionEditorDialog::on_export_clicked);
  connect(field_tree_, &QTreeWidget::itemDoubleClicked, this, &PerceptionEditorDialog::on_field_double_clicked);
  connect(ok_button, &QPushButton::clicked, this, &QDialog::accept);
  connect(cancel_button, &QPushButton::clicked, this, &QDialog::reject);

  const auto refresh_tree = [this]() {
    auto schema = vlink::SchemaType::kUnknown;

    switch (static_cast<perception::Encoding>(encoding_combo_->currentData().toInt())) {
      case perception::Encoding::kProtobuf:
        schema = vlink::SchemaType::kProtobuf;
        break;
      case perception::Encoding::kFlatbuffers:
        schema = vlink::SchemaType::kFlatbuffers;
        break;
      case perception::Encoding::kZeroCopy:
        schema = vlink::SchemaType::kZeroCopy;
        break;
      default:
        break;
    }

    rebuild_field_tree(ser_edit_->text(), schema);
  };
  connect(ser_edit_, &QLineEdit::editingFinished, this, refresh_tree);
  connect(collection_edit_, &QLineEdit::editingFinished, this, refresh_tree);
  connect(inner_collection_edit_, &QLineEdit::editingFinished, this, refresh_tree);
  connect(encoding_combo_, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
          [refresh_tree](int) { refresh_tree(); });

  load_entries_from_config();
  update_path_label();
  rebuild_mapping_list();

  if (mapping_list_->count() > 0) {
    mapping_list_->setCurrentRow(0);
  }
}

PerceptionEditorDialog::~PerceptionEditorDialog() = default;

bool PerceptionEditorDialog::current_is_hud() const { return type_combo_->currentData().toInt() == kHudComboData; }

void PerceptionEditorDialog::load_entries_from_config() {
  entries_.clear();
  entries_.reserve(config_.mappings().size() + config_.hud_bindings().size());

  for (const auto& rule : config_.mappings()) {
    entries_.push_back({rule, false});
  }

  for (const auto& rule : config_.hud_bindings()) {
    entries_.push_back({rule, true});
  }
}

void PerceptionEditorDialog::apply_entries_to_config() {
  config_.mappings().clear();
  config_.hud_bindings().clear();

  for (auto& entry : entries_) {
    if (entry.rule.field_mappings.empty()) {
      continue;
    }

    PerceptionConfig::finalize_mapping(entry.rule);

    if (entry.is_hud) {
      config_.hud_bindings().push_back(entry.rule);
    } else {
      config_.mappings().push_back(entry.rule);
    }
  }
}

bool PerceptionEditorDialog::save_config_to(const QString& path) {
  commit_form_to_mapping(current_index_);
  apply_entries_to_config();

  QString error;

  if (!config_.save_to_file(path, &error)) {
    QMessageBox::warning(this, "Save failed", error);
    return false;
  }

  config_path_ = path;
  config_.set_source_path(path);
  update_path_label();
  return true;
}

void PerceptionEditorDialog::update_path_label() {
  path_label_->setText(config_path_.isEmpty() ? QStringLiteral("(unsaved — use Save / Export As)") : config_path_);
}

void PerceptionEditorDialog::rebuild_mapping_list() {
  mapping_list_->blockSignals(true);
  mapping_list_->clear();

  for (const auto& entry : entries_) {
    const QString kind =
        entry.is_hud ? QStringLiteral("HUD") : PerceptionConfig::render_type_to_string(entry.rule.type);
    const QString name = entry.rule.name.isEmpty() ? entry.rule.ser : entry.rule.name;
    const QString label = (name.trimmed().isEmpty() ? QStringLiteral("(unnamed)") : name) + "  [" + kind + "]";
    mapping_list_->addItem(label);
  }

  mapping_list_->blockSignals(false);
}

void PerceptionEditorDialog::on_mapping_selection_changed() {
  commit_form_to_mapping(current_index_);

  current_index_ = mapping_list_->currentRow();
  load_mapping_to_form(current_index_);
}

void PerceptionEditorDialog::load_mapping_to_form(int index) {
  if (index < 0 || index >= static_cast<int>(entries_.size())) {
    return;
  }

  const Entry& entry = entries_[index];
  const auto& rule = entry.rule;
  const bool is_hud = entry.is_hud;

  name_edit_->setText(rule.name);
  ser_edit_->setText(rule.ser);

  type_combo_->blockSignals(true);
  type_combo_->setCurrentIndex(type_combo_->findData(is_hud ? kHudComboData : static_cast<int>(rule.type)));
  type_combo_->blockSignals(false);

  collection_edit_->setEnabled(!is_hud);
  inner_collection_edit_->setEnabled(!is_hud);

  encoding_combo_->blockSignals(true);
  encoding_combo_->setCurrentIndex(encoding_combo_->findData(static_cast<int>(rule.encoding)));
  encoding_combo_->blockSignals(false);
  collection_edit_->setText(rule.collection);
  inner_collection_edit_->setText(rule.inner_collection);

  rebuild_target_table(is_hud ? PerceptionConfig::hud_target_slots() : PerceptionConfig::target_slots_for(rule.type),
                       &rule);

  vlink::SchemaType schema = vlink::SchemaType::kUnknown;

  switch (rule.encoding) {
    case perception::Encoding::kProtobuf:
      schema = vlink::SchemaType::kProtobuf;
      break;
    case perception::Encoding::kFlatbuffers:
      schema = vlink::SchemaType::kFlatbuffers;
      break;
    case perception::Encoding::kZeroCopy:
      schema = vlink::SchemaType::kZeroCopy;
      break;
    default:
      break;
  }

  rebuild_field_tree(rule.ser, schema);
}

void PerceptionEditorDialog::commit_form_to_mapping(int index) {
  if (index < 0 || index >= static_cast<int>(entries_.size())) {
    return;
  }

  Entry& entry = entries_[index];
  auto& rule = entry.rule;
  const bool is_hud = current_is_hud();
  entry.is_hud = is_hud;

  const auto new_ser = ser_edit_->text();
  const auto old_ser_normalized = normalized_serializer(rule.ser);
  const auto new_ser_normalized = normalized_serializer(new_ser);

  if (!old_ser_normalized.isEmpty() && rule.match.serializer_equals.size() == 1 &&
      rule.match.serializer_equals.front() == old_ser_normalized) {
    rule.match.serializer_equals = new_ser_normalized.isEmpty() ? QStringList{} : QStringList{new_ser_normalized};
  }

  const auto new_encoding = static_cast<perception::Encoding>(encoding_combo_->currentData().toInt());

  if (rule.encoding != perception::Encoding::kUnknown && rule.match.schema_types.size() == 1 &&
      rule.match.schema_types.front() == normalized_serializer(PerceptionConfig::encoding_to_string(rule.encoding))) {
    rule.match.schema_types =
        new_encoding == perception::Encoding::kUnknown
            ? QStringList{}
            : QStringList{normalized_serializer(PerceptionConfig::encoding_to_string(new_encoding))};
  }

  rule.name = name_edit_->text();
  rule.ser = new_ser;
  rule.encoding = new_encoding;

  if (is_hud) {
    rule.collection.clear();
    rule.inner_collection.clear();
  } else {
    rule.type = static_cast<perception::RenderType>(type_combo_->currentData().toInt());
    rule.collection = collection_edit_->text().trimmed();
    rule.inner_collection = inner_collection_edit_->text().trimmed();
  }

  rule.field_mappings.clear();

  for (int row = 0; row < target_table_->rowCount(); ++row) {
    const auto* target_item = target_table_->item(row, 0);

    if (!target_item) {
      continue;
    }

    const auto source = target_table_->item(row, 1) ? target_table_->item(row, 1)->text().trimmed() : QString();
    const auto expression = target_table_->item(row, 2) ? target_table_->item(row, 2)->text().trimmed() : QString();
    const auto default_value = target_table_->item(row, 3) ? target_table_->item(row, 3)->text().trimmed() : QString();

    if (source.isEmpty() && expression.isEmpty() && default_value.isEmpty()) {
      continue;
    }

    perception::FieldMapping mapping;
    mapping.target = target_item->text().toStdString();
    mapping.source = source.toStdString();
    mapping.expression = expression.toStdString();

    if (!default_value.isEmpty()) {
      mapping.default_value = default_value.toStdString();
      mapping.has_default_value = true;

      const auto parsed = nlohmann::json::parse(mapping.default_value, nullptr, false);
      mapping.default_value_is_string = parsed.is_discarded() || parsed.is_string();
    }

    rule.field_mappings.emplace_back(std::move(mapping));
  }

  PerceptionConfig::finalize_mapping(rule);
}

void PerceptionEditorDialog::rebuild_target_table(const QStringList& slot_names,
                                                  const PerceptionConfig::MappingRule* rule) {
  QStringList rows = slot_names;

  if (rule) {
    for (const auto& mapping : rule->field_mappings) {
      const auto target = QString::fromStdString(mapping.target);

      if (!target.isEmpty() && !rows.contains(target)) {
        rows << target;
      }
    }
  }

  target_table_->setRowCount(rows.size());

  for (int row = 0; row < rows.size(); ++row) {
    const QString& slot = rows.at(row);

    QString source;
    QString expression;
    QString default_value;

    if (rule) {
      for (const auto& mapping : rule->field_mappings) {
        if (QString::fromStdString(mapping.target) == slot) {
          source = QString::fromStdString(mapping.source);
          expression = QString::fromStdString(mapping.expression);
          default_value = QString::fromStdString(mapping.default_value);
          break;
        }
      }
    }

    auto* target_item = new QTableWidgetItem(slot);
    target_item->setFlags(target_item->flags() & ~Qt::ItemIsEditable);
    target_table_->setItem(row, 0, target_item);
    target_table_->setItem(row, 1, new QTableWidgetItem(source));
    target_table_->setItem(row, 2, new QTableWidgetItem(expression));
    target_table_->setItem(row, 3, new QTableWidgetItem(default_value));
  }
}

void PerceptionEditorDialog::rebuild_field_tree(const QString& ser, vlink::SchemaType schema) {
  field_tree_->clear();

  if (ser.isEmpty()) {
    return;
  }

  const QString want = normalized_serializer(ser);

  const google::protobuf::Descriptor* descriptor = nullptr;

  const bool allow_proto = schema == vlink::SchemaType::kUnknown || schema == vlink::SchemaType::kProtobuf;
  const bool allow_fbs = schema == vlink::SchemaType::kUnknown || schema == vlink::SchemaType::kFlatbuffers;

  if (allow_proto && window_ && window_->des_pool_) {
    descriptor = window_->des_pool_->FindMessageTypeByName(ser.toStdString());

    if (!descriptor) {
      for (const auto& candidate : scanned_types_) {
        const QString have = normalized_serializer(candidate);

        if (!have.isEmpty() && (have.contains(want) || want.contains(have))) {
          descriptor = window_->des_pool_->FindMessageTypeByName(candidate.toStdString());

          if (descriptor) {
            break;
          }
        }
      }
    }
  }

  std::shared_ptr<FlatbuffersSchemaContext> fbs_context;

  if (!descriptor) {
    for (const auto& source : sources_) {
      if (schema != vlink::SchemaType::kUnknown && source.schema != schema) {
        continue;
      }

      const QString have = normalized_serializer(source.ser);

      if (have.isEmpty() || !(have.contains(want) || want.contains(have))) {
        continue;
      }

      descriptor = allow_proto ? source.proto_desc : nullptr;
      fbs_context = allow_fbs ? source.fbs_ctx : nullptr;

      if (descriptor || fbs_context) {
        break;
      }
    }
  }

  if (allow_fbs && !descriptor && !fbs_context && window_) {
    fbs_context = window_->flatbuffers_runtime_.find_context(ser.toStdString());
  }

  if (descriptor) {
    const auto* element = resolve_collection_element(descriptor, collection_edit_->text());

    if (!element) {
      element = descriptor;
    }

    if (!inner_collection_edit_->text().trimmed().isEmpty()) {
      const auto* inner = resolve_collection_element(element, inner_collection_edit_->text());

      if (inner) {
        element = inner;
      }
    }

    auto* root = new QTreeWidgetItem(field_tree_, {QString::fromStdString(std::string(element->name())), "message"});
    add_proto_fields(root, element, {}, 0);
    root->setExpanded(true);
    return;
  }

  if (fbs_context && fbs_context->valid() && fbs_context->schema && fbs_context->root_object) {
    auto* root = new QTreeWidgetItem(field_tree_, {ser, "table"});
    add_fbs_fields(root, fbs_context->root_object, fbs_context->schema, {}, 0);
    root->setExpanded(true);
  }
}

void PerceptionEditorDialog::add_proto_fields(QTreeWidgetItem* parent, const google::protobuf::Descriptor* descriptor,
                                              const QString& prefix, int depth) {
  if (!descriptor || depth > kFieldTreeMaxDepth) {
    return;
  }

  for (int i = 0; i < descriptor->field_count(); ++i) {
    const auto* field = descriptor->field(i);
    const QString name = QString::fromStdString(std::string(field->name()));
    const QString path = prefix.isEmpty() ? name : prefix + "." + name;

    auto* item = new QTreeWidgetItem(parent, {name, proto_type_label(field)});
    item->setData(0, kPathRole, path);

    if (field->cpp_type() == google::protobuf::FieldDescriptor::CPPTYPE_MESSAGE) {
      add_proto_fields(item, field->message_type(), path, depth + 1);
    }
  }
}

void PerceptionEditorDialog::add_fbs_fields(QTreeWidgetItem* parent, const reflection::Object* object,
                                            const reflection::Schema* schema, const QString& prefix, int depth) {
  if (!object || !schema || !object->fields() || depth > kFieldTreeMaxDepth) {
    return;
  }

  for (const auto* field : *object->fields()) {
    if (!field || !field->name()) {
      continue;
    }

    const QString name = QString::fromStdString(field->name()->str());
    const QString path = prefix.isEmpty() ? name : prefix + "." + name;
    const auto base_type = field->type() ? field->type()->base_type() : reflection::None;
    const bool is_vector = base_type == reflection::Vector;
    const QString type_label = is_vector ? "vector" : QString::number(static_cast<int>(base_type));

    auto* item = new QTreeWidgetItem(parent, {name, type_label});
    item->setData(0, kPathRole, path);

    const bool is_obj = base_type == reflection::Obj || (is_vector && field->type()->element() == reflection::Obj);

    if (is_obj && schema->objects() && field->type()) {
      const auto* sub_obj = schema->objects()->Get(static_cast<uint32_t>(field->type()->index()));
      add_fbs_fields(item, sub_obj, schema, path, depth + 1);
    }
  }
}

void PerceptionEditorDialog::on_field_double_clicked(QTreeWidgetItem* item, int column) {
  (void)column;

  if (!item) {
    return;
  }

  const auto path = item->data(0, kPathRole).toString();

  if (path.isEmpty()) {
    return;
  }

  const int row = target_table_->currentRow();

  if (row < 0) {
    return;
  }

  if (!target_table_->item(row, 1)) {
    target_table_->setItem(row, 1, new QTableWidgetItem);
  }

  target_table_->item(row, 1)->setText(path);
}

void PerceptionEditorDialog::on_type_changed(int index) {
  (void)index;

  commit_form_to_mapping(current_index_);

  const bool is_hud = current_is_hud();

  collection_edit_->setEnabled(!is_hud);
  inner_collection_edit_->setEnabled(!is_hud);

  const PerceptionConfig::MappingRule* rule =
      (current_index_ >= 0 && current_index_ < static_cast<int>(entries_.size())) ? &entries_[current_index_].rule
                                                                                  : nullptr;

  if (is_hud) {
    rebuild_target_table(PerceptionConfig::hud_target_slots(), rule);
  } else {
    const auto type = static_cast<perception::RenderType>(type_combo_->currentData().toInt());
    rebuild_target_table(PerceptionConfig::target_slots_for(type), rule);
  }
}

void PerceptionEditorDialog::on_add_mapping() {
  commit_form_to_mapping(current_index_);

  Entry entry;
  entry.rule.name = "new_rule";
  entry.rule.type = perception::RenderType::kObjectDetection;
  entry.rule.priority = 100;
  entry.is_hud = false;
  entries_.push_back(std::move(entry));

  current_index_ = -1;
  rebuild_mapping_list();
  mapping_list_->setCurrentRow(static_cast<int>(entries_.size()) - 1);
}

void PerceptionEditorDialog::on_remove_mapping() {
  const int row = mapping_list_->currentRow();

  if (row < 0 || row >= static_cast<int>(entries_.size())) {
    return;
  }

  entries_.erase(entries_.begin() + row);
  current_index_ = -1;

  rebuild_mapping_list();

  if (!entries_.empty()) {
    mapping_list_->setCurrentRow(std::min(row, static_cast<int>(entries_.size()) - 1));
  } else {
    name_edit_->clear();
    ser_edit_->clear();
    collection_edit_->clear();
    inner_collection_edit_->clear();
    rebuild_target_table({}, nullptr);
    field_tree_->clear();
  }
}

void PerceptionEditorDialog::on_import_clicked() {
  const auto path = QFileDialog::getOpenFileName(this, "Import perception config", config_path_, "JSON (*.json)");

  if (path.isEmpty()) {
    return;
  }

  PerceptionConfig loaded;
  QString error;

  if (!loaded.load_from_file(path, &error)) {
    QMessageBox::warning(this, "Import failed", error);
    return;
  }

  config_ = std::move(loaded);
  config_path_ = path;
  load_entries_from_config();
  current_index_ = -1;
  update_path_label();
  rebuild_mapping_list();

  if (mapping_list_->count() > 0) {
    mapping_list_->setCurrentRow(0);
  }
}

void PerceptionEditorDialog::on_save_clicked() {
  if (config_path_.isEmpty()) {
    on_export_clicked();
    return;
  }

  save_config_to(config_path_);
}

void PerceptionEditorDialog::on_export_clicked() {
  const auto path = QFileDialog::getSaveFileName(
      this, "Export perception config",
      config_path_.isEmpty() ? QStringLiteral("perception_config.json") : config_path_, "JSON (*.json)");

  if (path.isEmpty()) {
    return;
  }

  save_config_to(path);
}

void PerceptionEditorDialog::accept() {
  commit_form_to_mapping(current_index_);
  apply_entries_to_config();
  QDialog::accept();
}

// NOLINTEND
