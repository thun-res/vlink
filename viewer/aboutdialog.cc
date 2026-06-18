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

#include "./aboutdialog.h"

#include <vlink/version.h>

#include <QApplication>
#include <QDesktopServices>

#include "./mainwindow.h"
#include "./ui_aboutdialog.h"

AboutDialog::AboutDialog(QWidget* parent) : QDialog(parent), ui(new Ui::AboutDialog) {
  setWindowFlags(Qt::Dialog | Qt::WindowCloseButtonHint);

  ui->setupUi(this);

  window_ = MainWindow::get_instance();

  ui->label_rtversion2->setText(QStringLiteral(VLINK_VERSION));

  ui->label_platform2->setText(QApplication::platformName());

#ifdef VLINK_ENABLE_VIEWER_FFMPEG
  ui->label_ffmpeg2->setText(tr("Yes"));
#else
  ui->label_ffmpeg2->setText(tr("No"));
#endif

#ifdef VLINK_ENABLE_VIEWER_OSG
  ui->label_osg2->setText(tr("Yes"));
#else
  ui->label_osg2->setText(tr("No"));
#endif

#ifdef VLINK_ENABLE_EXPRTK
  ui->label_exprtk2->setText(tr("Yes"));
#else
  ui->label_exprtk2->setText(tr("No"));
#endif

  ui->label_stamp2->setText(QStringLiteral(VLINK_VERSION_TIMESTAMP));
  ui->label_tag2->setText(QStringLiteral(VLINK_VERSION_TAG));
  ui->label_commit2->setText(QStringLiteral(VLINK_VERSION_COMMIT_ID));
  ui->label_copyright2->setText(QStringLiteral("Thun Lu"));

  ui->label_contact2->setText(tr("Thun Lu <thun.lu@zohomail.cn>"));
}

AboutDialog::~AboutDialog() { delete ui; }

void AboutDialog::on_pushButton_open_clicked() {
#ifdef __linux__
  std::string lib_env = vlink::Utils::get_env("LD_LIBRARY_PATH");
  vlink::Utils::unset_env("LD_LIBRARY_PATH");
#endif

  QDesktopServices::openUrl(QUrl::fromLocalFile(QApplication::applicationDirPath()));

#ifdef __linux__

  if (!lib_env.empty()) {
    vlink::Utils::set_env("LD_LIBRARY_PATH", lib_env);
  }
#endif
}

void AboutDialog::on_pushButton_ok_clicked() { this->close(); }

void AboutDialog::on_label_vlink_linkActivated(const QString& link) { MainWindow::open_url(link); }

void AboutDialog::on_label_github_linkActivated(const QString& link) { MainWindow::open_url(link); }

// NOLINTEND
