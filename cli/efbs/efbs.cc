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

#include <flatbuffers/flatbuffers.h>
#include <flatbuffers/idl.h>
#include <flatbuffers/reflection.h>

#include <nlohmann/json.hpp>

#include "private/flat_gen_text.h"
//
#include <vlink/base/elapsed_timer.h>
#include <vlink/base/helpers.h>
#include <vlink/base/terminal_stream.h>
#include <vlink/base/utils.h>
#include <vlink/extension/discovery_viewer.h>
#include <vlink/extension/schema_plugin_manager.h>
#include <vlink/version.h>
#include <vlink/vlink.h>
#include <vlink/zerocopy/audio_frame.h>
#include <vlink/zerocopy/camera_frame.h>
#include <vlink/zerocopy/message_parser.h>
#include <vlink/zerocopy/occupancy_grid.h>
#include <vlink/zerocopy/point_cloud.h>
#include <vlink/zerocopy/tensor.h>

#include <algorithm>
#include <cstdio>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>
//
#include <argparse/argparse.hpp>

#if __has_include(<unistd.h>)
#include <unistd.h>
#endif

#ifdef _WIN32
#include <Windows.h>
#undef min
#undef max
#undef GetMessage
[[maybe_unused]] static constexpr int kFlushMinSleep{0};
[[maybe_unused]] static constexpr int kFlushMinLine{5};
#elif defined(__APPLE__)
[[maybe_unused]] static constexpr int kFlushMinSleep{10};
[[maybe_unused]] static constexpr int kFlushMinLine{1};
#else
[[maybe_unused]] static constexpr int kFlushMinSleep{10};
[[maybe_unused]] static constexpr int kFlushMinLine{5};
#endif

using RawPub = vlink::Publisher<vlink::Bytes>;
using RawSub = vlink::Subscriber<vlink::Bytes>;
using RawGetter = vlink::Getter<vlink::Bytes>;

[[maybe_unused]] static constexpr size_t kMaxTaskSize{20000U};
[[maybe_unused]] static constexpr int kCounterCache{2};
[[maybe_unused]] static constexpr int kCounterWeight{2};
[[maybe_unused]] static constexpr int kCollectInterval{1000};
[[maybe_unused]] static constexpr int kTerminalInterval{50};
[[maybe_unused]] static constexpr int kMaxElapsedTime{200};

[[maybe_unused]] static std::atomic_bool has_quit{false};

[[maybe_unused]] std::atomic_bool has_intra_bind{false};

[[maybe_unused]] std::atomic_bool is_paused{false};
[[maybe_unused]] std::atomic_bool is_changed{false};
[[maybe_unused]] std::atomic_bool has_printed{false};
[[maybe_unused]] std::atomic_bool force_update{false};
[[maybe_unused]] std::atomic_bool is_fbs_type{false};
[[maybe_unused]] std::atomic_bool is_out_of_range{false};

[[maybe_unused]] std::atomic_bool black_mode{false};
[[maybe_unused]] std::atomic<size_t> max_str_count{0};
[[maybe_unused]] std::atomic_bool ignore_array{false};
[[maybe_unused]] std::atomic_bool ignore_string{false};
[[maybe_unused]] std::atomic_bool ignore_default{false};
[[maybe_unused]] std::atomic_bool use_long_repeated{false};
[[maybe_unused]] std::atomic_bool print_time_string{false};
[[maybe_unused]] std::atomic_bool print_hex_string{false};
[[maybe_unused]] std::atomic_bool print_enum_string{false};
[[maybe_unused]] std::atomic<int> current_page{0};
[[maybe_unused]] std::atomic<int> total_page{0};
[[maybe_unused]] std::atomic<int> max_rows{0};
[[maybe_unused]] std::atomic<int> max_columns{0};

[[maybe_unused]] std::vector<std::string> filter_list;
// NOLINTNEXTLINE(bugprone-throwing-static-initialization)
[[maybe_unused]] std::pair<int, int> terminal_size{0, 0};

[[maybe_unused]] static std::pair<int, int> get_terminal_size() {
  auto size = vlink::Utils::get_terminal_size();

  if (max_columns > 0) {
    size.first = max_columns;
  }

  if (max_rows > 0) {
    size.second = max_rows;
  }

  if (size.first < 0) {
    size.first = 1000;
  }

  if (size.second < 0) {
    size.second = 25;
  }

  return size;
}

[[maybe_unused]] static void decode_terminal_utf8(std::string_view text, size_t index, uint32_t& code_point,
                                                  size_t& bytes) {
  const auto lead = static_cast<unsigned char>(text[index]);

  if (lead < 0x80) {
    code_point = lead;
    bytes = 1;
    return;
  }

  if ((lead & 0xE0) == 0xC0 && index + 1 < text.size() &&
      (static_cast<unsigned char>(text[index + 1]) & 0xC0) == 0x80) {
    code_point = (static_cast<uint32_t>(lead & 0x1F) << 6) | (static_cast<unsigned char>(text[index + 1]) & 0x3F);
    bytes = 2;
    return;
  }

  if ((lead & 0xF0) == 0xE0 && index + 2 < text.size() &&
      (static_cast<unsigned char>(text[index + 1]) & 0xC0) == 0x80 &&
      (static_cast<unsigned char>(text[index + 2]) & 0xC0) == 0x80) {
    code_point = (static_cast<uint32_t>(lead & 0x0F) << 12) |
                 (static_cast<uint32_t>(static_cast<unsigned char>(text[index + 1]) & 0x3F) << 6) |
                 (static_cast<unsigned char>(text[index + 2]) & 0x3F);
    bytes = 3;
    return;
  }

  if ((lead & 0xF8) == 0xF0 && index + 3 < text.size() &&
      (static_cast<unsigned char>(text[index + 1]) & 0xC0) == 0x80 &&
      (static_cast<unsigned char>(text[index + 2]) & 0xC0) == 0x80 &&
      (static_cast<unsigned char>(text[index + 3]) & 0xC0) == 0x80) {
    code_point = (static_cast<uint32_t>(lead & 0x07) << 18) |
                 (static_cast<uint32_t>(static_cast<unsigned char>(text[index + 1]) & 0x3F) << 12) |
                 (static_cast<uint32_t>(static_cast<unsigned char>(text[index + 2]) & 0x3F) << 6) |
                 (static_cast<unsigned char>(text[index + 3]) & 0x3F);
    bytes = 4;
    return;
  }

  code_point = lead;
  bytes = 1;
}

[[maybe_unused]] static int terminal_codepoint_width(uint32_t code_point) {
  if (code_point < 0x20 || code_point == 0x7F) {
    return 0;
  }

  if ((code_point >= 0x0300 && code_point <= 0x036F) || (code_point >= 0x200B && code_point <= 0x200D) ||
      code_point == 0xFEFF) {
    return 0;
  }

  if ((code_point >= 0x1100 && code_point <= 0x115F) || (code_point >= 0x2E80 && code_point <= 0x303E) ||
      (code_point >= 0x3041 && code_point <= 0x33FF) || (code_point >= 0x3400 && code_point <= 0x4DBF) ||
      (code_point >= 0x4E00 && code_point <= 0x9FFF) || (code_point >= 0xA000 && code_point <= 0xA4CF) ||
      (code_point >= 0xAC00 && code_point <= 0xD7A3) || (code_point >= 0xF900 && code_point <= 0xFAFF) ||
      (code_point >= 0xFE30 && code_point <= 0xFE4F) || (code_point >= 0xFF00 && code_point <= 0xFF60) ||
      (code_point >= 0xFFE0 && code_point <= 0xFFE6) || (code_point >= 0x20000 && code_point <= 0x2FFFD) ||
      (code_point >= 0x30000 && code_point <= 0x3FFFD)) {
    return 2;
  }

  return 1;
}

