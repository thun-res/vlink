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

#include "./rawdialog.h"

#include <QClipboard>
#include <QScrollBar>

#include "./mainwindow.h"
#include "./ui_mainwindow.h"
#include "./ui_rawdialog.h"

RawDialog::RawDialog(QWidget* parent) : QDialog(parent), ui(new Ui::RawDialog) {
  setWindowFlags(Qt::Dialog | Qt::WindowMaximizeButtonHint | Qt::WindowCloseButtonHint);

  ui->setupUi(this);

  {
    QFont font = ui->textEdit->font();
    font.setFamily("Noto Mono");
    font.setPixelSize(12);
    ui->textEdit->setFont(font);
  }

  window_ = MainWindow::get_instance();

  timer_ = new QTimer(this);
  timer_->setInterval(20);

  connect(timer_, &QTimer::timeout, this, [this]() {
    if (!is_changed_) {
      return;
    }

    is_changed_ = false;

    {
      std::lock_guard lock(mtx_);

      auto* bar = ui->textEdit->verticalScrollBar();

      int pos = bar->value();

      ui->textEdit->setText(current_str_);

      if (pos > bar->maximum()) {
        pos = bar->maximum();
      }

      bar->setValue(pos);
    }
  });

  {
    std::lock_guard lock(window_->data_mutex_);

    const auto& selected_items = window_->ui->treeWidget_url->selectedItems();

    select_urls_.clear();

    for (const auto& item : selected_items) {
      select_urls_.emplace(item->text(1).toStdString());
    }

    window_->data_callback_ = [this](const vlink::ProxyAPI::Data& proxy_data) {
      if (is_paused.load(std::memory_order_relaxed)) {
        return;
      }

      if (select_urls_.count(proxy_data.url) == 0) {
        return;
      }

      {
        std::lock_guard lock(mtx_);
        current_str_ =
            QString::fromStdString(vlink::Bytes::convert_to_hex_str(proxy_data.raw.data(), proxy_data.raw.size()));
      }

      is_changed_ = true;
    };
  }

  is_paused.store(false, std::memory_order_relaxed);

  ui->pushButton_pause->setEnabled(true);
  ui->pushButton_resume->setEnabled(false);

  ui->pushButton_close->setFocusPolicy(Qt::NoFocus);

  setFocus();

  timer_->start();
}

RawDialog::~RawDialog() {
  timer_->stop();

  {
    std::lock_guard lock(window_->data_mutex_);

    window_->data_callback_ = nullptr;
  }

  delete ui;
}

void RawDialog::on_pushButton_pause_clicked() {
  is_paused.store(true, std::memory_order_relaxed);

  ui->pushButton_pause->setEnabled(false);
  ui->pushButton_resume->setEnabled(true);

  timer_->stop();
}

void RawDialog::on_pushButton_resume_clicked() {
  is_paused.store(false, std::memory_order_relaxed);

  ui->pushButton_pause->setEnabled(true);
  ui->pushButton_resume->setEnabled(false);

  timer_->start();
}

void RawDialog::on_pushButton_close_clicked() { this->close(); }

// NOLINTEND
