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

#include "./projectiondialog.h"

#include <vlink/base/logger.h>

#include <QMessageBox>
#include <QRegularExpressionValidator>
#include <QSettings>
#include <QStandardPaths>
#include <algorithm>
#include <cmath>

#include "./ui_projectiondialog.h"

[[maybe_unused]] static bool convert_m3f(const QString& str, Eigen::Matrix3Xf& m3f) {
  if (str.isEmpty()) {
    m3f.setZero();
    return false;
  }

  const auto& list = str.split(",");

  if (list.size() != 9) {
    m3f.setZero();
    return false;
  }

  m3f.resize(3, 3);

  bool ok = true;
  int index = 0;

  for (int row = 0; row < 3; ++row) {
    for (int col = 0; col < 3; ++col) {
      auto s = list[index].trimmed();

      if (s.isEmpty()) {
        m3f(row, col) = 0;
      } else {
        m3f(row, col) = s.toFloat(&ok);

        if (!ok) {
          m3f.setZero();
          return false;
        }
      }

      ++index;
    }
  }

  return true;
}

[[maybe_unused]] static bool convert_v3f(const QString& str, Eigen::Vector3f& v3f) {
  if (str.isEmpty()) {
    v3f.setZero();
    return false;
  }

  const auto& list = str.split(",");

  if (list.size() != 3) {
    v3f.setZero();
    return false;
  }

  bool ok = false;

  for (int i = 0; i < list.size(); ++i) {
    auto s = list[i].trimmed();

    if (s.isEmpty()) {
      v3f[i] = 0;
    } else {
      v3f[i] = s.toFloat(&ok);

      if (!ok) {
        v3f.setZero();
        return false;
      }
    }
  }

  return true;
}

[[maybe_unused]] static bool convert_v5f(const QString& str, std::array<float, 5>& v5f) {
  if (str.isEmpty()) {
    v5f = std::array<float, 5>{0, 0, 0, 0, 0};
    return false;
  }

  const auto& list = str.split(",");

  if (list.size() != 5) {
    v5f = std::array<float, 5>{0, 0, 0, 0, 0};
    return false;
  }

  bool ok = false;

  for (int i = 0; i < list.size(); ++i) {
    auto s = list[i].trimmed();

    if (s.isEmpty()) {
      v5f[i] = 0;
    } else {
      v5f[i] = s.toFloat(&ok);

      if (!ok) {
        v5f = std::array<float, 5>{0, 0, 0, 0, 0};
        return false;
      }
    }
  }

  return true;
}

ProjectionDialog::ProjectionDialog(QWidget* parent) : QDialog(parent), ui(new Ui::ProjectionDialog) {
  setWindowFlags(Qt::Dialog | Qt::WindowCloseButtonHint);

  ui->setupUi(this);

  this->adjustSize();
  this->setFixedSize(this->size());

  {
    // QDoubleValidator *img_width_validator = new QDoubleValidator(-1e10, 1e10, 6, ui->lineEdit_img_width);
    // img_width_validator->setNotation(QDoubleValidator::StandardNotation);
    // ui->lineEdit_img_width->setValidator(img_width_validator);

    // QDoubleValidator *img_height_validator = new QDoubleValidator(-1e10, 1e10, 6, ui->lineEdit_img_height);
    // img_height_validator->setNotation(QDoubleValidator::StandardNotation);
    // ui->lineEdit_img_height->setValidator(img_height_validator);

    // QRegularExpression in_mat_regex(R"(^([0-9]*\.?[0-9]+)(,([0-9]*\.?[0-9]+)){0,9}$)");
    // QValidator *in_mat_validator = new QRegularExpressionValidator(in_mat_regex, ui->lineEdit_in_mat);
    // ui->lineEdit_in_mat->setValidator(in_mat_validator);

    // QRegularExpression ext_tvec_regex(R"(^([0-9]*\.?[0-9]+)(,([0-9]*\.?[0-9]+)){0,3}$)");
    // QValidator *ext_tvec_validator = new QRegularExpressionValidator(ext_tvec_regex, ui->lineEdit_ext_tvec);
    // ui->lineEdit_ext_tvec->setValidator(ext_tvec_validator);

    // QRegularExpression ext_rvec_regex(R"(^([0-9]*\.?[0-9]+)(,([0-9]*\.?[0-9]+)){0,3}$)");
    // QValidator *ext_rvec_validator = new QRegularExpressionValidator(ext_rvec_regex, ui->lineEdit_ext_rvec);
    // ui->lineEdit_ext_rvec->setValidator(ext_rvec_validator);

    // QRegularExpression distortion_mat_regex(R"(^([0-9]*\.?[0-9]+)(,([0-9]*\.?[0-9]+)){0,5}$)");
    // QValidator *distortion_mat_validator = new QRegularExpressionValidator(distortion_mat_regex,
    // ui->lineEdit_distortion_mat); ui->lineEdit_distortion_mat->setValidator(distortion_mat_validator);
  }

  QSettings settings(QStandardPaths::writableLocation(QStandardPaths::CacheLocation) + "/settings.ini",
                     QSettings::IniFormat);

  settings.beginGroup("ProjectionDialog");

  ui->lineEdit_img_width->setText(settings.value("img_width").toString());
  ui->lineEdit_img_height->setText(settings.value("img_height").toString());
  ui->lineEdit_in_mat->setText(settings.value("in_mat").toString());
  ui->lineEdit_ext_tvec->setText(settings.value("ext_tvec").toString());
  ui->lineEdit_ext_rvec->setText(settings.value("ext_rvec").toString());
  ui->lineEdit_distortion_mat->setText(settings.value("distortion_mat").toString());
  ui->checkBox_distortion_mat->setChecked(settings.value("distortion_mat_enable", false).toBool());

  ui->lineEdit_distortion_mat->setEnabled(ui->checkBox_distortion_mat->isChecked());

  settings.endGroup();
}

