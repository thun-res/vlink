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

#include "./extension/trigger_plugin_interface.h"

#include <doctest/doctest.h>

#include <string>
#include <vector>

#include "../common_test.h"

class RecordingPlugin final : public TriggerPluginInterface {
 public:
  bool init(const std::string& config) override {
    init_config = config;
    return init_result;
  }

  void on_start() override { started = true; }

  void on_stop() override { stopped = true; }

  void on_trigger(const TriggerContext& context) override { trigger_reason = context.reason; }

  void on_dump_started(const DumpContext& context) override { started_path = context.path; }

  void on_frame(const Frame& frame, const DumpContext&) override {
    ++frame_count;
    last_frame_url = frame.url;
  }

  void on_dump_finished(const DumpResult& result) override {
    ++finished_count;
    finished_result = result;
  }

  void on_dump_failed(const DumpResult& result) override {
    ++failed_count;
    failed_error = result.error;
  }

  void on_file_rotated(const std::string& path) override { rotated.push_back(path); }

  void flush() override { ++flush_count; }

  bool started{false};
  bool stopped{false};
  std::string trigger_reason;
  std::string started_path;
  int frame_count{0};
  std::string last_frame_url;
  int finished_count{0};
  DumpResult finished_result;
  int failed_count{0};
  std::string failed_error;
  std::vector<std::string> rotated;
  int flush_count{0};
  std::string init_config;
  bool init_result{true};
};

TEST_SUITE("extension-TriggerPluginInterface") {
  TEST_CASE("default hook implementations are no-ops and never throw") {
    struct MinimalPlugin final : public TriggerPluginInterface {
      bool init(const std::string&) override { return true; }

      void on_dump_finished(const DumpResult& result) override { (void)result; }
    };

    MinimalPlugin plugin;
    TriggerPluginInterface::TriggerContext trigger_context;
    TriggerPluginInterface::DumpContext dump_context;
    TriggerPluginInterface::DumpResult dump_result;
    Frame frame;

    CHECK(plugin.init(""));
    CHECK_NOTHROW(plugin.on_start());
    CHECK_NOTHROW(plugin.on_trigger(trigger_context));
    CHECK_NOTHROW(plugin.on_dump_started(dump_context));
    CHECK_NOTHROW(plugin.on_frame(frame, dump_context));
    CHECK_NOTHROW(plugin.on_dump_finished(dump_result));
    CHECK_NOTHROW(plugin.on_dump_failed(dump_result));
    CHECK_NOTHROW(plugin.on_file_rotated("/tmp/x.vdb"));
    CHECK_NOTHROW(plugin.flush());
    CHECK_NOTHROW(plugin.on_stop());
  }

  TEST_CASE("overridden hooks receive their context and payload") {
    RecordingPlugin plugin;

    TriggerPluginInterface::TriggerContext trigger_context;
    trigger_context.reason = "hard-brake";
    trigger_context.pre_ms = 60000;
    trigger_context.post_ms = 5000;

    TriggerPluginInterface::DumpContext dump_context;
    dump_context.reason = "hard-brake";
    dump_context.path = "/tmp/edr.vdb";
    dump_context.url_count = 3;

    TriggerPluginInterface::DumpResult dump_result;
    dump_result.path = "/tmp/edr.vdb";
    dump_result.frame_count = 100;
    dump_result.dropped_count = 2;
    dump_result.byte_count = 4096;
    dump_result.success = true;

    Frame frame;
    frame.url = "dds://camera/front";

    CHECK(plugin.init("{\"upload\":true}"));
    plugin.on_start();
    plugin.on_trigger(trigger_context);
    plugin.on_dump_started(dump_context);
    plugin.on_frame(frame, dump_context);
    plugin.on_frame(frame, dump_context);
    plugin.on_dump_finished(dump_result);
    plugin.on_file_rotated("/tmp/old.vdb");
    plugin.flush();
    plugin.on_stop();

    CHECK(plugin.started);
    CHECK_EQ(plugin.init_config, "{\"upload\":true}");
    CHECK(plugin.stopped);
    CHECK_EQ(plugin.trigger_reason, "hard-brake");
    CHECK_EQ(plugin.started_path, "/tmp/edr.vdb");
    CHECK_EQ(plugin.frame_count, 2);
    CHECK_EQ(plugin.last_frame_url, "dds://camera/front");
    REQUIRE_EQ(plugin.finished_count, 1);
    CHECK_EQ(plugin.finished_result.frame_count, 100);
    CHECK_EQ(plugin.finished_result.dropped_count, 2);
    CHECK_EQ(plugin.finished_result.byte_count, 4096);
    CHECK(plugin.finished_result.success);
    REQUIRE_EQ(plugin.rotated.size(), 1u);
    CHECK_EQ(plugin.rotated[0], "/tmp/old.vdb");
    CHECK_EQ(plugin.flush_count, 1);
  }

  TEST_CASE("init propagates plugin configuration failures") {
    RecordingPlugin plugin;
    plugin.init_result = false;

    CHECK_FALSE(plugin.init("invalid"));
    CHECK_EQ(plugin.init_config, "invalid");
    CHECK_FALSE(plugin.started);
  }

  TEST_CASE("on_dump_failed carries the failure reason") {
    RecordingPlugin plugin;

    TriggerPluginInterface::DumpResult dump_result;
    dump_result.success = false;
    dump_result.error = "unsupported bag suffix";

    plugin.on_dump_failed(dump_result);

    CHECK_EQ(plugin.failed_count, 1);
    CHECK_EQ(plugin.failed_error, "unsupported bag suffix");
    CHECK_FALSE(plugin.finished_result.success);
  }

  TEST_CASE("plugin id resolves to a non-empty interface type name") {
    CHECK_FALSE(TriggerPluginInterface::get_plugin_id().empty());
  }
}

// NOLINTEND
