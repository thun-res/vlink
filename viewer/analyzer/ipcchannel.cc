/*
 *
 * Copyright (C) 2026 by Thun Lu. All rights reserved.
 *
 * Author: Thun Lu <thun.lu@zohomail.cn>
 *
 *  _    __   __      _           __
 * | |  / /  / /     (_) ____    / /__
 * | | / /  / /     / / / __ \  / //_/
 * | |/ /  / /___  / / / / / / / ,<
 * |___/  /_____/ /_/ /_/ /_/ /_/|_|
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

      if (!ret || read == 0 || read >= static_cast<int>(sizeof(buffer))) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        continue;
      }

      buffer[read] = 0;

      QString timestamp_str(buffer);

      auto list = timestamp_str.split(";");

      if (list.size() < 2) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        continue;
      }

      bool ok = false;
      int64_t timestamp = list.at(list.size() - 2).toLongLong(&ok);

      if (!ok) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        continue;
      }

      emit timestamp_changed(timestamp);
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

            if (ret <= 0 || ret >= static_cast<int>(sizeof(buffer))) {
              std::this_thread::sleep_for(std::chrono::milliseconds(5));
              return;
            }

            buffer[ret] = 0;

            QString timestamp_str(buffer);

            const auto& list = timestamp_str.split(";");

            if (list.size() < 2) {
              std::this_thread::sleep_for(std::chrono::milliseconds(5));
              return;
            }

            bool ok = false;
            int64_t timestamp = list.at(list.size() - 2).toLongLong(&ok);

            if (!ok) {
              std::this_thread::sleep_for(std::chrono::milliseconds(5));
              return;
            }

            emit timestamp_changed(timestamp);
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

    if (ret <= 0 || ret >= sizeof(buffer)) {
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
      return;
    }

    buffer[ret] = 0;

    QString timestamp_str(buffer);

    const auto& list = timestamp_str.split(";");

    if (list.size() < 2) {
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
      return;
    }

    bool ok = false;
    int64_t timestamp = list.at(list.size() - 2).toLongLong(&ok);

    if (!ok) {
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
      return;
    }

    emit timestamp_changed(timestamp);
  });
#endif
#endif
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

void IpcChannel::send_timestamp(int64_t timestamp) { std::cout << std::to_string(timestamp) + ";" << std::endl; }

// NOLINTEND
