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

#include "./mainwindow.h"

#include <vlink/base/helpers.h>
#include <vlink/base/utils.h>
#include <vlink/extension/discovery_viewer.h>
#include <vlink/version.h>
#include <vlink/zerocopy/audio_frame.h>
#include <vlink/zerocopy/camera_frame.h>
#include <vlink/zerocopy/object_array.h>
#include <vlink/zerocopy/occupancy_grid.h>
#include <vlink/zerocopy/point_cloud.h>
#include <vlink/zerocopy/raw_data.h>
#include <vlink/zerocopy/tensor.h>

#include <QClipboard>
#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QFileDialog>
#include <QFontDatabase>
#include <QHideEvent>
#include <QItemDelegate>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMessageBox>
#include <QMetaMethod>
#include <QNetworkAccessManager>
#include <QNetworkProxy>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPainter>
#include <QProcess>
#include <QRegularExpressionValidator>
#include <QResizeEvent>
#include <QSettings>
#include <QShowEvent>
#include <QStandardPaths>
#include <QTime>
#include <QTimeZone>
#include <QUrl>
#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <queue>
#include <string>

#include "./aboutdialog.h"
#include "./analyzedialog.h"
#include "./cameradialog.h"
#include "./editdialog.h"
#include "./perceptiondialog.h"
#include "./playdialog.h"
#include "./point3ddialog.h"
#include "./rawdialog.h"
#include "./recorddialog.h"
#include "./settingsdialog.h"
#include "./topologydialog.h"
#include "./ui_mainwindow.h"

#ifdef _WIN32
#ifdef GetMessage
#undef GetMessage
#endif
#endif

QString global_proto_dir_config = QStandardPaths::writableLocation(QStandardPaths::HomeLocation) + "/.vlink_proto_dir";
QString global_fbs_dir_config = QStandardPaths::writableLocation(QStandardPaths::HomeLocation) + "/.vlink_fbs_dir";

class CustomSqlQueryModel : public QSqlQueryModel {
 public:
  using QSqlQueryModel::QSqlQueryModel;

  QVariant data(const QModelIndex& index, int role) const override {
    if (index.column() == 0) {
      if (role == Qt::DisplayRole) {
        int64_t microseconds = QSqlQueryModel::data(index, role).toLongLong();
        int hours = microseconds / 3600000000;
        microseconds %= 3600000000;
        int minutes = microseconds / 60000000;
        microseconds %= 60000000;
        int seconds = microseconds / 1000000;
        microseconds %= 1000000;

        return QString("%1:%2:%3.%4")
            .arg(hours, 2, 10, QChar('0'))
            .arg(minutes, 2, 10, QChar('0'))
            .arg(seconds, 2, 10, QChar('0'))
            .arg(microseconds, 6, 10, QChar('0'));
      } else if (role == Qt::UserRole) {
        return QSqlQueryModel::data(index, Qt::DisplayRole);
      }
    }

    return QSqlQueryModel::data(index, role);
  }

  QVariant headerData(int section, Qt::Orientation orientation, int role) const override {
    if (role == Qt::DisplayRole) {
      if (orientation == Qt::Horizontal) {
        switch (section) {
          case 0:
            return "TIME";
          case 1:
            return "URL";
          case 2:
            return "DATA";
        }
      } else {
        return QString::number(section + 1);
      }
    }

    return QVariant();
  }
};

