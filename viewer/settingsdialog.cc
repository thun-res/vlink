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

#include "./settingsdialog.h"

#include <vlink/base/utils.h>
#include <vlink/version.h>

#include <QCheckBox>
#include <QRegularExpressionValidator>
#include <QSettings>
#include <QStandardPaths>

#include "./ui_settingsdialog.h"

QRegularExpression g_ip_regex("((2[0-4]\\d|25[0-5]|[01]?\\d\\d?)\\.){3}(2[0-4]\\d|25[0-5]|[01]?\\d\\d?)");

static constexpr int kDefaultDdsBufSize = 8388608;
static constexpr int kDefaultDdsMtuSize = 65500;

bool SettingsDialog::do_select() {
  SettingsDialog dialog;
  return dialog.exec() == SettingsDialog::Accepted;
}

SettingsDialog::Mode SettingsDialog::get_mode() { return mode_; }

int SettingsDialog::get_domain_id() { return domain_id_; }

std::string SettingsDialog::get_dds_impl() { return dds_impl_; }

std::string SettingsDialog::get_security_key() { return security_key_; }

bool SettingsDialog::get_detect_upgrade() { return detect_upgrade_; }

bool SettingsDialog::get_match_version() { return match_version_; }

bool SettingsDialog::get_native_mode() { return native_mode_; }

bool SettingsDialog::get_reliable_mode() { return reliable_mode_; }

bool SettingsDialog::get_tcp_mode() { return tcp_mode_; }

bool SettingsDialog::get_direct_mode() { return direct_mode_; }

std::string SettingsDialog::get_allow_ip() { return allow_ip_; }

std::string SettingsDialog::get_peer_ip() { return peer_ip_; }

int SettingsDialog::get_buf_size() { return buf_size_; }

int SettingsDialog::get_mtu_size() { return mtu_size_; }

void SettingsDialog::on_checkBox_native_clicked(bool checked) {
  ui->groupBox_allow->setEnabled(!checked);
  ui->groupBox_peer->setEnabled(!checked);
}

void SettingsDialog::on_pushButton_controller_clicked() {
  mode_ = kController;

  save_profile();

  accept();
}

void SettingsDialog::on_pushButton_listener_clicked() {
  mode_ = kListener;

  save_profile();

  accept();
}

void SettingsDialog::on_toolButton_show_clicked() {
  if (ui->lineEdit_key->echoMode() == QLineEdit::Normal) {
    ui->lineEdit_key->setEchoMode(QLineEdit::Password);
    ui->toolButton_show->setIcon(QIcon(":/resource/hide.png"));
  } else {
    ui->lineEdit_key->setEchoMode(QLineEdit::Normal);
    ui->toolButton_show->setIcon(QIcon(":/resource/show.png"));
  }
}

