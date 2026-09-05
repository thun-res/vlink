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

    if VUNLIKELY (pub_command.is_used("-f") == pub_command.is_used("-c")) {
      std::cerr << "One of fbstxt_file and fbstxt_content must be specified." << std::endl;
      return -1;
    } else if VUNLIKELY (pub_command.is_used("-f") && fbstxt_file.empty()) {
      std::cerr << "Fbs txt file path cannot be empty." << std::endl;
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

    if (!schema_plugin_path.empty() && schema_plugin_path.back() == '/') {
      schema_plugin_path.pop_back();
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
