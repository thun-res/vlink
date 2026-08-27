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

#include "./playerwindow.h"

#include <vlink/base/helpers.h>
#ifdef VLINK_SUPPORT_SHM
#include <vlink/modules/shm_conf.h>
#endif

#include <QCloseEvent>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFileDialog>
#include <QHideEvent>
#include <QMessageBox>
#include <QMimeData>
#include <QMouseEvent>
#include <QMoveEvent>
#include <QResizeEvent>
#include <QScreen>
#include <QSettings>
#include <QShortcut>
#include <QShowEvent>
#include <QStandardPaths>
#include <QTimer>
#include <limits>
#include <sstream>

#include "./infodialog.h"
#include "./pointdialog.h"
#include "./ui_playerwindow.h"
#include "./waitwidget.h"

#define USE_LOCK_PROXY_DESTROY 0

PlayerWindow::PlayerWindow(const QString& bag_path, QWidget* parent) : QMainWindow(parent), ui(new Ui::PlayerWindow) {
  setWindowFlags(Qt::Dialog | Qt::WindowMinimizeButtonHint | Qt::WindowCloseButtonHint | Qt::WindowStaysOnTopHint);

  ui->setupUi(this);

#if QT_VERSION >= QT_VERSION_CHECK(6, 7, 0)
  {
    QColor highlight_color = QApplication::palette().color(QPalette::Active, QPalette::Highlight);
    QColor accent_color = QApplication::palette().color(QPalette::Active, QPalette::Accent);

    QPalette pal = ui->horizontalSlider_progress->palette();

    pal.setColor(QPalette::Inactive, QPalette::Highlight, highlight_color);
    pal.setColor(QPalette::Inactive, QPalette::Accent, accent_color);

    ui->horizontalSlider_progress->setPalette(pal);
  }
#endif

  {
    QFont font = ui->label_time->font();

#if defined(Q_OS_LINUX)
    font.setFamily("Noto Sans");
#endif

    font.setPixelSize(12);

    ui->label_bytes->setFont(font);

    font.setPixelSize(16);

    ui->label_time->setFont(font);
  }

  this->setAcceptDrops(true);

  // ui->toolButton_viewer->setVisible(false);

  update_windows_title(false);

  this->adjustSize();

  this->setMinimumWidth(width() + 20);
  this->setMinimumHeight(height());
  this->setMaximumHeight(height() + 10);

  fixed_height_ = height();

  wait_widget_ = new WaitWidget(this->centralWidget());

  info_dialog_ = new InfoDialog(this);

  point_dialog_ = new PointDialog(this);

#ifdef _WIN32
  proxy_process_.setProgram(qApp->applicationDirPath() + "/vlink-proxy.exe");
  viewer_process_.setProgram(qApp->applicationDirPath() + "/vlink-viewer.exe");
  analyzer_process_.setProgram(qApp->applicationDirPath() + "/vlink-analyzer.exe");
#else
  proxy_process_.setProgram(qApp->applicationDirPath() + "/vlink-proxy");
  viewer_process_.setProgram(qApp->applicationDirPath() + "/vlink-viewer");
  analyzer_process_.setProgram(qApp->applicationDirPath() + "/vlink-analyzer");
#endif

  main_timer_ = new QTimer(this);
  main_timer_->setInterval(50);

  play_timer_ = new QTimer(this);
  play_timer_->setInterval(2000);

  connect(point_dialog_, &PointDialog::jump_point, this, [this](int64_t timestamp) {
    if (!player_) {
      return;
    }

    if (status_ != vlink::BagReader::kPlaying && status_ != vlink::BagReader::kPaused) {
      update_timestamp(timestamp);
    } else {
      double rate = ui->doubleSpinBox_rate->value() <= 0 ? 1 : ui->doubleSpinBox_rate->value();

      int times = ui->checkBox_loop->isChecked() ? 0 : 1;

      player_->jump(timestamp, rate, times, false);

      wait_widget_->start_wait();
    }
  });

  connect(main_timer_, &QTimer::timeout, this, [this]() {
    if (ui->horizontalSlider_progress->isSliderDown()) {
      return;
    }

    if (status_ != vlink::BagReader::kPlaying && status_ != vlink::BagReader::kPaused) {
      return;
    }

    if VUNLIKELY (!player_) {
      return;
    }

    update_timestamp(player_->get_timestamp());
  });

  connect(play_timer_, &QTimer::timeout, this, [this]() {
    if (!player_) {
      return;
    }

    play_timer_->stop();

    if (!has_played_ && player_->get_status() == vlink::BagReader::kStopped) {
      ui->toolButton_play->setEnabled(false);
      ui->toolButton_pause->setEnabled(false);
      ui->toolButton_close->setEnabled(false);
      ui->toolButton_select->setEnabled(false);

      wait_widget_->stop_wait();

      QMessageBox::critical(this, tr("Proxy Error"),
                            tr("The handle is blocked, please make sure the proxy is started."));
    }

    has_played_ = true;
  });

  QShortcut* shortcut_esc = new QShortcut(QKeySequence(Qt::Key_Escape), this);
  QShortcut* shortcut_space = new QShortcut(QKeySequence(Qt::Key_Space), this);
  QShortcut* shortcut_left = new QShortcut(QKeySequence(Qt::Key_Left), this);
  QShortcut* shortcut_right = new QShortcut(QKeySequence(Qt::Key_Right), this);
  QShortcut* shortcut_up = new QShortcut(QKeySequence(Qt::Key_Up), this);
  QShortcut* shortcut_down = new QShortcut(QKeySequence(Qt::Key_Down), this);
  QShortcut* shortcut_q = new QShortcut(QKeySequence(Qt::Key_Q), this);
  QShortcut* shortcut_p = new QShortcut(QKeySequence(Qt::Key_P), this);
  QShortcut* shortcut_s = new QShortcut(QKeySequence(Qt::Key_S), this);

  connect(shortcut_esc, &QShortcut::activated, this, [this]() {
    if (wait_widget_->is_working()) {
      return;
    }

    if (status_ != vlink::BagReader::kPlaying && status_ != vlink::BagReader::kPaused) {
      return;
    }

    on_toolButton_play_clicked();
  });

  connect(shortcut_space, &QShortcut::activated, this, &PlayerWindow::on_toolButton_pause_clicked);

  connect(shortcut_left, &QShortcut::activated, this, [this]() {
    if (wait_widget_->is_working()) {
      return;
    }

    if (status_ != vlink::BagReader::kPlaying && status_ != vlink::BagReader::kPaused) {
      return;
    }

    double rate = ui->doubleSpinBox_rate->value() <= 0 ? 1 : ui->doubleSpinBox_rate->value();

    int times = ui->checkBox_loop->isChecked() ? 0 : 1;

    player_->jump(player_->get_timestamp() - 1000, rate, times, false);
  });

  connect(shortcut_right, &QShortcut::activated, this, [this]() {
    if (wait_widget_->is_working()) {
      return;
    }

    if (status_ != vlink::BagReader::kPlaying && status_ != vlink::BagReader::kPaused) {
      return;
    }

    double rate = ui->doubleSpinBox_rate->value() <= 0 ? 1 : ui->doubleSpinBox_rate->value();

    int times = ui->checkBox_loop->isChecked() ? 0 : 1;

    player_->jump(player_->get_timestamp() + 1000, rate, times, false);
  });

  connect(shortcut_up, &QShortcut::activated, this, [this]() {
    if (wait_widget_->is_working()) {
      return;
    }

    if (status_ != vlink::BagReader::kPlaying && status_ != vlink::BagReader::kPaused) {
      return;
    }

    double rate = ui->doubleSpinBox_rate->value() <= 0 ? 1 : ui->doubleSpinBox_rate->value();

    int times = ui->checkBox_loop->isChecked() ? 0 : 1;

    player_->jump(player_->get_timestamp() - 5000, rate, times, false);
  });

  connect(shortcut_down, &QShortcut::activated, this, [this]() {
    if (wait_widget_->is_working()) {
      return;
    }

    if (status_ != vlink::BagReader::kPlaying && status_ != vlink::BagReader::kPaused) {
      return;
    }

    double rate = ui->doubleSpinBox_rate->value() <= 0 ? 1 : ui->doubleSpinBox_rate->value();

    int times = ui->checkBox_loop->isChecked() ? 0 : 1;

    player_->jump(player_->get_timestamp() + 5000, rate, times, false);
  });

  connect(shortcut_q, &QShortcut::activated, this, [this]() {
    if (wait_widget_->is_working()) {
      return;
    }

    if (status_ != vlink::BagReader::kPlaying && status_ != vlink::BagReader::kPaused) {
      return;
    }

    on_toolButton_play_clicked();
  });

  connect(shortcut_p, &QShortcut::activated, this, [this]() {
    if (wait_widget_->is_working()) {
      return;
    }

    if (status_ != vlink::BagReader::kPaused) {
      return;
    }

    pause_to_next_flag_ = true;

    player_->pause_to_next();
  });

  connect(shortcut_s, &QShortcut::activated, this, [this]() {
    if (wait_widget_->is_working()) {
      return;
    }

    if (status_ == vlink::BagReader::kPlaying || status_ == vlink::BagReader::kPaused) {
      return;
    }

    on_toolButton_play_clicked();
  });

  connect(&proxy_process_, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
    (void)error;

    if (quit_flag_) {
      return;
    }

    if (!proxy_user_kill_) {
      QString error_str = proxy_process_.errorString();

      if (!error_str.isEmpty()) {
        QMessageBox::critical(this, tr("Proxy Error"), error_str);
      }
    }

    ui->toolButton_proxy->setEnabled(true);
    ui->toolButton_proxy->setChecked(false);
    update_proxy_icon();

    if (!player_) {
      ui->lineEdit_proxy->setEnabled(true);
      return;
    }

    if (!player_ || status_ == vlink::BagReader::kStopped || status_ < 0) {
      ui->lineEdit_proxy->setEnabled(true);
    }
  });

  connect(&proxy_process_, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), this,
          [this](int exit_code, QProcess::ExitStatus exit_status) {
            if (quit_flag_) {
              return;
            }

            if (!proxy_user_kill_ && (exit_code != 0 || exit_status != QProcess::NormalExit)) {
              VLOG_W("Proxy exit code: ", exit_code, ".");

              QString error_str = proxy_process_.readAllStandardError();

              if (!error_str.isEmpty()) {
                QMessageBox::critical(this, tr("Proxy Error"), error_str);
              }
            }

            ui->toolButton_proxy->setEnabled(true);
            ui->toolButton_proxy->setChecked(false);
            update_proxy_icon();

            if (!player_ || status_ == vlink::BagReader::kStopped || status_ < 0) {
              ui->lineEdit_proxy->setEnabled(true);
            }
          });

  connect(&analyzer_process_, &QProcess::readyReadStandardOutput, this, [this]() {
    if (quit_flag_) {
      return;
    }

    consume_analyzer_timestamp_bytes(analyzer_process_.readAllStandardOutput());
  });

  update_status();

  {
    QSettings settings(QStandardPaths::writableLocation(QStandardPaths::CacheLocation) + "/settings.ini",
                       QSettings::IniFormat);
    settings.beginGroup("PlayerWindow");

    file_path_ = settings.value("bag_path", "").toString();

    remap_path_ = settings.value("remap_path", "").toString();

    ui->checkBox_loop->setChecked(settings.value("loop_mode", true).toBool());

    ui->checkBox_native->setChecked(settings.value("native_mode", true).toBool());

    ui->lineEdit_proxy->setText(settings.value("proxy_args", "-c -d 100 -n").toString());

    auto geometry = settings.value("geometry", this->geometry()).toByteArray();

    settings.endGroup();

    restoreGeometry(geometry);
  }

  total_elapsed_timer.start();

  if (!bag_path.isEmpty()) {
    load_bag(bag_path);
  }

  instance_ = this;
}

