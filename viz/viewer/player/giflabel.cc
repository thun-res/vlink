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

#include "./giflabel.h"

#include <vlink/base/logger.h>

#include <QGuiApplication>
#include <QMovie>
#include <QPainter>

GifLabel::GifLabel(QWidget* parent) : QLabel(parent) {
  movie_ = new QMovie(":/resource/wait.gif");

  movie_->jumpToFrame(0);

  img_size_ = movie_->currentPixmap().size();

  this->setFixedSize(img_size_);

  update_current_pixmap();

  connect(movie_, &QMovie::frameChanged, this, [this](int) {
    update_current_pixmap();
    this->update();
  });
}

GifLabel::~GifLabel() {
  delete movie_;

  movie_ = nullptr;
}

void GifLabel::start() { movie_->start(); }

void GifLabel::stop() { movie_->stop(); }

void GifLabel::paintEvent(QPaintEvent* event) {
  (void)event;

  QPainter painter(this);

  painter.setRenderHint(QPainter::Antialiasing);

  painter.drawPixmap(0, 0, pixmap_);
}

void GifLabel::update_current_pixmap() {
  auto scale_factor = qApp->devicePixelRatio();

  if (scale_factor == 1) {
    pixmap_ = movie_->currentPixmap();
  } else {
    pixmap_ = movie_->currentPixmap().scaled(img_size_ * scale_factor, Qt::KeepAspectRatioByExpanding,
                                             Qt::SmoothTransformation);
  }

  pixmap_.setDevicePixelRatio(scale_factor);
}

// NOLINTEND
