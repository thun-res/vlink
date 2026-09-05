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

[[maybe_unused]] std::atomic_bool has_quit{false};
[[maybe_unused]] std::atomic_bool has_intra_bind{false};
[[maybe_unused]] std::atomic_bool is_paused{false};
[[maybe_unused]] std::atomic_bool is_changed{false};
[[maybe_unused]] std::atomic_bool has_printed{false};
[[maybe_unused]] std::atomic_bool force_update{false};
[[maybe_unused]] std::atomic_bool is_proto_type{false};
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

[[maybe_unused]] bool is_text_ser_type(std::string_view ser_type) {
  std::string lower_ser{ser_type};
  std::transform(lower_ser.begin(), lower_ser.end(), lower_ser.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return lower_ser == "string" || lower_ser == "std::string" || lower_ser == "json" ||
         lower_ser == "application/json" || lower_ser == "text/json" || lower_ser == "text";
}

[[maybe_unused]] bool load_text_for_file(const std::string& filename, std::string& content) {
  std::ifstream input(filename, std::ios::binary);

  if VUNLIKELY (!input.is_open()) {
    return false;
  }

  content.assign(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
  return !input.bad();
}

[[maybe_unused]] std::filesystem::path utf8_to_path(const std::string& utf8) noexcept {
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

[[maybe_unused]] std::string get_home_config_path(const std::string& filename) {
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

[[maybe_unused]] std::string read_home_config(const std::string& filename) {
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

[[maybe_unused]] bool write_home_config(const std::string& filename, const std::string& value) {
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

[[maybe_unused]] bool format_json_text(const std::string& content, std::string& out) {
  auto json = nlohmann::ordered_json::parse(content, nullptr, false);

  if (json.is_discarded()) {
    return false;
  }

  out = json.dump(2);

  return true;
}
