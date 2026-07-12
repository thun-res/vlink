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

#include <vlink/base/helpers.h>
#include <vlink/base/utils.h>
#include <vlink/extension/schema_plugin_manager.h>
#include <vlink/version.h>

#include "./dump_context.h"
#include "./dump_expr.h"
#include "./dump_extract.h"
#include "./dump_features.h"
#include "./dump_path.h"
#include "./dump_plan.h"
#include "./dump_proto_cache.h"
#include "./dump_schema.h"
#include "./dump_slice.h"
#include "./dump_types.h"
#include "./dump_validate.h"

#if __has_include(<google/protobuf/compiler/importer.h>) && __has_include(<google/protobuf/text_format.h>)

#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wcpp"
#endif

#include <google/protobuf/compiler/importer.h>
#include <google/protobuf/descriptor.h>
#include <google/protobuf/dynamic_message.h>
#include <google/protobuf/io/zero_copy_stream_impl.h>
#include <google/protobuf/message.h>
#include <google/protobuf/reflection.h>
#include <google/protobuf/text_format.h>

#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic pop
#endif

#endif

#if __has_include(<flatbuffers/idl.h>)
#include <flatbuffers/flatbuffers.h>
#include <flatbuffers/idl.h>

#if FLATBUFFERS_VERSION_MAJOR >= 22
#include "private/flat_gen_text.h"
#endif

#endif

#include <argparse/argparse.hpp>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <nlohmann/json.hpp>
#include <sstream>
#include <string_view>
#include <system_error>
#include <unordered_set>

#if __has_include(<unistd.h>)
#include <unistd.h>
#endif

#ifdef _WIN32
#include <Windows.h>
#undef min
#undef max
#undef GetMessage
#endif

#ifdef VLINK_HAS_PROTOBUF_COMPILER

namespace vlink::dump {

bool condition_contains_empty_comma_field(std::string_view condition) {
  const auto trimmed = vlink::Helpers::trim_string_view(condition);

  if (trimmed.empty()) {
    return false;
  }

  size_t start = 0;

  while (start <= trimmed.size()) {
    const size_t comma = trimmed.find(',', start);
    const size_t end = comma == std::string_view::npos ? trimmed.size() : comma;

    if (vlink::Helpers::trim_string_view(trimmed.substr(start, end - start)).empty()) {
      return true;
    }

    if (comma == std::string_view::npos) {
      break;
    }

    start = comma + 1;
  }

  return false;
}

}  // namespace vlink::dump

static bool check_rate_limit() {
  auto& ctx = vlink::dump::DumpContext::get();

  if VUNLIKELY (ctx.max_count > 0 && ctx.output_count.load(std::memory_order_relaxed) >= ctx.max_count) {
    return false;
  }

  if VUNLIKELY (ctx.max_hz > 0) {
    int64_t now = ctx.main_elapsed_timer.get();
    auto min_interval = static_cast<int64_t>(1000000.0 / ctx.max_hz);
    int64_t prev = ctx.last_output_us.load(std::memory_order_relaxed);

    while (true) {
      if VUNLIKELY (now - prev < min_interval) {
        return false;
      }

      if (ctx.last_output_us.compare_exchange_weak(prev, now, std::memory_order_relaxed)) {
        break;
      }
    }
  }

  ++ctx.output_count;
  return true;
}

static void start_print() {
  auto& ctx = vlink::dump::DumpContext::get();

  if (ctx.quiet_flag) {
    return;
  }

  ctx.main_elapsed_timer.start();

  if VUNLIKELY (ctx.print_thread.joinable()) {
    return;
  }

  ctx.print_thread = std::thread([]() {
    auto& ctx_ref = vlink::dump::DumpContext::get();
    int64_t print_time = 0;
    int64_t real_begin_time = 0;
    int64_t real_end_time = 0;

    if (ctx_ref.dump_for_bag) {
      real_begin_time = ctx_ref.begin_time > 0 ? ctx_ref.begin_time : ctx_ref.bag_player->get_info().blank_duration;
      real_end_time = ctx_ref.end_time > 0 ? ctx_ref.end_time : ctx_ref.bag_player->get_info().total_duration;
    }

    while (!ctx_ref.quit_flag) {
      std::unique_lock lock(ctx_ref.print_mtx);
      ctx_ref.quit_cv.wait_for(lock, std::chrono::milliseconds(50), [&ctx_ref]() -> bool { return ctx_ref.quit_flag; });

      if VUNLIKELY (ctx_ref.quit_flag) {
        break;
      }

      if (ctx_ref.detail_flag || ctx_ref.dump_type == DumpType::kConsole) {
        continue;
      }

      if (ctx_ref.dump_for_bag) {
        if VLIKELY (ctx_ref.bag_player->get_status() == vlink::BagReader::kPlaying) {
          std::cout << "\033[2K\r";
          std::cout << "Progress: ";
          const auto duration = real_end_time - real_begin_time;
          std::cout << vlink::Helpers::double_to_string(
              duration > 0 ? static_cast<double>(100 * (ctx_ref.bag_player->get_real_timestamp() - real_begin_time)) /
                                 static_cast<double>(duration)
                           : 0.0,
              2);
          std::cout << "% [" << ctx_ref.output_count << " samples]";
          std::cout.flush();
        }
      } else {
        std::cout << "\033[2K\r";

        if (ctx_ref.data_has_changed) {
          ctx_ref.data_has_changed = false;
          std::cout << "\033[32m";
        } else {
          std::cout << "\033[31m";
        }

        print_time = ctx_ref.main_elapsed_timer.get() / 1000;
        std::cout << vlink::Helpers::format_milliseconds(print_time + 50, false);
        std::cout << " (" << std::fixed << std::setprecision(1) << print_time / 1000.0F << "s)";
        std::cout << " [" << ctx_ref.output_count << " samples]";
        std::cout << "\033[0m:";
        std::cout.flush();
      }
    }
  });
}

static void stop_print() {
  auto& ctx = vlink::dump::DumpContext::get();

  if (ctx.quiet_flag) {
    return;
  }

  std::unique_lock lock(ctx.print_mtx);

  if VLIKELY (!ctx.quit_flag) {
    ctx.quit_flag = true;
    lock.unlock();
    ctx.quit_cv.notify_all();

    if VLIKELY (ctx.print_thread.joinable()) {
      ctx.print_thread.join();
    }
  }
}

static int start_bag_play(const std::string& bag_file) {
  auto& ctx = vlink::dump::DumpContext::get();
  ctx.bag_config.begin_time = ctx.begin_time;
  ctx.bag_config.end_time = ctx.end_time;
  ctx.bag_config.times = 1;
  ctx.bag_config.rate = 1.0;
  ctx.bag_config.skip_blank = false;
  ctx.bag_config.force_delay = 0;
  ctx.bag_config.auto_pause = false;
  ctx.bag_config.auto_quit = true;

  try {
    ctx.bag_player = vlink::BagReader::create(bag_file, true);
  } catch (vlink::Exception::RuntimeError& e) {
    std::cerr << e.what() << std::endl;
    ctx.has_quit = true;
    return -1;
  }

  ctx.bind_bag_plugin(ctx.bag_player);

  ctx.bag_player->register_output_callback([](const vlink::Frame& frame) {
    auto& cb_ctx = vlink::dump::DumpContext::get();

    if VUNLIKELY (!cb_ctx.callback_has_set) {
      return;
    }

    cb_ctx.invoke_callback(frame.timestamp, frame.url, frame.ser_type, frame.schema_type, frame.data);
  });

  return 0;
}

