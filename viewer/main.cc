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

#include <google/protobuf/stubs/common.h>
#include <vlink/base/utils.h>

#include <QApplication>
#include <QElapsedTimer>
#include <QFontDatabase>
#include <QMessageBox>
#include <QSettings>
#include <QStyleFactory>
#include <QSurfaceFormat>

#include "./mainwindow.h"
#include "./settingsdialog.h"

#if GOOGLE_PROTOBUF_VERSION < 3004000
#error "Protobuf version must >= 3.4.0"
#endif

int main(int argc, char* argv[]) {
#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
  QCoreApplication::setAttribute(Qt::AA_EnableHighDpiScaling, true);
  QCoreApplication::setAttribute(Qt::AA_UseHighDpiPixmaps, true);
#if QT_VERSION >= QT_VERSION_CHECK(5, 15, 0)
  QApplication::setHighDpiScaleFactorRoundingPolicy(Qt::HighDpiScaleFactorRoundingPolicy::PassThrough);
#endif
#endif

#ifdef VLINK_ENABLE_VIEWER_OSG
  QSurfaceFormat format = QSurfaceFormat::defaultFormat();
  format.setRenderableType(QSurfaceFormat::OpenGL);
  format.setProfile(QSurfaceFormat::CompatibilityProfile);
  format.setSamples(4);
  QSurfaceFormat::setDefaultFormat(format);
#endif

  QApplication app(argc, argv);
  app.addLibraryPath(app.applicationDirPath());

  app.setStyle(QStyleFactory::create("fusion"));

  qRegisterMetaType<QElapsedTimer>("QElapsedTimer");

  QFontDatabase::addApplicationFont(":/resource/notomono.ttf");

#if defined(Q_OS_WIN)
  QFont font = qApp->font();
  font.setFamily("Microsoft YaHei");
  app.setFont(font);
#elif defined(Q_OS_APPLE)
  QFont font = qApp->font();
  font.setFamily("Helvetica Neue");
  app.setFont(font);
#elif defined(Q_OS_LINUX)
  QFont font = QFontDatabase::systemFont(QFontDatabase::GeneralFont);
  app.setFont(font);
#endif

  //   {
  // #ifdef Q_OS_WIN
  //     QSettings settings("HKEY_CURRENT_USER\\Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
  //                        QSettings::NativeFormat);
  //
  //     if (settings.value("AppsUseLightTheme") == 0) {
  //       qApp->setStyle(QStyleFactory::create("Fusion"));
  //       QPalette darkPalette;
  //       QColor darkColor = QColor(45, 45, 45);
  //       QColor disabledColor = QColor(127, 127, 127);
  //
  //       darkPalette.setColor(QPalette::Window, darkColor);
  //       darkPalette.setColor(QPalette::WindowText, Qt::white);
  //       darkPalette.setColor(QPalette::Base, QColor(18, 18, 18));
  //       darkPalette.setColor(QPalette::AlternateBase, darkColor);
  //       darkPalette.setColor(QPalette::ToolTipBase, Qt::white);
  //       darkPalette.setColor(QPalette::ToolTipText, Qt::white);
  //       darkPalette.setColor(QPalette::Text, Qt::white);
  //       darkPalette.setColor(QPalette::Disabled, QPalette::Text, disabledColor);
  //       darkPalette.setColor(QPalette::Button, darkColor);
  //       darkPalette.setColor(QPalette::ButtonText, Qt::white);
  //       darkPalette.setColor(QPalette::Disabled, QPalette::ButtonText, disabledColor);
  //       darkPalette.setColor(QPalette::BrightText, Qt::red);
  //       darkPalette.setColor(QPalette::Link, QColor(42, 130, 218));

  //       darkPalette.setColor(QPalette::Highlight, QColor(42, 130, 218));
  //       darkPalette.setColor(QPalette::HighlightedText, Qt::black);
  //       darkPalette.setColor(QPalette::Disabled, QPalette::HighlightedText, disabledColor);

  //       qApp->setPalette(darkPalette);

  //       qApp->setStyleSheet("QToolTip { color: #ffffff; background-color: #2a82da; border: 1px solid white; }");
  //     }
  // #endif
  //   }

  // settings

  if (!SettingsDialog::do_select()) {
    return 0;
  }

  std::string process_name;

  if (SettingsDialog::get_mode() == SettingsDialog::kController) {
    process_name = "vlink-viewer-" + std::to_string(SettingsDialog::get_domain_id()) + "-controller";

    if (!vlink::Utils::check_singleton(process_name)) {
      QMessageBox::warning(nullptr, QObject::tr("Warning"),
                           QObject::tr("Only one controller with the same domain id can be started."));
      return 1;
    }
  } else {
    process_name = "vlink-viewer-" + std::to_string(SettingsDialog::get_domain_id()) + "-listener";
  }

  // vlink::Logger::set_console_level(vlink::Logger::kOff);
  // vlink::Logger::set_file_level(vlink::Logger::kOff);

  // init
  vlink::Logger::init(process_name);

  // env
  vlink::Utils::unset_env("VLINK_BAG_PATH");
  // vlink::Utils::set_env("VLINK_DISCOVER_DISABLE", "1");

  MainWindow::create_instance();

  auto window = MainWindow::get_instance();

  vlink::Utils::register_terminate_signal([&window](int) { window->close(); });

  window->show();

  int ret = app.exec();

  MainWindow::destroy_instance();

  return ret;
}

// NOLINTEND
