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

#include "./infodialog.h"

#include <vlink/base/helpers.h>
#include <vlink/base/logger.h>

#include <QCloseEvent>
#include <QFontMetrics>
#include <QHideEvent>
#include <QResizeEvent>
#include <QShowEvent>
#include <QTextBlock>
#include <QTextDocument>
#include <iomanip>
#include <sstream>

#include "./ui_infodialog.h"

InfoDialog::InfoDialog(QWidget* parent) : QDialog(parent), ui(new Ui::InfoDialog) {
  setWindowFlags(Qt::Dialog | Qt::WindowMaximizeButtonHint | Qt::WindowCloseButtonHint | Qt::WindowStaysOnTopHint);

  ui->setupUi(this);

  ui->textEdit->setLineWrapMode(QTextEdit::NoWrap);

  {
    QFont font = ui->textEdit->font();
    font.setFamily("Noto Mono");
    font.setPixelSize(12);
    ui->textEdit->setFont(font);
  }
}

InfoDialog::~InfoDialog() { delete ui; }

void InfoDialog::show_information(const vlink::BagReader::Info& info) {
  // init
  std::stringstream ss;

  ss << "File Name:     " << info.file_name << std::endl;
  ss << "File Size:     " << vlink::Helpers::format_file_size(info.file_size);

  if (info.total_raw_size > 0) {
    ss << " (Raw: " << vlink::Helpers::format_file_size(info.total_raw_size) + ")";
  }

  ss << std::endl;

  ss << "Tag Name:      " << info.tag_name << std::endl;
  ss << "Version:       " << info.version << std::endl;
  ss << "Storage Type:  " << info.storage_type << std::endl;
  ss << "Compression:   " << info.compression_type;

  if (!info.compression_type.empty() && info.compression_type != "None" && info.total_raw_size > 0) {
    auto file_size = info.file_size;

    if (file_size > info.total_raw_size) {
      file_size = info.total_raw_size;
    }

    ss << " (Ratio: " << vlink::Helpers::double_to_string(100.0 * file_size / info.total_raw_size, 0) + "%)";
  }

  ss << std::endl;

  ss << "Process Name:  " << info.process_name << std::endl;

  ss << "Meta Flags:    ";

  std::string flags_str;

  if (info.has_completed) {
    flags_str.append("completed | ");
  }

  if (info.has_idx_elapsed) {
    flags_str.append("idx_elapsed | ");
  }

  if (info.has_idx_url) {
    flags_str.append("idx_url | ");
  }

  if (info.has_schema) {
    flags_str.append("schema | ");
  }

  if (flags_str.size() >= 3) {
    flags_str.pop_back();
    flags_str.pop_back();
    flags_str.pop_back();
  }

  ss << flags_str;
  ss << std::endl;

  ss << "Date Time:     " << info.date_time;

  if (info.timezone == 0) {
    ss << " (UTC)";
  } else {
    if (info.timezone > 0) {
      ss << " (Timezone: +";
    } else {
      ss << " (Timezone: -";
    }

    ss << std::setw(2) << std::setfill('0') << info.timezone / 60;
    ss << ":";
    ss << std::setw(2) << std::setfill('0') << info.timezone % 60;
    ss << std::setfill(' ');
    ss << ":00)";
  }

  ss << std::endl;

  ss << "Duration:      " << vlink::Helpers::format_milliseconds(info.blank_duration, true);
  ss << " ~ ";
  ss << vlink::Helpers::format_milliseconds(info.total_duration, true);
  ss << std::endl;

  ss << "Message Count: " << info.message_count << std::endl;

  if (info.split_count > 0) {
    ss << "Split Count:   " << std::to_string(info.split_count);

    if (info.split_by_time > 0) {
      ss << " (";
      ss << "By time: ";
      ss << vlink::Helpers::double_to_string(info.split_by_time / 1000.0, 2);
      ss << "s)";
    } else if (info.split_by_size > 0) {
      ss << " (";
      ss << "By size: ";
      ss << vlink::Helpers::double_to_string(info.split_by_size / 1024.0 / 1024.0 / 1024.0, 2);
      ss << "GB)";
    }

    ss << std::endl;
  } else {
    ss << "Split Count:   "
       << "---" << std::endl;
  }

  size_t max_url_type_size = 6;
  size_t max_count_type_size = 7;
  size_t max_size_type_size = 7;
  size_t max_freq_type_size = 7;
  size_t max_loss_type_size = 6;
  size_t max_url_size = 10;
  size_t max_ser_type_size = 10;

  for (const auto& meta : info.url_metas) {
    max_url_type_size = std::max(max_url_type_size, meta.url_type.size());
    max_count_type_size = std::max(max_count_type_size, std::to_string(meta.count).size());
    max_size_type_size = std::max(max_size_type_size, vlink::Helpers::format_file_size(meta.size).size());

    std::string freq_str;

    if (meta.freq >= 1000000) {
      freq_str = "999999.99Hz";
    } else {
      freq_str = vlink::Helpers::double_to_string(meta.freq, 2) + "Hz";
    }

    max_freq_type_size = std::max(max_freq_type_size, freq_str.size());

    if (meta.loss > 0 && meta.loss < 0.0001) {
      max_loss_type_size = std::max(max_loss_type_size, std::string("00.0000%").size());
    } else {
      max_loss_type_size = std::max(max_loss_type_size, std::string("00.00%").size());
    }

    max_url_size = std::max(max_url_size, meta.url.size());
    max_ser_type_size = std::max(max_ser_type_size, meta.ser_type.size());
  }

  (void)max_ser_type_size;

  ss << "Meta List:";
  ss << std::string("     ");
  ss << "[Type]";
  ss << std::string(max_url_type_size - 4, ' ');
  ss << "[Count]";
  ss << std::string(max_count_type_size - 5, ' ');
  ss << "[Size]";
  ss << std::string(max_size_type_size - 4, ' ');
  ss << "[Freq]";
  ss << std::string(max_freq_type_size - 4, ' ');
  ss << "[Loss]";
  ss << std::string(max_loss_type_size - 4, ' ');
  ss << "[Url]";
  ss << std::string(max_url_size - 3, ' ');
  ss << "[Ser]";

  ss << std::endl;

  std::string loss_str;
  for (const auto& meta : info.url_metas) {
    ss << std::string("               ");

    ss << meta.url_type;
    ss << std::string(std::max(static_cast<int>(max_url_type_size) - static_cast<int>(meta.url_type.size()) + 2, 2),
                      ' ');

    if (meta.count == 0) {
      ss << "Unknown";
      ss << std::string(std::max(static_cast<int>(max_count_type_size) - 7 + 2, 2), ' ');
    } else {
      ss << meta.count;
      ss << std::string(
          std::max(static_cast<int>(max_count_type_size) - static_cast<int>(std::to_string(meta.count).size()) + 2, 2),
          ' ');
    }

    if (meta.size == 0) {
      ss << "Unknown";
      ss << std::string(std::max(static_cast<int>(max_size_type_size) - 7 + 2, 2), ' ');
    } else {
      auto size_str = vlink::Helpers::format_file_size(meta.size);
      ss << size_str;
      ss << std::string(std::max(static_cast<int>(max_size_type_size) - static_cast<int>(size_str.size()) + 2, 2), ' ');
    }

    if (meta.freq == 0) {
      ss << "Unknown";
      ss << std::string(std::max(static_cast<int>(max_freq_type_size) - 7 + 2, 2), ' ');
    } else {
      std::string freq_str;

      if (meta.freq >= 1000000) {
        freq_str = "999999.99Hz";
      } else {
        freq_str = vlink::Helpers::double_to_string(meta.freq, 2) + "Hz";
      }

      ss << freq_str;
      ss << std::string(std::max(static_cast<int>(max_freq_type_size) - static_cast<int>(freq_str.size()) + 2, 2), ' ');
    }

    if (max_loss_type_size > 6) {
      loss_str = vlink::Helpers::double_to_string(meta.loss * 100, 4) + "%";
    } else {
      loss_str = vlink::Helpers::double_to_string(meta.loss * 100, 2) + "%";
    }

    ss << loss_str;
    ss << std::string(std::max(static_cast<int>(max_loss_type_size) - static_cast<int>(loss_str.size()) + 2, 2), ' ');

    ss << meta.url;
    ss << std::string(std::max(static_cast<int>(max_url_size) - static_cast<int>(meta.url.size()) + 2, 2), ' ');

    ss << meta.ser_type;

    ss << std::endl;
  }

  ui->textEdit->setPlainText(QString::fromUtf8(ss.str().data()));

  // show
  int max_width = 450;
  int max_height = 200;

  QFontMetrics metrics(ui->textEdit->font());

  QTextDocument* doc = ui->textEdit->document();

  QTextBlock block = doc->begin();

  while (block.isValid()) {
    QString block_text = block.text();
    int blockWidth = metrics.horizontalAdvance(block_text);

    if (blockWidth > max_width) {
      max_width = blockWidth;
    }

    block = block.next();
  }

  max_height = metrics.height() * doc->blockCount();

  max_width += 80;
  max_height += 120;

  if (max_width > 1366) {
    max_width = 1366;
  }

  if (max_height > 768) {
    max_height = 768;
  }

  resize(max_width, max_height);

  show();
}

void InfoDialog::showEvent(class QShowEvent* event) { QDialog::showEvent(event); }

void InfoDialog::hideEvent(class QHideEvent* event) { QDialog::hideEvent(event); }

void InfoDialog::closeEvent(class QCloseEvent* event) { QDialog::closeEvent(event); }

void InfoDialog::resizeEvent(class QResizeEvent* event) { QDialog::resizeEvent(event); }

void InfoDialog::on_pushButton_close_clicked() { this->close(); }

// NOLINTEND
