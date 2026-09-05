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

#include "./monitor_common.h"

int main(int argc, char* argv[]) {
  std::ios::sync_with_stdio(false);
  vlink::Utils::set_console_utf8_output();

  VLINK_TERM_OUT.init();

  // init
  vlink::Logger::set_console_level(vlink::Logger::kOff);
  vlink::Logger::set_file_level(vlink::Logger::kOff);
  vlink::Logger::init("vlink-monitor");

  // env
  vlink::Utils::unset_env("VLINK_BAG_PATH");

  // intra_bind
  std::string intra_bind = vlink::Utils::get_env("VLINK_INTRA_BIND");

  if (!intra_bind.empty()) {
    has_intra_bind = true;
  }

  // arg parser
  argparse::ArgumentParser program("vlink-monitor", VLINK_VERSION, argparse::default_arguments::all);

  program.add_description("Note: You may need to add multicast/broadcast [" +
                          vlink::DiscoveryViewer::get_listen_address() + "]");

  program.add_argument("-u", "--urls")
      .help("Bind urls")
      .default_value(std::vector<std::string>())
      .nargs(argparse::nargs_pattern::any);
  program.add_argument("-i", "--filter")
      .help("URL keyword filter, comma-separated or quoted space-separated")
      .default_value(std::string());
  program.add_argument("--hostname")
      .help("Hostname keyword filter (not affected by --black), comma-separated or quoted space-separated")
      .default_value(std::string());
  program.add_argument("-b", "--blob")
      .help("Force blob output for Enter jump")
      .default_value(false)
      .implicit_value(true);
  program.add_argument("-k", "--black").help("Blacklist mode").default_value(false).implicit_value(true);
  program.add_argument("-n", "--native").help("Native mode").default_value(false).implicit_value(true);
  program.add_argument("-t", "--node_count").help("Node count mode").default_value(false).implicit_value(true);
  program.add_argument("-l", "--detail").help("Detail mode (Hot key)").default_value(false).implicit_value(true);
  program.add_argument("-o", "--observe_all")
      .help("Observe all mode (Hot key)")
      .default_value(false)
      .implicit_value(true);
  program.add_argument("-e", "--profiler").help("Show profiler (Hot key)").default_value(false).implicit_value(true);
  program.add_argument("-s", "--ser").help("Show serialize type (Hot key)").default_value(false).implicit_value(true);
  program.add_argument("-a", "--active").help("Only show active (Hot key)").default_value(false).implicit_value(true);
  program.add_argument("-y", "--pubsub").help("Only show pub/sub (Hot key)").default_value(false).implicit_value(true);
  program.add_argument("-p", "--process")
      .help("Show process panel (Hot key)")
      .default_value(false)
      .implicit_value(true);
  program.add_argument("-c", "--chart").help("Show chart panel (Hot key)").default_value(false).implicit_value(true);
  program.add_argument("-x", "--preset")
      .help("Preset mode(Same as enabling '-l -o -p -c')")
      .default_value(false)
      .implicit_value(true);

  program.add_argument("-g", "--proto_args").help("Append eproto/efbs args").default_value(std::string());

  program.add_argument("-d", "--proto_dir").help("Proto dir").default_value(std::string());

  program.add_argument("-f", "--fbs_dir").help("Flatbuffers dir").default_value(std::string());

  program.add_argument("--plain")
      .help("Plain text output mode (for redirection)")
      .default_value(false)
      .implicit_value(true);

  program.add_argument("--dot").help("Use chart dot to paint").default_value(false).implicit_value(true);

  program.add_argument("--rows")
      .help("Maximum rows(0 means automatic)")
      .scan<'d', int>()
      // NOLINTNEXTLINE(readability-redundant-casting)
      .default_value(static_cast<int>(0));
  program.add_argument("--columns")
      .help("Maximum columns(0 means automatic)")
      .scan<'d', int>()
      // NOLINTNEXTLINE(readability-redundant-casting)
      .default_value(static_cast<int>(0));

  program.add_argument("--chart_width")
      .help("Chart width")
      .scan<'d', int>()
      // NOLINTNEXTLINE(readability-redundant-casting)
      .default_value(static_cast<int>(30));

  program.add_argument("--process_width")
      .help("Process width")
      .scan<'d', int>()
      // NOLINTNEXTLINE(readability-redundant-casting)
      .default_value(static_cast<int>(40));

  program.add_epilog("Example:\n  vlink-monitor -lo");

  try {
    program.parse_args(argc, argv);
  } catch (const std::exception& e) {
    std::cerr << e.what() << std::endl;
    std::cerr << program << std::endl;
    return 1;
  }

  const auto& urls = program.get<std::vector<std::string>>("-u");
  const auto& filter = program.get<std::string>("-i");
  const auto& hostname_filter = program.get<std::string>("--hostname");

  black_mode = program.is_used("-k");
  blob_mode = program.is_used("-b");
  native_mode = program.is_used("-n");
  count_mode = program.is_used("-t");
  detail_mode = program.is_used("-l");
  observe_all_mode = program.is_used("-o");
  profiler_mode = program.is_used("-e");
  active_mode = program.is_used("-a");
  ser_mode = program.is_used("-s");
  pubsub_mode = program.is_used("-y");
  process_mode = program.is_used("-p");
  chart_mode = program.is_used("-c");

  preset_mode = program.is_used("-x");

  plain_mode = program.is_used("--plain");

  use_chart_dot = program.is_used("--dot");

  max_rows = program.get<int>("--rows");

  max_columns = program.get<int>("--columns");

  chart_width = program.get<int>("--chart_width");

  process_width = program.get<int>("--process_width");

  proto_args = program.get<std::string>("-g");

  auto proto_dir = program.get<std::string>("-d");

  auto fbs_dir = program.get<std::string>("-f");

  if (proto_dir.empty()) {
    proto_dir = vlink::Utils::get_env("VLINK_PROTO_DIR");
  }

  if (fbs_dir.empty()) {
    fbs_dir = vlink::Utils::get_env("VLINK_FBS_DIR");
  }

  if (preset_mode) {
    detail_mode = true;
    observe_all_mode = true;
    process_mode = true;
    chart_mode = true;
  }

  if VUNLIKELY (chart_width < 10 || chart_width > 100) {
    std::cerr << "Invalid [chart_width], range 10 - 100." << std::endl;
    return -1;
  }

  if VUNLIKELY (process_width < 20 || process_width > 100) {
    std::cerr << "Invalid [process_width], range 20 - 100." << std::endl;
    return -1;
  }

#ifdef _WIN32

  if (program.is_used("-d")) {
    try {
      proto_dir = vlink::Helpers::path_to_string(std::filesystem::path(proto_dir));
    } catch (std::filesystem::filesystem_error&) {
    }

    std::replace(proto_dir.begin(), proto_dir.end(), '\\', '/');
  }

  if (program.is_used("-f")) {
    try {
      fbs_dir = vlink::Helpers::path_to_string(std::filesystem::path(fbs_dir));
    } catch (std::filesystem::filesystem_error&) {
    }

    std::replace(fbs_dir.begin(), fbs_dir.end(), '\\', '/');
  }
#endif

  if VUNLIKELY (!detail_mode && observe_all_mode) {
    std::cerr << "Observe all mode[-o] only use for Detail mode[-l]." << std::endl;
    return -1;
  }

  if VUNLIKELY (!detail_mode && active_mode) {
    std::cerr << "Active mode[-a] only use for Detail mode[-l]." << std::endl;
    return -1;
  }

  VLINK_TERM_OUT << "\033[?25l";
  VLINK_TERM_OUT.flush();

  int ret = start_monitor(urls, filter, hostname_filter, proto_dir, fbs_dir);

  VLINK_TERM_OUT << "\033[?25h";
  VLINK_TERM_OUT.flush();

  return ret;
}
