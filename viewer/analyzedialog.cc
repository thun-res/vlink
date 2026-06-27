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

#include "./analyzedialog.h"

#include <vlink/base/bytes.h>

#include <QMessageBox>
#include <cmath>

#include "./mainwindow.h"
#include "./ui_analyzedialog.h"

AnalyzeDialog::AnalyzeDialog(QWidget* parent) : QDialog(parent), ui(new Ui::AnalyzeDialog) {
  setWindowFlags(Qt::Dialog | Qt::WindowCloseButtonHint);

  ui->setupUi(this);

  window_ = MainWindow::get_instance();

  connect(ui->horizontalSlider, &QSlider::valueChanged, this, [this](int value) {
    ui->widget_number->set_max_count(value);
    ui->widget_number->update();
  });

  ui->widget_number->set_unit(1);
  ui->widget_number->set_suffix(0);
  ui->widget_number->set_unit_text("(VALUE)");
  ui->widget_number->set_header_size(QSize(150, 30));
  ui->widget_number->set_value_range(0, 100);
  ui->widget_number->set_total_max_count(ui->horizontalSlider->maximum());
  ui->widget_number->set_count_text("t");
  ui->widget_number->set_color(Qt::darkGreen);

  ui->stackedWidget->setCurrentIndex(0);
  ui->horizontalSlider->setValue(100);

  ui->pushButton_pause->setEnabled(true);
  ui->pushButton_resume->setEnabled(false);

  ui->pushButton_close->setFocusPolicy(Qt::NoFocus);
  setFocus();
}

AnalyzeDialog::~AnalyzeDialog() { delete ui; }

void AnalyzeDialog::init(Type type) {
  if (type == kUnknownType) {
    return;
  }

  ui->widget_number->clear();
  ui->plainTextEdit->clear();
  ui->horizontalSlider->setValue(100);
  ui->widget_number->set_value_range(0, 100);
  is_paused = false;
  ui->widget_number->update();

  current_type_ = type;

  if (current_type_ == kNumberType) {
    ui->stackedWidget->setCurrentIndex(0);
  } else if (current_type_ == kStringType) {
    ui->stackedWidget->setCurrentIndex(1);
  } else if (current_type_ == kRawType) {
    ui->stackedWidget->setCurrentIndex(1);
  }

  this->resize(this->minimumSize());

  this->show();
}

bool AnalyzeDialog::is_number_type() const { return current_type_ == kNumberType; }

bool AnalyzeDialog::is_string_type() const { return current_type_ == kStringType; }

bool AnalyzeDialog::is_raw_type() const { return current_type_ == kRawType; }

void AnalyzeDialog::add_number(double number) {
  ui->widget_number->set_block(true);

  if (current_type_ != kNumberType) {
    init(kNumberType);
  }

  ui->widget_number->add_value(number);

  double max_value = ui->widget_number->get_real_max_value();
  double min_value = ui->widget_number->get_real_min_value();

  bool max_value_is_integer = (std::floor(max_value) == max_value);
  bool min_value_is_integer = (std::floor(min_value) == min_value);

  if (max_value_is_integer && min_value_is_integer) {
    ui->widget_number->set_suffix(0);
  } else {
    ui->widget_number->set_suffix(4);
  }

  ui->widget_number->set_value_range(min_value - std::abs(min_value) * 0.05, max_value + std::abs(max_value) * 0.05);

  if (is_paused) {
    ui->widget_number->set_block(false);
    return;
  }

  ui->widget_number->update();

  ui->widget_number->set_block(false);
}

void AnalyzeDialog::add_string(const std::string& str) {
  if (current_type_ != kStringType) {
    init(kStringType);
  }

  if (is_paused) {
    return;
  }

  ui->plainTextEdit->setPlainText(QString::fromStdString(str));
}

void AnalyzeDialog::add_raw(const std::string& str) {
  if (current_type_ != kRawType) {
    init(kRawType);
  }

  if (is_paused) {
    return;
  }

  ui->plainTextEdit->setPlainText(QString::fromStdString(str));
}

void AnalyzeDialog::closeEvent(QCloseEvent* event) {
  QDialog::closeEvent(event);

  ui->widget_number->clear();
  ui->plainTextEdit->clear();
  ui->widget_number->set_unit(1);
  ui->widget_number->set_suffix(0);
  ui->horizontalSlider->setValue(100);
  ui->widget_number->set_value_range(0, 100);

  is_paused = false;

  ui->widget_number->update();

  ui->stackedWidget->setCurrentIndex(0);
  current_type_ = kUnknownType;
}

void AnalyzeDialog::resizeEvent(QResizeEvent* event) { QDialog::resizeEvent(event); }

void AnalyzeDialog::on_pushButton_pause_clicked() {
  is_paused = true;
  ui->pushButton_pause->setEnabled(false);
  ui->pushButton_resume->setEnabled(true);
}

void AnalyzeDialog::on_pushButton_resume_clicked() {
  is_paused = false;
  ui->pushButton_pause->setEnabled(true);
  ui->pushButton_resume->setEnabled(false);
}

void AnalyzeDialog::on_pushButton_close_clicked() { this->close(); }

// NOLINTEND
