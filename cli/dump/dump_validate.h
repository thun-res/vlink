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

#include <argparse/argparse.hpp>
#include <string>

#include "dump_types.h"

namespace vlink::dump {

const char* dump_type_to_string(DumpType type);

bool is_dump_export_type(DumpType type);

bool option_used(const argparse::ArgumentParser& program, std::string_view short_option,
                 std::string_view long_option = {});

bool validate_mode_options(const argparse::ArgumentParser& program, DumpType type, bool has_bag_input,
                           bool url_argument_used, const std::string& target_url);

}  // namespace vlink::dump