MainWindow::MainWindow(QWidget* parent) : QMainWindow(parent), ui(new Ui::MainWindow) {
  setWindowFlags(Qt::Window | Qt::WindowMinimizeButtonHint | Qt::WindowMaximizeButtonHint | Qt::WindowCloseButtonHint);

  ui->setupUi(this);

  ui->actionAnalyzer_K->setVisible(false);

  {
    QFont font = ui->label_status->font();
    font.setBold(true);
    ui->label_status->setFont(font);
  }

  analyze_dialog_ = new AnalyzeDialog(this);

#ifdef _WIN32
  analyzer_process_.setProgram(qApp->applicationDirPath() + "/" + "vlink-analyzer.exe");
#else
  analyzer_process_.setProgram(qApp->applicationDirPath() + "/" + "vlink-analyzer");
#endif

  vlink::ProxyAPI::Config proxy_config;

  if (SettingsDialog::get_mode() == SettingsDialog::kController) {
    proxy_config.role = vlink::ProxyAPI::kController;
  } else {
    proxy_config.role = vlink::ProxyAPI::kListener;
  }

  proxy_config.domain_id = SettingsDialog::get_domain_id();
  proxy_config.dds_impl = SettingsDialog::get_dds_impl();
  proxy_config.security_key = SettingsDialog::get_security_key();
  proxy_config.native = SettingsDialog::get_native_mode();
  proxy_config.reliable = SettingsDialog::get_reliable_mode();
  proxy_config.enable_tcp = SettingsDialog::get_tcp_mode();
  proxy_config.direct = SettingsDialog::get_direct_mode();
  proxy_config.match_version = SettingsDialog::get_match_version();
  proxy_config.allow_ip = SettingsDialog::get_allow_ip();
  proxy_config.peer_ip = SettingsDialog::get_peer_ip();
  proxy_config.buf_size = SettingsDialog::get_buf_size();
  proxy_config.mtu_size = SettingsDialog::get_mtu_size();

  proxy_ = std::make_shared<vlink::ProxyAPI>(proxy_config);

  if (proxy_->get_current_config().role == vlink::ProxyAPI::kController) {
    setWindowTitle(tr("VLink Viewer") + " v" + VLINK_VERSION +
                   tr("  [Role: Controller]  [Domain ID: %2]").arg(QString::number(proxy_config.domain_id)));
  } else {
    setWindowTitle(tr("VLink Viewer") + " v" + VLINK_VERSION +
                   tr("  [Role: Listener]  [Domain ID: %2]").arg(QString::number(proxy_config.domain_id)));

    // ui->checkBox_active->setEnabled(false);
    // ui->checkBox_observe->setEnabled(false);
    // ui->lineEdit_filter->setEnabled(false);
    // ui->comboBox_condi->setEnabled(false);
    // ui->comboBox_filter->setEnabled(false);
  }

  validator_normal_int_ = new QRegularExpressionValidator(QRegularExpression("^\\d+$"));
  validator_int32_ = new QRegularExpressionValidator(QRegularExpression("^-?(0[xX][0-9a-fA-F]{1,8}|[0-9]{1,9})$"));
  validator_int64_ = new QRegularExpressionValidator(QRegularExpression("^-?(0[xX][0-9a-fA-F]{1,16}|[0-9]{1,18})$"));
  validator_uint32_ = new QRegularExpressionValidator(QRegularExpression("^(0[xX][0-9a-fA-F]{1,8}|[0-9]{1,10})$"));
  validator_uint64_ = new QRegularExpressionValidator(QRegularExpression("^(0[xX][0-9a-fA-F]{1,16}|[0-9]{1,20})$"));
  validator_double_ = new QRegularExpressionValidator(QRegularExpression("^-?[0-9]+([.][0-9]*)?$"));
  validator_float_ = new QRegularExpressionValidator(QRegularExpression("^-?[0-9]+([.][0-9]*)?$"));
  validator_bool_ = new QRegularExpressionValidator(QRegularExpression("^(true|false)$"));
  validator_enum_ = new QRegularExpressionValidator(QRegularExpression("^[0-9]{1,10}$"));
  validator_string_ = new QRegularExpressionValidator(QRegularExpression("^.*$"));

  ui->label_access->setVisible(false);

  status_label1_ = new QLabel(this);
  status_label2_ = new QLabel(this);

  update_status_bar(-1, -1, -1, -1, -1);

  ui->statusbar->addWidget(status_label1_);
  ui->statusbar->addPermanentWidget(status_label2_);
  ui->statusbar->setSizeGripEnabled(false);
  ui->statusbar->setStyleSheet(QString("QStatusBar::item{border: 0px}"));

  ui->widget_freq->set_max_count(60);
  ui->widget_freq->set_value_range(0, 100);
  ui->widget_freq->set_unit_text("Hz");

  ui->widget_rate->set_max_count(60);
  ui->widget_rate->set_value_range(0, 100);
  ui->widget_rate->set_unit_text("B");

  ui->widget_loss->set_max_count(60);
  ui->widget_loss->set_value_range(0, 100);
  ui->widget_loss->set_unit_text("%");

  ui->widget_lat->set_max_count(60);
  ui->widget_lat->set_value_range(0, 10);
  ui->widget_lat->set_unit_text("ms");

  sys_timer_ = new QTimer(this);
  sys_timer_->setInterval(100);
  connect(sys_timer_, &QTimer::timeout, this, [this]() { update_time(); });

  flag_timer_ = new QTimer(this);
  flag_timer_->setInterval(50);

  connect(flag_timer_, &QTimer::timeout, this, [this]() {
    ui->label_access->setVisible(false);
    flag_timer_->stop();
  });

  filter_timer_ = new QTimer(this);

  if (proxy_->is_enable_filter()) {
    filter_timer_->setInterval(100);

    connect(filter_timer_, &QTimer::timeout, this, [this]() {
      send_control(ui->checkBox_observe->isChecked() ? vlink::ProxyAPI::kObserveAll : vlink::ProxyAPI::kObserveOne,
                   true);
      filter_timer_->stop();
    });
  } else {
    filter_timer_->setInterval(1000);
  }

  QStringList url_headers = {
      tr("TYPE"), tr("URL"), tr("FREQ"), tr("RATE"), tr("LOSS"), tr("LATENCY"),
  };

  QStringList property_headers = {
      tr("FIELD"),
      tr("TYPE"),
      tr("PROPERTY"),
      tr("VALUE"),
  };

  QStringList process_headers = {
      tr("PID"),
      tr("NAME"),
  };

  local_database_ = QSqlDatabase::addDatabase("QSQLITE");

  ui->checkBox_showfreq->setChecked(true);
  ui->checkBox_showrate->setChecked(true);

  ui->treeWidget_url->setRootIsDecorated(false);
  ui->treeWidget_url->setContextMenuPolicy(Qt::CustomContextMenu);
  ui->treeWidget_url->setHeaderLabels(url_headers);
  ui->treeWidget_url->setColumnWidth(0, 95);
  ui->treeWidget_url->setColumnWidth(1, 150);
  ui->treeWidget_url->setColumnWidth(2, 90);
  ui->treeWidget_url->setColumnWidth(3, 90);
  ui->treeWidget_url->setColumnWidth(4, 90);
  ui->treeWidget_url->setColumnWidth(5, 90);
  ui->treeWidget_url->setSelectionMode(QAbstractItemView::ExtendedSelection);
  ui->treeWidget_url->setColumnHidden(2, false);
  ui->treeWidget_url->setColumnHidden(3, false);
  ui->treeWidget_url->setColumnHidden(4, true);
  ui->treeWidget_url->setColumnHidden(5, true);

  // ui->treeWidget_property->setRootIsDecorated(false);
  ui->treeWidget_property->setContextMenuPolicy(Qt::CustomContextMenu);
  ui->treeWidget_property->setHeaderLabels(property_headers);
  ui->treeWidget_property->setColumnWidth(0, 110);
  ui->treeWidget_property->setColumnWidth(1, 110);
  ui->treeWidget_property->setColumnWidth(2, 110);
  ui->treeWidget_property->setColumnWidth(3, 115);
  ui->treeWidget_property->setSelectionMode(QAbstractItemView::SingleSelection);
  ui->treeWidget_property->setAnimated(false);
  ui->treeWidget_property->setSortingEnabled(false);
  ui->treeWidget_property->setEnabled(false);

  ui->treeWidget_process1->setRootIsDecorated(false);
  ui->treeWidget_process1->setContextMenuPolicy(Qt::CustomContextMenu);
  ui->treeWidget_process1->setHeaderLabels(process_headers);
  ui->treeWidget_process1->setColumnWidth(0, 80);
  ui->treeWidget_process1->setColumnWidth(1, 160);

  ui->treeWidget_process2->setRootIsDecorated(false);
  ui->treeWidget_process2->setContextMenuPolicy(Qt::CustomContextMenu);
  ui->treeWidget_process2->setHeaderLabels(process_headers);
  ui->treeWidget_process2->setColumnWidth(0, 80);
  ui->treeWidget_process2->setColumnWidth(1, 160);

  ui->tableView_data->setModel(new CustomSqlQueryModel(this));
  ui->tableView_data->setContextMenuPolicy(Qt::CustomContextMenu);
  ui->tableView_data->setColumnWidth(0, 150);
  ui->tableView_data->setColumnWidth(1, ui->tableView_data->size().width() - 150 - 100);

  ui->lineEdit_protodir->setReadOnly(true);
  ui->lineEdit_fbsdir->setReadOnly(true);

  //  ui->actionTopology_N->setEnabled(false);
  //  ui->actionRecord_R->setEnabled(false);
  //  ui->actionPlay_P->setEnabled(false);
  ui->actionEdit_E->setEnabled(false);

  ui->checkBox_active->setEnabled(false);
  ui->checkBox_selectall->setEnabled(false);

  ui->lineEdit_jump->setValidator(validator_normal_int_);

  ui->horizontalSlider->setValue(2);

  // connect(
  //     ui->treeWidget_url, &QTreeWidget::currentItemChanged, this,
  //     [this](QTreeWidgetItem *, QTreeWidgetItem *) { process_current_item_changed(); }, Qt::QueuedConnection);

  ui->stackedWidget_status->setCurrentIndex(0);
  ui->buttonGroup_graph->setId(ui->radioButton_freq, 0);
  ui->buttonGroup_graph->setId(ui->radioButton_rate, 1);
  ui->buttonGroup_graph->setId(ui->radioButton_loss, 2);
  ui->buttonGroup_graph->setId(ui->radioButton_lat, 3);

#if QT_VERSION >= QT_VERSION_CHECK(5, 15, 0)
  connect(ui->buttonGroup_graph, &QButtonGroup::idClicked, this, [this](int index) {
#else
  connect(ui->buttonGroup_graph, static_cast<void (QButtonGroup::*)(int)>(&QButtonGroup::buttonClicked), this,
          [this](int index) {
#endif
    ui->stackedWidget_status->setCurrentIndex(index);

    auto* current_item = ui->treeWidget_url->currentItem();
    const auto& selected_items = ui->treeWidget_url->selectedItems();

    switch (ui->stackedWidget_status->currentIndex()) {
      case 0:
        ui->label_graph_value1->setText(tr("Current Freq: "));
        break;
      case 1:
        ui->label_graph_value1->setText(tr("Current Rate: "));
        break;
      case 2:
        ui->label_graph_value1->setText(tr("Current Loss: "));
        break;
      case 3:
        ui->label_graph_value1->setText(tr("Current Latency: "));
        break;
      default:
        break;
    }

    ui->label_graph_value2->setText("---");

    if (current_item && selected_items.count() == 1) {
      if (selected_items.contains(current_item)) {
        if (current_item) {
          switch (ui->stackedWidget_status->currentIndex()) {
            case 0:
              ui->label_graph_value2->setText(current_item->text(2));
              break;
            case 1:
              ui->label_graph_value2->setText(current_item->text(3));
              break;
            case 2:
              ui->label_graph_value2->setText(current_item->text(4));
              break;
            case 3:
              ui->label_graph_value2->setText(current_item->text(5));
              break;
            default:
              break;
          }
        }
      }
    }
  });

  connect(
      ui->treeWidget_url, &QTreeWidget::itemSelectionChanged, this,
      [this]() {
        int select_count = 0;

        for (int i = 0; i < ui->treeWidget_url->topLevelItemCount(); ++i) {
          QTreeWidgetItem* item = ui->treeWidget_url->topLevelItem(i);

          if (item->isSelected()) {
            ++select_count;
          }
        }

        ui->checkBox_selectall->blockSignals(true);

        if (select_count <= 1) {
          ui->checkBox_selectall->setCheckState(Qt::Unchecked);
        } else if (select_count == ui->treeWidget_url->topLevelItemCount()) {
          ui->checkBox_selectall->setCheckState(Qt::Checked);
        } else {
          ui->checkBox_selectall->setCheckState(Qt::PartiallyChecked);
        }

        ui->checkBox_selectall->blockSignals(false);

        // ui->checkBox_selectall->setEnabled(ui->treeWidget_url->topLevelItemCount() != 0);

        if (is_in_model_) {
          return;
        }

        if (ui->treeWidget_url->selectedItems().empty() && ui->treeWidget_url->currentItem()) {
          ui->treeWidget_url->blockSignals(true);

          ui->treeWidget_url->currentItem()->setSelected(true);

          ui->treeWidget_url->blockSignals(false);

          if (last_select_items_.size() == 1) {
            if (last_select_items_.at(0) == ui->treeWidget_url->currentItem()) {
              return;
            }
          }
        } else if (!ui->treeWidget_url->selectedItems().empty() && ui->treeWidget_url->currentItem() &&
                   !ui->treeWidget_url->currentItem()->isSelected()) {
          const auto& selected_items = ui->treeWidget_url->selectedItems();
          ui->treeWidget_url->blockSignals(true);
          ui->treeWidget_url->setCurrentItem(ui->treeWidget_url->selectedItems().last());

          for (const auto& item : selected_items) {
            item->setSelected(true);
          }

          ui->treeWidget_url->blockSignals(false);
        }

        process_current_item_changed();
      },
      Qt::QueuedConnection);

  connect(ui->tableView_data->selectionModel(), &QItemSelectionModel::currentChanged, this,
          [this](const QModelIndex& current, const QModelIndex& previous) {
            (void)current;
            (void)previous;

            clear_all_property_item(ui->treeWidget_property);
            update_local_proto();
          });

  connect(ui->treeWidget_property, &QTreeWidget::currentItemChanged, this, [this]() {
    if (ui->stackedWidget_main->currentIndex() == 0) {
      auto* current_item = ui->treeWidget_property->currentItem();
      if (current_item) {
        if (current_item->data(1, Qt::UserRole).toInt() > 0) {
          ui->pushButton_analyze->setEnabled(true);
        } else {
          ui->pushButton_analyze->setEnabled(false);
        }
      } else {
        ui->pushButton_analyze->setEnabled(false);
      }
    } else {
      ui->pushButton_analyze->setEnabled(false);
    }
  });

  connect(ui->treeWidget_url, &QTreeWidget::customContextMenuRequested, this, [this](const QPoint& pos) {
    auto item_index = ui->treeWidget_url->indexAt(pos);

    if (!item_index.isValid()) {
      return;
    }

    QMenu menu;
    menu.addAction(tr("Copy to Clipboard"), this,
                   [item_index]() { qApp->clipboard()->setText(item_index.data(Qt::DisplayRole).toString()); });
    menu.exec(QCursor::pos());
  });

  connect(ui->treeWidget_property, &QTreeWidget::customContextMenuRequested, this, [this](const QPoint& pos) {
    auto item_index = ui->treeWidget_property->indexAt(pos);

    if (!item_index.isValid()) {
      return;
    }

    QMenu menu;
    menu.addAction(tr("Copy to Clipboard"), this, [item_index]() {
      if (item_index.data(Qt::DisplayRole) == "{...}") {
        qApp->clipboard()->setText(item_index.data(Qt::UserRole).toString());
      } else {
        qApp->clipboard()->setText(item_index.data(Qt::DisplayRole).toString());
      }
    });
    menu.exec(QCursor::pos());
  });

  connect(ui->treeWidget_process1, &QTreeWidget::customContextMenuRequested, this, [this](const QPoint& pos) {
    auto item_index = ui->treeWidget_process1->indexAt(pos);

    if (!item_index.isValid()) {
      return;
    }

    QMenu menu;
    menu.addAction(tr("Copy to Clipboard"), this,
                   [item_index]() { qApp->clipboard()->setText(item_index.data(Qt::DisplayRole).toString()); });
    menu.exec(QCursor::pos());
  });

  connect(ui->treeWidget_process2, &QTreeWidget::customContextMenuRequested, this, [this](const QPoint& pos) {
    auto item_index = ui->treeWidget_process2->indexAt(pos);

    if (!item_index.isValid()) {
      return;
    }

    QMenu menu;
    menu.addAction(tr("Copy to Clipboard"), this,
                   [item_index]() { qApp->clipboard()->setText(item_index.data(Qt::DisplayRole).toString()); });
    menu.exec(QCursor::pos());
  });

  connect(ui->tableView_data, &QTableView::customContextMenuRequested, this, [this](const QPoint& pos) {
    auto item_index = ui->tableView_data->indexAt(pos);

    if (!item_index.isValid()) {
      return;
    }

    QMenu menu;
    menu.addAction(tr("Copy to Clipboard"), this,
                   [item_index]() { qApp->clipboard()->setText(item_index.data(Qt::DisplayRole).toString()); });
    menu.exec(QCursor::pos());
  });

  connect(ui->checkBox_observe, &QCheckBox::clicked, this, [this](bool checked) {
    ui->widget_freq->clear();
    ui->widget_rate->clear();
    ui->widget_loss->clear();
    ui->widget_lat->clear();

    ui->checkBox_active->setEnabled(checked);

    if (!checked) {
      ui->checkBox_active->setChecked(false);
    }

    send_control(ui->checkBox_observe->isChecked() ? vlink::ProxyAPI::kObserveAll : vlink::ProxyAPI::kObserveOne, true);
  });

  connect(ui->checkBox_selectall, &QCheckBox::clicked, this, [this](bool checked) {
    if (ui->treeWidget_url->currentItem()) {
      ui->treeWidget_url->currentItem()->setSelected(true);
    } else {
      if (ui->treeWidget_url->topLevelItemCount() > 0) {
        ui->treeWidget_url->setCurrentItem(ui->treeWidget_url->topLevelItem(0));
      }
    }

    for (int i = 0; i < ui->treeWidget_url->topLevelItemCount(); ++i) {
      QTreeWidgetItem* item = ui->treeWidget_url->topLevelItem(i);
      item->setSelected(checked);
    }
  });

  connect(ui->comboBox_filter, static_cast<void (QComboBox::*)(int)>(&QComboBox::currentIndexChanged), this,
          [this](int) {
            filter_timer_->stop();
            filter_timer_->start();
          });

  connect(ui->lineEdit_filter, &QLineEdit::textEdited, this, [this](const QString&) {
    filter_timer_->stop();
    filter_timer_->start();
  });

  connect(ui->comboBox_condi, static_cast<void (QComboBox::*)(int)>(&QComboBox::currentIndexChanged), this,
          [this](int) {
            filter_timer_->stop();
            filter_timer_->start();
          });

  connect(ui->splitter_main, &QSplitter::splitterMoved, this, [this](int pos, int index) {
    (void)pos;
    (void)index;

    adjust_size();
  });

  connect(ui->lineEdit_jump, &QLineEdit::returnPressed, this, [this]() { on_pushButton_jump_clicked(); });

  connect(ui->comboBox_jump, static_cast<void (QComboBox::*)(int)>(&QComboBox::currentIndexChanged), this,
          [this](int index) {
            ui->lineEdit_jump->clear();

            switch (index) {
              case 0:
                ui->lineEdit_jump->setValidator(validator_normal_int_);
                break;
              case 1:
                ui->lineEdit_jump->setValidator(validator_double_);
                break;
              default:
                break;
            }
          });

  connect(ui->horizontalSlider, &QSlider::valueChanged, this, [this](int position) {
    ui->widget_freq->set_max_count(position * 30);
    ui->widget_rate->set_max_count(position * 30);
    ui->widget_loss->set_max_count(position * 30);
    ui->widget_lat->set_max_count(position * 30);
    ui->widget_freq->update();
    ui->widget_rate->update();
    ui->widget_loss->update();
    ui->widget_lat->update();
  });

  ui->splitter_main->setChildrenCollapsible(false);
  ui->splitter_main->setCollapsible(0, false);
  ui->splitter_main->setCollapsible(1, false);
  ui->splitter_main->setCollapsible(2, false);

  ui->splitter_status->setChildrenCollapsible(false);
  ui->splitter_status->setCollapsible(0, false);
  ui->splitter_status->setCollapsible(1, false);
  // ui->splitter_status->setCollapsible(2, false);

  {
    QString proto_dir = QStandardPaths::writableLocation(QStandardPaths::HomeLocation);
    QString fbs_dir;
    QFile proto_file(global_proto_dir_config);
    QFile fbs_file(global_fbs_dir_config);

    if (proto_file.exists() && proto_file.open(QFile::ReadOnly)) {
      proto_dir = proto_file.readAll().simplified();
      proto_file.close();
    }

    if (fbs_file.exists() && fbs_file.open(QFile::ReadOnly)) {
      fbs_dir = fbs_file.readAll().simplified();
      fbs_file.close();
    }

    QSettings settings(QStandardPaths::writableLocation(QStandardPaths::CacheLocation) + "/settings.ini",
                       QSettings::IniFormat);
    settings.beginGroup("MainWindow");

    auto geometry = settings.value("geometry", this->geometry()).toByteArray();

    auto splitter_main_state = settings.value("splitter_main", ui->splitter_main->saveState()).toByteArray();
    auto splitter_status_state = settings.value("splitter_status", ui->splitter_status->saveState()).toByteArray();

    settings.endGroup();

    select_source_dir(proto_dir.toStdString());
    select_fbs_dir(fbs_dir.toStdString());

    restoreGeometry(geometry);

    ui->splitter_main->restoreState(splitter_main_state);
    ui->splitter_status->restoreState(splitter_status_state);
  }

  proxy_->register_connect_callback([this](bool connected) {
    QMetaObject::invokeMethod(this, "update_connected", Qt::QueuedConnection, Q_ARG(bool, connected));
  });

  proxy_->register_error_callback([this](vlink::ProxyAPI::Error error) {
    QMetaObject::invokeMethod(this, "update_error", Qt::QueuedConnection, Q_ARG(int, error));
  });

  proxy_->register_info_callback([this](const std::vector<vlink::ProxyAPI::Info>& info_list) {
    {
      std::shared_lock lock(data_mutex_);

      if (info_callback_) {
        info_callback_(info_list);
        return;
      }

      if (data_callback_) {
        return;
      }
    }

    QMetaObject::invokeMethod(this, "update_url_widget", Qt::QueuedConnection,
                              Q_ARG(QVariant, QVariant::fromValue<std::vector<vlink::ProxyAPI::Info>>(info_list)));
  });

  proxy_->register_data_callback([this, proxy_ptr = proxy_.get()](const vlink::ProxyAPI::Data& proxy_data) {
    {
      std::shared_lock lock(data_mutex_);

      if (data_callback_) {
        data_callback_(proxy_data);
        return;
      }

      if (data_callback2_) {
        data_callback2_(proxy_data);
        return;
      }
    }

    ++total_data_seq_;
    total_data_latency_ += proxy_ptr->get_latency();

    if (current_proxy_mode_ == vlink::ProxyAPI::kObserveAll || current_proxy_mode_ == vlink::ProxyAPI::kObserveOne) {
      QElapsedTimer timer;
      timer.start();

      QMetaObject::invokeMethod(this, "update_property_widget", Qt::QueuedConnection,
                                Q_ARG(QVariant, QVariant::fromValue<vlink::ProxyAPI::Data>(proxy_data)),
                                Q_ARG(QElapsedTimer, timer), Q_ARG(bool, true));
    }
  });

  sys_timer_->start();

  ui->stackedWidget_main->setCurrentIndex(0);

  update_connected(false);

  proxy_->async_run();

  if (SettingsDialog::get_detect_upgrade()) {
    QTimer::singleShot(1000, this, [this]() { check_new_version(); });
  }
}

MainWindow::~MainWindow() {
  proxy_->quit();

  {
    std::lock_guard lock(data_mutex_);
    info_callback_ = nullptr;
    data_callback_ = nullptr;
    data_callback2_ = nullptr;
  }

  {
    QSettings settings(QStandardPaths::writableLocation(QStandardPaths::CacheLocation) + "/settings.ini",
                       QSettings::IniFormat);
    settings.beginGroup("MainWindow");
    settings.setValue("geometry", saveGeometry());
    settings.setValue("splitter_main", ui->splitter_main->saveState());
    settings.setValue("splitter_status", ui->splitter_status->saveState());
    settings.endGroup();
    settings.sync();
  }

  clear_all_url_item();

  clear_all_property_item(ui->treeWidget_property);

  clear_all_process_item();

  if (root_msg_) {
    delete root_msg_;
    root_msg_ = nullptr;
  }

  if (analyzer_process_.state() != QProcess::NotRunning) {
    analyzer_process_.terminate();
    analyzer_process_.waitForFinished(500);

    if (analyzer_process_.state() != QProcess::NotRunning) {
      analyzer_process_.kill();
      analyzer_process_.waitForFinished(500);
    }
  }

  delete validator_normal_int_;
  delete validator_int32_;
  delete validator_int64_;
  delete validator_uint32_;
  delete validator_uint64_;
  delete validator_double_;
  delete validator_float_;
  delete validator_bool_;
  delete validator_enum_;
  delete validator_string_;

  delete ui;
}

void MainWindow::create_instance() { instance_.store(new MainWindow()); }

void MainWindow::destroy_instance() {
  delete instance_.load();
  instance_ = nullptr;
}

MainWindow* MainWindow::get_instance() { return instance_.load(); }

void MainWindow::open_url(const QString& url) {
#ifdef __linux__
  std::string lib_env = vlink::Utils::get_env("LD_LIBRARY_PATH");
  vlink::Utils::unset_env("LD_LIBRARY_PATH");
#endif

  QDesktopServices::openUrl(QUrl(url, QUrl::TolerantMode));

#ifdef __linux__

  if (!lib_env.empty()) {
    vlink::Utils::set_env("LD_LIBRARY_PATH", lib_env);
  }
#endif
}

void MainWindow::on_actionLocal_D_triggered(bool checked) {
  if (checked) {
    ui->stackedWidget_main->setCurrentIndex(1);
    ui->actionStatus_Viewer->setChecked(false);
    ui->groupBox_status->hide();
    ui->pushButton_analyze->setEnabled(false);

    ui->checkBox_perf->setEnabled(false);

    ui->lineEdit_protoser->clear();
    clear_all_property_item(ui->treeWidget_property);

    if (root_msg_) {
      delete root_msg_;
      root_msg_ = nullptr;
    }

    status_label2_->clear();
    ui->label_graph_value2->setText("---");

    clear_all_process_item();

    status_label1_->clear();

    ui->actionTopology_N->setEnabled(false);
    ui->actionEdit_E->setEnabled(false);
    ui->actionRaw_J->setEnabled(false);
    ui->actionCamera_S->setEnabled(false);
    ui->actionPoint3D_Z->setEnabled(false);
    ui->actionPerception->setEnabled(false);
    ui->actionMap_G->setEnabled(false);
    ui->actionRecord_R->setEnabled(false);
    ui->actionPlay_P->setEnabled(false);

    ui->comboBox_jump->setEnabled(false);
    ui->lineEdit_jump->setEnabled(false);
    ui->pushButton_jump->setEnabled(false);
    ui->pushButton_datareload->setEnabled(false);
    ui->pushButton_datadetails->setEnabled(false);

    ui->groupBox_url->setEnabled(true);
    ui->groupBox_status->setEnabled(true);
    ui->groupBox_proto->setEnabled(true);
    ui->widget_freq->clear();
    ui->widget_rate->clear();
    ui->widget_loss->clear();
    ui->widget_lat->clear();

    ui->treeWidget_property->setEnabled(true);

    current_url_.clear();
    current_ser_.clear();

    ui->treeWidget_url->setCurrentItem(nullptr);

    update_proto_ser();

    send_control(vlink::ProxyAPI::kObserveOne, false);
  } else {
    ui->stackedWidget_main->setCurrentIndex(0);
    ui->actionStatus_Viewer->setChecked(true);
    ui->groupBox_status->show();
    ui->checkBox_perf->setEnabled(true);

    ui->checkBox_perf->setEnabled(true);

    update_status_bar(-1, -1, -1, -1, -1);

    ui->treeWidget_property->setEnabled(false);

    ui->actionTopology_N->setEnabled(proxy_->is_connected());
    ui->actionRecord_R->setEnabled(proxy_->is_connected());
    ui->actionPlay_P->setEnabled(proxy_->is_connected());

    ui->groupBox_url->setEnabled(proxy_->is_connected());
    ui->groupBox_status->setEnabled(proxy_->is_connected());
    ui->groupBox_proto->setEnabled(proxy_->is_connected());

    auto* sql_model = static_cast<CustomSqlQueryModel*>(ui->tableView_data->model());
    sql_model->clear();
    ui->lineEdit_datapath->clear();
    local_database_.close();

    current_url_.clear();
    current_ser_.clear();

    update_proto_ser();

    send_control(ui->checkBox_observe->isChecked() ? vlink::ProxyAPI::kObserveAll : vlink::ProxyAPI::kObserveOne, true);

    ui->tableView_data->setFocus();
  }

  QMetaObject::invokeMethod(this, "adjust_size", Qt::QueuedConnection);
}

void MainWindow::on_actionAnalyzer_K_triggered() {
  if (analyzer_process_.state() != QProcess::NotRunning) {
    return;
  }

  analyzer_process_.start();
  analyzer_process_.waitForStarted(1000);

  if (analyzer_process_.error() == QProcess::FailedToStart) {
    QMessageBox::critical(this, tr("Analyzer Error"), tr("Can not open."));
    return;
  }
}

void MainWindow::on_actionTopology_N_triggered() {
  if (ui->stackedWidget_main->currentIndex() != 0) {
    QMessageBox::warning(this, tr("Warning"), tr("Cannot be executed in database mode."));
    return;
  }

  if (!proxy_->is_connected()) {
    QMessageBox::warning(this, tr("Warning"), tr("Proxy is disconnected."));
    return;
  }

  if (ui->treeWidget_url->currentItem()) {
    auto rate = ui->treeWidget_url->currentItem()->data(3, Qt::UserRole).toULongLong();
    if (rate > 1024 * 1024) {
      QMessageBox message_box(QMessageBox::Warning, tr("Warning"),
                              tr("The bandwidth of raw data is very large. Do you want to continue?"),
                              QMessageBox::Yes | QMessageBox::No, this);

      int result = message_box.exec();

      if (result != QMessageBox::Yes) {
        return;
      }
    }

    ui->treeWidget_url->currentItem()->setText(2, "---");
    ui->treeWidget_url->currentItem()->setText(3, "---");
    ui->treeWidget_url->currentItem()->setText(4, "---");
    ui->treeWidget_url->currentItem()->setText(5, "---");
    update_status_bar(-1, -1, -1, -1, -1);
    status_label2_->clear();
    ui->label_graph_value2->setText("---");

    clear_all_process_item();
  }

  ui->groupBox_url->setEnabled(false);
  ui->groupBox_status->setEnabled(false);
  ui->groupBox_proto->setEnabled(false);
  ui->widget_freq->clear();
  ui->widget_rate->clear();
  ui->widget_loss->clear();
  ui->widget_lat->clear();

  send_control(vlink::ProxyAPI::kAuto, true);

  is_in_model_ = true;
  TopologyDialog dialog(this);
  dialog.exec();
  is_in_model_ = false;

  update_proto_ser();
  ui->groupBox_url->setEnabled(proxy_->is_connected());
  ui->groupBox_status->setEnabled(proxy_->is_connected());
  ui->groupBox_proto->setEnabled(proxy_->is_connected());

  send_control(ui->checkBox_observe->isChecked() ? vlink::ProxyAPI::kObserveAll : vlink::ProxyAPI::kObserveOne, true);
}

void MainWindow::on_actionRecord_R_triggered() {
  if (ui->stackedWidget_main->currentIndex() != 0) {
    QMessageBox::warning(this, tr("Warning"), tr("Cannot be executed in database mode."));
    return;
  }

  if (!proxy_->is_connected()) {
    QMessageBox::warning(this, tr("Warning"), tr("Proxy is disconnected."));
    return;
  }

  if (ui->treeWidget_url->currentItem()) {
    ui->treeWidget_url->currentItem()->setText(2, "---");
    ui->treeWidget_url->currentItem()->setText(3, "---");
    ui->treeWidget_url->currentItem()->setText(4, "---");
    ui->treeWidget_url->currentItem()->setText(5, "---");
    update_status_bar(-1, -1, -1, -1, -1);
    status_label2_->clear();
    ui->label_graph_value2->setText("---");

    clear_all_process_item();
  }

  ui->groupBox_url->setEnabled(false);
  ui->groupBox_status->setEnabled(false);
  ui->groupBox_proto->setEnabled(false);
  ui->widget_freq->clear();
  ui->widget_rate->clear();
  ui->widget_loss->clear();
  ui->widget_lat->clear();

  clear_all_property_item(ui->treeWidget_property);
  ui->pushButton_analyze->setEnabled(false);

  send_control(vlink::ProxyAPI::kAuto, false);

  is_in_model_ = true;
  RecordDialog dialog(this);
  dialog.exec();
  is_in_model_ = false;

  update_proto_ser();
  ui->groupBox_url->setEnabled(proxy_->is_connected());
  ui->groupBox_status->setEnabled(proxy_->is_connected());
  ui->groupBox_proto->setEnabled(proxy_->is_connected());

  send_control(ui->checkBox_observe->isChecked() ? vlink::ProxyAPI::kObserveAll : vlink::ProxyAPI::kObserveOne, true);
}

void MainWindow::on_actionPlay_P_triggered() {
  if (proxy_->get_current_config().role != vlink::ProxyAPI::kController) {
    QMessageBox::warning(this, tr("Warning"), tr("Listener role cannot be play."));
    return;
  }

  if (ui->stackedWidget_main->currentIndex() != 0) {
    QMessageBox::warning(this, tr("Warning"), tr("Cannot be executed in database mode."));
    return;
  }

  if (!proxy_->is_connected()) {
    QMessageBox::warning(this, tr("Warning"), tr("Proxy is disconnected."));
    return;
  }

  if (ui->treeWidget_url->currentItem()) {
    ui->treeWidget_url->currentItem()->setText(2, "---");
    ui->treeWidget_url->currentItem()->setText(3, "---");
    ui->treeWidget_url->currentItem()->setText(4, "---");
    ui->treeWidget_url->currentItem()->setText(5, "---");
    update_status_bar(-1, -1, -1, -1, -1);
    status_label2_->clear();
    ui->label_graph_value2->setText("---");

    clear_all_process_item();
  }

  ui->groupBox_url->setEnabled(false);
  ui->groupBox_status->setEnabled(false);
  ui->groupBox_proto->setEnabled(false);
  ui->widget_freq->clear();
  ui->widget_rate->clear();
  ui->widget_loss->clear();
  ui->widget_lat->clear();

  clear_all_property_item(ui->treeWidget_property);
  ui->pushButton_analyze->setEnabled(false);

  send_control(vlink::ProxyAPI::kPlay, false);

  is_in_model_ = true;
  PlayDialog dialog(this);
  dialog.exec();
  is_in_model_ = false;

  update_proto_ser();
  ui->groupBox_url->setEnabled(proxy_->is_connected());
  ui->groupBox_status->setEnabled(proxy_->is_connected());
  ui->groupBox_proto->setEnabled(proxy_->is_connected());

  send_control(ui->checkBox_observe->isChecked() ? vlink::ProxyAPI::kObserveAll : vlink::ProxyAPI::kObserveOne, true);
}

void MainWindow::on_actionEdit_E_triggered() {
  if (is_zero_copy_types_) {
    QMessageBox::warning(this, tr("Warning"), tr("Zero-Copy types cannot be edit."));
    return;
  }

  if (proxy_->get_current_config().role != vlink::ProxyAPI::kController) {
    QMessageBox::warning(this, tr("Warning"), tr("Listener role cannot be edit."));
    return;
  }

  if (ui->stackedWidget_main->currentIndex() != 0) {
    QMessageBox::warning(this, tr("Warning"), tr("Cannot be executed in database mode."));
    return;
  }

  if (!proxy_->is_connected()) {
    QMessageBox::warning(this, tr("Warning"), tr("Proxy is disconnected."));
    return;
  }

  if (ui->treeWidget_url->currentItem()) {
    ui->treeWidget_url->currentItem()->setText(2, "---");
    ui->treeWidget_url->currentItem()->setText(3, "---");
    ui->treeWidget_url->currentItem()->setText(4, "---");
    ui->treeWidget_url->currentItem()->setText(5, "---");
    update_status_bar(-1, -1, -1, -1, -1);
    status_label2_->clear();
    ui->label_graph_value2->setText("---");

    clear_all_process_item();
  }

  ui->groupBox_url->setEnabled(false);
  ui->groupBox_status->setEnabled(false);
  ui->groupBox_proto->setEnabled(false);
  ui->widget_freq->clear();
  ui->widget_rate->clear();
  ui->widget_loss->clear();
  ui->widget_lat->clear();

  clear_all_property_item(ui->treeWidget_property);
  ui->pushButton_analyze->setEnabled(false);

  send_control(vlink::ProxyAPI::kEdit, true);

  is_in_model_ = true;
  EditDialog dialog(this);
  dialog.exec();
  is_in_model_ = false;

  update_proto_ser();
  ui->groupBox_url->setEnabled(proxy_->is_connected());
  ui->groupBox_status->setEnabled(proxy_->is_connected());
  ui->groupBox_proto->setEnabled(proxy_->is_connected());

  send_control(ui->checkBox_observe->isChecked() ? vlink::ProxyAPI::kObserveAll : vlink::ProxyAPI::kObserveOne, true);
}

void MainWindow::on_actionRaw_J_triggered() {
  if (ui->stackedWidget_main->currentIndex() != 0) {
    QMessageBox::warning(this, tr("Warning"), tr("Cannot be executed in database mode."));
    return;
  }

  if (!proxy_->is_connected()) {
    QMessageBox::warning(this, tr("Warning"), tr("Proxy is disconnected."));
    return;
  }

  if (ui->treeWidget_url->currentItem()) {
    auto rate = ui->treeWidget_url->currentItem()->data(3, Qt::UserRole).toULongLong();
    if (rate > 1024 * 1024) {
      QMessageBox message_box(QMessageBox::Warning, tr("Warning"),
                              tr("The bandwidth of raw data is very large. Do you want to continue?"),
                              QMessageBox::Yes | QMessageBox::No, this);

      int result = message_box.exec();

      if (result != QMessageBox::Yes) {
        return;
      }
    }

    ui->treeWidget_url->currentItem()->setText(2, "---");
    ui->treeWidget_url->currentItem()->setText(3, "---");
    ui->treeWidget_url->currentItem()->setText(4, "---");
    ui->treeWidget_url->currentItem()->setText(5, "---");
    update_status_bar(-1, -1, -1, -1, -1);
    status_label2_->clear();
    ui->label_graph_value2->setText("---");

    clear_all_process_item();
  }

  ui->groupBox_url->setEnabled(false);
  ui->groupBox_status->setEnabled(false);
  ui->groupBox_proto->setEnabled(false);
  ui->widget_freq->clear();
  ui->widget_rate->clear();
  ui->widget_loss->clear();
  ui->widget_lat->clear();

  send_control(vlink::ProxyAPI::kAuto, true);

  is_in_model_ = true;
  RawDialog dialog(this);
  dialog.exec();
  is_in_model_ = false;

  update_proto_ser();
  ui->groupBox_url->setEnabled(proxy_->is_connected());
  ui->groupBox_status->setEnabled(proxy_->is_connected());
  ui->groupBox_proto->setEnabled(proxy_->is_connected());

  send_control(ui->checkBox_observe->isChecked() ? vlink::ProxyAPI::kObserveAll : vlink::ProxyAPI::kObserveOne, true);
}

void MainWindow::on_actionCamera_S_triggered() {
  if (ui->stackedWidget_main->currentIndex() != 0) {
    QMessageBox::warning(this, tr("Warning"), tr("Cannot be executed in database mode."));
    return;
  }

  if (!proxy_->is_connected()) {
    QMessageBox::warning(this, tr("Warning"), tr("Proxy is disconnected."));
    return;
  }

  if (ui->treeWidget_url->currentItem()) {
    ui->treeWidget_url->currentItem()->setText(2, "---");
    ui->treeWidget_url->currentItem()->setText(3, "---");
    ui->treeWidget_url->currentItem()->setText(4, "---");
    ui->treeWidget_url->currentItem()->setText(5, "---");
    update_status_bar(-1, -1, -1, -1, -1);
    status_label2_->clear();
    ui->label_graph_value2->setText("---");

    clear_all_process_item();
  }

  ui->groupBox_url->setEnabled(false);
  ui->groupBox_status->setEnabled(false);
  ui->groupBox_proto->setEnabled(false);
  ui->widget_freq->clear();
  ui->widget_rate->clear();
  ui->widget_loss->clear();
  ui->widget_lat->clear();

  send_control(vlink::ProxyAPI::kAuto, true);

  is_in_model_ = true;
  CameraDialog dialog;
  qApp->processEvents();
  // this->hide();
  // dialog.setWindowState(Qt::WindowMaximized);
  dialog.exec();
  // this->show();
  is_in_model_ = false;

  update_proto_ser();
  ui->groupBox_url->setEnabled(proxy_->is_connected());
  ui->groupBox_status->setEnabled(proxy_->is_connected());
  ui->groupBox_proto->setEnabled(proxy_->is_connected());

  send_control(ui->checkBox_observe->isChecked() ? vlink::ProxyAPI::kObserveAll : vlink::ProxyAPI::kObserveOne, true);
}

void MainWindow::on_actionPoint3D_Z_triggered() {
  if (ui->stackedWidget_main->currentIndex() != 0) {
    QMessageBox::warning(this, tr("Warning"), tr("Cannot be executed in database mode."));
    return;
  }

  if (!proxy_->is_connected()) {
    QMessageBox::warning(this, tr("Warning"), tr("Proxy is disconnected."));
    return;
  }

  if (ui->treeWidget_url->currentItem()) {
    ui->treeWidget_url->currentItem()->setText(2, "---");
    ui->treeWidget_url->currentItem()->setText(3, "---");
    ui->treeWidget_url->currentItem()->setText(4, "---");
    ui->treeWidget_url->currentItem()->setText(5, "---");
    update_status_bar(-1, -1, -1, -1, -1);
    status_label2_->clear();
    ui->label_graph_value2->setText("---");
    clear_all_process_item();
  }

  ui->groupBox_url->setEnabled(false);
  ui->groupBox_status->setEnabled(false);
  ui->groupBox_proto->setEnabled(false);
  ui->widget_freq->clear();
  ui->widget_rate->clear();
  ui->widget_loss->clear();
  ui->widget_lat->clear();

  send_control(vlink::ProxyAPI::kAuto, true);

  is_in_model_ = true;
  Point3DDialog dialog;
  qApp->processEvents();
  // this->hide();
  // dialog.setWindowState(Qt::WindowMaximized);
  dialog.exec();
  // this->show();
  is_in_model_ = false;

  update_proto_ser();
  ui->groupBox_url->setEnabled(proxy_->is_connected());
  ui->groupBox_status->setEnabled(proxy_->is_connected());
  ui->groupBox_proto->setEnabled(proxy_->is_connected());

  send_control(ui->checkBox_observe->isChecked() ? vlink::ProxyAPI::kObserveAll : vlink::ProxyAPI::kObserveOne, true);
}

void MainWindow::on_actionPerception_triggered() {
  if (ui->stackedWidget_main->currentIndex() != 0) {
    QMessageBox::warning(this, tr("Warning"), tr("Cannot be executed in database mode."));
    return;
  }

  if (!proxy_->is_connected()) {
    QMessageBox::warning(this, tr("Warning"), tr("Proxy is disconnected."));
    return;
  }

  if (ui->treeWidget_url->currentItem()) {
    ui->treeWidget_url->currentItem()->setText(2, "---");
    ui->treeWidget_url->currentItem()->setText(3, "---");
    ui->treeWidget_url->currentItem()->setText(4, "---");
    ui->treeWidget_url->currentItem()->setText(5, "---");
    update_status_bar(-1, -1, -1, -1, -1);
    status_label2_->clear();
    ui->label_graph_value2->setText("---");
    clear_all_process_item();
  }

  ui->groupBox_url->setEnabled(false);
  ui->groupBox_status->setEnabled(false);
  ui->groupBox_proto->setEnabled(false);
  ui->widget_freq->clear();
  ui->widget_rate->clear();
  ui->widget_loss->clear();
  ui->widget_lat->clear();

  send_control(vlink::ProxyAPI::kAuto, true);

  is_in_model_ = true;
  PerceptionDialog dialog;
  qApp->processEvents();
  dialog.exec();
  is_in_model_ = false;

  update_proto_ser();
  ui->groupBox_url->setEnabled(proxy_->is_connected());
  ui->groupBox_status->setEnabled(proxy_->is_connected());
  ui->groupBox_proto->setEnabled(proxy_->is_connected());

  send_control(ui->checkBox_observe->isChecked() ? vlink::ProxyAPI::kObserveAll : vlink::ProxyAPI::kObserveOne, true);
}

void MainWindow::on_actionMap_G_triggered() {
  // is_in_model_ = true;
  this->message_box_todo(this);
  // is_in_model_ = false;
}

void MainWindow::on_actionQuit_Q_triggered() { this->close(); }

void MainWindow::on_actionAboutQt_A_triggered() { QMessageBox::aboutQt(this); }

void MainWindow::on_actionAbout_this_S_triggered() {
  // is_in_model_ = true;
  AboutDialog dialog(this);
  dialog.exec();
  // is_in_model_ = false;
}

void MainWindow::on_actionHow_to_use_U_triggered() {
  open_url("http://172.16.2.225:8090/pages/viewpage.action?pageId=162179006");
}

void MainWindow::on_actionBug_Report_B_triggered() {
  open_url("http://172.16.2.225:8090/pages/viewpage.action?pageId=162179008");
}

void MainWindow::on_actionDownload_L_triggered() { open_url("https://vlink.work/official_releases/"); }

void MainWindow::on_actionDB_Browser_W_triggered() { open_url("https://sqlitebrowser.org"); }

void MainWindow::on_actionProtobuf_Decoder_F_triggered() { open_url("https://protobuf-decoder.netlify.app"); }

void MainWindow::on_actionCommunication_Matrix_M_triggered() {
  open_url("http://172.16.2.225:8090/pages/viewpage.action?pageId=162179012");
}

void MainWindow::on_actionStatus_Viewer_triggered(bool checked) {
  ui->groupBox_status->setVisible(checked);
  QMetaObject::invokeMethod(this, "adjust_size", Qt::QueuedConnection);
}

void MainWindow::on_actionProto_Viewer_triggered(bool checked) {
  ui->groupBox_proto->setVisible(checked);
  QMetaObject::invokeMethod(this, "adjust_size", Qt::QueuedConnection);
}

void MainWindow::on_pushButton_protoselect_clicked() {
  QFileDialog dialog(this, tr("Select proto dir"), QString::fromStdString(source_dir_));

#if QT_VERSION < QT_VERSION_CHECK(5, 15, 0) || defined(Q_OS_LINUX)
  dialog.setOption(QFileDialog::DontUseNativeDialog, true);
#endif

  dialog.setFileMode(QFileDialog::Directory);
  dialog.setAcceptMode(QFileDialog::AcceptOpen);

  if (dialog.exec() != QDialog::Accepted) {
    return;
  }

  QString file_dir = dialog.selectedFiles().constFirst();

  select_source_dir(file_dir.toStdString());

  QFile file(global_proto_dir_config);

  if (file.open(QFile::WriteOnly | QFile::Truncate)) {
    file.write(file_dir.toUtf8());
    file.close();
  }
}

void MainWindow::on_pushButton_protoreload_clicked() {
  if (!source_dir_.empty()) {
    select_source_dir(source_dir_);

    QString file_dir = ui->lineEdit_protodir->text();

    QFile file(global_proto_dir_config);
    if (file.open(QFile::WriteOnly | QFile::Truncate)) {
      file.write(file_dir.toUtf8());
      file.close();
    }
  }
}

void MainWindow::on_pushButton_fbsselect_clicked() {
  QFileDialog dialog(this, tr("Select fbs dir"), QString::fromStdString(fbs_dir_));

#if QT_VERSION < QT_VERSION_CHECK(5, 15, 0) || defined(Q_OS_LINUX)
  dialog.setOption(QFileDialog::DontUseNativeDialog, true);
#endif

  dialog.setFileMode(QFileDialog::Directory);
  dialog.setAcceptMode(QFileDialog::AcceptOpen);

  if (dialog.exec() != QDialog::Accepted) {
    return;
  }

  QString file_dir = dialog.selectedFiles().constFirst();

  if (!select_fbs_dir(file_dir.toStdString())) {
    return;
  }

  QFile file(global_fbs_dir_config);

  if (file.open(QFile::WriteOnly | QFile::Truncate)) {
    file.write(file_dir.toUtf8());
    file.close();
  }
}

void MainWindow::on_pushButton_fbsreload_clicked() {
  if (!fbs_dir_.empty()) {
    if (!select_fbs_dir(fbs_dir_)) {
      return;
    }

    QString file_dir = ui->lineEdit_fbsdir->text();

    QFile file(global_fbs_dir_config);
    if (file.open(QFile::WriteOnly | QFile::Truncate)) {
      file.write(file_dir.toUtf8());
      file.close();
    }
  }
}

void MainWindow::on_checkBox_showfreq_clicked(bool checked) {
  ui->treeWidget_url->setColumnHidden(2, !checked);
  adjust_size();
}

void MainWindow::on_checkBox_showrate_clicked(bool checked) {
  ui->treeWidget_url->setColumnHidden(3, !checked);
  adjust_size();
}

void MainWindow::on_checkBox_showloss_clicked(bool checked) {
  ui->treeWidget_url->setColumnHidden(4, !checked);
  adjust_size();
}

void MainWindow::on_checkBox_showlantency_clicked(bool checked) {
  ui->treeWidget_url->setColumnHidden(5, !checked);
  adjust_size();
}

void MainWindow::on_checkBox_view_clicked(bool checked) {
  ui->checkBox_hex->setEnabled(checked);
  ui->checkBox_enum->setEnabled(checked);
  ui->checkBox_time->setEnabled(checked);

  (void)checked;
  update_proto_ser();
}

void MainWindow::on_checkBox_perf_clicked(bool checked) { (void)checked; }

void MainWindow::on_checkBox_array_clicked(bool checked) {
  (void)checked;
  update_proto_ser();
}

void MainWindow::on_checkBox_hex_clicked(bool checked) {
  (void)checked;
  update_proto_ser();
}

void MainWindow::on_checkBox_enum_clicked(bool checked) {
  (void)checked;
  update_proto_ser();
}

void MainWindow::on_checkBox_time_clicked(bool checked) {
  (void)checked;
  update_proto_ser();
}

void MainWindow::on_pushButton_analyze_clicked() {
  auto current_item = ui->treeWidget_property->currentItem();

  if (!current_item) {
    return;
  }

  int type = current_item->data(1, Qt::UserRole).toInt();

  if (type == AnalyzeDialog::kNumberType) {
    analyze_dialog_->init(AnalyzeDialog::kNumberType);
  } else if (type == AnalyzeDialog::kStringType) {
    analyze_dialog_->init(AnalyzeDialog::kStringType);
  } else if (type == AnalyzeDialog::kRawType) {
    analyze_dialog_->init(AnalyzeDialog::kRawType);
  }
}

void MainWindow::on_pushButton_dataselect_clicked() {
  QSettings settings(QStandardPaths::writableLocation(QStandardPaths::CacheLocation) + "/settings.ini",
                     QSettings::IniFormat);

  settings.beginGroup("MainWindow");

  QFileDialog dialog(this, tr("Select bag file"), settings.value("bag_dir", qApp->applicationDirPath()).toString(),
                     "Bag files (*.vdb)");

  settings.endGroup();

#if QT_VERSION < QT_VERSION_CHECK(5, 15, 0)
  dialog.setOption(QFileDialog::DontUseNativeDialog, true);
#endif

  dialog.setFileMode(QFileDialog::ExistingFile);
  dialog.setAcceptMode(QFileDialog::AcceptOpen);
  dialog.setDefaultSuffix("vdb");

  if (dialog.exec() != QDialog::Accepted) {
    return;
  }

  QString file_path = dialog.selectedFiles().constFirst();

  if (file_path.isEmpty()) {
    return;
  }

  settings.beginGroup("MainWindow");
  settings.setValue("bag_dir", QFileInfo(file_path).dir().path());
  settings.endGroup();
  settings.sync();

  ui->lineEdit_datapath->setText(file_path);

  on_pushButton_datareload_clicked();

  ui->comboBox_jump->setEnabled(true);
  ui->lineEdit_jump->setEnabled(true);
  ui->pushButton_jump->setEnabled(true);
  ui->pushButton_datareload->setEnabled(true);
  ui->pushButton_datadetails->setEnabled(true);
}

void MainWindow::on_pushButton_datareload_clicked() {
  try {
    auto player = vlink::BagReader::create(ui->lineEdit_datapath->text().toStdString(), true);
    local_info_ = player->get_info();
    local_use_compress_ = (local_info_.compression_type != "None");
    ser_map_.clear();
    schema_type_map_.clear();
    ser_map_.reserve(local_info_.url_metas.size());
    schema_type_map_.reserve(local_info_.url_metas.size());

    for (const auto& meta : local_info_.url_metas) {
      ser_map_[meta.url] = meta.ser_type;
      schema_type_map_[meta.url] = vlink::SchemaData::resolve_type(meta.schema_type, meta.ser_type);
    }
  } catch (std::exception&) {
    QMessageBox::warning(this, tr("Warning"), tr("Cannot open database."));
    return;
  }

  local_database_.setDatabaseName(ui->lineEdit_datapath->text());

  if (!local_database_.open()) {
    QMessageBox::warning(this, tr("Warning"), tr("Cannot open database."));
    return;
  }

  auto* sql_model = static_cast<CustomSqlQueryModel*>(ui->tableView_data->model());

  sql_model->setQuery(R"(
        SELECT VLinkDatas.elapsed, VLinkUrls.url, VLinkUrls.ser, VLinkDatas.data FROM VLinkDatas LEFT JOIN VLinkUrls ON VLinkDatas.url = VLinkUrls.id;
    )");

  if (sql_model->lastError().isValid()) {
    QMessageBox::warning(this, tr("Warning"), sql_model->lastError().text());
    return;
  }

  ui->tableView_data->setSelectionBehavior(QTableView::SelectRows);
  ui->tableView_data->setSelectionMode(QTableView::SingleSelection);
  ui->tableView_data->hideColumn(2);
  ui->tableView_data->hideColumn(3);
  ui->tableView_data->hideColumn(4);
  ui->tableView_data->hideColumn(5);

  adjust_size();
}

void MainWindow::on_pushButton_datadetails_clicked() {
  QString file_name = tr("FileName: ") + QString::fromUtf8(local_info_.file_name.c_str());
  QString file_size = tr("File Size: ");
  QString tag_name = tr("Tag Name: ") + QString::fromUtf8(local_info_.tag_name.c_str());
  QString version = tr("Version: ") + QString::fromStdString(local_info_.version);
  QString compression_type = tr("Compression: ") + QString::fromStdString(local_info_.compression_type);
  QString process_name = tr("Process Name: ") + QString::fromUtf8(local_info_.process_name.c_str());

  QString flags_str;

  if (local_info_.has_completed) {
    flags_str.append("completed |");
  }

  if (local_info_.has_idx_elapsed) {
    flags_str.append("idx_elapsed | ");
  }

  if (local_info_.has_idx_url) {
    flags_str.append("idx_url | ");
  }

  if (local_info_.has_schema) {
    flags_str.append("schema | ");
  }

  if (flags_str.size() >= 3) {
    flags_str.chop(3);
  }

  flags_str = tr("Meta Flags: ") + flags_str;

  QString date_time = tr("Date Time: ") + QString::fromStdString(local_info_.date_time);
  QString duration =
      tr("Duration: ") + QString::fromStdString(vlink::Helpers::format_milliseconds(local_info_.blank_duration, true)) +
      " ~ " + QString::fromStdString(vlink::Helpers::format_milliseconds(local_info_.total_duration, true));

  if (local_info_.file_size < 1024LL * 1024) {
    file_size += QString::number(local_info_.file_size / 1024.0F, 'f', 2) + "KB";
  } else if (local_info_.file_size < 1024LL * 1024 * 1024) {
    file_size += QString::number(local_info_.file_size / 1024 / 1024.0F, 'f', 2) + "MB";
  } else {
    file_size += QString::number(local_info_.file_size / 1024 / 1024 / 1024.0F, 'f', 2) + "GB";
  }

  QString message_count = tr("Message Count: ") + QString::number(local_info_.message_count);

  QString split_count = tr("Spilt Count: ") + "---";

  if (local_info_.split_count > 0) {
    if (local_info_.split_by_time > 0) {
      split_count = tr("Spilt Count: ") + QString::number(local_info_.split_count) +
                    tr(" (by time: %1s)").arg(QString::number(local_info_.split_by_time / 1000.0, 'f', 2));
    } else if (local_info_.split_by_size > 0) {
      split_count =
          tr("Spilt Count: ") + QString::number(local_info_.split_count) +
          tr(" (by size: %1GB)").arg(QString::number(local_info_.split_by_size / 1024.0 / 1024.0 / 1024.0, 'f', 2));
    }
  }

  QString all_message = file_name + "\n" + file_size + "\n" + tag_name + "\n" + version + "\n" + compression_type +
                        "\n" + process_name + "\n" + date_time + "\n" + duration + "\n" + message_count + "\n" +
                        flags_str + "\n" + split_count;

  QMessageBox::information(this, tr("Details"), all_message);
}

void MainWindow::on_pushButton_jump_clicked() {
  QString value = ui->lineEdit_jump->text();
  switch (ui->comboBox_jump->currentIndex()) {
    case 0: {
      int row = value.toInt() - 1;

      if (row >= 0 && row < local_info_.message_count) {
        ui->tableView_data->scrollTo(ui->tableView_data->model()->index(row, 0), QAbstractItemView::PositionAtCenter);
        ui->tableView_data->selectRow(row);
      } else {
        QMessageBox::warning(this, tr("Warning"), tr("Invalid row index."));
        return;
      }
    } break;
    case 1: {
      int left = 0;
      int right = local_info_.message_count - 1;
      int row_index = -1;
      uint64_t target_elapsed = value.toDouble() * 1000'000;
      while (left <= right) {
        uint64_t mid = left + (right - left) / 2;
        uint64_t current_elapsed =
            ui->tableView_data->model()->data(ui->tableView_data->model()->index(mid, 0), Qt::UserRole).toULongLong();

        if (current_elapsed >= target_elapsed) {
          row_index = mid;
          right = mid - 1;
        } else {
          left = mid + 1;
        }
      }

      if (row_index > 0) {
        --row_index;
      }

      if (row_index != -1) {
        ui->tableView_data->scrollTo(ui->tableView_data->model()->index(row_index, 0),
                                     QAbstractItemView::PositionAtCenter);
        ui->tableView_data->selectRow(row_index);
      } else {
        QMessageBox::warning(this, tr("Warning"), tr("Invalid seconds."));
      }
    } break;
    default:
      break;
  }
}

void MainWindow::update_connected(bool connected) {
  if (proxy_->is_ready_to_quit()) {
    return;
  }

  emit connect_changed(connected);

  ui->widget_freq->clear();
  ui->widget_rate->clear();
  ui->widget_loss->clear();
  ui->widget_lat->clear();

  ui->label_graph_value2->setText("---");

  ui->lineEdit_protoser->clear();

  if (ui->stackedWidget_main->currentIndex() == 0) {
    ui->groupBox_url->setEnabled(connected);
    ui->groupBox_status->setEnabled(connected);
    ui->groupBox_proto->setEnabled(connected);
  }

  ui->actionLocal_D->setEnabled(true);
  ui->actionTopology_N->setEnabled(connected);
  ui->actionRecord_R->setEnabled(connected);
  ui->actionPlay_P->setEnabled(connected);
  ui->actionEdit_E->setEnabled(false);
  ui->actionRaw_J->setEnabled(false);
  ui->actionCamera_S->setEnabled(false);
  ui->actionPoint3D_Z->setEnabled(false);
  ui->actionPerception->setEnabled(false);
  ui->actionMap_G->setEnabled(false);

  clear_all_url_item();

  if (connected) {
    if (proxy_->get_current_config().role == vlink::ProxyAPI::kController) {
      ui->label_status->setStyleSheet("QLabel { background-color: green; color: white; }");
    } else {
      ui->label_status->setStyleSheet("QLabel { background-color: blue; color: white; }");
    }

    ui->label_status->setText(tr("Proxy is connected."));
    // ui->treeWidget_url->setFocus();

    foreach (auto* widget, QApplication::topLevelWidgets()) {
      auto* msg_box = qobject_cast<QMessageBox*>(widget);

      if (msg_box) {
        if (msg_box->icon() == QMessageBox::Warning || msg_box->icon() == QMessageBox::Critical) {
          msg_box->close();
        }
      }
    }

    is_show_warn_ = false;

  } else {
    if (proxy_->get_current_config().role == vlink::ProxyAPI::kController) {
      ui->label_status->setStyleSheet("QLabel { background-color: red; color: white; }");
    } else {
      ui->label_status->setStyleSheet("QLabel { background-color: #996633; color: white; }");
    }

    ui->label_status->setText(tr("Proxy is disconnected."));
    update_status_bar(-1, -1, -1, -1, -1);
    status_label2_->clear();
  }
}

void MainWindow::update_error(int error) {
  // if (!proxy_->is_connected()) {
  //   return;
  // }

  if (is_show_warn_) {
    return;
  }

  if (proxy_->get_current_error() == vlink::ProxyAPI::kControlError) {
    is_show_warn_ = true;
    QMessageBox::warning(this, tr("Warning"), tr("Another control started, current loss of control."));
  } else if (error == vlink::ProxyAPI::kModeError) {
    is_show_warn_ = true;
    QMessageBox::warning(this, tr("Warning"), tr("The current mode does not match the target mode."));
  } else if (error == vlink::ProxyAPI::kReliableCompError) {
    is_show_warn_ = true;
    QMessageBox::warning(this, tr("Warning"), tr("Reliable mode is incompatible."));
  } else if (error == vlink::ProxyAPI::kTcpCompError) {
    is_show_warn_ = true;
    QMessageBox::warning(this, tr("Warning"), tr("Tcp mode is incompatible."));
  } else if (error == vlink::ProxyAPI::kDirectCompError) {
    is_show_warn_ = true;
    QMessageBox::warning(this, tr("Warning"), tr("Direct mode is incompatible."));
  } else if (error == vlink::ProxyAPI::kMultiProxyError) {
    auto proxy_hostname_list = proxy_->get_proxy_hostnames();

    QString hostname_info;

    for (const auto& hostname : proxy_hostname_list) {
      hostname_info.append(QString::fromUtf8(hostname.c_str()));
      hostname_info.append("\n");
    }

    is_show_warn_ = true;
    QMessageBox::warning(this, tr("Warning"),
                         tr("Multiple proxy servers or proxy identity mismatch detected:\n%1").arg(hostname_info));
  } else if (error == vlink::ProxyAPI::kVersionCompError) {
    QString proxy_version = QString::fromStdString(proxy_->get_proxy_version());
    QString viewer_version = VLINK_VERSION;

    if (proxy_version.isEmpty()) {
      proxy_version = tr("Unknown");
    }

    is_show_warn_ = true;
    QMessageBox::warning(this, tr("Warning"),
                         tr("The viewer and proxy versions do not match.\nProxy version: [%1]\nViewer version: [%2]")
                             .arg(proxy_version, viewer_version));
  } else if (error == vlink::ProxyAPI::kTokenError) {
    is_show_warn_ = true;
    QMessageBox::warning(this, tr("Warning"),
                         tr("The proxy authentication handshake failed or same-server token does not match."));
  }
}

void MainWindow::update_time() {
  if (!proxy_->is_connected()) {
    ui->label_host2->setText("---");
    ui->label_systime2->setText("---");
    ui->label_boottime2->setText("---");
    ui->label_resource2->setText("---");
    return;
  }

  ui->label_host2->setText(QString::fromUtf8(proxy_->get_current_hostname().c_str()));

  std::string sys_time_str = vlink::ProxyAPI::get_format_sys_time(proxy_->get_current_sys_time());

  std::string boot_time_str = vlink::ProxyAPI::get_format_boot_time(proxy_->get_current_boot_time());

  ui->label_systime2->setText(QString::fromStdString(sys_time_str));
  ui->label_boottime2->setText(QString::fromStdString(boot_time_str));

  if (proxy_->get_current_cpu_usage() < 0 || proxy_->get_current_memory_usage() < 0) {
    ui->label_resource2->setEnabled(false);
    ui->label_resource2->setText("---");
  } else {
    ui->label_resource2->setEnabled(true);
    ui->label_resource2->setText(tr("CPU: %1% | MEM: %2%")
                                     .arg(QString::number(proxy_->get_current_cpu_usage(), 'f', 2),
                                          QString::number(proxy_->get_current_memory_usage(), 'f', 2)));
  }
}

void MainWindow::update_url_widget(const QVariant& variant) {
  if (proxy_->is_ready_to_quit()) {
    return;
  }

  const auto& info_list = variant.value<std::vector<vlink::ProxyAPI::Info>>();

  const auto& selected_items = ui->treeWidget_url->selectedItems();

  for (int i = 0; i < ui->treeWidget_url->topLevelItemCount(); ++i) {
    auto* p = ui->treeWidget_url->topLevelItem(i);
    bool find = false;
    for (const auto& info : info_list) {
      if (p->text(1) == QString::fromStdString(info.url)) {
        find = true;
        break;
      }
    }

    if (!find) {
      QTreeWidgetItem* current_item = ui->treeWidget_url->currentItem();
      // ui->treeWidget_url->blockSignals(true);
      QTreeWidgetItem* item = ui->treeWidget_url->takeTopLevelItem(i);
      // ui->treeWidget_url->blockSignals(false);

      if (item == current_item) {
        ui->widget_freq->clear();
        ui->widget_rate->clear();
        ui->widget_loss->clear();
        ui->widget_lat->clear();
        ui->label_graph_value2->setText("---");
        ui->treeWidget_url->setCurrentItem(nullptr);
        update_proto_ser();
      }
      delete item;
      --i;
    }
  }

  can_record_urls_.clear();
  can_record_urls_.reserve(info_list.size());
  ser_map_.clear();
  schema_type_map_.clear();
  process_map_.clear();

  int active_count = 0;
  int64_t total_rate = 0;

  float agg_freq = 0;
  uint64_t agg_rate = 0;
  float agg_loss = 0;
  float agg_latency = 0;
  int agg_count = 0;

  for (size_t m = 0; m < info_list.size(); ++m) {
    QTreeWidgetItem* item = nullptr;

    for (int n = 0; n < ui->treeWidget_url->topLevelItemCount(); ++n) {
      auto* p = ui->treeWidget_url->topLevelItem(n);

      if (p->text(1) == QString::fromStdString(info_list[m].url)) {
        item = p;
      }
    }

    if (!item) {
      item = new QTreeWidgetItem;

      {
        QFont font = item->font(0);
        font.setFamily("Noto Mono");
        font.setBold(true);
        item->setFont(0, font);
      }

      // item->setTextAlignment(0, Qt::AlignVCenter | Qt::AlignHCenter);
      // item->setTextAlignment(1, Qt::AlignVCenter | Qt::AlignLeft);
      // item->setTextAlignment(2, Qt::AlignVCenter | Qt::AlignRight);
      // item->setTextAlignment(3, Qt::AlignVCenter | Qt::AlignRight);
      // item->setTextAlignment(4, Qt::AlignVCenter | Qt::AlignHCenter);

      // ui->treeWidget_url->addTopLevelItem(item);

      ui->treeWidget_url->insertTopLevelItem(m, item);
    }

    const auto& type = info_list.at(m).type;
    const auto& url = info_list.at(m).url;
    const auto& ser = info_list.at(m).ser;
    auto status = info_list.at(m).status;
    auto freq = info_list.at(m).freq;
    auto rate = info_list.at(m).rate;
    auto loss = info_list.at(m).loss;
    auto latency = info_list.at(m).latency;
    const auto& process_list = info_list.at(m).process_list;

    if (loss > 1) {
      loss = 0;
    }

    if (status == vlink::ProxyAPI::kActive) {
      ++active_count;
    }

    total_rate += rate;

    ser_map_[url] = ser;
    schema_type_map_[url] = vlink::SchemaData::resolve_type(info_list.at(m).schema, ser);

    process_map_[url] = process_list;
    item->setText(0, QString::fromStdString(vlink::DiscoveryViewer::convert_type_to_view(type)));
    item->setData(0, Qt::UserRole, type);
    item->setText(1, QString::fromStdString(url));
    item->setData(1, Qt::UserRole, QString::fromStdString(ser));

    // item->setTextAlignment(2, Qt::AlignRight | Qt::AlignVCenter);
    item->setText(2, QString::number(freq, 'f', 2) + "Hz");

    // item->setTextAlignment(3, Qt::AlignRight | Qt::AlignVCenter);

    if (rate < 1024) {
      item->setText(3, QString::number(rate) + "B/s");
    } else if (rate < 1024LL * 1024) {
      item->setText(3, QString::number(rate / 1024.0, 'f', 2) + "KB/s");
    } else if (rate < 1024LL * 1024 * 1024) {
      item->setText(3, QString::number(rate / 1024.0 / 1024.0, 'f', 2) + "MB/s");
    } else {
      item->setText(3, QString::number(rate / 1024.0 / 1024.0 / 1024.0, 'f', 2) + "GB/s");
    }

    item->setData(3, Qt::UserRole, static_cast<quint64>(rate));

    // item->setTextAlignment(4, Qt::AlignRight | Qt::AlignVCenter);
    item->setText(4, QString::number(loss * 100, 'f', 2) + "%");

    if (latency <= -2) {
      item->setText(5, "N/A");
      latency = 0;
    } else if (latency <= -1) {
      item->setText(5, "---");
      latency = 0;
    } else if (latency < 0) {
      item->setText(5, "0 ms");
      latency = 0;
    } else {
      item->setText(5, QString::number(latency, 'f', 2) + "ms");
    }

    if (ui->checkBox_observe->isChecked() || selected_items.contains(item)) {
      if (selected_items.count() > 1 && selected_items.contains(item)) {
        agg_freq += freq;
        agg_rate += rate;
        agg_loss += freq * loss;
        agg_latency += freq * latency;
        ++agg_count;
      } else if (ui->treeWidget_url->currentItem() == item && selected_items.count() <= 1) {
        if (desc_ || (is_flatbuffers_types_ && current_fbs_context_ && current_fbs_context_->valid())) {
          ui->actionEdit_E->setEnabled(true);
        } else {
          ui->actionEdit_E->setEnabled(false);
        }

        if ((type & vlink::kPublisher) || (type & vlink::kSetter)) {
          ui->actionMap_G->setEnabled(true);
          ui->actionPoint3D_Z->setEnabled(true);
          ui->actionPerception->setEnabled(true);
          ui->actionCamera_S->setEnabled(true);
          ui->actionRaw_J->setEnabled(true);
        } else {
          ui->actionMap_G->setEnabled(false);
          ui->actionPoint3D_Z->setEnabled(false);
          ui->actionPerception->setEnabled(false);
          ui->actionCamera_S->setEnabled(false);
          ui->actionRaw_J->setEnabled(false);
        }

        if (!info_elapsed_timer_.is_active() || info_elapsed_timer_.get() > 500) {
          double max_value = 0;

          ui->widget_freq->set_unit(1);
          ui->widget_freq->set_unit_text("Hz");
          ui->widget_freq->add_value(freq);
          max_value = ui->widget_freq->get_real_max_value();

          if (max_value >= 100000) {
            ui->widget_freq->set_color(Qt::red);
            ui->widget_freq->set_value_range(0, 1000000);
          } else if (max_value >= 10000) {
            ui->widget_freq->set_color(Qt::red);
            ui->widget_freq->set_value_range(0, 100000);
          } else if (max_value >= 1000) {
            ui->widget_freq->set_color(Qt::darkMagenta);
            ui->widget_freq->set_value_range(0, 10000);
          } else if (max_value >= 100) {
            ui->widget_freq->set_color(Qt::darkYellow);
            ui->widget_freq->set_value_range(0, 1000);
          } else {
            ui->widget_freq->set_color(Qt::darkGreen);
            ui->widget_freq->set_value_range(0, 100);
          }
          ui->widget_freq->update();

          ui->widget_rate->add_value(rate);
          max_value = ui->widget_rate->get_real_max_value();
          if (max_value < 100) {
            ui->widget_rate->set_color(Qt::darkGreen);
            ui->widget_rate->set_value_range(0, 100);
            ui->widget_rate->set_unit(1);
            ui->widget_rate->set_unit_text("B");
          } else if (max_value >= 100 && max_value < 1024) {
            ui->widget_rate->set_color(Qt::darkGreen);
            ui->widget_rate->set_value_range(0, 1024);
            ui->widget_rate->set_unit(1);
            ui->widget_rate->set_unit_text("B");
          } else if (max_value >= 1024 && max_value < 102400) {
            ui->widget_rate->set_color(Qt::darkYellow);
            ui->widget_rate->set_value_range(0, 102400);
            ui->widget_rate->set_unit(1024);
            ui->widget_rate->set_unit_text("KB");
          } else if (max_value >= 102400 && max_value < 1024LL * 1024) {
            ui->widget_rate->set_color(Qt::darkYellow);
            ui->widget_rate->set_value_range(0, 1024 * 1024);
            ui->widget_rate->set_unit(1024);
            ui->widget_rate->set_unit_text("KB");
          } else if (max_value >= 1024LL * 1024 && max_value < 1024LL * 1024 * 100) {
            ui->widget_rate->set_color(Qt::darkMagenta);
            ui->widget_rate->set_value_range(0, 1024LL * 1024 * 100);
            ui->widget_rate->set_unit(1024LL * 1024);
            ui->widget_rate->set_unit_text("MB");
          } else if (max_value >= 1024LL * 1024 * 100 && max_value < 1024LL * 1024 * 1024) {
            ui->widget_rate->set_color(Qt::darkMagenta);
            ui->widget_rate->set_value_range(0, 1024LL * 1024 * 1024);
            ui->widget_rate->set_unit(1024LL * 1024);
            ui->widget_rate->set_unit_text("MB");
          } else if (max_value >= 1024LL * 1024 * 1024 && max_value < 1024LL * 1024 * 1024 * 100) {
            ui->widget_rate->set_color(Qt::red);
            ui->widget_rate->set_value_range(0, 1024LL * 1024 * 1024 * 100);
            ui->widget_rate->set_unit(1024LL * 1024 * 1024);
            ui->widget_rate->set_unit_text("GB");
          } else if (max_value >= 1024LL * 1024 * 1024 * 100) {
            ui->widget_rate->set_color(Qt::red);
            ui->widget_rate->set_value_range(0, 1024LL * 1024 * 1024 * 1024);
            ui->widget_rate->set_unit(1024LL * 1024 * 1024);
            ui->widget_rate->set_unit_text("GB");
          }
          ui->widget_rate->update();

          ui->widget_loss->set_unit(1);
          ui->widget_loss->set_unit_text("%");
          ui->widget_loss->add_value(loss * 100);
          ui->widget_loss->set_value_range(0, 100);
          max_value = ui->widget_loss->get_real_max_value();
          if (max_value >= 50) {
            ui->widget_loss->set_color(Qt::red);
          } else if (max_value >= 10) {
            ui->widget_loss->set_color(Qt::darkMagenta);
          } else if (max_value > 0) {
            ui->widget_loss->set_color(Qt::darkYellow);
          } else {
            ui->widget_loss->set_color(Qt::darkGreen);
          }
          ui->widget_loss->update();

          ui->widget_lat->set_unit(1);
          ui->widget_lat->set_unit_text("ms");
          ui->widget_lat->add_value(latency);
          max_value = ui->widget_lat->get_real_max_value();

          if (max_value < 10) {
            ui->widget_lat->set_color(Qt::darkGreen);
            ui->widget_lat->set_value_range(0, 10);
          } else if (max_value >= 10 && max_value < 100) {
            ui->widget_lat->set_color(Qt::darkYellow);
            ui->widget_lat->set_value_range(0, 1000);
          } else if (max_value >= 100 && max_value < 1000) {
            ui->widget_lat->set_color(Qt::darkMagenta);
            ui->widget_lat->set_value_range(0, 10000);
          } else if (max_value >= 1000) {
            ui->widget_lat->set_color(Qt::red);
            ui->widget_lat->set_value_range(0, 100000);
          }

          ui->widget_lat->update();

          switch (ui->stackedWidget_status->currentIndex()) {
            case 0:
              ui->label_graph_value1->setText(tr("Current Freq: "));
              ui->label_graph_value2->setText(item->text(2));
              break;
            case 1:
              ui->label_graph_value1->setText(tr("Current Rate: "));
              ui->label_graph_value2->setText(item->text(3));
              break;
            case 2:
              ui->label_graph_value1->setText(tr("Current Loss: "));
              ui->label_graph_value2->setText(item->text(4));
              break;
            case 3:
              ui->label_graph_value1->setText(tr("Current Latency: "));
              ui->label_graph_value2->setText(item->text(5));
              break;
            default:
              break;
          }
        }
      }

      switch (status) {
        case vlink::ProxyAPI::kActive:
          item->setForeground(0, QColor(0x20C997));
          item->setIcon(0, QIcon(":/resource/active.png"));
          break;
        case vlink::ProxyAPI::kInActive:
          item->setForeground(0, QColor(0xFA5252));
          item->setIcon(0, QIcon(":/resource/inactive.png"));
          break;
        case vlink::ProxyAPI::kPending:
          item->setForeground(0, QColor(0xFCC419));
          item->setIcon(0, QIcon(":/resource/pending.png"));
          break;
        case vlink::ProxyAPI::kInvalid:
          item->setForeground(0, Qt::darkGray);
          item->setIcon(0, QIcon(":/resource/empty.png"));
          break;
        default:
          break;
      }
    } else {
      item->setForeground(0, Qt::darkGray);
      item->setIcon(0, QIcon(":/resource/empty.png"));
      item->setText(2, "---");
      item->setText(3, "---");
      item->setText(4, "---");
      item->setText(5, "---");
    }

    if (type & vlink::kPublisher || type & vlink::kSetter) {
      can_record_urls_.emplace_back(url);
    }

    item->setData(0, Qt::ToolTipRole, item->text(0));
    item->setData(1, Qt::ToolTipRole, item->text(1));
    item->setData(2, Qt::ToolTipRole, item->text(2));
    item->setData(3, Qt::ToolTipRole, item->text(3));
    item->setData(4, Qt::ToolTipRole, item->text(4));

    bool contains = false;

    if (ui->lineEdit_filter->text().isEmpty()) {
      contains = true;
    } else {
      const std::vector<std::string> filter_tokens =
          vlink::Helpers::split_any(ui->lineEdit_filter->text().toStdString());

      if (ui->comboBox_filter->currentIndex() == 0) {
        QString target_sp = QString::fromStdString(url);

        for (const auto& sp : filter_tokens) {
          if (target_sp.contains(QString::fromStdString(sp), Qt::CaseInsensitive)) {
            contains = true;
            break;
          }
        }
      } else {
        for (const auto& sp : filter_tokens) {
          auto iter =
              std::find_if(process_list.begin(), process_list.end(), [&sp](const vlink::ProxyAPI::Process& process) {
                return QString::fromStdString(process.name).contains(QString::fromStdString(sp), Qt::CaseInsensitive);
              });

          if (iter != process_list.end()) {
            contains = true;
            break;
          }
        }
      }
    }

    if (contains) {
      if (!ui->checkBox_active->isChecked() || status == vlink::ProxyAPI::Status::kActive) {
        switch (ui->comboBox_condi->currentIndex()) {
          case 0:
            item->setHidden(false);
            break;
          case 1:
            item->setHidden(!(type & vlink::kPublisher && type & vlink::kSubscriber));
            break;
          case 2:
            item->setHidden(!(type & vlink::kServer && type & vlink::kClient));
            break;
          case 3:
            item->setHidden(!(type & vlink::kSetter && type & vlink::kGetter));
            break;
          case 4:
            item->setHidden(!(type & vlink::kPublisher || type & vlink::kSubscriber));
            break;
          case 5:
            item->setHidden(!(type & vlink::kServer || type & vlink::kClient));
            break;
          case 6:
            item->setHidden(!(type & vlink::kSetter || type & vlink::kGetter));
            break;
          case 7:
            item->setHidden(type != vlink::kPublisher);
            break;
          case 8:
            item->setHidden(type != vlink::kSubscriber);
            break;
          case 9:
            item->setHidden(type != vlink::kServer);
            break;
          case 10:
            item->setHidden(type != vlink::kClient);
            break;
          case 11:
            item->setHidden(type != vlink::kSetter);
            break;
          case 12:
            item->setHidden(type != vlink::kGetter);
            break;
          default:
            item->setHidden(true);
            break;
        }
      } else {
        item->setHidden(true);
      }
    } else {
      item->setHidden(true);
    }

    if (item->isHidden()) {
      if (ui->treeWidget_url->currentItem() == item) {
        ui->widget_freq->clear();
        ui->widget_rate->clear();
        ui->widget_loss->clear();
        ui->widget_lat->clear();
        ui->label_graph_value2->setText("---");
        ui->treeWidget_url->setCurrentItem(nullptr);
        update_proto_ser();
      }
    }
  }

  if (agg_count > 0 && (!info_elapsed_timer_.is_active() || info_elapsed_timer_.get() > 500)) {
    float avg_loss = (agg_freq > 0) ? (agg_loss / agg_freq) : 0;
    float avg_latency = (agg_freq > 0) ? (agg_latency / agg_freq) : 0;

    double max_value = 0;

    ui->widget_freq->set_unit(1);
    ui->widget_freq->set_unit_text("Hz");
    ui->widget_freq->add_value(agg_freq);
    max_value = ui->widget_freq->get_real_max_value();

    if (max_value >= 100000) {
      ui->widget_freq->set_color(Qt::red);
      ui->widget_freq->set_value_range(0, 1000000);
    } else if (max_value >= 10000) {
      ui->widget_freq->set_color(Qt::red);
      ui->widget_freq->set_value_range(0, 100000);
    } else if (max_value >= 1000) {
      ui->widget_freq->set_color(Qt::darkMagenta);
      ui->widget_freq->set_value_range(0, 10000);
    } else if (max_value >= 100) {
      ui->widget_freq->set_color(Qt::darkYellow);
      ui->widget_freq->set_value_range(0, 1000);
    } else {
      ui->widget_freq->set_color(Qt::darkGreen);
      ui->widget_freq->set_value_range(0, 100);
    }

    ui->widget_freq->update();

    ui->widget_rate->add_value(agg_rate);
    max_value = ui->widget_rate->get_real_max_value();

    if (max_value < 100) {
      ui->widget_rate->set_color(Qt::darkGreen);
      ui->widget_rate->set_value_range(0, 100);
      ui->widget_rate->set_unit(1);
      ui->widget_rate->set_unit_text("B");
    } else if (max_value < 1024) {
      ui->widget_rate->set_color(Qt::darkGreen);
      ui->widget_rate->set_value_range(0, 1024);
      ui->widget_rate->set_unit(1);
      ui->widget_rate->set_unit_text("B");
    } else if (max_value < 102400) {
      ui->widget_rate->set_color(Qt::darkYellow);
      ui->widget_rate->set_value_range(0, 102400);
      ui->widget_rate->set_unit(1024);
      ui->widget_rate->set_unit_text("KB");
    } else if (max_value < 1048576) {
      ui->widget_rate->set_color(Qt::darkMagenta);
      ui->widget_rate->set_value_range(0, 1048576);
      ui->widget_rate->set_unit(1024);
      ui->widget_rate->set_unit_text("KB");
    } else {
      ui->widget_rate->set_color(Qt::red);
      ui->widget_rate->set_value_range(0, 104857600);
      ui->widget_rate->set_unit(1048576);
      ui->widget_rate->set_unit_text("MB");
    }

    ui->widget_rate->update();

    ui->widget_loss->set_value_range(0, 100);
    ui->widget_loss->set_unit(1);
    ui->widget_loss->set_unit_text("%");
    ui->widget_loss->add_value(avg_loss * 100);

    if (avg_loss >= 0.5F) {
      ui->widget_loss->set_color(Qt::red);
    } else if (avg_loss >= 0.1F) {
      ui->widget_loss->set_color(Qt::darkMagenta);
    } else if (avg_loss > 0) {
      ui->widget_loss->set_color(Qt::darkYellow);
    } else {
      ui->widget_loss->set_color(Qt::darkGreen);
    }

    ui->widget_loss->update();

    ui->widget_lat->set_unit(1);
    ui->widget_lat->set_unit_text("ms");
    ui->widget_lat->add_value(avg_latency);
    max_value = ui->widget_lat->get_real_max_value();

    if (max_value < 10) {
      ui->widget_lat->set_color(Qt::darkGreen);
      ui->widget_lat->set_value_range(0, 10);
    } else if (max_value < 100) {
      ui->widget_lat->set_color(Qt::darkYellow);
      ui->widget_lat->set_value_range(0, 1000);
    } else if (max_value < 1000) {
      ui->widget_lat->set_color(Qt::darkMagenta);
      ui->widget_lat->set_value_range(0, 10000);
    } else {
      ui->widget_lat->set_color(Qt::red);
      ui->widget_lat->set_value_range(0, 100000);
    }

    ui->widget_lat->update();

    switch (ui->stackedWidget_status->currentIndex()) {
      case 0:
        ui->label_graph_value1->setText(tr("Total Freq: "));
        ui->label_graph_value2->setText(QString::number(agg_freq, 'f', 2) + "Hz");
        break;
      case 1:
        ui->label_graph_value1->setText(tr("Total Rate: "));

        if (agg_rate < 1024) {
          ui->label_graph_value2->setText(QString::number(agg_rate) + "B/s");
        } else if (agg_rate < 1024LL * 1024) {
          ui->label_graph_value2->setText(QString::number(agg_rate / 1024.0, 'f', 2) + "KB/s");
        } else {
          ui->label_graph_value2->setText(QString::number(agg_rate / 1024.0 / 1024.0, 'f', 2) + "MB/s");
        }

        break;
      case 2:
        ui->label_graph_value1->setText(tr("Avg Loss: "));
        ui->label_graph_value2->setText(QString::number(avg_loss * 100, 'f', 2) + "%");
        break;
      case 3:
        ui->label_graph_value1->setText(tr("Avg Latency: "));
        ui->label_graph_value2->setText(QString::number(avg_latency, 'f', 2) + "ms");
        break;
      default:
        break;
    }
  }

  double loss = 0;
  {
    uint64_t total = proxy_->get_lost().total - last_sample_info_.total;
    uint64_t lost = proxy_->get_lost().lost - last_sample_info_.lost;

    if (total > 0 && lost > 0) {
      loss = static_cast<double>(lost) / total;
    }

    last_sample_info_ = proxy_->get_lost();
  }

  update_status_bar(info_list.size(), active_count, ui->treeWidget_url->selectedItems().size(), total_rate, loss);

  update_process_widget();

  info_elapsed_timer_.restart();

  ui->checkBox_selectall->setEnabled(ui->treeWidget_url->topLevelItemCount() != 0);
}

void MainWindow::update_proto_ser() {
  if (proxy_->is_ready_to_quit()) {
    return;
  }

  if (ui->stackedWidget_main->currentIndex() != 0) {
    clear_all_property_item(ui->treeWidget_property);
    ui->pushButton_analyze->setEnabled(false);

    ui->treeWidget_property->setEnabled(false);

    update_local_proto();

    return;
  }

  auto* current_item = ui->treeWidget_url->currentItem();
  const auto& selected_items = ui->treeWidget_url->selectedItems();

  if (!current_item) {
    ui->lineEdit_protoser->clear();
    clear_all_property_item(ui->treeWidget_property);
    ui->pushButton_analyze->setEnabled(false);
    status_label2_->clear();
    if (root_msg_) {
      delete root_msg_;
      root_msg_ = nullptr;
    }

    ui->treeWidget_property->setEnabled(false);

    ui->treeWidget_process1->setEnabled(false);
    ui->treeWidget_process2->setEnabled(false);
    clear_all_process_item();

    ui->label_process1->setText("---");
    ui->label_process2->setText("---");

    ui->actionEdit_E->setEnabled(false);
    ui->actionCamera_S->setEnabled(false);
    ui->actionPoint3D_Z->setEnabled(false);
    ui->actionPerception->setEnabled(false);
    ui->actionMap_G->setEnabled(false);
    ui->actionRaw_J->setEnabled(false);

    return;
  }

  reload_root_msg(current_item);

  int type = current_item->data(0, Qt::UserRole).toInt();
  std::string url = current_item->text(1).toStdString();

  // if (desc_ && (type & vlink::kPublisher)) {
  //   ui->actionPoint3D_Z->setEnabled(true);
  // } else {
  //   ui->actionPoint3D_Z->setEnabled(false);
  // }

  if ((type & vlink::kPublisher) || (type & vlink::kSetter)) {
    ui->actionCamera_S->setEnabled(true);
    ui->actionPoint3D_Z->setEnabled(true);
    ui->actionPerception->setEnabled(true);
    ui->actionMap_G->setEnabled(true);
    if (selected_items.count() == 1) {
      ui->actionRaw_J->setEnabled(true);
    } else {
      ui->actionRaw_J->setEnabled(false);
    }
  } else {
    ui->actionCamera_S->setEnabled(false);
    ui->actionPoint3D_Z->setEnabled(false);
    ui->actionPerception->setEnabled(false);
    ui->actionMap_G->setEnabled(false);
    ui->actionRaw_J->setEnabled(false);
  }

  if (desc_ || (is_flatbuffers_types_ && current_fbs_context_ && current_fbs_context_->valid())) {
    if (selected_items.count() == 1) {
      ui->actionEdit_E->setEnabled(true);
    } else {
      ui->actionEdit_E->setEnabled(false);
    }
  } else {
    ui->actionEdit_E->setEnabled(false);
  }
}

void MainWindow::update_property_widget(const QVariant& variant, const QElapsedTimer& timer, bool refresh) {
  if (proxy_->is_ready_to_quit()) {
    return;
  }

  if (current_proxy_mode_ != vlink::ProxyAPI::kObserveAll && current_proxy_mode_ != vlink::ProxyAPI::kObserveOne) {
    return;
  }

  if (ui->stackedWidget_main->currentIndex() == 0 && ui->checkBox_perf->isChecked() && timer.elapsed() > 500) {
    property_timer_.restart();
    qApp->processEvents();
    return;
  }

  const auto& proxy_data = variant.value<vlink::ProxyAPI::Data>();
  const auto schema_type = proxy_data.schema;

  if (ui->stackedWidget_main->currentIndex() == 0) {
    if (!ui->treeWidget_url->currentItem()) {
      return;
    }

    if (ui->treeWidget_url->selectedItems().count() <= 1) {
      if (ui->treeWidget_url->currentItem()->text(1).toStdString() != proxy_data.url) {
        return;
      }

      if (ui->treeWidget_url->currentItem()->data(1, Qt::UserRole).toString().toStdString() != proxy_data.ser) {
        return;
      }
    }
  }

  if (refresh) {
    last_data_map_[proxy_data.url] = proxy_data.raw;
    ui->label_access->setVisible(true);
    flag_timer_->stop();
    flag_timer_->start();
  }

  if (!ui->checkBox_view->isChecked()) {
    status_label2_->setText(tr("   The view is disabled.   "));
    clear_all_property_item(ui->treeWidget_property);
    ui->treeWidget_property->setEnabled(false);
    ui->pushButton_analyze->setEnabled(false);
    return;
  }

  if (ui->treeWidget_url->selectedItems().count() > 1) {
    status_label2_->setText(tr("   Not support multi-url.   "));
    clear_all_property_item(ui->treeWidget_property);
    ui->treeWidget_property->setEnabled(false);
    ui->pushButton_analyze->setEnabled(false);
    return;
  }

  if (is_zero_copy_types_) {
    if (schema_type != vlink::SchemaType::kZeroCopy) {
      status_label2_->setText(tr("   Schema family mismatch.   "));
      clear_all_property_item(ui->treeWidget_property);
      ui->treeWidget_property->setEnabled(false);
      ui->pushButton_analyze->setEnabled(false);
      return;
    }

    status_label2_->setText(tr("   Found zero-copy types.   "));
    update_zero_copy_item_property(proxy_data.raw);
    ui->treeWidget_property->setEnabled(true);
    return;
  }

  if (is_flatbuffers_types_) {
    if (schema_type != vlink::SchemaType::kFlatbuffers) {
      status_label2_->setText(tr("   Schema family mismatch.   "));
      clear_all_property_item(ui->treeWidget_property);
      ui->treeWidget_property->setEnabled(false);
      ui->pushButton_analyze->setEnabled(false);
      return;
    }

    if (!current_fbs_context_ || !current_fbs_context_->valid()) {
      status_label2_->setText(tr("   No matching schema found.   "));
      clear_all_property_item(ui->treeWidget_property);
      ui->treeWidget_property->setEnabled(false);
      ui->pushButton_analyze->setEnabled(false);
      return;
    }

    FlatbuffersObjectView root_view;
    if (!make_root_view(*current_fbs_context_, proxy_data.raw, root_view)) {
      status_label2_->setText(tr("   Deserialization failed.   "));
      clear_all_property_item(ui->treeWidget_property);
      ui->treeWidget_property->setEnabled(false);
      ui->pushButton_analyze->setEnabled(false);
      return;
    }

    to_hide_item_list_ = all_item_list_;

    ui->treeWidget_property->setUpdatesEnabled(false);
    if (ui->checkBox_perf->isChecked()) {
      property_timer_.restart();
      qApp->processEvents();
    }

    bool ret = get_flatbuffers_property_list(ui->treeWidget_property, "", root_view, *current_fbs_context_);

    ui->treeWidget_property->setUpdatesEnabled(true);

    if (ret) {
      status_label2_->setText(tr("   Deserialization success.   "));
    } else {
      status_label2_->setText(tr("   Deserialization failed.   "));
      clear_all_property_item(ui->treeWidget_property);
      ui->pushButton_analyze->setEnabled(false);
    }

    ui->treeWidget_property->setEnabled(ret);

    for (const auto& item : to_hide_item_list_) {
      item->setHidden(true);
    }

    return;
  }

  if (schema_type != vlink::SchemaType::kProtobuf) {
    status_label2_->setText(tr("   Schema family mismatch.   "));
    clear_all_property_item(ui->treeWidget_property);
    ui->treeWidget_property->setEnabled(false);
    ui->pushButton_analyze->setEnabled(false);
    return;
  }

  if (!root_msg_) {
    status_label2_->setText(tr("   No matching schema found.   "));
    clear_all_property_item(ui->treeWidget_property);
    ui->treeWidget_property->setEnabled(false);
    ui->pushButton_analyze->setEnabled(false);
    return;
  }

  if (!root_msg_->ParseFromArray(proxy_data.raw.data(), proxy_data.raw.size())) {
    status_label2_->setText(tr("   Deserialization failed.   "));
    clear_all_property_item(ui->treeWidget_property);
    ui->treeWidget_property->setEnabled(false);
    ui->pushButton_analyze->setEnabled(false);
    return;
  }

  is_zero_copy_types_ = false;

  to_hide_item_list_ = all_item_list_;

  ui->treeWidget_property->setUpdatesEnabled(false);

  if (ui->checkBox_perf->isChecked()) {
    property_timer_.restart();
    qApp->processEvents();
  }

  bool ret = get_property_list(ui->treeWidget_property, "", root_msg_);

  ui->treeWidget_property->setUpdatesEnabled(true);

  if (ret) {
    status_label2_->setText(tr("   Deserialization success.   "));
  } else {
    status_label2_->setText(tr("   Deserialization failed.   "));
    clear_all_property_item(ui->treeWidget_property);
    ui->pushButton_analyze->setEnabled(false);
  }

  ui->treeWidget_property->setEnabled(ret);

  for (const auto& item : to_hide_item_list_) {
    item->setHidden(true);
  }

  // ui->treeWidget_property->expandAll();
}

void MainWindow::update_process_widget() {
  if (proxy_->is_ready_to_quit()) {
    return;
  }

  const auto& selected_items = ui->treeWidget_url->selectedItems();

  if (selected_items.count() > 1) {
    ui->treeWidget_process1->setEnabled(false);
    ui->treeWidget_process2->setEnabled(false);
    clear_all_process_item();
    return;
  }

  auto* current_item = ui->treeWidget_url->currentItem();

  if (!current_item) {
    ui->treeWidget_process1->setEnabled(false);
    ui->treeWidget_process2->setEnabled(false);
    clear_all_process_item();
    return;
  }

  int type = current_item->data(0, Qt::UserRole).toInt();
  std::string url = current_item->text(1).toStdString();

  const auto& process_list = process_map_[url];

  if (type & vlink::kPublisher || type & vlink::kSubscriber) {
    ui->label_process1->setText(tr("Publishers:"));
    ui->label_process2->setText(tr("Subscribers:"));
  } else if (type & vlink::kServer || type & vlink::kClient) {
    ui->label_process1->setText(tr("Servers:"));
    ui->label_process2->setText(tr("Clients:"));
  } else if (type & vlink::kSetter || type & vlink::kGetter) {
    ui->label_process1->setText(tr("Setters:"));
    ui->label_process2->setText(tr("Getters:"));
  }

  for (int i = 0; i < ui->treeWidget_process1->topLevelItemCount(); i++) {
    auto p = ui->treeWidget_process1->topLevelItem(i);
    bool find = false;

    for (size_t j = 0; j < process_list.size(); ++j) {
      if (process_list.at(j).type == vlink::kPublisher || process_list.at(j).type & vlink::kServer ||
          process_list.at(j).type & vlink::kSetter) {
        if (p->text(0).toUInt() == process_list.at(j).pid) {
          find = true;
          break;
        }
      }
    }

    if (!find) {
      QTreeWidgetItem* item = ui->treeWidget_process1->takeTopLevelItem(i);
      delete item;
      i--;
    }
  }

  for (int i = 0; i < ui->treeWidget_process2->topLevelItemCount(); i++) {
    auto p = ui->treeWidget_process2->topLevelItem(i);

    bool find = false;
    for (size_t j = 0; j < process_list.size(); ++j) {
      if (process_list.at(j).type == vlink::kSubscriber || process_list.at(j).type & vlink::kClient ||
          process_list.at(j).type & vlink::kGetter) {
        if (p->text(0).toUInt() == process_list.at(j).pid) {
          find = true;
          break;
        }
      }
    }

    if (!find) {
      QTreeWidgetItem* item = ui->treeWidget_process2->takeTopLevelItem(i);
      delete item;
      i--;
    }
  }

  for (const auto& process : process_list) {
    QTreeWidgetItem* item = nullptr;

    if (process.type == vlink::kPublisher || process.type & vlink::kServer || process.type & vlink::kSetter) {
      for (int i = 0; i < ui->treeWidget_process1->topLevelItemCount(); i++) {
        auto* p = ui->treeWidget_process1->topLevelItem(i);
        if (p->text(0).toUInt() == process.pid) {
          item = p;
          break;
        }
      }

      if (!item) {
        item = new QTreeWidgetItem;
        ui->treeWidget_process1->addTopLevelItem(item);
      }
    } else {
      for (int i = 0; i < ui->treeWidget_process2->topLevelItemCount(); i++) {
        auto* p = ui->treeWidget_process2->topLevelItem(i);
        if (p->text(0).toUInt() == process.pid) {
          item = p;
          break;
        }
      }

      if (!item) {
        item = new QTreeWidgetItem;
        ui->treeWidget_process2->addTopLevelItem(item);
      }
    }

    item->setFlags(item->flags() & ~Qt::ItemIsSelectable);

    item->setText(0, QString::number(process.pid));
    item->setText(1, QString::fromUtf8(process.name.c_str()));

    QString tip_str = tr("Host: %1\nPID: %2\nName: %3\nIP: %4")
                          .arg(QString::fromUtf8(process.host.c_str()), QString::number(process.pid),
                               QString::fromUtf8(process.name.c_str()), QString::fromStdString(process.ip));

    item->setData(0, Qt::ToolTipRole, tip_str);
    item->setData(1, Qt::ToolTipRole, tip_str);
  }

  ui->treeWidget_process1->setEnabled(true);
  ui->treeWidget_process2->setEnabled(true);
}

void MainWindow::adjust_size() {
  ui->treeWidget_url->setColumnWidth(0, 95);

  int url_width = ui->treeWidget_url->size().width() - 95 - (ui->treeWidget_url->isColumnHidden(2) ? 0 : 90) -
                  (ui->treeWidget_url->isColumnHidden(3) ? 0 : 90) - (ui->treeWidget_url->isColumnHidden(4) ? 0 : 90) -
                  (ui->treeWidget_url->isColumnHidden(5) ? 0 : 90) - 25;

  if (url_width < 150) {
    url_width = 150;
  }

  ui->treeWidget_url->setColumnWidth(1, url_width);
  ui->treeWidget_url->setColumnWidth(2, 90);
  ui->treeWidget_url->setColumnWidth(3, 90);
  ui->treeWidget_url->setColumnWidth(4, 90);
  ui->treeWidget_url->setColumnWidth(5, 90);

  ui->treeWidget_property->setColumnWidth(0, 90);
  ui->treeWidget_property->setColumnWidth(1, (ui->treeWidget_property->size().width() - 90 - 25) / 5);
  ui->treeWidget_property->setColumnWidth(2, (ui->treeWidget_property->size().width() - 90 - 25) / 5 * 2);
  ui->treeWidget_property->setColumnWidth(3, (ui->treeWidget_property->size().width() - 90 - 25) / 5 * 2);

  ui->tableView_data->setColumnWidth(0, 150);
  ui->tableView_data->setColumnWidth(1, ui->tableView_data->size().width() - 150 - 100);
}

vlink::SchemaData MainWindow::search_schema(const std::string& name, vlink::SchemaType schema_type) {
  std::lock_guard lock(schema_mtx_);

  if VUNLIKELY (name.empty() || !vlink::SchemaData::is_real_type(schema_type)) {
    return {};
  }

  const auto cache_key = std::to_string(static_cast<int>(schema_type)) + ":" + name;
  auto iter = schema_map_.find(cache_key);

  if (iter != schema_map_.end()) {
    return iter->second;
  }

  if (schema_type == vlink::SchemaType::kZeroCopy) {
    vlink::SchemaData schema_data;
    schema_data.name = name;
    schema_data.encoding = "vlink_msg";
    schema_data.schema_type = vlink::SchemaType::kZeroCopy;
    schema_map_.emplace(cache_key, schema_data);
    return schema_data;
  }

  if (schema_type == vlink::SchemaType::kRaw) {
    vlink::SchemaData schema_data;
    schema_data.name = name;
    schema_data.encoding = std::string(vlink::SchemaData::convert_type(vlink::SchemaType::kRaw));
    schema_data.schema_type = vlink::SchemaType::kRaw;
    schema_map_.emplace(cache_key, schema_data);
    return schema_data;
  }

  if (schema_type == vlink::SchemaType::kProtobuf && des_pool_) {
    const auto* msg_desc = des_pool_->FindMessageTypeByName(name);

    if (msg_desc && msg_desc->file()) {
      google::protobuf::FileDescriptorSet proto_fd_set;

      std::queue<const google::protobuf::FileDescriptor*> to_add;
      to_add.push(msg_desc->file());

#if GOOGLE_PROTOBUF_VERSION >= 6030000
      std::unordered_set<std::string_view> seen_dependencies;
#else
      std::unordered_set<std::string> seen_dependencies;
#endif

      while (!to_add.empty()) {
        const google::protobuf::FileDescriptor* next = to_add.front();
        to_add.pop();

        next->CopyTo(proto_fd_set.add_file());

        for (int i = 0; i < next->dependency_count(); ++i) {
          const auto& dep = next->dependency(i);

          if (seen_dependencies.find(dep->name()) == seen_dependencies.end()) {
            seen_dependencies.insert(dep->name());
            to_add.push(dep);
          }
        }
      }

      vlink::SchemaData schema_data;
      schema_data.name = name;
      schema_data.schema_type = vlink::SchemaType::kProtobuf;
      schema_data.data = vlink::Bytes::create(proto_fd_set.ByteSizeLong());

      if VLIKELY (proto_fd_set.SerializeToArray(schema_data.data.data(), schema_data.data.size())) {
        schema_data.encoding = "protobuf";
        schema_map_.emplace(cache_key, schema_data);
        return schema_data;
      }
    }
  }

  if (schema_type == vlink::SchemaType::kFlatbuffers) {
    auto fbs_schema = flatbuffers_runtime_.search_schema(name);

    if (!fbs_schema.encoding.empty()) {
      schema_map_.emplace(cache_key, fbs_schema);
      return fbs_schema;
    }
  }

  return {};
}

void MainWindow::process_current_item_changed() {
  ui->widget_freq->clear();
  ui->widget_rate->clear();
  ui->widget_loss->clear();
  ui->widget_lat->clear();
  ui->label_graph_value2->setText("---");

  update_proto_ser();

  if (proxy_->get_current_error() != vlink::ProxyAPI::kNoError) {
    return;
  }

  send_control(ui->checkBox_observe->isChecked() ? vlink::ProxyAPI::kObserveAll : vlink::ProxyAPI::kObserveOne, true);

  last_select_items_ = ui->treeWidget_url->selectedItems();
}

void MainWindow::reload_root_msg(QTreeWidgetItem* item) {
  std::string url = item->text(1).toStdString();
  std::string ser = item->data(1, Qt::UserRole).toString().toStdString();

  update_process_widget();

  clear_all_property_item(ui->treeWidget_property);
  ui->pushButton_analyze->setEnabled(false);

  if (!update_proto_root_msg(url, ser, true)) {
    return;
  }

  vlink::ProxyAPI::Data proxy_data;
  proxy_data.timestamp = 0;
  proxy_data.url = url;
  proxy_data.ser = ser;
  {
    const auto schema_iter = schema_type_map_.find(url);
    proxy_data.schema = schema_iter != schema_type_map_.end() ? schema_iter->second : vlink::SchemaType::kUnknown;
  }

  auto iter = last_data_map_.find(url);

  if (iter != last_data_map_.end()) {
    proxy_data.raw = iter->second;
  }

  QElapsedTimer timer;
  timer.start();
  update_property_widget(QVariant::fromValue<vlink::ProxyAPI::Data>(proxy_data), timer, false);

  if (iter != last_data_map_.end()) {
    status_label2_->setText(tr("   Load cache data.   "));
  } else {
    status_label2_->setText(tr("   Load empty data.   "));
  }
}

bool MainWindow::update_proto_root_msg(const std::string& url, const std::string& ser, bool force) {
  if (!force && current_ser_ == ser) {
    return false;
  }

  current_url_ = url;
  current_ser_ = ser;

  if (root_msg_) {
    delete root_msg_;
    root_msg_ = nullptr;
  }

  current_fbs_context_.reset();

  ui->lineEdit_protoser->setText(QString::fromStdString(ser));

  const auto schema_iter = schema_type_map_.find(url);
  const auto schema_type = vlink::SchemaData::resolve_type(
      schema_iter != schema_type_map_.end() ? schema_iter->second : vlink::SchemaType::kUnknown, ser);

  if (ser.find('|') != std::string::npos) {
    desc_ = nullptr;
    status_label2_->setText(tr("   Method routes are not supported in the property viewer.   "));
    ui->actionEdit_E->setEnabled(false);
    ui->treeWidget_property->setEnabled(false);
    clear_all_property_item(ui->treeWidget_property);
    ui->pushButton_analyze->setEnabled(false);
    is_zero_copy_types_ = false;
    is_flatbuffers_types_ = false;
    return false;
  }

  switch (schema_type) {
    case vlink::SchemaType::kZeroCopy:
      desc_ = nullptr;
      ui->actionEdit_E->setEnabled(false);
      clear_all_property_item(ui->treeWidget_property);
      is_zero_copy_types_ = true;
      is_flatbuffers_types_ = false;
      return true;

    case vlink::SchemaType::kProtobuf:
      desc_ = (!des_pool_ || ser.empty())
                  ? nullptr
                  : const_cast<google::protobuf::Descriptor*>(des_pool_->FindMessageTypeByName(ser));

      if (!desc_ || !desc_->file() || !factory_) {
        status_label2_->setText(tr("   Protobuf schema is unavailable.   "));
        ui->actionEdit_E->setEnabled(false);
        ui->treeWidget_property->setEnabled(false);
        clear_all_property_item(ui->treeWidget_property);
        ui->pushButton_analyze->setEnabled(false);
        is_zero_copy_types_ = false;
        is_flatbuffers_types_ = false;
        return false;
      }

      root_msg_ = factory_->GetPrototype(desc_)->New();
      ui->actionEdit_E->setEnabled(root_msg_ != nullptr);
      is_zero_copy_types_ = false;
      is_flatbuffers_types_ = false;
      return root_msg_ != nullptr;

    case vlink::SchemaType::kFlatbuffers:
      desc_ = nullptr;
      current_fbs_context_ =
          ser.empty() ? std::shared_ptr<FlatbuffersSchemaContext>{} : flatbuffers_runtime_.find_context(ser);

      if (!current_fbs_context_ || !current_fbs_context_->valid()) {
        status_label2_->setText(tr("   FlatBuffers schema is unavailable.   "));
        ui->actionEdit_E->setEnabled(false);
        ui->treeWidget_property->setEnabled(false);
        clear_all_property_item(ui->treeWidget_property);
        ui->pushButton_analyze->setEnabled(false);
        is_zero_copy_types_ = false;
        is_flatbuffers_types_ = false;
        return false;
      }

      ui->actionEdit_E->setEnabled(true);
      is_zero_copy_types_ = false;
      is_flatbuffers_types_ = true;
      return true;

    case vlink::SchemaType::kRaw:
    case vlink::SchemaType::kUnknown:
      desc_ = nullptr;
      status_label2_->setText(tr("   Schema family is unavailable.   "));
      ui->actionEdit_E->setEnabled(false);
      ui->treeWidget_property->setEnabled(false);
      clear_all_property_item(ui->treeWidget_property);
      ui->pushButton_analyze->setEnabled(false);
      is_zero_copy_types_ = false;
      is_flatbuffers_types_ = false;
      return false;

    default:
      desc_ = nullptr;
      is_zero_copy_types_ = false;
      is_flatbuffers_types_ = false;
      return false;
  }
}

void MainWindow::update_local_proto() {
  auto current = ui->tableView_data->currentIndex();

  if (!current.isValid()) {
    return;
  }

  auto time_variant = current.sibling(current.row(), 0).data(Qt::UserRole);
  auto url_variant = current.sibling(current.row(), 1).data(Qt::DisplayRole);
  auto ser_variant = current.sibling(current.row(), 2).data(Qt::DisplayRole);
  auto data_variant = current.sibling(current.row(), 3).data(Qt::DisplayRole);

  vlink::ProxyAPI::Data proxy_data;
  proxy_data.timestamp = time_variant.toLongLong();
  proxy_data.url = url_variant.toString().toStdString();
  proxy_data.ser = ser_variant.toString().toStdString();
  {
    const auto schema_iter = schema_type_map_.find(proxy_data.url);
    proxy_data.schema = schema_iter != schema_type_map_.end() ? schema_iter->second : vlink::SchemaType::kUnknown;
  }

  const auto& byte_array = data_variant.toByteArray();

  const auto* target_data = reinterpret_cast<const uint8_t*>(byte_array.data());

  if (local_use_compress_ && vlink::Bytes::is_compress_data(target_data, byte_array.size())) {
    proxy_data.raw = vlink::Bytes::uncompress_data(target_data, byte_array.size(), false);
  } else {
    proxy_data.raw = vlink::Bytes::shallow_copy(target_data, byte_array.size());
  }

  if (!update_proto_root_msg(proxy_data.url, proxy_data.ser, true)) {
    return;
  }

  QElapsedTimer timer;
  timer.start();
  update_property_widget(QVariant::fromValue<vlink::ProxyAPI::Data>(proxy_data), timer, false);
}

QString MainWindow::get_str_for_number(int64_t num) {
  if (ui->checkBox_hex->isChecked()) {
    if (num < 0) {
      return "-0x" + QString::number(qAbs(num), 16).toUpper();
    } else {
      return "0x" + QString::number(num, 16).toUpper();
    }
  } else {
    return QString::number(num, 10);
  }
}

QString MainWindow::get_str_for_enum(const std::string& enum_str, int64_t num) {
  if (ui->checkBox_enum->isChecked()) {
    return QString::fromStdString(enum_str);
  } else {
    return QString::number(num, 10);
  }
}

int MainWindow::get_int_for_str(const QString& str) {
  if (str.startsWith("0x") || str.startsWith("0X")) {
    return str.mid(2).toInt(nullptr, 16);
  } else {
    return str.toInt(nullptr, 10);
  }
}

qlonglong MainWindow::get_longlong_for_str(const QString& str) {
  if (str.startsWith("0x") || str.startsWith("0X")) {
    return str.mid(2).toLongLong(nullptr, 16);
  } else {
    return str.toLongLong(nullptr, 10);
  }
}

unsigned int MainWindow::get_uint_for_str(const QString& str) {
  if (str.startsWith("0x") || str.startsWith("0X")) {
    return str.mid(2).toUInt(nullptr, 16);
  } else {
    return str.toUInt(nullptr, 10);
  }
}

qulonglong MainWindow::get_ulonglong_for_str(const QString& str) {
  if (str.startsWith("0x") || str.startsWith("0X")) {
    return str.mid(2).toULongLong(nullptr, 16);
  } else {
    return str.toULongLong(nullptr, 10);
  }
}

void MainWindow::import_protos(const QString& dir, bool& has_import, int depth) {
  if (depth >= 100) {
    return;
  }

  QDir qdir(dir);

  const QFileInfoList file_list =
      qdir.entryInfoList(QDir::Dirs | QDir::Files | QDir::NoDotAndDotDot, QDir::DirsLast | QDir::Name);

  if (file_list.isEmpty() || file_list.size() > 1000) {
    return;
  }

  for (const QFileInfo& file : file_list) {
    if (file.isFile() && file.suffix() == "proto") {
      QString file_path = file.absoluteFilePath();
      QString relative_path = QDir(QString::fromStdString(source_dir_)).relativeFilePath(file_path);
      std::string import_path = relative_path.toStdString();

      auto* ptr = importer_->Import(import_path);

      if (ptr) {
        has_import = true;
        proto_files_.push_back(ptr);
      }

    } else if (file.isDir()) {
      import_protos(file.absoluteFilePath(), has_import, depth + 1);
    }
  }
}

static void collect_proto_message_names(const google::protobuf::Descriptor* descriptor, std::vector<QString>& out) {
  if (!descriptor) {
    return;
  }

  out.push_back(QString::fromStdString(std::string(descriptor->full_name())));

  for (int i = 0; i < descriptor->nested_type_count(); ++i) {
    collect_proto_message_names(descriptor->nested_type(i), out);
  }
}

std::vector<QString> MainWindow::scanned_message_types() const {
  std::vector<QString> out;

  for (const auto* file : proto_files_) {
    if (!file) {
      continue;
    }

    for (int i = 0; i < file->message_type_count(); ++i) {
      collect_proto_message_names(file->message_type(i), out);
    }
  }

  for (const auto& name : flatbuffers_runtime_.type_names()) {
    out.push_back(QString::fromStdString(name));
  }

  std::sort(out.begin(), out.end());
  out.erase(std::unique(out.begin(), out.end()), out.end());
  return out;
}

void MainWindow::send_control(vlink::ProxyAPI::Mode mode, bool has_url) {
  const auto previous_proxy_mode = current_proxy_mode_;
  current_proxy_mode_ = mode;

  if (proxy_->get_current_config().role != vlink::ProxyAPI::kController) {
    return;
  }

  vlink::ProxyAPI::Control control;
  control.mode = mode;

  if (has_url || mode == vlink::ProxyAPI::kObserveAll) {
    control.filter_by_process = ui->comboBox_filter->currentIndex() == 1;
    control.filter_str = ui->lineEdit_filter->text().toStdString();
    control.filter_type = ui->comboBox_condi->currentIndex();
  }

  if (has_url) {
    const auto& selected_items = ui->treeWidget_url->selectedItems();
    bool skipped_missing_meta = false;
    bool skipped_unsupported_type = false;

    for (const auto& item : selected_items) {
      const auto url = item->text(1).toStdString();
      const auto type = static_cast<uint32_t>(item->data(0, Qt::UserRole).toUInt());
      const auto ser_iter = ser_map_.find(url);
      const auto schema_iter = schema_type_map_.find(url);
      auto schema_type = schema_iter != schema_type_map_.end() ? schema_iter->second : vlink::SchemaType::kUnknown;

      if (((type & vlink::kPublisher) == 0U) && ((type & vlink::kSetter) == 0U)) {
        skipped_unsupported_type = true;
        continue;
      }

      if (ser_iter == ser_map_.end() || ser_iter->second.empty()) {
        skipped_missing_meta = true;
        continue;
      }

      control.url_meta_list.emplace_back(
          vlink::ProxyAPI::UrlMeta{url, ser_iter->second, schema_type, vlink::kSubscriber});
    }

    if (skipped_missing_meta && control.url_meta_list.empty()) {
      current_proxy_mode_ = previous_proxy_mode;
      status_label2_->setText(tr("   Missing schema metadata for one or more selected topics.   "));
      return;
    }

    if (skipped_missing_meta) {
      status_label2_->setText(tr("   Some selected topics were skipped because schema metadata is incomplete.   "));
    }

    if (control.url_meta_list.empty() && skipped_unsupported_type) {
      current_proxy_mode_ = previous_proxy_mode;
      status_label2_->setText(tr("   Selected topics do not support live observation.   "));
      return;
    }
  }

  if VUNLIKELY (!proxy_->send_control(control)) {
    current_proxy_mode_ = previous_proxy_mode;
  }
}

bool MainWindow::select_source_dir(const std::string& dir) {
  if (root_msg_) {
    delete root_msg_;
    root_msg_ = nullptr;
  }

  {
    std::lock_guard lock(schema_mtx_);
    schema_map_.clear();
  }

  factory_ = std::make_shared<google::protobuf::DynamicMessageFactory>();
  source_tree_ = std::make_shared<google::protobuf::compiler::DiskSourceTree>();
  importer_ = std::make_shared<google::protobuf::compiler::Importer>(source_tree_.get(), nullptr);

  source_dir_ = dir;
  source_tree_->MapPath("", source_dir_);

  if (importer_) {
    des_pool_ = const_cast<google::protobuf::DescriptorPool*>(importer_->pool());
  } else {
    des_pool_ = nullptr;
  }

  bool has_imported = false;

  ui->lineEdit_protodir->setText(QString::fromStdString(source_dir_));

  proto_files_.clear();

  import_protos(ui->lineEdit_protodir->text(), has_imported);

  update_proto_ser();

  return has_imported;
}

bool MainWindow::select_fbs_dir(const std::string& dir) {
  if (dir.empty()) {
    {
      std::lock_guard lock(schema_mtx_);
      schema_map_.clear();
    }

    flatbuffers_runtime_.clear();
    fbs_dir_.clear();
    ui->lineEdit_fbsdir->clear();
    return true;
  }

  {
    std::lock_guard lock(schema_mtx_);
    schema_map_.clear();
  }

  std::string error;

  if (!flatbuffers_runtime_.load_dir(dir, &error)) {
    status_label2_->setText(tr("   Load fbs dir failed.   "));
    return false;
  }

  fbs_dir_ = dir;
  ui->lineEdit_fbsdir->setText(QString::fromStdString(fbs_dir_));
  update_proto_ser();
  return true;
}

bool MainWindow::get_property_list(QTreeWidget* widget, const std::string& parent_id,
                                   const google::protobuf::Message* msg) {
  if (ui->checkBox_perf->isChecked() && property_timer_.restart() > 100) {
    qApp->processEvents();
  }

  const auto* ref = msg->GetReflection();
  for (int i = 0; i < msg->GetDescriptor()->field_count(); ++i) {
    std::string current_id = parent_id + "." + std::to_string(i);
    const auto* field = msg->GetDescriptor()->field(i);

    if (!field->is_repeated()) {
      auto* item = get_item_property(widget, parent_id, current_id);
      item_to_msg_map_[item] = const_cast<google::protobuf::Message*>(msg);
      item_to_field_map_[item] = const_cast<google::protobuf::FieldDescriptor*>(field);

      item->setText(0, QString::number(field->number()));

#if GOOGLE_PROTOBUF_VERSION >= 6030000

      if (field->message_type()) {
        item->setText(1, field->message_type()->name().data());
      } else {
        item->setText(1, field->type_name().data());
      }

      item->setData(0, Qt::UserRole, field->cpp_type());
      item->setData(0, EditDialog::kEditValueKindRole,
                    field->cpp_type() == google::protobuf::FieldDescriptor::CPPTYPE_STRING
                        ? static_cast<int>(field->type() == google::protobuf::FieldDescriptor::TYPE_BYTES
                                               ? EditDialog::EditValueKind::kBytes
                                               : EditDialog::EditValueKind::kString)
                        : static_cast<int>(EditDialog::EditValueKind::kUnknown));
      item->setText(2, field->name().data());
#else

      if (field->message_type()) {
        item->setText(1, field->message_type()->name().c_str());
      } else {
        item->setText(1, field->type_name());
      }

      item->setData(0, Qt::UserRole, field->cpp_type());
      item->setData(0, EditDialog::kEditValueKindRole,
                    field->cpp_type() == google::protobuf::FieldDescriptor::CPPTYPE_STRING
                        ? static_cast<int>(field->type() == google::protobuf::FieldDescriptor::TYPE_BYTES
                                               ? EditDialog::EditValueKind::kBytes
                                               : EditDialog::EditValueKind::kString)
                        : static_cast<int>(EditDialog::EditValueKind::kUnknown));
      item->setText(2, field->name().c_str());
#endif

      switch (field->cpp_type()) {
        case google::protobuf::FieldDescriptor::CPPTYPE_INT32: {
          auto value = ref->GetInt32(*msg, field);

          item->setText(3, get_str_for_number(value));

          item->setData(1, Qt::UserRole, AnalyzeDialog::kNumberType);

          if (analyze_dialog_->is_number_type() && ui->treeWidget_property->currentItem() == item) {
            analyze_dialog_->add_number(value);
          }
        } break;
        case google::protobuf::FieldDescriptor::CPPTYPE_INT64: {
          auto value = ref->GetInt64(*msg, field);

          if (ui->checkBox_time->isChecked() && field->name().find("time") != std::string::npos) {
            item->setText(3, QString::fromStdString(vlink::Helpers::format_date(value)));
          } else {
            item->setText(3, get_str_for_number(value));
          }

          item->setData(1, Qt::UserRole, AnalyzeDialog::kNumberType);

          if (analyze_dialog_->is_number_type() && ui->treeWidget_property->currentItem() == item) {
            analyze_dialog_->add_number(value);
          }
        } break;
        case google::protobuf::FieldDescriptor::CPPTYPE_UINT32: {
          auto value = ref->GetUInt32(*msg, field);
          item->setText(3, get_str_for_number(value));

          item->setData(1, Qt::UserRole, AnalyzeDialog::kNumberType);

          if (analyze_dialog_->is_number_type() && ui->treeWidget_property->currentItem() == item) {
            analyze_dialog_->add_number(value);
          }
        } break;
        case google::protobuf::FieldDescriptor::CPPTYPE_UINT64: {
          auto value = ref->GetUInt64(*msg, field);

          if (ui->checkBox_time->isChecked() && field->name().find("time") != std::string::npos) {
            item->setText(3, QString::fromStdString(vlink::Helpers::format_date(value)));
          } else {
            item->setText(3, get_str_for_number(value));
          }

          item->setData(1, Qt::UserRole, AnalyzeDialog::kNumberType);

          if (analyze_dialog_->is_number_type() && ui->treeWidget_property->currentItem() == item) {
            analyze_dialog_->add_number(value);
          }
        } break;
        case google::protobuf::FieldDescriptor::CPPTYPE_DOUBLE: {
          auto value = ref->GetDouble(*msg, field);
          item->setText(3, QString::number(value, 'g', 16));

          item->setData(1, Qt::UserRole, AnalyzeDialog::kNumberType);

          if (analyze_dialog_->is_number_type() && ui->treeWidget_property->currentItem() == item) {
            analyze_dialog_->add_number(value);
          }
        } break;
        case google::protobuf::FieldDescriptor::CPPTYPE_FLOAT: {
          auto value = ref->GetFloat(*msg, field);
          item->setText(3, QString::number(value, 'g', 8));

          item->setData(1, Qt::UserRole, AnalyzeDialog::kNumberType);

          if (analyze_dialog_->is_number_type() && ui->treeWidget_property->currentItem() == item) {
            analyze_dialog_->add_number(value);
          }
        } break;
        case google::protobuf::FieldDescriptor::CPPTYPE_BOOL: {
          auto value = ref->GetBool(*msg, field);
          item->setText(3, value ? "true" : "false");

          item->setData(1, Qt::UserRole, AnalyzeDialog::kNumberType);

          if (analyze_dialog_->is_number_type() && ui->treeWidget_property->currentItem() == item) {
            analyze_dialog_->add_number(static_cast<int>(value));
          }
        } break;
        case google::protobuf::FieldDescriptor::CPPTYPE_ENUM: {
          auto str = ref->GetEnum(*msg, field)->name();
          auto value = ref->GetEnum(*msg, field)->number();

#if GOOGLE_PROTOBUF_VERSION >= 6030000
          item->setText(3, get_str_for_enum(std::string(str), value));
#else
          item->setText(3, get_str_for_enum(str, value));
#endif

          item->setData(1, Qt::UserRole, AnalyzeDialog::kNumberType);

          if (analyze_dialog_->is_number_type() && ui->treeWidget_property->currentItem() == item) {
            analyze_dialog_->add_number(value);
          }
        } break;
        case google::protobuf::FieldDescriptor::CPPTYPE_STRING: {
          if (field->type() == google::protobuf::FieldDescriptor::TYPE_BYTES) {
            item->setText(3, "{...}");
            const std::string& raw_data = ref->GetString(*msg, field);
            std::string raw_str;
            if (raw_data.size() > 1024) {
              item->setData(3, Qt::ToolTipRole, QString("Raw data is too large."));
              item->setData(3, Qt::UserRole, QString("Raw data is too large."));
            } else {
              raw_str =
                  vlink::Bytes::convert_to_hex_str(reinterpret_cast<const uint8_t*>(raw_data.data()), raw_data.size());
              item->setData(3, Qt::ToolTipRole, QString::fromStdString(raw_str));
              item->setData(3, Qt::UserRole, QString::fromStdString(raw_str));
            }

            item->setData(1, Qt::UserRole, AnalyzeDialog::kRawType);

            if (analyze_dialog_->is_raw_type() && ui->treeWidget_property->currentItem() == item) {
              if (raw_str.empty()) {
                raw_str = vlink::Bytes::convert_to_hex_str(reinterpret_cast<const uint8_t*>(raw_data.data()),
                                                           raw_data.size());
              }

              analyze_dialog_->add_raw(raw_str);
            }
          } else {
            const std::string& str_data = ref->GetString(*msg, field);

            item->setText(3, QString::fromStdString(str_data));

            item->setData(1, Qt::UserRole, AnalyzeDialog::kStringType);

            if (analyze_dialog_->is_string_type() && ui->treeWidget_property->currentItem() == item) {
              analyze_dialog_->add_string(str_data);
            }
          }
        } break;
        case google::protobuf::FieldDescriptor::CPPTYPE_MESSAGE: {
          get_property_list(widget, current_id, &ref->GetMessage(*msg, field));
        } break;
        default:
          break;
      }

      item->setData(0, Qt::ToolTipRole, item->text(0));
      item->setData(1, Qt::ToolTipRole, item->text(1));
      item->setData(2, Qt::ToolTipRole, item->text(2));

      if (item->text(3) != "{...}") {
        if (field->cpp_type() == google::protobuf::FieldDescriptor::CPPTYPE_ENUM) {
#if GOOGLE_PROTOBUF_VERSION >= 6030000
          item->setData(3, Qt::ToolTipRole, QString::fromStdString(ref->GetEnum(*msg, field)->name().data()));
#else
          item->setData(3, Qt::ToolTipRole, QString::fromStdString(ref->GetEnum(*msg, field)->name()));
#endif
        } else {
          item->setData(3, Qt::ToolTipRole, item->text(3));
        }
      }

    } else {
      if (!ui->checkBox_array->isChecked()) {
        int field_size = ref->FieldSize(*msg, field);

        auto* array_item = get_item_property(widget, parent_id, current_id);
        item_to_msg_map_[array_item] = const_cast<google::protobuf::Message*>(msg);
        item_to_field_map_[array_item] = const_cast<google::protobuf::FieldDescriptor*>(field);

        array_item->setText(0, QString::number(field->number()));

#if GOOGLE_PROTOBUF_VERSION >= 6030000

        if (field->is_map()) {
          if (field->message_type()) {
            array_item->setText(
                1, QString("%1{%2}").arg(field->message_type()->name().data(), QString::number(field_size)));
          } else {
            array_item->setText(1, QString("%1{%2}").arg(field->type_name().data(), QString::number(field_size)));
          }
        } else {
          if (field->message_type()) {
            array_item->setText(
                1, QString("%1[%2]").arg(field->message_type()->name().data(), QString::number(field_size)));
          } else {
            array_item->setText(1, QString("%1[%2]").arg(field->type_name().data(), QString::number(field_size)));
          }
        }

        array_item->setText(2, field->name().data());
#else

        if (field->is_map()) {
          if (field->message_type()) {
            array_item->setText(
                1, QString("%1{%2}").arg(field->message_type()->name().c_str(), QString::number(field_size)));
          } else {
            array_item->setText(1, QString("%1{%2}").arg(field->type_name(), QString::number(field_size)));
          }
        } else {
          if (field->message_type()) {
            array_item->setText(
                1, QString("%1[%2]").arg(field->message_type()->name().c_str(), QString::number(field_size)));
          } else {
            array_item->setText(1, QString("%1[%2]").arg(field->type_name(), QString::number(field_size)));
          }
        }

        array_item->setText(2, field->name().c_str());
#endif

        array_item->setText(3, "");
        array_item->setData(0, Qt::UserRole, 100 + field->cpp_type());
        array_item->setData(0, Qt::ToolTipRole, array_item->text(0));
        array_item->setData(1, Qt::ToolTipRole, array_item->text(1));
        array_item->setData(2, Qt::ToolTipRole, array_item->text(2));
        array_item->setData(3, Qt::ToolTipRole, array_item->text(3));

        for (int j = 0; j < field_size; ++j) {
          std::string current_array_id = current_id + "." + std::to_string(j);
          auto* item = get_item_property(widget, current_id, current_array_id);
          item_to_msg_map_[item] = const_cast<google::protobuf::Message*>(msg);
          item_to_field_map_[item] = const_cast<google::protobuf::FieldDescriptor*>(field);

          item->setText(0, "");

          if (field->is_map()) {
            item->setText(1, QString("{%1}").arg(QString::number(j)));
          } else {
            item->setText(1, QString("[%1]").arg(QString::number(j)));
          }

          item->setData(0, Qt::UserRole, field->cpp_type());
          item->setText(2, "");

          switch (field->cpp_type()) {
            case google::protobuf::FieldDescriptor::CPPTYPE_INT32: {
              auto value = ref->GetRepeatedInt32(*msg, field, j);

              item->setText(3, get_str_for_number(value));

              item->setData(1, Qt::UserRole, AnalyzeDialog::kNumberType);

              if (analyze_dialog_->is_number_type() && ui->treeWidget_property->currentItem() == item) {
                analyze_dialog_->add_number(value);
              }
            } break;
            case google::protobuf::FieldDescriptor::CPPTYPE_INT64: {
              auto value = ref->GetRepeatedInt64(*msg, field, j);

              if (ui->checkBox_time->isChecked() && field->name().find("time") != std::string::npos) {
                item->setText(3, QString::fromStdString(vlink::Helpers::format_date(value)));
              } else {
                item->setText(3, get_str_for_number(value));
              }

              item->setData(1, Qt::UserRole, AnalyzeDialog::kNumberType);

              if (analyze_dialog_->is_number_type() && ui->treeWidget_property->currentItem() == item) {
                analyze_dialog_->add_number(value);
              }
            } break;
            case google::protobuf::FieldDescriptor::CPPTYPE_UINT32: {
              auto value = ref->GetRepeatedUInt32(*msg, field, j);
              item->setText(3, get_str_for_number(value));

              item->setData(1, Qt::UserRole, AnalyzeDialog::kNumberType);

              if (analyze_dialog_->is_number_type() && ui->treeWidget_property->currentItem() == item) {
                analyze_dialog_->add_number(value);
              }
            } break;
            case google::protobuf::FieldDescriptor::CPPTYPE_UINT64: {
              auto value = ref->GetRepeatedUInt64(*msg, field, j);

              if (ui->checkBox_time->isChecked() && field->name().find("time") != std::string::npos) {
                item->setText(3, QString::fromStdString(vlink::Helpers::format_date(value)));
              } else {
                item->setText(3, get_str_for_number(value));
              }

              item->setData(1, Qt::UserRole, AnalyzeDialog::kNumberType);

              if (analyze_dialog_->is_number_type() && ui->treeWidget_property->currentItem() == item) {
                analyze_dialog_->add_number(value);
              }
            } break;
            case google::protobuf::FieldDescriptor::CPPTYPE_DOUBLE: {
              auto value = ref->GetRepeatedDouble(*msg, field, j);
              item->setText(3, QString::number(value, 'g', 16));

              item->setData(1, Qt::UserRole, AnalyzeDialog::kNumberType);

              if (analyze_dialog_->is_number_type() && ui->treeWidget_property->currentItem() == item) {
                analyze_dialog_->add_number(value);
              }
            } break;
            case google::protobuf::FieldDescriptor::CPPTYPE_FLOAT: {
              auto value = ref->GetRepeatedFloat(*msg, field, j);
              item->setText(3, QString::number(value, 'g', 8));

              item->setData(1, Qt::UserRole, AnalyzeDialog::kNumberType);

              if (analyze_dialog_->is_number_type() && ui->treeWidget_property->currentItem() == item) {
                analyze_dialog_->add_number(value);
              }
            } break;
            case google::protobuf::FieldDescriptor::CPPTYPE_BOOL: {
              auto value = ref->GetRepeatedBool(*msg, field, j);
              item->setText(3, value ? "true" : "false");

              item->setData(1, Qt::UserRole, AnalyzeDialog::kNumberType);

              if (analyze_dialog_->is_number_type() && ui->treeWidget_property->currentItem() == item) {
                analyze_dialog_->add_number(static_cast<int>(value));
              }
            } break;
            case google::protobuf::FieldDescriptor::CPPTYPE_ENUM: {
              auto str = ref->GetRepeatedEnum(*msg, field, j)->name();
              auto value = ref->GetRepeatedEnum(*msg, field, j)->number();

#if GOOGLE_PROTOBUF_VERSION >= 6030000
              item->setText(3, get_str_for_enum(std::string(str), value));
#else
              item->setText(3, get_str_for_enum(str, value));
#endif

              item->setData(1, Qt::UserRole, AnalyzeDialog::kNumberType);

              if (analyze_dialog_->is_number_type() && ui->treeWidget_property->currentItem() == item) {
                analyze_dialog_->add_number(value);
              }
            } break;
            case google::protobuf::FieldDescriptor::CPPTYPE_STRING: {
              if (field->type() == google::protobuf::FieldDescriptor::TYPE_BYTES) {
                item->setText(3, "{...}");
                const std::string& raw_data = ref->GetRepeatedString(*msg, field, j);
                std::string raw_str;
                if (raw_data.size() > 1024) {
                  item->setData(3, Qt::ToolTipRole, QString("Raw data is too large."));
                  item->setData(3, Qt::UserRole, QString("Raw data is too large."));
                } else {
                  raw_str = vlink::Bytes::convert_to_hex_str(reinterpret_cast<const uint8_t*>(raw_data.data()),
                                                             raw_data.size());
                  item->setData(3, Qt::ToolTipRole, QString::fromStdString(raw_str));
                  item->setData(3, Qt::UserRole, QString::fromStdString(raw_str));
                }

                item->setData(1, Qt::UserRole, AnalyzeDialog::kRawType);

                if (analyze_dialog_->is_raw_type() && ui->treeWidget_property->currentItem() == item) {
                  if (raw_str.empty()) {
                    raw_str = vlink::Bytes::convert_to_hex_str(reinterpret_cast<const uint8_t*>(raw_data.data()),
                                                               raw_data.size());
                  }

                  analyze_dialog_->add_raw(raw_str);
                }
              } else {
                const std::string& str_data = ref->GetRepeatedString(*msg, field, j);

                item->setText(3, QString::fromStdString(str_data));

                item->setData(1, Qt::UserRole, AnalyzeDialog::kStringType);

                if (analyze_dialog_->is_string_type() && ui->treeWidget_property->currentItem() == item) {
                  analyze_dialog_->add_string(str_data);
                }
              }
            } break;
            case google::protobuf::FieldDescriptor::CPPTYPE_MESSAGE: {
              get_property_list(widget, current_array_id, &ref->GetRepeatedMessage(*msg, field, j));
            } break;
            default:
              break;
          }

          item->setData(0, Qt::ToolTipRole, item->text(0));
          item->setData(1, Qt::ToolTipRole, item->text(1));
          item->setData(2, Qt::ToolTipRole, item->text(2));

          if (item->text(3) != "{...}") {
            if (field->cpp_type() == google::protobuf::FieldDescriptor::CPPTYPE_ENUM) {
#if GOOGLE_PROTOBUF_VERSION >= 6030000
              item->setData(3, Qt::ToolTipRole,
                            QString::fromStdString(ref->GetRepeatedEnum(*msg, field, j)->name().data()));
#else
              item->setData(3, Qt::ToolTipRole, QString::fromStdString(ref->GetRepeatedEnum(*msg, field, j)->name()));
#endif
            } else {
              item->setData(3, Qt::ToolTipRole, item->text(3));
            }
          }
        }
      }
    }
  }

  if (!msg->IsInitialized()) {
    return false;
  }

  return true;
}

bool MainWindow::set_property_list(QTreeWidget* widget, const std::string& parent_id, google::protobuf::Message* msg) {
  auto* ref = msg->GetReflection();
  for (int i = 0; i < msg->GetDescriptor()->field_count(); ++i) {
    std::string current_id = parent_id + "." + std::to_string(i);
    const auto* field = msg->GetDescriptor()->field(i);

    if (!field->is_repeated()) {
      auto* item = get_item_property(widget, parent_id, current_id);
      item_to_msg_map_[item] = const_cast<google::protobuf::Message*>(msg);
      item_to_field_map_[item] = const_cast<google::protobuf::FieldDescriptor*>(field);

      item->setText(0, QString::number(field->number()));

#if GOOGLE_PROTOBUF_VERSION >= 6030000

      if (field->message_type()) {
        item->setText(1, field->message_type()->name().data());
      } else {
        item->setText(1, field->type_name().data());
      }

      item->setData(0, Qt::UserRole, field->cpp_type());
      item->setData(0, EditDialog::kEditValueKindRole,
                    field->cpp_type() == google::protobuf::FieldDescriptor::CPPTYPE_STRING
                        ? static_cast<int>(field->type() == google::protobuf::FieldDescriptor::TYPE_BYTES
                                               ? EditDialog::EditValueKind::kBytes
                                               : EditDialog::EditValueKind::kString)
                        : static_cast<int>(EditDialog::EditValueKind::kUnknown));
      item->setText(2, field->name().data());
#else

      if (field->message_type()) {
        item->setText(1, field->message_type()->name().c_str());
      } else {
        item->setText(1, field->type_name());
      }

      item->setData(0, Qt::UserRole, field->cpp_type());
      item->setData(0, EditDialog::kEditValueKindRole,
                    field->cpp_type() == google::protobuf::FieldDescriptor::CPPTYPE_STRING
                        ? static_cast<int>(field->type() == google::protobuf::FieldDescriptor::TYPE_BYTES
                                               ? EditDialog::EditValueKind::kBytes
                                               : EditDialog::EditValueKind::kString)
                        : static_cast<int>(EditDialog::EditValueKind::kUnknown));
      item->setText(2, field->name().c_str());
#endif

      switch (field->cpp_type()) {
        case google::protobuf::FieldDescriptor::CPPTYPE_INT32: {
          if (item->text(3).isEmpty()) {
            item->setText(3, "0");
          }

          ref->SetInt32(msg, field, get_int_for_str(item->text(3)));
        } break;
        case google::protobuf::FieldDescriptor::CPPTYPE_INT64: {
          if (item->text(3).isEmpty()) {
            item->setText(3, "0");
          }

          ref->SetInt64(msg, field, get_longlong_for_str(item->text(3)));
        } break;
        case google::protobuf::FieldDescriptor::CPPTYPE_UINT32: {
          if (item->text(3).isEmpty()) {
            item->setText(3, "0");
          }

          ref->SetUInt32(msg, field, get_uint_for_str(item->text(3)));
        } break;
        case google::protobuf::FieldDescriptor::CPPTYPE_UINT64: {
          if (item->text(3).isEmpty()) {
            item->setText(3, "0");
          }

          ref->SetUInt64(msg, field, get_ulonglong_for_str(item->text(3)));
        } break;
        case google::protobuf::FieldDescriptor::CPPTYPE_DOUBLE: {
          if (item->text(3).isEmpty()) {
            item->setText(3, "0");
          }

          ref->SetDouble(msg, field, item->text(3).toDouble());
        } break;
        case google::protobuf::FieldDescriptor::CPPTYPE_FLOAT: {
          if (item->text(3).isEmpty()) {
            item->setText(3, "0");
          }

          ref->SetFloat(msg, field, item->text(3).toFloat());
        } break;
        case google::protobuf::FieldDescriptor::CPPTYPE_BOOL: {
          if (item->text(3).isEmpty()) {
            item->setText(3, "false");
          }

          ref->SetBool(msg, field, item->text(3) == "true");
        } break;
        case google::protobuf::FieldDescriptor::CPPTYPE_ENUM: {
          if (item->text(3).isEmpty()) {
            item->setText(3, "0");
          }

          ref->SetEnumValue(msg, field, item->text(3).toInt());
        } break;
        case google::protobuf::FieldDescriptor::CPPTYPE_STRING: {
          ref->SetString(msg, field, item->text(3).toStdString());
        } break;
        case google::protobuf::FieldDescriptor::CPPTYPE_MESSAGE: {
          if (!set_property_list(widget, current_id, ref->MutableMessage(msg, field))) {
            return false;
          }
        } break;
        default:
          return false;
      }

      item->setData(0, Qt::ToolTipRole, item->text(0));
      item->setData(1, Qt::ToolTipRole, item->text(1));
      item->setData(2, Qt::ToolTipRole, item->text(2));
      item->setData(3, Qt::ToolTipRole, item->text(3));
    } else {
      int field_size = ref->FieldSize(*msg, field);

      auto* array_item = get_item_property(widget, parent_id, current_id);
      item_to_msg_map_[array_item] = const_cast<google::protobuf::Message*>(msg);
      item_to_field_map_[array_item] = const_cast<google::protobuf::FieldDescriptor*>(field);

      array_item->setText(0, QString::number(field->number()));

#if GOOGLE_PROTOBUF_VERSION >= 6030000

      if (field->is_map()) {
        if (field->message_type()) {
          array_item->setText(1,
                              QString("%1{%2}").arg(field->message_type()->name().data(), QString::number(field_size)));
        } else {
          array_item->setText(1, QString("%1{%2}").arg(field->type_name().data(), QString::number(field_size)));
        }
      } else {
        if (field->message_type()) {
          array_item->setText(1,
                              QString("%1[%2]").arg(field->message_type()->name().data(), QString::number(field_size)));
        } else {
          array_item->setText(1, QString("%1[%2]").arg(field->type_name().data(), QString::number(field_size)));
        }
      }

      array_item->setText(2, field->name().data());
#else

      if (field->is_map()) {
        if (field->message_type()) {
          array_item->setText(
              1, QString("%1{%2}").arg(field->message_type()->name().c_str(), QString::number(field_size)));
        } else {
          array_item->setText(1, QString("%1{%2}").arg(field->type_name(), QString::number(field_size)));
        }
      } else {
        if (field->message_type()) {
          array_item->setText(
              1, QString("%1[%2]").arg(field->message_type()->name().c_str(), QString::number(field_size)));
        } else {
          array_item->setText(1, QString("%1[%2]").arg(field->type_name(), QString::number(field_size)));
        }
      }

      array_item->setText(2, field->name().c_str());
#endif
      array_item->setText(3, "");
      array_item->setData(0, Qt::UserRole, 100 + field->cpp_type());
      array_item->setData(0, EditDialog::kEditValueKindRole, static_cast<int>(EditDialog::EditValueKind::kUnknown));
      array_item->setData(0, Qt::ToolTipRole, array_item->text(0));
      array_item->setData(1, Qt::ToolTipRole, array_item->text(1));
      array_item->setData(2, Qt::ToolTipRole, array_item->text(2));
      array_item->setData(3, Qt::ToolTipRole, array_item->text(3));

      for (int j = 0; j < field_size; ++j) {
        std::string current_array_id = current_id + "." + std::to_string(j);
        auto* item = get_item_property(widget, current_id, current_array_id);
        item_to_msg_map_[item] = const_cast<google::protobuf::Message*>(msg);
        item_to_field_map_[item] = const_cast<google::protobuf::FieldDescriptor*>(field);

        item->setText(0, "");

        if (field->is_map()) {
          item->setText(1, QString("{%1}").arg(QString::number(j)));
        } else {
          item->setText(1, QString("[%1]").arg(QString::number(j)));
        }

        item->setData(0, Qt::UserRole, field->cpp_type());
        item->setData(0, EditDialog::kEditValueKindRole,
                      field->cpp_type() == google::protobuf::FieldDescriptor::CPPTYPE_STRING
                          ? static_cast<int>(field->type() == google::protobuf::FieldDescriptor::TYPE_BYTES
                                                 ? EditDialog::EditValueKind::kBytes
                                                 : EditDialog::EditValueKind::kString)
                          : static_cast<int>(EditDialog::EditValueKind::kUnknown));
        item->setText(2, "");

        switch (field->cpp_type()) {
          case google::protobuf::FieldDescriptor::CPPTYPE_INT32: {
            if (item->text(3).isEmpty()) {
              item->setText(3, "0");
            }

            ref->SetRepeatedInt32(msg, field, j, get_int_for_str(item->text(3)));
          } break;
          case google::protobuf::FieldDescriptor::CPPTYPE_INT64: {
            if (item->text(3).isEmpty()) {
              item->setText(3, "0");
            }

            ref->SetRepeatedInt64(msg, field, j, get_longlong_for_str(item->text(3)));
          } break;
          case google::protobuf::FieldDescriptor::CPPTYPE_UINT32: {
            if (item->text(3).isEmpty()) {
              item->setText(3, "0");
            }

            ref->SetRepeatedUInt32(msg, field, j, get_uint_for_str(item->text(3)));
          } break;
          case google::protobuf::FieldDescriptor::CPPTYPE_UINT64: {
            if (item->text(3).isEmpty()) {
              item->setText(3, "0");
            }

            ref->SetRepeatedUInt64(msg, field, j, get_ulonglong_for_str(item->text(3)));
          } break;
          case google::protobuf::FieldDescriptor::CPPTYPE_DOUBLE: {
            if (item->text(3).isEmpty()) {
              item->setText(3, "0");
            }

            ref->SetRepeatedDouble(msg, field, j, item->text(3).toDouble());
          } break;
          case google::protobuf::FieldDescriptor::CPPTYPE_FLOAT: {
            if (item->text(3).isEmpty()) {
              item->setText(3, "0");
            }

            ref->SetRepeatedFloat(msg, field, j, item->text(3).toFloat());
          } break;
          case google::protobuf::FieldDescriptor::CPPTYPE_BOOL: {
            if (item->text(3).isEmpty()) {
              item->setText(3, "false");
            }

            ref->SetRepeatedBool(msg, field, j, item->text(3) == "true");
          } break;
          case google::protobuf::FieldDescriptor::CPPTYPE_ENUM: {
            if (item->text(3).isEmpty()) {
              item->setText(3, "0");
            }

            ref->SetRepeatedEnumValue(msg, field, j, item->text(3).toInt());
          } break;
          case google::protobuf::FieldDescriptor::CPPTYPE_STRING: {
            ref->SetRepeatedString(msg, field, j, item->text(3).toStdString());
          } break;
          case google::protobuf::FieldDescriptor::CPPTYPE_MESSAGE: {
            if (!set_property_list(widget, current_array_id, ref->MutableRepeatedMessage(msg, field, j))) {
              return false;
            }
          } break;
          default:
            return false;
        }

        item->setData(0, Qt::ToolTipRole, item->text(0));
        item->setData(1, Qt::ToolTipRole, item->text(1));
        item->setData(2, Qt::ToolTipRole, item->text(2));
        item->setData(3, Qt::ToolTipRole, item->text(3));
      }

      std::vector<std::string> del_list;

      while (array_item->childCount() > field_size) {
        auto* delete_item = array_item->takeChild(array_item->childCount() - 1);
        del_list.clear();

        for (const auto& [t, pitem] : id_to_item_map_) {
          QTreeWidgetItem* target_item = pitem;
          while (target_item) {
            if (delete_item == target_item) {
              del_list.emplace_back(t);
            }

            target_item = target_item->parent();
          }
        }

        for (const auto& t : del_list) {
          id_to_item_map_.erase(t);
        }

        item_to_msg_map_.erase(delete_item);
        item_to_field_map_.erase(delete_item);
        delete delete_item;
      }
    }
  }

  return true;
}

bool MainWindow::get_flatbuffers_property_list(QTreeWidget* widget, const std::string& parent_id,
                                               const FlatbuffersObjectView& view, const FlatbuffersSchemaContext& ctx) {
  if (ui->checkBox_perf->isChecked() && property_timer_.restart() > 100) {
    qApp->processEvents();
  }

  if (!view.valid() || !view.object || !view.object->fields()) {
    return false;
  }

  std::vector<const reflection::Field*> ordered_fields;
  ordered_fields.reserve(view.object->fields()->size());
  bool need_sort = false;
  bool has_prev_id = false;
  int prev_id = 0;

  for (unsigned i = 0; i < view.object->fields()->size(); ++i) {
    const auto* field = view.object->fields()->Get(i);

    if (!field) {
      continue;
    }

    if (has_prev_id && field->id() < prev_id) {
      need_sort = true;
    }

    prev_id = field->id();
    has_prev_id = true;
    ordered_fields.emplace_back(field);
  }

  if (need_sort) {
    std::sort(ordered_fields.begin(), ordered_fields.end(),
              [](const reflection::Field* lhs, const reflection::Field* rhs) { return lhs->id() < rhs->id(); });
  }

  auto update_scalar_item = [this](QTreeWidgetItem* item, const reflection::Field& field, const QString& value_text,
                                   int analyze_type, const vlink::Function<void()>& analyze_func) {
    (void)field;
    item->setText(3, value_text);
    item->setData(1, Qt::UserRole, analyze_type);
    item->setData(0, Qt::ToolTipRole, item->text(0));
    item->setData(1, Qt::ToolTipRole, item->text(1));
    item->setData(2, Qt::ToolTipRole, item->text(2));
    item->setData(3, Qt::ToolTipRole, item->text(3));

    if (ui->treeWidget_property->currentItem() == item && analyze_func) {
      if ((analyze_type == AnalyzeDialog::kNumberType && analyze_dialog_->is_number_type()) ||
          (analyze_type == AnalyzeDialog::kStringType && analyze_dialog_->is_string_type()) ||
          (analyze_type == AnalyzeDialog::kRawType && analyze_dialog_->is_raw_type())) {
        analyze_func();
      }
    }
  };

  for (const auto* field : ordered_fields) {
    if (!field || !field->name()) {
      continue;
    }

    const std::string current_id = parent_id + "." + std::to_string(static_cast<int>(field->id()));
    const std::string field_name = field->name()->str();
    const auto base_type = field->type()->base_type();

    if ((base_type == reflection::Vector || base_type == reflection::Vector64) && ui->checkBox_array->isChecked()) {
      continue;
    }

    if (base_type == reflection::Vector || base_type == reflection::Vector64) {
      auto* array_item = get_item_property(widget, parent_id, current_id);
      size_t field_size = get_vector_size(view, *field);
      const auto element_type = field->type()->element();
      const bool is_enum_vector = is_enum_field(*field, *ctx.schema);
      const bool bytes_like =
          (element_type == reflection::Byte || element_type == reflection::UByte) && !is_enum_vector;

      array_item->setText(0, QString::number(static_cast<int>(field->id())));
      array_item->setText(1, QString::fromStdString(get_field_type_name(*field, *ctx.schema)));
      array_item->setText(2, QString::fromStdString(field_name));
      array_item->setData(0, Qt::UserRole, 100);

      if (bytes_like) {
        array_item->setText(3, QString("{bytes:%1}").arg(QString::number(field_size)));
        array_item->setData(1, Qt::UserRole, AnalyzeDialog::kRawType);
        array_item->setData(3, Qt::ToolTipRole, array_item->text(3));
        continue;
      }

      array_item->setText(3, "");
      array_item->setData(0, Qt::ToolTipRole, array_item->text(0));
      array_item->setData(1, Qt::ToolTipRole, array_item->text(1));
      array_item->setData(2, Qt::ToolTipRole, array_item->text(2));
      array_item->setData(3, Qt::ToolTipRole, array_item->text(3));

      for (size_t j = 0; j < field_size; ++j) {
        const std::string current_array_id = current_id + "." + std::to_string(j);
        auto* item = get_item_property(widget, current_id, current_array_id);
        item->setText(0, "");
        item->setText(1, QString("[%1]").arg(QString::number(static_cast<int>(j))));
        item->setText(2, "");
        item->setData(0, Qt::UserRole, 0);

        if (element_type == reflection::Obj) {
          FlatbuffersObjectView child_view;
          if (!get_vector_elem_view(view, *field, j, *ctx.schema, child_view)) {
            item->setText(3, "");
            continue;
          }

          item->setText(3, "");
          item->setData(0, Qt::ToolTipRole, item->text(0));
          item->setData(1, Qt::ToolTipRole, item->text(1));
          item->setData(2, Qt::ToolTipRole, item->text(2));
          item->setData(3, Qt::ToolTipRole, item->text(3));

          get_flatbuffers_property_list(widget, current_array_id, child_view, ctx);
          continue;
        }

        if (element_type == reflection::String) {
          const auto* vec = flatbuffers::GetFieldAnyV(*reinterpret_cast<const flatbuffers::Table*>(view.data), *field);
          if (!vec || j >= vec->size()) {
            item->setText(3, "");
            continue;
          }

          const std::string value = flatbuffers::GetAnyVectorElemS(vec, element_type, j);
          update_scalar_item(item, *field, QString::fromStdString(value), AnalyzeDialog::kStringType,
                             [this, value]() { analyze_dialog_->add_string(value); });
          continue;
        }

        if (element_type == reflection::Bool || element_type == reflection::Byte || element_type == reflection::UByte ||
            element_type == reflection::Short || element_type == reflection::UShort ||
            element_type == reflection::Int || element_type == reflection::UInt || element_type == reflection::Long ||
            element_type == reflection::ULong) {
          const auto* vec = flatbuffers::GetFieldAnyV(*reinterpret_cast<const flatbuffers::Table*>(view.data), *field);
          if (!vec || j >= vec->size()) {
            item->setText(3, "");
            continue;
          }

          const auto value = flatbuffers::GetAnyVectorElemI(vec, element_type, j);
          const auto enum_name = get_enum_value_name(*field, *ctx.schema, value);
          if (!enum_name.empty()) {
            update_scalar_item(item, *field, get_str_for_enum(enum_name, value), AnalyzeDialog::kNumberType,
                               [this, value]() { analyze_dialog_->add_number(value); });
          } else {
            update_scalar_item(item, *field, get_str_for_number(value), AnalyzeDialog::kNumberType,
                               [this, value]() { analyze_dialog_->add_number(value); });
          }
          continue;
        }

        if (element_type == reflection::Float || element_type == reflection::Double) {
          const auto* vec = flatbuffers::GetFieldAnyV(*reinterpret_cast<const flatbuffers::Table*>(view.data), *field);
          if (!vec || j >= vec->size()) {
            item->setText(3, "");
            continue;
          }

          const auto value = flatbuffers::GetAnyVectorElemF(vec, element_type, j);
          update_scalar_item(item, *field, QString::number(value, 'g', element_type == reflection::Float ? 8 : 16),
                             AnalyzeDialog::kNumberType, [this, value]() { analyze_dialog_->add_number(value); });
          continue;
        }

        item->setText(3, QString::fromStdString(get_field_type_name(*field, *ctx.schema)));
      }

      continue;
    }

    auto* item = get_item_property(widget, parent_id, current_id);
    item->setText(0, QString::number(static_cast<int>(field->id())));
    item->setText(1, QString::fromStdString(get_field_type_name(*field, *ctx.schema)));
    item->setText(2, QString::fromStdString(field_name));
    item->setData(0, Qt::UserRole, 0);

    if (base_type == reflection::Obj) {
      FlatbuffersObjectView child_view;
      if (!get_child_view(view, *field, *ctx.schema, child_view)) {
        item->setText(3, "");
        continue;
      }

      item->setText(3, "");
      item->setData(0, Qt::ToolTipRole, item->text(0));
      item->setData(1, Qt::ToolTipRole, item->text(1));
      item->setData(2, Qt::ToolTipRole, item->text(2));
      item->setData(3, Qt::ToolTipRole, item->text(3));

      get_flatbuffers_property_list(widget, current_id, child_view, ctx);
      continue;
    }

    if (base_type == reflection::String) {
      const std::string value = get_string(view, *field, ctx.schema);
      update_scalar_item(item, *field, QString::fromStdString(value), AnalyzeDialog::kStringType,
                         [this, value]() { analyze_dialog_->add_string(value); });
      continue;
    }

    if (base_type == reflection::Bool || base_type == reflection::Byte || base_type == reflection::UByte ||
        base_type == reflection::Short || base_type == reflection::UShort || base_type == reflection::Int ||
        base_type == reflection::UInt || base_type == reflection::Long || base_type == reflection::ULong) {
      auto value = get_numeric(view, *field);
      if (!value.has_value()) {
        item->setText(3, "");
        continue;
      }

      qlonglong int_value = static_cast<qlonglong>(value.value());
      if (ui->checkBox_time->isChecked() && field_name.find("time") != std::string::npos) {
        update_scalar_item(item, *field, QString::fromStdString(vlink::Helpers::format_date(int_value)),
                           AnalyzeDialog::kNumberType, [this, int_value]() { analyze_dialog_->add_number(int_value); });
      } else if (base_type == reflection::Bool) {
        update_scalar_item(item, *field, int_value != 0 ? "true" : "false", AnalyzeDialog::kNumberType,
                           [this, int_value]() { analyze_dialog_->add_number(int_value); });
      } else {
        const auto enum_name = get_enum_value_name(*field, *ctx.schema, int_value);
        if (!enum_name.empty()) {
          update_scalar_item(item, *field, get_str_for_enum(enum_name, int_value), AnalyzeDialog::kNumberType,
                             [this, int_value]() { analyze_dialog_->add_number(int_value); });
        } else {
          update_scalar_item(item, *field, get_str_for_number(int_value), AnalyzeDialog::kNumberType,
                             [this, int_value]() { analyze_dialog_->add_number(int_value); });
        }
      }
      continue;
    }

    if (base_type == reflection::Float || base_type == reflection::Double) {
      auto value = get_numeric(view, *field);
      if (!value.has_value()) {
        item->setText(3, "");
        continue;
      }

      update_scalar_item(item, *field, QString::number(value.value(), 'g', base_type == reflection::Float ? 8 : 16),
                         AnalyzeDialog::kNumberType, [this, value]() { analyze_dialog_->add_number(value.value()); });
      continue;
    }

    item->setText(3, QString::fromStdString(get_string(view, *field, ctx.schema)));
    item->setData(1, Qt::UserRole, AnalyzeDialog::kUnknownType);
    item->setData(0, Qt::ToolTipRole, item->text(0));
    item->setData(1, Qt::ToolTipRole, item->text(1));
    item->setData(2, Qt::ToolTipRole, item->text(2));
    item->setData(3, Qt::ToolTipRole, item->text(3));
  }

  return true;
}

QTreeWidgetItem* MainWindow::get_item_property(class QTreeWidget* widget, const std::string& parent_id,
                                               const std::string& id) {
  QTreeWidgetItem* item = nullptr;

  auto iter = id_to_item_map_.find(id);

  if (iter == id_to_item_map_.end()) {
    auto parent_iter = id_to_item_map_.find(parent_id);
    if (parent_iter == id_to_item_map_.end()) {
      item = new QTreeWidgetItem;
      widget->addTopLevelItem(item);
    } else {
      item = new QTreeWidgetItem;
      parent_iter->second->addChild(item);
    }
    // item->setFlags(item->flags() | Qt::ItemIsEditable);
    // item->setFlags(item->flags() & ~Qt::ItemIsSelectable);
    item->setExpanded(true);
    all_item_list_.emplace(item);
    id_to_item_map_.emplace(id, item);
  } else {
    item = iter->second;
    to_hide_item_list_.erase(item);
  }

  item->setHidden(false);

  if (widget != ui->treeWidget_property) {
    item->setFlags(item->flags() | Qt::ItemIsSelectable | Qt::ItemIsEditable);
  }

  return item;
}

void MainWindow::clear_all_url_item() {
  for (int i = 0; i < ui->treeWidget_url->topLevelItemCount(); ++i) {
    auto* item = ui->treeWidget_url->takeTopLevelItem(i);
    --i;
    delete item;
  }

  ui->treeWidget_url->clear();
}

void MainWindow::clear_all_property_item(class QTreeWidget* widget) {
  for (int i = 0; i < widget->topLevelItemCount(); ++i) {
    auto* item = widget->takeTopLevelItem(i);
    remove_all_item(item);
    --i;
  }

  widget->clear();

  id_to_item_map_.clear();
  item_to_msg_map_.clear();
  item_to_field_map_.clear();
  all_item_list_.clear();
  to_hide_item_list_.clear();

  raw_item_map_.clear();
  camera_item_map_.clear();
  pcl_item_map_.clear();
  occupancy_item_map_.clear();
  tensor_item_map_.clear();
  object_array_item_map_.clear();
  audio_item_map_.clear();
}

void MainWindow::clear_all_process_item() {
  for (int i = 0; i < ui->treeWidget_process1->topLevelItemCount(); ++i) {
    auto* item = ui->treeWidget_process1->takeTopLevelItem(i);
    delete item;
    --i;
  }

  for (int i = 0; i < ui->treeWidget_process2->topLevelItemCount(); ++i) {
    auto* item = ui->treeWidget_process2->takeTopLevelItem(i);
    delete item;
    --i;
  }

  ui->treeWidget_process1->clear();
  ui->treeWidget_process2->clear();
}

void MainWindow::remove_all_item(QTreeWidgetItem* item) {
  int count = item->childCount();

  if (count == 0) {
    delete item;
    return;
  }

  for (int i = 0; i < count; i++) {
    QTreeWidgetItem* child_item = item->child(0);
    remove_all_item(child_item);
  }

  delete item;
}

void MainWindow::message_box_todo(class QWidget* parent) {
  QMessageBox::information(parent, tr("Prompt"), tr("This feature will be implemented in the next version."));
}

void MainWindow::update_status_bar(int total, int active, int select, int64_t rate, double loss) {
  QString total_str = total >= 0 ? QString::number(total) : "---";
  QString active_str = active >= 0 ? QString::number(active) : "---";
  QString select_str = select >= 0 ? QString::number(select) : "---";
  QString rate_str;

  if (rate < 0) {
    rate_str = "---";
  } else if (rate < 1024) {
    rate_str = QString::number(rate) + "B/s";
  } else if (rate < 1024LL * 1024) {
    rate_str = QString::number(rate / 1024.0, 'f', 2) + "KB/s";
  } else if (rate < 1024LL * 1024 * 1024) {
    rate_str = QString::number(rate / 1024.0 / 1024.0, 'f', 2) + "MB/s";
  } else {
    rate_str = QString::number(rate / 1024.0 / 1024.0 / 1024.0, 'f', 2) + "GB/s";
  }

  QString loss_str;

  QString latency_str;

  if (proxy_->get_current_config().direct) {
    loss_str = "---";
    latency_str = "---";
  } else {
    if (loss > 1) {
      loss = 0;
    }

    int64_t proxy_latency = 0;

    loss_str = loss >= 0 ? (QString::number(loss * 100, 'f', 2) + "%") : "---";

    if (total_data_seq_ > 0) {
      proxy_latency = total_data_latency_ / total_data_seq_;
    }

    if (total_data_seq_ == 0) {
      latency_str = "---";
    } else if (proxy_latency > 5000'000'000 || proxy_latency < -500'000) {
      latency_str = "N/A";
    } else if (proxy_latency < 0) {
      latency_str = "0ms";
    } else {
      latency_str = loss >= 0 ? (QString::number(proxy_latency / 1000'000.0, 'f', 2) + "ms") : "---";
    }

    total_data_seq_ = 0;
    total_data_latency_ = 0;
  }

  QString status_str = tr("   Total Count: %1   |   Active Count: %2   |   Select Count: %3   |   Total Rate: %4   |   "
                          "Proxy Loss: %5   |   Proxy Latency: %6")
                           .arg(total_str, active_str, select_str, rate_str, loss_str, latency_str);

  status_label1_->setText(status_str);
}

void MainWindow::check_new_version() {
  if (network_manager_) {
    return;
  }

  network_manager_ = new QNetworkAccessManager(this);
  network_manager_->setProxy(QNetworkProxy::NoProxy);

#if QT_VERSION >= QT_VERSION_CHECK(5, 15, 0)
  network_manager_->setTransferTimeout(5000);
#endif

  QObject::connect(network_manager_, &QNetworkAccessManager::sslErrors, this,
                   [](QNetworkReply* reply, const QList<QSslError>&) { reply->ignoreSslErrors(); });

  QObject::connect(network_manager_, &QNetworkAccessManager::finished, this, [this](QNetworkReply* reply) {
    if (reply->error() != QNetworkReply::NoError) {
      // VLOG_W("QNetworkAccessManager Error: ", reply->errorString().toStdString());
      reply->deleteLater();
      network_manager_->deleteLater();
      network_manager_ = nullptr;
      return;
    }

    QByteArray response_data = reply->readAll();
    QJsonDocument json_doc = QJsonDocument::fromJson(response_data);

    if (!json_doc.isObject()) {
      VLOG_W("QNetworkAccessManager error: Invalid JSON response.");
      reply->deleteLater();
      network_manager_->deleteLater();
      network_manager_ = nullptr;
      return;
    }

    QJsonObject json_obj = json_doc.object();
    QString latest_version_str = json_obj.value("current_version").toString();

    vlink::Version latest_version = vlink::Version::from_string(latest_version_str.toStdString());

    if (latest_version > vlink::Version{VLINK_VERSION_MAJOR, VLINK_VERSION_MINOR, VLINK_VERSION_PATCH}) {
      QMessageBox message_box(
          QMessageBox::Information, tr("Prompt"),
          tr("A new version [%1] has been detected. \nDo you want to upgrade?").arg(latest_version_str),
          QMessageBox::Yes | QMessageBox::No, this);

      int result = message_box.exec();

      if (result == QMessageBox::Yes) {
        open_url("https://vlink.work/official_releases/" + latest_version_str + "/");
      }
    }

    reply->deleteLater();
    network_manager_->deleteLater();
    network_manager_ = nullptr;
  });

  QNetworkRequest request(QUrl("https://vlink.work/official_releases/current_version.json"));

  QSslConfiguration config = QSslConfiguration::defaultConfiguration();
  config.setProtocol(QSsl::AnyProtocol);
  config.setPeerVerifyMode(QSslSocket::VerifyNone);

  request.setSslConfiguration(config);

  network_manager_->get(request);
}

static QTreeWidgetItem* ensure_property_item(std::unordered_map<std::string, QTreeWidgetItem*>& map, QTreeWidget* tree,
                                             QTreeWidgetItem* parent, const std::string& key, const QString& type_label,
                                             const QString& field_name) {
  auto it = map.find(key);
  if (it != map.end()) {
    return it->second;
  }

  auto* item = new QTreeWidgetItem();
  map[key] = item;

  if (parent != nullptr) {
    parent->addChild(item);
  } else {
    tree->addTopLevelItem(item);
  }

  item->setText(0, "---");
  item->setText(1, type_label);
  item->setText(2, field_name);

  item->setData(0, Qt::ToolTipRole, item->text(0));
  item->setData(1, Qt::ToolTipRole, item->text(1));
  item->setData(2, Qt::ToolTipRole, item->text(2));

  return item;
}

template <typename ValueT>
static void set_property_number(QTreeWidgetItem* item, ValueT value, bool hex, AnalyzeDialog* analyze_dialog,
                                QTreeWidget* tree) {
  if (hex) {
    // Use the source value's own type so signed inputs format as e.g. "-0x1"
    // rather than the giant unsigned promotion produced by casting to uint64_t.
    item->setText(3, "0x" + QString::number(value, 16));
  } else {
    item->setText(3, QString::number(value));
  }

  item->setHidden(false);
  item->setData(3, Qt::ToolTipRole, item->text(3));
  item->setData(1, Qt::UserRole, AnalyzeDialog::kNumberType);

  if (analyze_dialog->is_number_type() && tree->currentItem() == item) {
    analyze_dialog->add_number(value);
  }
}

static void set_property_text(QTreeWidgetItem* item, const QString& value) {
  item->setText(3, value);
  item->setHidden(false);
  item->setData(3, Qt::ToolTipRole, item->text(3));
  item->setData(1, Qt::UserRole, AnalyzeDialog::kStringType);
}

void MainWindow::update_zero_copy_item_property(const vlink::Bytes& bytes) {
  QTreeWidget* property_tree = ui->treeWidget_property;
  property_tree->setUpdatesEnabled(false);

  struct PropertyUpdateGuard final {
    QTreeWidget* tree{nullptr};

    ~PropertyUpdateGuard() {
      if (tree) {
        tree->setUpdatesEnabled(true);
      }
    }
  } property_update_guard{property_tree};

  if (current_ser_.find("RawData") != std::string::npos) {
    for (const auto& [name, item] : raw_item_map_) {
      item->setHidden(true);
    }

    vlink::zerocopy::RawData raw;

    if (!(raw << bytes)) {
      return;
    }

    QTreeWidgetItem* header_root_item = raw_item_map_["header"];
    if (!header_root_item) {
      header_root_item = new QTreeWidgetItem();
      raw_item_map_["header"] = header_root_item;
      ui->treeWidget_property->addTopLevelItem(header_root_item);

      header_root_item->setText(0, "---");
      header_root_item->setText(1, "Header");
      header_root_item->setText(2, "header");

      header_root_item->setData(0, Qt::ToolTipRole, header_root_item->text(0));
      header_root_item->setData(1, Qt::ToolTipRole, header_root_item->text(1));
      header_root_item->setData(2, Qt::ToolTipRole, header_root_item->text(2));

      header_root_item->setExpanded(true);
    }

    QTreeWidgetItem* header_frame_id_item = raw_item_map_["header.frame_id"];
    if (!header_frame_id_item) {
      header_frame_id_item = new QTreeWidgetItem();
      raw_item_map_["header.frame_id"] = header_frame_id_item;
      header_root_item->addChild(header_frame_id_item);

      header_frame_id_item->setText(0, "---");
      header_frame_id_item->setText(1, "string");
      header_frame_id_item->setText(2, "frame_id");

      header_frame_id_item->setData(0, Qt::ToolTipRole, header_frame_id_item->text(0));
      header_frame_id_item->setData(1, Qt::ToolTipRole, header_frame_id_item->text(1));
      header_frame_id_item->setData(2, Qt::ToolTipRole, header_frame_id_item->text(2));
    }

    QTreeWidgetItem* header_seq_item = raw_item_map_["header.seq"];
    if (!header_seq_item) {
      header_seq_item = new QTreeWidgetItem();
      raw_item_map_["header.seq"] = header_seq_item;
      header_root_item->addChild(header_seq_item);

      header_seq_item->setText(0, "---");
      header_seq_item->setText(1, "uint32");
      header_seq_item->setText(2, "seq");

      header_seq_item->setData(0, Qt::ToolTipRole, header_seq_item->text(0));
      header_seq_item->setData(1, Qt::ToolTipRole, header_seq_item->text(1));
      header_seq_item->setData(2, Qt::ToolTipRole, header_seq_item->text(2));
    }

    QTreeWidgetItem* header_time_meas_item = raw_item_map_["header.time_meas"];
    if (!header_time_meas_item) {
      header_time_meas_item = new QTreeWidgetItem();
      raw_item_map_["header.time_meas"] = header_time_meas_item;
      header_root_item->addChild(header_time_meas_item);

      header_time_meas_item->setText(0, "---");
      header_time_meas_item->setText(1, "uint64");
      header_time_meas_item->setText(2, "time_meas");

      header_time_meas_item->setData(0, Qt::ToolTipRole, header_time_meas_item->text(0));
      header_time_meas_item->setData(1, Qt::ToolTipRole, header_time_meas_item->text(1));
      header_time_meas_item->setData(2, Qt::ToolTipRole, header_time_meas_item->text(2));
    }

    QTreeWidgetItem* header_time_pub_item = raw_item_map_["header.time_pub"];
    if (!header_time_pub_item) {
      header_time_pub_item = new QTreeWidgetItem();
      raw_item_map_["header.time_pub"] = header_time_pub_item;
      header_root_item->addChild(header_time_pub_item);

      header_time_pub_item->setText(0, "---");
      header_time_pub_item->setText(1, "uint64");
      header_time_pub_item->setText(2, "time_pub");

      header_time_pub_item->setData(0, Qt::ToolTipRole, header_time_pub_item->text(0));
      header_time_pub_item->setData(1, Qt::ToolTipRole, header_time_pub_item->text(1));
      header_time_pub_item->setData(2, Qt::ToolTipRole, header_time_pub_item->text(2));
    }

    QTreeWidgetItem* size_item = raw_item_map_["size"];
    if (!size_item) {
      size_item = new QTreeWidgetItem();
      raw_item_map_["size"] = size_item;
      ui->treeWidget_property->addTopLevelItem(size_item);

      size_item->setText(0, "---");
      size_item->setText(1, "uint64");
      size_item->setText(2, "size");

      size_item->setData(0, Qt::ToolTipRole, size_item->text(0));
      size_item->setData(1, Qt::ToolTipRole, size_item->text(1));
      size_item->setData(2, Qt::ToolTipRole, size_item->text(2));
    }

    QTreeWidgetItem* data_item = raw_item_map_["data"];
    if (!data_item) {
      data_item = new QTreeWidgetItem();
      raw_item_map_["data"] = data_item;
      ui->treeWidget_property->addTopLevelItem(data_item);

      data_item->setText(0, "---");
      data_item->setText(1, "bytes");
      data_item->setText(2, "data");

      data_item->setData(0, Qt::ToolTipRole, data_item->text(0));
      data_item->setData(1, Qt::ToolTipRole, data_item->text(1));
      data_item->setData(2, Qt::ToolTipRole, data_item->text(2));
    }

    header_frame_id_item->setText(
        3, QString::fromLatin1(raw.header.frame_id,
                               static_cast<int>(::strnlen(raw.header.frame_id, sizeof(raw.header.frame_id)))));

    if (ui->checkBox_hex->isChecked()) {
      header_seq_item->setText(3, "0x" + QString::number(raw.header.seq, 16));
      header_time_meas_item->setText(3, "0x" + QString::number(raw.header.time_meas, 16));
      header_time_pub_item->setText(3, "0x" + QString::number(raw.header.time_pub, 16));

      size_item->setText(3, "0x" + QString::number(raw.size(), 16));
    } else {
      header_seq_item->setText(3, QString::number(raw.header.seq));
      header_time_meas_item->setText(3, QString::number(raw.header.time_meas));
      header_time_pub_item->setText(3, QString::number(raw.header.time_pub));

      size_item->setText(3, QString::number(raw.size()));
    }

    header_root_item->setHidden(false);

    header_seq_item->setHidden(false);
    header_time_meas_item->setHidden(false);
    header_time_pub_item->setHidden(false);
    header_frame_id_item->setHidden(false);
    size_item->setHidden(false);

    if (ui->checkBox_time->isChecked()) {
      header_time_meas_item->setText(3, QString::fromStdString(vlink::Helpers::format_date(raw.header.time_meas)));
      header_time_pub_item->setText(3, QString::fromStdString(vlink::Helpers::format_date(raw.header.time_pub)));
    }

    data_item->setText(3, "{...}");

    data_item->setHidden(false);

    header_frame_id_item->setData(3, Qt::ToolTipRole, header_frame_id_item->text(3));
    header_seq_item->setData(3, Qt::ToolTipRole, header_seq_item->text(3));
    header_time_meas_item->setData(3, Qt::ToolTipRole, header_time_meas_item->text(3));
    header_time_pub_item->setData(3, Qt::ToolTipRole, header_time_pub_item->text(3));

    size_item->setData(3, Qt::ToolTipRole, size_item->text(3));
    data_item->setData(3, Qt::ToolTipRole, data_item->text(3));

    header_frame_id_item->setData(1, Qt::UserRole, AnalyzeDialog::kStringType);

    header_seq_item->setData(1, Qt::UserRole, AnalyzeDialog::kNumberType);
    if (analyze_dialog_->is_number_type() && ui->treeWidget_property->currentItem() == header_seq_item) {
      analyze_dialog_->add_number(raw.header.seq);
    }

    header_time_meas_item->setData(1, Qt::UserRole, AnalyzeDialog::kNumberType);
    if (analyze_dialog_->is_number_type() && ui->treeWidget_property->currentItem() == header_time_meas_item) {
      analyze_dialog_->add_number(raw.header.time_meas);
    }

    header_time_pub_item->setData(1, Qt::UserRole, AnalyzeDialog::kNumberType);
    if (analyze_dialog_->is_number_type() && ui->treeWidget_property->currentItem() == header_time_pub_item) {
      analyze_dialog_->add_number(raw.header.time_pub);
    }

    size_item->setData(1, Qt::UserRole, AnalyzeDialog::kNumberType);
    if (analyze_dialog_->is_number_type() && ui->treeWidget_property->currentItem() == size_item) {
      analyze_dialog_->add_number(raw.size());
    }

  } else if (current_ser_.find("CameraFrame") != std::string::npos) {
    for (const auto& [name, item] : camera_item_map_) {
      item->setHidden(true);
    }

    vlink::zerocopy::CameraFrame frame;

    if (!(frame << bytes)) {
      return;
    }

    QTreeWidgetItem* header_root_item = camera_item_map_["header"];
    if (!header_root_item) {
      header_root_item = new QTreeWidgetItem();
      camera_item_map_["header"] = header_root_item;
      ui->treeWidget_property->addTopLevelItem(header_root_item);

      header_root_item->setText(0, "---");
      header_root_item->setText(1, "Header");
      header_root_item->setText(2, "header");

      header_root_item->setData(0, Qt::ToolTipRole, header_root_item->text(0));
      header_root_item->setData(1, Qt::ToolTipRole, header_root_item->text(1));
      header_root_item->setData(2, Qt::ToolTipRole, header_root_item->text(2));

      header_root_item->setExpanded(true);
    }

    QTreeWidgetItem* header_frame_id_item = camera_item_map_["header.frame_id"];
    if (!header_frame_id_item) {
      header_frame_id_item = new QTreeWidgetItem();
      camera_item_map_["header.frame_id"] = header_frame_id_item;
      header_root_item->addChild(header_frame_id_item);

      header_frame_id_item->setText(0, "---");
      header_frame_id_item->setText(1, "string");
      header_frame_id_item->setText(2, "frame_id");

      header_frame_id_item->setData(0, Qt::ToolTipRole, header_frame_id_item->text(0));
      header_frame_id_item->setData(1, Qt::ToolTipRole, header_frame_id_item->text(1));
      header_frame_id_item->setData(2, Qt::ToolTipRole, header_frame_id_item->text(2));
    }

    QTreeWidgetItem* header_seq_item = camera_item_map_["header.seq"];
    if (!header_seq_item) {
      header_seq_item = new QTreeWidgetItem();
      camera_item_map_["header.seq"] = header_seq_item;
      header_root_item->addChild(header_seq_item);

      header_seq_item->setText(0, "---");
      header_seq_item->setText(1, "uint32");
      header_seq_item->setText(2, "seq");

      header_seq_item->setData(0, Qt::ToolTipRole, header_seq_item->text(0));
      header_seq_item->setData(1, Qt::ToolTipRole, header_seq_item->text(1));
      header_seq_item->setData(2, Qt::ToolTipRole, header_seq_item->text(2));
    }

    QTreeWidgetItem* header_time_meas_item = camera_item_map_["header.time_meas"];
    if (!header_time_meas_item) {
      header_time_meas_item = new QTreeWidgetItem();
      camera_item_map_["header.time_meas"] = header_time_meas_item;
      header_root_item->addChild(header_time_meas_item);

      header_time_meas_item->setText(0, "---");
      header_time_meas_item->setText(1, "uint64");
      header_time_meas_item->setText(2, "time_meas");

      header_time_meas_item->setData(0, Qt::ToolTipRole, header_time_meas_item->text(0));
      header_time_meas_item->setData(1, Qt::ToolTipRole, header_time_meas_item->text(1));
      header_time_meas_item->setData(2, Qt::ToolTipRole, header_time_meas_item->text(2));
    }

    QTreeWidgetItem* header_time_pub_item = camera_item_map_["header.time_pub"];
    if (!header_time_pub_item) {
      header_time_pub_item = new QTreeWidgetItem();
      camera_item_map_["header.time_pub"] = header_time_pub_item;
      header_root_item->addChild(header_time_pub_item);

      header_time_pub_item->setText(0, "---");
      header_time_pub_item->setText(1, "uint64");
      header_time_pub_item->setText(2, "time_pub");

      header_time_pub_item->setData(0, Qt::ToolTipRole, header_time_pub_item->text(0));
      header_time_pub_item->setData(1, Qt::ToolTipRole, header_time_pub_item->text(1));
      header_time_pub_item->setData(2, Qt::ToolTipRole, header_time_pub_item->text(2));
    }

    QTreeWidgetItem* channel_item = camera_item_map_["channel"];
    if (!channel_item) {
      channel_item = new QTreeWidgetItem();
      camera_item_map_["channel"] = channel_item;
      ui->treeWidget_property->addTopLevelItem(channel_item);

      channel_item->setText(0, "---");
      channel_item->setText(1, "uint32");
      channel_item->setText(2, "channel");

      channel_item->setData(0, Qt::ToolTipRole, channel_item->text(0));
      channel_item->setData(1, Qt::ToolTipRole, channel_item->text(1));
      channel_item->setData(2, Qt::ToolTipRole, channel_item->text(2));
    }

    QTreeWidgetItem* height_item = camera_item_map_["height"];
    if (!height_item) {
      height_item = new QTreeWidgetItem();
      camera_item_map_["height"] = height_item;
      ui->treeWidget_property->addTopLevelItem(height_item);

      height_item->setText(0, "---");
      height_item->setText(1, "uint32");
      height_item->setText(2, "height");

      height_item->setData(0, Qt::ToolTipRole, height_item->text(0));
      height_item->setData(1, Qt::ToolTipRole, height_item->text(1));
      height_item->setData(2, Qt::ToolTipRole, height_item->text(2));
    }

    QTreeWidgetItem* width_item = camera_item_map_["width"];
    if (!width_item) {
      width_item = new QTreeWidgetItem();
      camera_item_map_["width"] = width_item;
      ui->treeWidget_property->addTopLevelItem(width_item);

      width_item->setText(0, "---");
      width_item->setText(1, "uint32");
      width_item->setText(2, "width");

      width_item->setData(0, Qt::ToolTipRole, width_item->text(0));
      width_item->setData(1, Qt::ToolTipRole, width_item->text(1));
      width_item->setData(2, Qt::ToolTipRole, width_item->text(2));
    }

    QTreeWidgetItem* freq_item = camera_item_map_["freq"];
    if (!freq_item) {
      freq_item = new QTreeWidgetItem();
      camera_item_map_["freq"] = freq_item;
      ui->treeWidget_property->addTopLevelItem(freq_item);

      freq_item->setText(0, "---");
      freq_item->setText(1, "uint32");
      freq_item->setText(2, "freq");

      freq_item->setData(0, Qt::ToolTipRole, freq_item->text(0));
      freq_item->setData(1, Qt::ToolTipRole, freq_item->text(1));
      freq_item->setData(2, Qt::ToolTipRole, freq_item->text(2));
    }

    QTreeWidgetItem* format_item = camera_item_map_["format"];
    if (!format_item) {
      format_item = new QTreeWidgetItem();
      camera_item_map_["format"] = format_item;
      ui->treeWidget_property->addTopLevelItem(format_item);

      format_item->setText(0, "---");
      format_item->setText(1, "Format");
      format_item->setText(2, "format");

      format_item->setData(0, Qt::ToolTipRole, format_item->text(0));
      format_item->setData(1, Qt::ToolTipRole, format_item->text(1));
      format_item->setData(2, Qt::ToolTipRole, format_item->text(2));
    }

    QTreeWidgetItem* stream_item = camera_item_map_["stream"];
    if (!stream_item) {
      stream_item = new QTreeWidgetItem();
      camera_item_map_["stream"] = stream_item;
      ui->treeWidget_property->addTopLevelItem(stream_item);

      stream_item->setText(0, "---");
      stream_item->setText(1, "Stream");
      stream_item->setText(2, "stream");

      stream_item->setData(0, Qt::ToolTipRole, stream_item->text(0));
      stream_item->setData(1, Qt::ToolTipRole, stream_item->text(1));
      stream_item->setData(2, Qt::ToolTipRole, stream_item->text(2));
    }

    QTreeWidgetItem* size_item = camera_item_map_["size"];
    if (!size_item) {
      size_item = new QTreeWidgetItem();
      camera_item_map_["size"] = size_item;
      ui->treeWidget_property->addTopLevelItem(size_item);

      size_item->setText(0, "---");
      size_item->setText(1, "uint64");
      size_item->setText(2, "size");

      size_item->setData(0, Qt::ToolTipRole, size_item->text(0));
      size_item->setData(1, Qt::ToolTipRole, size_item->text(1));
      size_item->setData(2, Qt::ToolTipRole, size_item->text(2));
    }

    QTreeWidgetItem* data_item = camera_item_map_["data"];
    if (!data_item) {
      data_item = new QTreeWidgetItem();
      camera_item_map_["data"] = data_item;
      ui->treeWidget_property->addTopLevelItem(data_item);

      data_item->setText(0, "---");
      data_item->setText(1, "bytes");
      data_item->setText(2, "data");

      data_item->setData(0, Qt::ToolTipRole, data_item->text(0));
      data_item->setData(1, Qt::ToolTipRole, data_item->text(1));
      data_item->setData(2, Qt::ToolTipRole, data_item->text(2));
    }

    header_frame_id_item->setText(
        3, QString::fromLatin1(frame.header.frame_id,
                               static_cast<int>(::strnlen(frame.header.frame_id, sizeof(frame.header.frame_id)))));

    if (ui->checkBox_hex->isChecked()) {
      header_seq_item->setText(3, "0x" + QString::number(frame.header.seq, 16));
      header_time_meas_item->setText(3, "0x" + QString::number(frame.header.time_meas, 16));
      header_time_pub_item->setText(3, "0x" + QString::number(frame.header.time_pub, 16));

      channel_item->setText(3, "0x" + QString::number(frame.channel(), 16));
      height_item->setText(3, "0x" + QString::number(frame.height(), 16));
      width_item->setText(3, "0x" + QString::number(frame.width(), 16));
      freq_item->setText(3, "0x" + QString::number(frame.freq(), 16));
      size_item->setText(3, "0x" + QString::number(frame.size(), 16));
    } else {
      header_seq_item->setText(3, QString::number(frame.header.seq));
      header_time_meas_item->setText(3, QString::number(frame.header.time_meas));
      header_time_pub_item->setText(3, QString::number(frame.header.time_pub));

      channel_item->setText(3, QString::number(frame.channel()));
      height_item->setText(3, QString::number(frame.height()));
      width_item->setText(3, QString::number(frame.width()));
      freq_item->setText(3, QString::number(frame.freq()));
      size_item->setText(3, QString::number(frame.size()));
    }

    header_root_item->setHidden(false);

    header_seq_item->setHidden(false);
    header_time_meas_item->setHidden(false);
    header_time_pub_item->setHidden(false);
    header_frame_id_item->setHidden(false);
    channel_item->setHidden(false);
    height_item->setHidden(false);
    width_item->setHidden(false);
    freq_item->setHidden(false);
    size_item->setHidden(false);

    if (ui->checkBox_enum->isChecked()) {
      format_item->setText(3, QString::fromStdString(std::string(vlink::NameDetector::get_enum(frame.format()))));
      stream_item->setText(3, QString::fromStdString(std::string(vlink::NameDetector::get_enum(frame.stream()))));
    } else {
      format_item->setText(3, QString::number(frame.format()));
      stream_item->setText(3, QString::number(frame.stream()));
    }

    format_item->setHidden(false);
    stream_item->setHidden(false);

    if (ui->checkBox_time->isChecked()) {
      header_time_meas_item->setText(3, QString::fromStdString(vlink::Helpers::format_date(frame.header.time_meas)));
      header_time_pub_item->setText(3, QString::fromStdString(vlink::Helpers::format_date(frame.header.time_pub)));
    }

    data_item->setText(3, "{...}");

    data_item->setHidden(false);

    header_frame_id_item->setData(3, Qt::ToolTipRole, header_frame_id_item->text(3));
    header_seq_item->setData(3, Qt::ToolTipRole, header_seq_item->text(3));
    header_time_meas_item->setData(3, Qt::ToolTipRole, header_time_meas_item->text(3));
    header_time_pub_item->setData(3, Qt::ToolTipRole, header_time_pub_item->text(3));

    channel_item->setData(3, Qt::ToolTipRole, channel_item->text(3));
    height_item->setData(3, Qt::ToolTipRole, height_item->text(3));
    width_item->setData(3, Qt::ToolTipRole, width_item->text(3));
    freq_item->setData(3, Qt::ToolTipRole, freq_item->text(3));
    format_item->setData(3, Qt::ToolTipRole, format_item->text(3));
    stream_item->setData(3, Qt::ToolTipRole, stream_item->text(3));
    size_item->setData(3, Qt::ToolTipRole, size_item->text(3));
    data_item->setData(3, Qt::ToolTipRole, data_item->text(3));

    header_frame_id_item->setData(1, Qt::UserRole, AnalyzeDialog::kStringType);

    header_seq_item->setData(1, Qt::UserRole, AnalyzeDialog::kNumberType);
    if (analyze_dialog_->is_number_type() && ui->treeWidget_property->currentItem() == header_seq_item) {
      analyze_dialog_->add_number(frame.header.seq);
    }

    header_time_meas_item->setData(1, Qt::UserRole, AnalyzeDialog::kNumberType);
    if (analyze_dialog_->is_number_type() && ui->treeWidget_property->currentItem() == header_time_meas_item) {
      analyze_dialog_->add_number(frame.header.time_meas);
    }

    header_time_pub_item->setData(1, Qt::UserRole, AnalyzeDialog::kNumberType);
    if (analyze_dialog_->is_number_type() && ui->treeWidget_property->currentItem() == header_time_pub_item) {
      analyze_dialog_->add_number(frame.header.time_pub);
    }

    channel_item->setData(1, Qt::UserRole, AnalyzeDialog::kNumberType);
    if (analyze_dialog_->is_number_type() && ui->treeWidget_property->currentItem() == channel_item) {
      analyze_dialog_->add_number(frame.channel());
    }

    height_item->setData(1, Qt::UserRole, AnalyzeDialog::kNumberType);
    if (analyze_dialog_->is_number_type() && ui->treeWidget_property->currentItem() == height_item) {
      analyze_dialog_->add_number(frame.height());
    }

    width_item->setData(1, Qt::UserRole, AnalyzeDialog::kNumberType);
    if (analyze_dialog_->is_number_type() && ui->treeWidget_property->currentItem() == width_item) {
      analyze_dialog_->add_number(frame.width());
    }

    freq_item->setData(1, Qt::UserRole, AnalyzeDialog::kNumberType);
    if (analyze_dialog_->is_number_type() && ui->treeWidget_property->currentItem() == freq_item) {
      analyze_dialog_->add_number(frame.freq());
    }

    format_item->setData(1, Qt::UserRole, AnalyzeDialog::kNumberType);
    if (analyze_dialog_->is_number_type() && ui->treeWidget_property->currentItem() == format_item) {
      analyze_dialog_->add_number(frame.format());
    }

    stream_item->setData(1, Qt::UserRole, AnalyzeDialog::kNumberType);
    if (analyze_dialog_->is_number_type() && ui->treeWidget_property->currentItem() == stream_item) {
      analyze_dialog_->add_number(frame.stream());
    }

    size_item->setData(1, Qt::UserRole, AnalyzeDialog::kNumberType);
    if (analyze_dialog_->is_number_type() && ui->treeWidget_property->currentItem() == size_item) {
      analyze_dialog_->add_number(frame.size());
    }
  } else if (current_ser_.find("PointCloud") != std::string::npos) {
    for (const auto& [name, item] : pcl_item_map_) {
      item->setHidden(true);
    }

    vlink::zerocopy::PointCloud pcl;

    if (!(pcl << bytes)) {
      return;
    }

    QTreeWidgetItem* header_root_item = pcl_item_map_["header"];
    if (!header_root_item) {
      header_root_item = new QTreeWidgetItem();
      pcl_item_map_["header"] = header_root_item;
      ui->treeWidget_property->addTopLevelItem(header_root_item);

      header_root_item->setText(0, "---");
      header_root_item->setText(1, "Header");
      header_root_item->setText(2, "header");

      header_root_item->setData(0, Qt::ToolTipRole, header_root_item->text(0));
      header_root_item->setData(1, Qt::ToolTipRole, header_root_item->text(1));
      header_root_item->setData(2, Qt::ToolTipRole, header_root_item->text(2));

      header_root_item->setExpanded(true);
    }

    QTreeWidgetItem* header_frame_id_item = pcl_item_map_["header.frame_id"];
    if (!header_frame_id_item) {
      header_frame_id_item = new QTreeWidgetItem();
      pcl_item_map_["header.frame_id"] = header_frame_id_item;
      header_root_item->addChild(header_frame_id_item);

      header_frame_id_item->setText(0, "---");
      header_frame_id_item->setText(1, "string");
      header_frame_id_item->setText(2, "frame_id");

      header_frame_id_item->setData(0, Qt::ToolTipRole, header_frame_id_item->text(0));
      header_frame_id_item->setData(1, Qt::ToolTipRole, header_frame_id_item->text(1));
      header_frame_id_item->setData(2, Qt::ToolTipRole, header_frame_id_item->text(2));
    }

    QTreeWidgetItem* header_seq_item = pcl_item_map_["header.seq"];
    if (!header_seq_item) {
      header_seq_item = new QTreeWidgetItem();
      pcl_item_map_["header.seq"] = header_seq_item;
      header_root_item->addChild(header_seq_item);

      header_seq_item->setText(0, "---");
      header_seq_item->setText(1, "uint32");
      header_seq_item->setText(2, "seq");

      header_seq_item->setData(0, Qt::ToolTipRole, header_seq_item->text(0));
      header_seq_item->setData(1, Qt::ToolTipRole, header_seq_item->text(1));
      header_seq_item->setData(2, Qt::ToolTipRole, header_seq_item->text(2));
    }

    QTreeWidgetItem* header_time_meas_item = pcl_item_map_["header.time_meas"];
    if (!header_time_meas_item) {
      header_time_meas_item = new QTreeWidgetItem();
      pcl_item_map_["header.time_meas"] = header_time_meas_item;
      header_root_item->addChild(header_time_meas_item);

      header_time_meas_item->setText(0, "---");
      header_time_meas_item->setText(1, "uint64");
      header_time_meas_item->setText(2, "time_meas");

      header_time_meas_item->setData(0, Qt::ToolTipRole, header_time_meas_item->text(0));
      header_time_meas_item->setData(1, Qt::ToolTipRole, header_time_meas_item->text(1));
      header_time_meas_item->setData(2, Qt::ToolTipRole, header_time_meas_item->text(2));
    }

    QTreeWidgetItem* header_time_pub_item = pcl_item_map_["header.time_pub"];
    if (!header_time_pub_item) {
      header_time_pub_item = new QTreeWidgetItem();
      pcl_item_map_["header.time_pub"] = header_time_pub_item;
      header_root_item->addChild(header_time_pub_item);

      header_time_pub_item->setText(0, "---");
      header_time_pub_item->setText(1, "uint64");
      header_time_pub_item->setText(2, "time_pub");

      header_time_pub_item->setData(0, Qt::ToolTipRole, header_time_pub_item->text(0));
      header_time_pub_item->setData(1, Qt::ToolTipRole, header_time_pub_item->text(1));
      header_time_pub_item->setData(2, Qt::ToolTipRole, header_time_pub_item->text(2));
    }

    QTreeWidgetItem* protocol_root_item = pcl_item_map_["protocol"];
    if (!protocol_root_item) {
      protocol_root_item = new QTreeWidgetItem();
      pcl_item_map_["protocol"] = protocol_root_item;
      ui->treeWidget_property->addTopLevelItem(protocol_root_item);

      protocol_root_item->setText(0, "---");
      protocol_root_item->setText(1, "Protocol");
      protocol_root_item->setText(2, "protocol");

      protocol_root_item->setData(0, Qt::ToolTipRole, protocol_root_item->text(0));
      protocol_root_item->setData(1, Qt::ToolTipRole, protocol_root_item->text(1));
      protocol_root_item->setData(2, Qt::ToolTipRole, protocol_root_item->text(2));

      protocol_root_item->setExpanded(true);
    }

    QTreeWidgetItem* protocol_size_item = pcl_item_map_["protocol.size_list"];
    if (!protocol_size_item) {
      protocol_size_item = new QTreeWidgetItem();
      pcl_item_map_["protocol.size_list"] = protocol_size_item;
      protocol_root_item->addChild(protocol_size_item);

      protocol_size_item->setText(0, "---");
      protocol_size_item->setText(1, "string");
      protocol_size_item->setText(2, "size_list");

      protocol_size_item->setData(0, Qt::ToolTipRole, protocol_size_item->text(0));
      protocol_size_item->setData(1, Qt::ToolTipRole, protocol_size_item->text(1));
      protocol_size_item->setData(2, Qt::ToolTipRole, protocol_size_item->text(2));
    }

    QTreeWidgetItem* protocol_str_item = pcl_item_map_["protocol.name_list"];
    if (!protocol_str_item) {
      protocol_str_item = new QTreeWidgetItem();
      pcl_item_map_["protocol.name_list"] = protocol_str_item;
      protocol_root_item->addChild(protocol_str_item);

      protocol_str_item->setText(0, "---");
      protocol_str_item->setText(1, "string");
      protocol_str_item->setText(2, "name_list");

      protocol_str_item->setData(0, Qt::ToolTipRole, protocol_str_item->text(0));
      protocol_str_item->setData(1, Qt::ToolTipRole, protocol_str_item->text(1));
      protocol_str_item->setData(2, Qt::ToolTipRole, protocol_str_item->text(2));
    }

    QTreeWidgetItem* protocol_format_item = pcl_item_map_["protocol.type_list"];
    if (!protocol_format_item) {
      protocol_format_item = new QTreeWidgetItem();
      pcl_item_map_["protocol.type_list"] = protocol_format_item;
      protocol_root_item->addChild(protocol_format_item);

      protocol_format_item->setText(0, "---");
      protocol_format_item->setText(1, "string");
      protocol_format_item->setText(2, "type_list");

      protocol_format_item->setData(0, Qt::ToolTipRole, protocol_format_item->text(0));
      protocol_format_item->setData(1, Qt::ToolTipRole, protocol_format_item->text(1));
      protocol_format_item->setData(2, Qt::ToolTipRole, protocol_format_item->text(2));
    }

    QTreeWidgetItem* size_item = pcl_item_map_["size"];
    if (!size_item) {
      size_item = new QTreeWidgetItem();
      pcl_item_map_["size"] = size_item;
      ui->treeWidget_property->addTopLevelItem(size_item);

      size_item->setText(0, "---");
      size_item->setText(1, "uint64");
      size_item->setText(2, "size");

      size_item->setData(0, Qt::ToolTipRole, size_item->text(0));
      size_item->setData(1, Qt::ToolTipRole, size_item->text(1));
      size_item->setData(2, Qt::ToolTipRole, size_item->text(2));
    }

    QTreeWidgetItem* pack_size_item = pcl_item_map_["pack_size"];
    if (!pack_size_item) {
      pack_size_item = new QTreeWidgetItem();
      pcl_item_map_["pack_size"] = pack_size_item;
      ui->treeWidget_property->addTopLevelItem(pack_size_item);

      pack_size_item->setText(0, "---");
      pack_size_item->setText(1, "uint64");
      pack_size_item->setText(2, "pack_size");

      pack_size_item->setData(0, Qt::ToolTipRole, pack_size_item->text(0));
      pack_size_item->setData(1, Qt::ToolTipRole, pack_size_item->text(1));
      pack_size_item->setData(2, Qt::ToolTipRole, pack_size_item->text(2));
    }

    QTreeWidgetItem* extent_item = pcl_item_map_["extent"];
    if (!extent_item) {
      extent_item = new QTreeWidgetItem();
      pcl_item_map_["extent"] = extent_item;
      ui->treeWidget_property->addTopLevelItem(extent_item);

      extent_item->setText(0, "---");
      extent_item->setText(1, "uint16");
      extent_item->setText(2, "extent");

      extent_item->setData(0, Qt::ToolTipRole, extent_item->text(0));
      extent_item->setData(1, Qt::ToolTipRole, extent_item->text(1));
      extent_item->setData(2, Qt::ToolTipRole, extent_item->text(2));
    }

    QTreeWidgetItem* downsample_item = pcl_item_map_["downsample"];
    if (!downsample_item) {
      downsample_item = new QTreeWidgetItem();
      pcl_item_map_["downsample"] = downsample_item;
      ui->treeWidget_property->addTopLevelItem(downsample_item);

      downsample_item->setText(0, "---");
      downsample_item->setText(1, "uint8");
      downsample_item->setText(2, "downsample");

      downsample_item->setData(0, Qt::ToolTipRole, downsample_item->text(0));
      downsample_item->setData(1, Qt::ToolTipRole, downsample_item->text(1));
      downsample_item->setData(2, Qt::ToolTipRole, downsample_item->text(2));
    }

    QTreeWidgetItem* vertical_item = pcl_item_map_["vertical"];
    if (!vertical_item) {
      vertical_item = new QTreeWidgetItem();
      pcl_item_map_["vertical"] = vertical_item;
      ui->treeWidget_property->addTopLevelItem(vertical_item);

      vertical_item->setText(0, "---");
      vertical_item->setText(1, "bool");
      vertical_item->setText(2, "vertical");

      vertical_item->setData(0, Qt::ToolTipRole, vertical_item->text(0));
      vertical_item->setData(1, Qt::ToolTipRole, vertical_item->text(1));
      vertical_item->setData(2, Qt::ToolTipRole, vertical_item->text(2));
    }

    header_root_item->setHidden(false);
    protocol_root_item->setHidden(false);

    header_frame_id_item->setText(
        3, QString::fromLatin1(pcl.header.frame_id,
                               static_cast<int>(::strnlen(pcl.header.frame_id, sizeof(pcl.header.frame_id)))));

    if (ui->checkBox_hex->isChecked()) {
      header_seq_item->setText(3, "0x" + QString::number(pcl.header.seq, 16));
      header_time_meas_item->setText(3, "0x" + QString::number(pcl.header.time_meas, 16));
      header_time_pub_item->setText(3, "0x" + QString::number(pcl.header.time_pub, 16));
      size_item->setText(3, "0x" + QString::number(pcl.size(), 16));
      pack_size_item->setText(3, "0x" + QString::number(pcl.pack_size(), 16));
      extent_item->setText(3, "0x" + QString::number(pcl.get_extent(), 16));
      downsample_item->setText(3, "0x" + QString::number(pcl.get_downsample(), 16));
    } else {
      header_seq_item->setText(3, QString::number(pcl.header.seq));
      header_time_meas_item->setText(3, QString::number(pcl.header.time_meas));
      size_item->setText(3, QString::number(pcl.size()));
      pack_size_item->setText(3, QString::number(pcl.pack_size()));
      extent_item->setText(3, QString::number(pcl.get_extent()));
      downsample_item->setText(3, QString::number(pcl.get_downsample()));
    }

    vertical_item->setText(3, pcl.get_vertical() ? "true" : "false");

    header_seq_item->setHidden(false);
    header_time_meas_item->setHidden(false);
    header_time_pub_item->setHidden(false);
    header_frame_id_item->setHidden(false);
    size_item->setHidden(false);
    pack_size_item->setHidden(false);
    extent_item->setHidden(false);
    downsample_item->setHidden(false);
    vertical_item->setHidden(false);

    if (ui->checkBox_time->isChecked()) {
      header_time_meas_item->setText(3, QString::fromStdString(vlink::Helpers::format_date(pcl.header.time_meas)));
      header_time_pub_item->setText(3, QString::fromStdString(vlink::Helpers::format_date(pcl.header.time_pub)));
    }

    protocol_size_item->setText(3, QString::fromStdString(pcl.get_protocol_size_str()));
    protocol_str_item->setText(3, QString::fromStdString(pcl.get_protocol_name_str()));
    protocol_format_item->setText(3, QString::fromStdString(pcl.get_protocol_type_str()));

    protocol_size_item->setHidden(false);
    protocol_str_item->setHidden(false);
    protocol_format_item->setHidden(false);

    header_frame_id_item->setData(3, Qt::ToolTipRole, header_frame_id_item->text(3));
    header_seq_item->setData(3, Qt::ToolTipRole, header_seq_item->text(3));
    header_time_meas_item->setData(3, Qt::ToolTipRole, header_time_meas_item->text(3));
    header_time_pub_item->setData(3, Qt::ToolTipRole, header_time_pub_item->text(3));
    size_item->setData(3, Qt::ToolTipRole, size_item->text(3));
    pack_size_item->setData(3, Qt::ToolTipRole, pack_size_item->text(3));
    extent_item->setData(3, Qt::ToolTipRole, extent_item->text(3));
    downsample_item->setData(3, Qt::ToolTipRole, downsample_item->text(3));
    vertical_item->setData(3, Qt::ToolTipRole, vertical_item->text(3));

    protocol_size_item->setData(3, Qt::ToolTipRole, protocol_size_item->text(3));
    protocol_str_item->setData(3, Qt::ToolTipRole, protocol_str_item->text(3));
    protocol_format_item->setData(3, Qt::ToolTipRole, protocol_format_item->text(3));

    header_frame_id_item->setData(1, Qt::UserRole, AnalyzeDialog::kStringType);

    header_seq_item->setData(1, Qt::UserRole, AnalyzeDialog::kNumberType);
    if (analyze_dialog_->is_number_type() && ui->treeWidget_property->currentItem() == header_seq_item) {
      analyze_dialog_->add_number(pcl.header.seq);
    }

    header_time_meas_item->setData(1, Qt::UserRole, AnalyzeDialog::kNumberType);
    if (analyze_dialog_->is_number_type() && ui->treeWidget_property->currentItem() == header_time_meas_item) {
      analyze_dialog_->add_number(pcl.header.time_meas);
    }

    header_time_pub_item->setData(1, Qt::UserRole, AnalyzeDialog::kNumberType);
    if (analyze_dialog_->is_number_type() && ui->treeWidget_property->currentItem() == header_time_pub_item) {
      analyze_dialog_->add_number(pcl.header.time_pub);
    }

    size_item->setData(1, Qt::UserRole, AnalyzeDialog::kNumberType);
    if (analyze_dialog_->is_number_type() && ui->treeWidget_property->currentItem() == size_item) {
      analyze_dialog_->add_number(pcl.size());
    }

    pack_size_item->setData(1, Qt::UserRole, AnalyzeDialog::kNumberType);
    if (analyze_dialog_->is_number_type() && ui->treeWidget_property->currentItem() == pack_size_item) {
      analyze_dialog_->add_number(pcl.pack_size());
    }

    extent_item->setData(1, Qt::UserRole, AnalyzeDialog::kNumberType);
    if (analyze_dialog_->is_number_type() && ui->treeWidget_property->currentItem() == extent_item) {
      analyze_dialog_->add_number(pcl.get_extent());
    }

    downsample_item->setData(1, Qt::UserRole, AnalyzeDialog::kNumberType);
    if (analyze_dialog_->is_number_type() && ui->treeWidget_property->currentItem() == downsample_item) {
      analyze_dialog_->add_number(pcl.get_downsample());
    }

    vertical_item->setData(1, Qt::UserRole, AnalyzeDialog::kStringType);
    if (analyze_dialog_->is_string_type() && ui->treeWidget_property->currentItem() == vertical_item) {
      analyze_dialog_->add_string(pcl.get_vertical() ? "true" : "false");
    }

    if (!ui->checkBox_array->isChecked()) {
      int max_point_size = std::min(pcl.size(), static_cast<size_t>(5000));

      QTreeWidgetItem* data_item = pcl_item_map_["data"];
      if (!data_item) {
        data_item = new QTreeWidgetItem();
        pcl_item_map_["data"] = data_item;
        ui->treeWidget_property->addTopLevelItem(data_item);

        data_item->setText(0, "---");
        data_item->setText(2, "data");

        data_item->setData(0, Qt::ToolTipRole, data_item->text(0));
        data_item->setData(1, Qt::ToolTipRole, data_item->text(1));
        data_item->setData(2, Qt::ToolTipRole, data_item->text(2));

        data_item->setExpanded(true);
      }

      data_item->setText(1, "Data[" + QString::number(max_point_size) + "]");
      data_item->setHidden(false);

      vlink::zerocopy::PointCloud::KeyList key_list;
      auto key_map = pcl.get_key_map(&key_list);

      bool has_compressed_xyz = (pcl.get_extent() != 0);

      if (!key_map.empty()) {
        for (int i = 0; i < max_point_size; ++i) {
          std::string mkey = std::string("[") + std::to_string(i) + "]";
          std::string key = std::string("data") + mkey;
          QTreeWidgetItem* p_item = pcl_item_map_[key];
          if (!p_item) {
            p_item = new QTreeWidgetItem();
            pcl_item_map_[key] = p_item;
            data_item->addChild(p_item);

            p_item->setText(0, "---");
            p_item->setText(1, QString::fromStdString(mkey));

            p_item->setData(0, Qt::ToolTipRole, p_item->text(0));
            p_item->setData(1, Qt::ToolTipRole, p_item->text(1));
            p_item->setData(2, Qt::ToolTipRole, p_item->text(2));

            p_item->setExpanded(true);
          }

          p_item->setHidden(false);

          vlink::zerocopy::PointCloud::Vector3f v3f;

          if (has_compressed_xyz) {
            pcl.get_value_v3f(v3f, i);
          }

          for (size_t pkey_index = 0; pkey_index < key_list.size(); ++pkey_index) {
            const auto& pkey = key_list[pkey_index];

            bool is_compressed_xyz = (has_compressed_xyz && pkey_index < 3);

            std::string dkey = key + "." + pkey.name;
            QTreeWidgetItem* d_item = pcl_item_map_[dkey];
            if (!d_item) {
              d_item = new QTreeWidgetItem();
              pcl_item_map_[dkey] = d_item;
              p_item->addChild(d_item);

              d_item->setText(0, "---");

              if (pkey.type == vlink::zerocopy::PointCloud::kUnknownType) {
                d_item->setText(1, QString("number(") + QString::number(pkey.size) + ")");
              } else {
                switch (pkey.type) {
                  case vlink::zerocopy::PointCloud::kBoolType:
                    d_item->setText(1, QString("bool"));
                    break;
                  case vlink::zerocopy::PointCloud::kInt8Type:
                    d_item->setText(1, QString("int8"));
                    break;
                  case vlink::zerocopy::PointCloud::kUint8Type:
                    d_item->setText(1, QString("uint8"));
                    break;
                  case vlink::zerocopy::PointCloud::kInt16Type:
                    d_item->setText(1, QString("int16"));
                    break;
                  case vlink::zerocopy::PointCloud::kUint16Type:
                    d_item->setText(1, QString("uint16"));
                    break;
                  case vlink::zerocopy::PointCloud::kInt32Type:
                    d_item->setText(1, QString("int32"));
                    break;
                  case vlink::zerocopy::PointCloud::kUint32Type:
                    d_item->setText(1, QString("uint32"));
                    break;
                  case vlink::zerocopy::PointCloud::kInt64Type:
                    d_item->setText(1, QString("int64"));
                    break;
                  case vlink::zerocopy::PointCloud::kUint64Type:
                    d_item->setText(1, QString("uint64"));
                    break;
                  case vlink::zerocopy::PointCloud::kFloatType:
                    d_item->setText(1, QString("float"));
                    break;
                  case vlink::zerocopy::PointCloud::kDoubleType:
                    d_item->setText(1, QString("double"));
                    break;
                  default:
                    break;
                }
              }

              d_item->setText(2, QString::fromStdString(pkey.name));

              d_item->setData(0, Qt::ToolTipRole, d_item->text(0));
              d_item->setData(1, Qt::ToolTipRole, d_item->text(1));
              d_item->setData(2, Qt::ToolTipRole, d_item->text(2));
            }

            d_item->setData(1, Qt::UserRole, AnalyzeDialog::kNumberType);

            if (is_compressed_xyz) {
              float value = v3f.x;

              if (pkey_index == 1) {
                value = v3f.y;
              } else if (pkey_index == 2) {
                value = v3f.z;
              }

              d_item->setText(3, QString::number(value, 'g', 8));

              d_item->setHidden(false);

              if (analyze_dialog_->is_number_type() && ui->treeWidget_property->currentItem() == d_item) {
                analyze_dialog_->add_number(value);
              }
            } else if (pkey.type == vlink::zerocopy::PointCloud::kUnknownType) {
              if (pkey.size == 1) {
                auto value = pcl.get_value<uint8_t>(i, key_map, pkey.name);
                if (ui->checkBox_hex->isChecked()) {
                  d_item->setText(3, "0x" + QString::number(value, 16));
                } else {
                  d_item->setText(3, QString::number(value));
                }

                d_item->setHidden(false);

                if (analyze_dialog_->is_number_type() && ui->treeWidget_property->currentItem() == d_item) {
                  analyze_dialog_->add_number(value);
                }
              } else if (pkey.size == 2) {
                auto value = pcl.get_value<int16_t>(i, key_map, pkey.name);
                if (ui->checkBox_hex->isChecked()) {
                  d_item->setText(3, "0x" + QString::number(value, 16));
                } else {
                  d_item->setText(3, QString::number(value));
                }

                d_item->setHidden(false);

                if (analyze_dialog_->is_number_type() && ui->treeWidget_property->currentItem() == d_item) {
                  analyze_dialog_->add_number(value);
                }
              } else if (pkey.size == 4) {
                auto value = pcl.get_value<float>(i, key_map, pkey.name);
                d_item->setText(3, QString::number(value, 'g', 8));

                d_item->setHidden(false);

                if (analyze_dialog_->is_number_type() && ui->treeWidget_property->currentItem() == d_item) {
                  analyze_dialog_->add_number(value);
                }
              } else if (pkey.size == 8) {
                auto value = pcl.get_value<double>(i, key_map, pkey.name);
                d_item->setText(3, QString::number(value, 'g', 16));

                d_item->setHidden(false);

                if (analyze_dialog_->is_number_type() && ui->treeWidget_property->currentItem() == d_item) {
                  analyze_dialog_->add_number(value);
                }
              }
            } else {
              switch (pkey.type) {
                case vlink::zerocopy::PointCloud::kBoolType: {
                  auto value = pcl.get_value<bool>(i, key_map, pkey.name);
                  d_item->setText(3, value ? "true" : "false");

                  d_item->setHidden(false);

                  if (analyze_dialog_->is_number_type() && ui->treeWidget_property->currentItem() == d_item) {
                    analyze_dialog_->add_number(value);
                  }
                } break;
                case vlink::zerocopy::PointCloud::kInt8Type: {
                  auto value = pcl.get_value<int8_t>(i, key_map, pkey.name);
                  if (ui->checkBox_hex->isChecked()) {
                    d_item->setText(3, "0x" + QString::number(value, 16));
                  } else {
                    d_item->setText(3, QString::number(value));
                  }

                  d_item->setHidden(false);

                  if (analyze_dialog_->is_number_type() && ui->treeWidget_property->currentItem() == d_item) {
                    analyze_dialog_->add_number(value);
                  }
                } break;
                case vlink::zerocopy::PointCloud::kUint8Type: {
                  auto value = pcl.get_value<uint8_t>(i, key_map, pkey.name);
                  if (ui->checkBox_hex->isChecked()) {
                    d_item->setText(3, "0x" + QString::number(value, 16));
                  } else {
                    d_item->setText(3, QString::number(value));
                  }

                  d_item->setHidden(false);

                  if (analyze_dialog_->is_number_type() && ui->treeWidget_property->currentItem() == d_item) {
                    analyze_dialog_->add_number(value);
                  }
                } break;
                case vlink::zerocopy::PointCloud::kInt16Type: {
                  auto value = pcl.get_value<int16_t>(i, key_map, pkey.name);
                  if (ui->checkBox_hex->isChecked()) {
                    d_item->setText(3, "0x" + QString::number(value, 16));
                  } else {
                    d_item->setText(3, QString::number(value));
                  }

                  d_item->setHidden(false);

                  if (analyze_dialog_->is_number_type() && ui->treeWidget_property->currentItem() == d_item) {
                    analyze_dialog_->add_number(value);
                  }
                } break;
                case vlink::zerocopy::PointCloud::kUint16Type: {
                  auto value = pcl.get_value<uint16_t>(i, key_map, pkey.name);
                  if (ui->checkBox_hex->isChecked()) {
                    d_item->setText(3, "0x" + QString::number(value, 16));
                  } else {
                    d_item->setText(3, QString::number(value));
                  }

                  d_item->setHidden(false);

                  if (analyze_dialog_->is_number_type() && ui->treeWidget_property->currentItem() == d_item) {
                    analyze_dialog_->add_number(value);
                  }
                } break;
                case vlink::zerocopy::PointCloud::kInt32Type: {
                  auto value = pcl.get_value<int32_t>(i, key_map, pkey.name);
                  if (ui->checkBox_hex->isChecked()) {
                    d_item->setText(3, "0x" + QString::number(value, 16));
                  } else {
                    d_item->setText(3, QString::number(value));
                  }

                  d_item->setHidden(false);

                  if (analyze_dialog_->is_number_type() && ui->treeWidget_property->currentItem() == d_item) {
                    analyze_dialog_->add_number(value);
                  }
                } break;
                case vlink::zerocopy::PointCloud::kUint32Type: {
                  auto value = pcl.get_value<uint32_t>(i, key_map, pkey.name);
                  if (ui->checkBox_hex->isChecked()) {
                    d_item->setText(3, "0x" + QString::number(value, 16));
                  } else {
                    d_item->setText(3, QString::number(value));
                  }

                  d_item->setHidden(false);

                  if (analyze_dialog_->is_number_type() && ui->treeWidget_property->currentItem() == d_item) {
                    analyze_dialog_->add_number(value);
                  }
                } break;
                case vlink::zerocopy::PointCloud::kInt64Type: {
                  auto value = pcl.get_value<int64_t>(i, key_map, pkey.name);
                  if (ui->checkBox_hex->isChecked()) {
                    d_item->setText(3, "0x" + QString::number(value, 16));
                  } else {
                    d_item->setText(3, QString::number(value));
                  }

                  d_item->setHidden(false);

                  if (analyze_dialog_->is_number_type() && ui->treeWidget_property->currentItem() == d_item) {
                    analyze_dialog_->add_number(value);
                  }
                } break;
                case vlink::zerocopy::PointCloud::kUint64Type: {
                  auto value = pcl.get_value<uint64_t>(i, key_map, pkey.name);
                  if (ui->checkBox_hex->isChecked()) {
                    d_item->setText(3, "0x" + QString::number(value, 16));
                  } else {
                    d_item->setText(3, QString::number(value));
                  }

                  d_item->setHidden(false);

                  if (analyze_dialog_->is_number_type() && ui->treeWidget_property->currentItem() == d_item) {
                    analyze_dialog_->add_number(value);
                  }
                } break;
                case vlink::zerocopy::PointCloud::kFloatType: {
                  auto value = pcl.get_value<float>(i, key_map, pkey.name);
                  d_item->setText(3, QString::number(value, 'g', 8));

                  d_item->setHidden(false);

                  if (analyze_dialog_->is_number_type() && ui->treeWidget_property->currentItem() == d_item) {
                    analyze_dialog_->add_number(value);
                  }
                } break;
                case vlink::zerocopy::PointCloud::kDoubleType: {
                  auto value = pcl.get_value<double>(i, key_map, pkey.name);
                  d_item->setText(3, QString::number(value, 'g', 16));

                  d_item->setHidden(false);

                  if (analyze_dialog_->is_number_type() && ui->treeWidget_property->currentItem() == d_item) {
                    analyze_dialog_->add_number(value);
                  }
                } break;
                default:
                  break;
              }
            }

            d_item->setData(3, Qt::ToolTipRole, d_item->text(3));
          }
        }
      }
    }
  } else if (current_ser_.find("OccupancyGrid") != std::string::npos) {
    for (const auto& [name, item] : occupancy_item_map_) {
      item->setHidden(true);
    }

    vlink::zerocopy::OccupancyGrid grid;

    if (!(grid << bytes)) {
      return;
    }

    QTreeWidget* tree = ui->treeWidget_property;

    QTreeWidgetItem* header_root_item =
        ensure_property_item(occupancy_item_map_, tree, nullptr, "header", "Header", "header");
    header_root_item->setExpanded(true);

    QTreeWidgetItem* header_frame_id_item =
        ensure_property_item(occupancy_item_map_, tree, header_root_item, "header.frame_id", "string", "frame_id");
    QTreeWidgetItem* header_seq_item =
        ensure_property_item(occupancy_item_map_, tree, header_root_item, "header.seq", "uint32", "seq");
    QTreeWidgetItem* header_time_meas_item =
        ensure_property_item(occupancy_item_map_, tree, header_root_item, "header.time_meas", "uint64", "time_meas");
    QTreeWidgetItem* header_time_pub_item =
        ensure_property_item(occupancy_item_map_, tree, header_root_item, "header.time_pub", "uint64", "time_pub");

    QTreeWidgetItem* width_item = ensure_property_item(occupancy_item_map_, tree, nullptr, "width", "uint32", "width");
    QTreeWidgetItem* height_item =
        ensure_property_item(occupancy_item_map_, tree, nullptr, "height", "uint32", "height");
    QTreeWidgetItem* resolution_item =
        ensure_property_item(occupancy_item_map_, tree, nullptr, "resolution", "float", "resolution");

    QTreeWidgetItem* origin_x_item =
        ensure_property_item(occupancy_item_map_, tree, nullptr, "origin_x", "float", "origin_x");
    QTreeWidgetItem* origin_y_item =
        ensure_property_item(occupancy_item_map_, tree, nullptr, "origin_y", "float", "origin_y");
    QTreeWidgetItem* origin_z_item =
        ensure_property_item(occupancy_item_map_, tree, nullptr, "origin_z", "float", "origin_z");
    QTreeWidgetItem* origin_yaw_item =
        ensure_property_item(occupancy_item_map_, tree, nullptr, "origin_yaw", "float", "origin_yaw");

    QTreeWidgetItem* cell_type_item =
        ensure_property_item(occupancy_item_map_, tree, nullptr, "cell_type", "CellType", "cell_type");

    QTreeWidgetItem* value_min_item =
        ensure_property_item(occupancy_item_map_, tree, nullptr, "value_min", "float", "value_min");
    QTreeWidgetItem* value_max_item =
        ensure_property_item(occupancy_item_map_, tree, nullptr, "value_max", "float", "value_max");
    QTreeWidgetItem* default_value_item =
        ensure_property_item(occupancy_item_map_, tree, nullptr, "default_value", "int32", "default_value");

    QTreeWidgetItem* occupied_threshold_item =
        ensure_property_item(occupancy_item_map_, tree, nullptr, "occupied_threshold", "float", "occupied_threshold");
    QTreeWidgetItem* free_threshold_item =
        ensure_property_item(occupancy_item_map_, tree, nullptr, "free_threshold", "float", "free_threshold");

    QTreeWidgetItem* map_id_item =
        ensure_property_item(occupancy_item_map_, tree, nullptr, "map_id", "string", "map_id");
    QTreeWidgetItem* channel_item =
        ensure_property_item(occupancy_item_map_, tree, nullptr, "channel", "uint32", "channel");
    QTreeWidgetItem* freq_item = ensure_property_item(occupancy_item_map_, tree, nullptr, "freq", "uint32", "freq");
    QTreeWidgetItem* update_time_ns_item =
        ensure_property_item(occupancy_item_map_, tree, nullptr, "update_time_ns", "uint64", "update_time_ns");

    QTreeWidgetItem* valid_cell_count_item =
        ensure_property_item(occupancy_item_map_, tree, nullptr, "valid_cell_count", "uint32", "valid_cell_count");
    QTreeWidgetItem* size_item = ensure_property_item(occupancy_item_map_, tree, nullptr, "size", "uint64", "size");
    QTreeWidgetItem* is_owner_item =
        ensure_property_item(occupancy_item_map_, tree, nullptr, "is_owner", "bool", "is_owner");

    header_root_item->setHidden(false);

    set_property_text(header_frame_id_item, QString::fromLatin1(grid.header.frame_id_view().data(),
                                                                static_cast<int>(grid.header.frame_id_view().size())));

    const bool hex = ui->checkBox_hex->isChecked();
    set_property_number(header_seq_item, grid.header.seq, hex, analyze_dialog_, tree);
    set_property_number(header_time_meas_item, grid.header.time_meas, hex, analyze_dialog_, tree);
    set_property_number(header_time_pub_item, grid.header.time_pub, hex, analyze_dialog_, tree);

    if (ui->checkBox_time->isChecked()) {
      header_time_meas_item->setText(3, QString::fromStdString(vlink::Helpers::format_date(grid.header.time_meas)));
      header_time_pub_item->setText(3, QString::fromStdString(vlink::Helpers::format_date(grid.header.time_pub)));
    }

    set_property_number(width_item, grid.width(), hex, analyze_dialog_, tree);
    set_property_number(height_item, grid.height(), hex, analyze_dialog_, tree);

    resolution_item->setText(3, QString::number(grid.resolution(), 'g', 8));
    resolution_item->setHidden(false);
    resolution_item->setData(3, Qt::ToolTipRole, resolution_item->text(3));
    resolution_item->setData(1, Qt::UserRole, AnalyzeDialog::kNumberType);

    origin_x_item->setText(3, QString::number(grid.origin_x(), 'g', 8));
    origin_x_item->setHidden(false);
    origin_x_item->setData(3, Qt::ToolTipRole, origin_x_item->text(3));
    origin_x_item->setData(1, Qt::UserRole, AnalyzeDialog::kNumberType);

    origin_y_item->setText(3, QString::number(grid.origin_y(), 'g', 8));
    origin_y_item->setHidden(false);
    origin_y_item->setData(3, Qt::ToolTipRole, origin_y_item->text(3));
    origin_y_item->setData(1, Qt::UserRole, AnalyzeDialog::kNumberType);

    origin_z_item->setText(3, QString::number(grid.origin_z(), 'g', 8));
    origin_z_item->setHidden(false);
    origin_z_item->setData(3, Qt::ToolTipRole, origin_z_item->text(3));
    origin_z_item->setData(1, Qt::UserRole, AnalyzeDialog::kNumberType);

    origin_yaw_item->setText(3, QString::number(grid.origin_yaw(), 'g', 8));
    origin_yaw_item->setHidden(false);
    origin_yaw_item->setData(3, Qt::ToolTipRole, origin_yaw_item->text(3));
    origin_yaw_item->setData(1, Qt::UserRole, AnalyzeDialog::kNumberType);

    if (ui->checkBox_enum->isChecked()) {
      cell_type_item->setText(3, QString::fromStdString(std::string(vlink::NameDetector::get_enum(grid.cell_type()))));
    } else {
      cell_type_item->setText(3, QString::number(grid.cell_type()));
    }

    cell_type_item->setHidden(false);
    cell_type_item->setData(3, Qt::ToolTipRole, cell_type_item->text(3));

    value_min_item->setText(3, QString::number(grid.value_min(), 'g', 8));
    value_min_item->setHidden(false);
    value_min_item->setData(3, Qt::ToolTipRole, value_min_item->text(3));
    value_min_item->setData(1, Qt::UserRole, AnalyzeDialog::kNumberType);

    value_max_item->setText(3, QString::number(grid.value_max(), 'g', 8));
    value_max_item->setHidden(false);
    value_max_item->setData(3, Qt::ToolTipRole, value_max_item->text(3));
    value_max_item->setData(1, Qt::UserRole, AnalyzeDialog::kNumberType);

    set_property_number(default_value_item, grid.default_value(), hex, analyze_dialog_, tree);

    occupied_threshold_item->setText(3, QString::number(grid.occupied_threshold(), 'g', 8));
    occupied_threshold_item->setHidden(false);
    occupied_threshold_item->setData(3, Qt::ToolTipRole, occupied_threshold_item->text(3));
    occupied_threshold_item->setData(1, Qt::UserRole, AnalyzeDialog::kNumberType);

    free_threshold_item->setText(3, QString::number(grid.free_threshold(), 'g', 8));
    free_threshold_item->setHidden(false);
    free_threshold_item->setData(3, Qt::ToolTipRole, free_threshold_item->text(3));
    free_threshold_item->setData(1, Qt::UserRole, AnalyzeDialog::kNumberType);

    set_property_text(map_id_item, QString::fromUtf8(grid.map_id().data(), static_cast<int>(grid.map_id().size())));
    set_property_number(channel_item, grid.channel(), hex, analyze_dialog_, tree);
    set_property_number(freq_item, grid.freq(), hex, analyze_dialog_, tree);
    set_property_number(update_time_ns_item, grid.update_time_ns(), hex, analyze_dialog_, tree);
    set_property_number(valid_cell_count_item, grid.valid_cell_count(), hex, analyze_dialog_, tree);
    set_property_number(size_item, grid.size(), hex, analyze_dialog_, tree);

    is_owner_item->setText(3, grid.is_owner() ? "true" : "false");
    is_owner_item->setHidden(false);
    is_owner_item->setData(3, Qt::ToolTipRole, is_owner_item->text(3));

  } else if (current_ser_.find("Tensor") != std::string::npos) {
    for (const auto& [name, item] : tensor_item_map_) {
      item->setHidden(true);
    }

    vlink::zerocopy::Tensor t;

    if (!(t << bytes)) {
      return;
    }

    QTreeWidget* tree = ui->treeWidget_property;

    QTreeWidgetItem* header_root_item =
        ensure_property_item(tensor_item_map_, tree, nullptr, "header", "Header", "header");
    header_root_item->setExpanded(true);

    QTreeWidgetItem* header_frame_id_item =
        ensure_property_item(tensor_item_map_, tree, header_root_item, "header.frame_id", "string", "frame_id");
    QTreeWidgetItem* header_seq_item =
        ensure_property_item(tensor_item_map_, tree, header_root_item, "header.seq", "uint32", "seq");
    QTreeWidgetItem* header_time_meas_item =
        ensure_property_item(tensor_item_map_, tree, header_root_item, "header.time_meas", "uint64", "time_meas");
    QTreeWidgetItem* header_time_pub_item =
        ensure_property_item(tensor_item_map_, tree, header_root_item, "header.time_pub", "uint64", "time_pub");

    QTreeWidgetItem* name_item = ensure_property_item(tensor_item_map_, tree, nullptr, "name", "string", "name");
    QTreeWidgetItem* model_id_item =
        ensure_property_item(tensor_item_map_, tree, nullptr, "model_id", "string", "model_id");
    QTreeWidgetItem* layout_item = ensure_property_item(tensor_item_map_, tree, nullptr, "layout", "string", "layout");

    QTreeWidgetItem* dtype_item = ensure_property_item(tensor_item_map_, tree, nullptr, "dtype", "DataType", "dtype");
    QTreeWidgetItem* rank_item = ensure_property_item(tensor_item_map_, tree, nullptr, "rank", "uint8", "rank");
    QTreeWidgetItem* num_elements_item =
        ensure_property_item(tensor_item_map_, tree, nullptr, "num_elements", "uint64", "num_elements");
    QTreeWidgetItem* element_size_item =
        ensure_property_item(tensor_item_map_, tree, nullptr, "element_size", "uint8", "element_size");
    QTreeWidgetItem* shape_item = ensure_property_item(tensor_item_map_, tree, nullptr, "shape", "uint32[]", "shape");

    QTreeWidgetItem* device_item = ensure_property_item(tensor_item_map_, tree, nullptr, "device", "Device", "device");
    QTreeWidgetItem* batch_size_item =
        ensure_property_item(tensor_item_map_, tree, nullptr, "batch_size", "uint32", "batch_size");
    QTreeWidgetItem* quant_scale_item =
        ensure_property_item(tensor_item_map_, tree, nullptr, "quant_scale", "float", "quant_scale");
    QTreeWidgetItem* quant_zero_point_item =
        ensure_property_item(tensor_item_map_, tree, nullptr, "quant_zero_point", "int32", "quant_zero_point");

    QTreeWidgetItem* channel_item =
        ensure_property_item(tensor_item_map_, tree, nullptr, "channel", "uint32", "channel");
    QTreeWidgetItem* freq_item = ensure_property_item(tensor_item_map_, tree, nullptr, "freq", "uint32", "freq");
    QTreeWidgetItem* update_time_ns_item =
        ensure_property_item(tensor_item_map_, tree, nullptr, "update_time_ns", "uint64", "update_time_ns");
    QTreeWidgetItem* size_item = ensure_property_item(tensor_item_map_, tree, nullptr, "size", "uint64", "size");

    header_root_item->setHidden(false);

    set_property_text(header_frame_id_item, QString::fromLatin1(t.header.frame_id_view().data(),
                                                                static_cast<int>(t.header.frame_id_view().size())));

    const bool hex = ui->checkBox_hex->isChecked();
    set_property_number(header_seq_item, t.header.seq, hex, analyze_dialog_, tree);
    set_property_number(header_time_meas_item, t.header.time_meas, hex, analyze_dialog_, tree);
    set_property_number(header_time_pub_item, t.header.time_pub, hex, analyze_dialog_, tree);

    if (ui->checkBox_time->isChecked()) {
      header_time_meas_item->setText(3, QString::fromStdString(vlink::Helpers::format_date(t.header.time_meas)));
      header_time_pub_item->setText(3, QString::fromStdString(vlink::Helpers::format_date(t.header.time_pub)));
    }

    set_property_text(name_item, QString::fromUtf8(t.name().data(), static_cast<int>(t.name().size())));
    set_property_text(model_id_item, QString::fromUtf8(t.model_id().data(), static_cast<int>(t.model_id().size())));
    set_property_text(layout_item, QString::fromUtf8(t.layout().data(), static_cast<int>(t.layout().size())));

    if (ui->checkBox_enum->isChecked()) {
      dtype_item->setText(3, QString::fromStdString(std::string(vlink::NameDetector::get_enum(t.dtype()))));
      device_item->setText(3, QString::fromStdString(std::string(vlink::NameDetector::get_enum(t.device()))));
    } else {
      dtype_item->setText(3, QString::number(t.dtype()));
      device_item->setText(3, QString::number(t.device()));
    }

    dtype_item->setHidden(false);
    dtype_item->setData(3, Qt::ToolTipRole, dtype_item->text(3));
    device_item->setHidden(false);
    device_item->setData(3, Qt::ToolTipRole, device_item->text(3));

    set_property_number(rank_item, t.rank(), hex, analyze_dialog_, tree);
    set_property_number(num_elements_item, t.num_elements(), hex, analyze_dialog_, tree);
    set_property_number(element_size_item, t.element_size(), hex, analyze_dialog_, tree);

    QString shape_str = "[";
    for (uint8_t i = 0; i < t.rank(); ++i) {
      if (i > 0) {
        shape_str += ", ";
      }

      shape_str += QString::number(t.shape_at(i));
    }

    shape_str += "]";
    shape_item->setText(3, shape_str);
    shape_item->setHidden(false);
    shape_item->setData(3, Qt::ToolTipRole, shape_item->text(3));

    set_property_number(batch_size_item, t.batch_size(), hex, analyze_dialog_, tree);

    quant_scale_item->setText(3, QString::number(t.quant_scale(), 'g', 8));
    quant_scale_item->setHidden(false);
    quant_scale_item->setData(3, Qt::ToolTipRole, quant_scale_item->text(3));
    quant_scale_item->setData(1, Qt::UserRole, AnalyzeDialog::kNumberType);

    set_property_number(quant_zero_point_item, t.quant_zero_point(), hex, analyze_dialog_, tree);
    set_property_number(channel_item, t.channel(), hex, analyze_dialog_, tree);
    set_property_number(freq_item, t.freq(), hex, analyze_dialog_, tree);
    set_property_number(update_time_ns_item, t.update_time_ns(), hex, analyze_dialog_, tree);
    set_property_number(size_item, t.size(), hex, analyze_dialog_, tree);

  } else if (current_ser_.find("ObjectArray") != std::string::npos) {
    for (const auto& [name, item] : object_array_item_map_) {
      item->setHidden(true);
    }

    vlink::zerocopy::ObjectArray arr;

    if (!(arr << bytes)) {
      return;
    }

    QTreeWidget* tree = ui->treeWidget_property;

    QTreeWidgetItem* header_root_item =
        ensure_property_item(object_array_item_map_, tree, nullptr, "header", "Header", "header");
    header_root_item->setExpanded(true);

    QTreeWidgetItem* header_frame_id_item =
        ensure_property_item(object_array_item_map_, tree, header_root_item, "header.frame_id", "string", "frame_id");
    QTreeWidgetItem* header_seq_item =
        ensure_property_item(object_array_item_map_, tree, header_root_item, "header.seq", "uint32", "seq");
    QTreeWidgetItem* header_time_meas_item =
        ensure_property_item(object_array_item_map_, tree, header_root_item, "header.time_meas", "uint64", "time_meas");
    QTreeWidgetItem* header_time_pub_item =
        ensure_property_item(object_array_item_map_, tree, header_root_item, "header.time_pub", "uint64", "time_pub");

    QTreeWidgetItem* source_id_item =
        ensure_property_item(object_array_item_map_, tree, nullptr, "source_id", "string", "source_id");
    QTreeWidgetItem* channel_item =
        ensure_property_item(object_array_item_map_, tree, nullptr, "channel", "uint32", "channel");
    QTreeWidgetItem* freq_item = ensure_property_item(object_array_item_map_, tree, nullptr, "freq", "uint32", "freq");
    QTreeWidgetItem* update_time_ns_item =
        ensure_property_item(object_array_item_map_, tree, nullptr, "update_time_ns", "uint64", "update_time_ns");
    QTreeWidgetItem* count_item =
        ensure_property_item(object_array_item_map_, tree, nullptr, "count", "uint32", "count");
    QTreeWidgetItem* pack_size_item =
        ensure_property_item(object_array_item_map_, tree, nullptr, "pack_size", "uint32", "pack_size");
    QTreeWidgetItem* capacity_item =
        ensure_property_item(object_array_item_map_, tree, nullptr, "capacity", "uint64", "capacity");
    QTreeWidgetItem* is_owner_item =
        ensure_property_item(object_array_item_map_, tree, nullptr, "is_owner", "bool", "is_owner");

    header_root_item->setHidden(false);

    set_property_text(header_frame_id_item, QString::fromLatin1(arr.header.frame_id_view().data(),
                                                                static_cast<int>(arr.header.frame_id_view().size())));

    const bool hex = ui->checkBox_hex->isChecked();
    set_property_number(header_seq_item, arr.header.seq, hex, analyze_dialog_, tree);
    set_property_number(header_time_meas_item, arr.header.time_meas, hex, analyze_dialog_, tree);
    set_property_number(header_time_pub_item, arr.header.time_pub, hex, analyze_dialog_, tree);

    if (ui->checkBox_time->isChecked()) {
      header_time_meas_item->setText(3, QString::fromStdString(vlink::Helpers::format_date(arr.header.time_meas)));
      header_time_pub_item->setText(3, QString::fromStdString(vlink::Helpers::format_date(arr.header.time_pub)));
    }

    set_property_text(source_id_item,
                      QString::fromUtf8(arr.source_id().data(), static_cast<int>(arr.source_id().size())));
    set_property_number(channel_item, arr.channel(), hex, analyze_dialog_, tree);
    set_property_number(freq_item, arr.freq(), hex, analyze_dialog_, tree);
    set_property_number(update_time_ns_item, arr.update_time_ns(), hex, analyze_dialog_, tree);
    set_property_number(count_item, arr.count(), hex, analyze_dialog_, tree);
    set_property_number(pack_size_item, arr.pack_size(), hex, analyze_dialog_, tree);
    set_property_number(capacity_item, arr.capacity(), hex, analyze_dialog_, tree);

    is_owner_item->setText(3, arr.is_owner() ? "true" : "false");
    is_owner_item->setHidden(false);
    is_owner_item->setData(3, Qt::ToolTipRole, is_owner_item->text(3));

    if (!ui->checkBox_array->isChecked()) {
      static constexpr uint32_t kMaxObjectPreview{8};
      const uint32_t shown = std::min(arr.count(), kMaxObjectPreview);

      QTreeWidgetItem* data_item = ensure_property_item(object_array_item_map_, tree, nullptr, "data",
                                                        "Object[" + QString::number(shown) + "]", "data");
      data_item->setText(1, "Object[" + QString::number(shown) + "]");
      data_item->setExpanded(true);
      data_item->setHidden(false);

      for (uint32_t i = 0; i < shown; ++i) {
        const auto* obj = arr.objects(i);
        if (obj == nullptr) {
          continue;
        }

        const std::string mkey = std::string("[") + std::to_string(i) + "]";
        const std::string key = std::string("data") + mkey;
        QTreeWidgetItem* p_item =
            ensure_property_item(object_array_item_map_, tree, data_item, key, QString::fromStdString(mkey), "");
        p_item->setText(1, QString::fromStdString(mkey));
        p_item->setExpanded(true);
        p_item->setHidden(false);

        QTreeWidgetItem* label_item =
            ensure_property_item(object_array_item_map_, tree, p_item, key + ".label", "string", "label");
        set_property_text(label_item,
                          QString::fromLatin1(obj->label, static_cast<int>(::strnlen(obj->label, sizeof(obj->label)))));

        QTreeWidgetItem* position_item =
            ensure_property_item(object_array_item_map_, tree, p_item, key + ".position", "float[3]", "position");
        position_item->setText(3, QString("(%1, %2, %3)")
                                      .arg(QString::number(obj->position[0], 'g', 6))
                                      .arg(QString::number(obj->position[1], 'g', 6))
                                      .arg(QString::number(obj->position[2], 'g', 6)));
        position_item->setHidden(false);
        position_item->setData(3, Qt::ToolTipRole, position_item->text(3));

        QTreeWidgetItem* class_id_item =
            ensure_property_item(object_array_item_map_, tree, p_item, key + ".class_id", "uint32", "class_id");
        set_property_number(class_id_item, obj->class_id, hex, analyze_dialog_, tree);

        QTreeWidgetItem* track_id_item =
            ensure_property_item(object_array_item_map_, tree, p_item, key + ".track_id", "uint32", "track_id");
        set_property_number(track_id_item, obj->track_id, hex, analyze_dialog_, tree);
      }
    }

  } else if (current_ser_.find("AudioFrame") != std::string::npos) {
    for (const auto& [name, item] : audio_item_map_) {
      item->setHidden(true);
    }

    vlink::zerocopy::AudioFrame af;

    if (!(af << bytes)) {
      return;
    }

    QTreeWidget* tree = ui->treeWidget_property;

    QTreeWidgetItem* header_root_item =
        ensure_property_item(audio_item_map_, tree, nullptr, "header", "Header", "header");
    header_root_item->setExpanded(true);

    QTreeWidgetItem* header_frame_id_item =
        ensure_property_item(audio_item_map_, tree, header_root_item, "header.frame_id", "string", "frame_id");
    QTreeWidgetItem* header_seq_item =
        ensure_property_item(audio_item_map_, tree, header_root_item, "header.seq", "uint32", "seq");
    QTreeWidgetItem* header_time_meas_item =
        ensure_property_item(audio_item_map_, tree, header_root_item, "header.time_meas", "uint64", "time_meas");
    QTreeWidgetItem* header_time_pub_item =
        ensure_property_item(audio_item_map_, tree, header_root_item, "header.time_pub", "uint64", "time_pub");

    QTreeWidgetItem* sample_rate_item =
        ensure_property_item(audio_item_map_, tree, nullptr, "sample_rate", "uint32", "sample_rate");
    QTreeWidgetItem* num_channels_item =
        ensure_property_item(audio_item_map_, tree, nullptr, "num_channels", "uint16", "num_channels");
    QTreeWidgetItem* num_samples_item =
        ensure_property_item(audio_item_map_, tree, nullptr, "num_samples", "uint32", "num_samples");
    QTreeWidgetItem* bit_depth_item =
        ensure_property_item(audio_item_map_, tree, nullptr, "bit_depth", "uint16", "bit_depth");

    QTreeWidgetItem* format_item = ensure_property_item(audio_item_map_, tree, nullptr, "format", "Format", "format");
    QTreeWidgetItem* layout_item = ensure_property_item(audio_item_map_, tree, nullptr, "layout", "Layout", "layout");
    QTreeWidgetItem* codec_item = ensure_property_item(audio_item_map_, tree, nullptr, "codec", "string", "codec");
    QTreeWidgetItem* language_item =
        ensure_property_item(audio_item_map_, tree, nullptr, "language", "string", "language");
    QTreeWidgetItem* bitrate_item =
        ensure_property_item(audio_item_map_, tree, nullptr, "bitrate", "uint32", "bitrate");
    QTreeWidgetItem* duration_ns_item =
        ensure_property_item(audio_item_map_, tree, nullptr, "duration_ns", "uint64", "duration_ns");

    QTreeWidgetItem* channel_item =
        ensure_property_item(audio_item_map_, tree, nullptr, "channel", "uint32", "channel");
    QTreeWidgetItem* freq_item = ensure_property_item(audio_item_map_, tree, nullptr, "freq", "uint32", "freq");
    QTreeWidgetItem* update_time_ns_item =
        ensure_property_item(audio_item_map_, tree, nullptr, "update_time_ns", "uint64", "update_time_ns");
    QTreeWidgetItem* size_item = ensure_property_item(audio_item_map_, tree, nullptr, "size", "uint64", "size");

    header_root_item->setHidden(false);

    set_property_text(header_frame_id_item, QString::fromLatin1(af.header.frame_id_view().data(),
                                                                static_cast<int>(af.header.frame_id_view().size())));

    const bool hex = ui->checkBox_hex->isChecked();
    set_property_number(header_seq_item, af.header.seq, hex, analyze_dialog_, tree);
    set_property_number(header_time_meas_item, af.header.time_meas, hex, analyze_dialog_, tree);
    set_property_number(header_time_pub_item, af.header.time_pub, hex, analyze_dialog_, tree);

    if (ui->checkBox_time->isChecked()) {
      header_time_meas_item->setText(3, QString::fromStdString(vlink::Helpers::format_date(af.header.time_meas)));
      header_time_pub_item->setText(3, QString::fromStdString(vlink::Helpers::format_date(af.header.time_pub)));
    }

    set_property_number(sample_rate_item, af.sample_rate(), hex, analyze_dialog_, tree);
    set_property_number(num_channels_item, af.num_channels(), hex, analyze_dialog_, tree);
    set_property_number(num_samples_item, af.num_samples(), hex, analyze_dialog_, tree);
    set_property_number(bit_depth_item, af.bit_depth(), hex, analyze_dialog_, tree);

    if (ui->checkBox_enum->isChecked()) {
      format_item->setText(3, QString::fromStdString(std::string(vlink::NameDetector::get_enum(af.format()))));
      layout_item->setText(3, QString::fromStdString(std::string(vlink::NameDetector::get_enum(af.layout()))));
    } else {
      format_item->setText(3, QString::number(af.format()));
      layout_item->setText(3, QString::number(af.layout()));
    }

    format_item->setHidden(false);
    format_item->setData(3, Qt::ToolTipRole, format_item->text(3));
    layout_item->setHidden(false);
    layout_item->setData(3, Qt::ToolTipRole, layout_item->text(3));

    set_property_text(codec_item, QString::fromUtf8(af.codec().data(), static_cast<int>(af.codec().size())));
    set_property_text(language_item, QString::fromUtf8(af.language().data(), static_cast<int>(af.language().size())));

    set_property_number(bitrate_item, af.bitrate(), hex, analyze_dialog_, tree);
    set_property_number(duration_ns_item, af.duration_ns(), hex, analyze_dialog_, tree);
    set_property_number(channel_item, af.channel(), hex, analyze_dialog_, tree);
    set_property_number(freq_item, af.freq(), hex, analyze_dialog_, tree);
    set_property_number(update_time_ns_item, af.update_time_ns(), hex, analyze_dialog_, tree);
    set_property_number(size_item, af.size(), hex, analyze_dialog_, tree);
  }

  ui->treeWidget_property->setUpdatesEnabled(true);
}

void MainWindow::showEvent(QShowEvent* event) {
  QMainWindow::showEvent(event);

  adjust_size();
}

void MainWindow::hideEvent(QHideEvent* event) { QMainWindow::hideEvent(event); }

void MainWindow::closeEvent(QCloseEvent* event) { QMainWindow::closeEvent(event); }

void MainWindow::resizeEvent(QResizeEvent* event) {
  QMainWindow::resizeEvent(event);

  adjust_size();
}

// NOLINTEND
