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

#include <QFile>
#include <QObject>
#include <thread>

class IpcChannel : public QObject {
  Q_OBJECT
 public:
  explicit IpcChannel(QObject* parent = nullptr);

  ~IpcChannel();

  void send_timestamp(int64_t timestamp);

 signals:
  void timestamp_changed(int64_t timestamp);

 private:
  std::atomic<bool> quit_flag_{false};
  class QSocketNotifier* notifier_{nullptr};
  void* hstdin_dup_{nullptr};
  void* hstop_event_{nullptr};
  QFile file_;
  std::thread thread_;
};

// NOLINTEND