static int stop_bag_play() {
  auto& ctx = vlink::dump::DumpContext::get();

  if VUNLIKELY (!ctx.bag_player) {
    return -1;
  }

  ctx.has_quit = true;
  ctx.bag_player.reset();
  return 0;
}

static int start_viewer(bool native_mode) {
  auto& ctx = vlink::dump::DumpContext::get();

  try {
    auto filter_type = native_mode ? vlink::DiscoveryViewer::kFilterNative : vlink::DiscoveryViewer::kFilterNone;
    ctx.discovery_viewer = std::make_shared<vlink::DiscoveryViewer>(filter_type);
  } catch (vlink::Exception::RuntimeError& e) {
    std::cerr << e.what() << std::endl;
    ctx.has_quit = true;
    return -1;
  }

  ctx.discovery_viewer->async_run();

  vlink::Utils::register_terminate_signal(
      [](int) {
        auto& sig_ctx = vlink::dump::DumpContext::get();

        if VUNLIKELY (sig_ctx.has_quit) {
          return;
        }

        sig_ctx.has_quit = true;
        sig_ctx.discovery_viewer->quit(true);
      },
      true);

  std::cout << "Information Collecting, Please Wait...";
  std::cout.flush();
  ctx.discovery_viewer->wait_for_quit(1000);
  std::cout << "\033[2K\r";
  std::cout.flush();

  ctx.main_elapsed_timer.restart();

  auto sync_subs = [native_mode](const std::vector<vlink::DiscoveryViewer::Info>& info_list) {
    auto& sync_ctx = vlink::dump::DumpContext::get();
    std::unordered_set<std::string> current_urls;
    current_urls.reserve(info_list.size());

    {
      std::lock_guard lock(sync_ctx.sub_urls_mtx);

      for (const auto& info : info_list) {
        if ((info.type & vlink::kPublisher) == 0 && (info.type & vlink::kSetter) == 0) {
          continue;
        }

        current_urls.emplace(info.url);

        auto sub_iter = sync_ctx.sub_urls.find(info.url);

        if (sub_iter != sync_ctx.sub_urls.end()) {
          const auto current_schema_type = sub_iter->second->get_schema_type();
          const auto expected_schema_type =
              info.schema_type == vlink::SchemaType::kUnknown ? current_schema_type : info.schema_type;

          if VUNLIKELY (sub_iter->second->get_ser_type() != info.ser_type ||
                        current_schema_type != expected_schema_type) {
            sub_iter->second->set_ser_type(info.ser_type, info.schema_type);
          }

          continue;
        }

        std::shared_ptr<RawSub> raw_sub;

        try {
          raw_sub = std::make_shared<RawSub>(info.url, vlink::InitType::kWithoutInit);
        } catch (vlink::Exception::RuntimeError&) {
          continue;
        }

        if (native_mode) {
          raw_sub->set_property("dds.ip", "127.0.0.1");
        }

        raw_sub->set_ser_type(info.ser_type, info.schema_type);
        raw_sub->init();
        std::weak_ptr<RawSub> weak_sub = raw_sub;
        raw_sub->listen([weak_sub, url = info.url](const vlink::Bytes& bytes) {
          auto& cb_ctx = vlink::dump::DumpContext::get();

          if VUNLIKELY (cb_ctx.has_quit || !cb_ctx.callback_has_set) {
            return;
          }

          auto sub = weak_sub.lock();

          if VUNLIKELY (!sub) {
            return;
          }

          cb_ctx.invoke_callback(cb_ctx.main_elapsed_timer.get(), url, sub->get_ser_type(), sub->get_schema_type(),
                                 bytes);
        });
        sync_ctx.sub_urls.emplace(info.url, std::move(raw_sub));
      }

      for (auto iter = sync_ctx.sub_urls.begin(); iter != sync_ctx.sub_urls.end();) {
        if VUNLIKELY (current_urls.count(iter->first) == 0) {
          iter = sync_ctx.sub_urls.erase(iter);
        } else {
          ++iter;
        }
      }
    }
  };

  sync_subs(ctx.discovery_viewer->get_info_list());
  ctx.discovery_viewer->register_callback(
      [sync_subs = std::move(sync_subs)](const std::vector<vlink::DiscoveryViewer::Info>& info_list) {
        sync_subs(info_list);
      });

  return 0;
}

static int stop_viewer() {
  auto& ctx = vlink::dump::DumpContext::get();

  if VUNLIKELY (!ctx.discovery_viewer) {
    return -1;
  }

  ctx.has_quit = true;
  ctx.discovery_viewer.reset();
  return 0;
}

static void print_console_header(int64_t timestamp, int64_t seq) {
  std::cout << "\033[36m--- [" << seq << "] ";
  std::cout << std::fixed << std::setprecision(6) << timestamp / 1000000.0;
  std::cout.unsetf(std::ios::fixed);
  std::cout << "s ---\033[0m" << std::endl;
}

static void print_console_fields(int64_t timestamp, int64_t seq, const std::vector<VariantType>& values,
                                 const std::vector<double>& expr_results = {}) {
  const auto& ctx = vlink::dump::DumpContext::get();
  std::cout << "\033[36m[" << seq << "] ";
  std::cout << std::fixed << std::setprecision(6) << timestamp / 1000000.0;
  std::cout.unsetf(std::ios::fixed);
  std::cout << "s\033[0m ";

  for (size_t i = 0; i < ctx.field_specs.size() && i < values.size(); ++i) {
    if (i > 0) {
      std::cout << ", ";
    }

    std::cout << "\033[33m" << ctx.field_specs[i] << "\033[0m=";
    std::cout << variant_to_string(values[i]);
  }

  for (size_t i = 0; i < expr_results.size() && i < ctx.expr_strings.size(); ++i) {
    std::cout << ", \033[35mexpr(" << ctx.expr_strings[i] << ")\033[0m=";
    std::ostringstream oss;
    oss << std::setprecision(12) << expr_results[i];
    std::cout << oss.str();
  }

  std::cout << std::endl;
}

static std::vector<double> evaluate_expressions(const std::vector<VariantType>& values) {
  auto& ctx = vlink::dump::DumpContext::get();

  if (!ctx.expr_ctx.ready()) {
    return {};
  }

  std::lock_guard lock(ctx.expr_mtx);
  ctx.expr_ctx.load_values(values);
  return ctx.expr_ctx.evaluate_all();
}

static void notify_count_limit_reached() {
  auto& ctx = vlink::dump::DumpContext::get();

  if (ctx.max_count > 0 && ctx.output_count >= ctx.max_count) {
    ctx.request_stop();
  }
}

