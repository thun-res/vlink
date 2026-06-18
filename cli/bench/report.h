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

#include <string>

#include "./bench.h"

namespace vlink::bench::report {

struct TerminalOptions final {
  bool interactive{false};
};

struct Summary final {
  size_t sample_count{0};
  size_t case_count{0};
  size_t passing_case_count{0};
  size_t warning_case_count{0};
  size_t failing_case_count{0};
};

Summary summarize(const Bench::Result& result);

bool save_csv(const Bench::Result& result, const std::string& file_path, std::string& error);

bool save_html(const Bench::Result& result, const std::string& file_path, std::string& error);

bool print_terminal(const Bench::Result& result, const TerminalOptions& options, std::string& error);

}  // namespace vlink::bench::report