SettingsDialog::SettingsDialog(QWidget* parent) : QDialog(parent), ui(new Ui::SettingsDialog) {
  setWindowFlags(Qt::Dialog | Qt::WindowCloseButtonHint);

  ui->setupUi(this);

  setWindowTitle(tr("VLink Viewer") + " v" + VLINK_VERSION);

  ui->radioButton_dds->setChecked(true);
  ui->radioButton_ddsc->setChecked(false);

  ui->spinBox_domain->setMinimum(0);
  ui->spinBox_domain->setMaximum(255);
  ui->spinBox_domain->setSingleStep(1);
  ui->spinBox_domain->setValue(0);

  ui->lineEdit_key->setEchoMode(QLineEdit::Password);
  ui->toolButton_show->setText(tr("Show"));

  validator_ip_ = new QRegularExpressionValidator(g_ip_regex);
  ui->lineEdit_peer->setValidator(validator_ip_);

  ui->spinBox_buf->setMinimum(65536);
  ui->spinBox_buf->setMaximum(99999999);
  ui->spinBox_buf->setValue(kDefaultDdsBufSize);
  ui->spinBox_buf->setSingleStep(1);

  ui->spinBox_mtu->setMinimum(1024);
  ui->spinBox_mtu->setMaximum(16777216);
  ui->spinBox_mtu->setValue(kDefaultDdsMtuSize);
  ui->spinBox_mtu->setSingleStep(1);

  QString allow_ip;
  QString peer_ip;
  int buf_size = 0;
  int mtu_size = 0;

  {
    QSettings settings(QStandardPaths::writableLocation(QStandardPaths::CacheLocation) + "/settings.ini",
                       QSettings::IniFormat);

    settings.beginGroup("SettingsDialog");

    domain_id_ = settings.value("domain_id", 100).toInt();

    dds_impl_ = settings.value("dds_impl", "dds").toString().toStdString();

    QByteArray key_base64 = settings.value("security_key", "").toByteArray();

    if (!key_base64.isEmpty()) {
      security_key_ = QByteArray::fromBase64(key_base64).toStdString();
    }

    detect_upgrade_ = settings.value("detect_upgrade", true).toBool();

    match_version_ = settings.value("match_version", true).toBool();

    native_mode_ = settings.value("native_mode", false).toBool();

    reliable_mode_ = settings.value("reliable_mode", false).toBool();

    tcp_mode_ = settings.value("tcp_mode", false).toBool();

    direct_mode_ = settings.value("direct_mode", false).toBool();

    allow_ip = settings.value("allow_ip", "").toString();

    peer_ip = settings.value("peer_ip", "").toString();

    buf_size = settings.value("buf_size", 0).toInt();

    mtu_size = settings.value("mtu_size", 0).toInt();

    settings.endGroup();
  }

  ui->spinBox_domain->setValue(domain_id_);

  if (dds_impl_ == "ddsc") {
    ui->radioButton_ddsc->setChecked(true);
    ui->radioButton_dds->setChecked(false);
  } else {
    ui->radioButton_ddsc->setChecked(false);
    ui->radioButton_dds->setChecked(true);
  }

  ui->lineEdit_key->setText(QString::fromStdString(security_key_));

  ui->toolButton_show->setEnabled(!ui->lineEdit_key->text().isEmpty());

  ui->checkBox_upgrade->setChecked(detect_upgrade_);

  ui->checkBox_version->setChecked(match_version_);

  ui->checkBox_native->setChecked(native_mode_);

  ui->checkBox_reliable->setChecked(reliable_mode_);

  ui->checkBox_tcp->setChecked(tcp_mode_);

  ui->checkBox_direct->setChecked(direct_mode_);

  const auto& ip_list = allow_ip.split(";");

  std::vector<std::string> setting_ip_list;
  for (const auto& ip : ip_list) {
    setting_ip_list.emplace_back(ip.toStdString());
  }

  for (const auto& address : vlink::Utils::get_all_ipv4_address(false)) {
    QListWidgetItem* item = new QListWidgetItem;
    ui->listWidget->addItem(item);
    item->setData(Qt::UserRole, QString::fromStdString(address));
    item->setData(Qt::ToolTipRole, QString::fromStdString(address));
    QCheckBox* checkbox = new QCheckBox(ui->listWidget);

    if (std::find(setting_ip_list.begin(), setting_ip_list.end(), address) == setting_ip_list.end()) {
      checkbox->setChecked(false);
    } else {
      checkbox->setChecked(true);
    }

    checkbox->setText(QString::fromStdString(address));
    ui->listWidget->setItemWidget(item, checkbox);
  }

  if (!allow_ip.isEmpty()) {
    ui->groupBox_allow->setChecked(true);
  } else {
    ui->groupBox_allow->setChecked(false);
  }

  QRegularExpressionMatch ip_match = g_ip_regex.match(peer_ip);

  if (!ip_match.hasMatch()) {
    peer_ip.clear();
  }

  if (!peer_ip.isEmpty()) {
    ui->lineEdit_peer->setText(peer_ip);
    ui->groupBox_peer->setChecked(true);
  } else {
    ui->lineEdit_peer->setText("0.0.0.0");
    ui->groupBox_peer->setChecked(false);
  }

  if (buf_size > 0) {
    ui->spinBox_buf->setValue(std::max(buf_size, ui->spinBox_buf->minimum()));
    ui->groupBox_buf->setChecked(true);
  } else {
    ui->spinBox_buf->setValue(kDefaultDdsBufSize);
    ui->groupBox_buf->setChecked(false);
  }

  if (mtu_size > 0) {
    ui->spinBox_mtu->setValue(std::max(mtu_size, ui->spinBox_mtu->minimum()));
    ui->groupBox_mtu->setChecked(true);
  } else {
    ui->spinBox_mtu->setValue(kDefaultDdsMtuSize);
    ui->groupBox_mtu->setChecked(false);
  }

  ui->groupBox_allow->setEnabled(!ui->checkBox_native->isChecked());
  ui->groupBox_peer->setEnabled(!ui->checkBox_native->isChecked());

  connect(ui->groupBox_allow, &QGroupBox::clicked, this, [this](bool checked) {
    if (!checked) {
      for (int i = 0; i < ui->listWidget->count(); ++i) {
        auto* item = ui->listWidget->item(i);
        auto* checkbox = qobject_cast<QCheckBox*>(ui->listWidget->itemWidget(item));
        checkbox->setChecked(false);
      }
    }
  });

  connect(ui->groupBox_peer, &QGroupBox::clicked, this, [this](bool checked) {
    if (!checked) {
      ui->lineEdit_peer->setText("0.0.0.0");
    }
  });

  connect(ui->groupBox_buf, &QGroupBox::clicked, this, [this](bool checked) {
    if (!checked) {
      ui->spinBox_buf->setValue(kDefaultDdsBufSize);
    }
  });

  connect(ui->groupBox_mtu, &QGroupBox::clicked, this, [this](bool checked) {
    if (!checked) {
      ui->spinBox_mtu->setValue(kDefaultDdsMtuSize);
    }
  });

  connect(ui->lineEdit_key, &QLineEdit::textEdited, this,
          [this](const QString& text) { ui->toolButton_show->setEnabled(!text.isEmpty()); });

  allow_ip_ = allow_ip.toStdString();
  peer_ip_ = peer_ip.toStdString();
  buf_size_ = buf_size;
  mtu_size_ = mtu_size;

  this->setFocus();
}

