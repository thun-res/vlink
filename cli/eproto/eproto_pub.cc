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

#include "./eproto_common.h"

#ifdef VLINK_HAS_PROTOBUF_COMPILER

int start_eproto_pub(const std::string& url, const std::string& proto_dir, const std::string& prototxt_file,
                     const std::string& prototxt_content, const std::string& ser, vlink::SchemaType schema_type,
                     bool use_blob_encoding, bool native_mode, int times, int interval, bool use_json_format) {
  GOOGLE_PROTOBUF_VERIFY_VERSION;

  const std::string native_ip = native_mode ? vlink::Utils::get_env("VLINK_DDS_NATIVE_IP", "127.0.0.1") : std::string();

  if VUNLIKELY (!has_intra_bind && vlink::Url::is_intra_type(url)) {
    std::cerr << "Cannot pub intra url." << std::endl;
    has_quit = true;
    return -1;
  }

  std::shared_ptr<vlink::DiscoveryViewer> discovery_viewer;
  std::shared_ptr<google::protobuf::compiler::DiskSourceTree> source_tree;
  std::shared_ptr<google::protobuf::compiler::Importer> importer;
  std::shared_ptr<google::protobuf::DynamicMessageFactory> factory;
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
    target_schema_type = vlink::SchemaType::kProtobuf;
  }

  const auto inferred_schema_type = vlink::SchemaData::infer_ser_type(target_ser);

  if VUNLIKELY (target_schema_type != vlink::SchemaType::kRaw && inferred_schema_type != vlink::SchemaType::kUnknown &&
                inferred_schema_type != target_schema_type) {
    std::cerr << "ser_type and encoding do not match." << std::endl;
    has_quit = true;
    return -1;
  }

  if VUNLIKELY (target_schema_type == vlink::SchemaType::kFlatbuffers) {
    std::cerr << "eproto pub does not support flatbuffers schema_type." << std::endl;
    has_quit = true;
    return -1;
  }

  if VUNLIKELY (discovery_viewer && discovery_viewer->is_ready_to_quit()) {
    has_quit = true;

    raw_pub.reset();
    factory.reset();
    importer.reset();
    source_tree.reset();
    discovery_viewer.reset();

    return 0;
  }

  const bool is_blob_type = target_schema_type == vlink::SchemaType::kRaw && use_blob_encoding;
  bool is_text_type = !is_blob_type && (target_schema_type == vlink::SchemaType::kRaw || is_text_ser_type(target_ser));
  const bool is_zerocopy_type = target_schema_type == vlink::SchemaType::kZeroCopy;

  if VUNLIKELY (is_zerocopy_type) {
    std::cerr << "eproto pub only supports protobuf or raw text/json payloads." << std::endl;
    has_quit = true;
    return -1;
  }

#ifndef VLINK_HAS_PROTOBUF_JSON_UTIL

  if VUNLIKELY (use_json_format && target_schema_type == vlink::SchemaType::kProtobuf) {
    std::cerr << "Current protobuf does not support JSON conversion." << std::endl;
    has_quit = true;
    return -1;
  }