PlayerWindow::~PlayerWindow() {
  quit_flag_ = true;

  if (proxy_process_.state() != QProcess::NotRunning) {
    kill_proxy_clients();
  }

  if (viewer_process_.state() != QProcess::NotRunning) {
    viewer_process_.terminate();
    viewer_process_.waitForFinished(500);
    viewer_process_.kill();
    viewer_process_.waitForFinished(1000);
  }

  if (analyzer_process_.state() != QProcess::NotRunning) {
    analyzer_process_.terminate();
    analyzer_process_.waitForFinished(500);
    analyzer_process_.kill();
    analyzer_process_.waitForFinished(1000);
  }

  {
    QSettings settings(QStandardPaths::writableLocation(QStandardPaths::CacheLocation) + "/settings.ini",
                       QSettings::IniFormat);
    settings.beginGroup("PlayerWindow");
    settings.setValue("geometry", saveGeometry());
    settings.endGroup();
    settings.sync();
  }

  if (has_block_shm_) {
#if defined(VLINK_SUPPORT_SHM)
    std::exit(0);
    return;
#endif
  }

  if (player_) {
    if (player_->is_running()) {
      player_->stop();
      player_->quit();
      player_->wait_for_quit(2000);
    }
    player_.reset();
  }

  {
    std::lock_guard lock(urls_mtx_);
    pub_urls_map_.clear();
  }

  if (proxy_process_.state() != QProcess::NotRunning) {
#if defined(VLINK_SUPPORT_SHM)

    if (vlink::ShmConf::has_runtime_inited()) {
      vlink::ShmConf::deinit_runtime();
      proxy_process_.waitForFinished(100);
    }
#endif

    proxy_user_kill_ = true;
    proxy_process_.terminate();
    proxy_process_.waitForFinished(1000);
    proxy_process_.kill();
    proxy_process_.waitForFinished(1000);
  }

  delete ui;
}

PlayerWindow* PlayerWindow::get_instance() { return instance_; }

bool PlayerWindow::has_error() const { return has_error_; }

int64_t PlayerWindow::get_current_timestamp() const {
  if (player_ && (status_ == vlink::BagReader::kPlaying || status_ == vlink::BagReader::kPaused)) {
    return player_->get_timestamp();
  }

  return curent_timestamp_;
}

void PlayerWindow::showEvent(QShowEvent* event) { QMainWindow::showEvent(event); }

void PlayerWindow::hideEvent(QHideEvent* event) { QMainWindow::hideEvent(event); }

void PlayerWindow::closeEvent(QCloseEvent* event) {
  if (proxy_process_.state() == QProcess::NotRunning) {
    QMainWindow::closeEvent(event);
    return;
  }

  if (QMessageBox::question(this, tr("Close"), tr("Proxy is running, are you sure you want to close?")) ==
      QMessageBox::Yes) {
    event->accept();
  } else {
    event->ignore();
  }
}

void PlayerWindow::dragEnterEvent(QDragEnterEvent* event) {
  if (wait_widget_->is_working()) {
    event->ignore();
    return;
  }

  if (!ui->toolButton_select->isEnabled()) {
    event->ignore();
    return;
  }

  if (event->mimeData()->hasUrls()) {
    auto urls = event->mimeData()->urls();

    if (urls.size() != 1) {
      event->ignore();
      return;
    }

    QString file_path = urls.first().toLocalFile();
    QFileInfo file_info(file_path);

    if (file_info.suffix() != "vdb" && file_info.suffix() != "vdbx" && file_info.suffix() != "vcap" &&
        file_info.suffix() != "vcapx") {
      event->ignore();
      return;
    }

    event->acceptProposedAction();
  } else {
    event->ignore();
  }
}

void PlayerWindow::dropEvent(QDropEvent* event) {
  if (wait_widget_->is_working()) {
    event->ignore();
    return;
  }

  if (!ui->toolButton_select->isEnabled()) {
    event->ignore();
    return;
  }

  if (event->mimeData()->hasUrls()) {
    auto urls = event->mimeData()->urls();

    if (urls.size() != 1) {
      event->ignore();
      return;
    }

    QString file_path = urls.first().toLocalFile();
    QFileInfo file_info(file_path);

    if (file_info.suffix() != "vdb" && file_info.suffix() != "vdbx" && file_info.suffix() != "vcap" &&
        file_info.suffix() != "vcapx") {
      event->ignore();
      return;
    }

    load_bag(file_info.absoluteFilePath());

    event->acceptProposedAction();
  } else {
    event->ignore();
  }
}

void PlayerWindow::resizeEvent(QResizeEvent* event) {
  QMainWindow::resizeEvent(event);
  wait_widget_->adjust_geometry();
  ui->checkBox_black->setMinimumSize(ui->horizontalLayout_tool->minimumSize());
}

