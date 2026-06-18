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

#include "./pointdialog.h"

#include <vlink/base/helpers.h>

#include <QCloseEvent>
#include <QDir>
#include <QFileInfo>
#include <QHideEvent>
#include <QKeySequence>
#include <QLineEdit>
#include <QMenu>
#include <QRegularExpression>
#include <QResizeEvent>
#include <QShortcut>
#include <QShowEvent>

#include "./playerwindow.h"
#include "./ui_pointdialog.h"

PointDialog::PointDialog(QWidget* parent) : QDialog(parent), ui(new Ui::PointDialog) {
  setWindowFlags(Qt::Dialog | Qt::WindowMaximizeButtonHint | Qt::WindowCloseButtonHint | Qt::WindowStaysOnTopHint);

  ui->setupUi(this);

  QShortcut* shortcut_rename = new QShortcut(QKeySequence(Qt::Key_F2), ui->treeWidget);
  connect(shortcut_rename, &QShortcut::activated, this, &PointDialog::on_toolButton_rename_clicked);

  QShortcut* shortcut_enter = new QShortcut(QKeySequence(Qt::Key_Enter), ui->treeWidget);
  connect(shortcut_enter, &QShortcut::activated, this, &PointDialog::on_toolButton_jump_clicked);

  QShortcut* shortcut_return = new QShortcut(QKeySequence(Qt::Key_Return), ui->treeWidget);
  connect(shortcut_return, &QShortcut::activated, this, &PointDialog::on_toolButton_jump_clicked);

  QStringList headers = {
      tr("Timestamp"),
      tr("Name"),
      tr("Timestamp_num"),
  };

  ui->treeWidget->setRootIsDecorated(false);
  ui->treeWidget->setContextMenuPolicy(Qt::CustomContextMenu);
  ui->treeWidget->setHeaderLabels(headers);
  ui->treeWidget->setColumnWidth(0, 120);
  ui->treeWidget->setSelectionMode(QTreeWidget::ExtendedSelection);
  ui->treeWidget->setColumnHidden(2, true);

  connect(ui->treeWidget, &QTreeWidget::customContextMenuRequested, this, [this](const QPoint& pos) {
    auto item_index = ui->treeWidget->indexAt(pos);

    if (!item_index.isValid()) {
      return;
    }

    QTreeWidgetItem* item = ui->treeWidget->itemAt(pos);

    if (!item) {
      return;
    }

    const QList<QTreeWidgetItem*>& selected_items = ui->treeWidget->selectedItems();

    QMenu menu;

    if (selected_items.size() > 1) {
      menu.addAction(QIcon(":/resource/sub.png"), tr("Remove"), this,
                     [this, selected_items]() { on_toolButton_del_clicked(); });
    } else {
      menu.addAction(QIcon(":/resource/go.png"), tr("Jump"), this, [this]() { on_toolButton_jump_clicked(); });

      menu.addAction(QIcon(":/resource/rename.png"), tr("Rename"), this, [this]() { on_toolButton_rename_clicked(); });

      menu.addAction(QIcon(":/resource/sub.png"), tr("Remove"), this, [this]() { on_toolButton_del_clicked(); });
    }

    menu.exec(QCursor::pos());
  });

  connect(ui->treeWidget, &QTreeWidget::doubleClicked, this,
          [this](const QModelIndex&) { on_toolButton_jump_clicked(); });

  connect(ui->treeWidget, &QTreeWidget::itemSelectionChanged, this, [this]() {
    ui->toolButton_jump->setEnabled(ui->treeWidget->currentItem());
    ui->toolButton_rename->setEnabled(ui->treeWidget->currentItem());
    ui->toolButton_del->setEnabled(ui->treeWidget->currentItem());

    if (ui->treeWidget->currentItem()) {
      ui->treeWidget->currentItem()->setSelected(true);
    }
  });

  connect(ui->treeWidget, &QTreeWidget::currentItemChanged, this, [this](QTreeWidgetItem*, QTreeWidgetItem*) {
    ui->toolButton_jump->setEnabled(ui->treeWidget->currentItem());
    ui->toolButton_rename->setEnabled(ui->treeWidget->currentItem());
    ui->toolButton_del->setEnabled(ui->treeWidget->currentItem());
  });
}

PointDialog::~PointDialog() { delete ui; }

