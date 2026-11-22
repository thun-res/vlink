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

#include <algorithm>
#include <chrono>
#include <iostream>
#include <thread>

#ifdef _WIN32
#include <Windows.h>
#else
#include <unistd.h>

#include <cerrno>
#endif

#include <QSocketNotifier>

IpcChannel::IpcChannel(QObject* parent) : QObject{parent} {
#ifdef _WIN32
  const HANDLE hstdin = ::GetStdHandle(STD_INPUT_HANDLE);

  if (hstdin == nullptr || hstdin == INVALID_HANDLE_VALUE) {
    return;
  }

  HANDLE duplicated = nullptr;

  if (!::DuplicateHandle(::GetCurrentProcess(), hstdin, ::GetCurrentProcess(), &duplicated, 0, FALSE,
                         DUPLICATE_SAME_ACCESS) ||
      duplicated == nullptr || duplicated == INVALID_HANDLE_VALUE) {
    return;
  }

  hstdin_dup_ = duplicated;
  ::CloseHandle(hstdin);

  thread_ = std::thread([this, pipe_handle = duplicated]() {
    char buffer[64];

    while (!quit_flag_.load(std::memory_order_relaxed)) {
      DWORD available = 0;

      if (!::PeekNamedPipe(pipe_handle, nullptr, 0, nullptr, &available, nullptr)) {
        break;
      }

      if (available == 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        continue;
      }

      DWORD read = 0;
      const DWORD to_read = std::min<DWORD>(available, static_cast<DWORD>(sizeof(buffer)));

      if (!::ReadFile(pipe_handle, buffer, to_read, &read, nullptr) || read == 0) {
        break;
      }

      consume_timestamp_bytes(QByteArray(buffer, static_cast<int>(read)));
    }
  });
#else
  if (!file_.open(STDIN_FILENO, QFile::ReadOnly)) {
    return;
  }

  notifier_ = new QSocketNotifier(file_.handle(), QSocketNotifier::Read, this);
  connect(notifier_, &QSocketNotifier::activated, this, [this]() {
    if (quit_flag_) {
      return;
    }

    char buffer[64];
    int ret = ::read(file_.handle(), buffer, sizeof(buffer));

    if (ret <= 0) {
      if (ret == 0 || (errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK)) {
        notifier_->setEnabled(false);
      }

      return;
    }

    consume_timestamp_bytes(QByteArray(buffer, ret));
  });
#endif
}

IpcChannel::~IpcChannel() {
  quit_flag_.store(true, std::memory_order_relaxed);

  if (file_.isOpen()) {
    file_.close();
  }

  if (thread_.joinable()) {
    thread_.join();
  }

#ifdef _WIN32
  if (hstdin_dup_ != nullptr) {
    ::CloseHandle(hstdin_dup_);
    hstdin_dup_ = nullptr;
  }
#endif
}

void IpcChannel::send_timestamp(int64_t timestamp) { std::cout << timestamp << std::endl; }

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

// NOLINTEND
