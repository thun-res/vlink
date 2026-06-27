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

#pragma once

#ifdef _WIN32
#pragma warning(push)
#pragma warning(disable : 4127)
#endif

#include <Eigen/Dense>

#ifdef _WIN32
#pragma warning(pop)
#endif

#include <QDialog>
#include <array>

namespace Ui {
class ProjectionDialog;
}

class ProjectionDialog : public QDialog {
  Q_OBJECT

 public:
  struct Params {
    bool is_valid{false};
    float img_width{0};
    float img_height{0};
    Eigen::Matrix3Xf in_mat;
    std::array<float, 5> distortion_mat{0, 0, 0, 0, 0};
    Eigen::Vector3f ext_rvec;
    Eigen::Vector3f ext_tvec;
    bool enable_distortion_mat{false};

    float point_size{2};
    float color_percent{0.5};
    bool inversion{false};
  };

  explicit ProjectionDialog(QWidget* parent = nullptr);

  ~ProjectionDialog();

  Params process();

  void invalid_args(int step);

 private slots:
  void on_toolButton_inversion_clicked(bool checked);

  void on_checkBox_distortion_mat_clicked(bool checked);

  void on_pushButton_ok_clicked();

  void on_pushButton_close_clicked();

 private:
  Ui::ProjectionDialog* ui;
  Params params_;
};

// NOLINTEND
