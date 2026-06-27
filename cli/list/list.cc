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

#include <vlink/base/utils.h>
#include <vlink/extension/discovery_viewer.h>
#include <vlink/version.h>
#include <vlink/vlink.h>

#include <argparse/argparse.hpp>
//
#include <algorithm>
#include <iostream>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

[[maybe_unused]] static std::atomic_bool has_quit{false};

[[maybe_unused]] static constexpr int kCollectInterval{1000};

std::shared_ptr<vlink::DiscoveryViewer> discovery_viewer;

int start_viewer(bool native_mode, bool check_process_count) {
  try {
    vlink::DiscoveryViewer::FilterType filter_type = vlink::DiscoveryViewer::kFilterNone;

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

  auto quit_function = [](int) {
    if VUNLIKELY (has_quit) {
      return;
    }

    has_quit = true;

    discovery_viewer->quit(true);
  };

  vlink::Utils::register_terminate_signal(quit_function);

  if (!check_process_count) {
    std::cout << "Information Collecting, Please Wait...";
    std::cout.flush();
  }

  discovery_viewer->wait_for_quit(kCollectInterval);

  if (!check_process_count) {
    std::cout << "\033[2K\r";
    std::cout.flush();
  }

  return 0;
}

int stop_viewer() {
  if VUNLIKELY (!discovery_viewer) {
    return -1;
  }

  discovery_viewer->quit(true);
  discovery_viewer->wait_for_quit();

  has_quit = true;

  discovery_viewer.reset();

  return 0;
}

int list_process(const std::string& name, uint32_t pid, bool check_process_count) {
  if VUNLIKELY (!discovery_viewer || has_quit) {
    if (check_process_count) {
      return 0;
    }

    return -1;
  }

  if VUNLIKELY (discovery_viewer->is_ready_to_quit()) {
    return 0;
  }

  struct ProcessExtOption {
    bool has_server{false};
    bool has_client{false};
    bool has_publisher{false};
    bool has_subscriber{false};
    bool has_setter{false};
    bool has_getter{false};
  };

  std::set<std::string> process_set;

  std::map<std::tuple<std::string, std::string, std::string, uint32_t>,
           std::vector<std::tuple<uint32_t, std::string, std::string>>>
      process_list_map;

  std::map<std::tuple<std::string, std::string, std::string, uint32_t>, ProcessExtOption> process_option_map;

  auto emplace_function = [&process_list_map, &process_option_map](const auto& info, const auto& process) {
    auto p = std::make_tuple(process.host, process.ip, process.name, process.pid);

    auto& msg_list = process_list_map[p];
    auto& option = process_option_map[p];

    msg_list.emplace_back(process.type, info.url, info.ser_type);

    if (!option.has_server) {
      option.has_server = process.type & vlink::kServer;
    }

    if (!option.has_client) {
      option.has_client = process.type & vlink::kClient;
    }

    if (!option.has_publisher) {
      option.has_publisher = process.type & vlink::kPublisher;
    }

    if (!option.has_subscriber) {
      option.has_subscriber = process.type & vlink::kSubscriber;
    }

    if (!option.has_setter) {
      option.has_setter = process.type & vlink::kSetter;
    }

    if (!option.has_getter) {
      option.has_getter = process.type & vlink::kGetter;
    }
  };

  size_t max_url_size = 0;

  for (const auto& info : discovery_viewer->get_info_list()) {
    max_url_size = std::max(max_url_size, info.url.size());

    for (const auto& process : info.process_list) {
      if (check_process_count) {
        process_set.emplace(process.name);
      } else {
        if (name.empty() && pid == 0) {
          emplace_function(info, process);
        } else if (pid != 0) {
          if (process.pid == pid) {
            emplace_function(info, process);
          }
        } else if (!name.empty()) {
          if (process.name == name) {
            emplace_function(info, process);
          }
        }
      }
    }
  }

  if (check_process_count) {
    return static_cast<int>(process_set.size());
  }

  for (const auto& [process_info, msg_list] : process_list_map) {
    const auto& [host_name, ip, process_name, process_pid] = process_info;

    const auto& option = process_option_map[process_info];

    std::cout << process_name;
    std::cout << " (";
    std::cout << "pid: ";
    std::cout << process_pid;
    std::cout << ", ";
    std::cout << "host: ";
    std::cout << host_name;
    std::cout << ", ";
    std::cout << "ip: ";
    std::cout << ip;
    std::cout << ")";
    std::cout << std::endl;

#ifdef __APPLE__
    std::cout.flush();
    std::this_thread::sleep_for(std::chrono::microseconds(1000));
#endif

    if (option.has_server) {
      std::cout << std::string(2, ' ');
      std::cout << "Server:";
      std::cout << std::endl;
      for (const auto& [type, url, ser] : msg_list) {
        if (type & vlink::kServer) {
          std::cout << std::string(4, ' ');
          std::cout << url;
          std::cout << std::string(max_url_size - url.size() + 4, ' ');
          std::cout << ser;
          std::cout << std::endl;

#ifdef __APPLE__
          std::cout.flush();
          std::this_thread::sleep_for(std::chrono::microseconds(1000));
#endif
        }
      }
    }

    if (option.has_client) {
      std::cout << std::string(2, ' ');
      std::cout << "Client:";
      std::cout << std::endl;
      for (const auto& [type, url, ser] : msg_list) {
        if (type & vlink::kClient) {
          std::cout << std::string(4, ' ');
          std::cout << url;
          std::cout << std::string(max_url_size - url.size() + 4, ' ');
          std::cout << ser;
          std::cout << std::endl;

#ifdef __APPLE__
          std::cout.flush();
          std::this_thread::sleep_for(std::chrono::microseconds(1000));
#endif
        }
      }
    }

    if (option.has_publisher) {
      std::cout << std::string(2, ' ');
      std::cout << "Publisher:";
      std::cout << std::endl;
      for (const auto& [type, url, ser] : msg_list) {
        if (type & vlink::kPublisher) {
          std::cout << std::string(4, ' ');
          std::cout << url;
          std::cout << std::string(max_url_size - url.size() + 4, ' ');
          std::cout << ser;
          std::cout << std::endl;

#ifdef __APPLE__
          std::cout.flush();
          std::this_thread::sleep_for(std::chrono::microseconds(1000));
#endif
        }
      }
    }

    if (option.has_subscriber) {
      std::cout << std::string(2, ' ');
      std::cout << "Subscriber:";
      std::cout << std::endl;
      for (const auto& [type, url, ser] : msg_list) {
        if (type & vlink::kSubscriber) {
          std::cout << std::string(4, ' ');
          std::cout << url;
          std::cout << std::string(max_url_size - url.size() + 4, ' ');
          std::cout << ser;
          std::cout << std::endl;

#ifdef __APPLE__
          std::cout.flush();
          std::this_thread::sleep_for(std::chrono::microseconds(1000));
#endif
        }
      }
    }

    if (option.has_setter) {
      std::cout << std::string(2, ' ');
      std::cout << "Setter:";
      std::cout << std::endl;
      for (const auto& [type, url, ser] : msg_list) {
        if (type & vlink::kSetter) {
          std::cout << std::string(4, ' ');
          std::cout << url;
          std::cout << std::string(max_url_size - url.size() + 4, ' ');
          std::cout << ser;
          std::cout << std::endl;

#ifdef __APPLE__
          std::cout.flush();
          std::this_thread::sleep_for(std::chrono::microseconds(1000));
#endif
        }
      }
    }

    if (option.has_getter) {
      std::cout << std::string(2, ' ');
      std::cout << "Getter:";
      std::cout << std::endl;
      for (const auto& [type, url, ser] : msg_list) {
        if (type & vlink::kGetter) {
          std::cout << std::string(4, ' ');
          std::cout << url;
          std::cout << std::string(max_url_size - url.size() + 4, ' ');
          std::cout << ser;
          std::cout << std::endl;

#ifdef __APPLE__
          std::cout.flush();
          std::this_thread::sleep_for(std::chrono::microseconds(1000));
#endif
        }
      }
    }

    std::cout << std::endl;
  }

  return 0;
}

int main(int argc, char* argv[]) {
  std::ios::sync_with_stdio(false);
  vlink::Utils::set_console_utf8_output();

  // init
  vlink::Logger::set_console_level(vlink::Logger::kOff);
  vlink::Logger::set_file_level(vlink::Logger::kOff);
  vlink::Logger::init("vlink-list");

  // env
  vlink::Utils::unset_env("VLINK_BAG_PATH");
  // vlink::Utils::set_env("VLINK_DISCOVER_DISABLE", "1");

  // arg parser
  argparse::ArgumentParser program("vlink-list", VLINK_VERSION, argparse::default_arguments::all);

  program.add_description("Note: You may need to add multicast/broadcast [" +
                          vlink::DiscoveryViewer::get_listen_address() + "]");

  program.add_argument("-n", "--native").help("Native mode").default_value(false).implicit_value(true);
  program.add_argument("-m", "--name").help("Process name").default_value(std::string());
  program.add_argument("-p", "--pid").help("Process id").scan<'u', uint32_t>().default_value(static_cast<uint32_t>(0U));
  program.add_argument("-c", "--check_process_count")
      .help("Get process count (By return value, no terminal output)")
      .default_value(false)
      .implicit_value(true);

  program.add_epilog("Example:\n  vlink-list");

  try {
    program.parse_args(argc, argv);
  } catch (const std::exception& e) {
    std::cerr << e.what() << std::endl;
    std::cerr << program << std::endl;
    return 1;
  }

  auto native_mode = program.is_used("-n");
  auto name = program.get<std::string>("-m");
  auto pid = program.get<uint32_t>("-p");
  auto check_process_count = program.is_used("-c");

  int ret = start_viewer(native_mode, check_process_count);

  if VUNLIKELY (ret != 0) {
    return ret;
  }

  ret = list_process(name, pid, check_process_count);

  stop_viewer();

  return ret;
}