void PointDialog::reload_file(const QString& bag_path) {
  ui->toolButton_jump->setEnabled(false);
  ui->toolButton_rename->setEnabled(false);
  ui->toolButton_del->setEnabled(false);
  ui->treeWidget->clear();

  QFileInfo file_info(bag_path);

  QString base_name = file_info.fileName();
  QString suffix_name = file_info.suffix();

  if (!suffix_name.isEmpty()) {
    base_name = base_name.left(base_name.size() - suffix_name.size() - 1);
  }

  txt_file_.setFileName(file_info.dir().absolutePath() + "/" + base_name + ".dbtxt");

  if (!txt_file_.exists()) {
    return;
  }

  (void)txt_file_.open(QFile::ReadOnly | QFile::Text);

  if (!txt_file_.isOpen()) {
    return;
  }

  QString read_all = QString::fromUtf8(txt_file_.readAll());

  txt_file_.close();

#if QT_VERSION >= QT_VERSION_CHECK(5, 15, 0)
  const QStringList& lines = read_all.split('\n', Qt::SkipEmptyParts);
#else
  const QStringList& lines = read_all.split('\n');
#endif

  static QRegularExpression reg("\\s*,\\s*");

  for (auto line : lines) {
    line = line.trimmed();

    if (line.isEmpty()) {
      continue;
    }

#if QT_VERSION >= QT_VERSION_CHECK(5, 15, 0)
    const auto& words = line.split(reg, Qt::SkipEmptyParts);
#else
    const auto& words = line.split(reg);
#endif

    if (words.size() != 2) {
      continue;
    }

    const QString& timestamp = words.at(0);

    qint64 timestamp_num = timestamp.toLongLong();

    const QString& name = words.at(1);

    QTreeWidgetItem* item = new QTreeWidgetItem;

    std::string timestamp_str = vlink::Helpers::format_milliseconds(timestamp.toLongLong() + date_time_, true);

    item->setData(0, Qt::UserRole, timestamp_num);

    item->setText(0, QString::fromStdString(timestamp_str));

    item->setText(1, name);

    item->setData(1, Qt::UserRole, static_cast<quint64>(date_time_));

    ui->treeWidget->addTopLevelItem(item);
  }

  ui->treeWidget->sortByColumn(0, Qt::AscendingOrder);

  if (ui->treeWidget->topLevelItemCount() > 0) {
    ui->treeWidget->setCurrentItem(ui->treeWidget->topLevelItem(ui->treeWidget->topLevelItemCount() - 1));
  }

  if (ui->treeWidget->currentItem()) {
    ui->treeWidget->currentItem()->setSelected(true);
  }
}

void PointDialog::save_file() {
  if (txt_file_.fileName().isEmpty()) {
    return;
  }

  (void)txt_file_.open(QFile::WriteOnly | QFile::Text | QFile::Truncate);

  if (!txt_file_.isOpen()) {
    return;
  }

  QString write_all_str;
  QString line_str;

  for (int i = 0; i < ui->treeWidget->topLevelItemCount(); ++i) {
    QTreeWidgetItem* item = ui->treeWidget->topLevelItem(i);

    line_str = QString::number(item->data(0, Qt::UserRole).toLongLong()) + "," + item->text(1) + "\n";

    write_all_str += line_str;
  }

  txt_file_.write(write_all_str.toUtf8());

  txt_file_.close();
}

void PointDialog::set_date_time(uint64_t date_time) {
  date_time_ = date_time;

  for (int i = 0; i < ui->treeWidget->topLevelItemCount(); ++i) {
    QTreeWidgetItem* item = ui->treeWidget->topLevelItem(i);

    quint64 timestamp = item->data(0, Qt::UserRole).toULongLong();

    std::string timestamp_str = vlink::Helpers::format_milliseconds(timestamp + date_time_, true);

    item->setData(1, Qt::UserRole, static_cast<qint64>(date_time_));
    item->setText(0, QString::fromStdString(timestamp_str));
  }
}

void PointDialog::showEvent(QShowEvent* event) {
  if (line_edit_ && line_edit_->hasFocus()) {
    line_edit_->clearFocus();
  }

  QDialog::showEvent(event);
}

void PointDialog::hideEvent(QHideEvent* event) {
  if (line_edit_ && line_edit_->hasFocus()) {
    line_edit_->clearFocus();
  }

  QDialog::hideEvent(event);
}

void PointDialog::closeEvent(QCloseEvent* event) {
  if (line_edit_ && line_edit_->hasFocus()) {
    line_edit_->clearFocus();
  }

  QDialog::closeEvent(event);
}

