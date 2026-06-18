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

#include <vlink/base/utils.h>

#include <QApplication>
#include <QFontDatabase>
#include <QMessageBox>
#include <QSettings>
#include <QStyleFactory>
#include <QSurfaceFormat>

#include "./playerwindow.h"

int main(int argc, char* argv[]) {
#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
  QCoreApplication::setAttribute(Qt::AA_EnableHighDpiScaling, true);
  QCoreApplication::setAttribute(Qt::AA_UseHighDpiPixmaps, true);
#if QT_VERSION >= QT_VERSION_CHECK(5, 15, 0)
  QApplication::setHighDpiScaleFactorRoundingPolicy(Qt::HighDpiScaleFactorRoundingPolicy::PassThrough);
#endif
#endif

  QApplication app(argc, argv);
  app.addLibraryPath(app.applicationDirPath());

  app.setStyle(QStyleFactory::create("fusion"));

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

  // vlink::Logger::set_console_level(vlink::Logger::kOff);
  // vlink::Logger::set_file_level(vlink::Logger::kOff);

  // init
  vlink::Logger::init("vlink-player");

  // env
  vlink::Utils::unset_env("VLINK_BAG_PATH");
  // vlink::Utils::set_env("VLINK_DISCOVER_DISABLE", "1");

  QString bag_path;

  if (app.arguments().size() == 2) {
    bag_path = app.arguments().at(1);
  } else if (app.arguments().size() > 2) {
    bag_path = app.arguments().at(1);

    // for (int i = 2; i < app.arguments().size(); ++i) {
    //   bag_path.append(" ");
    //   bag_path.append(app.arguments().at(i));
    // }
  }

#if defined(Q_OS_WIN)
  bag_path.replace("\\", "/");
#endif

  auto window = std::make_shared<PlayerWindow>(bag_path);

  if (window->has_error()) {
    return 1;
  }

  vlink::Utils::register_terminate_signal([&window](int) {
    if (window) {
      window->close();
    }
  });

  window->show();

  int ret = app.exec();

  return ret;
}

// NOLINTEND