ProjectionDialog::~ProjectionDialog() {
  QSettings settings(QStandardPaths::writableLocation(QStandardPaths::CacheLocation) + "/settings.ini",
                     QSettings::IniFormat);

  settings.beginGroup("ProjectionDialog");

  settings.setValue("img_width", ui->lineEdit_img_width->text());
  settings.setValue("img_height", ui->lineEdit_img_height->text());
  settings.setValue("in_mat", ui->lineEdit_in_mat->text());
  settings.setValue("ext_tvec", ui->lineEdit_ext_tvec->text());
  settings.setValue("ext_rvec", ui->lineEdit_ext_rvec->text());
  settings.setValue("distortion_mat", ui->lineEdit_distortion_mat->text());
  settings.setValue("distortion_mat_enable", ui->checkBox_distortion_mat->isChecked());

  settings.endGroup();
  settings.sync();

  delete ui;
}

void ProjectionDialog::invalid_args(int step) {
  QMessageBox::warning(this, tr("Warning"), tr("Invalid args (step:%1)").arg(QString::number(step)));
}

ProjectionDialog::Params ProjectionDialog::process() {
  if (this->exec() == QDialog::Accepted) {
    return params_;
  }

  return Params();
}

void ProjectionDialog::on_toolButton_inversion_clicked(bool checked) {
  if (checked) {
    ui->toolButton_inversion->setIcon(QIcon(":/resource/change_red.png"));
  } else {
    ui->toolButton_inversion->setIcon(QIcon(":/resource/change.png"));
  }
}

void ProjectionDialog::on_checkBox_distortion_mat_clicked(bool checked) {
  ui->lineEdit_distortion_mat->setEnabled(checked);
}

void ProjectionDialog::on_pushButton_ok_clicked() {
  bool ok = false;

  params_.is_valid = false;

  params_.img_width = ui->lineEdit_img_width->text().toFloat(&ok);

  if (!ok || !std::isfinite(params_.img_width) || params_.img_width <= 0) {
    invalid_args(1);
    return;
  }

  params_.img_height = ui->lineEdit_img_height->text().toFloat(&ok);

  if (!ok || !std::isfinite(params_.img_height) || params_.img_height <= 0) {
    invalid_args(2);
    return;
  }

  ok = convert_m3f(ui->lineEdit_in_mat->text(), params_.in_mat);

  if (!ok || !params_.in_mat.allFinite() || std::abs(params_.in_mat(0, 0)) <= 1e-6F ||
      std::abs(params_.in_mat(1, 1)) <= 1e-6F) {
    invalid_args(3);
    return;
  }

  ok = convert_v3f(ui->lineEdit_ext_tvec->text(), params_.ext_tvec);

  if (!ok || !params_.ext_tvec.allFinite()) {
    invalid_args(4);
    return;
  }

  ok = convert_v3f(ui->lineEdit_ext_rvec->text(), params_.ext_rvec);

  if (!ok || !params_.ext_rvec.allFinite()) {
    invalid_args(5);
    return;
  }

  params_.enable_distortion_mat = ui->checkBox_distortion_mat->isChecked();

  if (params_.enable_distortion_mat) {
    ok = convert_v5f(ui->lineEdit_distortion_mat->text(), params_.distortion_mat);

    if (!ok || std::any_of(params_.distortion_mat.begin(), params_.distortion_mat.end(),
                           [](float value) { return !std::isfinite(value); })) {
      invalid_args(6);
      return;
    }
  } else {
    params_.distortion_mat = std::array<float, 5>{0, 0, 0, 0, 0};
  }

  params_.is_valid = true;

  params_.point_size = ui->doubleSpinBox_point->value();
  params_.color_percent = ui->doubleSpinBox_percent->value() / 100.0;
  params_.inversion = ui->toolButton_inversion->isChecked();

  this->accept();
}

void ProjectionDialog::on_pushButton_close_clicked() { this->close(); }

// NOLINTEND