#endif

  if (!prototxt_file.empty()) {
    try {
#ifdef _WIN32
      auto filesys_prototxt_file = std::filesystem::path(vlink::Helpers::string_to_wstring(prototxt_file));
#else
      auto filesys_prototxt_file = std::filesystem::path(prototxt_file);
#endif

      if VUNLIKELY (!std::filesystem::exists(filesys_prototxt_file)) {
        std::cerr << "Proto txt file does not exist." << std::endl;
        has_quit = true;
        return -1;
      }

      if VUNLIKELY (!std::filesystem::is_regular_file(filesys_prototxt_file)) {
        std::cerr << "Proto txt file is not a file." << std::endl;
        has_quit = true;
        return -1;
      }
    } catch (std::filesystem::filesystem_error& e) {
      std::cerr << e.what() << std::endl;
      has_quit = true;
      return -1;
    }
  }

  factory = std::make_shared<google::protobuf::DynamicMessageFactory>();
  source_tree = std::make_shared<google::protobuf::compiler::DiskSourceTree>();
  importer = std::make_shared<google::protobuf::compiler::Importer>(source_tree.get(), nullptr);
  std::unique_ptr<google::protobuf::Message> root_msg;
  vlink::Bytes raw_data;

  if (is_blob_type) {
    if (prototxt_file.empty()) {
      bool ok = false;
      raw_data = vlink::Bytes::from_user_input(prototxt_content, &ok);

      if VUNLIKELY (!ok) {
        std::cerr << "Blob content must be hex bytes." << std::endl;
        has_quit = true;
        return -1;
      }
    } else {
      std::string blob_payload;

      if VUNLIKELY (!load_text_for_file(prototxt_file, blob_payload)) {
        std::cerr << "load_text_for_file failed." << std::endl;
        has_quit = true;
        return -1;
      }

      raw_data = vlink::Bytes::from_string(blob_payload);
    }
  } else if (is_text_type) {
    std::string text_payload;

    if (prototxt_file.empty()) {
      text_payload = prototxt_content;
    } else if VUNLIKELY (!load_text_for_file(prototxt_file, text_payload)) {
      std::cerr << "load_text_for_file failed." << std::endl;
      has_quit = true;
      return -1;
    }

    raw_data = vlink::Bytes::from_string(text_payload);
  } else {
    google::protobuf::Descriptor* descriptor = nullptr;

    auto schema_interface = vlink::SchemaPluginManager::get().get_interface();

    if (schema_interface) {
      descriptor = static_cast<google::protobuf::Descriptor*>(schema_interface->search_protobuf_descriptor(target_ser));
    } else {
      if VUNLIKELY (proto_dir.empty()) {
        std::cerr << "Must set proto dir [-d], set env 'VLINK_PROTO_DIR', run 'vlink-eproto import <dir>', or load "
                     "VLINK_SCHEMA_PLUGIN."
                  << std::endl;
        has_quit = true;
        return 1;
      }

      bool has_import = false;

      try {
#ifdef _WIN32
        auto proto_path = std::filesystem::path(vlink::Helpers::string_to_wstring(proto_dir));
#else
        auto proto_path = std::filesystem::path(proto_dir);
#endif

        if VUNLIKELY (!std::filesystem::exists(proto_path)) {
          std::cerr << "Proto dir does not exist." << std::endl;
          has_quit = true;
          return -1;
        }

        if VUNLIKELY (!std::filesystem::is_directory(proto_path)) {
          std::cerr << "Proto dir is not a directory." << std::endl;
          has_quit = true;
          return -1;
        }

#ifdef _WIN32
        source_tree->MapPath("", vlink::Helpers::path_to_string(proto_path));
#else
        source_tree->MapPath("", proto_path.string());
#endif

        import_protos(importer.get(), proto_path, proto_path, has_import);
      } catch (std::filesystem::filesystem_error& e) {
        std::cerr << e.what() << std::endl;
        has_quit = true;
        return -1;
      }

      if VUNLIKELY (!has_import) {
        std::cerr << "Import proto dir failed." << std::endl;
        has_quit = true;
        return -1;
      }

      auto* des_pool = const_cast<google::protobuf::DescriptorPool*>(importer->pool());

      if VUNLIKELY (!des_pool) {
        std::cerr << "Cannot find proto." << std::endl;
        has_quit = true;
        return -1;
      }

      descriptor = const_cast<google::protobuf::Descriptor*>(des_pool->FindMessageTypeByName(target_ser));
    }

    if VUNLIKELY (!descriptor) {
      std::cerr << "Cannot find ser." << std::endl;
      has_quit = true;
      return -1;
    }

    root_msg.reset(factory->GetPrototype(descriptor)->New());

    if VUNLIKELY (!root_msg) {
      std::cerr << "Create root msg failed." << std::endl;
      has_quit = true;
      return -1;
    }

    if (use_json_format) {
#ifdef VLINK_HAS_PROTOBUF_JSON_UTIL
      std::string json_error;

      if (prototxt_file.empty()) {
        if VUNLIKELY (!load_proto_for_json_string(prototxt_content, root_msg.get(), &json_error)) {
          std::cerr << "load_proto_for_json_string failed.";
          if (!json_error.empty()) {
            std::cerr << " " << json_error;
          }
          std::cerr << std::endl;
          has_quit = true;
          return -1;
        }
      } else {
        if VUNLIKELY (!load_proto_for_json_file(prototxt_file, root_msg.get(), &json_error)) {
          std::cerr << "load_proto_for_json_file failed.";
          if (!json_error.empty()) {
            std::cerr << " " << json_error;
          }
          std::cerr << std::endl;
          has_quit = true;
          return -1;
        }
      }
#endif
    } else {
      if (prototxt_file.empty()) {
        if VUNLIKELY (!load_proto_for_string(prototxt_content, root_msg.get())) {
          std::cerr << "load_proto_for_string failed." << std::endl;
          has_quit = true;
          return -1;
        }
      } else {
        if VUNLIKELY (!load_proto_for_file(prototxt_file, root_msg.get())) {
          std::cerr << "load_proto_for_file failed." << std::endl;
          has_quit = true;
          return -1;
        }
      }
    }

    raw_data = vlink::Bytes::create(root_msg->ByteSizeLong());

    bool ret = root_msg->SerializePartialToArray(raw_data.data(), raw_data.size());

    if VUNLIKELY (!ret) {
      raw_data.clear();
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

  root_msg.reset();

  raw_pub.reset();
  factory.reset();
  importer.reset();
  source_tree.reset();
  discovery_viewer.reset();

  return 0;
}

#endif
