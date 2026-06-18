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

#include "waitwidget.h"

#include <vlink/base/logger.h>

#include <QEvent>
#include <QTimer>

#include "./ui_waitwidget.h"

WaitWidget::WaitWidget(QWidget* parent) : QFrame(parent), ui(new Ui::WaitWidget) {
  setWindowFlags(Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint);

  ui->setupUi(this);

  adjustSize();

  setFixedSize(size());

  timer_ = new QTimer(this);
  timer_->setInterval(50);
  connect(timer_, &QTimer::timeout, this, [this]() {
    if (this->parentWidget()) {
      this->parentWidget()->setEnabled(false);
    }

    this->show();
  });

  this->hide();
}

WaitWidget::~WaitWidget() { delete ui; }

void WaitWidget::adjust_geometry() {
  if (this->parentWidget()) {
    const QRect parent_geometry = this->parentWidget()->geometry();
    const int x = parent_geometry.x() + (parent_geometry.width() - width()) / 2;
    const int y = parent_geometry.y() + (parent_geometry.height() - height()) / 2;

    this->move(x, y);
  }
}

void WaitWidget::start_wait(int delay) {
  if (delay > 0) {
    timer_->setInterval(delay);

    this->hide();

    adjust_geometry();

    ui->label->stop();
    ui->label->start();

    timer_->stop();
    timer_->start();

    if (this->parentWidget()) {
      this->parentWidget()->setEnabled(true);
    }
  } else {
    adjust_geometry();

    ui->label->stop();
    ui->label->start();

    if (this->parentWidget()) {
      this->parentWidget()->setEnabled(false);
    }

    this->show();
  }

  is_working_ = true;
}

void WaitWidget::stop_wait() {
  this->hide();

  ui->label->stop();

  timer_->stop();

  if (this->parentWidget()) {
    this->parentWidget()->setEnabled(true);
  }

  is_working_ = false;
}

bool WaitWidget::is_working() const { return is_working_; }

bool WaitWidget::event(QEvent* event) {
  if (event->type() == QEvent::MouseButtonPress || event->type() == QEvent::KeyPress ||
      event->type() == QEvent::MouseButtonRelease || event->type() == QEvent::KeyRelease ||
      event->type() == QEvent::Wheel) {
    return true;
  }

  return QWidget::event(event);
}

// NOLINTEND