SettingsDialog::~SettingsDialog() {
  for (int i = 0; i < ui->listWidget->count(); ++i) {
    auto* item = ui->listWidget->takeItem(i);
    --i;
    auto* checkbox = qobject_cast<QCheckBox*>(ui->listWidget->itemWidget(item));
    delete checkbox;
    delete item;
  }

  delete ui;
  delete validator_ip_;
}

void SettingsDialog::save_profile() {
  domain_id_ = ui->spinBox_domain->value();

  if (ui->radioButton_ddsc->isChecked()) {
    dds_impl_ = "ddsc";
  } else {
    dds_impl_ = "dds";
  }

  security_key_ = ui->lineEdit_key->text().toStdString();

  detect_upgrade_ = ui->checkBox_upgrade->isChecked();

  match_version_ = ui->checkBox_version->isChecked();

  native_mode_ = ui->checkBox_native->isChecked();

  reliable_mode_ = ui->checkBox_reliable->isChecked();

  tcp_mode_ = ui->checkBox_tcp->isChecked();

  direct_mode_ = ui->checkBox_direct->isChecked();

  std::string allow_ip;
  std::string peer_ip;
  int buf_size = 0;
  int mtu_size = 0;

  {
    QSettings settings(QStandardPaths::writableLocation(QStandardPaths::CacheLocation) + "/settings.ini",
                       QSettings::IniFormat);

    settings.beginGroup("SettingsDialog");

    settings.setValue("domain_id", domain_id_);

    settings.setValue("dds_impl", QString::fromStdString(dds_impl_));

    settings.setValue("security_key", ui->lineEdit_key->text().toLocal8Bit().toBase64());

    settings.setValue("detect_upgrade", detect_upgrade_);

    settings.setValue("match_version", match_version_);

    settings.setValue("native_mode", native_mode_);

    settings.setValue("reliable_mode", reliable_mode_);

    settings.setValue("tcp_mode", tcp_mode_);

    settings.setValue("direct_mode", direct_mode_);

    peer_ip = ui->lineEdit_peer->text().toStdString();

    if (peer_ip == "0.0.0.0") {
      peer_ip.clear();
    }

    buf_size = ui->spinBox_buf->value();

    mtu_size = ui->spinBox_mtu->value();

    if (ui->groupBox_allow->isChecked()) {
      for (int i = 0; i < ui->listWidget->count(); ++i) {
        auto* item = ui->listWidget->item(i);
        auto* checkbox = qobject_cast<QCheckBox*>(ui->listWidget->itemWidget(item));
        std::string text = item->data(Qt::UserRole).toString().toStdString();
        if (checkbox->isChecked()) {
          if (allow_ip.empty()) {
            allow_ip = text;
          } else {
            allow_ip += ";" + text;
          }
        }
      }

      settings.setValue("allow_ip", QString::fromStdString(allow_ip));
    } else {
      settings.remove("allow_ip");
    }

    if (ui->groupBox_peer->isChecked()) {
      settings.setValue("peer_ip", QString::fromStdString(peer_ip));
    } else {
      settings.remove("peer_ip");
    }

    if (ui->groupBox_buf->isChecked()) {
      settings.setValue("buf_size", buf_size);
    } else {
      settings.remove("buf_size");
    }

    if (ui->groupBox_mtu->isChecked()) {
      settings.setValue("mtu_size", mtu_size);
    } else {
      settings.remove("mtu_size");
    }

    settings.endGroup();
    settings.sync();
  }

  allow_ip_ = allow_ip;
  peer_ip_ = peer_ip;
  buf_size_ = buf_size;
  mtu_size_ = mtu_size;

  this->accept();
}

// NOLINTEND
