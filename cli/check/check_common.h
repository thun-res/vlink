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

#pragma once

#include <vlink/base/helpers.h>
#include <vlink/base/message_loop.h>
#include <vlink/base/utils.h>
#include <vlink/version.h>
#include <vlink/vlink.h>
#ifdef VLINK_SUPPORT_SHM
#include <vlink/modules/fdbus_conf.h>
#include <vlink/modules/shm_conf.h>
#endif

#include <algorithm>
#include <argparse/argparse.hpp>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#ifndef _WIN32
#include <sys/resource.h>
#include <sys/wait.h>
#endif

#if defined(__linux__) || defined(__ANDROID__)
#include <net/if.h>
#include <sys/ioctl.h>
#include <unistd.h>
#endif

[[maybe_unused]] static constexpr int kTitleWidth = 50;
[[maybe_unused]] static constexpr int kStatusPassPad = 50;
[[maybe_unused]] static constexpr int kStatusWarnPad = 49;
[[maybe_unused]] static constexpr int kStatusFailPad = 50;
[[maybe_unused]] static constexpr int kMulticastDiscovery[] = {239, 255, 0, 100};
[[maybe_unused]] static constexpr int kMulticastDds[] = {239, 255, 0, 1};

[[maybe_unused]] static const char kColorReset[] = "\033[0m";
[[maybe_unused]] static const char kColorPass[] = "\033[32m";
[[maybe_unused]] static const char kColorWarn[] = "\033[33m";
[[maybe_unused]] static const char kColorFail[] = "\033[31m";
[[maybe_unused]] static const char kColorInfo[] = "\033[36m";
[[maybe_unused]] static const char kColorHeader[] = "\033[44;37;1m";

int check_test();

int check_env(bool available_case, const std::string& prefix);

int check_diag(bool all_case, bool show_summary, const std::string& filter);