static void fail_output_write(const std::string& path) {
  auto& ctx = vlink::dump::DumpContext::get();
  std::cerr << "Failed to write output file: " << path << std::endl;
  ctx.is_broken = true;
  ctx.request_stop();
}

static bool write_binary_output(const std::string& path, const vlink::Bytes& data) {
  std::ofstream file(path, std::ios::binary);

  if (!file.is_open()) {
    fail_output_write(path);
    return false;
  }

  file.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
  file.close();

  if (!file.good()) {
    fail_output_write(path);
    return false;
  }

  return true;
}

// NOLINTNEXTLINE(google-readability-function-size)
static int start_dump(const std::string& target_url, const std::string& out_dir, const std::string& base_name,
                      const std::string& proto_dir, [[maybe_unused]] const std::string& fbs_dir,
                      const std::string& dump_type_suffix) {
  auto& ctx = vlink::dump::DumpContext::get();
  auto filesys_out_dir = vlink::dump::utf8_to_path(out_dir);
  auto filesys_proto_dir = vlink::dump::utf8_to_path(proto_dir);

  std::error_code fs_ec;

  if (!std::filesystem::exists(filesys_out_dir, fs_ec) && !fs_ec) {
    std::filesystem::create_directories(filesys_out_dir, fs_ec);

    if (fs_ec) {
      std::cerr << "Failed to create output directory: " << out_dir << " (" << fs_ec.message() << ")" << std::endl;
      ctx.has_quit = true;
      return -1;
    }
  }

  ProtoRuntime proto_runtime;

  if (!proto_dir.empty() && std::filesystem::exists(filesys_proto_dir, fs_ec) && !fs_ec &&
      std::filesystem::is_directory(filesys_proto_dir, fs_ec) && !fs_ec) {
    proto_runtime = load_proto_runtime({proto_dir});
  } else {
    proto_runtime = load_proto_runtime({});
  }

  vlink::dump::ProtoMessageCache proto_cache(proto_runtime);

#ifdef VLINK_HAS_FBS_COMPILER
  std::shared_ptr<flatbuffers::Parser> fbs_parser;
#endif
  bool warned_flatbuffers_fields = false;

  std::string out_file_name;

  try {
    std::filesystem::path out_file = std::filesystem::path(out_dir) / std::filesystem::path(base_name);
    out_file_name = vlink::dump::path_to_utf8(out_file);
  } catch (std::filesystem::filesystem_error& e) {
    std::cerr << e.what() << std::endl;
    ctx.has_quit = true;
    return -1;
  }

  int64_t dump_seq = 0;
  std::string cached_ser;

#ifdef VLINK_HAS_FBS_COMPILER
  auto reset_cached_decoder = [&fbs_parser]() { fbs_parser.reset(); };
#else
  auto reset_cached_decoder = []() {};
#endif

  auto sync_cached_ser = [&cached_ser, &reset_cached_decoder](const std::string& ser) {
    if (cached_ser == ser) {
      return;
    }

    cached_ser = ser;
    reset_cached_decoder();
  };

#ifdef VLINK_HAS_FBS_COMPILER
  auto schema_interface = vlink::SchemaPluginManager::get().get_interface();

  auto ensure_fbs_parser = [&fbs_dir, &fbs_parser, &sync_cached_ser,
                            &schema_interface](const std::string& ser) -> flatbuffers::Parser* {
    sync_cached_ser(ser);

    if (!fbs_parser) {
      if (schema_interface) {
        auto* plugin_parser = static_cast<flatbuffers::Parser*>(schema_interface->create_flatbuffers_parser(ser));

        if (plugin_parser != nullptr) {
          fbs_parser = std::shared_ptr<flatbuffers::Parser>(schema_interface, plugin_parser);
        }
      }

      if (!fbs_parser && !fbs_dir.empty()) {
        bool has_import = false;
        std::error_code fbs_ec;
        auto fbs_path = std::filesystem::path(fbs_dir);

        if (std::filesystem::exists(fbs_path, fbs_ec) && !fbs_ec) {
          import_fbs(fbs_parser, ser, fbs_path, fbs_path, has_import);
        }
      }
    }

    return fbs_parser.get();
  };
#else
  (void)sync_cached_ser;
#endif

  auto warn_flatbuffers_fields = [&warned_flatbuffers_fields](std::string_view mode) {
    if (warned_flatbuffers_fields) {
      return;
    }

    warned_flatbuffers_fields = true;
    std::cerr << "Warning: FlatBuffers field extraction is not supported in dump " << mode
              << "; use explicit protobuf fields or raw/console output." << std::endl;
  };

  {
    std::lock_guard lock(ctx.dump_callback_mtx);
    ctx.dump_callback = [target_url, &dump_seq, &out_file_name, &proto_cache,
#ifdef VLINK_HAS_FBS_COMPILER
                         &ensure_fbs_parser,
#endif
                         &warn_flatbuffers_fields,
                         &dump_type_suffix](int64_t timestamp, const std::string& url, const std::string& ser,
                                            vlink::SchemaType schema_type, const vlink::Bytes& bytes) {
      auto& cb_ctx = vlink::dump::DumpContext::get();

      if VLIKELY (target_url != url) {
        return;
      }

      if VUNLIKELY (!check_rate_limit()) {
        notify_count_limit_reached();
        return;
      }

      cb_ctx.data_has_changed = true;

      const auto resolved_schema_type = vlink::SchemaData::resolve_type(schema_type, ser);
      const bool is_zerocopy = resolved_schema_type == vlink::SchemaType::kZeroCopy;

      auto print_raw_summary = [&bytes, &ser, resolved_schema_type]() {
        const auto schema_label = vlink::SchemaData::convert_type(resolved_schema_type);
        std::cout << "<raw " << bytes.size() << " bytes, ser=" << ser
                  << ", schema=" << (schema_label.empty() ? std::string_view{"unknown"} : schema_label) << ">"
                  << std::endl;
      };

      auto parse_proto = [&proto_cache, &bytes](const std::string& proto_ser) -> google::protobuf::Message* {
        auto* msg = proto_cache.get(proto_ser);

        if (msg == nullptr || !msg->ParseFromArray(bytes.data(), static_cast<int>(bytes.size()))) {
          return nullptr;
        }

        return msg;
      };

      if (cb_ctx.dump_type == DumpType::kConsole) {
        if (cb_ctx.field_specs.empty()) {
          print_console_header(timestamp, cb_ctx.output_count);

          if (is_zerocopy) {
            std::cout << format_zerocopy_message(ser, bytes) << std::endl;
          } else if (resolved_schema_type == vlink::SchemaType::kProtobuf) {
            auto* proto_message = parse_proto(ser);

            if (proto_message != nullptr) {
              std::string text;
              bool ret = google::protobuf::TextFormat::PrintToString(*proto_message, &text);

              if VLIKELY (ret) {
                std::cout << text << std::endl;
              }
            } else {
              print_raw_summary();
            }
          } else if (resolved_schema_type == vlink::SchemaType::kFlatbuffers) {
#ifdef VLINK_HAS_FBS_COMPILER
            auto* parser = ensure_fbs_parser(ser);

            if (parser != nullptr) {
              std::string text;
              const auto* error_chars = flatbuffers::custom::GenText(*parser, bytes.data(), &text);

              if VLIKELY (error_chars == nullptr) {
                std::cout << text << std::endl;
              } else {
                print_raw_summary();
              }
            } else
#endif
            {
              print_raw_summary();
            }
          } else {
            print_raw_summary();
          }
        } else {
          google::protobuf::Message* proto_message = nullptr;

          if (resolved_schema_type == vlink::SchemaType::kProtobuf) {
            proto_message = parse_proto(ser);
          } else if (resolved_schema_type == vlink::SchemaType::kFlatbuffers) {
            warn_flatbuffers_fields("console mode");
          }

          std::vector<VariantType> values;
          values.reserve(cb_ctx.field_specs.size());
          vlink::zerocopy::MessageParser zerocopy_parser;
          const bool zerocopy_parsed = is_zerocopy && zerocopy_parser.parse(ser, bytes);

          for (size_t i = 0; i < cb_ctx.field_specs.size(); ++i) {
            VariantType val;
            bool found = false;

            if (is_zerocopy) {
              found = zerocopy_parsed && extract_zerocopy_value(zerocopy_parser, cb_ctx.field_specs[i], val);
            } else if (proto_message != nullptr) {
              found = extract_proto_value(*proto_message, cb_ctx.field_paths[i], 0, val);
            }

            values.emplace_back(found ? std::move(val) : VariantType{std::string{"N/A"}});
          }

          std::vector<double> expr_results;

          if (cb_ctx.expr_ctx.ready()) {
            expr_results = evaluate_expressions(values);
          }

          print_console_fields(timestamp, cb_ctx.output_count, values, expr_results);
        }

        return;
      }

      if (cb_ctx.dump_type == DumpType::kBin) {
        ++dump_seq;
        write_binary_output(out_file_name + "." + std::to_string(dump_seq) + ".bin", bytes);

        return;
      }

      if (cb_ctx.dump_type == DumpType::kPcd) {
        if (!is_zerocopy ||
            vlink::zerocopy::MessageParser::detect_type(ser) != vlink::zerocopy::MessageParser::Type::kPointCloud) {
          return;
        }

        vlink::zerocopy::PointCloud point_cloud;

        if VUNLIKELY (!vlink::Serializer::convert(bytes, point_cloud)) {
          return;
        }

        ++dump_seq;
        std::string pcd_path = out_file_name + "." + std::to_string(dump_seq) + ".pcd";

        if (!write_pcd_file(pcd_path, point_cloud)) {
          fail_output_write(pcd_path);
          return;
        }

        if (!cb_ctx.quiet_flag && cb_ctx.detail_flag) {
          std::cout << "PCD: " << pcd_path << " (" << point_cloud.size() << " points)" << std::endl;
        }

        return;
      }

      if (cb_ctx.dump_type == DumpType::kJpg || cb_ctx.dump_type == DumpType::kH264 ||
          cb_ctx.dump_type == DumpType::kH265 || cb_ctx.dump_type == DumpType::kRaw) {
        vlink::Bytes out_bytes;
        std::string field_to_extract = cb_ctx.field_specs.empty() ? "data" : cb_ctx.field_specs[0];

        if (is_zerocopy) {
          out_bytes = extract_zerocopy_binary(ser, bytes, field_to_extract);
        } else if (resolved_schema_type == vlink::SchemaType::kProtobuf) {
          auto* proto_message = parse_proto(ser);

          if (proto_message != nullptr) {
            VariantType val;
            auto path = vlink::Helpers::split(field_to_extract, '.');

            if (extract_proto_value(*proto_message, path, 0, val) && std::holds_alternative<vlink::Bytes>(val)) {
              out_bytes = std::get<vlink::Bytes>(std::move(val));
            }
          }
        } else if (resolved_schema_type == vlink::SchemaType::kFlatbuffers) {
          warn_flatbuffers_fields("binary mode");
        }

        if VLIKELY (!out_bytes.empty()) {
          ++dump_seq;
          write_binary_output(out_file_name + "." + std::to_string(dump_seq) + "." + dump_type_suffix, out_bytes);
        }

        return;
      }

      if (cb_ctx.dump_type == DumpType::kCsv || cb_ctx.dump_type == DumpType::kJson) {
        google::protobuf::Message* proto_message = nullptr;

        if (resolved_schema_type == vlink::SchemaType::kProtobuf) {
          proto_message = parse_proto(ser);
        } else if (resolved_schema_type == vlink::SchemaType::kFlatbuffers) {
          warn_flatbuffers_fields("csv/json mode");
        }

        DumpRecord record;
        record.timestamp = timestamp;
        record.values.reserve(cb_ctx.field_specs.size());
        vlink::zerocopy::MessageParser zerocopy_parser;
        const bool zerocopy_parsed = is_zerocopy && zerocopy_parser.parse(ser, bytes);

        for (size_t i = 0; i < cb_ctx.field_specs.size(); ++i) {
          VariantType val;
          bool found = false;

          if (is_zerocopy) {
            found = zerocopy_parsed && extract_zerocopy_value(zerocopy_parser, cb_ctx.field_specs[i], val);
          } else if (proto_message != nullptr) {
            found = extract_proto_value(*proto_message, cb_ctx.field_paths[i], 0, val);
          }

          record.values.emplace_back(found ? std::move(val) : VariantType{std::string{"N/A"}});
        }

        if (cb_ctx.expr_ctx.ready()) {
          record.expr_results = evaluate_expressions(record.values);
        }

        if (!cb_ctx.quiet_flag && cb_ctx.detail_flag) {
          std::cout << "timestamp: " << std::fixed << std::setprecision(6) << timestamp / 1000000.0;
          std::cout.unsetf(std::ios::fixed);

          for (size_t i = 0; i < cb_ctx.field_specs.size() && i < record.values.size(); ++i) {
            std::cout << ", " << cb_ctx.field_specs[i] << "=" << variant_to_string(record.values[i]);
          }

          for (size_t i = 0; i < record.expr_results.size() && i < cb_ctx.expr_strings.size(); ++i) {
            std::cout << ", expr(" << cb_ctx.expr_strings[i] << ")=" << std::setprecision(12) << record.expr_results[i];
          }

          std::cout << std::endl;
        }

        {
          std::lock_guard lock(cb_ctx.cache_mtx);
          static constexpr size_t kMaxCacheRecords = 50'000'000;

          if VUNLIKELY (cb_ctx.cache_buffer.size() >= kMaxCacheRecords) {
            static std::atomic_bool warned{false};

            if (!warned.exchange(true)) {
              std::cerr << "Warning: record limit reached (" << kMaxCacheRecords
                        << "), further samples will be dropped." << std::endl;
            }

            return;
          }

          cb_ctx.cache_buffer.emplace_back(std::move(record));
        }

        return;
      }
    };

    ctx.callback_has_set = true;
  }

  if (ctx.dump_for_bag) {
    start_print();
    ctx.bag_player->play(ctx.bag_config);

    try {
      ctx.bag_player->run();
    } catch (const std::exception& e) {
      std::cerr << "Playback error: " << e.what() << std::endl;
      ctx.is_broken = true;
    }

    ctx.has_quit = true;
    stop_print();

    if (!ctx.quiet_flag) {
      std::cout << "\033[2K\r" << (ctx.is_broken ? "Break." : "Done.") << std::endl;
    }
  } else {
    start_print();

    auto quit_function = [](int) {
      auto& sig_ctx = vlink::dump::DumpContext::get();

      if (sig_ctx.has_quit) {
        return;
      }

      sig_ctx.has_quit = true;
      sig_ctx.discovery_viewer->quit(true);
      sig_ctx.is_broken = true;
    };

    vlink::Utils::start_detect_keyboard([&quit_function](const std::string& key) {
      if (key == "q" || key == "esc") {
        quit_function(0);
      }
    });

    vlink::Utils::register_terminate_signal(quit_function, true);

    ctx.discovery_viewer->wait_for_quit();
    ctx.has_quit = true;
    stop_print();
    vlink::Utils::stop_detect_keyboard();

    if (!ctx.quiet_flag) {
      std::cout << "\033[2K\r"
                << "Done." << std::endl;
    }
  }

  ctx.has_quit = true;

  {
    std::lock_guard lock(ctx.dump_callback_mtx);
    ctx.callback_has_set = false;
    ctx.dump_callback = nullptr;
  }

  if (ctx.dump_type == DumpType::kCsv) {
    auto csv_path = out_file_name + "." + dump_type_suffix;
    std::ofstream file(csv_path);

    if (!file.is_open()) {
      std::cerr << "Failed to write output file: " << csv_path << std::endl;
      ctx.is_broken = true;
    } else {
      write_csv_cell(file, "timestamp");

      for (const auto& spec : ctx.field_specs) {
        file << ",";
        write_csv_cell(file, spec);
      }

      for (const auto& ex : ctx.expr_strings) {
        file << ",";
        write_csv_cell(file, "expr(" + ex + ")");
      }

      file << "\n";

      std::lock_guard lock(ctx.cache_mtx);

      for (const auto& rec : ctx.cache_buffer) {
        file << std::fixed << std::setprecision(6) << rec.timestamp / 1000000.0;
        file.unsetf(std::ios::fixed);

        for (const auto& val : rec.values) {
          file << ",";

          if (std::holds_alternative<int64_t>(val)) {
            file << std::get<int64_t>(val);
          } else if (std::holds_alternative<uint64_t>(val)) {
            file << std::get<uint64_t>(val);
          } else if (std::holds_alternative<double>(val)) {
            file << std::setprecision(12) << std::get<double>(val);
          } else if (std::holds_alternative<std::string>(val)) {
            write_csv_cell(file, std::get<std::string>(val));
          } else if (std::holds_alternative<vlink::Bytes>(val)) {
            file << "<bytes:" << std::get<vlink::Bytes>(val).size() << ">";
          }
        }

        for (const auto& er : rec.expr_results) {
          file << "," << std::setprecision(12) << er;
        }

        file << "\n";
      }

      file.close();

      if (!file.good()) {
        std::cerr << "Failed to write output file: " << csv_path << std::endl;
        ctx.is_broken = true;
      }
    }

    if (!ctx.quiet_flag && !ctx.is_broken) {
      std::cout << "Saved " << ctx.cache_buffer.size() << " records to " << out_file_name << "." << dump_type_suffix
                << std::endl;
    }
  } else if (ctx.dump_type == DumpType::kJson) {
    auto json_path = out_file_name + "." + dump_type_suffix;
    std::ofstream file(json_path);

    if (!file.is_open()) {
      std::cerr << "Failed to write output file: " << json_path << std::endl;
      ctx.is_broken = true;
    } else {
      nlohmann::ordered_json root_json;

      std::lock_guard lock(ctx.cache_mtx);

      for (const auto& rec : ctx.cache_buffer) {
        nlohmann::ordered_json json;
        json["timestamp"] = rec.timestamp / 1000000.0;

        for (size_t i = 0; i < ctx.field_specs.size() && i < rec.values.size(); ++i) {
          const auto& val = rec.values[i];

          if (std::holds_alternative<int64_t>(val)) {
            json[ctx.field_specs[i]] = std::get<int64_t>(val);
          } else if (std::holds_alternative<uint64_t>(val)) {
            json[ctx.field_specs[i]] = std::get<uint64_t>(val);
          } else if (std::holds_alternative<double>(val)) {
            json[ctx.field_specs[i]] = std::get<double>(val);
          } else if (std::holds_alternative<std::string>(val)) {
            json[ctx.field_specs[i]] = std::get<std::string>(val);
          } else if (std::holds_alternative<vlink::Bytes>(val)) {
            json[ctx.field_specs[i]] = "<bytes:" + std::to_string(std::get<vlink::Bytes>(val).size()) + ">";
          }
        }

        for (size_t i = 0; i < rec.expr_results.size() && i < ctx.expr_strings.size(); ++i) {
          json["expr(" + ctx.expr_strings[i] + ")"] = rec.expr_results[i];
        }

        root_json.emplace_back(json);
      }

      file << root_json.dump(2);
      file.close();

      if (!file.good()) {
        std::cerr << "Failed to write output file: " << json_path << std::endl;
        ctx.is_broken = true;
      }
    }

    if (!ctx.quiet_flag && !ctx.is_broken) {
      std::cout << "Saved " << ctx.cache_buffer.size() << " records to " << out_file_name << "." << dump_type_suffix
                << std::endl;
    }
  }

  if (!ctx.quiet_flag &&
      (ctx.dump_type == DumpType::kBin || ctx.dump_type == DumpType::kJpg || ctx.dump_type == DumpType::kH264 ||
       ctx.dump_type == DumpType::kH265 || ctx.dump_type == DumpType::kRaw || ctx.dump_type == DumpType::kPcd)) {
    std::cout << "Saved " << dump_seq << " files to " << out_dir << std::endl;
  }

  ctx.has_quit = true;

  {
    std::lock_guard lock(ctx.sub_urls_mtx);
    ctx.sub_urls.clear();
  }

  {
    std::lock_guard lock(ctx.cache_mtx);
    ctx.cache_buffer.clear();
  }

  return ctx.is_broken ? -1 : 0;
}