[[maybe_unused]] static bool is_text_ser_type(std::string_view ser_type) {
  std::string lower_ser{ser_type};
  std::transform(lower_ser.begin(), lower_ser.end(), lower_ser.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return lower_ser == "string" || lower_ser == "std::string" || lower_ser == "json" ||
         lower_ser == "application/json" || lower_ser == "text/json" || lower_ser == "text";
}

[[maybe_unused]] static bool load_text_for_file(const std::string& filename, std::string& content) {
  std::ifstream input(filename, std::ios::binary);

  if VUNLIKELY (!input.is_open()) {
    return false;
  }

  content.assign(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
  return !input.bad();
}

[[maybe_unused]] static std::filesystem::path utf8_to_path(const std::string& utf8) noexcept {
  try {
#ifdef _WIN32
    return std::filesystem::path(vlink::Helpers::string_to_wstring(utf8));
#else
    return std::filesystem::path(utf8);
#endif
  } catch (const std::filesystem::filesystem_error&) {
    return {};
  } catch (const std::exception&) {
    return {};
  }
}

[[maybe_unused]] static std::string get_home_config_path(const std::string& filename) {
  std::string home = vlink::Utils::get_env("HOME");

  if (home.empty()) {
    home = vlink::Utils::get_env("USERPROFILE");
  }

  if VUNLIKELY (home.empty()) {
    return {};
  }

  auto home_path = utf8_to_path(home);

  if VUNLIKELY (home_path.empty()) {
    return {};
  }

  std::string result;

  try {
    auto joined = home_path / filename;
    result = vlink::Helpers::path_to_string(joined);
  } catch (const std::filesystem::filesystem_error&) {
    return {};
  } catch (const std::exception&) {
    return {};
  }

#ifdef _WIN32
  std::replace(result.begin(), result.end(), '\\', '/');
#endif
  return result;
}

[[maybe_unused]] static std::string read_home_config(const std::string& filename) {
  auto config_path_utf8 = get_home_config_path(filename);

  if VUNLIKELY (config_path_utf8.empty()) {
    return {};
  }

  auto fs_path = utf8_to_path(config_path_utf8);

  if VUNLIKELY (fs_path.empty()) {
    return {};
  }

  std::error_code ec;

  if (!std::filesystem::is_regular_file(fs_path, ec) || ec) {
    return {};
  }

  std::ifstream input(fs_path, std::ios::binary);

  if VUNLIKELY (!input.is_open()) {
    return {};
  }

  std::string content;
  content.assign(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());

  auto back_pos = content.find_last_not_of(" \t\r\n");

  if (back_pos != std::string::npos) {
    content.erase(back_pos + 1);
  } else {
    content.clear();
  }

  auto front_pos = content.find_first_not_of(" \t\r\n");

  if (front_pos != std::string::npos) {
    content.erase(0, front_pos);
  }

  return content;
}

[[maybe_unused]] static bool write_home_config(const std::string& filename, const std::string& value) {
  auto config_path_utf8 = get_home_config_path(filename);

  if VUNLIKELY (config_path_utf8.empty()) {
    return false;
  }

  auto fs_path = utf8_to_path(config_path_utf8);

  if VUNLIKELY (fs_path.empty()) {
    return false;
  }

  std::ofstream output(fs_path, std::ios::binary | std::ios::trunc);

  if VUNLIKELY (!output.is_open()) {
    return false;
  }

  output << value;
  output.flush();

  return output.good();
}

[[maybe_unused]] static bool format_json_text(const std::string& content, std::string& out) {
  auto json = nlohmann::ordered_json::parse(content, nullptr, false);

  if (json.is_discarded()) {
    return false;
  }

  out = json.dump(2);

  return true;
}

[[maybe_unused]] static bool import_fbs_from_plugin(
    std::shared_ptr<flatbuffers::Parser>& parser, const std::shared_ptr<vlink::SchemaPluginInterface>& schema_interface,
    const std::string& target_ser) {
  if (!schema_interface) {
    return false;
  }

  auto* parser_ptr = static_cast<flatbuffers::Parser*>(schema_interface->create_flatbuffers_parser(target_ser));

  if (!parser_ptr) {
    return false;
  }

  parser = std::shared_ptr<flatbuffers::Parser>(schema_interface, parser_ptr);
  return true;
}

[[maybe_unused]] static void import_fbs(std::shared_ptr<flatbuffers::Parser>& parser, const std::string& target_ser,
                                        const std::filesystem::path& root_dir, const std::filesystem::path& sub_dir,
                                        bool& has_import, int depth = 0) {
  if (parser) {
    return;
  }

  std::shared_ptr<flatbuffers::Parser> target_parser = std::make_shared<flatbuffers::Parser>();

  if VUNLIKELY (depth >= 100) {
    return;
  }

  std::vector<std::filesystem::directory_entry> file_list;

  try {
    for (const auto& entry : std::filesystem::directory_iterator(sub_dir)) {
      file_list.emplace_back(entry);
    }
  } catch (std::filesystem::filesystem_error&) {
    return;
  }

  if VUNLIKELY (file_list.empty() || file_list.size() > 1000) {
    return;
  }

  std::string root_dir_str = root_dir.string();
  std::string sub_dir_str = sub_dir.string();

  const char* include_root_dirs[] = {root_dir_str.c_str(), nullptr};
  const char* include_dirs[] = {root_dir_str.c_str(), sub_dir_str.c_str(), nullptr};

  bool ret = false;
  std::string schema_file;

  for (const auto& file : file_list) {
    if (file.is_regular_file() && file.path().extension() == ".fbs") {
      try {
        ret = flatbuffers::LoadFile(file.path().string().c_str(), false, &schema_file);

        if (!ret) {
          continue;
        }

        if (root_dir == sub_dir) {
          ret = target_parser->Parse(schema_file.c_str(), include_root_dirs);
        } else {
          ret = target_parser->Parse(schema_file.c_str(), include_dirs);
        }

        if (!ret) {
          continue;
        }

        if (target_parser->LookupStruct(target_ser)) {
          target_parser->SetRootType(target_ser.c_str());
          parser = std::move(target_parser);
          has_import = true;
          return;
        }
      } catch (std::filesystem::filesystem_error&) {
        continue;
      }
    } else if (file.is_directory()) {
      import_fbs(parser, target_ser, root_dir, file.path(), has_import, depth + 1);
    }
  }
}

int start_efbs_pub(const std::string& url, const std::string& fbs_dir, const std::string& fbstxt_file,
                   const std::string& fbs_json, const std::string& ser, vlink::SchemaType schema_type,
                   bool use_blob_encoding, bool native_mode, int times, int interval) {
  const std::string native_ip = native_mode ? vlink::Utils::get_env("VLINK_DDS_NATIVE_IP", "127.0.0.1") : std::string();

  if VUNLIKELY (!has_intra_bind && vlink::Url::is_intra_type(url)) {
    std::cerr << "Cannot pub intra url." << std::endl;
    has_quit = true;
    return -1;
  }

  std::shared_ptr<vlink::DiscoveryViewer> discovery_viewer;
  std::shared_ptr<flatbuffers::Parser> parser;
  std::shared_ptr<RawPub> raw_pub;

  if (interval < 0) {
    interval = 0;
  }

  if VUNLIKELY (url.empty()) {
    std::cerr << "Url is empty." << std::endl;
    has_quit = true;
    return -1;
  }

  try {
    raw_pub = std::make_shared<RawPub>(url, vlink::InitType::kWithoutInit);
  } catch (vlink::Exception::RuntimeError& e) {
    std::cerr << e.what() << std::endl;
    has_quit = true;
    return -1;
  }

  std::string target_ser = ser;

  if (target_ser.empty()) {
    try {
      vlink::DiscoveryViewer::FilterType filter_type = vlink::DiscoveryViewer::kFilterAvailable;

      if (native_mode) {
        filter_type = vlink::DiscoveryViewer::kFilterNative;
      }

      discovery_viewer = std::make_shared<vlink::DiscoveryViewer>(filter_type);
    } catch (vlink::Exception::RuntimeError& e) {
      std::cerr << e.what() << std::endl;
      has_quit = true;
      return -1;
    }

    discovery_viewer->async_run();
  }

  auto quit_function = [&discovery_viewer, &raw_pub](int) {
    if VUNLIKELY (has_quit) {
      return;
    }

    has_quit = true;

    raw_pub->deinit();

    if (discovery_viewer) {
      discovery_viewer->quit(true);
    }
  };

  vlink::Utils::register_terminate_signal(quit_function);

  auto target_schema_type = schema_type;

  if (target_ser.empty()) {
    VLINK_TERM_OUT << "Information Collecting, Please Wait...";
    VLINK_TERM_OUT.flush();

    discovery_viewer->wait_for_quit(kCollectInterval);

    VLINK_TERM_OUT << "\033[2K\r";
    VLINK_TERM_OUT.flush();

    target_ser = discovery_viewer->get_ser_type(url);

    if VUNLIKELY (target_ser.empty()) {
      std::cerr << "Cannot find ser for discovery." << std::endl;
      has_quit = true;
      return -1;
    }
  }

  if (target_schema_type == vlink::SchemaType::kUnknown && discovery_viewer) {
    target_schema_type = vlink::SchemaData::resolve_type(discovery_viewer->get_schema_type(url), target_ser);
  }

  if (target_schema_type == vlink::SchemaType::kUnknown) {
    target_schema_type = vlink::SchemaData::infer_ser_type(target_ser);
  }

  if (target_schema_type == vlink::SchemaType::kUnknown) {
    target_schema_type = vlink::SchemaType::kFlatbuffers;
  }

  const auto inferred_schema_type = vlink::SchemaData::infer_ser_type(target_ser);

  if VUNLIKELY (target_schema_type != vlink::SchemaType::kRaw && inferred_schema_type != vlink::SchemaType::kUnknown &&
                inferred_schema_type != target_schema_type) {
    std::cerr << "ser_type and encoding do not match." << std::endl;
    has_quit = true;
    return -1;
  }

  if VUNLIKELY (target_schema_type == vlink::SchemaType::kProtobuf) {
    std::cerr << "efbs pub does not support protobuf schema_type." << std::endl;
    has_quit = true;
    return -1;
  }

  if VUNLIKELY (discovery_viewer && discovery_viewer->is_ready_to_quit()) {
    has_quit = true;

    raw_pub.reset();
    parser.reset();
    discovery_viewer.reset();

    return 0;
  }

  const bool is_blob_type = target_schema_type == vlink::SchemaType::kRaw && use_blob_encoding;
  const bool is_text_type =
      !is_blob_type && (target_schema_type == vlink::SchemaType::kRaw || is_text_ser_type(target_ser));
  const bool is_zerocopy_type = target_schema_type == vlink::SchemaType::kZeroCopy;

  if VUNLIKELY (is_zerocopy_type) {
    std::cerr << "efbs pub only supports flatbuffers or raw text/json payloads." << std::endl;
    has_quit = true;
    return -1;
  }

  auto schema_interface = vlink::SchemaPluginManager::get().get_interface();

  try {
    if (!fbstxt_file.empty()) {
#ifdef _WIN32
      auto filesys_fbstxt_file = std::filesystem::path(vlink::Helpers::string_to_wstring(fbstxt_file));
#else
      auto filesys_fbstxt_file = std::filesystem::path(fbstxt_file);
#endif

      if VUNLIKELY (!std::filesystem::exists(filesys_fbstxt_file)) {
        std::cerr << "Fbs txt file does not exist." << std::endl;
        has_quit = true;
        return -1;
      }

      if VUNLIKELY (!std::filesystem::is_regular_file(filesys_fbstxt_file)) {
        std::cerr << "Fbs txt file is not a file." << std::endl;
        has_quit = true;
        return -1;
      }
    }

    if (target_schema_type == vlink::SchemaType::kFlatbuffers && !fbs_dir.empty()) {
#ifdef _WIN32
      auto filesys_fbs_dir = std::filesystem::path(vlink::Helpers::string_to_wstring(fbs_dir));
#else
      auto filesys_fbs_dir = std::filesystem::path(fbs_dir);
#endif

      if VUNLIKELY (!std::filesystem::exists(filesys_fbs_dir)) {
        std::cerr << "Fbs dir does not exist." << std::endl;
        has_quit = true;
        return -1;
      }

      if VUNLIKELY (!std::filesystem::is_directory(filesys_fbs_dir)) {
        std::cerr << "Fbs dir is not a directory." << std::endl;
        has_quit = true;
        return -1;
      }
    }
  } catch (std::filesystem::filesystem_error& e) {
    std::cerr << e.what() << std::endl;
    has_quit = true;
    return -1;
  }

  vlink::Bytes raw_data;

  if (is_blob_type) {
    if (fbstxt_file.empty()) {
      bool ok = false;
      raw_data = vlink::Bytes::from_user_input(fbs_json, &ok);

      if VUNLIKELY (!ok) {
        std::cerr << "Blob content must be hex bytes." << std::endl;
        has_quit = true;
        return -1;
      }
    } else {
      std::string blob_payload;

      if VUNLIKELY (!load_text_for_file(fbstxt_file, blob_payload)) {
        std::cerr << "load_text_for_file failed." << std::endl;
        has_quit = true;
        return -1;
      }

      raw_data = vlink::Bytes::from_string(blob_payload);
    }
  } else if (is_text_type) {
    std::string text_payload;

    if (fbstxt_file.empty()) {
      text_payload = fbs_json;
    } else if VUNLIKELY (!load_text_for_file(fbstxt_file, text_payload)) {
      std::cerr << "load_text_for_file failed." << std::endl;
      has_quit = true;
      return -1;
    }

    raw_data = vlink::Bytes::from_string(text_payload);
  } else {
    bool has_import = import_fbs_from_plugin(parser, schema_interface, target_ser);

    if (!has_import) {
      if VUNLIKELY (fbs_dir.empty()) {
        std::cerr << "Must set fbs dir [-d], set env 'VLINK_FBS_DIR', run 'vlink-efbs import <dir>', or load "
                     "VLINK_SCHEMA_PLUGIN."
                  << std::endl;
        has_quit = true;
        return -1;
      }

      try {
#ifdef _WIN32
        auto fbs_path = std::filesystem::path(vlink::Helpers::string_to_wstring(fbs_dir));
#else
        auto fbs_path = std::filesystem::path(fbs_dir);
#endif

        import_fbs(parser, target_ser, fbs_path, fbs_path, has_import);
      } catch (std::filesystem::filesystem_error& e) {
        std::cerr << e.what() << std::endl;
        has_quit = true;
        return -1;
      }
    }

    if VUNLIKELY (!parser || !has_import) {
      std::cerr << "Import flatbuffers schema failed." << std::endl;
      has_quit = true;
      return -1;
    }
  }

  if (native_mode) {
    raw_pub->set_property("dds.ip", native_ip);
  }

  raw_pub->set_ser_type(target_ser, target_schema_type);
  raw_pub->init();

  if (!is_text_type && !is_blob_type) {
    if (!fbs_json.empty()) {
      if VUNLIKELY (!parser->ParseJson(fbs_json.c_str())) {
        std::cerr << "Parse flatbuffers json failed: " << parser->error_ << std::endl;
        has_quit = true;
        return -1;
      }
    } else {
      std::string json_text;

      if VUNLIKELY (!load_text_for_file(fbstxt_file, json_text)) {
        std::cerr << "load_text_for_file failed." << std::endl;
        has_quit = true;
        return -1;
      }

      if VUNLIKELY (!parser->ParseJson(json_text.c_str(), fbstxt_file.c_str())) {
        std::cerr << "Parse flatbuffers json failed: " << parser->error_ << std::endl;
        has_quit = true;
        return -1;
      }
    }

    raw_data = vlink::Bytes::shallow_copy(parser->builder_.GetBufferPointer(), parser->builder_.GetSize());
  }

  vlink::Utils::start_detect_keyboard([&quit_function](const std::string& key) {
    if (key == "q" || key == "esc") {
      quit_function(0);
    } else if (key == " ") {
      if (is_paused) {
        is_paused = false;
      } else {
        is_paused = true;
      }
    }
  });

  if (raw_pub->has_inited()) {
    raw_pub->wait_for_subscribers(std::chrono::milliseconds(500));
  }

  vlink::ElapsedTimer elapsed;
  elapsed.start();

  int dx = 0;
  int64_t paused_elapsed = 0;

  for (int64_t i = 0; i < times || times <= 0; ++i) {
    if (!raw_pub->has_inited()) {
      break;
    }

    if (is_paused) {
      const int64_t pause_begin = elapsed.get();

      while (is_paused && raw_pub->has_inited() && !has_quit) {
        if (discovery_viewer) {
          if (discovery_viewer->wait_for_quit(10)) {
            break;
          }
        } else {
          std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
      }

      paused_elapsed += std::max<int64_t>(elapsed.get() - pause_begin, 0);

      if (!raw_pub->has_inited() || has_quit || (discovery_viewer && discovery_viewer->is_ready_to_quit())) {
        break;
      }
    }

    raw_pub->publish(raw_data);

    dx = static_cast<int>((i + 1) * interval - (elapsed.get() - paused_elapsed));

    if (dx < 0) {
      dx = 0;
    }

    if (discovery_viewer) {
      if (discovery_viewer->wait_for_quit(dx)) {
        break;
      }
    } else {
      if VUNLIKELY (has_quit) {
        break;
      }

      if (dx > 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(dx));
      }

      if VUNLIKELY (has_quit) {
        break;
      }
    }
  }

  if (discovery_viewer) {
    discovery_viewer->quit(true);
    discovery_viewer->wait_for_quit();
  }

  has_quit = true;

  vlink::Utils::stop_detect_keyboard();
  // VLINK_TERM_OUT << std::endl;
  VLINK_TERM_OUT.flush();

  raw_pub.reset();
  parser.reset();
  discovery_viewer.reset();

  return 0;
}

class ParserLoop : public vlink::MessageLoop {
 public:
  ParserLoop() {
    set_name("ParserLoop");
    set_strategy(vlink::MessageLoop::kPopStrategy);
  }

 protected:
  size_t get_max_task_count() const override { return kMaxTaskSize; }

  uint32_t get_max_elapsed_time() const override { return kMaxElapsedTime; }
};

// NOLINTNEXTLINE(google-readability-function-size)
int start_efbs_sub(const std::string& url, const std::string& fbs_dir, const std::string& ser,
                   vlink::SchemaType schema_type, bool use_blob_encoding, bool native_mode, const std::string& filter,
                   bool use_getter) {
  const std::string native_ip = native_mode ? vlink::Utils::get_env("VLINK_DDS_NATIVE_IP", "127.0.0.1") : std::string();

  if VUNLIKELY (!has_intra_bind && vlink::Url::is_intra_type(url)) {
    std::cerr << "Cannot sub intra url." << std::endl;
    has_quit = true;
    return -1;
  }

  std::shared_ptr<vlink::DiscoveryViewer> discovery_viewer;
  std::shared_ptr<ParserLoop> parser_loop;
  std::shared_ptr<flatbuffers::Parser> parser;
  const reflection::Schema* fbs_schema = nullptr;
  const reflection::Object* fbs_root_object = nullptr;
  std::optional<std::shared_ptr<RawSub>> raw_sub;
  std::optional<std::shared_ptr<RawGetter>> raw_getter;

  if VUNLIKELY (url.empty()) {
    std::cerr << "Url is empty." << std::endl;
    has_quit = true;
    return -1;
  }

  bool has_explicit_ser = !ser.empty();
  std::string target_ser = ser;

  if (target_ser.empty()) {
    try {
      vlink::DiscoveryViewer::FilterType filter_type = vlink::DiscoveryViewer::kFilterAvailable;

      if (native_mode) {
        filter_type = vlink::DiscoveryViewer::kFilterNative;
      }

      discovery_viewer = std::make_shared<vlink::DiscoveryViewer>(filter_type);
    } catch (vlink::Exception::RuntimeError& e) {
      std::cerr << e.what() << std::endl;
      has_quit = true;
      return -1;
    }

    discovery_viewer->async_run();
  }

  parser_loop = std::make_shared<ParserLoop>();
  parser_loop->async_run();

  auto quit_function = [&discovery_viewer, &parser_loop](int) {
    if VUNLIKELY (has_quit) {
      return;
    }

    has_quit = true;

    if (discovery_viewer) {
      discovery_viewer->quit(true);
    }

    parser_loop->quit(true);
  };

  vlink::Utils::register_terminate_signal(quit_function);
  uint32_t target_type = 0;
  auto target_schema_type = schema_type;

  if (target_ser.empty()) {
    VLINK_TERM_OUT << "Information Collecting, Please Wait...";
    VLINK_TERM_OUT.flush();

    discovery_viewer->wait_for_quit(kCollectInterval);

    VLINK_TERM_OUT << "\033[2K\r";
    VLINK_TERM_OUT.flush();

    target_ser = discovery_viewer->get_ser_type(url);

    for (const auto& info : discovery_viewer->get_info_list()) {
      if (info.url == url) {
        target_type = info.type;
        break;
      }
    }

    if VUNLIKELY (target_ser.empty()) {
      std::cerr << "Cannot find ser for discovery." << std::endl;
      has_quit = true;
      return -1;
    }
  }

  if (target_schema_type == vlink::SchemaType::kUnknown && discovery_viewer) {
    target_schema_type = vlink::SchemaData::resolve_type(discovery_viewer->get_schema_type(url), target_ser);
  }

  if (target_schema_type == vlink::SchemaType::kUnknown) {
    target_schema_type = vlink::SchemaData::infer_ser_type(target_ser);
  }

  if (target_schema_type == vlink::SchemaType::kUnknown) {
    target_schema_type = vlink::SchemaType::kFlatbuffers;
  }

  const auto inferred_schema_type = vlink::SchemaData::infer_ser_type(target_ser);

  if VUNLIKELY (target_schema_type != vlink::SchemaType::kRaw && inferred_schema_type != vlink::SchemaType::kUnknown &&
                inferred_schema_type != target_schema_type) {
    std::cerr << "ser_type and encoding do not match." << std::endl;
    has_quit = true;
    return -1;
  }

  if VUNLIKELY (target_schema_type == vlink::SchemaType::kProtobuf) {
    std::cerr << "efbs sub does not support protobuf schema_type." << std::endl;
    has_quit = true;
    return -1;
  }

  if VUNLIKELY (discovery_viewer && discovery_viewer->is_ready_to_quit()) {
    has_quit = true;

    raw_sub.reset();
    raw_getter.reset();
    parser.reset();
    discovery_viewer.reset();
    parser_loop.reset();

    return 0;
  }

  filter_list = vlink::Helpers::split_any(filter);
  bool is_blob_type = target_schema_type == vlink::SchemaType::kRaw && use_blob_encoding;
  bool is_text_type = !is_blob_type && (target_schema_type == vlink::SchemaType::kRaw || is_text_ser_type(target_ser));
  auto schema_interface = vlink::SchemaPluginManager::get().get_interface();

  if (target_schema_type == vlink::SchemaType::kZeroCopy) {
    is_fbs_type = false;
  } else if (target_schema_type == vlink::SchemaType::kRaw) {
    is_fbs_type = false;
  } else if (target_schema_type == vlink::SchemaType::kFlatbuffers) {
    is_fbs_type = true;

    bool has_import = import_fbs_from_plugin(parser, schema_interface, target_ser);

    if (!has_import) {
      if VUNLIKELY (fbs_dir.empty()) {
        std::cerr << "Must set fbs dir [-d], set env 'VLINK_FBS_DIR', run 'vlink-efbs import <dir>', or load "
                     "VLINK_SCHEMA_PLUGIN."
                  << std::endl;
        has_quit = true;
        return -1;
      }

      try {
#ifdef _WIN32
        auto fbs_path = std::filesystem::path(vlink::Helpers::string_to_wstring(fbs_dir));
#else
        auto fbs_path = std::filesystem::path(fbs_dir);
#endif

        import_fbs(parser, target_ser, fbs_path, fbs_path, has_import);
      } catch (std::filesystem::filesystem_error& e) {
        std::cerr << e.what() << std::endl;
        has_quit = true;
        return -1;
      }
    }

    if VUNLIKELY (!parser || !has_import) {
      std::cerr << "Import flatbuffers schema failed." << std::endl;
      has_quit = true;
      return -1;
    }

    parser->Serialize();

    flatbuffers::Verifier schema_verifier(parser->builder_.GetBufferPointer(), parser->builder_.GetSize());

    if VUNLIKELY (!reflection::VerifySchemaBuffer(schema_verifier)) {
      std::cerr << "Verify flatbuffers schema failed." << std::endl;
      has_quit = true;
      return -1;
    }

    fbs_schema = reflection::GetSchema(parser->builder_.GetBufferPointer());
    fbs_root_object = fbs_schema != nullptr ? fbs_schema->root_table() : nullptr;

    if VUNLIKELY (fbs_schema == nullptr || fbs_root_object == nullptr) {
      std::cerr << "Load flatbuffers schema failed." << std::endl;
      has_quit = true;
      return -1;
    }
  } else {
    std::cerr << "Unsupported schema_type for efbs sub: " << static_cast<int>(target_schema_type) << std::endl;
    has_quit = true;
    return -1;
  }

  std::atomic<int> parse_ret = 0;

  vlink::Bytes current_bytes;
  std::mutex bytes_mtx;
  std::atomic_bool has_new_data{false};
  std::atomic<int64_t> frame_seq{0};

  std::vector<std::string> print_list;
  std::vector<int> line_list;
  std::string print_str;
  std::string current_str;
  int current_line = 0;
  bool is_url_title_with_dot = false;
  bool has_rendered_url_title = false;
  bool rendered_url_title_with_dot = false;
  double current_frame_rate = 0.0;
  double rendered_frame_rate = -1.0;
  std::deque<int64_t> frame_seq_buffer;
  std::atomic<uint64_t> last_frame_timestamp_ms{0};
  int current_rate_color = 33;
  int rendered_rate_color = -1;

  auto redraw_url_title = [&url, &current_frame_rate, &current_rate_color](bool show_data_dot, bool restore_cursor) {
    const char* rate_color = "\033[33m";

    switch (current_rate_color) {
      case 31:
        rate_color = "\033[31m";
        break;
      case 32:
        rate_color = "\033[32m";
        break;
      default:
        break;
    }

    if (restore_cursor) {
      VLINK_TERM_OUT << "\033[s";
    }

    VLINK_TERM_OUT << "\033[2;1H";
    VLINK_TERM_OUT << "\033[K";

    VLINK_TERM_OUT << "\033[34;1;4m" << url << "\033[0m ";

    if (show_data_dot) {
      VLINK_TERM_OUT << rate_color << "\u25CF " << vlink::Helpers::double_to_string(current_frame_rate) << "Hz\033[0m";
    } else {
      VLINK_TERM_OUT << rate_color << "  " << vlink::Helpers::double_to_string(current_frame_rate) << "Hz\033[0m";
    }

    if (restore_cursor) {
      VLINK_TERM_OUT << "\033[u";
    } else {
      VLINK_TERM_OUT << std::endl;
    }
  };

  auto mark_url_title_rendered = [&has_rendered_url_title, &rendered_url_title_with_dot, &rendered_frame_rate,
                                  &rendered_rate_color, &current_frame_rate, &current_rate_color](bool show_data_dot) {
    has_rendered_url_title = true;
    rendered_url_title_with_dot = show_data_dot;
    rendered_frame_rate = current_frame_rate;
    rendered_rate_color = current_rate_color;
  };

  auto should_redraw_url_title = [&has_rendered_url_title, &rendered_url_title_with_dot, &rendered_frame_rate,
                                  &rendered_rate_color, &current_frame_rate, &current_rate_color](bool show_data_dot) {
    return !has_rendered_url_title || rendered_url_title_with_dot != show_data_dot ||
           rendered_rate_color != current_rate_color || rendered_frame_rate != current_frame_rate;
  };

  auto update_terminal_function = [&url, &target_ser, &current_bytes, &bytes_mtx, &print_str, &print_list, &line_list,
                                   &current_str, &current_line, &has_new_data, &is_url_title_with_dot,
                                   &redraw_url_title, &mark_url_title_rendered, &should_redraw_url_title,
                                   &discovery_viewer, &parser_loop, &quit_function, &parse_ret, is_text_type,
                                   is_blob_type, parser, fbs_schema, fbs_root_object]() {
    auto target_terminal_size = get_terminal_size();
    bool show_data_dot = has_new_data.exchange(false);
    const bool terminal_size_changed = terminal_size != target_terminal_size;

    if VUNLIKELY (terminal_size_changed) {
      terminal_size = target_terminal_size;
      is_changed = true;
    }

    if VUNLIKELY (terminal_size.first <= 0 || terminal_size.second <= 0) {
      return;
    }

    if VUNLIKELY (!has_printed) {
      if VLIKELY (!force_update) {
        is_changed = false;
        return;
      }

      VLINK_TERM_OUT << "\033[H\033[J";

      if (is_paused) {
        VLINK_TERM_OUT << "\033[33m"
                       << "Message Parsed by vlink-efbs (Wait For Message, Paused):"
                       << "\033[0m" << std::endl;
      } else {
        VLINK_TERM_OUT << "Message Parsed by vlink-efbs (Wait For Message)... " << std::endl;
      }

      VLINK_TERM_OUT << "\033[34;1;4m" << url << "\033[0m" << std::endl;
      VLINK_TERM_OUT.flush();

      is_changed = false;
      force_update = false;
      return;
    }

    if VLIKELY (!is_changed) {
      if (!is_paused && should_redraw_url_title(show_data_dot)) {
        redraw_url_title(show_data_dot, true);
        mark_url_title_rendered(show_data_dot);
        VLINK_TERM_OUT.flush();
      }

      is_url_title_with_dot = false;
      force_update = false;
      return;
    }

    if (is_fbs_type) {
      {
        if VUNLIKELY (!parser) {
          return;
        }

        std::lock_guard lock(bytes_mtx);

        if VUNLIKELY (current_bytes.empty()) {
          force_update = false;
          return;
        }

        const bool fbs_verified =
            parser->opts.size_prefixed
                ? flatbuffers::VerifySizePrefixed(*fbs_schema, *fbs_root_object, current_bytes.data(),
                                                  current_bytes.size())
                : flatbuffers::Verify(*fbs_schema, *fbs_root_object, current_bytes.data(), current_bytes.size());

        if VUNLIKELY (!fbs_verified) {
          force_update = false;
          return;
        }

        print_str.clear();

        flatbuffers::custom::JsonPrinter::ignore_array = ignore_array;
        flatbuffers::custom::JsonPrinter::ignore_string = ignore_string;
        flatbuffers::custom::JsonPrinter::ignore_default = ignore_default;
        flatbuffers::custom::JsonPrinter::use_long_repeated = use_long_repeated;
        flatbuffers::custom::JsonPrinter::print_time_string = print_time_string;
        flatbuffers::custom::JsonPrinter::print_hex_string = print_hex_string;
        flatbuffers::custom::JsonPrinter::print_enum_string = print_enum_string;
        flatbuffers::custom::JsonPrinter::black_mode = black_mode;
        flatbuffers::custom::JsonPrinter::filter_list = &filter_list;

        parser->opts.output_enum_identifiers = print_enum_string;
        parser->opts.force_defaults = !ignore_default;
        parser->opts.output_default_scalars_in_json = !ignore_default;

        // NOLINTNEXTLINE(readability-redundant-smartptr-get)
        const auto* error_chars = flatbuffers::custom::GenText(*parser.get(), current_bytes.data(), &print_str);

        if VUNLIKELY (error_chars) {
          std::cerr << "Failed to gen fbs text(" << error_chars << ")." << std::endl;
          quit_function(0);

          parse_ret = 1;

          force_update = false;
          return;
        }
      }

      if VUNLIKELY ((discovery_viewer && discovery_viewer->is_ready_to_quit()) || parser_loop->is_ready_to_quit()) {
        force_update = false;
        return;
      }
    } else {
      {
        std::unique_lock lock(bytes_mtx);

        const auto target_zerocopy_type = vlink::zerocopy::MessageParser::detect_type(target_ser);
        vlink::zerocopy::MessageParser message_parser;
        bool zerocopy_parse_succeeded = false;

        if (!is_blob_type && !is_text_type && !current_bytes.empty() &&
            target_zerocopy_type != vlink::zerocopy::MessageParser::Type::kUnknown) {
          zerocopy_parse_succeeded = message_parser.parse(target_zerocopy_type, current_bytes);
        }

        if (is_blob_type) {
          print_str.clear();

          int max_line_chars = target_terminal_size.first - 2;

          if (max_line_chars < 2) {
            max_line_chars = 2;
          }

          int per_line = (max_line_chars + 1) / 3;

          if (per_line > 50) {
            per_line = 50;
          }

          if (per_line >= 10) {
            per_line = (per_line / 10) * 10;
          }

          if (per_line <= 0) {
            per_line = 10;
          }

          const size_t per_rows = current_bytes.empty() ? 0
                                                        : (current_bytes.size() + static_cast<size_t>(per_line) - 1) /
                                                              static_cast<size_t>(per_line);

          print_str += std::string("per_line: ") + std::to_string(per_line) + "\n";
          print_str += std::string("per_rows: ") + std::to_string(per_rows) + "\n";
          print_str += std::string("data_size: ") + std::to_string(current_bytes.size()) + "\n";
          print_str += std::string("data_blob:\n");

          const auto hex_str = vlink::Bytes::convert_to_hex_str(current_bytes.data(), current_bytes.size());
          size_t pos = 0;

          for (size_t i = 0; i < per_rows; ++i) {
            const size_t remain_bytes = current_bytes.size() - i * static_cast<size_t>(per_line);
            const size_t line_bytes = std::min<size_t>(static_cast<size_t>(per_line), remain_bytes);
            const size_t line_chars = line_bytes == 0 ? 0 : line_bytes * 3 - 1;

            print_str.append(hex_str, pos, line_chars);
            print_str += "\n";

            pos += line_chars;

            if (pos < hex_str.size() && hex_str[pos] == ' ') {
              ++pos;
            }
          }
        } else if (is_text_type) {
          std::string text_payload = current_bytes.to_string();

          if (!format_json_text(text_payload, print_str)) {
            print_str = std::move(text_payload);
          }
        } else if VUNLIKELY (current_bytes.empty()) {
          force_update = false;
          return;
        } else if (target_zerocopy_type != vlink::zerocopy::MessageParser::Type::kUnknown) {
          if VUNLIKELY (!zerocopy_parse_succeeded) {
            std::cerr << "Failed to parse " << vlink::zerocopy::MessageParser::type_name(target_zerocopy_type)
                      << " message." << std::endl;
            quit_function(0);

            parse_ret = 1;

            force_update = false;
            return;
          }

          vlink::zerocopy::MessageFormatOptions format_options;
          format_options.hex = print_hex_string;
          format_options.date = print_time_string;
          format_options.enum_name = print_enum_string;
          format_options.expand_arrays = !ignore_array;

          bool format_truncated = false;
          print_str = vlink::zerocopy::format_message(message_parser, format_options, &format_truncated);

          if (format_truncated) {
            is_out_of_range = true;
          }

        } else {
          std::cerr << "Unsupported type." << std::endl;
          quit_function(0);

          parse_ret = 1;

          force_update = false;
          return;
        }
      }
    }

    if VUNLIKELY ((discovery_viewer && discovery_viewer->is_ready_to_quit()) || parser_loop->is_ready_to_quit()) {
      force_update = false;
      return;
    }

    if (is_paused && !force_update && !terminal_size_changed) {
      VLINK_TERM_OUT << "\033[H";
      VLINK_TERM_OUT.flush();

      total_page = print_list.size();

      if (current_page > total_page - 1) {
        current_page = total_page - 1;
      }

      if (current_page < 0) {
        current_page = 0;
      }

      if (!print_list.empty() && !line_list.empty()) {
        current_str = print_list.at(current_page);
        current_line = line_list.at(current_page);
      }

      VLINK_TERM_OUT << "\033[K";

      VLINK_TERM_OUT << "\033[33m"
                     << "Message Parsed by vlink-efbs (Paused):"
                     << "\033[0m" << std::endl;

      redraw_url_title(show_data_dot, false);
      is_url_title_with_dot = show_data_dot;
      mark_url_title_rendered(show_data_dot);

      if (!current_str.empty()) {
        VLINK_TERM_OUT << "\033[32m";

        auto print_split_view_list = vlink::Helpers::split_view(current_str, '\n');

        for (size_t i = 0; i < print_split_view_list.size(); ++i) {
          VLINK_TERM_OUT << "\033[K";
          VLINK_TERM_OUT << print_split_view_list[i];

          if (i < print_split_view_list.size() - 1) {
            VLINK_TERM_OUT << "\n";

            if (i > 0 && i % kFlushMinLine == 0) {
              VLINK_TERM_OUT.flush();
              if constexpr (kFlushMinSleep > 0) {
                std::this_thread::sleep_for(std::chrono::microseconds(kFlushMinSleep));
              }
            }
          }
        }

        VLINK_TERM_OUT << "\033[0m";
        VLINK_TERM_OUT << "\033[K";
        VLINK_TERM_OUT << std::endl;
      }

      VLINK_TERM_OUT.flush();

    } else {
      if VUNLIKELY ((discovery_viewer && discovery_viewer->is_ready_to_quit()) || parser_loop->is_ready_to_quit()) {
        force_update = false;
        return;
      }

      if VUNLIKELY (print_str.size() > max_str_count) {
        VLINK_TERM_OUT << "\033[H\033[J";
        VLINK_TERM_OUT.flush();
        std::cerr << "The Message is too large to display." << std::endl;
        std::cerr.flush();

        force_update = false;
        return;
      }

      auto split_str_list = vlink::Helpers::split(print_str, '\n');

      if VUNLIKELY ((discovery_viewer && discovery_viewer->is_ready_to_quit()) || parser_loop->is_ready_to_quit()) {
        force_update = false;
        return;
      }

      std::string page_str;
      int line_count = 0;

      print_list.clear();
      line_list.clear();

      print_list.reserve(split_str_list.size() + 5);
      line_list.reserve(split_str_list.size() + 5);

      const auto line_width = static_cast<size_t>(terminal_size.first);

      for (const auto& str : split_str_list) {
        size_t offset = 0;
        size_t display_width = 0;

        auto complete_line = [&page_str, &line_count, &print_list, &line_list]() {
          page_str += "\n";
          ++line_count;

          if (line_count >= terminal_size.second - 3) {
            page_str.pop_back();
            print_list.emplace_back(page_str);
            line_list.emplace_back(line_count);
            page_str.clear();
            line_count = 0;
          }
        };

        while (offset < str.size()) {
          uint32_t code_point = 0;
          size_t bytes = 0;
          decode_terminal_utf8(str, offset, code_point, bytes);

          if (code_point == '\t') {
            size_t spaces = 8U - display_width % 8U;
            offset += bytes;

            while (spaces > 0) {
              if (display_width >= line_width) {
                complete_line();
                display_width = 0;
              }

              const size_t count = std::min(spaces, line_width - display_width);
              page_str.append(count, ' ');
              display_width += count;
              spaces -= count;
            }

            continue;
          }

          const auto width = static_cast<size_t>(terminal_codepoint_width(code_point));

          if (width > 0 && display_width > 0 && display_width + width > line_width) {
            complete_line();
            display_width = 0;
          }

          page_str.append(str, offset, bytes);
          display_width += width;
          offset += bytes;
        }

        complete_line();
      }

      if (!page_str.empty()) {
        page_str.pop_back();
      }

      if (!page_str.empty()) {
        print_list.emplace_back(page_str);
        line_list.emplace_back(line_count);
      }

      total_page = std::min(print_list.size(), static_cast<size_t>(5000));

      if (current_page > total_page - 1) {
        current_page = total_page - 1;
      }

      if (current_page < 0) {
        current_page = 0;
      }

      if (!print_list.empty() && !line_list.empty()) {
        current_str = print_list.at(current_page);
        current_line = line_list.at(current_page);
      }

      VLINK_TERM_OUT << "\033[H\033[K";

      if (is_paused) {
        VLINK_TERM_OUT << "\033[33m"
                       << "Message Parsed by vlink-efbs (Paused):"
                       << "\033[0m" << std::endl;
      } else {
        VLINK_TERM_OUT << "Message Parsed by vlink-efbs:" << std::endl;
      }

      redraw_url_title(show_data_dot, false);
      is_url_title_with_dot = show_data_dot;
      mark_url_title_rendered(show_data_dot);
      VLINK_TERM_OUT.flush();

      if (!current_str.empty()) {
        if (!current_str.empty()) {
          VLINK_TERM_OUT << "\033[32m";

          auto print_split_view_list = vlink::Helpers::split_view(current_str, '\n');

          for (size_t i = 0; i < print_split_view_list.size(); ++i) {
            VLINK_TERM_OUT << "\033[K";
            VLINK_TERM_OUT << print_split_view_list[i];

            if (i < print_split_view_list.size() - 1) {
              VLINK_TERM_OUT << "\n";

              if (i > 0 && i % kFlushMinLine == 0) {
                VLINK_TERM_OUT.flush();
                if constexpr (kFlushMinSleep > 0) {
                  std::this_thread::sleep_for(std::chrono::microseconds(kFlushMinSleep));
                }
              }
            }
          }

          VLINK_TERM_OUT << "\033[0m";
          VLINK_TERM_OUT << "\033[K";
          VLINK_TERM_OUT << std::endl;
        }
      }
    }

    if (current_line < terminal_size.second - 3) {
      for (int i = 0; i < terminal_size.second - 3 - current_line; ++i) {
        VLINK_TERM_OUT << "\033[K";
        VLINK_TERM_OUT << std::endl;
      }
    }

    std::string last_line_str;

    last_line_str = std::string("\033[44;37;1m") + std::string("<") +
                    (total_page == 0 ? std::string("0") : std::to_string(current_page + 1)) + std::string("/") +
                    ((total_page >= 5000 || is_out_of_range) ? std::string("5000+") : std::to_string(total_page)) +
                    std::string(">") + std::string("\033[0m [ ");

    if (print_enum_string) {
      last_line_str += "\033[4mE\033[0m ";
    } else {
      last_line_str += "\033[0mE\033[0m ";
    }

    if (ignore_array) {
      last_line_str += "\033[4mR\033[0m ";
    } else {
      last_line_str += "\033[0mR\033[0m ";
    }

    if (ignore_string) {
      last_line_str += "\033[4mT\033[0m ";
    } else {
      last_line_str += "\033[0mT\033[0m ";
    }

    if (print_time_string) {
      last_line_str += "\033[4mY\033[0m ";
    } else {
      last_line_str += "\033[0mY\033[0m ";
    }

    if (print_hex_string) {
      last_line_str += "\033[4mU\033[0m ";
    } else {
      last_line_str += "\033[0mU\033[0m ";
    }

    if (ignore_default) {
      last_line_str += "\033[4mO\033[0m ";
    } else {
      last_line_str += "\033[0mO\033[0m ";
    }

    if (use_long_repeated) {
      last_line_str += "\033[4mP\033[0m ";
    } else {
      last_line_str += "\033[0mP\033[0m ";
    }

    last_line_str += std::string("] ");

    VLINK_TERM_OUT << "\033[K";

    if VLIKELY (last_line_str.size() <= static_cast<size_t>(terminal_size.first + 69)) {
      VLINK_TERM_OUT << last_line_str;
    } else {
      VLINK_TERM_OUT << last_line_str.substr(0, terminal_size.first + 69);
    }

    VLINK_TERM_OUT.flush();

    is_changed = false;
    is_out_of_range = false;
    force_update = false;
  };

  auto listen_bytes_function = [&parser_loop, &update_terminal_function, &current_bytes, &bytes_mtx, &has_new_data,
                                &frame_seq, &last_frame_timestamp_ms](const vlink::Bytes& bytes) {
    if VUNLIKELY (has_quit) {
      return;
    }

    std::lock_guard lock(bytes_mtx);

    if VLIKELY (!is_paused) {
      current_bytes = bytes;
      has_new_data = true;
      ++frame_seq;
      last_frame_timestamp_ms.store(vlink::ElapsedTimer::get_cpu_timestamp(vlink::ElapsedTimer::kMilli));
      is_changed = true;

      if (!has_printed) {
        has_printed = true;
        parser_loop->post_task(update_terminal_function);
      }
    }
  };

  bool should_use_getter = use_getter || (!has_explicit_ser && ((target_type & vlink::kSetter) != 0));

  try {
    if (should_use_getter) {
      raw_getter.emplace(std::make_shared<RawGetter>(url, vlink::InitType::kWithoutInit));

      if (native_mode) {
        (*raw_getter)->set_property("dds.ip", native_ip);
      }

      (*raw_getter)->set_ser_type(target_ser, target_schema_type);
      (*raw_getter)->init();
      (*raw_getter)->listen(listen_bytes_function);
    } else {
      raw_sub.emplace(std::make_shared<RawSub>(url, vlink::InitType::kWithoutInit));

      if (native_mode) {
        (*raw_sub)->set_property("dds.ip", native_ip);
      }

      (*raw_sub)->set_ser_type(target_ser, target_schema_type);
      (*raw_sub)->init();
      (*raw_sub)->listen(listen_bytes_function);
    }
  } catch (vlink::Exception::RuntimeError& e) {
    std::cerr << e.what() << std::endl;
    has_quit = true;
    return -1;
  }

  auto reset_frame_rate_state = [&has_new_data, &frame_seq, &frame_seq_buffer, &current_frame_rate, &current_rate_color,
                                 &last_frame_timestamp_ms]() {
    has_new_data = false;
    frame_seq.store(0);
    frame_seq_buffer.clear();
    current_frame_rate = 0.0;
    current_rate_color = 33;

    if (last_frame_timestamp_ms.load() == 0) {
      last_frame_timestamp_ms.store(vlink::ElapsedTimer::get_cpu_timestamp(vlink::ElapsedTimer::kMilli));
    }
  };

  vlink::Utils::start_detect_keyboard(
      [&parser_loop, &update_terminal_function, &quit_function, &reset_frame_rate_state](const std::string& key) {
        // VLINK_TERM_OUT << "key:" << key << std::endl;

        if (key == "q" || key == "esc") {
          quit_function(0);
        } else if (key == " ") {
          if (is_paused) {
            is_paused = false;
            is_changed = true;
            force_update = true;
            parser_loop->post_task(update_terminal_function);
          } else {
            is_paused = true;
            parser_loop->post_task([&reset_frame_rate_state, &update_terminal_function]() {
              reset_frame_rate_state();
              is_changed = true;
              force_update = true;
              update_terminal_function();
            });
          }
        } else if (key == "left") {
          if (current_page >= 1) {
            --current_page;
            is_changed = true;
            parser_loop->post_task(update_terminal_function);
          }
        } else if (key == "right") {
          if (current_page < total_page - 1) {
            ++current_page;
            is_changed = true;
            parser_loop->post_task(update_terminal_function);
          }
        } else if (key == "up") {
          if (current_page >= 10) {
            current_page -= 10;
            is_changed = true;
            parser_loop->post_task(update_terminal_function);
          } else if (current_page >= 1) {
            current_page = 0;
            is_changed = true;
            parser_loop->post_task(update_terminal_function);
          }
        } else if (key == "down") {
          if (current_page < total_page - 10) {
            current_page += 10;
            is_changed = true;
            parser_loop->post_task(update_terminal_function);
          } else if (current_page < total_page - 1) {
            current_page = total_page - 1;
            is_changed = true;
            parser_loop->post_task(update_terminal_function);
          }
        } else if (key == "pgup") {
          if (current_page >= 100) {
            current_page -= 100;
            is_changed = true;
            parser_loop->post_task(update_terminal_function);
          } else if (current_page >= 1) {
            current_page = 0;
            is_changed = true;
            parser_loop->post_task(update_terminal_function);
          }
        } else if (key == "pgdown") {
          if (current_page < total_page - 100) {
            current_page += 100;
            is_changed = true;
            parser_loop->post_task(update_terminal_function);
          } else if (current_page < total_page - 1) {
            current_page = total_page - 1;
            is_changed = true;
            parser_loop->post_task(update_terminal_function);
          }
        } else if (key == "home") {
          if (current_page != 0) {
            current_page = 0;
            is_changed = true;
            parser_loop->post_task(update_terminal_function);
          }
        } else if (key == "end") {
          if (current_page != total_page - 1) {
            current_page = total_page - 1;
            is_changed = true;
            parser_loop->post_task(update_terminal_function);
          }
        } else if (key == "e") {
          print_enum_string = !print_enum_string;
          is_changed = true;
          force_update = true;
          parser_loop->post_task(update_terminal_function);
        } else if (key == "r") {
          ignore_array = !ignore_array;
          is_changed = true;
          force_update = true;
          parser_loop->post_task(update_terminal_function);
        } else if (key == "t") {
          ignore_string = !ignore_string;
          is_changed = true;
          force_update = true;
          parser_loop->post_task(update_terminal_function);
        } else if (key == "y") {
          print_time_string = !print_time_string;
          is_changed = true;
          force_update = true;
          parser_loop->post_task(update_terminal_function);
        } else if (key == "u") {
          print_hex_string = !print_hex_string;
          is_changed = true;
          force_update = true;
          parser_loop->post_task(update_terminal_function);
        } else if (key == "o") {
          ignore_default = !ignore_default;
          is_changed = true;
          force_update = true;
          parser_loop->post_task(update_terminal_function);
        } else if (key == "p") {
          use_long_repeated = !use_long_repeated;
          is_changed = true;
          force_update = true;
          parser_loop->post_task(update_terminal_function);
        }
      });

  vlink::Timer terminal_timer;
  terminal_timer.set_interval(kTerminalInterval);
  terminal_timer.set_loop_count(vlink::Timer::kInfinite);
  terminal_timer.attach(parser_loop.get());
  terminal_timer.set_callback(update_terminal_function);
  terminal_timer.start();

  vlink::Timer stats_timer;
  stats_timer.set_interval(kCollectInterval);
  stats_timer.set_loop_count(vlink::Timer::kInfinite);
  stats_timer.attach(parser_loop.get());
  stats_timer.set_callback(
      [&frame_seq, &frame_seq_buffer, &current_frame_rate, &current_rate_color, &last_frame_timestamp_ms]() {
        double freq = 0;
        int weight = 1;
        int total_weight = 0;

        if VUNLIKELY (!has_printed) {
          return;
        }

        frame_seq_buffer.emplace_back(frame_seq.exchange(0));
        uint64_t now_ms = vlink::ElapsedTimer::get_cpu_timestamp(vlink::ElapsedTimer::kMilli);
        uint64_t last_frame_ms = last_frame_timestamp_ms.load();

        while (frame_seq_buffer.size() > kCounterCache) {
          frame_seq_buffer.pop_front();
        }

        for (auto seq : frame_seq_buffer) {
          freq += seq * weight;
          total_weight += weight;
          weight *= kCounterWeight;
        }

        if (total_weight > 0) {
          current_frame_rate = freq / total_weight;
        } else {
          current_frame_rate = 0.0;
        }

        if (frame_seq_buffer.back() > 0 && frame_seq_buffer.size() >= kCounterCache) {
          current_rate_color = 32;
        } else if (last_frame_ms > 0 && (now_ms - last_frame_ms) > kCollectInterval * kCounterCache) {
          current_frame_rate = 0.0;
          frame_seq_buffer.clear();
          current_rate_color = 31;
        } else {
          current_rate_color = 33;
        }
      });
  stats_timer.start();

  vlink::Timer::call_once(parser_loop.get(), 250, [&parser_loop, &update_terminal_function]() {
    if VUNLIKELY (has_quit) {
      return;
    }

    if VLIKELY (!has_printed) {
      force_update = true;
      is_changed = true;
      parser_loop->post_task(update_terminal_function);
    }
  });

  if (discovery_viewer) {
    discovery_viewer->wait_for_quit();
  }

  parser_loop->wait_for_quit();

  has_quit = true;

  vlink::Utils::stop_detect_keyboard();

  if (parse_ret == 0) {
    // VLINK_TERM_OUT << std::endl;
    VLINK_TERM_OUT << "\033[H\033[J";
    VLINK_TERM_OUT.flush();
  }

  raw_sub.reset();
  raw_getter.reset();
  parser.reset();
  discovery_viewer.reset();
  parser_loop.reset();

  return parse_ret;
}

int main(int argc, char* argv[]) {
  std::ios::sync_with_stdio(false);
  vlink::Utils::set_console_utf8_output();

  VLINK_TERM_OUT.init();

  // init
  vlink::Logger::set_console_level(vlink::Logger::kOff);
  vlink::Logger::set_file_level(vlink::Logger::kOff);
  vlink::Logger::init("vlink-efbs");

  // env
  vlink::Utils::unset_env("VLINK_BAG_PATH");
  // vlink::Utils::set_env("VLINK_DISCOVER_DISABLE", "1");

  // intra_bind
  std::string intra_bind = vlink::Utils::get_env("VLINK_INTRA_BIND");

  if (!intra_bind.empty()) {
    has_intra_bind = true;
  }

  // arg parser
  argparse::ArgumentParser program("vlink-efbs", VLINK_VERSION, argparse::default_arguments::all);

  program.add_description("Note: You may need to add multicast/broadcast [" +
                          vlink::DiscoveryViewer::get_listen_address() + "]");

  argparse::ArgumentParser pub_command("pub", VLINK_VERSION, argparse::default_arguments::help);
  pub_command.add_argument("url").help("Bind url").required();
  pub_command.add_argument("-d", "--fbs_dir").help("Fbs dir").default_value(std::string());
  pub_command.add_argument("--schema_plugin").help("Path to schema plugin shared library").default_value(std::string());
  pub_command.add_argument("-s", "--ser_type").help("Serialization type").default_value(std::string());
  pub_command.add_argument("-x", "--encoding")
      .help("Encoding (protobuf/flatbuffers/raw/blob/zerocopy, blob sends binary bytes: -c hex / -f file)")
      .default_value(std::string());
  pub_command.add_argument("-n", "--native").help("Native mode").default_value(false).implicit_value(true);
  pub_command.add_argument("-f", "--fbstxt_file").help("Fbs txt file").default_value(std::string());
  pub_command.add_argument("-c", "--fbstxt_content").help("Fbs txt content").default_value(std::string());

  pub_command.add_argument("-t", "--times")
      .help("Pub times, times <= 0 means infinite")
      .scan<'d', int>()
      // NOLINTNEXTLINE(readability-redundant-casting)
      .default_value(static_cast<int>(1));
  pub_command.add_argument("-l", "--interval")
      .help("Pub interval(ms)")
      .scan<'d', int>()
      // NOLINTNEXTLINE(readability-redundant-casting)
      .default_value(static_cast<int>(100));

  pub_command.add_description("Publish data");

  std::string pub_example_str = "Example:\n  vlink-efbs pub shm://test -d /home/fbs_dir -s pb.Test -f test.fbstxt";
  pub_example_str += "\n  ";
  pub_example_str += "vlink-efbs pub shm://test -d /home/fbs_dir -s pb.Test -c '{ width: 800, height: 600 }'";

  pub_command.add_epilog(std::move(pub_example_str));

  argparse::ArgumentParser sub_command("sub", VLINK_VERSION, argparse::default_arguments::help);
  sub_command.add_argument("url").help("Bind url").required();
  sub_command.add_argument("-d", "--fbs_dir").help("Fbs dir").default_value(std::string());
  sub_command.add_argument("--schema_plugin").help("Path to schema plugin shared library").default_value(std::string());
  sub_command.add_argument("-s", "--ser_type").help("Serialization type").default_value(std::string());
  sub_command.add_argument("-x", "--encoding")
      .help("Encoding (protobuf/flatbuffers/raw/blob/zerocopy)")
      .default_value(std::string());
  sub_command.add_argument("-i", "--filter")
      .help("Property filter list, comma-separated or quoted space-separated")
      .default_value(std::string());
  sub_command.add_argument("-g", "--getter")
      .help("Use getter to receive data")
      .default_value(false)
      .implicit_value(true);
  sub_command.add_argument("-k", "--black").help("Blacklist mode").default_value(false).implicit_value(true);
  sub_command.add_argument("-n", "--native").help("Native mode").default_value(false).implicit_value(true);
  sub_command.add_argument("-m", "--max_str_count")
      .help("Max string count")
      .scan<'u', uint64_t>()
      // NOLINTNEXTLINE(readability-redundant-casting)
      .default_value(static_cast<uint64_t>(1000'00000UL));

  sub_command.add_argument("-e", "--print_enum_string")
      .help("Print enum number (Hot key)")
      .default_value(false)
      .implicit_value(true);
  sub_command.add_argument("-r", "--ignore_array")
      .help("Ignore array (Hot key)")
      .default_value(false)
      .implicit_value(true);
  sub_command.add_argument("-t", "--ignore_string")
      .help("Ignore string (Hot key)")
      .default_value(false)
      .implicit_value(true);
  sub_command.add_argument("-y", "--print_time_string")
      .help("Print time string (Hot key)")
      .default_value(false)
      .implicit_value(true);
  sub_command.add_argument("-u", "--print_hex_string")
      .help("Print hex string (Hot key)")
      .default_value(false)
      .implicit_value(true);
  sub_command.add_argument("-o", "--ignore_default")
      .help("Ignore default value (Hot key)")
      .default_value(false)
      .implicit_value(true);
  sub_command.add_argument("-p", "--use_long_repeated")
      .help("Use long repeated (Hot key)")
      .default_value(false)
      .implicit_value(true);

  sub_command.add_argument("--rows")
      .help("Maximum rows(0 means automatic)")
      .scan<'d', int>()
      // NOLINTNEXTLINE(readability-redundant-casting)
      .default_value(static_cast<int>(0));
  sub_command.add_argument("--columns")
      .help("Maximum columns(0 means automatic)")
      .scan<'d', int>()
      // NOLINTNEXTLINE(readability-redundant-casting)
      .default_value(static_cast<int>(0));

  sub_command.add_description("Subscribe data");

  sub_command.add_epilog("Example:\n  vlink-efbs sub shm://test -d /home/fbs_dir -s pb.Test");

  argparse::ArgumentParser import_command("import", VLINK_VERSION, argparse::default_arguments::help);
  import_command.add_argument("dir").help("Fbs dir path to persist").required();
  import_command.add_description("Save fbs dir to $HOME/.vlink_fbs_dir");
  import_command.add_epilog("Example:\n  vlink-efbs import /home/fbs_dir");

  program.add_subparser(pub_command);
  program.add_subparser(sub_command);
  program.add_subparser(import_command);

  try {
    program.parse_args(argc, argv);
  } catch (const std::exception& e) {
    std::cerr << e.what() << std::endl;

    if (program.is_subcommand_used("pub")) {
      std::cerr << pub_command << std::endl;
    } else if (program.is_subcommand_used("sub")) {
      std::cerr << sub_command << std::endl;
    } else if (program.is_subcommand_used("import")) {
      std::cerr << import_command << std::endl;
    }

    return 1;
  }

  int ret = 0;
  std::string fbs_dir;

  if (program.is_subcommand_used("pub")) {
    const auto& url = pub_command.get<std::string>("url");
    fbs_dir = pub_command.get<std::string>("-d");
    auto schema_plugin_path = pub_command.get<std::string>("--schema_plugin");

    if (fbs_dir.empty()) {
      fbs_dir = vlink::Utils::get_env("VLINK_FBS_DIR");
    }

    if (fbs_dir.empty()) {
      fbs_dir = read_home_config(".vlink_fbs_dir");
    }

    if (schema_plugin_path.empty()) {
      schema_plugin_path = vlink::Utils::get_env("VLINK_SCHEMA_PLUGIN");
    }

#ifdef _WIN32

    if (pub_command.is_used("-d")) {
      try {
        fbs_dir = vlink::Helpers::path_to_string(std::filesystem::path(fbs_dir));
      } catch (std::filesystem::filesystem_error&) {
      }
    }

    if (pub_command.is_used("--schema_plugin")) {
      try {
        schema_plugin_path = vlink::Helpers::path_to_string(std::filesystem::path(schema_plugin_path));
      } catch (std::filesystem::filesystem_error&) {
      }
    }
#endif

    auto fbstxt_file = pub_command.get<std::string>("-f");
    auto fbstxt_content = pub_command.get<std::string>("-c");
    const auto& ser = pub_command.get<std::string>("-s");
    auto encoding = pub_command.get<std::string>("-x");
    std::transform(encoding.begin(), encoding.end(), encoding.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    auto use_blob_encoding = encoding == "blob";
    auto schema_type = vlink::SchemaData::convert_encoding(encoding);

    auto native_mode = pub_command.is_used("-n");

    int times = pub_command.get<int>("-t");
    int interval = pub_command.get<int>("-l");

    if VUNLIKELY (fbstxt_file.empty() && fbstxt_content.empty()) {
      std::cerr << "One of fbstxt_file and fbstxt_content must be specified." << std::endl;
      return -1;
    } else if VUNLIKELY (!fbstxt_file.empty() && !fbstxt_content.empty()) {
      std::cerr << "One of fbstxt_file and fbstxt_content must be specified." << std::endl;
      return -1;
    } else if VUNLIKELY (schema_type == vlink::SchemaType::kUnknown && !encoding.empty() && encoding != "unknown") {
      std::cerr << "Invalid encoding." << std::endl;
      return -1;
    }

#ifdef _WIN32
    std::replace(fbs_dir.begin(), fbs_dir.end(), '\\', '/');
    std::replace(schema_plugin_path.begin(), schema_plugin_path.end(), '\\', '/');

    if (!fbstxt_file.empty()) {
      std::replace(fbstxt_file.begin(), fbstxt_file.end(), '\\', '/');
    }
#endif

    if (!fbs_dir.empty() && fbs_dir.back() == '/') {
      fbs_dir.pop_back();
    }

    if (!schema_plugin_path.empty() && schema_plugin_path.back() == '/') {
      schema_plugin_path.pop_back();
    }

    if (!fbstxt_file.empty()) {
      if (fbstxt_file.back() == '/') {
        fbstxt_file.pop_back();
      }
    }

    (void)vlink::SchemaPluginManager::get(schema_plugin_path);
    ret = start_efbs_pub(url, fbs_dir, fbstxt_file, fbstxt_content, ser, schema_type, use_blob_encoding, native_mode,
                         times, interval);

    return ret;

  } else if (program.is_subcommand_used("sub")) {
    const auto& url = sub_command.get<std::string>("url");
    const auto& ser = sub_command.get<std::string>("-s");
    auto encoding = sub_command.get<std::string>("-x");
    std::transform(encoding.begin(), encoding.end(), encoding.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    auto use_blob_encoding = encoding == "blob";
    auto schema_type = vlink::SchemaData::convert_encoding(encoding);
    fbs_dir = sub_command.get<std::string>("-d");
    auto schema_plugin_path = sub_command.get<std::string>("--schema_plugin");

    if (fbs_dir.empty()) {
      fbs_dir = vlink::Utils::get_env("VLINK_FBS_DIR");
    }

    if (fbs_dir.empty()) {
      fbs_dir = read_home_config(".vlink_fbs_dir");
    }

    if (schema_plugin_path.empty()) {
      schema_plugin_path = vlink::Utils::get_env("VLINK_SCHEMA_PLUGIN");
    }

#ifdef _WIN32

    if (sub_command.is_used("-d")) {
      try {
        fbs_dir = vlink::Helpers::path_to_string(std::filesystem::path(fbs_dir));
      } catch (std::filesystem::filesystem_error&) {
      }
    }

    if (sub_command.is_used("--schema_plugin")) {
      try {
        schema_plugin_path = vlink::Helpers::path_to_string(std::filesystem::path(schema_plugin_path));
      } catch (std::filesystem::filesystem_error&) {
      }
    }
#endif

    auto native_mode = sub_command.is_used("-n");
    auto filter = sub_command.get<std::string>("-i");
    auto use_getter = sub_command.is_used("-g");

    if VUNLIKELY (schema_type == vlink::SchemaType::kUnknown && !encoding.empty() && encoding != "unknown") {
      std::cerr << "Invalid encoding." << std::endl;
      return -1;
    }

    black_mode = sub_command.is_used("-k");

    print_enum_string = sub_command.is_used("-e");
    ignore_array = sub_command.is_used("-r");
    ignore_string = sub_command.is_used("-t");
    print_time_string = sub_command.is_used("-y");
    print_hex_string = sub_command.is_used("-u");
    ignore_default = sub_command.is_used("-o");
    use_long_repeated = sub_command.is_used("-p");

    max_str_count = sub_command.get<uint64_t>("-m");

    max_rows = sub_command.get<int>("--rows");

    max_columns = sub_command.get<int>("--columns");

#ifdef _WIN32
    std::replace(fbs_dir.begin(), fbs_dir.end(), '\\', '/');
    std::replace(schema_plugin_path.begin(), schema_plugin_path.end(), '\\', '/');
#endif

    if (!fbs_dir.empty() && fbs_dir.back() == '/') {
      fbs_dir.pop_back();
    }

    if (!schema_plugin_path.empty() && schema_plugin_path.back() == '/') {
      schema_plugin_path.pop_back();
    }

    (void)vlink::SchemaPluginManager::get(schema_plugin_path);

    VLINK_TERM_OUT << "\033[?25l";
    VLINK_TERM_OUT.flush();

    ret = start_efbs_sub(url, fbs_dir, ser, schema_type, use_blob_encoding, native_mode, filter, use_getter);

    VLINK_TERM_OUT << "\033[?25h";
    VLINK_TERM_OUT.flush();

    return ret;
  } else if (program.is_subcommand_used("import")) {
    auto dir = import_command.get<std::string>("dir");

#ifdef _WIN32
    try {
      dir = vlink::Helpers::path_to_string(std::filesystem::path(dir));
    } catch (const std::filesystem::filesystem_error&) {
    } catch (const std::exception&) {
    }

    std::replace(dir.begin(), dir.end(), '\\', '/');
#endif

    while (!dir.empty() && dir.back() == '/') {
      dir.pop_back();
    }

    if VUNLIKELY (dir.empty()) {
      std::cerr << "Fbs dir cannot be empty." << std::endl;
      return -1;
    }

    auto input_path = utf8_to_path(dir);

    if VUNLIKELY (input_path.empty()) {
      std::cerr << "Invalid fbs dir path: " << dir << "." << std::endl;
      return -1;
    }

    std::error_code ec;
    auto absolute_path = std::filesystem::absolute(input_path, ec);

    if VUNLIKELY (ec) {
      std::cerr << "Invalid fbs dir path: " << dir << " (" << ec.message() << ")." << std::endl;
      return -1;
    }

    if VUNLIKELY (!std::filesystem::exists(absolute_path, ec) || ec) {
      std::cerr << "Fbs dir does not exist: " << vlink::Helpers::path_to_string(absolute_path) << "." << std::endl;
      return -1;
    }

    if VUNLIKELY (!std::filesystem::is_directory(absolute_path, ec) || ec) {
      std::cerr << "Fbs dir is not a directory: " << vlink::Helpers::path_to_string(absolute_path) << "." << std::endl;
      return -1;
    }

    auto saved = vlink::Helpers::path_to_string(absolute_path);

#ifdef _WIN32
    std::replace(saved.begin(), saved.end(), '\\', '/');
#endif

    while (saved.size() > 1 && saved.back() == '/') {
      saved.pop_back();
    }

    if VUNLIKELY (saved.empty()) {
      std::cerr << "Failed to resolve absolute path for: " << dir << "." << std::endl;
      return -1;
    }

    auto config_path = get_home_config_path(".vlink_fbs_dir");

    if VUNLIKELY (config_path.empty()) {
      std::cerr << "Cannot resolve HOME directory." << std::endl;
      return -1;
    }

    if VUNLIKELY (!write_home_config(".vlink_fbs_dir", saved)) {
      std::cerr << "Failed to write " << config_path << "." << std::endl;
      return -1;
    }

    std::cout << "Saved VLINK_FBS_DIR to " << config_path << ": " << saved << std::endl;

    return 0;
  }

  std::cerr << program << std::endl;

  return 1;
}
