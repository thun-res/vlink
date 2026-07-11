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

#include "./recorddialog.h"

#include <vlink/base/helpers.h>

#include <QCheckBox>
#include <QFileDialog>
#include <QMessageBox>
#include <QRegularExpressionValidator>
#include <QSettings>
#include <QStandardPaths>
#include <mutex>
#include <string>

#include "./mainwindow.h"
#include "./ui_mainwindow.h"
#include "./ui_recorddialog.h"

RecordDialog::RecordDialog(QWidget* parent) : QDialog(parent), ui(new Ui::RecordDialog) {
  setWindowFlags(Qt::Dialog | Qt::WindowMaximizeButtonHint | Qt::WindowCloseButtonHint);

  ui->setupUi(this);

  window_ = MainWindow::get_instance();

  vlink::BagWriter::Config config;

  validator_ = new QRegularExpressionValidator(QRegularExpression("[0-9]*"), this);
  ui->lineEdit_row->setValidator(validator_);
  ui->lineEdit_row->setText(QString::number(config.max_row_count));
  ui->doubleSpinBox_bytes->setValue(config.max_bytes_size / 1024LL / 1024LL / 1024LL);
  ui->doubleSpinBox_split_size->setValue(config.split_by_size / 1024LL / 1024LL / 1024LL);
  ui->doubleSpinBox_split_time->setValue(config.split_by_time / 1000.0);
  ui->doubleSpinBox_cache->setValue(config.cache_size / 1024LL / 1024LL);

  ui->checkBox_compress->setChecked(false);
  ui->checkBox_wal->setChecked(config.wal_mode);
  ui->checkBox_limit->setChecked(config.enable_limit);
  ui->checkBox_split_name->setChecked(config.split_name_by_time);

  if (window_->des_pool_ || !window_->flatbuffers_runtime_.empty()) {
    ui->checkBox_schema->setEnabled(true);
    ui->checkBox_schema->setChecked(true);
  } else {
    ui->checkBox_schema->setEnabled(false);
    ui->checkBox_schema->setChecked(false);
  }

  label_timer_ = new QTimer(this);
  label_timer_->setInterval(50);

  connect(label_timer_, &QTimer::timeout, this, [this]() { update_time_label(); });

  {
    std::lock_guard lock(window_->data_mutex_);

    window_->data_callback_ = [this](const vlink::ProxyAPI::Data& proxy_data) {
      std::shared_ptr<vlink::BagWriter> recorder;
      int64_t timestamp = 0;
      bool should_push = false;

      {
        std::unique_lock select_lock(select_mtx_);

        if (status_ != kRecording || select_urls_.count(proxy_data.url) == 0 || !recorder_) {
          return;
        }

        if (window_->proxy_->get_current_config().role == vlink::ProxyAPI::kController) {
          if (proxy_data.timestamp - record_timer_.get() * 1000U > 1000'000U) {
            return;
          }
        }

        timestamp = proxy_data.timestamp - pause_total_ * 1000;

        if (timestamp >= 0 && last_timestamp_ <= timestamp + 10'000U) {
          data_has_changed.store(true, std::memory_order_relaxed);

          if (window_->proxy_->get_current_config().role != vlink::ProxyAPI::kController) {
            if (dx_timestamp_ < 0) {
              dx_timestamp_ = timestamp;
              timestamp = 0;
            } else {
              timestamp -= dx_timestamp_;
            }
          }

          url_set.emplace(proxy_data.url);

          if (!window_->proxy_->get_current_config().direct) {
            loss_map_[proxy_data.url].update(proxy_data.seq);
          }

          recorder = recorder_;
          should_push = true;

          ++progress_index_;
          if (progress_index_ > 50) {
            progress_index_ = 0;
          }
        } else {
          VLOG_W("Ignore timestamp: out of range.");
          VLOG_W("Last_timestamp: ", last_timestamp_, " | ", "Timestamp: ", timestamp, ".");
        }

        last_timestamp_ = timestamp;
      }

      if (should_push) {
        vlink::Frame frame;
        frame.timestamp = timestamp;
        frame.url = proxy_data.url;
        frame.ser_type = proxy_data.ser;
        frame.schema_type = proxy_data.schema;
        frame.action_type = vlink::ActionType::kSubscribe;
        frame.data = vlink::Bytes::shallow_copy(proxy_data.raw.data(), proxy_data.raw.size());
        *recorder << frame;
      }
    };
  }

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

  for (const auto& url : window_->can_record_urls_) {
    QListWidgetItem* item = new QListWidgetItem;
    ui->listWidget->addItem(item);
    item->setData(Qt::UserRole, QString::fromStdString(url));
    item->setData(Qt::ToolTipRole, QString::fromStdString(url));

    QCheckBox* checkbox = new QCheckBox(ui->listWidget);
    connect(checkbox, &QCheckBox::clicked, this, [this](bool) { update_status(); });
    checkbox->setText(QString::fromStdString(url));
    ui->listWidget->setItemWidget(item, checkbox);

    const auto& selected_items = window_->ui->treeWidget_url->selectedItems();

    for (const auto& pitem : selected_items) {
      if (pitem->text(1) == checkbox->text()) {
        checkbox->setChecked(true);
      }
    }
  }

  update_status();

  ui->pushButton_close->setFocusPolicy(Qt::NoFocus);

  setFocus();
}

