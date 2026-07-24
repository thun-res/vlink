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

#include "ipcchannel.h"

#include <vlink/base/logger.h>

#include <chrono>
#include <thread>

#ifdef _WIN32
#include <Windows.h>
#else
#include <unistd.h>
#endif

#include <QSocketNotifier>

IpcChannel::IpcChannel(QObject* parent) : QObject{parent} {
#ifdef _WIN32
  thread_ = std::thread([this]() {
    bool ret = true;
    char buffer[64];
    DWORD read;

    const HANDLE hstdin = ::GetStdHandle(STD_INPUT_HANDLE);

    if (hstdin == INVALID_HANDLE_VALUE) {
      return;
    }

    ::DuplicateHandle(GetCurrentProcess(), hstdin, GetCurrentProcess(), &hstdin_dup_, 0, false, DUPLICATE_SAME_ACCESS);

    ::CloseHandle(hstdin);

    while (!quit_flag_ && ret) {
      ret = ::ReadFile(hstdin_dup_, buffer, sizeof(buffer), &read, nullptr);

      if (!ret || read == 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        continue;
      }

      consume_timestamp_bytes(QByteArray(buffer, static_cast<int>(read)));
    }
  });
#else
  (void)file_.open(0, QFile::WriteOnly);

  notifier_ = new QSocketNotifier(file_.handle(), QSocketNotifier::Read, this);

#if QT_VERSION >= QT_VERSION_CHECK(5, 15, 0)
  connect(notifier_, &QSocketNotifier::activated, this,
          [this](QSocketDescriptor socket, QSocketNotifier::Type activation_event) {
            if (quit_flag_) {
              return;
            }

            if (!socket.isValid() || activation_event != QSocketNotifier::Read) {
              return;
            }

            char buffer[64];

            int ret = ::read(file_.handle(), buffer, sizeof(buffer));

            if (ret <= 0) {
              std::this_thread::sleep_for(std::chrono::milliseconds(5));
              return;
            }

            consume_timestamp_bytes(QByteArray(buffer, ret));
          });
#else
  connect(notifier_, &QSocketNotifier::activated, this, [this](int socket) {
    if (quit_flag_) {
      return;
    }

    if (socket != file_.handle()) {
      return;
    }

    char buffer[64];

    int ret = ::read(file_.handle(), buffer, sizeof(buffer));

    if (ret <= 0) {
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
      return;
    }

    consume_timestamp_bytes(QByteArray(buffer, ret));
  });
#endif
#endif
}

void IpcChannel::consume_timestamp_bytes(const QByteArray& bytes) {
  if (bytes.isEmpty()) {
    return;
  }

  timestamp_buffer_.append(bytes);

  while (true) {
    const auto newline_pos = timestamp_buffer_.indexOf('\n');

    if (newline_pos < 0) {
      break;
    }

    const auto frame = timestamp_buffer_.left(newline_pos).trimmed();
    timestamp_buffer_.remove(0, newline_pos + 1);

    if (frame.isEmpty()) {
      continue;
    }

    bool ok = false;
    const int64_t timestamp = QString::fromLatin1(frame).toLongLong(&ok);

    if (ok) {
      emit timestamp_changed(timestamp);
    }
  }

  if VUNLIKELY (timestamp_buffer_.size() > 128) {
    timestamp_buffer_.clear();
  }
}

IpcChannel::~IpcChannel() {
  quit_flag_ = true;

  if (file_.isOpen()) {
    file_.close();
  }

#ifdef _WIN32
  ::CloseHandle(hstdin_dup_);
  hstdin_dup_ = nullptr;
#endif

  if (thread_.joinable()) {
    thread_.join();
  }
}

void IpcChannel::send_timestamp(int64_t timestamp) { std::cout << timestamp << std::endl; }

// NOLINTEND
