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
#include <vlink/base/logger.h>
#include <vlink/base/macros.h>
#include <vlink/base/message_loop.h>

#include <atomic>
#include <memory>
#include <mutex>
#include <rerun.hpp>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "proxy_bridge.h"
#include "rerun_converter.h"
#include "webviz_time_utils.h"

namespace vlink {
namespace webviz {

class RerunServer final : public MessageLoop {
 public:
  enum Mode : uint8_t {
    kSpawn = 0,
    kConnect = 1,
    kServe = 2,
    kSave = 3,
  };

  struct Config final {
    Mode mode{kSpawn};
    std::string address{"rerun+http://127.0.0.1:9876/proxy"};
    std::string bind_ip{"0.0.0.0"};
    uint16_t port{9876};
    std::string save_path;
    std::string name{"vlink-rerun"};
    std::string recording_id;
    std::string config_file;
    std::string proto_dir;
    std::string fbs_dir;
    std::string schema_plugin_path;
    std::string convert_plugin_path;
    std::string convert_plugin_config;
    std::vector<std::string> vlink_msgs;
    std::vector<std::string> whitelist_exact;
    std::vector<std::string> whitelist_patterns;
    std::vector<std::string> blacklist_exact;
    std::vector<std::string> blacklist_patterns;
    std::string spawn_memory_limit{"75%"};
    std::string spawn_server_memory_limit{"1GiB"};
    bool spawn_hide_welcome_screen{false};
    bool spawn_detach_process{true};
    std::string spawn_executable_name{"rerun"};
    std::string spawn_executable_path;
    std::string serve_memory_limit{"1GiB"};
    std::string playback_behavior{"oldest_first"};
    std::string sequence_timeline{"seq"};
    std::string timestamp_timeline{"timestamp"};
    bool use_sequence_timeline{true};
    bool use_timestamp_timeline{true};
    ProxyBridge::Config proxy_config;
  };

  explicit RerunServer(const Config& config);

  ~RerunServer() override;

  bool start();

  void stop();

  [[nodiscard]] size_t get_max_task_count() const override;

 private:
  bool init_rerun();

  bool open_recording(std::shared_ptr<::rerun::RecordingStream>& rec);

  bool reconnect_recording();

  bool init_bridge();

  void flush_recording();

  void probe_recording();

  void apply_timeline_policy(::rerun::RecordingStream& rec) const;

  bool update_bridge_control();

  void on_bridge_connected(bool connected);

  void on_bridge_info(const std::vector<ProxyAPI::Info>& info_list);

  void on_bridge_data(const ProxyAPI::Data& data);

  void on_bridge_time(uint64_t sys_time, uint64_t boot_time);

  bool is_url_allowed(std::string_view url) const;

  static std::string url_to_entity_path(const std::string& url);

  Config config_;
  std::unique_ptr<ProxyBridge> bridge_;
  mutable std::shared_mutex rec_mtx_;
  std::shared_ptr<::rerun::RecordingStream> rec_;
  std::atomic<::rerun::RecordingStream*> rec_raw_{nullptr};
  std::unique_ptr<RerunConverter> rerun_converter_;
  Timer probe_timer_;

  mutable std::shared_mutex info_mtx_;
  std::unordered_map<std::string, ProxyAPI::Info> last_info_map_;
  std::unordered_set<std::string> subscribed_urls_;
  std::atomic<uint64_t> subscribed_urls_generation_{0};

  std::atomic_bool running_{false};
  std::atomic<uint64_t> last_sys_time_ns_{0};
  std::atomic<uint64_t> session_start_sys_time_ns_{0};
  ElapsedTimer bridge_time_elapsed_{ElapsedTimer::kCpuTimestamp, ElapsedTimer::kNano};
  mutable std::mutex bridge_control_mtx_;
  std::string bridge_control_signature_;

  VLINK_DISALLOW_COPY_AND_ASSIGN(RerunServer)
};

}  // namespace webviz
}  // namespace vlink