RecordDialog::FinalizeResult RecordDialog::finalize_recorder(const std::shared_ptr<vlink::BagWriter>& recorder) {
  bool stopped = !recorder->is_running();

  if (recorder->is_running()) {
    recorder->quit(!recorder->wait_for_idle(5000));
    stopped = recorder->wait_for_quit(5000);
  }

  if VUNLIKELY (!stopped) {
    return kStopTimeout;
  }

  set_record_loss();
  recorder->close();

  if VUNLIKELY (recorder->fail()) {
    return kFinalizeFailed;
  }

  return kFinalized;
}

RecordDialog::~RecordDialog() {
  {
    std::lock_guard lock(window_->data_mutex_);

    window_->data_callback_ = nullptr;
  }

  {
    std::unique_lock select_lock(select_mtx_);
    status_ = kStopped;
    select_urls_.clear();
  }

  for (int i = 0; i < ui->listWidget->count(); ++i) {
    auto* item = ui->listWidget->takeItem(i);
    auto* checkbox = qobject_cast<QCheckBox*>(ui->listWidget->itemWidget(item));
    delete checkbox;
    delete item;
    --i;
  }

  std::shared_ptr<vlink::BagWriter> recorder;
  {
    std::lock_guard select_lock(select_mtx_);
    recorder = recorder_;
  }

  if (recorder) {
    const FinalizeResult result = finalize_recorder(recorder);

    if VUNLIKELY (result == kFinalizeFailed) {
      CLOG_E("recorddialog: failed to finalize recording");
    } else if VUNLIKELY (result == kStopTimeout) {
      CLOG_E("recorddialog: timed out while stopping recording");
    }
  }

  {
    std::unique_lock select_lock(select_mtx_);
    recorder_.reset();
  }

  delete label_timer_;

  delete validator_;

  delete ui;
}

void RecordDialog::on_pushButton_all_clicked() {
  for (int i = 0; i < ui->listWidget->count(); ++i) {
    auto* item = ui->listWidget->item(i);
    auto* checkbox = qobject_cast<QCheckBox*>(ui->listWidget->itemWidget(item));

    if (!item->isHidden()) {
      checkbox->setChecked(true);
    }
  }

  update_status();
}

void RecordDialog::on_pushButton_unall_clicked() {
  for (int i = 0; i < ui->listWidget->count(); ++i) {
    auto* item = ui->listWidget->item(i);
    auto* checkbox = qobject_cast<QCheckBox*>(ui->listWidget->itemWidget(item));

    if (!item->isHidden()) {
      checkbox->setChecked(false);
    }
  }

  update_status();
}