void PointDialog::resizeEvent(QResizeEvent* event) { QDialog::resizeEvent(event); }

void PointDialog::on_toolButton_jump_clicked() {
  if (!PlayerWindow::get_instance()) {
    return;
  }

  if (line_edit_ && line_edit_->hasFocus()) {
    line_edit_->clearFocus();
    return;
  }

  const QList<QTreeWidgetItem*>& selected_items = ui->treeWidget->selectedItems();

  if (selected_items.size() != 1) {
    return;
  }

  QTreeWidgetItem* item = ui->treeWidget->currentItem();

  if (!item) {
    return;
  }

  ui->treeWidget->setFocus();

  int64_t timestamp = static_cast<int64_t>(item->data(0, Qt::UserRole).toLongLong());

  emit jump_point(timestamp);
}

void PointDialog::on_toolButton_rename_clicked() {
  if (!PlayerWindow::get_instance()) {
    return;
  }

  const QList<QTreeWidgetItem*>& selected_items = ui->treeWidget->selectedItems();

  if (selected_items.size() != 1) {
    return;
  }

  QTreeWidgetItem* item = ui->treeWidget->currentItem();

  if (!item) {
    return;
  }

  if (line_edit_ && line_edit_->hasFocus()) {
    line_edit_->clearFocus();
  }

  line_edit_ = new QLineEdit(ui->treeWidget);
  line_edit_->setMaximumHeight(ui->treeWidget->visualItemRect(item).height());
  line_edit_->setText(item->text(1));
  line_edit_->selectAll();
  line_edit_->setFocus();

  connect(line_edit_, &QLineEdit::editingFinished, this, [this, item]() {
    item->setText(1, line_edit_->text());

    save_file();

    ui->treeWidget->removeItemWidget(item, 1);

    if (line_edit_) {
      line_edit_->deleteLater();
      line_edit_ = nullptr;
    }
  });

  ui->treeWidget->setItemWidget(item, 1, line_edit_);
}

void PointDialog::on_toolButton_add_clicked() {
  if (!PlayerWindow::get_instance()) {
    return;
  }

  int64_t timestamp_num = PlayerWindow::get_instance()->get_current_timestamp();

  std::string timestamp_str = vlink::Helpers::format_milliseconds(timestamp_num + date_time_, true);

  QTreeWidgetItem* item = new QTreeWidgetItem;

  item->setData(0, Qt::UserRole, static_cast<qint64>(timestamp_num));

  item->setText(0, QString::fromStdString(timestamp_str));

  item->setText(1, "Check_Point");

  item->setData(1, Qt::UserRole, static_cast<quint64>(date_time_));

  ui->treeWidget->addTopLevelItem(item);

  ui->treeWidget->sortByColumn(0, Qt::AscendingOrder);

  item->setText(1, "Check_Point_" + QString::number(ui->treeWidget->indexOfTopLevelItem(item) + 1));

  if (line_edit_ && line_edit_->hasFocus()) {
    line_edit_->clearFocus();
  }

  line_edit_ = new QLineEdit(ui->treeWidget);
  line_edit_->setMaximumHeight(ui->treeWidget->visualItemRect(item).height());
  line_edit_->setText(item->text(1));
  line_edit_->selectAll();
  line_edit_->setFocus();

  connect(line_edit_, &QLineEdit::editingFinished, this, [this, item]() {
    item->setText(1, line_edit_->text());

    save_file();

    ui->treeWidget->removeItemWidget(item, 1);

    if (line_edit_) {
      line_edit_->deleteLater();
      line_edit_ = nullptr;
    }
  });

  ui->treeWidget->setItemWidget(item, 1, line_edit_);

  ui->treeWidget->setCurrentItem(item);
  item->setSelected(true);

  // save_file();
}

void PointDialog::on_toolButton_del_clicked() {
  if (!PlayerWindow::get_instance()) {
    return;
  }

  const QList<QTreeWidgetItem*>& selected_items = ui->treeWidget->selectedItems();

  if (selected_items.empty()) {
    return;
  }

  QTreeWidgetItem* item = ui->treeWidget->currentItem();

  if (!item) {
    return;
  }

  for (auto* pitem : selected_items) {
    auto index = ui->treeWidget->indexOfTopLevelItem(pitem);
    ui->treeWidget->takeTopLevelItem(index);
    delete pitem;
  }

  save_file();
}

void PointDialog::on_pushButton_close_clicked() { this->close(); }

// NOLINTEND
