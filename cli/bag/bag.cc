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
#include <vlink/extension/bag_writer.h>
#include <vlink/extension/discovery_viewer.h>
#include <vlink/version.h>
#include <vlink/vlink.h>

#include <algorithm>
#include <argparse/argparse.hpp>
#include <cctype>
#include <cmath>
#include <exception>
#include <filesystem>
#include <iostream>
#include <limits>
#include <sstream>
#include <string>
#include <vector>

#include "./bag_commands.h"
#include "./bag_common.h"

[[maybe_unused]] static double convert_time_to_seconds(const std::string& time_str) {
  int64_t hours = 0;
  int64_t minutes = 0;
  int64_t seconds = 0;
  int64_t milliseconds = 0;

  char delimiter1 = {0};
  char delimiter2 = {0};
  char delimiter3 = {0};

  thread_local std::stringstream ss;
  ss.clear();
  ss.str(time_str);

  if (ss >> hours >> delimiter1 >> minutes >> delimiter2 >> seconds) {
    if (delimiter1 != ':' || delimiter2 != ':') {
      return -1;
    }

    if (ss >> delimiter3 >> milliseconds) {
      if (delimiter3 != ':') {
        return -1;
      }
    } else {
      milliseconds = 0;
    }

    if (hours < 0 || minutes < 0 || minutes >= 60 || seconds < 0 || seconds >= 60 || milliseconds < 0 ||
        milliseconds >= 1000) {
      return -1;
    }

    return (hours * 3600.0) + (minutes * 60) + seconds + (milliseconds / 1000.0);
  }

  return -1;
}