void RecordDialog::on_pushButton_select_clicked() {
  QSettings settings(QStandardPaths::writableLocation(QStandardPaths::CacheLocation) + "/settings.ini",
                     QSettings::IniFormat);

  settings.beginGroup("RecordDialog");

  QFileDialog dialog(this, tr("Select bag file"), settings.value("bag_dir", qApp->applicationDirPath()).toString(),
                     "Bag files (*.vdb *.vdbx *.vcap *.vcapx)");

  settings.endGroup();

#if QT_VERSION < QT_VERSION_CHECK(5, 15, 0)
  dialog.setOption(QFileDialog::DontUseNativeDialog, true);
#endif

  dialog.setFileMode(QFileDialog::AnyFile);
  dialog.setAcceptMode(QFileDialog::AcceptSave);
  dialog.setDefaultSuffix("vdb");

  if (dialog.exec() != QDialog::Accepted) {
    return;
  }

  QString file_path = dialog.selectedFiles().constFirst();

  if (file_path.isEmpty()) {
    return;
  }

  settings.beginGroup("RecordDialog");
  settings.setValue("bag_dir", QFileInfo(file_path).dir().path());
  settings.endGroup();
  settings.sync();

  ui->lineEdit_save->setText(file_path);

  update_status();
}

void RecordDialog::on_pushButton_start_clicked() {
  vlink::BagWriter::Config config;

  if (ui->groupBox_config->isChecked()) {
    config.compress =
        ui->checkBox_compress->isChecked() ? vlink::BagWriter::kCompressAuto : vlink::BagWriter::kCompressNone;
    config.max_row_count = ui->lineEdit_row->text().toLongLong();
    config.max_bytes_size = 1024LL * 1024LL * 1024LL * ui->doubleSpinBox_bytes->value();
    config.cache_size = 1024LL * 1024LL * ui->doubleSpinBox_cache->value();
    config.wal_mode = ui->checkBox_wal->isChecked();
    config.enable_limit = ui->checkBox_limit->isChecked();
    config.split_name_by_time = ui->checkBox_split_name->isChecked();
    config.split_by_size = 1024LL * 1024LL * 1024LL * ui->doubleSpinBox_split_size->value();
    config.split_by_time = 1000.0 * ui->doubleSpinBox_split_time->value();
    config.begin_time = 0;
    config.optimize_on_exit = true;
  }

  loss_map_.clear();

  try {
    recorder_ = vlink::BagWriter::create(ui->lineEdit_save->text().toStdString(), config);
  } catch (vlink::Exception::RuntimeError& e) {
    QMessageBox::critical(this, tr("Error"), QString::fromStdString(e.what()));
    return;
  }

  recorder_->register_split_callback(
      [this](int split_index, const std::string& split_filename) {
        (void)split_filename;

        if (split_index == 0) {
          return;
        }

        set_record_loss();
      },
      true);

  recorder_->async_run();

  bool skipped_missing_meta = false;
  bool skipped_missing_schema = false;
  vlink::ProxyAPI::Control control;
  control.mode = vlink::ProxyAPI::kRecord;

  {
    std::unique_lock select_lock(select_mtx_);
    select_urls_.clear();
    std::unordered_set<std::string> pushed_schema_set;
    std::unordered_set<std::string> failed_schema_set;

    for (int i = 0; i < ui->listWidget->count(); ++i) {
      auto* item = ui->listWidget->item(i);
      auto* checkbox = qobject_cast<QCheckBox*>(ui->listWidget->itemWidget(item));

      if (!item->isHidden() && checkbox->isChecked()) {
        auto url_str = item->data(Qt::UserRole).toString().toStdString();
        auto ser_iter = window_->ser_map_.find(url_str);
        const auto schema_iter = window_->schema_type_map_.find(url_str);
        auto schema_type =
            schema_iter != window_->schema_type_map_.end() ? schema_iter->second : vlink::SchemaType::kUnknown;
        const auto schema_key = ser_iter != window_->ser_map_.end()
                                    ? ser_iter->second + "#" + std::to_string(static_cast<int>(schema_type))
                                    : std::string();

        if (ser_iter == window_->ser_map_.end() || ser_iter->second.empty()) {
          skipped_missing_meta = true;
          continue;
        }

        if (!vlink::SchemaData::is_real_type(schema_type)) {
          skipped_missing_meta = true;
          continue;
        }

        if (failed_schema_set.count(schema_key) != 0) {
          skipped_missing_schema = true;
          continue;
        }

        if (ui->checkBox_schema->isChecked() && pushed_schema_set.emplace(schema_key).second) {
          auto schema = window_->search_schema(ser_iter->second, schema_type);

          if (!schema.encoding.empty()) {
            if (!recorder_->push_schema(schema, true)) {
              CLOG_W("recorddialog: push_schema failed for ser=[%s] schema_type=[%d]", schema.name.c_str(),
                     static_cast<int>(schema.schema_type));
              failed_schema_set.emplace(schema_key);
              skipped_missing_schema = true;
              continue;
            }
          } else {
            failed_schema_set.emplace(schema_key);
            skipped_missing_schema = true;
            continue;
          }
        }

        control.url_meta_list.emplace_back(
            vlink::ProxyAPI::UrlMeta{url_str, ser_iter->second, schema_type, vlink::kSubscriber});
        select_urls_.emplace(url_str);
      }
    }
  }

  record_timer_.restart();

  pause_total_ = 0;

  label_timer_->start();

  update_time_label();

  last_timestamp_ = 0;

  dx_timestamp_ = -1;

  {
    std::lock_guard select_lock(select_mtx_);
    status_ = kRecording;
  }

  if (window_->proxy_->get_current_config().role == vlink::ProxyAPI::kController) {
    window_->proxy_->send_control(control);
  }

  if (skipped_missing_meta) {
    QMessageBox::warning(this, tr("Warning"),
                         tr("Some selected topics were skipped because schema metadata is incomplete."));
  } else if (skipped_missing_schema) {
    QMessageBox::warning(this, tr("Warning"),
                         tr("Some selected topics could not preload schema data with the current schema_type."));
  }

  update_status();
}

