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

#include "./playdialog.h"

#include <vlink/base/helpers.h>

#include <QCheckBox>
#include <QFileDialog>
#include <QMessageBox>
#include <QSettings>
#include <QStandardPaths>
#include <string>

#include "./mainwindow.h"
#include "./ui_playdialog.h"

PlayDialog::PlayDialog(QWidget* parent) : QDialog(parent), ui(new Ui::PlayDialog) {
  ui->setupUi(this);

  setWindowFlags(Qt::Dialog | Qt::WindowMaximizeButtonHint | Qt::WindowCloseButtonHint);

  window_ = MainWindow::get_instance();

  vlink::BagReader::Config config;

  ui->doubleSpinBox_begin->setValue(config.begin_time / 1000'000);

  ui->doubleSpinBox_end->setValue(config.end_time / 1000'000);

  ui->spinBox_loop->setValue(config.times);

  ui->doubleSpinBox_rate->setValue(config.rate);

  label_timer_ = new QTimer(this);
  label_timer_->setInterval(100);

  connect(label_timer_, &QTimer::timeout, this, [this]() { update_time_label(); });

  connect(
      ui->lineEdit_filter, &QLineEdit::textEdited, this,
      [this](const QString& text) {
        for (int i = 0; i < ui->listWidget->count(); ++i) {
          auto* item = ui->listWidget->item(i);

          bool contains = false;

          if (text.isEmpty()) {
            contains = true;
          } else {
            const std::vector<std::string> filter_tokens = vlink::Helpers::split_any(text.toStdString());
            const auto& target_sp = item->data(Qt::UserRole).toString();

            for (const auto& sp : filter_tokens) {
              if (target_sp.contains(QString::fromStdString(sp), Qt::CaseInsensitive)) {
                contains = true;
                break;
              }
            }
          }

          if (contains) {
            item->setHidden(false);
          } else {
            item->setHidden(true);
            // checkbox->setChecked(false);
          }
        }

        update_status();
      },
      Qt::QueuedConnection);

  connect(
      window_, &MainWindow::connect_changed, this,
      [this](bool connected) {
        if (!connected) {
          on_pushButton_stop_clicked();
          QMessageBox::warning(this, tr("Warning"), tr("Proxy is disconnected."));
          this->close();
        }
      },
      Qt::QueuedConnection);

  ui->listWidget->setSelectionMode(QAbstractItemView::NoSelection);

  update_status();

  ui->pushButton_close->setFocusPolicy(Qt::NoFocus);

  setFocus();
}

PlayDialog::~PlayDialog() {
  for (int i = 0; i < ui->listWidget->count(); ++i) {
    auto* item = ui->listWidget->takeItem(i);
    auto* checkbox = qobject_cast<QCheckBox*>(ui->listWidget->itemWidget(item));
    delete checkbox;
    delete item;
    --i;
  }

  if (player_) {
    if (player_->is_running()) {
      player_->stop();
      player_->quit();
      player_->wait_for_quit(2000);
    }
  }

  delete label_timer_;

  delete ui;
}

void PlayDialog::on_pushButton_all_clicked() {
  for (int i = 0; i < ui->listWidget->count(); ++i) {
    auto* item = ui->listWidget->item(i);
    auto* checkbox = qobject_cast<QCheckBox*>(ui->listWidget->itemWidget(item));

    if (!item->isHidden()) {
      checkbox->setChecked(true);
    }
  }

  update_status();
}

void PlayDialog::on_pushButton_unall_clicked() {
  for (int i = 0; i < ui->listWidget->count(); ++i) {
    auto* item = ui->listWidget->item(i);
    auto* checkbox = qobject_cast<QCheckBox*>(ui->listWidget->itemWidget(item));

    if (!item->isHidden()) {
      checkbox->setChecked(false);
    }
  }

  update_status();
}

void PlayDialog::on_pushButton_select_clicked() {
  QSettings settings(QStandardPaths::writableLocation(QStandardPaths::CacheLocation) + "/settings.ini",
                     QSettings::IniFormat);

  settings.beginGroup("PlayDialog");

  QFileDialog dialog(this, tr("Select bag file"), settings.value("bag_dir", qApp->applicationDirPath()).toString(),
                     "Bag files (*.vdb *.vdbx *.vcap *.vcapx)");

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

  settings.beginGroup("PlayDialog");
  settings.setValue("bag_dir", QFileInfo(file_path).dir().path());
  settings.endGroup();
  settings.sync();

  ui->lineEdit_load->setText(file_path);

  if (player_) {
    if (player_->is_running()) {
      player_->quit();
      player_->wait_for_quit(2000);
    }
    player_.reset();
  }

  ui->listWidget->clear();
  ser_map_.clear();
  schema_type_map_.clear();
  status_ = kDisable;

  try {
    player_ = vlink::BagReader::create(file_path.toStdString(), true);
  } catch (vlink::Exception::RuntimeError& e) {
    QMessageBox::critical(this, tr("Error"), QString::fromStdString(e.what()));
    update_status();
    return;
  }

  player_->async_run();

  player_->register_output_callback([this](const vlink::Frame& frame) {
    const int64_t timestamp = frame.timestamp;
    const std::string& url = frame.url;
    const vlink::ActionType action_type = frame.action_type;
    const vlink::Bytes& raw_data = frame.data;

    if (action_type == vlink::ActionType::kClientRequest || action_type == vlink::ActionType::kClientResponse ||
        action_type == vlink::ActionType::kServerRequest || action_type == vlink::ActionType::kServerResponse) {
      return;
    }

    {
      std::shared_lock lock(url_mtx_);

      if (url_list_.count(url) == 0) {
        return;
      }
    }

    vlink::ProxyAPI::Data proxy_data;
    proxy_data.timestamp = 0;
    proxy_data.url = url;
    proxy_data.ser = frame.ser_type;
    {
      const auto schema_iter = schema_type_map_.find(url);
      proxy_data.schema = schema_iter != schema_type_map_.end() ? schema_iter->second : vlink::SchemaType::kUnknown;
    }
    proxy_data.raw.shallow_copy(raw_data);

    window_->proxy_->send_data(proxy_data);

    data_has_changed.store(true, std::memory_order_relaxed);

    const auto total_duration = player_->get_info().total_duration;
    double process = total_duration > 0 ? 100.0 * timestamp / 1000.0 / total_duration : 0.0;
    QMetaObject::invokeMethod(this, "update_progress_for_player", Qt::QueuedConnection, Q_ARG(double, process));
  });

  player_->register_status_callback([this](vlink::BagReader::Status status) {
    QMetaObject::invokeMethod(this, "update_status_for_player", Qt::QueuedConnection, Q_ARG(int, status));
  });

  const auto& player_info = player_->get_info();

  for (const auto& meta : player_info.url_metas) {
    if (meta.url_type == "Method") {
      continue;
    }

    QListWidgetItem* item = new QListWidgetItem;
    ui->listWidget->addItem(item);
    item->setData(Qt::UserRole, QString::fromStdString(meta.url));
    item->setData(Qt::ToolTipRole, QString::fromStdString(meta.url));
    QCheckBox* checkbox = new QCheckBox(ui->listWidget);
    checkbox->setChecked(true);
    connect(checkbox, &QCheckBox::clicked, this, [this](bool) { update_status(); });
    checkbox->setText(QString::fromStdString(meta.url));
    ui->listWidget->setItemWidget(item, checkbox);

    ser_map_.emplace(meta.url, meta.ser_type);
    schema_type_map_.emplace(meta.url, vlink::SchemaData::resolve_type(meta.schema_type, meta.ser_type));
  }

  //

  ui->label_filename2->setText(QString::fromUtf8(player_info.file_name.c_str()));

  if (player_info.file_size < 1024 * 1024) {
    ui->label_filesize2->setText(QString::number(player_info.file_size / 1024.0F, 'f', 2) + "KB");
  } else if (player_info.file_size < 1024 * 1024 * 1024) {
    ui->label_filesize2->setText(QString::number(player_info.file_size / 1024 / 1024.0F, 'f', 2) + "MB");
  } else {
    ui->label_filesize2->setText(QString::number(player_info.file_size / 1024 / 1024 / 1024.0F, 'f', 2) + "GB");
  }

  ui->label_tag2->setText(QString::fromUtf8(player_info.tag_name.c_str()));
  ui->label_version2->setText(QString::fromStdString(player_info.version));

  QString info_flags;

  if (player_info.has_completed) {
    info_flags.append("completed | ");
  }

  if (player_info.has_idx_elapsed) {
    info_flags.append("idx_elapsed | ");
  }

  if (player_info.has_idx_url) {
    info_flags.append("idx_url | ");
  }

  if (player_info.has_schema) {
    info_flags.append("schema | ");
  }

  if (info_flags.size() >= 3) {
    info_flags.chop(3);
  }

  ui->label_flags2->setText(info_flags);

  ui->label_compression2->setText(QString::fromStdString(player_info.compression_type));
  ui->label_process2->setText(QString::fromUtf8(player_info.process_name.c_str()));

  QString date_append_str;

  if (player_info.timezone == 0) {
    date_append_str = tr(" (UTC)");
  } else {
    if (player_info.timezone > 0) {
      date_append_str = tr(" (Timezone: +");
    } else {
      date_append_str = tr(" (Timezone: -");
    }

    int hours = std::abs(player_info.timezone) / 60;
    int minutes = std::abs(player_info.timezone) % 60;

    date_append_str += QString("%1:%2:00)").arg(hours, 2, 10, QChar('0')).arg(minutes, 2, 10, QChar('0'));
  }

  ui->label_date2->setText(QString::fromStdString(player_info.date_time) + date_append_str);

  ui->label_duration2->setText(
      QString::fromStdString(vlink::Helpers::format_milliseconds(player_info.blank_duration, true)) + " ~ " +
      QString::fromStdString(vlink::Helpers::format_milliseconds(player_info.total_duration, true)));

  ui->label_msgcount2->setText(QString::number(player_info.message_count));

  if (player_info.split_count > 0) {
    if (player_info.split_by_time > 0) {
      ui->label_split2->setText(QString::number(player_info.split_count) +
                                tr(" (By time: %1s)").arg(QString::number(player_info.split_by_time / 1000.0, 'f', 2)));
    } else if (player_info.split_by_size > 0) {
      ui->label_split2->setText(
          QString::number(player_info.split_count) +
          tr(" (By size: %1GB)").arg(QString::number(player_info.split_by_size / 1024.0 / 1024.0 / 1024.0, 'f', 2)));
    } else {
      ui->label_split2->setText(QString::number(player_info.split_count));
    }

  } else {
    ui->label_split2->setText("---");
  }

  update_status();
}

void PlayDialog::on_pushButton_start_clicked() {
  if (!player_) {
    return;
  }

  vlink::BagReader::Config config;

  if (ui->groupBox_config->isChecked()) {
    config.begin_time = ui->doubleSpinBox_begin->isEnabled() ? (ui->doubleSpinBox_begin->value() * 1000) : 0;
    config.end_time = ui->doubleSpinBox_end->value() * 1000;
    config.times = ui->spinBox_loop->value();
    config.rate = ui->doubleSpinBox_rate->value();
    config.skip_blank = ui->checkBox_blank->isChecked();
    config.force_delay = -1;
    config.auto_quit = false;
  }

  bool has_unselected = false;

  vlink::ProxyAPI::Control control;
  control.mode = vlink::ProxyAPI::kPlay;

  {
    std::lock_guard lock(url_mtx_);
    bool skipped_missing_meta = false;

    url_list_.clear();

    for (int i = 0; i < ui->listWidget->count(); ++i) {
      auto* item = ui->listWidget->item(i);
      auto* checkbox = qobject_cast<QCheckBox*>(ui->listWidget->itemWidget(item));

      if (!item->isHidden()) {
        if (checkbox->isChecked()) {
          const auto& url = item->data(Qt::UserRole).toString().toStdString();
          auto ser_iter = ser_map_.find(url);
          auto schema_iter = schema_type_map_.find(url);

          if (ser_iter == ser_map_.end() || ser_iter->second.empty()) {
            skipped_missing_meta = true;
            continue;
          }

          const auto schema_type =
              schema_iter != schema_type_map_.end() ? schema_iter->second : vlink::SchemaType::kUnknown;

          url_list_.emplace(url);

          control.url_meta_list.emplace_back(
              vlink::ProxyAPI::UrlMeta{url, ser_iter->second, schema_type, vlink::kPublisher});
        } else {
          has_unselected = true;
        }
      }
    }

    if (skipped_missing_meta && control.url_meta_list.empty()) {
      QMessageBox::warning(this, tr("Warning"),
                           tr("Some selected bag topics were skipped because schema metadata is incomplete."));
      return;
    }

    if (skipped_missing_meta) {
      QMessageBox::warning(this, tr("Warning"),
                           tr("Some selected bag topics were skipped because schema metadata is incomplete."));
    }

    if (window_->proxy_->get_current_config().role == vlink::ProxyAPI::kController) {
      window_->proxy_->send_control(control);
    }
  }

  if (!has_unselected) {
    config.filter_urls = url_list_;
  }

  vlink::Utils::yield_cpu();

  player_->play(config);

  label_timer_->start();
  update_progress_for_player(0);
}

void PlayDialog::on_pushButton_pause_clicked() {
  if (!player_) {
    return;
  }

  player_->pause();

  label_timer_->stop();
}

void PlayDialog::on_pushButton_resume_clicked() {
  if (!player_) {
    return;
  }

  player_->resume();

  label_timer_->start();
}

void PlayDialog::on_pushButton_stop_clicked() {
  if (!player_) {
    return;
  }

  if (player_) {
    player_->stop();
  }

  label_timer_->stop();
}

void PlayDialog::on_pushButton_close_clicked() { this->close(); }

void PlayDialog::on_checkBox_blank_toggled(bool checked) {
  if (checked) {
    ui->doubleSpinBox_begin->setEnabled(false);
  } else {
    ui->doubleSpinBox_begin->setEnabled(true);
  }
}

void PlayDialog::update_time_label() {
  if (data_has_changed.exchange(false, std::memory_order_relaxed)) {
    if (status_ == kPlaying || status_ == kPaused) {
      ui->label_systime2->setStyleSheet("QLabel { color: green; }");
    }
  } else {
    if (status_ == kPlaying || status_ == kPaused) {
      ui->label_systime2->setStyleSheet("QLabel { color: red; }");
    }
  }

  if (status_ == kPlaying || status_ == kPaused) {
    ui->label_systime2->setText(
        QString::fromStdString(vlink::Helpers::format_milliseconds(player_->get_timestamp(), true) + "/" +
                               vlink::Helpers::format_milliseconds(player_->get_info().total_duration, true)));
  }
}

void PlayDialog::update_status_for_player(int status) {
  switch (status) {
    case vlink::BagReader::kStopped:
      status_ = kStopped;
      break;
    case vlink::BagReader::kPaused:
      status_ = kPaused;
      break;
    case vlink::BagReader::kPlaying:
      status_ = kPlaying;
      break;
    default:
      break;
  }

  update_status();

  update_time_label();
}

void PlayDialog::update_progress_for_player(double progress) {
  ui->progressBar->setValue(progress);
  ui->progressBar->setFormat(QString("%1%").arg(QString::number(progress, 'f', 2)));
}

void PlayDialog::update_status() {
  bool has_url = false;
  for (int i = 0; i < ui->listWidget->count(); ++i) {
    auto* item = ui->listWidget->item(i);
    auto* checkbox = qobject_cast<QCheckBox*>(ui->listWidget->itemWidget(item));

    if (!item->isHidden() && checkbox->isChecked()) {
      has_url = true;
      break;
    }
  }

  if (!has_url) {
    status_ = kDisable;
  } else if (ui->lineEdit_load->text().isEmpty()) {
    status_ = kDisable;
  } else if (status_ == kDisable) {
    status_ = kStopped;
  }

  switch (status_) {
    case kDisable:
      ui->pushButton_start->setEnabled(false);
      ui->pushButton_pause->setEnabled(false);
      ui->pushButton_resume->setEnabled(false);
      ui->pushButton_stop->setEnabled(false);
      ui->pushButton_select->setEnabled(true);
      ui->lineEdit_load->setEnabled(true);
      ui->label_load->setEnabled(true);
      ui->listWidget->setEnabled(true);
      ui->pushButton_all->setEnabled(ui->listWidget->count() != 0);
      ui->pushButton_unall->setEnabled(ui->listWidget->count() != 0);
      ui->label_status2->setText("---");
      ui->label_status2->setStyleSheet("");
      ui->label_systime2->setStyleSheet("");
      ui->label_systime2->setText("---");
      ui->groupBox_config->setEnabled(false);
      ui->groupBox_state->setEnabled(false);
      ui->groupBox_info->setEnabled(false);
      break;
    case kStopped:
      ui->pushButton_start->setEnabled(true);
      ui->pushButton_pause->setEnabled(false);
      ui->pushButton_resume->setEnabled(false);
      ui->pushButton_stop->setEnabled(false);
      ui->pushButton_select->setEnabled(true);
      ui->lineEdit_load->setEnabled(true);
      ui->label_load->setEnabled(true);
      ui->listWidget->setEnabled(true);
      ui->pushButton_all->setEnabled(ui->listWidget->count() != 0);
      ui->pushButton_unall->setEnabled(ui->listWidget->count() != 0);
      ui->label_status2->setText(tr("Stopped"));
      ui->label_status2->setStyleSheet("QLabel { color: red; }");
      ui->label_systime2->setStyleSheet("");
      ui->label_systime2->setText(
          QString::fromStdString(vlink::Helpers::format_milliseconds(0, true) + "/" +
                                 vlink::Helpers::format_milliseconds(player_->get_info().total_duration, true)));
      ui->groupBox_config->setEnabled(true);
      ui->groupBox_state->setEnabled(true);
      ui->groupBox_info->setEnabled(true);
      break;
    case kPlaying:
      ui->pushButton_start->setEnabled(false);
      ui->pushButton_pause->setEnabled(true);
      ui->pushButton_resume->setEnabled(false);
      ui->pushButton_stop->setEnabled(true);
      ui->pushButton_select->setEnabled(false);
      ui->lineEdit_load->setEnabled(false);
      ui->label_load->setEnabled(false);
      ui->listWidget->setEnabled(false);
      ui->pushButton_all->setEnabled(false);
      ui->pushButton_unall->setEnabled(false);
      ui->label_status2->setText(tr("Playing"));
      ui->label_status2->setStyleSheet("QLabel { color: green; }");
      ui->groupBox_config->setEnabled(false);
      ui->groupBox_state->setEnabled(true);
      ui->groupBox_info->setEnabled(true);
      break;
    case kPaused:
      ui->pushButton_start->setEnabled(false);
      ui->pushButton_pause->setEnabled(false);
      ui->pushButton_resume->setEnabled(true);
      ui->pushButton_stop->setEnabled(true);
      ui->pushButton_select->setEnabled(false);
      ui->lineEdit_load->setEnabled(false);
      ui->label_load->setEnabled(false);
      ui->listWidget->setEnabled(false);
      ui->pushButton_all->setEnabled(false);
      ui->pushButton_unall->setEnabled(false);
      ui->label_status2->setText(tr("Paused"));
      ui->label_status2->setStyleSheet("QLabel { color: red; }");
      ui->groupBox_config->setEnabled(false);
      ui->groupBox_state->setEnabled(true);
      ui->groupBox_info->setEnabled(true);
      break;
    default:
      break;
  }
}

void PlayDialog::closeEvent(QCloseEvent* event) {
  if (!window_->proxy_->is_connected() || status_ == kDisable || status_ == kStopped) {
    QDialog::closeEvent(event);
    return;
  }

  if (QMessageBox::question(this, tr("Close"), tr("Task is still running, are you sure you want to close?")) ==
      QMessageBox::Yes) {
    event->accept();
  } else {
    event->ignore();
  }
}

// NOLINTEND