void PlayerWindow::moveEvent(QMoveEvent* event) {
  QMainWindow::moveEvent(event);
  wait_widget_->adjust_geometry();
}

void PlayerWindow::mousePressEvent(QMouseEvent* event) {
  //
  QMainWindow::mousePressEvent(event);
}

void PlayerWindow::mouseReleaseEvent(QMouseEvent* event) {
  //
  QMainWindow::mouseReleaseEvent(event);
}

void PlayerWindow::mouseMoveEvent(QMouseEvent* event) {
  //
  QMainWindow::mouseMoveEvent(event);
}

void PlayerWindow::on_toolButton_select_clicked() {
  if (wait_widget_->is_working()) {
    return;
  }

  QFileInfo file_info(file_path_);

  if (file_info.isFile() && file_info.exists()) {
    file_path_ = file_info.absoluteFilePath();
  } else {
    file_path_ = qApp->applicationDirPath();
  }

  QFileDialog dialog(this, tr("Select bag file"), file_path_, "Bag files (*.vdb *.vdbx *.vcap *.vcapx)");

#if QT_VERSION < QT_VERSION_CHECK(5, 15, 0)
  dialog.setOption(QFileDialog::DontUseNativeDialog, true);
#endif

  dialog.setFileMode(QFileDialog::ExistingFile);
  dialog.setAcceptMode(QFileDialog::AcceptOpen);
  dialog.setDefaultSuffix("vdb");

  if (dialog.exec() != QDialog::Accepted) {
    return;
  }

  load_bag(dialog.selectedFiles().constFirst());
}

void PlayerWindow::on_toolButton_close_clicked() {
  wait_widget_->stop_wait();
  info_dialog_->close();
  point_dialog_->close();

  // if (viewer_process_.state() != QProcess::NotRunning) {
  //   viewer_process_.terminate();
  //   viewer_process_.waitForFinished(500);
  //   viewer_process_.kill();
  //   viewer_process_.waitForFinished(1000);
  // }

  if (analyzer_process_.state() != QProcess::NotRunning) {
    analyzer_process_.terminate();
    analyzer_process_.waitForFinished(500);
    analyzer_process_.kill();
    analyzer_process_.waitForFinished(1000);
  }

  if (player_) {
    if (player_->is_running()) {
      player_->stop();
      player_->quit();
      player_->wait_for_quit(2000);
    }

    player_.reset();
  }

  {
    std::lock_guard lock(urls_mtx_);
    pub_urls_map_.clear();
  }

  ui->lineEdit_path->clear();
  ui->lineEdit_filter->clear();
  ui->checkBox_black->setChecked(false);
  ui->doubleSpinBox_rate->setValue(1);
  ui->horizontalSlider_progress->setRange(0, 100);
  last_rate_ = 1;

  main_timer_->stop();
  play_timer_->stop();

  is_changed_ = false;
  bytes_size_ = 0;

  status_ = -1;

  update_status();
}

void PlayerWindow::on_toolButton_point_clicked() {
  if (wait_widget_->is_working()) {
    return;
  }

  // info_dialog_->close();

  if (!player_) {
    return;
  }

  point_dialog_->reload_file(ui->lineEdit_path->text());

  int64_t timeoffset = player_->get_info().timezone * 60 * 1000;

  switch (time_method_) {
    case TimeMethod::kTimeRel:
      point_dialog_->set_date_time(0);
      break;
    case TimeMethod::kTimeLocal:
      point_dialog_->set_date_time(date_timestamp_ + timeoffset);
      break;
    case TimeMethod::kTimeUtc:
      point_dialog_->set_date_time(date_timestamp_);
      break;
    default:
      break;
  }

  point_dialog_->show();

  QScreen* current_screen = qApp->screenAt(this->geometry().center());

  if (!current_screen) {
    return;
  }

  QRect frame_geometry = frameGeometry();
  QRect available_geometry = current_screen->availableGeometry();

  QPoint target_pos = frame_geometry.topRight();
  target_pos.setX(target_pos.x() + 10);
  target_pos.setY(frame_geometry.top());

  if (target_pos.x() + point_dialog_->width() > available_geometry.right()) {
    target_pos = frame_geometry.topLeft();
    target_pos.setX(target_pos.x() - point_dialog_->width() - 10);
  }

  if (target_pos.x() < available_geometry.left()) {
    target_pos = frame_geometry.topLeft();
    target_pos.setY(frame_geometry.top() - point_dialog_->height() - 10);
  }

  if (target_pos.y() < available_geometry.top()) {
    target_pos = frame_geometry.bottomLeft();
    target_pos.setY(frame_geometry.bottom() + 10);
  }

  if (target_pos.y() + point_dialog_->height() > available_geometry.bottom()) {
    target_pos.setY(available_geometry.bottom() - point_dialog_->height());
  }

  point_dialog_->move(target_pos);
}

void PlayerWindow::on_toolButton_info_clicked() {
  if (wait_widget_->is_working()) {
    return;
  }

  // point_dialog_->close();

  if (!player_) {
    return;
  }

  info_dialog_->show_information(player_->get_info());

  QScreen* current_screen = qApp->screenAt(this->geometry().center());

  if (!current_screen) {
    return;
  }

  QRect frame_geometry = frameGeometry();
  QRect available_geometry = current_screen->availableGeometry();

  QPoint target_pos = frame_geometry.topRight();
  target_pos.setX(target_pos.x() + 10);
  target_pos.setY(frame_geometry.top());

  if (target_pos.x() + info_dialog_->width() > available_geometry.right()) {
    target_pos = frame_geometry.topLeft();
    target_pos.setX(target_pos.x() - info_dialog_->width() - 10);
  }

  if (target_pos.x() < available_geometry.left()) {
    target_pos = frame_geometry.topLeft();
    target_pos.setY(frame_geometry.top() - info_dialog_->height() - 10);
  }

  if (target_pos.y() < available_geometry.top()) {
    target_pos = frame_geometry.bottomLeft();
    target_pos.setY(frame_geometry.bottom() + 10);
  }

  if (target_pos.y() + info_dialog_->height() > available_geometry.bottom()) {
    target_pos.setY(available_geometry.bottom() - info_dialog_->height());
  }

  info_dialog_->move(target_pos);
}

void PlayerWindow::on_toolButton_viewer_clicked() {
  if (wait_widget_->is_working()) {
    return;
  }

  if (viewer_process_.state() != QProcess::NotRunning) {
    return;
  }

  viewer_process_.start();
  viewer_process_.waitForStarted(1000);

  if (viewer_process_.error() == QProcess::FailedToStart) {
    QMessageBox::critical(this, tr("Viewer Error"), tr("Can not open."));
    return;
  }
}

void PlayerWindow::on_toolButton_cmd_clicked() {
#ifdef _WIN32
  QString script_path = qApp->applicationDirPath() + "/run_cmd.bat";

  if (!QFile::exists(script_path)) {
    return;
  }

  QProcess::startDetached(script_path);
#else
  QString script_path = qApp->applicationDirPath() + "/run_cmd.sh";

  if (!QFile::exists(script_path)) {
    return;
  }

#ifdef __APPLE__
  QString apple_script = QString(
                             "tell application \"Terminal\"\n"
                             "    activate\n"
                             "    do script \" printf '\\\\033]0;VLink CMD\\\\007'; clear; bash '%1'; exec bash\"\n"
                             "end tell")
                             .arg(script_path);

  QStringList args = {"-e", apple_script};
  QProcess::startDetached("/usr/bin/osascript", args);
#else

  QStringList terminal_candidates = {
      "gnome-terminal", "konsole", "xterm", "alacritty", "xfce4-terminal", "lxterminal",
  };

  QString terminal_program;

  for (const QString& candidate : terminal_candidates) {
    if (std::system(QString("%1 %2 > /dev/null 2>&1").arg("which", candidate).toLocal8Bit().data()) == 0) {
      terminal_program = candidate;
      break;
    }
  }

  if (terminal_program.isEmpty()) {
    return;
  }

  if (terminal_program == "gnome-terminal") {
    terminal_program += QString(" -- bash -c \"%1; exec bash\" &").arg(script_path);
  } else if (terminal_program == "konsole") {
    terminal_program += QString(" --noclose -e bash -c \"%1\" &").arg(script_path);
  } else if (terminal_program == "xterm") {
    terminal_program += QString(" -hold -e bash -c \"%1\" &").arg(script_path);
  } else if (terminal_program == "alacritty" || terminal_program == "xfce4-terminal" ||
             terminal_program == "lxterminal") {
    terminal_program += QString(" -e bash -c \"%1; exec bash\" &").arg(script_path);
  }

#ifdef __linux__
  std::string lib_env = vlink::Utils::get_env("LD_LIBRARY_PATH");
  vlink::Utils::unset_env("LD_LIBRARY_PATH");
#endif

  auto ret = std::system(terminal_program.toLocal8Bit().data());

  (void)ret;

#ifdef __linux__

  if (!lib_env.empty()) {
    vlink::Utils::set_env("LD_LIBRARY_PATH", lib_env);
  }
#endif
#endif
#endif
}