void RecordDialog::on_pushButton_pause_clicked() {
  pause_timer_.restart();

  label_timer_->stop();

  update_time_label();

  {
    std::lock_guard select_lock(select_mtx_);
    status_ = kPaused;
  }

  update_status();
}

void RecordDialog::on_pushButton_resume_clicked() {
  {
    std::lock_guard select_lock(select_mtx_);
    pause_total_ += pause_timer_.get();
    status_ = kRecording;
  }

  label_timer_->start();

  update_time_label();

  update_status();
}

void RecordDialog::on_pushButton_stop_clicked() {
  std::shared_ptr<vlink::BagWriter> recorder;
  {
    std::lock_guard select_lock(select_mtx_);
    status_ = kStopped;
    select_urls_.clear();
    recorder = recorder_;
  }

  if (recorder) {
    if VUNLIKELY (finalize_recorder(recorder) != kFinalized) {
      QMessageBox::warning(this, tr("Warning"), tr("The recording file could not be finalized completely."));
    }
  }

  {
    std::lock_guard select_lock(select_mtx_);
    recorder_.reset();
  }

  if (window_->proxy_->get_current_config().role == vlink::ProxyAPI::kController) {
    vlink::ProxyAPI::Control control;
    control.mode = vlink::ProxyAPI::kAuto;
    window_->proxy_->send_control(control);
  }

  label_timer_->stop();

  update_time_label();

  update_status();
}

void RecordDialog::on_pushButton_close_clicked() { this->close(); }

void RecordDialog::update_time_label() {
  if (data_has_changed.exchange(false, std::memory_order_relaxed)) {
    if (status_ == kRecording || status_ == kPaused) {
      ui->label_systime2->setStyleSheet("QLabel { color: green; }");
    }
  } else {
    if (status_ == kRecording || status_ == kPaused) {
      ui->label_systime2->setStyleSheet("QLabel { color: red; }");
    }
  }

  if (status_ == kRecording || status_ == kPaused) {
    ui->label_systime2->setText(
        QString::fromStdString(vlink::Helpers::format_milliseconds(record_timer_.get() - pause_total_, true)));
  }

  ui->label_progress->setText(QString(progress_index_, '.'));
}