// NOLINTNEXTLINE(google-readability-function-size)
int main(int argc, char* argv[]) {
  vlink::Utils::set_console_utf8_output();

  // init
  // vlink::Logger::set_console_level(vlink::Logger::kOff);
  // vlink::Logger::set_file_level(vlink::Logger::kOff);
  vlink::Logger::init("vlink-bag");

  // env
  vlink::Utils::unset_env("VLINK_BAG_PATH");
  // vlink::Utils::set_env("VLINK_DISCOVER_DISABLE", "1");

  // intra_bind
  std::string intra_bind = vlink::Utils::get_env("VLINK_INTRA_BIND");

  if (!intra_bind.empty()) {
    has_intra_bind = true;
  }

  // arg parser
  argparse::ArgumentParser program("vlink-bag", VLINK_VERSION, argparse::default_arguments::all);

  program.add_description("Note: You may need to add multicast/broadcast [" +
                          vlink::DiscoveryViewer::get_listen_address() + "]");

  // info command
  argparse::ArgumentParser info_command("info", VLINK_VERSION, argparse::default_arguments::help);
  info_command.add_argument("path").help("Database path").required();
  info_command.add_argument("-l", "--detail").help("Detail mode").default_value(false).implicit_value(true);

  info_command.add_description("Print infomation");

  std::string info_example_str = "Example:\n  vlink-bag info /tmp/bag.vdb";
  info_example_str += "\n  ";
  info_example_str += "vlink-bag info /tmp/bag.vdbx";
  info_example_str += "\n  ";
  info_example_str += "vlink-bag info /tmp/bag.vcap";
  info_example_str += "\n  ";
  info_example_str += "vlink-bag info /tmp/bag.vcapx";
  info_command.add_epilog(info_example_str);

  // record command
  argparse::ArgumentParser record_command("record", VLINK_VERSION, argparse::default_arguments::help);
  record_command.add_argument("path").help("Database path").required();
  record_command.add_argument("-u", "--urls")
      .help("Bind urls, empty is all")
      .default_value(std::vector<std::string>())
      .nargs(argparse::nargs_pattern::any);
  record_command.add_argument("-t", "--tag").help("Set tag name").default_value(std::string());
  record_command.add_argument("-i", "--filter")
      .help("URL keyword filter, comma-separated or quoted space-separated")
      .default_value(std::string());
  record_command.add_argument("-k", "--black").help("Blacklist mode").default_value(false).implicit_value(true);
  record_command.add_argument("-n", "--native").help("Native mode").default_value(false).implicit_value(true);
  record_command.add_argument("-d", "--duration")
      .help("Duration(s), duration <= 0 means invalid")
      .scan<'g', double>()
      // NOLINTNEXTLINE(readability-redundant-casting)
      .default_value(static_cast<double>(0));
  record_command.add_argument("-w", "--wait")
      .help("Max wait for quit time(s)")
      .scan<'g', double>()
      // NOLINTNEXTLINE(readability-redundant-casting)
      .default_value(static_cast<double>(30));
  record_command.add_argument("-p", "--compress").help("Compress data").default_value(false).implicit_value(true);
  record_command.add_argument("-f", "--force").help("Overwriting").default_value(false).implicit_value(true);
  record_command.add_argument("-q", "--quiet").help("Quiet mode").default_value(false).implicit_value(true);
  record_command.add_argument("-l", "--detail").help("Detail mode").default_value(false).implicit_value(true);
  record_command.add_argument("-o", "--split_name_by_time")
      .help("Split name by time")
      .default_value(false)
      .implicit_value(true);
  record_command.add_argument("-z", "--split_by_size")
      .help("Split size(GB)")
      .scan<'g', double>()
      // NOLINTNEXTLINE(readability-redundant-casting)
      .default_value(static_cast<double>(vlink::BagWriter::Config().split_by_size / 1024.0 / 1024.0 / 1024.0));
  record_command.add_argument("-y", "--split_by_time")
      .help("Split time(s)")
      .scan<'g', double>()
      // NOLINTNEXTLINE(readability-redundant-casting)
      .default_value(static_cast<double>(vlink::BagWriter::Config().split_by_time));
  record_command.add_argument("--max_split_count")
      .help("Max retained split file count (0 means unlimited)")
      .scan<'d', int64_t>()
      .default_value(vlink::BagWriter::Config().max_split_count);
  record_command.add_argument("-g", "--deft")
      .help("No collect serialization infomation")
      .default_value(false)
      .implicit_value(true);
  record_command.add_argument("-x", "--max_packet_size")
      .help("Max packet size(MB)")
      .scan<'g', double>()
      // NOLINTNEXTLINE(readability-redundant-casting)
      .default_value(static_cast<double>(4.0));
  record_command.add_argument("-j", "--wal_mode").help("Enable wal mode").default_value(false).implicit_value(true);
  record_command.add_argument("-c", "--cache_size")
      .help("Cache size(MB)")
      .scan<'g', double>()
      // NOLINTNEXTLINE(readability-redundant-casting)
      .default_value(static_cast<double>(vlink::BagWriter::Config().cache_size / 1024.0 / 1024.0));
  record_command.add_argument("-s", "--sync_mode")
      .help("Synchronous write mode")
      .default_value(false)
      .implicit_value(true);
  record_command.add_argument("--max_task_depth")
      .help("Max pending tasks in the queue")
      .scan<'d', int64_t>()
      .default_value(vlink::BagWriter::Config().max_task_depth);
  record_command.add_argument("--max_memory_size")
      .help("Max memory size in the queue(GB)")
      .scan<'g', double>()
      // NOLINTNEXTLINE(readability-redundant-casting)
      .default_value(static_cast<double>(vlink::BagWriter::Config().max_memory_size / 1024.0 / 1024.0 / 1024.0));
  record_command.add_argument("--max_row_count")
      .help("Max row count")
      .scan<'d', int64_t>()
      .default_value(vlink::BagWriter::Config().max_row_count);
  record_command.add_argument("--max_bytes_size")
      .help("Max bytes size(GB)")
      .scan<'g', double>()
      // NOLINTNEXTLINE(readability-redundant-casting)
      .default_value(static_cast<double>(vlink::BagWriter::Config().max_bytes_size / 1024.0 / 1024.0 / 1024.0));
  record_command.add_argument("--enable_limit").help("Enable limit").default_value(false).implicit_value(true);
  record_command.add_argument("--compress_level")
      .help("Compress level (range: 1 ~ 5, 0 means default)")
      .scan<'d', int>()
      .default_value(static_cast<int>(vlink::BagWriter::Config().compress_level));
  record_command.add_argument("--ignore_compress")
      .help("Ignore compress urls")
      .default_value(std::vector<std::string>())
      .nargs(argparse::nargs_pattern::any);

  record_command.add_argument("--plugin").help("Plugin name").default_value(std::string());

  record_command.add_description("Record data");

  std::string record_example_str = "Example:\n  vlink-bag record /tmp/bag.vdb";
  record_example_str += "\n  ";
  record_example_str += "vlink-bag record /tmp/bag.vdbx";
  record_example_str += "\n  ";
  record_example_str += "vlink-bag record /tmp/bag.vcap";
  record_example_str += "\n  ";
  record_example_str += "vlink-bag record /tmp/bag.vcapx";
  record_command.add_epilog(record_example_str);

  // play command
  argparse::ArgumentParser play_command("play", VLINK_VERSION, argparse::default_arguments::help);
  play_command.add_argument("path").help("Database path").required();
  play_command.add_argument("-u", "--urls")
      .help("Bind urls, empty is all")
      .default_value(std::vector<std::string>())
      .nargs(argparse::nargs_pattern::any);
  play_command.add_argument("-i", "--filter")
      .help("URL keyword filter, comma-separated or quoted space-separated")
      .default_value(std::string());
  play_command.add_argument("-k", "--black").help("Blacklist mode").default_value(false).implicit_value(true);
  play_command.add_argument("-n", "--native").help("Native mode").default_value(false).implicit_value(true);
  play_command.add_argument("-s", "--actions")
      .help(
          "1: C/Req, 2: C/Resp, "
          "3: S/Req, 4: S/Resp, "
          "5: Pub, 6: Sub, "
          "7: Set, 8: Get")
      .scan<'d', int>()
      .default_value(std::vector<int>{6})
      .nargs(argparse::nargs_pattern::any);
  play_command.add_argument("-b", "--begin_time")
      .help("Begin time(s)")
      .scan<'g', double>()
      // NOLINTNEXTLINE(readability-redundant-casting)
      .default_value(static_cast<double>(0));
  play_command.add_argument("-e", "--end_time")
      .help("End time(s)")
      .scan<'g', double>()
      // NOLINTNEXTLINE(readability-redundant-casting)
      .default_value(static_cast<double>(0));
  play_command.add_argument("-t", "--times")
      .help("Play times, times <= 0 means infinite")
      .scan<'d', int>()
      // NOLINTNEXTLINE(readability-redundant-casting)
      .default_value(static_cast<int>(1));
  play_command.add_argument("-r", "--rate")
      .help("Play rate[0.01 ~ 100]")
      .scan<'g', double>()
      // NOLINTNEXTLINE(readability-redundant-casting)
      .default_value(static_cast<double>(1.0));
  play_command.add_argument("-q", "--quiet").help("Quiet mode").default_value(false).implicit_value(true);
  play_command.add_argument("-l", "--detail").help("Detail mode").default_value(false).implicit_value(true);
  play_command.add_argument("-m", "--skip_blank").help("Skip black").default_value(false).implicit_value(true);
  play_command.add_argument("-j", "--auto_pause").help("Auto pause").default_value(false).implicit_value(true);

  play_command.add_argument("--local_time").help("Show local time").default_value(false).implicit_value(true);
  play_command.add_argument("--utc_time").help("Show utc time").default_value(false).implicit_value(true);
  play_command.add_argument("--rel_begin_time")
      .help("Relative Begin time(format: '00:00:00' or 00:00:00:000)")
      .default_value(std::string());
  play_command.add_argument("--rel_end_time")
      .help("Relative End time(format: '00:00:00' or 00:00:00:000)")
      .default_value(std::string());
  play_command.add_argument("--local_begin_time")
      .help("Local Begin time(format: '00:00:00' or 00:00:00:000)")
      .default_value(std::string());
  play_command.add_argument("--local_end_time")
      .help("Local End time(format: '00:00:00' or 00:00:00:000)")
      .default_value(std::string());
  play_command.add_argument("--utc_begin_time")
      .help("UTC Begin time(format: '00:00:00' or 00:00:00:000)")
      .default_value(std::string());
  play_command.add_argument("--utc_end_time")
      .help("UTC End time(format: '00:00:00' or 00:00:00:000)")
      .default_value(std::string());

  play_command.add_argument("--plugin").help("Plugin name").default_value(std::string());

  play_command.add_description("Play data");

  std::string play_example_str = "Example:\n  vlink-bag play /tmp/bag.vdb";
  play_example_str += "\n  ";
  play_example_str += "vlink-bag play /tmp/bag.vdbx";
  play_example_str += "\n  ";
  play_example_str += "vlink-bag play /tmp/bag.vcap";
  play_example_str += "\n  ";
  play_example_str += "vlink-bag play /tmp/bag.vcapx";
  play_command.add_epilog(play_example_str);

  // clone command
  argparse::ArgumentParser clone_command("clone", VLINK_VERSION, argparse::default_arguments::help);
  clone_command.add_argument("source_path").help("Source database path").required();
  clone_command.add_argument("target_path").help("Target database path").required();
  clone_command.add_argument("-u", "--urls")
      .help("Bind urls, empty is all")
      .default_value(std::vector<std::string>())
      .nargs(argparse::nargs_pattern::any);
  clone_command.add_argument("-t", "--tag").help("Set tag name").default_value(std::string());
  clone_command.add_argument("-i", "--filter")
      .help("URL keyword filter, comma-separated or quoted space-separated")
      .default_value(std::string());
  clone_command.add_argument("-k", "--black").help("Blacklist mode").default_value(false).implicit_value(true);
  clone_command.add_argument("-s", "--actions")
      .help(
          "1: C/Req, 2: C/Resp, "
          "3: S/Req, 4: S/Resp, "
          "5: Pub, 6: Sub, "
          "7: Set, 8: Get")
      .scan<'d', int>()
      .default_value(std::vector<int>{6})
      .nargs(argparse::nargs_pattern::any);
  clone_command.add_argument("-b", "--begin_time")
      .help("Begin time(s)")
      .scan<'g', double>()
      // NOLINTNEXTLINE(readability-redundant-casting)
      .default_value(static_cast<double>(0));
  clone_command.add_argument("-e", "--end_time")
      .help("End time(s)")
      .scan<'g', double>()
      // NOLINTNEXTLINE(readability-redundant-casting)
      .default_value(static_cast<double>(0));
  clone_command.add_argument("-q", "--quiet").help("Quiet mode").default_value(false).implicit_value(true);
  clone_command.add_argument("-l", "--detail").help("Detail mode").default_value(false).implicit_value(true);
  clone_command.add_argument("-p", "--compress").help("Compress data").default_value(false).implicit_value(true);
  clone_command.add_argument("-o", "--split_name_by_time")
      .help("Split name by time")
      .default_value(false)
      .implicit_value(true);
  clone_command.add_argument("-z", "--split_by_size")
      .help("Split size(GB)")
      .scan<'g', double>()
      // NOLINTNEXTLINE(readability-redundant-casting)
      .default_value(static_cast<double>(vlink::BagWriter::Config().split_by_size / 1024.0 / 1024.0 / 1024.0));
  clone_command.add_argument("-y", "--split_by_time")
      .help("Split time(s)")
      .scan<'g', double>()
      // NOLINTNEXTLINE(readability-redundant-casting)
      .default_value(static_cast<double>(vlink::BagWriter::Config().split_by_time));
  clone_command.add_argument("-f", "--force").help("Overwriting").default_value(false).implicit_value(true);
  clone_command.add_argument("-j", "--wal_mode").help("Enable wal mode").default_value(false).implicit_value(true);
  clone_command.add_argument("-c", "--cache_size")
      .help("Cache size(MB)")
      .scan<'g', double>()
      // NOLINTNEXTLINE(readability-redundant-casting)
      .default_value(static_cast<double>(vlink::BagWriter::Config().cache_size / 1024.0 / 1024.0));

  clone_command.add_argument("--rel_begin_time")
      .help("Relative Begin time(format: '00:00:00' or 00:00:00:000)")
      .default_value(std::string());
  clone_command.add_argument("--rel_end_time")
      .help("Relative End time(format: '00:00:00' or 00:00:00:000)")
      .default_value(std::string());
  clone_command.add_argument("--local_begin_time")
      .help("Local Begin time(format: '00:00:00' or 00:00:00:000)")
      .default_value(std::string());
  clone_command.add_argument("--local_end_time")
      .help("Local End time(format: '00:00:00' or 00:00:00:000)")
      .default_value(std::string());
  clone_command.add_argument("--utc_begin_time")
      .help("UTC Begin time(format: '00:00:00' or 00:00:00:000)")
      .default_value(std::string());
  clone_command.add_argument("--utc_end_time")
      .help("UTC End time(format: '00:00:00' or 00:00:00:000)")
      .default_value(std::string());
  clone_command.add_argument("--compress_level")
      .help("Compress level (range: 1 ~ 5, 0 means default)")
      .scan<'d', int>()
      .default_value(static_cast<int>(vlink::BagWriter::Config().compress_level));
  clone_command.add_argument("--ignore_compress")
      .help("Ignore compress urls")
      .default_value(std::vector<std::string>())
      .nargs(argparse::nargs_pattern::any);
  clone_command.add_argument("--import_schema")
      .help("Try import embedded schema data")
      .default_value(false)
      .implicit_value(true);

  clone_command.add_argument("--plugin").help("Plugin name").default_value(std::string());

  clone_command.add_description("Clone data");

  std::string clone_example_str = "Example:\n  vlink-bag clone /tmp/old_bag.vdb /tmp/new_bag.vdb";
  clone_example_str += "\n  ";
  clone_example_str += "vlink-bag clone /tmp/old_bag.vdbx /tmp/new_bag.vdbx";
  clone_example_str += "\n  ";
  clone_example_str += "vlink-bag clone /tmp/old_bag.vcap /tmp/new_bag.vcap";
  clone_example_str += "\n  ";
  clone_example_str += "vlink-bag clone /tmp/old_bag.vcapx /tmp/new_bag.vdbx";
  clone_example_str += "\n  ";
  clone_example_str += "vlink-bag clone /tmp/old_bag.vdb /tmp/new_bag.vcap";
  clone_example_str += "\n  ";
  clone_example_str += "vlink-bag clone /tmp/old_bag.vcap /tmp/new_bag.vdb";
  clone_command.add_epilog(clone_example_str);

  // merge command
  argparse::ArgumentParser merge_command("merge", VLINK_VERSION, argparse::default_arguments::help);
  merge_command.add_argument("source_paths")
      .help("Input bags (frames within each bag must have nondecreasing timestamps)")
      .nargs(2, std::numeric_limits<size_t>::max());
  merge_command.add_argument("-o", "--output").help("Output bag path").required();
  merge_command.add_argument("-t", "--tag").help("Tag name").default_value(std::string());
  merge_command.add_argument("-p", "--compress").help("Compress data").default_value(false).implicit_value(true);
  merge_command.add_argument("-f", "--force").help("Overwriting").default_value(false).implicit_value(true);
  merge_command.add_argument("-q", "--quiet").help("Quiet mode").default_value(false).implicit_value(true);
  merge_command.add_description("Merge bags by original absolute timestamps; equal timestamps keep input order");
  merge_command.add_epilog("Example:\n  vlink-bag merge /tmp/a.vdb /tmp/b.vcap -o /tmp/merged.vdb");

  // check command
  argparse::ArgumentParser check_command("check", VLINK_VERSION, argparse::default_arguments::help);
  check_command.add_argument("path").help("Database path").required();
  check_command.add_description("Check data");

  std::string check_example_str = "Example:\n  vlink-bag check /tmp/bag.vdb";
  check_example_str += "\n  ";
  check_example_str += "vlink-bag check /tmp/bag.vdbx";
  check_example_str += "\n  ";
  check_example_str += "vlink-bag check /tmp/bag.vcap";
  check_example_str += "\n  ";
  check_example_str += "vlink-bag check /tmp/bag.vcapx";
  check_command.add_epilog(check_example_str);

  // reindex command
  argparse::ArgumentParser reindex_command("reindex", VLINK_VERSION, argparse::default_arguments::help);
  reindex_command.add_argument("path").help("Database path").required();
  reindex_command.add_description("Rebuild index");

  std::string reindex_example_str = "Example:\n  vlink-bag reindex /tmp/bag.vdb";
  reindex_example_str += "\n  ";
  reindex_example_str += "vlink-bag reindex /tmp/bag.vdbx";
  reindex_example_str += "\n  ";
  reindex_example_str += "vlink-bag reindex /tmp/bag.vcap";
  reindex_example_str += "\n  ";
  reindex_example_str += "vlink-bag reindex /tmp/bag.vcapx";
  reindex_command.add_epilog(reindex_example_str);

  // fix command
  argparse::ArgumentParser fix_command("fix", VLINK_VERSION, argparse::default_arguments::help);
  fix_command.add_argument("path").help("Database path").required();
  fix_command.add_argument("-y", "--rebuild").help("Rebuild mode").default_value(false).implicit_value(true);
  fix_command.add_description("Fix data");

  std::string fix_example_str = "Example:\n  vlink-bag fix /tmp/bag.vdb";
  fix_example_str += "\n  ";
  fix_example_str += "vlink-bag fix /tmp/bag.vdbx";
  fix_example_str += "\n  ";
  fix_example_str += "vlink-bag fix /tmp/bag.vcap";
  fix_example_str += "\n  ";
  fix_example_str += "vlink-bag fix /tmp/bag.vcapx";
  fix_command.add_epilog(fix_example_str);

  // tag command
  argparse::ArgumentParser tag_command("tag", VLINK_VERSION, argparse::default_arguments::help);
  tag_command.add_argument("path").help("Database path").required();
  tag_command.add_argument("tag").help("Tag name").required();
  tag_command.add_description("Set tag");

  std::string tag_example_str = "Example:\n  vlink-bag tag /tmp/bag.vdb 'tag_name'";
  tag_example_str += "\n  ";
  tag_example_str += "vlink-bag tag /tmp/bag.vdbx 'tag_name'";
  tag_example_str += "\n  ";
  tag_example_str += "vlink-bag tag /tmp/bag.vcapx 'tag_name'";
  tag_command.add_epilog(tag_example_str);

  program.add_subparser(info_command);
  program.add_subparser(record_command);
  program.add_subparser(play_command);
  program.add_subparser(clone_command);
  program.add_subparser(merge_command);
  program.add_subparser(check_command);
  program.add_subparser(reindex_command);
  program.add_subparser(fix_command);
  program.add_subparser(tag_command);

  try {
    program.parse_args(argc, argv);
  } catch (const std::exception& e) {
    std::cerr << e.what() << std::endl;

    if (program.is_subcommand_used("info")) {
      std::cerr << info_command << std::endl;
    } else if (program.is_subcommand_used("record")) {
      std::cerr << record_command << std::endl;
    } else if (program.is_subcommand_used("play")) {
      std::cerr << play_command << std::endl;
    } else if (program.is_subcommand_used("clone")) {
      std::cerr << clone_command << std::endl;
    } else if (program.is_subcommand_used("merge")) {
      std::cerr << merge_command << std::endl;
    } else if (program.is_subcommand_used("check")) {
      std::cerr << check_command << std::endl;
    } else if (program.is_subcommand_used("reindex")) {
      std::cerr << reindex_command << std::endl;
    } else if (program.is_subcommand_used("fix")) {
      std::cerr << fix_command << std::endl;
    } else if (program.is_subcommand_used("tag")) {
      std::cerr << tag_command << std::endl;
    }

    return 1;
  }

  auto check_bag_path = [](const std::string& path, const char* label) {
    std::string suffix_check = path;

    std::transform(suffix_check.begin(), suffix_check.end(), suffix_check.begin(),
                   [](unsigned char c) { return std::tolower(c); });

    if (vlink::Helpers::has_endwith(suffix_check, ".vdb") || vlink::Helpers::has_endwith(suffix_check, ".vdbx") ||
        vlink::Helpers::has_endwith(suffix_check, ".vcap") || vlink::Helpers::has_endwith(suffix_check, ".vcapx")) {
      return true;
    }

    std::cerr << "Warning: Invalid " << label << " suffix: " << path << ". Expected .vdb, .vdbx, .vcap or .vcapx."
              << std::endl;
    return false;
  };

  if (program.is_subcommand_used("info")) {
    auto path = info_command.get<std::string>("path");

#ifdef _WIN32
    try {
      path = vlink::Helpers::path_to_string(std::filesystem::path(path));
    } catch (std::filesystem::filesystem_error&) {
    }
#endif

    detail_flag = info_command.is_used("-l");

    if VUNLIKELY (!check_bag_path(path, "input path")) {
      return -1;
    }

    return bag_info(path);
  } else if (program.is_subcommand_used("record")) {
    auto path = record_command.get<std::string>("path");

#ifdef _WIN32
    try {
      path = vlink::Helpers::path_to_string(std::filesystem::path(path));
    } catch (std::filesystem::filesystem_error&) {
    }
#endif

    const auto& urls = record_command.get<std::vector<std::string>>("-u");
    auto tag_name = record_command.get<std::string>("-t");
    const auto& filter = record_command.get<std::string>("-i");

#ifdef _WIN32
    tag_name = vlink::Helpers::string_local_to_utf8(tag_name);
#endif

    auto black_mode = record_command.is_used("-k");
    auto native_mode = record_command.is_used("-n");
    auto duration = record_command.get<double>("-d");
    auto wait_time = record_command.get<double>("-w");
    auto compress = record_command.is_used("-p");
    auto force = record_command.is_used("-f");

    auto max_row_count = record_command.get<int64_t>("--max_row_count");
    auto max_bytes_size = record_command.get<double>("--max_bytes_size");
    auto enable_limit = record_command.is_used("--enable_limit");

    auto split_name_by_time = record_command.is_used("-o");
    auto split_by_size = record_command.get<double>("-z");
    auto split_by_time = record_command.get<double>("-y");
    auto max_split_count = record_command.get<int64_t>("--max_split_count");

    auto deft = record_command.is_used("-g");
    auto max_packet_size = record_command.get<double>("-x");
    auto wal_mode = record_command.is_used("-j");
    auto cache_size = record_command.get<double>("-c");
    auto sync_mode = record_command.is_used("-s");

    max_task_depth = record_command.get<int64_t>("--max_task_depth");
    const auto max_memory_size_gb = record_command.get<double>("--max_memory_size");
    max_memory_size = max_memory_size_gb;

    quiet_flag = record_command.is_used("-q");
    detail_flag = record_command.is_used("-l");

    if VUNLIKELY (!check_bag_path(path, "output path")) {
      return -1;
    }

    if VUNLIKELY (sync_mode && record_command.is_used("--max_task_depth")) {
      std::cerr << "Sync mode and task depth cannot be set at the same time" << std::endl;
      return -1;
    }

    if VUNLIKELY (sync_mode && record_command.is_used("--max_memory_size")) {
      std::cerr << "Sync mode and memory size cannot be set at the same time" << std::endl;
      return -1;
    }

    if VUNLIKELY (urls.empty() && deft) {
      std::cerr << "The deft must be turned off in the bind all urls mode" << std::endl;
      return -1;
    }

    static constexpr auto kMaxDurationSeconds = std::numeric_limits<uint32_t>::max() / 1000ULL;
    static constexpr auto kMaxWaitSeconds = std::numeric_limits<int>::max() / 1000ULL;

    if VUNLIKELY (!std::isfinite(duration) || duration > kMaxDurationSeconds) {
      std::cerr << "Invalid duration [-d]" << std::endl;
      return -1;
    }

    if VUNLIKELY (!std::isfinite(wait_time) || wait_time < 0 || wait_time > kMaxWaitSeconds) {
      std::cerr << "Invalid wait_time [-w]" << std::endl;
      return -1;
    }

    static constexpr auto kMaxPacketSizeMb = std::numeric_limits<size_t>::max() / (1024ULL * 1024ULL);

    static constexpr uint64_t kMaxMemoryBytes = static_cast<uint64_t>(std::numeric_limits<size_t>::max()) <
                                                        static_cast<uint64_t>(std::numeric_limits<int64_t>::max())
                                                    ? static_cast<uint64_t>(std::numeric_limits<size_t>::max())
                                                    : static_cast<uint64_t>(std::numeric_limits<int64_t>::max());

    static constexpr auto kMaxMemorySizeGb = kMaxMemoryBytes / (1024ULL * 1024ULL * 1024ULL);

    static constexpr auto kMaxCacheSizeMb = std::numeric_limits<int64_t>::max() / (1024LL * 1024LL);

    static constexpr auto kMaxSizeGb = std::numeric_limits<int64_t>::max() / (1024ULL * 1024ULL * 1024ULL);

    static constexpr auto kMaxTimeSeconds = std::numeric_limits<int64_t>::max() / 1000000ULL;

    if VUNLIKELY (!std::isfinite(max_packet_size) || max_packet_size <= 0 || max_packet_size > kMaxPacketSizeMb) {
      std::cerr << "Invalid max_packet_size [-x]" << std::endl;
      return -1;
    }

    if VUNLIKELY (!std::isfinite(max_memory_size_gb) || max_memory_size_gb <= 0 ||
                  max_memory_size_gb > kMaxMemorySizeGb) {
      std::cerr << "Invalid max_memory_size [--max_memory_size]" << std::endl;
      return -1;
    }

    if VUNLIKELY (!std::isfinite(cache_size) || cache_size < 0 || cache_size > kMaxCacheSizeMb) {
      std::cerr << "Invalid cache_size [-c]" << std::endl;
      return -1;
    }

    if VUNLIKELY (!std::isfinite(max_bytes_size) || max_bytes_size <= 0 || max_bytes_size > kMaxSizeGb) {
      std::cerr << "Invalid max_bytes_size [--max_bytes_size]" << std::endl;
      return -1;
    }

    if VUNLIKELY (max_row_count <= 0) {
      std::cerr << "Invalid max_row_count [--max_row_count]" << std::endl;
      return -1;
    }

    if VUNLIKELY (!std::isfinite(split_by_size) || split_by_size < 0 || split_by_size > kMaxSizeGb) {
      std::cerr << "Invalid split_by_size [-z]" << std::endl;
      return -1;
    }

    if VUNLIKELY (!std::isfinite(split_by_time) || split_by_time < 0 || split_by_time >= kMaxTimeSeconds) {
      std::cerr << "Invalid split_by_time [-y]" << std::endl;
      return -1;
    }

    if VUNLIKELY (max_split_count < 0) {
      std::cerr << "Invalid max_split_count [--max_split_count]" << std::endl;
      return -1;
    }

    compress_level = record_command.get<int>("--compress_level");

    if VUNLIKELY (compress_level < 0 || compress_level > 5) {
      std::cerr << "Invalid compress_level [--compress_level]" << std::endl;
      return -1;
    }

    auto ignore_compress = record_command.get<std::vector<std::string>>("--ignore_compress");

    if VUNLIKELY (!compress &&
                  (record_command.is_used("--compress_level") || record_command.is_used("--ignore_compress"))) {
      std::cerr << "Must set compress [-p]" << std::endl;
      return -1;
    }

    auto record_plugin_name = record_command.get<std::string>("--plugin");

    return bag_record(path, urls, tag_name, filter, black_mode, native_mode, duration, wait_time, compress, force,
                      max_row_count, max_bytes_size, enable_limit, split_name_by_time, split_by_size,
                      split_by_time * 1000, max_split_count, deft, max_packet_size, wal_mode, cache_size, sync_mode,
                      ignore_compress, record_plugin_name);
  } else if (program.is_subcommand_used("play")) {
    auto path = play_command.get<std::string>("path");

#ifdef _WIN32
    try {
      path = vlink::Helpers::path_to_string(std::filesystem::path(path));
    } catch (std::filesystem::filesystem_error&) {
    }
#endif

    const auto& urls = play_command.get<std::vector<std::string>>("-u");
    const auto& filter = play_command.get<std::string>("-i");

    if VUNLIKELY (!check_bag_path(path, "input path")) {
      return -1;
    }

    auto black_mode = play_command.is_used("-k");
    auto native_mode = play_command.is_used("-n");

    quiet_flag = play_command.is_used("-q");
    detail_flag = play_command.is_used("-l");

    skip_blank = play_command.is_used("-m");

    auto auto_pause = play_command.is_used("-j");

    auto actions = play_command.get<std::vector<int>>("-s");
    auto begin_time = play_command.get<double>("-b");
    auto end_time = play_command.get<double>("-e");
    auto times = play_command.get<int>("-t");
    auto rate = play_command.get<double>("-r");

    auto show_local_time = play_command.is_used("--local_time");
    auto show_utc_time = play_command.is_used("--utc_time");

    auto rel_begin_time = play_command.get<std::string>("--rel_begin_time");
    auto rel_end_time = play_command.get<std::string>("--rel_end_time");

    auto local_begin_time = play_command.get<std::string>("--local_begin_time");
    auto local_end_time = play_command.get<std::string>("--local_end_time");

    auto utc_begin_time = play_command.get<std::string>("--utc_begin_time");
    auto utc_end_time = play_command.get<std::string>("--utc_end_time");

    auto plugin_name = play_command.get<std::string>("--plugin");

    time_method = kUseUnknown;

    if (play_command.is_used("--rel_begin_time") || play_command.is_used("--rel_end_time")) {
      time_method |= kUseRelTime;
    }

    if (play_command.is_used("--local_begin_time") || play_command.is_used("--local_end_time")) {
      time_method |= kUseLocalTime;
    }

    if (play_command.is_used("--utc_begin_time") || play_command.is_used("--utc_end_time")) {
      time_method |= kUseUtcTime;
    }

    if VUNLIKELY (time_method != kUseUnknown && time_method != kUseRelTime && time_method != kUseLocalTime &&
                  time_method != kUseUtcTime) {
      std::cerr << "You cannot use diff time formats at the same time" << std::endl;
      return -1;
    }

    switch (time_method) {
      case kUseUnknown:
        break;
      case kUseRelTime:
        if (!rel_begin_time.empty()) {
          begin_time = convert_time_to_seconds(rel_begin_time);
        }

        if (!rel_end_time.empty()) {
          end_time = convert_time_to_seconds(rel_end_time);
        }

        if VUNLIKELY (std::abs(begin_time) > 0.001 && std::abs(end_time) > 0.001 && begin_time >= end_time) {
          std::cerr << "Invalid begin_time and end_time [-b] [-e]" << std::endl;
          return -1;
        }

        break;
      case kUseLocalTime:
        if (!local_begin_time.empty()) {
          begin_time = convert_time_to_seconds(local_begin_time);
        }

        if (!local_end_time.empty()) {
          end_time = convert_time_to_seconds(local_end_time);
        }

        break;
      case kUseUtcTime:
        if (!utc_begin_time.empty()) {
          begin_time = convert_time_to_seconds(utc_begin_time);
        }

        if (!utc_end_time.empty()) {
          end_time = convert_time_to_seconds(utc_end_time);
        }

        break;
      default:
        break;
    }

    const auto input_time_method = time_method.load();

    if VUNLIKELY (show_local_time && show_utc_time) {
      std::cerr << "You cannot use diff time formats at the same time" << std::endl;
      return -1;
    }

    if (show_local_time) {
      time_method = kUseLocalTime;
    } else if (show_utc_time) {
      time_method = kUseUtcTime;
    }

    for (auto a : actions) {
      if VUNLIKELY (a < 1 || a > 8) {
        std::cerr << "Invalid actions [-s]" << std::endl;
        return -1;
      }
    }

    static constexpr auto kMaxTimeSeconds = static_cast<double>(std::numeric_limits<int64_t>::max()) / 1000.0;

    if VUNLIKELY (!std::isfinite(begin_time) || !std::isfinite(end_time) || begin_time < 0 || end_time < 0 ||
                  begin_time >= kMaxTimeSeconds || end_time >= kMaxTimeSeconds) {
      std::cerr << "Invalid begin_time or end_time [-b] [-e]" << std::endl;
      return -1;
    }

    if VUNLIKELY (!std::isfinite(rate) || rate < 0.009999 || rate > 100.000001) {
      std::cerr << "Invalid rate [-r]" << std::endl;
      return -1;
    }

    return bag_play(path, urls, filter, black_mode, native_mode, auto_pause, actions, begin_time * 1000,
                    end_time * 1000, input_time_method, !local_begin_time.empty() || !utc_begin_time.empty(),
                    !local_end_time.empty() || !utc_end_time.empty(), times, rate, plugin_name);
  } else if (program.is_subcommand_used("merge")) {
    auto source_paths = merge_command.get<std::vector<std::string>>("source_paths");
    auto target_path = merge_command.get<std::string>("-o");
    auto tag_name = merge_command.get<std::string>("-t");

#ifdef _WIN32
    try {
      for (auto& path : source_paths) {
        path = vlink::Helpers::path_to_string(std::filesystem::path(path));
      }
      target_path = vlink::Helpers::path_to_string(std::filesystem::path(target_path));
    } catch (std::filesystem::filesystem_error& e) {
      std::cerr << e.what() << std::endl;
      return -1;
    }
    tag_name = vlink::Helpers::string_local_to_utf8(tag_name);
#endif

    for (const auto& path : source_paths) {
      if VUNLIKELY (!check_bag_path(path, "input path")) {
        return -1;
      }
    }
    if VUNLIKELY (!check_bag_path(target_path, "output path")) {
      return -1;
    }

    quiet_flag = merge_command.is_used("-q");
    return bag_merge(source_paths, target_path, tag_name, merge_command.is_used("-p"), merge_command.is_used("-f"));
  } else if (program.is_subcommand_used("clone")) {
    auto source_path = clone_command.get<std::string>("source_path");

    auto target_path = clone_command.get<std::string>("target_path");

#ifdef _WIN32
    try {
      source_path = vlink::Helpers::path_to_string(std::filesystem::path(source_path));
      target_path = vlink::Helpers::path_to_string(std::filesystem::path(target_path));
    } catch (std::filesystem::filesystem_error&) {
    }
#endif

    const auto& urls = clone_command.get<std::vector<std::string>>("-u");
    auto tag_name = clone_command.get<std::string>("-t");
    const auto& filter = clone_command.get<std::string>("-i");

    if VUNLIKELY (!check_bag_path(source_path, "input path")) {
      return -1;
    }

    if VUNLIKELY (!check_bag_path(target_path, "output path")) {
      return -1;
    }

#ifdef _WIN32
    tag_name = vlink::Helpers::string_local_to_utf8(tag_name);
#endif

    auto black_mode = clone_command.is_used("-k");
    auto actions = clone_command.get<std::vector<int>>("-s");
    auto begin_time = clone_command.get<double>("-b");
    auto end_time = clone_command.get<double>("-e");

    quiet_flag = clone_command.is_used("-q");
    detail_flag = clone_command.is_used("-l");

    auto compress = clone_command.is_used("-p");
    auto split_name_by_time = clone_command.is_used("-o");
    auto split_by_size = clone_command.get<double>("-z");
    auto split_by_time = clone_command.get<double>("-y");

    auto force = clone_command.is_used("-f");
    auto wal_mode = clone_command.is_used("-j");
    auto cache_size = clone_command.get<double>("-c");

    auto rel_begin_time = clone_command.get<std::string>("--rel_begin_time");
    auto rel_end_time = clone_command.get<std::string>("--rel_end_time");

    auto local_begin_time = clone_command.get<std::string>("--local_begin_time");
    auto local_end_time = clone_command.get<std::string>("--local_end_time");

    auto utc_begin_time = clone_command.get<std::string>("--utc_begin_time");
    auto utc_end_time = clone_command.get<std::string>("--utc_end_time");

    time_method = kUseUnknown;

    if (clone_command.is_used("--rel_begin_time") || clone_command.is_used("--rel_end_time")) {
      time_method |= kUseRelTime;
    }

    if (clone_command.is_used("--local_begin_time") || clone_command.is_used("--local_end_time")) {
      time_method |= kUseLocalTime;
    }

    if (clone_command.is_used("--utc_begin_time") || clone_command.is_used("--utc_end_time")) {
      time_method |= kUseUtcTime;
    }

    if VUNLIKELY (time_method != kUseUnknown && time_method != kUseRelTime && time_method != kUseLocalTime &&
                  time_method != kUseUtcTime) {
      std::cerr << "You cannot use diff time formats at the same time" << std::endl;
      return -1;
    }

    switch (time_method) {
      case kUseUnknown:
        break;
      case kUseRelTime:
        if (!rel_begin_time.empty()) {
          begin_time = convert_time_to_seconds(rel_begin_time);
        }

        if (!rel_end_time.empty()) {
          end_time = convert_time_to_seconds(rel_end_time);
        }

        if VUNLIKELY (std::abs(begin_time) > 0.001 && std::abs(end_time) > 0.001 && begin_time >= end_time) {
          std::cerr << "Invalid begin_time and end_time [-b] [-e]" << std::endl;
          return -1;
        }

        break;
      case kUseLocalTime:
        if (!local_begin_time.empty()) {
          begin_time = convert_time_to_seconds(local_begin_time);
        }

        if (!local_end_time.empty()) {
          end_time = convert_time_to_seconds(local_end_time);
        }

        break;
      case kUseUtcTime:
        if (!utc_begin_time.empty()) {
          begin_time = convert_time_to_seconds(utc_begin_time);
        }

        if (!utc_end_time.empty()) {
          end_time = convert_time_to_seconds(utc_end_time);
        }

        break;
      default:
        break;
    }

    for (auto a : actions) {
      if VUNLIKELY (a < 1 || a > 8) {
        std::cerr << "Invalid actions [-s]" << std::endl;
        return -1;
      }
    }

    static constexpr auto kMaxCacheSizeMb = std::numeric_limits<int64_t>::max() / (1024LL * 1024LL);

    static constexpr auto kMaxSizeGb = std::numeric_limits<int64_t>::max() / (1024ULL * 1024ULL * 1024ULL);

    static constexpr auto kMaxTimeSeconds = static_cast<double>(std::numeric_limits<int64_t>::max()) / 1000.0;

    static constexpr auto kMaxSplitTimeSeconds = std::numeric_limits<int64_t>::max() / 1000000ULL;

    if VUNLIKELY (!std::isfinite(cache_size) || cache_size < 0 || cache_size > kMaxCacheSizeMb) {
      std::cerr << "Invalid cache_size [-c]" << std::endl;
      return -1;
    }

    if VUNLIKELY (!std::isfinite(begin_time) || !std::isfinite(end_time) || begin_time < 0 || end_time < 0 ||
                  begin_time >= kMaxTimeSeconds || end_time >= kMaxTimeSeconds) {
      std::cerr << "Invalid begin_time or end_time [-b] [-e]" << std::endl;
      return -1;
    }

    if VUNLIKELY (!std::isfinite(split_by_size) || split_by_size < 0 || split_by_size > kMaxSizeGb) {
      std::cerr << "Invalid split_by_size [-z]" << std::endl;
      return -1;
    }

    if VUNLIKELY (!std::isfinite(split_by_time) || split_by_time < 0 || split_by_time >= kMaxSplitTimeSeconds) {
      std::cerr << "Invalid split_by_time [-y]" << std::endl;
      return -1;
    }

    compress_level = clone_command.get<int>("--compress_level");

    if VUNLIKELY (compress_level < 0 || compress_level > 5) {
      std::cerr << "Invalid compress_level [--compress_level]" << std::endl;
      return -1;
    }

    auto ignore_compress = clone_command.get<std::vector<std::string>>("--ignore_compress");

    if VUNLIKELY (!compress &&
                  (clone_command.is_used("--compress_level") || clone_command.is_used("--ignore_compress"))) {
      std::cerr << "Must set compress [-p]" << std::endl;
      return -1;
    }

    if (!clone_command.is_used("--import_schema")) {
      vlink::Utils::unset_env("VLINK_SCHEMA_PLUGIN");
    }

    auto clone_plugin_name = clone_command.get<std::string>("--plugin");

    return bag_clone(source_path, target_path, urls, tag_name, filter, black_mode, actions, begin_time * 1000,
                     end_time * 1000, !local_begin_time.empty() || !utc_begin_time.empty(),
                     !local_end_time.empty() || !utc_end_time.empty(), compress, split_name_by_time, split_by_size,
                     split_by_time * 1000, force, wal_mode, cache_size, ignore_compress, clone_plugin_name);
  } else if (program.is_subcommand_used("check")) {
    auto path = check_command.get<std::string>("path");

#ifdef _WIN32
    try {
      path = vlink::Helpers::path_to_string(std::filesystem::path(path));
    } catch (std::filesystem::filesystem_error&) {
    }
#endif

    if VUNLIKELY (!check_bag_path(path, "input path")) {
      return -1;
    }

    return bag_check(path);
  } else if (program.is_subcommand_used("reindex")) {
    auto path = reindex_command.get<std::string>("path");

#ifdef _WIN32
    try {
      path = vlink::Helpers::path_to_string(std::filesystem::path(path));
    } catch (std::filesystem::filesystem_error&) {
    }
#endif

    if VUNLIKELY (!check_bag_path(path, "input path")) {
      return -1;
    }

    return bag_reindex(path);
  } else if (program.is_subcommand_used("fix")) {
    auto path = fix_command.get<std::string>("path");

#ifdef _WIN32
    try {
      path = vlink::Helpers::path_to_string(std::filesystem::path(path));
    } catch (std::filesystem::filesystem_error&) {
    }
#endif

    auto rebuild_mode = fix_command.is_used("-y");

    if VUNLIKELY (!check_bag_path(path, "input path")) {
      return -1;
    }

    return bag_fix(path, rebuild_mode);
  } else if (program.is_subcommand_used("tag")) {
    auto path = tag_command.get<std::string>("path");

#ifdef _WIN32
    try {
      path = vlink::Helpers::path_to_string(std::filesystem::path(path));
    } catch (std::filesystem::filesystem_error&) {
    }
#endif

    auto tag_name = tag_command.get<std::string>("tag");

    if VUNLIKELY (!check_bag_path(path, "input path")) {
      return -1;
    }

#ifdef _WIN32
    tag_name = vlink::Helpers::string_local_to_utf8(tag_name);
#endif

    return bag_tag(path, tag_name);
  }

  std::cerr << program << std::endl;

  return 1;
}