#endif

int main(int argc, char* argv[]) {
  vlink::Utils::set_console_utf8_output();

#ifdef VLINK_HAS_PROTOBUF_COMPILER
  auto& ctx = vlink::dump::DumpContext::get();

  vlink::Logger::set_file_level(vlink::Logger::kOff);
  vlink::Logger::init("vlink-dump");

  vlink::Utils::unset_env("VLINK_BAG_PATH");

  argparse::ArgumentParser program("vlink-dump", VLINK_VERSION, argparse::default_arguments::all);

  program.add_description(
      "Versatile data extraction and export tool for VLink topics.\n"
      "Modes: dump/export a topic, or slice/scan a bag.\n"
      "Note: You may need to add multicast/broadcast [" +
      vlink::DiscoveryViewer::get_listen_address() + "]");

  program.add_argument("url")
      .help("Target topic URL; optional for slice/scan, defaults to '*'")
      .default_value(std::string("*"))
      .nargs(argparse::nargs_pattern::optional);

  program.add_argument("-t", "--type")
      .help("Output type: console/text, csv, json, bin, jpg/jpeg, h264, h265, raw, pcd, slice, scan")
      .default_value(std::string("csv"));

  program.add_argument("-c", "--condition")
      .help(
          "Field(s) to extract, comma-separated or quoted space-separated "
          "(e.g. 'header.seq,pose.x,pose.y' or 'header.seq pose.x pose.y')")
      .default_value(std::string());

  program.add_argument("-o", "--out_dir").help("Output directory").default_value(std::string("./"));

  program.add_argument("-m", "--base_name")
      .help("Output file base name for dump/export")
      .default_value(std::string("output"));

  program.add_argument("-f", "--bag_file")
      .help("Bag file path (.vdb / .vdbx / .vcap / .vcapx)")
      .default_value(std::string());

  program.add_argument("-b", "--begin_time")
      .help("Playback start time in seconds (bag/slice/scan)")
      .scan<'g', double>()
      .default_value(0.0);

  program.add_argument("-e", "--end_time")
      .help("Playback end time in seconds (bag/slice/scan)")
      .scan<'g', double>()
      .default_value(0.0);

  program.add_argument("-n", "--count")
      .help("Maximum number of samples (0 = unlimited)")
      .scan<'d', int>()
      .default_value(0);

  program.add_argument("--hz").help("Maximum output rate in Hz (0 = unlimited)").scan<'g', double>().default_value(0.0);

  program.add_argument("--native").help("Use native/loopback mode").default_value(false).implicit_value(true);

  program.add_argument("-d", "--proto_dir").help("Protobuf .proto directory").default_value(std::string());

  program.add_argument("--fbs_dir").help("FlatBuffers .fbs directory (for FBS types)").default_value(std::string());

  program.add_argument("-q", "--quiet").help("Suppress progress output").default_value(false).implicit_value(true);

  program.add_argument("-l", "--detail")
      .help("Print each value in real-time")
      .default_value(false)
      .implicit_value(true);

  program.add_argument("-x", "--expression")
      .help(
          "Math expression(s) on extracted fields.\n"
          "Variables are field names with dots replaced by underscores.\n"
          "E.g. -c 'pose.x,pose.y' -x 'sqrt(pose_x*pose_x + pose_y*pose_y)'\n"
          "Multiple: -x 'pose_x+pose_y' -x 'pose_x-pose_y'\n"
          "Functions: -x 'min(pose_x, pose_y)' -x 'max(pose_x, pose_y)'")
      .append()
      .nargs(1)
      .default_value(std::vector<std::string>());

  program.add_argument("-w", "--window")
      .help("Slice time window in seconds (for -t slice)")
      .scan<'g', double>()
      .default_value(0.0);

  program.add_argument("--segments")
      .help("JSON file with segment list for slice (alternative to --window)")
      .default_value(std::string());

  program.add_argument("--event")
      .help(
          "Event expression for scan or event-driven slice (requires -c).\n"
          "E.g. --event 'brake > 80' with -c 'brake'")
      .default_value(std::string());

  program.add_argument("--pre")
      .help("Seconds before event to include (for --event)")
      .scan<'g', double>()
      .default_value(5.0);

  program.add_argument("--plugin").help("Bag plugin name (rewrites frames on read)").default_value(std::string());

  program.add_argument("--post")
      .help("Seconds after event to include (for --event)")
      .scan<'g', double>()
      .default_value(3.0);

  program.add_argument("--event_state_max_age")
      .help("Max age in seconds for cross-topic event variables (0 = no age limit)")
      .scan<'g', double>()
      .default_value(0.5);

  program.add_argument("--event_min_interval")
      .help("Minimum seconds between emitted event triggers")
      .scan<'g', double>()
      .default_value(0.0);

  program.add_argument("--suffix")
      .help("Output file suffix for slice (default: same as source)")
      .default_value(std::string());

  program.add_argument("--compress")
      .help("Compress output bag data (for -t slice)")
      .default_value(false)
      .implicit_value(true);

  program.add_argument("--force")
      .help("Overwrite existing output files (for -t slice/scan)")
      .default_value(false)
      .implicit_value(true);

  program.add_argument("--no_manifest")
      .help("Do not output manifest file (for -t slice)")
      .default_value(false)
      .implicit_value(true);

  program.add_argument("--manifest")
      .help("Manifest file name (default: manifest.json)")
      .default_value(std::string("manifest.json"));

  program.add_argument("--scan_output")
      .help("Scan result JSON file name under -o (default: events.json)")
      .default_value(std::string("events.json"));

  program.add_argument("--schema_config")
      .help("JSON config file for schema import rules (for -t slice/scan)")
      .default_value(std::string());

  program.add_argument("--schema_plugin")
      .help("Path to schema plugin shared library (or set VLINK_SCHEMA_PLUGIN)")
      .default_value(std::string());

  program.add_argument("--filter")
      .help(
          "Content filter expression (requires -c fields).\n"
          "Messages where expression evaluates to 0 are excluded.\n"
          "E.g. --filter 'speed > 60' with -c 'speed'")
      .default_value(std::string());

  program.add_argument("--export_csv")
      .help("Export parsed fields as CSV alongside each slice (requires -c)")
      .default_value(false)
      .implicit_value(true);

  program.add_argument("--urls")
      .help("Exact URL whitelist for slice/scan")
      .default_value(std::vector<std::string>())
      .nargs(argparse::nargs_pattern::any);

  program.add_argument("--url_filter")
      .help("Quoted keyword filter for URLs (space- or comma-separated), e.g. --url_filter 'camera lidar'")
      .default_value(std::string());

  program.add_argument("--black").help("Invert URL filter to blacklist mode").default_value(false).implicit_value(true);

  program.add_argument("--actions")
      .help(
          "Action filter for slice/scan: 0=Unknown 1=ClientReq 2=ClientResp 3=ServerReq 4=ServerResp 5=Pub 6=Sub 7=Set "
          "8=Get (default: 6)")
      .scan<'d', int>()
      .default_value(std::vector<int>{6})
      .nargs(argparse::nargs_pattern::any);

  program.add_argument("--tag").help("Tag name for output bag (for -t slice)").default_value(std::string());

  program.add_argument("--wal_mode")
      .help("Enable WAL mode for output (for -t slice)")
      .default_value(false)
      .implicit_value(true);

  program.add_argument("--cache_size")
      .help("Write cache size in MB (for -t slice)")
      .scan<'g', double>()
      .default_value(4.0);

  program.add_argument("--compress_level")
      .help("Compress level 1-5, 0=default (for -t slice)")
      .scan<'d', int>()
      .default_value(3);

  program.add_argument("--ignore_compress")
      .help("URLs to skip compression (for -t slice)")
      .default_value(std::vector<std::string>())
      .nargs(argparse::nargs_pattern::any);

  program.add_argument("--sample_step")
      .help("Keep every Nth message per URL (for -t slice, 1=all)")
      .scan<'d', int>()
      .default_value(1);

  program.add_argument("--dry_run")
      .help("Show what would happen without writing files (for -t slice)")
      .default_value(false)
      .implicit_value(true);

  program.add_argument("--quality_check")
      .help("Enable data quality checks during scan: dropout detection, frequency validation")
      .default_value(false)
      .implicit_value(true);

  program.add_argument("--dropout_threshold")
      .help("Max gap in seconds before reporting dropout (for --quality_check)")
      .scan<'g', double>()
      .default_value(1.0);

  std::string example_str = "Examples:\n";
  example_str += "  Dump/export:\n";
  example_str += "    vlink-dump dds://test -c 'header.seq' -t csv -f /tmp/bag.vdb\n";
  example_str += "    vlink-dump dds://test -c 'pose.x,pose.y,pose.z' -t csv --hz 10\n";
  example_str += "    vlink-dump dds://test -t console -n 5\n";
  example_str += "    vlink-dump dds://camera -c 'data' -t jpg -f /tmp/bag.vcap\n";
  example_str += "    vlink-dump dds://test -t bin -o /tmp/raw_output\n";
  example_str += "    vlink-dump dds://test -c 'header.seq' -t json --hz 1\n";
  example_str +=
      "    vlink-dump dds://test -c 'pose.x,pose.y' -x 'sqrt(pose_x*pose_x+pose_y*pose_y)' -x 'pose_x-pose_y' -t csv\n";
  example_str += "    vlink-dump dds://lidar -t pcd -f /tmp/bag.vdb\n";
  example_str += "  Slice/scan:\n";
  example_str += "    vlink-dump -t slice -f bag.vdb -w 30 -o /tmp/slices\n";
  example_str += "    vlink-dump -t slice -f bag.vdb --segments events.json -o /tmp/slices\n";
  example_str += "    vlink-dump -t slice -f bag.vdb -c 'brake' -d /opt/protos --event 'brake>80' --pre 5 --post 3\n";
  example_str +=
      "    vlink-dump -t slice -f bag.vdb -w 30 --urls dds://camera dds://lidar -c 'header.seq' --export_csv\n";
  example_str += "    vlink-dump -t scan -f bag.vdb --quality_check -o /tmp/scan\n";
  example_str += "    vlink-dump -t scan -f bag.vdb -c 'brake' -d /opt/protos --event 'brake>80' -o /tmp";
  program.add_epilog(example_str);

  try {
    program.parse_args(argc, argv);
  } catch (const std::exception& e) {
    std::cerr << e.what() << std::endl;
    std::cerr << program << std::endl;
    return 1;
  }

  const auto& target_url = program.get<std::string>("url");
  bool url_argument_used = program.is_used("url");
  auto type = vlink::dump::to_lower_copy(program.get<std::string>("-t"));
  auto condition = program.get<std::string>("-c");
  const auto& out_dir = program.get<std::string>("-o");
  const auto& base_name = program.get<std::string>("-m");

  auto bag_file = program.get<std::string>("-f");

#ifdef _WIN32

  try {
    bag_file = vlink::Helpers::path_to_string(std::filesystem::path(bag_file));
  } catch (std::filesystem::filesystem_error&) {
  }

#endif

  ctx.dump_for_bag = !bag_file.empty();

  auto begin_seconds = program.get<double>("-b");
  auto end_seconds = program.get<double>("-e");

  if VUNLIKELY (!vlink::dump::validate_duration_seconds("-b/--begin_time", begin_seconds, true) ||
                !vlink::dump::validate_duration_seconds("-e/--end_time", end_seconds, true)) {
    return -1;
  }

  ctx.begin_time = vlink::dump::seconds_to_milliseconds(begin_seconds);
  ctx.end_time = vlink::dump::seconds_to_milliseconds(end_seconds);
  ctx.max_count = program.get<int>("-n");
  ctx.max_hz = program.get<double>("--hz");

  if VUNLIKELY (ctx.max_count < 0) {
    std::cerr << "count [-n] must be non-negative." << std::endl;
    return -1;
  }

  if VUNLIKELY (!std::isfinite(ctx.max_hz) || ctx.max_hz < 0) {
    std::cerr << "--hz must be non-negative." << std::endl;
    return -1;
  }

  auto native_mode = program.is_used("--native");

  auto proto_dir = program.get<std::string>("-d");
  auto fbs_dir = program.get<std::string>("--fbs_dir");

  if (proto_dir.empty()) {
    proto_dir = vlink::Utils::get_env("VLINK_PROTO_DIR");
  }

  if (proto_dir.empty()) {
    proto_dir = vlink::dump::read_home_config(".vlink_proto_dir");
  }

  if (fbs_dir.empty()) {
    fbs_dir = vlink::Utils::get_env("VLINK_FBS_DIR");
  }

  if (fbs_dir.empty()) {
    fbs_dir = vlink::dump::read_home_config(".vlink_fbs_dir");
  }

  auto schema_plugin_path = program.get<std::string>("--schema_plugin");

  proto_dir = vlink::dump::normalize_dir(std::move(proto_dir));
  fbs_dir = vlink::dump::normalize_dir(std::move(fbs_dir));
  schema_plugin_path = vlink::dump::normalize_dir(std::move(schema_plugin_path));

  (void)vlink::SchemaPluginManager::get(schema_plugin_path);

  ctx.bag_plugin_name = program.get<std::string>("--plugin");

  if (!ctx.prepare_bag_plugin()) {
    return -1;
  }

  std::string dump_type_suffix;

  if (type == "console" || type == "text") {
    ctx.dump_type = DumpType::kConsole;
  } else if (type == "csv") {
    ctx.dump_type = DumpType::kCsv;
  } else if (type == "json") {
    ctx.dump_type = DumpType::kJson;
  } else if (type == "bin") {
    ctx.dump_type = DumpType::kBin;
  } else if (type == "jpg" || type == "jpeg") {
    ctx.dump_type = DumpType::kJpg;
    dump_type_suffix = "jpg";
  } else if (type == "h264") {
    ctx.dump_type = DumpType::kH264;
    dump_type_suffix = "h264";
  } else if (type == "h265") {
    ctx.dump_type = DumpType::kH265;
    dump_type_suffix = "h265";
  } else if (type == "raw") {
    ctx.dump_type = DumpType::kRaw;
    dump_type_suffix = "raw";
  } else if (type == "pcd") {
    ctx.dump_type = DumpType::kPcd;
    dump_type_suffix = "pcd";
  } else if (type == "slice") {
    ctx.dump_type = DumpType::kSlice;
  } else if (type == "scan") {
    ctx.dump_type = DumpType::kScan;
  } else {
    std::cerr << "Unknown type: " << type << std::endl;
    std::cerr << "Supported: console/text, csv, json, bin, jpg/jpeg, h264, h265, raw, pcd, slice, scan" << std::endl;
    return -1;
  }

  if (dump_type_suffix.empty()) {
    dump_type_suffix = type;
  }

  if VUNLIKELY (!vlink::dump::validate_mode_options(program, ctx.dump_type, !bag_file.empty(), url_argument_used,
                                                    target_url)) {
    return -1;
  }

  if (!condition.empty()) {
    if (vlink::dump::condition_contains_empty_comma_field(condition)) {
      std::cerr << "Option -c/--condition contains an empty field." << std::endl;
      return -1;
    }

    auto raw_field_specs = vlink::Helpers::split_any(condition);

    for (auto& spec : raw_field_specs) {
      spec = vlink::dump::trim_copy(spec);

      ctx.field_specs.emplace_back(spec);
      ctx.field_paths.emplace_back(vlink::Helpers::split(spec, '.'));
    }
  }

  auto expr_values = program.get<std::vector<std::string>>("-x");

  if VUNLIKELY (program.is_used("-x") && expr_values.empty()) {
    std::cerr << "Option -x/--expression requires an expression." << std::endl;
    return -1;
  }

  if (!expr_values.empty()) {
    ctx.expr_strings = std::move(expr_values);

    for (auto& ex : ctx.expr_strings) {
      ex = vlink::dump::trim_copy(ex);

      if VUNLIKELY (ex.empty()) {
        std::cerr << "Option -x/--expression contains an empty expression." << std::endl;
        return -1;
      }
    }
  }

  if VUNLIKELY (target_url.empty()) {
    std::cerr << "[url] cannot be empty." << std::endl;
    return -1;
  }

  if ((ctx.dump_type == DumpType::kCsv || ctx.dump_type == DumpType::kJson) && ctx.field_specs.empty()) {
    std::cerr << "CSV/JSON mode requires -c to specify fields." << std::endl;
    std::cerr << "Example: -c 'header.seq,pose.x,pose.y'" << std::endl;
    return -1;
  }

  if (!ctx.expr_strings.empty() && ctx.field_specs.empty()) {
    std::cerr << "Expression (-x) requires -c to specify fields as variables." << std::endl;
    return -1;
  }

#ifdef VLINK_ENABLE_EXPRTK

  if (!ctx.expr_strings.empty()) {
    if (!ctx.expr_ctx.compile(ctx.field_specs, ctx.expr_strings)) {
      return -1;
    }
  }

#else

  if (!ctx.expr_strings.empty()) {
    std::cerr << "Expression support requires exprtk library." << std::endl;
    return -1;
  }

#endif

  if (proto_dir.empty() && fbs_dir.empty() && !vlink::SchemaPluginManager::get().is_valid() &&
      ctx.dump_type != DumpType::kSlice && ctx.dump_type != DumpType::kScan) {
    std::cerr << "Warning: No proto_dir or fbs_dir set, only zerocopy types will work." << std::endl;
    std::cerr << "Set via [-d] / [--fbs_dir], env VLINK_PROTO_DIR / VLINK_FBS_DIR, or --schema_plugin / "
                 "VLINK_SCHEMA_PLUGIN"
              << std::endl;
  }

  if VUNLIKELY (std::abs(ctx.begin_time) > 0 && std::abs(ctx.end_time) > 0 && ctx.begin_time >= ctx.end_time) {
    std::cerr << "Invalid begin_time and end_time [-b] [-e]" << std::endl;
    return -1;
  }

  ctx.quiet_flag = vlink::dump::option_used(program, "-q", "--quiet");
  ctx.detail_flag = vlink::dump::option_used(program, "-l", "--detail");

  if (ctx.dump_type == DumpType::kSlice || ctx.dump_type == DumpType::kScan) {
    if VUNLIKELY (!ctx.dump_for_bag) {
      std::cerr << "Slice/scan mode requires -f/--bag_file." << std::endl;
      return -1;
    }

    auto segments_file = program.get<std::string>("--segments");
    auto event_expr = program.get<std::string>("--event");
    auto window = program.get<double>("-w");

    if (segments_file.empty() && event_expr.empty() && window <= 0 && ctx.dump_type == DumpType::kSlice) {
      std::cerr << "Slice mode requires one of: --window (-w), --segments, or --event." << std::endl;
      return -1;
    }

    vlink::dump::SliceOptions opt;
    opt.bag_file = bag_file;
    opt.target_url = target_url;
    opt.out_dir = out_dir;
    opt.window_seconds = window;
    opt.suffix = program.get<std::string>("--suffix");
    opt.compress = program.is_used("--compress");
    opt.force = program.is_used("--force");
    opt.no_manifest = program.is_used("--no_manifest");
    opt.manifest_name = program.get<std::string>("--manifest");
    opt.scan_output_name = program.get<std::string>("--scan_output");
    opt.proto_dir = proto_dir;
    opt.fbs_dir = fbs_dir;
    opt.schema_config_path = program.get<std::string>("--schema_config");
    opt.filter_expr = program.get<std::string>("--filter");
    opt.export_csv = program.is_used("--export_csv");
    opt.tag_name = program.get<std::string>("--tag");
    opt.urls = program.get<std::vector<std::string>>("--urls");
    opt.url_filter = program.get<std::string>("--url_filter");
    opt.black_mode = program.is_used("--black");
    opt.actions = program.get<std::vector<int>>("--actions");
    opt.begin_time = ctx.begin_time;
    opt.end_time = ctx.end_time;
    opt.wal_mode = program.is_used("--wal_mode");
    opt.cache_size = program.get<double>("--cache_size");
    opt.compress_level = program.get<int>("--compress_level");
    opt.ignore_compress = program.get<std::vector<std::string>>("--ignore_compress");
    opt.sample_step = program.get<int>("--sample_step");
    opt.segments_file = segments_file;
    opt.event_expr = event_expr;
    opt.event_pre = program.get<double>("--pre");
    opt.event_post = program.get<double>("--post");
    opt.event_state_max_age = program.get<double>("--event_state_max_age");
    opt.event_min_interval = program.get<double>("--event_min_interval");
    opt.scan_only = (ctx.dump_type == DumpType::kScan);
    opt.dry_run = program.is_used("--dry_run");
    opt.quality_check = program.is_used("--quality_check");
    opt.quality_only = opt.scan_only && opt.quality_check && opt.event_expr.empty();
    opt.dropout_threshold = program.get<double>("--dropout_threshold");
    opt.begin_time_set = vlink::dump::option_used(program, "-b", "--begin_time");
    opt.end_time_set = vlink::dump::option_used(program, "-e", "--end_time") && ctx.end_time > 0;

    return start_slice(opt);
  }

  int ret = 0;

  if (ctx.dump_for_bag) {
    ret = start_bag_play(bag_file);

    if (ret != 0) {
      return ret;
    }
  } else {
    ret = start_viewer(native_mode);

    if (ret != 0) {
      return ret;
    }
  }

  ret = start_dump(target_url, out_dir, base_name, proto_dir, fbs_dir, dump_type_suffix);

  if (ctx.dump_for_bag) {
    stop_bag_play();
  } else {
    stop_viewer();
  }

  return ret;
#else
  (void)argc;
  (void)argv;

  std::cerr << "The lower version of protobuf is not supported. Please change to a higher version of protobuf."
            << std::endl;
  return -1;
#endif
}