void PlayerWindow::on_toolButton_analyzer_clicked() {
  if (wait_widget_->is_working()) {
    return;
  }

  // info_dialog_->close();
  // point_dialog_->close();

  if (!player_) {
    return;
  }

  if (analyzer_process_.state() != QProcess::NotRunning) {
    return;
  }

  analyzer_timestamp_buffer_.clear();
  analyzer_process_.setArguments(QStringList() << ui->lineEdit_path->text() << "1");

  analyzer_process_.start();
  analyzer_process_.waitForStarted(1000);

  if (analyzer_process_.error() == QProcess::FailedToStart) {
    QMessageBox::critical(this, tr("Analyzer Error"), tr("Can not open."));
    return;
  }
}

void PlayerWindow::on_toolButton_play_clicked() {
  if (wait_widget_->is_working()) {
    return;
  }

  if (ui->doubleSpinBox_rate->value() <= 0) {
    ui->doubleSpinBox_rate->blockSignals(true);
    ui->doubleSpinBox_rate->setValue(1);
    ui->doubleSpinBox_rate->blockSignals(false);
    last_rate_ = 1;
  }

  if (!player_) {
    return;
  }

  if (player_->get_info().url_metas.empty()) {
    QMessageBox::warning(this, tr("Warning"), tr("Can't find any urls to play."));
    return;
  }

  if (status_ != vlink::BagReader::kStopped || status_ < 0) {
    player_->stop();
    main_timer_->stop();
    play_timer_->stop();

    return;
  }

#ifdef VLINK_SUPPORT_SHM

  if (check_has_shm()) {
    bool has_roudi_running = false;

    if (vlink::ShmConf::has_roudi_running()) {
      has_roudi_running = true;
    } else {
      std::this_thread::sleep_for(std::chrono::milliseconds(100));

      if (vlink::ShmConf::has_roudi_running()) {
        has_roudi_running = true;
      } else {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));

        has_roudi_running = vlink::ShmConf::has_roudi_running();
      }
    }

    if (!has_roudi_running) {
      if (QMessageBox::warning(this, tr("Warning"),
                               tr("The proxy does not seem to be running. Do you want to continue?"),
                               QMessageBox::StandardButtons(QMessageBox::Yes | QMessageBox::No)) != QMessageBox::Yes) {
        return;
      }
    }
  }
