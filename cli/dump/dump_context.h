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

#include <vlink/base/elapsed_timer.h>
#include <vlink/base/plugin.h>
#include <vlink/extension/bag_reader.h>
#include <vlink/extension/discovery_viewer.h>
#include <vlink/vlink.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "dump_expr.h"
#include "dump_types.h"

namespace vlink::dump {

// NOLINTNEXTLINE(clang-analyzer-optin.performance.Padding)
struct DumpContext final {
  static DumpContext& get();

  std::atomic_bool has_quit{false};
  std::atomic_bool is_broken{false};
  std::atomic_bool quit_flag{false};
  std::atomic_bool data_has_changed{false};
  std::atomic_bool callback_has_set{false};

  bool quiet_flag{false};
  bool detail_flag{false};
  vlink::ConditionVariable quit_cv;
  std::mutex print_mtx;
  std::thread print_thread;
  bool dump_for_bag{false};
  DumpType dump_type{DumpType::kConsole};
  int64_t begin_time{0};
  int64_t end_time{0};
  int max_count{0};
  double max_hz{0};

  vlink::Plugin bag_plugin;
  std::shared_ptr<vlink::BagPluginInterface> bag_plugin_interface;
  std::string bag_plugin_name;

  std::shared_ptr<vlink::DiscoveryViewer> discovery_viewer;
  std::shared_ptr<vlink::BagReader> bag_player;
  vlink::BagReader::Config bag_config;

  std::unordered_map<std::string, std::shared_ptr<RawSub>> sub_urls;
  std::mutex sub_urls_mtx;

  DumpCallback dump_callback;
  std::mutex dump_callback_mtx;

  vlink::ElapsedTimer main_elapsed_timer{vlink::ElapsedTimer::kMicro};
  std::vector<DumpRecord> cache_buffer;
  std::mutex cache_mtx;

  std::vector<std::string> field_specs;
  std::vector<std::vector<std::string>> field_paths;
  std::atomic<int64_t> output_count{0};
  std::atomic<int64_t> last_output_us{0};
  std::vector<std::string> expr_strings;
  ExprContext expr_ctx;
  std::mutex expr_mtx;

  bool invoke_callback(int64_t timestamp, const std::string& url, const std::string& ser, vlink::SchemaType schema_type,
                       const vlink::Bytes& bytes);

  bool prepare_bag_plugin();

  void bind_bag_plugin(const std::shared_ptr<vlink::BagReader>& reader);

  void request_stop();
};

}  // namespace vlink::dump
