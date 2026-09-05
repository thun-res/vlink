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

#include "./efbs_common.h"

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
  try {
    raw_pub->init();
  } catch (vlink::Exception::RuntimeError& e) {
    std::cerr << e.what() << std::endl;
    has_quit = true;
    return -1;
  }

  if (!is_text_type && !is_blob_type) {
    if (fbstxt_file.empty()) {
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