#endif

  bool is_black_mode = ui->checkBox_black->isChecked();
  bool is_native_mode = ui->checkBox_native->isChecked();

  std::string filter_str = ui->lineEdit_filter->text().toStdString();
  std::vector<std::string> filter_list;

  if (!filter_str.empty()) {
    filter_list = vlink::Helpers::split_any(filter_str);

    std::lock_guard lock(urls_mtx_);
    filter_urls_.clear();
  }

  play_timer_->stop();

  wait_widget_->start_wait();

  player_->post_task([is_black_mode, is_native_mode, filter_list, this]() {
    has_played_ = false;

    {
      std::lock_guard lock(urls_mtx_);
      pub_urls_map_.clear();
    }

    vlink::Utils::yield_cpu();

    QString error_log;

    {
      std::lock_guard remap_lock(remap_mtx_);

      std::string real_url;

      for (const auto& url_meta : player_->get_info().url_metas) {
        if (url_meta.url_type == "Method") {
          continue;
        }

        if (!filter_list.empty()) {
          bool skip = is_black_mode ? false : true;

          std::string left_str = url_meta.url;
          std::transform(left_str.begin(), left_str.end(), left_str.begin(),
                         [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
          for (const auto& f : filter_list) {
            if (f.empty()) {
              continue;
            }

            std::string right_str = f;
            std::transform(right_str.begin(), right_str.end(), right_str.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            if (left_str.find(right_str) != std::string::npos) {
              skip = is_black_mode ? true : false;
              break;
            }
          }

          if (skip) {
            continue;
          }
        }

        RawPubPtr ptr;

        real_url = url_meta.url;

        if (remap_.is_valid()) {
          real_url = remap_.convert(real_url);
        }

        if (vlink::Helpers::has_startwith(real_url, "shm://")) {
#if defined(VLINK_SUPPORT_SHM)
          if (!vlink::ShmConf::has_runtime_inited()) {
            has_block_shm_ = true;
            vlink::ShmConf::init_runtime();
            has_block_shm_ = false;
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
          }

          vlink::ShmConf::global_init();
#endif
        }

        try {
          ptr = std::make_shared<RawPub>(real_url, vlink::InitType::kWithoutInit);
        } catch (vlink::Exception::RuntimeError& e) {
          if (!error_log.isEmpty()) {
            error_log += "\n";
          }

          error_log += e.what();

          continue;
        }

        if (url_meta.url_type == "Field") {
          ptr->mark_as_setter();
        }

        ptr->set_ser_type(url_meta.ser_type, url_meta.schema_type);

        if (is_native_mode) {
          ptr->set_property("dds.ip", "127.0.0.1");
        }

        ptr->init();

        {
          std::lock_guard lock(urls_mtx_);
          pub_urls_map_.emplace(url_meta.url, std::move(ptr));
          filter_urls_.emplace(url_meta.url);
        }
      }
    }

    QMetaObject::invokeMethod(this, "ready_to_play", Qt::QueuedConnection, Q_ARG(QString, error_log));
  });

  play_timer_->start();
}

void PlayerWindow::on_toolButton_pause_clicked() {
  if (wait_widget_->is_working()) {
    return;
  }

  if (!player_) {
    return;
  }

  if (status_ == vlink::BagReader::kPlaying) {
    wait_widget_->start_wait();

    status_ = vlink::BagReader::kPaused;
    player_->pause();

    main_timer_->stop();
  } else if (status_ == vlink::BagReader::kPaused) {
    status_ = vlink::BagReader::kPlaying;
    player_->resume();

    main_timer_->start();
  }
}

void PlayerWindow::on_toolButton_time_clicked() {
  if (!player_) {
    return;
  }

  int64_t timeoffset = player_->get_info().timezone * 60 * 1000;

  switch (time_method_) {
    case TimeMethod::kTimeRel:
      time_method_ = TimeMethod::kTimeLocal;
      ui->toolButton_time->setIcon(QIcon(":/resource/loc.png"));
      ui->toolButton_time->setChecked(true);
      ui->toolButton_time->setToolTip(tr("Local Time"));

      if (point_dialog_->isVisible()) {
        point_dialog_->set_date_time(date_timestamp_.load() + timeoffset);
      }

      break;
    case TimeMethod::kTimeLocal:
      time_method_ = TimeMethod::kTimeUtc;
      ui->toolButton_time->setIcon(QIcon(":/resource/utc.png"));
      ui->toolButton_time->setChecked(true);
      ui->toolButton_time->setToolTip(tr("UTC Time"));

      if (player_ && point_dialog_->isVisible()) {
        point_dialog_->set_date_time(date_timestamp_.load());
      }

      break;
    case TimeMethod::kTimeUtc:
      time_method_ = TimeMethod::kTimeRel;
      ui->toolButton_time->setIcon(QIcon(":/resource/rel.png"));
      ui->toolButton_time->setChecked(false);
      ui->toolButton_time->setToolTip(tr("Real Time"));

      if (player_ && point_dialog_->isVisible()) {
        point_dialog_->set_date_time(0);
      }

      break;
    default:
      break;
  }

  if (status_ == vlink::BagReader::kPlaying || status_ == vlink::BagReader::kPaused) {
    update_timestamp(player_->get_timestamp());
  } else {
    update_timestamp(curent_timestamp_);
  }
}

void PlayerWindow::on_toolButton_remap_clicked(bool checked) {
  (void)checked;

  QFileDialog dialog(this, tr("Select remap file"), remap_path_, "Remap files (*.json)");

#if QT_VERSION < QT_VERSION_CHECK(5, 15, 0)
  dialog.setOption(QFileDialog::DontUseNativeDialog, true);
#endif

  dialog.setFileMode(QFileDialog::ExistingFile);
  dialog.setAcceptMode(QFileDialog::AcceptOpen);
  dialog.setDefaultSuffix("json");

  if (dialog.exec() != QDialog::Accepted) {
    ui->toolButton_remap->setIcon(QIcon(":/resource/remap.png"));
    ui->toolButton_remap->setChecked(false);

    {
      std::lock_guard remap_lock(remap_mtx_);
      remap_.unload();
    }

    return;
  }

  QFileInfo file_info(dialog.selectedFiles().constFirst());

  if (file_info.isFile() && file_info.exists()) {
    remap_path_ = file_info.absoluteFilePath();

    {
      QSettings settings(QStandardPaths::writableLocation(QStandardPaths::CacheLocation) + "/settings.ini",
                         QSettings::IniFormat);
      settings.beginGroup("PlayerWindow");
      settings.setValue("remap_path", remap_path_);
      settings.endGroup();
      settings.sync();
    }
  } else {
    remap_path_ = qApp->applicationDirPath();
  }

  bool remap_ret = false;

  {
    std::lock_guard remap_lock(remap_mtx_);
    remap_ret = remap_.reload(remap_path_.toStdString());
  }

  if (!remap_ret) {
    ui->toolButton_remap->setIcon(QIcon(":/resource/remap.png"));
    ui->toolButton_remap->setChecked(false);

    {
      std::lock_guard remap_lock(remap_mtx_);
      remap_.unload();
    }

    QString error_string = QString::fromStdString(remap_.get_error_string());

    if (error_string.isEmpty()) {
      QMessageBox::critical(this, tr("Remap Error"), tr("Remap file can not open"));
    } else {
      QMessageBox::critical(this, tr("Remap Error"), QString::fromStdString(remap_.get_error_string()));
    }

    return;
  }

  ui->toolButton_remap->setIcon(QIcon(":/resource/remap_down.png"));
  ui->toolButton_remap->setChecked(true);
}

void PlayerWindow::on_toolButton_skip_clicked(bool checked) {
  if (checked) {
    ui->toolButton_skip->setIcon(QIcon(":/resource/skip_down.png"));

    if (player_) {
      ui->horizontalSlider_progress->setRange(player_->get_info().blank_duration / 100,
                                              (player_->get_info().total_duration + 50) / 100);

      update_windows_title(true);
    }
  } else {
    ui->toolButton_skip->setIcon(QIcon(":/resource/skip.png"));
    if (player_) {
      ui->horizontalSlider_progress->setRange(0, (player_->get_info().total_duration + 50) / 100);

      update_windows_title(true);
    }
  }
}

void PlayerWindow::on_toolButton_proxy_clicked(bool checked) {
  ui->lineEdit_proxy->setEnabled(!checked);

  if (checked) {
    QStringList real_arg_list;
    const auto& arg_list = ui->lineEdit_proxy->text().split(" ");

    for (const auto& arg : arg_list) {
      if (arg.isEmpty()) {
        continue;
      }

      real_arg_list.append(arg);
    }

    QSettings settings(QStandardPaths::writableLocation(QStandardPaths::CacheLocation) + "/settings.ini",
                       QSettings::IniFormat);
    settings.beginGroup("PlayerWindow");
    settings.setValue("proxy_args", real_arg_list.join(" "));
    settings.endGroup();
    settings.sync();

    // real_arg_list.append("-m");
    // real_arg_list.append("off");

    if (proxy_process_.state() != QProcess::NotRunning) {
      {
        std::lock_guard lock(urls_mtx_);
        pub_urls_map_.clear();
      }

#if defined(VLINK_SUPPORT_SHM)

      if (vlink::ShmConf::has_runtime_inited()) {
        vlink::ShmConf::deinit_runtime();
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
      }
#endif

      proxy_user_kill_ = true;
      proxy_process_.terminate();
      proxy_process_.waitForFinished(500);
      proxy_process_.kill();
      proxy_process_.waitForFinished(1000);
    }

    proxy_user_kill_ = false;
    proxy_process_.setArguments(real_arg_list);
    proxy_process_.start();
    proxy_process_.waitForStarted(1000);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

#if USE_LOCK_PROXY_DESTROY
    ui->toolButton_proxy->setEnabled(proxy_process_.state() == QProcess::NotRunning);
#else
    ui->toolButton_proxy->setEnabled(true);
#endif
  } else {
    if (QMessageBox::warning(this, tr("Warning"), tr("Terminating the proxy may cause the program to crash, continue?"),
                             QMessageBox::StandardButtons(QMessageBox::Yes | QMessageBox::No)) != QMessageBox::Yes) {
      ui->toolButton_proxy->setChecked(true);
      ui->lineEdit_proxy->setEnabled(false);

      return;
    }

    if (viewer_process_.state() != QProcess::NotRunning) {
      viewer_process_.terminate();
      viewer_process_.waitForFinished(500);
      viewer_process_.kill();
      viewer_process_.waitForFinished(1000);
    }

    if (analyzer_process_.state() != QProcess::NotRunning) {
      analyzer_process_.terminate();
      analyzer_process_.waitForFinished(500);
      analyzer_process_.kill();
      analyzer_process_.waitForFinished(1000);
    }

    kill_proxy_clients();

    {
      std::lock_guard lock(urls_mtx_);
      pub_urls_map_.clear();
    }

#if defined(VLINK_SUPPORT_SHM)

    if (vlink::ShmConf::has_runtime_inited()) {
      vlink::ShmConf::deinit_runtime();
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
#endif

    proxy_user_kill_ = true;

    if (proxy_process_.state() != QProcess::NotRunning) {
      proxy_process_.terminate();
      proxy_process_.waitForFinished(500);
      proxy_process_.kill();
      proxy_process_.waitForFinished(1000);
    }
  }

  update_proxy_icon();

  update_status();
}

void PlayerWindow::on_checkBox_black_clicked(bool checked) { (void)checked; }

void PlayerWindow::on_checkBox_loop_clicked(bool checked) {
  QSettings settings(QStandardPaths::writableLocation(QStandardPaths::CacheLocation) + "/settings.ini",
                     QSettings::IniFormat);
  settings.beginGroup("PlayerWindow");
  settings.setValue("loop_mode", checked);
  settings.endGroup();
  settings.sync();

  reload();
}

void PlayerWindow::on_checkBox_native_clicked(bool checked) {
  QSettings settings(QStandardPaths::writableLocation(QStandardPaths::CacheLocation) + "/settings.ini",
                     QSettings::IniFormat);
  settings.beginGroup("PlayerWindow");
  settings.setValue("native_mode", checked);
  settings.endGroup();
  settings.sync();
}

void PlayerWindow::on_horizontalSlider_progress_sliderPressed() {
  if (!player_) {
    return;
  }

  if (status_ == vlink::BagReader::kPlaying) {
    wait_widget_->start_wait();

    status_ = vlink::BagReader::kPaused;
    player_->pause();

    main_timer_->stop();

    press_and_paused_ = true;
  }
}

void PlayerWindow::on_horizontalSlider_progress_sliderReleased() {
  if (!player_) {
    press_and_paused_ = false;
    return;
  }

  auto timestamp = static_cast<int64_t>(ui->horizontalSlider_progress->value()) * 100;

  update_timestamp(timestamp);

  if (status_ != vlink::BagReader::kPlaying && status_ != vlink::BagReader::kPaused) {
    press_and_paused_ = false;
    return;
  }

  wait_widget_->start_wait();

  double rate = ui->doubleSpinBox_rate->value() <= 0 ? 1 : ui->doubleSpinBox_rate->value();

  int times = ui->checkBox_loop->isChecked() ? 0 : 1;

  if (status_ == vlink::BagReader::kPaused && press_and_paused_) {
    status_ = vlink::BagReader::kPlaying;

    player_->jump(timestamp, rate, times, true);

    main_timer_->start();
  } else {
    player_->jump(timestamp, rate, times, false);
  }

  press_and_paused_ = false;
}

void PlayerWindow::on_horizontalSlider_progress_sliderMoved(int position) {
  if (!player_) {
    return;
  }

  update_timestamp(static_cast<int64_t>(position) * 100);
}

void PlayerWindow::on_doubleSpinBox_rate_editingFinished() {
  if (last_rate_ == ui->doubleSpinBox_rate->value()) {
    return;
  }

  reload();

  last_rate_ = ui->doubleSpinBox_rate->value();
}

void PlayerWindow::ready_to_play(const QString& warn_string) {
  ui->toolButton_play->setEnabled(true);
  ui->toolButton_pause->setEnabled(true);
  ui->toolButton_close->setEnabled(true);
  ui->toolButton_select->setEnabled(true);

  if (has_played_) {
    return;
  }

  if (!player_) {
    return;
  }

  wait_widget_->start_wait();

  vlink::BagReader::Config config;
  config.begin_time = curent_timestamp_;
  config.end_time = 0;
  config.times = ui->checkBox_loop->isChecked() ? 0 : 1;
  config.rate = ui->doubleSpinBox_rate->value();
  config.skip_blank = false;
  config.force_delay = -1;
  config.auto_pause = false;
  config.auto_quit = false;

  if (!filter_urls_.empty()) {
    std::lock_guard lock(urls_mtx_);
    config.filter_urls = filter_urls_;
  }

  status_ = vlink::BagReader::kPlaying;

  player_->play(config);

  main_timer_->start();
  play_timer_->stop();

  has_played_ = true;

  if (!warn_string.isEmpty()) {
    auto* warn_box = new QMessageBox(QMessageBox::Warning, tr("Warning"), warn_string, QMessageBox::Ok, this);
    warn_box->setAttribute(Qt::WA_DeleteOnClose);
    warn_box->setModal(false);
    warn_box->show();
  }
}

void PlayerWindow::update_windows_title(bool show_percent) {
  if (show_percent) {
    int value = ui->horizontalSlider_progress->value() - ui->horizontalSlider_progress->minimum();
    int total = ui->horizontalSlider_progress->maximum() - ui->horizontalSlider_progress->minimum();

    if (total > 0) {
      setWindowTitle(tr("VLink Player ") + VLINK_VERSION + " -> [" + QString::number(100.0 * value / total, 'f', 1) +
                     "%]");
    } else {
      setWindowTitle(tr("VLink Player ") + VLINK_VERSION + " -> [0.0%]");
    }
  } else {
    setWindowTitle(tr("VLink Player ") + VLINK_VERSION);
  }
}

void PlayerWindow::update_timestamp(int64_t timestamp) {
  if (!player_) {
    return;
  }

  std::string current_time_str;
  std::string total_time_str;

  int64_t timeoffset = player_->get_info().timezone * 60 * 1000;

  switch (time_method_) {
    case TimeMethod::kTimeRel:
      current_time_str = vlink::Helpers::format_milliseconds(timestamp, true);
      total_time_str = vlink::Helpers::format_milliseconds(player_->get_info().total_duration, true);
      ui->label_time->setText(QString::fromStdString(current_time_str + "/" + total_time_str));
      break;
    case TimeMethod::kTimeLocal:
      current_time_str = vlink::Helpers::format_milliseconds(timestamp + date_timestamp_ + timeoffset, true);
      total_time_str =
          vlink::Helpers::format_milliseconds(player_->get_info().total_duration + date_timestamp_ + timeoffset, true);
      ui->label_time->setText(QString::fromStdString(current_time_str + "/" + total_time_str));
      break;
    case TimeMethod::kTimeUtc:
      current_time_str = vlink::Helpers::format_milliseconds(timestamp + date_timestamp_, true);
      total_time_str = vlink::Helpers::format_milliseconds(player_->get_info().total_duration + date_timestamp_, true);
      ui->label_time->setText(QString::fromStdString(current_time_str + "/" + total_time_str));
      break;
    default:
      break;
  }

  ui->horizontalSlider_progress->setValue(timestamp / 100);

  update_windows_title(true);

  curent_timestamp_ = timestamp;

  if (status_ == vlink::BagReader::kPlaying) {
    if (is_changed_) {
      ui->label_time->setStyleSheet("QLabel { color: #008000; }");
    } else {
      ui->label_time->setStyleSheet("QLabel { color: #FF4500; }");
    }
  } else if (status_ == vlink::BagReader::kPaused) {
    ui->label_time->setStyleSheet("QLabel { color: #CAA000; }");
  }

  is_changed_ = false;

  if (total_elapsed_timer.get() >= 1000) {
    if (bytes_size_ < 1024) {
      ui->label_bytes->setText(QString::number(bytes_size_) + "B/s  ");
    } else if (bytes_size_ < 1024LL * 1024) {
      ui->label_bytes->setText(QString::number(bytes_size_ / 1024.0F, 'f', 2) + "KB/s  ");
    } else if (bytes_size_ < 1024LL * 1024 * 1024) {
      ui->label_bytes->setText(QString::number(bytes_size_ / 1024 / 1024.0F, 'f', 2) + "MB/s  ");
    } else {
      ui->label_bytes->setText(QString::number(bytes_size_ / 1024 / 1024 / 1024.0F, 'f', 2) + "GB/s  ");
    }

    bytes_size_ = 0;

    total_elapsed_timer.restart();
  }

  if (analyzer_process_.state() == QProcess::Running) {
    QString timestamp_str = QString::number(timestamp);
    timestamp_str.append('\n');
    analyzer_process_.write(timestamp_str.toUtf8());
  }
}

void PlayerWindow::update_status() {
  if (status_ == vlink::BagReader::kPlaying) {
    ui->toolButton_info->setEnabled(true);
    ui->toolButton_viewer->setEnabled(true);
    ui->toolButton_analyzer->setEnabled(true);
    ui->toolButton_play->setEnabled(true);
    ui->toolButton_pause->setEnabled(true);
    ui->toolButton_play->setIcon(QIcon(":/resource/stop.png"));
    ui->toolButton_pause->setIcon(QIcon(":/resource/pause.png"));
    ui->toolButton_play->setToolTip(tr("Stop"));
    ui->toolButton_pause->setToolTip(tr("Pause"));

    ui->checkBox_native->setEnabled(false);

    ui->toolButton_select->setEnabled(false);
    ui->toolButton_close->setEnabled(true);
    ui->toolButton_point->setEnabled(true);

    ui->toolButton_time->setEnabled(true);
    ui->toolButton_skip->setEnabled(true);
    ui->toolButton_remap->setEnabled(false);

    ui->label_filter->setEnabled(false);
    ui->lineEdit_filter->setEnabled(false);
    ui->checkBox_black->setEnabled(false);
    ui->doubleSpinBox_rate->setEnabled(true);
    ui->checkBox_loop->setEnabled(true);

    ui->label_time->setEnabled(true);
    ui->label_bytes->setEnabled(true);

    ui->toolButton_proxy->setEnabled(false);
    ui->lineEdit_proxy->setEnabled(false);

    ui->label_time->setStyleSheet("QLabel { color: #FF4500; }");
    ui->label_bytes->setStyleSheet("QLabel { color: #339AF0; }");

    ui->horizontalSlider_progress->setEnabled(true);

    // wait_widget_->stop_wait();
  } else if (status_ == vlink::BagReader::kPaused) {
    ui->toolButton_info->setEnabled(true);
    ui->toolButton_viewer->setEnabled(true);
    ui->toolButton_analyzer->setEnabled(true);
    ui->toolButton_play->setEnabled(true);
    ui->toolButton_pause->setEnabled(true);
    ui->toolButton_play->setIcon(QIcon(":/resource/stop.png"));
    ui->toolButton_pause->setIcon(QIcon(":/resource/resume.png"));
    ui->toolButton_play->setToolTip(tr("Stop"));
    ui->toolButton_pause->setToolTip(tr("Resume"));

    ui->checkBox_native->setEnabled(false);

    ui->toolButton_select->setEnabled(false);
    ui->toolButton_close->setEnabled(true);
    ui->toolButton_point->setEnabled(true);

    ui->toolButton_time->setEnabled(true);
    ui->toolButton_skip->setEnabled(true);
    ui->toolButton_remap->setEnabled(false);

    ui->label_filter->setEnabled(false);
    ui->lineEdit_filter->setEnabled(false);
    ui->checkBox_black->setEnabled(false);
    ui->doubleSpinBox_rate->setEnabled(true);
    ui->checkBox_loop->setEnabled(true);

    ui->label_time->setEnabled(true);
    ui->label_bytes->setEnabled(true);

    ui->toolButton_proxy->setEnabled(false);
    ui->lineEdit_proxy->setEnabled(false);

    ui->label_time->setStyleSheet("QLabel { color: #CAA000; }");
    ui->label_bytes->setStyleSheet("QLabel { color: #339AF0; }");
    ui->label_bytes->setText("0B/s  ");

    ui->horizontalSlider_progress->setEnabled(true);

    if (player_ && !press_and_paused_ && !ui->horizontalSlider_progress->is_mouse_pressed()) {
      update_timestamp(player_->get_timestamp());
    }

    wait_widget_->stop_wait();
  } else if (status_ == vlink::BagReader::kStopped) {
    ui->toolButton_info->setEnabled(true);
    ui->toolButton_viewer->setEnabled(true);
    ui->toolButton_analyzer->setEnabled(true);
    ui->toolButton_play->setEnabled(true);
    ui->toolButton_pause->setEnabled(false);
    ui->toolButton_play->setIcon(QIcon(":/resource/play.png"));
    ui->toolButton_pause->setIcon(QIcon(":/resource/pause.png"));
    ui->toolButton_play->setToolTip(tr("Play"));
    ui->toolButton_pause->setToolTip(tr("Pause"));

    ui->checkBox_native->setEnabled(true);

    ui->toolButton_select->setEnabled(true);
    ui->toolButton_close->setEnabled(true);
    ui->toolButton_point->setEnabled(true);

    ui->toolButton_time->setEnabled(true);
    ui->toolButton_skip->setEnabled(true);
    ui->toolButton_remap->setEnabled(true);

    ui->label_filter->setEnabled(true);
    ui->lineEdit_filter->setEnabled(true);
    ui->checkBox_black->setEnabled(true);
    ui->doubleSpinBox_rate->setEnabled(true);
    ui->checkBox_loop->setEnabled(true);

    ui->label_time->setEnabled(true);
    ui->label_bytes->setEnabled(true);

#if USE_LOCK_PROXY_DESTROY
    ui->toolButton_proxy->setEnabled(proxy_process_.state() == QProcess::NotRunning);
#else
    ui->toolButton_proxy->setEnabled(true);
#endif

    ui->lineEdit_proxy->setEnabled(!ui->toolButton_proxy->isChecked());

    update_proxy_icon();

    ui->label_time->setStyleSheet("QLabel { color: #FF4500; }");
    ui->label_bytes->setStyleSheet("QLabel { color: #339AF0; }");
    ui->label_bytes->setText("0B/s  ");

    ui->horizontalSlider_progress->setEnabled(true);

    if (player_) {
      update_timestamp(player_->get_info().blank_duration);
    }

    pause_to_next_flag_ = false;

    wait_widget_->stop_wait();
  } else {
    ui->toolButton_info->setEnabled(false);
    ui->toolButton_viewer->setEnabled(true);
    ui->toolButton_analyzer->setEnabled(false);
    ui->toolButton_play->setEnabled(false);
    ui->toolButton_pause->setEnabled(false);
    ui->toolButton_play->setIcon(QIcon(":/resource/play.png"));
    ui->toolButton_pause->setIcon(QIcon(":/resource/pause.png"));
    ui->toolButton_play->setToolTip(tr("Play"));
    ui->toolButton_pause->setToolTip(tr("Pause"));

    ui->checkBox_native->setEnabled(false);

    ui->toolButton_select->setEnabled(true);
    ui->toolButton_close->setEnabled(false);
    ui->toolButton_point->setEnabled(false);

    ui->toolButton_time->setEnabled(false);
    ui->toolButton_skip->setEnabled(false);
    ui->toolButton_remap->setEnabled(false);

    time_method_ = TimeMethod::kTimeRel;

    ui->toolButton_time->setChecked(false);
    ui->toolButton_skip->setChecked(false);
    ui->toolButton_remap->setChecked(false);

    ui->toolButton_time->setIcon(QIcon(":/resource/rel.png"));
    ui->toolButton_skip->setIcon(QIcon(":/resource/skip.png"));
    ui->toolButton_remap->setIcon(QIcon(":/resource/remap.png"));

    ui->toolButton_time->setToolTip(tr("Real Time"));

    {
      std::lock_guard remap_lock(remap_mtx_);
      remap_.unload();
    }

    ui->label_filter->setEnabled(false);
    ui->lineEdit_filter->setEnabled(false);
    ui->checkBox_black->setEnabled(false);
    ui->doubleSpinBox_rate->setEnabled(false);
    ui->checkBox_loop->setEnabled(false);

    ui->label_time->setEnabled(false);
    ui->label_bytes->setEnabled(false);

#if USE_LOCK_PROXY_DESTROY
    ui->toolButton_proxy->setEnabled(proxy_process_.state() == QProcess::NotRunning);
#else
    ui->toolButton_proxy->setEnabled(true);
#endif

    ui->lineEdit_proxy->setEnabled(!ui->toolButton_proxy->isChecked());

    update_proxy_icon();

    ui->label_time->setText(QString::fromStdString("00:00:00:000/00:00:00:000"));
    ui->label_time->setStyleSheet("");
    ui->label_bytes->setStyleSheet("");
    ui->label_bytes->setText("0B/s  ");

    ui->horizontalSlider_progress->setEnabled(false);
    ui->horizontalSlider_progress->setValue(0);
    update_windows_title(false);

    pause_to_next_flag_ = false;

    curent_timestamp_ = 0;

    wait_widget_->stop_wait();
  }
}

void PlayerWindow::process_ready() {
  wait_widget_->stop_wait();

  if (player_ && (status_ == vlink::BagReader::kPlaying || status_ == vlink::BagReader::kPaused)) {
    update_timestamp(player_->get_timestamp());
  }
}

void PlayerWindow::update_proxy_icon() {
  if (ui->toolButton_proxy->isChecked()) {
    ui->toolButton_proxy->setIcon(QIcon(":/resource/proxy_down.png"));
    ui->toolButton_proxy->setToolTip(tr("Stop Proxy"));
  } else {
    ui->toolButton_proxy->setIcon(QIcon(":/resource/proxy.png"));
    ui->toolButton_proxy->setToolTip(tr("Start Proxy"));
  }
}

bool PlayerWindow::load_bag(const QString& path) {
  wait_widget_->stop_wait();
  info_dialog_->close();
  point_dialog_->close();

  // if (viewer_process_.state() != QProcess::NotRunning) {
  //   viewer_process_.terminate();
  //   viewer_process_.waitForFinished(500);
  //   viewer_process_.kill();
  //   viewer_process_.waitForFinished(1000);
  // }

  if (analyzer_process_.state() != QProcess::NotRunning) {
    analyzer_process_.terminate();
    analyzer_process_.waitForFinished(500);
    analyzer_process_.kill();
    analyzer_process_.waitForFinished(1000);
  }

  if (!QFile::exists(path)) {
    QMessageBox::critical(this, tr("File Error"), tr("File not exists (%1).").arg(path));

    has_error_ = true;

    return false;
  }

  file_path_ = path;

  ui->lineEdit_path->setText(file_path_);

  if (player_) {
    if (player_->is_running()) {
      player_->stop();
      player_->quit();
      player_->wait_for_quit(2000);
    }
    player_.reset();
  }

  {
    std::lock_guard lock(urls_mtx_);
    pub_urls_map_.clear();
  }

  try {
    player_ = vlink::BagReader::create(file_path_.toUtf8().data(), true);
  } catch (vlink::Exception::RuntimeError& e) {
    QMessageBox::critical(this, tr("File Error"), QString::fromStdString(e.what()));

    ui->lineEdit_path->clear();

    has_error_ = true;

    ui->horizontalSlider_progress->setRange(0, 100);

    return false;
  }

  if VUNLIKELY (!player_) {
    QMessageBox::critical(this, tr("File Error"), tr("Unsupported file format."));

    ui->lineEdit_path->clear();

    has_error_ = true;

    ui->horizontalSlider_progress->setRange(0, 100);

    return false;
  }

  if (ui->toolButton_skip->isChecked()) {
    ui->horizontalSlider_progress->setRange(player_->get_info().blank_duration / 100,
                                            (player_->get_info().total_duration + 50) / 100);
  } else {
    ui->horizontalSlider_progress->setRange(0, (player_->get_info().total_duration + 50) / 100);
  }

  date_timestamp_ = player_->get_info().start_timestamp % (24ULL * 60 * 60 * 1000);

  has_error_ = false;

  main_timer_->stop();
  play_timer_->stop();

  player_->register_begin_handler([this]() {
    pause_to_next_flag_ = false;
    status_ = vlink::BagReader::kStopped;
    QMetaObject::invokeMethod(this, "update_status", Qt::QueuedConnection);
  });

  player_->register_end_handler([this]() {
    pause_to_next_flag_ = false;
    status_ = -1;
    QMetaObject::invokeMethod(this, "update_status", Qt::QueuedConnection);
  });

  player_->register_output_callback([this](const vlink::Frame& frame) {
    const int64_t timestamp = frame.timestamp;
    const std::string& url = frame.url;
    const vlink::ActionType action_type = frame.action_type;
    const vlink::Bytes& raw_data = frame.data;

    (void)timestamp;

    if (action_type == vlink::ActionType::kClientRequest || action_type == vlink::ActionType::kClientResponse ||
        action_type == vlink::ActionType::kServerRequest || action_type == vlink::ActionType::kServerResponse) {
      if (pause_to_next_flag_) {
        player_->pause_to_next();
      }

      return;
    }

    {
      std::shared_lock lock(urls_mtx_);
      auto iter = pub_urls_map_.find(url);

      if (iter == pub_urls_map_.end()) {
        if (pause_to_next_flag_) {
          player_->pause_to_next();
        }

        return;
      }

      iter->second->publish(raw_data);
    }

    if (pause_to_next_flag_) {
      QMetaObject::invokeMethod(this, "update_timestamp", Qt::QueuedConnection,
                                Q_ARG(int64_t, player_->get_timestamp()));
    }

    pause_to_next_flag_ = false;
    is_changed_ = true;
    bytes_size_ += raw_data.size();
  });

  player_->register_status_callback([this](vlink::BagReader::Status status) {
    status_ = status;

    if (player_ && player_->is_jumping() && !press_and_paused_) {
      return;
    }

    QMetaObject::invokeMethod(this, "update_status", Qt::QueuedConnection);

    bytes_size_ = 0;
    total_elapsed_timer.restart();
  });

  player_->register_ready_callback(
      [this]() { QMetaObject::invokeMethod(this, "process_ready", Qt::QueuedConnection); });

  player_->register_finish_callback([this](bool is_interrupted) {
    (void)is_interrupted;

    {
      std::lock_guard lock(urls_mtx_);
      pub_urls_map_.clear();
    }

    QMetaObject::invokeMethod(main_timer_, "stop", Qt::QueuedConnection);

    status_ = vlink::BagReader::kStopped;
  });

  status_ = vlink::BagReader::kStopped;

  player_->async_run();

  {
    QSettings settings(QStandardPaths::writableLocation(QStandardPaths::CacheLocation) + "/settings.ini",
                       QSettings::IniFormat);
    settings.beginGroup("PlayerWindow");
    settings.setValue("bag_path", file_path_);
    settings.endGroup();
    settings.sync();
  }

  return true;
}

void PlayerWindow::reload() {
  if (wait_widget_->is_working()) {
    return;
  }

  if (ui->doubleSpinBox_rate->value() <= 0) {
    ui->doubleSpinBox_rate->blockSignals(true);
    ui->doubleSpinBox_rate->setValue(1);
    ui->doubleSpinBox_rate->blockSignals(false);
    last_rate_ = 1;
  }

  if (!player_) {
    return;
  }

  if (status_ != vlink::BagReader::kPlaying && status_ != vlink::BagReader::kPaused) {
    return;
  }

  double rate = ui->doubleSpinBox_rate->value() <= 0 ? 1 : ui->doubleSpinBox_rate->value();

  int times = ui->checkBox_loop->isChecked() ? 0 : 1;

  player_->jump(player_->get_timestamp(), rate, times, false);
}

bool PlayerWindow::check_has_shm() {
  if (!player_) {
    return false;
  }

  for (const auto& url_meta : player_->get_info().url_metas) {
    if (vlink::Helpers::has_startwith(url_meta.url, "shm://")) {
      return true;
    }
  }

  return false;
}

void PlayerWindow::consume_analyzer_timestamp_bytes(const QByteArray& bytes) {
  if (bytes.isEmpty()) {
    return;
  }

  analyzer_timestamp_buffer_.append(bytes);

  while (true) {
    const auto newline_pos = analyzer_timestamp_buffer_.indexOf('\n');

    if (newline_pos < 0) {
      break;
    }

    const auto frame = analyzer_timestamp_buffer_.left(newline_pos).trimmed();
    analyzer_timestamp_buffer_.remove(0, newline_pos + 1);

    if (frame.isEmpty()) {
      continue;
    }

    bool ok = false;
    const int64_t timestamp = QString::fromLatin1(frame).toLongLong(&ok);

    if (ok) {
      handle_analyzer_timestamp(timestamp);
    }
  }

  if VUNLIKELY (analyzer_timestamp_buffer_.size() > 128) {
    analyzer_timestamp_buffer_.clear();
  }
}

void PlayerWindow::handle_analyzer_timestamp(int64_t timestamp) {
  if (player_ && (status_ == vlink::BagReader::kPlaying || status_ == vlink::BagReader::kPaused)) {
    double rate = ui->doubleSpinBox_rate->value() <= 0 ? 1 : ui->doubleSpinBox_rate->value();

    int times = ui->checkBox_loop->isChecked() ? 0 : 1;

    player_->jump(timestamp, rate, times, false);
  } else {
    update_timestamp(timestamp);
  }
}

void PlayerWindow::kill_proxy_clients() {
#ifdef Q_OS_WINDOWS
  QProcess kill_eproto;
  kill_eproto.start(QString("cmd.exe"), QStringList() << QString("/c") << QString("taskkill") << QString("-f")
                                                      << QString("-im") << "vlink-eproto.exe");
  kill_eproto.waitForStarted(500);
  kill_eproto.waitForFinished(1000);

  QProcess kill_efbs;
  kill_efbs.start(QString("cmd.exe"), QStringList() << QString("/c") << QString("taskkill") << QString("-f")
                                                    << QString("-im") << "vlink-efbs.exe");
  kill_efbs.waitForStarted(500);
  kill_efbs.waitForFinished(1000);

  QProcess kill_bag;
  kill_bag.start(QString("cmd.exe"), QStringList() << QString("/c") << QString("taskkill") << QString("-f")
                                                   << QString("-im") << "vlink-bag.exe");
  kill_bag.waitForStarted(500);
  kill_bag.waitForFinished(1000);

  QProcess kill_monitor;
  kill_monitor.start(QString("cmd.exe"), QStringList() << QString("/c") << QString("taskkill") << QString("-f")
                                                       << QString("-im") << "vlink-monitor.exe");
  kill_monitor.waitForStarted(500);
  kill_monitor.waitForFinished(1000);
#else
  int ret = 0;
  ret += std::system(QString("killall -q -9 eproto >/dev/null 2>&1").toUtf8());
  ret += std::system(QString("killall -q -9 efbs >/dev/null 2>&1").toUtf8());
  ret += std::system(QString("killall -q -9 bag >/dev/null 2>&1").toUtf8());
  ret += std::system(QString("killall -q -9 monitor >/dev/null 2>&1").toUtf8());
  ret += std::system(QString("killall -q -9 vlink-eproto >/dev/null 2>&1").toUtf8());
  ret += std::system(QString("killall -q -9 vlink-efbs >/dev/null 2>&1").toUtf8());
  ret += std::system(QString("killall -q -9 vlink-bag >/dev/null 2>&1").toUtf8());
  ret += std::system(QString("killall -q -9 vlink-monitor >/dev/null 2>&1").toUtf8());
  (void)ret;
#endif
}

// NOLINTEND