void RecordDialog::update_status() {
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
  } else if (ui->lineEdit_save->text().isEmpty()) {
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
      ui->pushButton_select->setEnabled(has_url);
      ui->lineEdit_save->setEnabled(has_url);
      ui->label_save->setEnabled(has_url);
      ui->listWidget->setEnabled(true);
      ui->pushButton_all->setEnabled(ui->listWidget->count() != 0);
      ui->pushButton_unall->setEnabled(ui->listWidget->count() != 0);
      ui->label_status2->setText("---");
      ui->label_status2->setStyleSheet("");
      ui->label_systime2->setStyleSheet("");
      ui->label_systime2->setText("---");
      ui->groupBox_config->setEnabled(false);
      ui->groupBox_state->setEnabled(false);
      ui->label_progress->clear();
      progress_index_ = 0;
      break;
    case kStopped:
      ui->pushButton_start->setEnabled(true);
      ui->pushButton_pause->setEnabled(false);
      ui->pushButton_resume->setEnabled(false);
      ui->pushButton_stop->setEnabled(false);
      ui->pushButton_select->setEnabled(has_url);
      ui->lineEdit_save->setEnabled(has_url);
      ui->label_save->setEnabled(has_url);
      ui->listWidget->setEnabled(true);
      ui->pushButton_all->setEnabled(ui->listWidget->count() != 0);
      ui->pushButton_unall->setEnabled(ui->listWidget->count() != 0);
      ui->label_status2->setText(tr("Stopped"));
      ui->label_status2->setStyleSheet("QLabel { color: red; }");
      ui->label_systime2->setStyleSheet("");
      ui->label_systime2->setText("---");
      ui->groupBox_config->setEnabled(true);
      ui->groupBox_state->setEnabled(true);
      ui->label_progress->clear();
      progress_index_ = 0;
      break;
    case kRecording:
      ui->pushButton_start->setEnabled(false);
      ui->pushButton_pause->setEnabled(true);
      ui->pushButton_resume->setEnabled(false);
      ui->pushButton_stop->setEnabled(true);
      ui->pushButton_select->setEnabled(false);
      ui->lineEdit_save->setEnabled(false);
      ui->label_save->setEnabled(false);
      ui->listWidget->setEnabled(false);
      ui->pushButton_all->setEnabled(false);
      ui->pushButton_unall->setEnabled(false);
      ui->label_status2->setText(tr("Recording"));
      ui->label_status2->setStyleSheet("QLabel { color: green; }");
      ui->groupBox_config->setEnabled(false);
      ui->groupBox_state->setEnabled(true);
      break;
    case kPaused:
      ui->pushButton_start->setEnabled(false);
      ui->pushButton_pause->setEnabled(false);
      ui->pushButton_resume->setEnabled(true);
      ui->pushButton_stop->setEnabled(true);
      ui->pushButton_select->setEnabled(false);
      ui->lineEdit_save->setEnabled(false);
      ui->label_save->setEnabled(false);
      ui->listWidget->setEnabled(false);
      ui->pushButton_all->setEnabled(false);
      ui->pushButton_unall->setEnabled(false);
      ui->label_status2->setText(tr("Paused"));
      ui->label_status2->setStyleSheet("QLabel { color: red; }");
      ui->groupBox_config->setEnabled(false);
      ui->groupBox_state->setEnabled(true);
      break;
    default:
      break;
  }
}

void RecordDialog::set_record_loss() {
  std::lock_guard select_lock(select_mtx_);

  if (!recorder_ || window_->proxy_->get_current_config().direct) {
    return;
  }

  double total = 0;
  double lost = 0;

  for (const auto& url : url_set) {
    total = loss_map_[url].get_total();
    lost = loss_map_[url].get_lost();

    if (total > 0 && lost > 0) {
      recorder_->set_url_loss(url, lost / total);
    } else {
      recorder_->set_url_loss(url, 0);
    }
  }
}

void RecordDialog::closeEvent(QCloseEvent* event) {
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
