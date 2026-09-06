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

#include <nanobind/stl/shared_ptr.h>
#include <nanobind/stl/string.h>
#include <nanobind/stl/unordered_map.h>
#include <nanobind/stl/unordered_set.h>
#include <nanobind/stl/vector.h>
#include <vlink/base/plugin.h>
#include <vlink/base/timer.h>
#include <vlink/extension/bag_plugin_interface.h>
#include <vlink/extension/bag_reader.h>
#include <vlink/extension/bag_writer.h>
#include <vlink/extension/trigger_plugin_interface.h>
#include <vlink/extension/trigger_recorder.h>

#include <stdexcept>

#include "buffer.h"
#include "callbacks.h"

namespace vlink::python {

using namespace nb::literals;  // NOLINT

static const void* python_bag_writer_schema_callback_kind() noexcept {
  static const char kKind{};
  return &kKind;
}

static const void* python_bag_writer_split_callback_kind() noexcept {
  static const char kKind{};
  return &kKind;
}

static const void* python_bag_reader_output_callback_kind() noexcept {
  static const char kKind{};
  return &kKind;
}

template <typename LoopT, typename Cleanup>
static nb::object cast_shared_message_loop(std::shared_ptr<LoopT> owner, Cleanup cleanup) {
  if (!owner) {
    return nb::none();
  }

  nb::object instance = nb::cast(owner);
  std::weak_ptr<LoopT> weak_owner = owner;
  auto& hooks = python_pre_destroy_hooks();
  auto [iter, inserted] = hooks.insert(instance.ptr());

  if (!inserted) {
    return instance;
  }

  PyObject* key = instance.ptr();
  LoopT* native = owner.get();

  try {
    nb::module_::import_("weakref").attr("finalize")(
        instance, nb::cpp_function([key, native, weak_owner, cleanup = std::move(cleanup)]() noexcept {
          python_pre_destroy_hooks().erase(key);
          auto retained = weak_owner.lock();

          if (!retained || retained.use_count() != 2 || !Py_IsInitialized()) {
            return;
          }

          python_native_finalizing().insert(native);
          try {
            nb::gil_scoped_release release;
            cleanup(*native);
          } catch (...) {
          }
          python_native_finalizing().erase(native);
        }));
  } catch (...) {
    hooks.erase(iter);
    throw;
  }

  return instance;
}

template <typename BagT>
static void bind_python_bag_interface(BagT& self, const std::shared_ptr<vlink::BagPluginInterface>& plugin) {
  if VUNLIKELY (is_in_python_owner_callback(&self)) {
    throw std::runtime_error("Bag plugins cannot be replaced from that object's active Python callback");
  }

  nb::gil_scoped_release release;
  self.bind_bag_interface(plugin);
}

void bind_bag(nb::module_& m) {
  nb::class_<vlink::BagWriter> bw(m, "BagWriter", "Message recorder", nb::is_weak_referenceable());
  nb::enum_<vlink::BagWriter::CompressType>(bw, "CompressType")
      .value("NONE", vlink::BagWriter::kCompressNone)
      .value("AUTO", vlink::BagWriter::kCompressAuto)
      .value("ZSTD", vlink::BagWriter::kCompressZstd)
      .value("LZ4", vlink::BagWriter::kCompressLz4)
      .value("LZAV", vlink::BagWriter::kCompressLzav);
  nb::class_<vlink::BagWriter::Config>(bw, "Config")
      .def(nb::init<>())
      .def_rw("tag_name", &vlink::BagWriter::Config::tag_name)
      .def_rw("compress", &vlink::BagWriter::Config::compress)
      .def_rw("wal_mode", &vlink::BagWriter::Config::wal_mode)
      .def_rw("enable_limit", &vlink::BagWriter::Config::enable_limit)
      .def_rw("split_name_by_time", &vlink::BagWriter::Config::split_name_by_time)
      .def_rw("sync_mode", &vlink::BagWriter::Config::sync_mode)
      .def_rw("optimize_on_exit", &vlink::BagWriter::Config::optimize_on_exit)
      .def_rw("max_row_count", &vlink::BagWriter::Config::max_row_count)
      .def_rw("max_bytes_size", &vlink::BagWriter::Config::max_bytes_size)
      .def_rw("split_by_size", &vlink::BagWriter::Config::split_by_size)
      .def_rw("split_by_time", &vlink::BagWriter::Config::split_by_time)
      .def_rw("max_split_count", &vlink::BagWriter::Config::max_split_count)
      .def_rw("begin_time", &vlink::BagWriter::Config::begin_time)
      .def_rw("cache_size", &vlink::BagWriter::Config::cache_size)
      .def_rw("compress_start_size", &vlink::BagWriter::Config::compress_start_size)
      .def_rw("compress_level", &vlink::BagWriter::Config::compress_level)
      .def_rw("max_task_depth", &vlink::BagWriter::Config::max_task_depth)
      .def_rw("max_memory_size", &vlink::BagWriter::Config::max_memory_size)
      .def_rw("start_timestamp", &vlink::BagWriter::Config::start_timestamp)
      .def_rw("ignore_compress_urls", &vlink::BagWriter::Config::ignore_compress_urls);
  bw.def_static(
        "create",
        [](const std::string& path, const vlink::BagWriter::Config& cfg) {
          return cast_shared_message_loop(vlink::BagWriter::create(path, cfg), [](vlink::BagWriter& writer) {
            writer.wait_for_idle(vlink::Timer::kInfinite, false);
            writer.quit(true);
            writer.wait_for_quit(vlink::Timer::kInfinite, false);
            writer.close();
          });
        },
        "path"_a, "config"_a = vlink::BagWriter::Config())
      .def_static(
          "filter_get",
          [](const std::string& path) {
            return cast_shared_message_loop(vlink::BagWriter::filter_get(path), [](vlink::BagWriter& writer) {
              writer.wait_for_idle(vlink::Timer::kInfinite, false);
              writer.quit(true);
              writer.wait_for_quit(vlink::Timer::kInfinite, false);
              writer.close();
            });
          },
          "path"_a)
      .def_static("global_get", &vlink::BagWriter::global_get, nb::rv_policy::reference)
      .def("bind_bag_interface", &bind_python_bag_interface<vlink::BagWriter>, "bag_interface"_a)
      .def("clear_bag_interface", [](vlink::BagWriter& self) { bind_python_bag_interface(self, {}); })
      .def(
          "push",
          [](vlink::BagWriter& self, const vlink::Frame& frame) {
            vlink::Frame owned = frame;
            nb::gil_scoped_release release;
            return self.push(owned);
          },
          "frame"_a,
          "Record a frame. For direct asynchronous writes without a bag plugin, a non-negative result means "
          "the queue accepted the frame; a negative result means it was rejected without evicting an accepted write.")
      .def(
          "register_schema_callback",
          [](nb::object instance, nb::callable callback) {
            auto& self = nb::cast<vlink::BagWriter&>(instance);
            if VUNLIKELY (is_in_python_owner_callback(&self, python_bag_writer_schema_callback_kind())) {
              throw std::runtime_error(
                  "BagWriter callbacks cannot be replaced from that writer's active Python callback");
            }

            auto activity = std::make_shared<PythonCallbackActivity>();
            auto cb = std::make_shared<GilSafePyFunction>(std::move(callback));
            nb::gil_scoped_release release;
            self.register_schema_callback(
                [native = &self, activity, cb](const std::string& ser_type, vlink::SchemaType schema_type) {
                  vlink::SchemaData schema;
                  invoke_owned_python_callback(
                      native, activity, "vlink::BagWriter.register_schema_callback",
                      [&]() {
                        nb::object result = cb->fn(ser_type, schema_type);

                        if VLIKELY (!result.is_none()) {
                          schema = nb::cast<vlink::SchemaData>(result);
                        }
                      },
                      python_bag_writer_schema_callback_kind());
                  return schema;
                });
          },
          "callback"_a)
      .def(
          "push_schema",
          [](vlink::BagWriter& self, const vlink::SchemaData& schema_data) {
            // NOLINTNEXTLINE(performance-unnecessary-copy-initialization)
            auto schema_copy = schema_data;
            nb::gil_scoped_release release;
            return self.push_schema(schema_copy);
          },
          "schema_data"_a)
      .def(
          "register_split_callback",
          [](nb::object instance, nb::callable callback, bool before) {
            auto& self = nb::cast<vlink::BagWriter&>(instance);
            if VUNLIKELY (is_in_python_owner_callback(&self, python_bag_writer_split_callback_kind())) {
              throw std::runtime_error(
                  "BagWriter callbacks cannot be replaced from that writer's active Python callback");
            }

            auto activity = std::make_shared<PythonCallbackActivity>();
            auto cb = std::make_shared<GilSafePyFunction>(std::move(callback));
            nb::gil_scoped_release release;
            self.register_split_callback(
                [native = &self, activity, cb](int idx, const std::string& file_name) {
                  invoke_owned_python_callback(
                      native, activity, "vlink::BagWriter.register_split_callback", [&]() { cb->fn(idx, file_name); },
                      python_bag_writer_split_callback_kind());
                },
                before);
          },
          "callback"_a, "before"_a = false)
      .def("is_dumping", &vlink::BagWriter::is_dumping)
      .def("is_split_mode", &vlink::BagWriter::is_split_mode)
      .def("get_split_index", &vlink::BagWriter::get_split_index)
      .def("set_url_loss", &vlink::BagWriter::set_url_loss, "url"_a, "loss"_a)
      .def("close",
           [](vlink::BagWriter& self) {
             nb::gil_scoped_release release;
             self.close();
           })
      .def("fail", &vlink::BagWriter::fail)
      .def("clear", &vlink::BagWriter::clear)
      .def(
          "wait_for_idle",
          [](vlink::BagWriter& self, int timeout_ms, bool check) {
            nb::gil_scoped_release release;
            return self.wait_for_idle(timeout_ms, check);
          },
          "timeout_ms"_a = -1, "check"_a = true)
      .def("get_task_count", &vlink::BagWriter::get_task_count)
      .def("__bool__", [](const vlink::BagWriter& self) { return static_cast<bool>(self); })
      .def(
          "__lshift__",
          [](vlink::BagWriter& self, const vlink::Frame& frame) -> vlink::BagWriter& {
            vlink::Frame owned = frame;
            nb::gil_scoped_release release;
            self << owned;
            return self;
          },
          "frame"_a, nb::rv_policy::reference_internal)
      .def(
          "__lshift__",
          [](vlink::BagWriter& self, const vlink::SchemaData& schema_data) -> vlink::BagWriter& {
            // NOLINTNEXTLINE(performance-unnecessary-copy-initialization)
            auto schema_copy = schema_data;
            nb::gil_scoped_release release;
            self << schema_copy;
            return self;
          },
          "schema_data"_a, nb::rv_policy::reference_internal)
      .def("async_run",
           [](vlink::BagWriter& self) {
             nb::gil_scoped_release release;
             return self.async_run();
           })
      .def(
          "quit",
          [](vlink::BagWriter& self, bool force) {
            nb::gil_scoped_release release;
            return self.quit(force);
          },
          "force"_a = false)
      .def(
          "wait_for_quit",
          [](vlink::BagWriter& self, int timeout_ms) {
            nb::gil_scoped_release release;
            return self.wait_for_quit(timeout_ms);
          },
          "timeout_ms"_a = -1)
      .def("is_running", &vlink::BagWriter::is_running)
      .def("__repr__", [](const vlink::BagWriter& self) {
        return std::string("BagWriter(running=") + (self.is_running() ? "True" : "False") + ")";
      });

  nb::class_<vlink::BagPluginInterface>(
      m, "BagPluginInterface",
      "Opaque bag-plugin interface returned by Plugin.load_bag_plugin(); lifecycle hooks such as "
      "on_reset() and flush() are invoked by the C++ host");

  nb::class_<vlink::TriggerPluginInterface>(m, "TriggerPluginInterface",
                                            "Opaque trigger-plugin interface returned by Plugin.load_trigger_plugin()");

  nb::class_<vlink::Plugin>(m, "Plugin", "Host-side shared-library plugin loader")
      .def(nb::init<>())
      .def(
          "load_bag_plugin",
          [](vlink::Plugin& self, const std::string& lib_name, const std::string& dir_name) {
            return self.load<vlink::BagPluginInterface>(lib_name, 2, 0, dir_name);
          },
          "lib_name"_a, "dir_name"_a = "",
          "Load a BagPluginInterface 2.0 implementation; return None when loading fails.")
      .def(
          "load_trigger_plugin",
          [](vlink::Plugin& self, const std::string& lib_name, const std::string& config,
             const std::string& dir_name) -> std::shared_ptr<vlink::TriggerPluginInterface> {
            auto plugin = self.load<vlink::TriggerPluginInterface>(lib_name, 2, 0, dir_name);

            if (!plugin || !plugin->init(config)) {
              return nullptr;
            }

            return plugin;
          },
          "lib_name"_a, "config"_a = "", "dir_name"_a = "",
          "Load a TriggerPluginInterface ABI 2.0 implementation and call init(config); return None when loading or "
          "init fails.");

  nb::class_<vlink::TriggerRecorder> tr(m, "TriggerRecorder", "Trigger-based event-data recorder");
  nb::enum_<vlink::TriggerRecorder::OverflowPolicy>(tr, "OverflowPolicy")
      .value("CoverOldest", vlink::TriggerRecorder::kCoverOldest)
      .value("DropNewest", vlink::TriggerRecorder::kDropNewest);
  nb::enum_<vlink::TriggerRecorder::FileType>(tr, "FileType")
      .value("Vdb", vlink::TriggerRecorder::kVdb)
      .value("Vcap", vlink::TriggerRecorder::kVcap);
  nb::class_<vlink::TriggerRecorder::UrlConfig>(tr, "UrlConfig")
      .def(nb::init<>())
      .def_rw("pre_ms", &vlink::TriggerRecorder::UrlConfig::pre_ms)
      .def_rw("post_ms", &vlink::TriggerRecorder::UrlConfig::post_ms)
      .def_rw("max_packet_size", &vlink::TriggerRecorder::UrlConfig::max_packet_size)
      .def_rw("max_size", &vlink::TriggerRecorder::UrlConfig::max_size)
      .def_rw("only_front", &vlink::TriggerRecorder::UrlConfig::only_front)
      .def_rw("only_back", &vlink::TriggerRecorder::UrlConfig::only_back);
  nb::class_<vlink::TriggerRecorder::Config>(tr, "Config")
      .def(nb::init<>())
      .def_rw("dump_dir", &vlink::TriggerRecorder::Config::dump_dir)
      .def_rw("file_type", &vlink::TriggerRecorder::Config::file_type)
      .def_rw("default_pre_ms", &vlink::TriggerRecorder::Config::default_pre_ms)
      .def_rw("default_post_ms", &vlink::TriggerRecorder::Config::default_post_ms)
      .def_rw("default_max_packet_size", &vlink::TriggerRecorder::Config::default_max_packet_size)
      .def_rw("default_max_size", &vlink::TriggerRecorder::Config::default_max_size)
      .def_rw("max_cache_size", &vlink::TriggerRecorder::Config::max_cache_size)
      .def_rw("retention_guard_ms", &vlink::TriggerRecorder::Config::retention_guard_ms)
      .def_rw("max_dump_file_count", &vlink::TriggerRecorder::Config::max_dump_file_count)
      .def_rw("enable_compress", &vlink::TriggerRecorder::Config::enable_compress)
      .def_rw("busy_skip_data", &vlink::TriggerRecorder::Config::busy_skip_data)
      .def_rw("destroy_on_offline", &vlink::TriggerRecorder::Config::destroy_on_offline)
      .def_rw("overflow", &vlink::TriggerRecorder::Config::overflow)
      .def_rw("sleep_interval", &vlink::TriggerRecorder::Config::sleep_interval)
      .def_rw("sleep_time_ms", &vlink::TriggerRecorder::Config::sleep_time_ms)
      .def_rw("discovery_filter", &vlink::TriggerRecorder::Config::discovery_filter)
      .def_rw("whitelist", &vlink::TriggerRecorder::Config::whitelist)
      .def_rw("blacklist", &vlink::TriggerRecorder::Config::blacklist)
      .def_rw("url_overrides", &vlink::TriggerRecorder::Config::url_overrides);
  nb::class_<vlink::TriggerRecorder::TriggerParams>(tr, "TriggerParams")
      .def(nb::init<>())
      .def_rw("reason", &vlink::TriggerRecorder::TriggerParams::reason)
      .def_rw("name_hint", &vlink::TriggerRecorder::TriggerParams::name_hint)
      .def_rw("out_file", &vlink::TriggerRecorder::TriggerParams::out_file)
      .def_rw("pre_ms", &vlink::TriggerRecorder::TriggerParams::pre_ms)
      .def_rw("post_ms", &vlink::TriggerRecorder::TriggerParams::post_ms)
      .def_rw("whitelist", &vlink::TriggerRecorder::TriggerParams::whitelist)
      .def_rw("blacklist", &vlink::TriggerRecorder::TriggerParams::blacklist)
      .def_rw("filter_str", &vlink::TriggerRecorder::TriggerParams::filter_str)
      .def_rw("black_mode", &vlink::TriggerRecorder::TriggerParams::black_mode);
  tr.def(nb::new_([](const vlink::TriggerRecorder::Config& config) {
           return new vlink::TriggerRecorder(config, [](const std::string& url, vlink::InitType type) {
             return vlink::TriggerRecorder::RawSub::create_shared(url, type);
           });
         }),
         "config"_a)
      .def("async_run",
           [](vlink::TriggerRecorder& self) {
             nb::gil_scoped_release release;
             const bool started = self.async_run();

             if (started) {
               self.invoke_task([]() {}).wait();
             }

             return started;
           })
      .def(
          "quit",
          [](vlink::TriggerRecorder& self, bool force) {
            nb::gil_scoped_release release;
            return self.quit(force);
          },
          "force"_a = false)
      .def(
          "wait_for_quit",
          [](vlink::TriggerRecorder& self, int timeout_ms) {
            nb::gil_scoped_release release;
            return self.wait_for_quit(timeout_ms);
          },
          "timeout_ms"_a = vlink::Timer::kInfinite)
      .def(
          "dump",
          [](vlink::TriggerRecorder& self, vlink::TriggerRecorder::TriggerParams params) {
            nb::gil_scoped_release release;
            return self.dump(params);
          },
          "params"_a = vlink::TriggerRecorder::TriggerParams())
      .def("bind_bag_interface", &vlink::TriggerRecorder::bind_bag_interface, "bag_interface"_a,
           "Bind a BagPluginInterface previously loaded by the host Plugin instance.")
      .def("clear_bag_interface", &vlink::TriggerRecorder::clear_bag_interface)
      .def("bind_trigger_interface", &vlink::TriggerRecorder::bind_trigger_interface, "trigger_interface"_a,
           "Bind a TriggerPluginInterface previously loaded by the host Plugin instance.")
      .def("clear_trigger_interface", &vlink::TriggerRecorder::clear_trigger_interface)
      .def("is_dumping", &vlink::TriggerRecorder::is_dumping)
      .def("is_running", &vlink::TriggerRecorder::is_running)
      .def("__repr__", [](const vlink::TriggerRecorder& self) {
        return std::string("TriggerRecorder(running=") + (self.is_running() ? "True" : "False") + ")";
      });

  nb::class_<vlink::BagReader> br(m, "BagReader", "Message playback", nb::is_weak_referenceable());
  nb::enum_<vlink::BagReader::Status>(br, "Status")
      .value("Stopped", vlink::BagReader::kStopped)
      .value("Paused", vlink::BagReader::kPaused)
      .value("Playing", vlink::BagReader::kPlaying);
  nb::class_<vlink::BagReader::Info::UrlMeta>(br, "UrlMeta")
      .def_ro("valid", &vlink::BagReader::Info::UrlMeta::valid)
      .def_ro("index", &vlink::BagReader::Info::UrlMeta::index)
      .def_ro("url", &vlink::BagReader::Info::UrlMeta::url)
      .def_ro("url_type", &vlink::BagReader::Info::UrlMeta::url_type)
      .def_ro("action_type", &vlink::BagReader::Info::UrlMeta::action_type)
      .def_ro("ser_type", &vlink::BagReader::Info::UrlMeta::ser_type)
      .def_ro("schema_type", &vlink::BagReader::Info::UrlMeta::schema_type)
      .def_ro("count", &vlink::BagReader::Info::UrlMeta::count)
      .def_ro("size", &vlink::BagReader::Info::UrlMeta::size)
      .def_ro("freq", &vlink::BagReader::Info::UrlMeta::freq)
      .def_ro("loss", &vlink::BagReader::Info::UrlMeta::loss)
      .def("__repr__", [](const vlink::BagReader::Info::UrlMeta& meta) {
        return "UrlMeta(url='" + meta.url + "', count=" + std::to_string(meta.count) + ")";
      });
  nb::class_<vlink::BagReader::Info>(br, "Info")
      .def_ro("file_name", &vlink::BagReader::Info::file_name)
      .def_ro("tag_name", &vlink::BagReader::Info::tag_name)
      .def_ro("version", &vlink::BagReader::Info::version)
      .def_ro("storage_type", &vlink::BagReader::Info::storage_type)
      .def_ro("compression_type", &vlink::BagReader::Info::compression_type)
      .def_ro("time_accuracy", &vlink::BagReader::Info::time_accuracy)
      .def_ro("process_name", &vlink::BagReader::Info::process_name)
      .def_ro("date_time", &vlink::BagReader::Info::date_time)
      .def_ro("has_completed", &vlink::BagReader::Info::has_completed)
      .def_ro("has_idx_elapsed", &vlink::BagReader::Info::has_idx_elapsed)
      .def_ro("has_idx_url", &vlink::BagReader::Info::has_idx_url)
      .def_ro("has_schema", &vlink::BagReader::Info::has_schema)
      .def_ro("timezone", &vlink::BagReader::Info::timezone)
      .def_ro("start_timestamp", &vlink::BagReader::Info::start_timestamp)
      .def_ro("blank_duration", &vlink::BagReader::Info::blank_duration)
      .def_ro("total_duration", &vlink::BagReader::Info::total_duration)
      .def_ro("file_size", &vlink::BagReader::Info::file_size)
      .def_ro("total_raw_size", &vlink::BagReader::Info::total_raw_size)
      .def_ro("message_count", &vlink::BagReader::Info::message_count)
      .def_ro("split_count", &vlink::BagReader::Info::split_count)
      .def_ro("split_by_size", &vlink::BagReader::Info::split_by_size)
      .def_ro("split_by_time", &vlink::BagReader::Info::split_by_time)
      .def_ro("url_metas", &vlink::BagReader::Info::url_metas)
      .def("__repr__", [](const vlink::BagReader::Info& info) {
        return "BagInfo(file='" + info.file_name + "', messages=" + std::to_string(info.message_count) +
               ", duration=" + std::to_string(info.total_duration) + "ms)";
      });
  nb::class_<vlink::BagReader::Config>(br, "Config")
      .def(nb::init<>())
      .def_rw("begin_time", &vlink::BagReader::Config::begin_time)
      .def_rw("end_time", &vlink::BagReader::Config::end_time)
      .def_rw("times", &vlink::BagReader::Config::times)
      .def_rw("rate", &vlink::BagReader::Config::rate)
      .def_rw("skip_blank", &vlink::BagReader::Config::skip_blank)
      .def_rw("force_delay", &vlink::BagReader::Config::force_delay)
      .def_rw("auto_pause", &vlink::BagReader::Config::auto_pause)
      .def_rw("auto_quit", &vlink::BagReader::Config::auto_quit)
      .def_rw("filter_urls", &vlink::BagReader::Config::filter_urls);
  br.attr("INFINITE") = vlink::BagReader::kInfinite;
  br.def_static(
        "create",
        [](const std::string& path, bool read_only, bool try_to_fix) {
          return cast_shared_message_loop(vlink::BagReader::create(path, read_only, try_to_fix),
                                          [](vlink::BagReader& reader) {
                                            reader.stop();
                                            reader.quit(true);
                                            reader.wait_for_quit(vlink::Timer::kInfinite, false);
                                          });
        },
        "path"_a, "read_only"_a = true, "try_to_fix"_a = false)
      .def("bind_bag_interface", &bind_python_bag_interface<vlink::BagReader>, "bag_interface"_a)
      .def("clear_bag_interface", [](vlink::BagReader& self) { bind_python_bag_interface(self, {}); })
      .def(
          "register_output_callback",
          [](nb::object instance, nb::callable callback) {
            auto& self = nb::cast<vlink::BagReader&>(instance);
            if VUNLIKELY (is_in_python_owner_callback(&self, python_bag_reader_output_callback_kind())) {
              throw std::runtime_error(
                  "BagReader output callback cannot be replaced from that reader's active Python callback");
            }

            auto activity = std::make_shared<PythonCallbackActivity>();
            auto cb = std::make_shared<GilSafePyFunction>(std::move(callback));
            nb::gil_scoped_release release;
            self.register_output_callback([native = &self, activity, cb](const vlink::Frame& frame) {
              invoke_owned_python_callback(
                  native, activity, "vlink::BagReader.register_output_callback",
                  [&]() { cb->fn(nb::cast(vlink::Frame(frame))); }, python_bag_reader_output_callback_kind());
            });
          },
          "callback"_a)
      .def(
          "register_status_callback",
          [](nb::object instance, nb::callable callback) {
            auto& self = nb::cast<vlink::BagReader&>(instance);
            auto activity = std::make_shared<PythonCallbackActivity>();
            auto cb = std::make_shared<GilSafePyFunction>(std::move(callback));
            self.register_status_callback([native = &self, activity, cb](vlink::BagReader::Status status) {
              invoke_owned_python_callback(native, activity, "vlink::BagReader.register_status_callback",
                                           [&]() { cb->fn(status); });
            });
          },
          "callback"_a)
      .def(
          "register_ready_callback",
          [](nb::object instance, nb::callable callback) {
            auto& self = nb::cast<vlink::BagReader&>(instance);
            auto activity = std::make_shared<PythonCallbackActivity>();
            auto cb = std::make_shared<GilSafePyFunction>(std::move(callback));
            self.register_ready_callback([native = &self, activity, cb]() {
              invoke_owned_python_callback(native, activity, "vlink::BagReader.register_ready_callback",
                                           [&]() { cb->fn(); });
            });
          },
          "callback"_a)
      .def(
          "register_finish_callback",
          [](nb::object instance, nb::callable callback) {
            auto& self = nb::cast<vlink::BagReader&>(instance);
            auto activity = std::make_shared<PythonCallbackActivity>();
            auto cb = std::make_shared<GilSafePyFunction>(std::move(callback));
            self.register_finish_callback([native = &self, activity, cb](bool interrupted) {
              invoke_owned_python_callback(native, activity, "vlink::BagReader.register_finish_callback",
                                           [&]() { cb->fn(interrupted); });
            });
          },
          "callback"_a)
      .def("play", &vlink::BagReader::play, "config"_a = vlink::BagReader::Config())
      .def("stop", &vlink::BagReader::stop)
      .def("pause", &vlink::BagReader::pause)
      .def("resume", &vlink::BagReader::resume)
      .def("pause_to_next", &vlink::BagReader::pause_to_next)
      .def(
          "jump",
          [](vlink::BagReader& self, int64_t begin_time, double rate, int times, bool force_to_play) {
            nb::gil_scoped_release release;
            self.jump(begin_time, rate, times, force_to_play);
          },
          "begin_time"_a, "rate"_a = 1.0, "times"_a = 1, "force_to_play"_a = false)
      .def("check",
           [](vlink::BagReader& self) {
             auto future = self.check();
             nb::gil_scoped_release release;
             return future.get();
           })
      .def("reindex",
           [](vlink::BagReader& self) {
             auto future = self.reindex();
             nb::gil_scoped_release release;
             return future.get();
           })
      .def(
          "fix",
          [](vlink::BagReader& self, bool rebuild) {
            auto future = self.fix(rebuild);
            nb::gil_scoped_release release;
            return future.get();
          },
          "rebuild"_a = false)
      .def("tag", &vlink::BagReader::tag, "tag_name"_a)
      .def("get_timestamp", &vlink::BagReader::get_timestamp)
      .def("get_real_timestamp", &vlink::BagReader::get_real_timestamp)
      .def("get_status", &vlink::BagReader::get_status)
      .def("get_info", &vlink::BagReader::get_info, nb::rv_policy::reference_internal)
      .def("detect_schema", &vlink::BagReader::detect_schema)
      .def("get_ser_type", &vlink::BagReader::get_ser_type, "url"_a)
      .def("get_schema_type", &vlink::BagReader::get_schema_type, "url"_a)
      .def("is_split_mode", &vlink::BagReader::is_split_mode)
      .def("get_split_index", &vlink::BagReader::get_split_index)
      .def("is_jumping", &vlink::BagReader::is_jumping)
      .def(
          "open_cursor",
          [](vlink::BagReader& self, const vlink::BagReader::Config& config) {
            nb::gil_scoped_release release;
            return self.open_cursor(config);
          },
          "config"_a = vlink::BagReader::Config())
      .def("read_next",
           [](vlink::BagReader& self) -> nb::object {
             vlink::Frame frame;
             bool ok = false;

             {
               nb::gil_scoped_release release;
               ok = self.read_next(frame);
             }

             if (!ok) {
               return nb::none();
             }

             frame.data.deep_copy_self();
             return nb::cast(std::move(frame));
           })
      .def("eof", &vlink::BagReader::eof)
      .def("fail", &vlink::BagReader::fail)
      .def("__bool__", [](const vlink::BagReader& self) { return static_cast<bool>(self); })
      .def(
          "__iter__", [](vlink::BagReader& self) -> vlink::BagReader& { return self; },
          nb::rv_policy::reference_internal)
      .def("__next__",
           [](vlink::BagReader& self) -> nb::object {
             vlink::Frame frame;
             bool ok = false;

             {
               nb::gil_scoped_release release;
               ok = self.read_next(frame);
             }

             if (!ok) {
               throw nb::stop_iteration();
             }

             frame.data.deep_copy_self();
             return nb::cast(std::move(frame));
           })
      .def("async_run",
           [](vlink::BagReader& self) {
             nb::gil_scoped_release release;
             return self.async_run();
           })
      .def(
          "quit",
          [](vlink::BagReader& self, bool force) {
            nb::gil_scoped_release release;
            return self.quit(force);
          },
          "force"_a = false)
      .def(
          "wait_for_quit",
          [](vlink::BagReader& self, int timeout_ms) {
            nb::gil_scoped_release release;
            return self.wait_for_quit(timeout_ms);
          },
          "timeout_ms"_a = -1)
      .def("is_running", &vlink::BagReader::is_running)
      .def("__repr__", [](const vlink::BagReader& self) {
        const auto& info = self.get_info();
        return "BagReader(file='" + info.file_name + "', messages=" + std::to_string(info.message_count) + ")";
      });
}

}  // namespace vlink::python
