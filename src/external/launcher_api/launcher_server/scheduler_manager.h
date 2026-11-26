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

#pragma once

#include <vlink/base/macros.h>
#include <vlink/base/message_loop.h>
#include <vlink/base/multi_loop.h>
#include <vlink/base/thread_pool.h>
//
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "./launcher_config.h"

namespace vlink {

namespace launcher {

class SchedulerManager final {
 public:
  SchedulerManager();

  ~SchedulerManager();

  bool init(const std::vector<SchedulerConfig>& configs);

  void shutdown(uint32_t timeout_ms = 5000U);

  [[nodiscard]] MessageLoop* acquire_loop(std::string_view group_name, std::string_view unit_name,
                                          int32_t worker_index = -1);

  [[nodiscard]] ThreadPool* acquire_thread_pool(std::string_view group_name) const;

  [[nodiscard]] std::unique_ptr<MessageLoop> detach_dedicated_loop(std::string_view unit_name,
                                                                   MessageLoop* expected_loop = nullptr);

  [[nodiscard]] bool is_dedicated_group(std::string_view group_name) const;

  void mark_loop_poisoned(MessageLoop* loop);

  [[nodiscard]] bool has_group(std::string_view group_name) const;

 private:
  struct GroupRuntime final {
    SchedulerConfig config;
    std::vector<std::unique_ptr<MessageLoop>> loops;
    std::unique_ptr<MultiLoop> multi_loop;
    std::unique_ptr<ThreadPool> thread_pool;
    std::atomic<size_t> next_index{0};
  };

  static std::unique_ptr<MessageLoop> create_configured_loop(const SchedulerConfig& cfg, const std::string& thread_name,
                                                             size_t worker_idx);

  mutable std::mutex mtx_;
  std::unordered_map<std::string, std::unique_ptr<GroupRuntime>> groups_;
  std::unordered_map<std::string, std::unique_ptr<MessageLoop>> dedicated_loops_;
  std::unordered_set<MessageLoop*> poisoned_loops_;
  bool running_{false};

  VLINK_DISALLOW_COPY_AND_ASSIGN(SchedulerManager)
};

}  // namespace launcher

}  // namespace vlink
