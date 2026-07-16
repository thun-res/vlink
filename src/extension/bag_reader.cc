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

#include "./extension/bag_reader.h"

#include <algorithm>
#include <memory>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#include "./base/helpers.h"
#include "./base/logger.h"
#include "./extension/bag_plugin_interface.h"
#include "./extension/vcap_reader.h"
#include "./extension/vdb_reader.h"
#include "./impl/url.h"

namespace vlink {

// UrlMeta
bool BagReader::Info::UrlMeta::operator<(const BagReader::Info::UrlMeta& target) const noexcept {
  int lindex = Url::get_sort_index(url);
  int rindex = Url::get_sort_index(target.url);

  if (lindex < rindex) {
    return true;
  } else if (lindex > rindex) {
    return false;
  } else if (url < target.url) {
    return true;
  } else if (url > target.url) {
    return false;
  }

  return index < target.index;
}

// BagReader::Impl
struct BagReader::Impl final {
  BagReader::OutputCallback output_callback;
  std::shared_ptr<BagPluginInterface> plugin_interface;
  std::unordered_map<std::string, std::string> playback_url_remap;
  std::unordered_set<std::string> excluded_playback_urls;
  std::unordered_map<std::string, std::string> url_to_ser_map;
  std::unordered_map<std::string, SchemaType> url_to_schema_type_map;
  std::atomic_bool playback_url_rules_enabled{false};
  mutable std::shared_mutex playback_state_mtx;
  mutable std::shared_mutex output_callback_mtx;

  bool cursor_opened{false};
  bool cursor_eof{false};
  bool cursor_fail{false};
};

// BagReader
std::shared_ptr<BagReader> BagReader::create(const std::string& path, bool read_only, bool try_to_fix) {
  std::string suffix_check = path;

  std::transform(suffix_check.begin(), suffix_check.end(), suffix_check.begin(),
                 [](unsigned char c) { return std::tolower(c); });

  if (Helpers::has_endwith(suffix_check, ".vdb") || Helpers::has_endwith(suffix_check, ".vdbx")) {
    return std::make_shared<VDBReader>(path, read_only, try_to_fix);
  } else if (Helpers::has_endwith(suffix_check, ".vcap") || Helpers::has_endwith(suffix_check, ".vcapx")) {
    return std::make_shared<VCAPReader>(path, read_only, try_to_fix);
  } else {
    CLOG_E("BagReader: Unknown bag suffix, path=%s", path.c_str());
    return nullptr;
  }
}

BagReader::BagReader(const std::string& path, bool read_only, bool try_to_fix) : impl_(std::make_unique<Impl>()) {
  (void)path;
  (void)read_only;
  (void)try_to_fix;

  Bytes::init_memory_pool();
}

BagReader::~BagReader() {
  std::shared_ptr<BagPluginInterface> plugin_interface;

  {
    std::shared_lock state_lock(impl_->playback_state_mtx);
    plugin_interface = impl_->plugin_interface;
  }

  if (plugin_interface) {
    plugin_interface->register_callback({});
  }
}

void BagReader::detach_plugin() {
  std::shared_ptr<BagPluginInterface> plugin_interface;

  {
    std::unique_lock state_lock(impl_->playback_state_mtx);
    plugin_interface = std::move(impl_->plugin_interface);
  }

  if (plugin_interface) {
    plugin_interface->flush();
    plugin_interface->register_callback({});
  }
}

void BagReader::reset_plugin() {
  std::shared_ptr<BagPluginInterface> plugin_interface;

  {
    std::shared_lock state_lock(impl_->playback_state_mtx);
    plugin_interface = impl_->plugin_interface;
  }

  if (plugin_interface) {
    plugin_interface->reset();
  }
}

void BagReader::flush_plugin() {
  std::shared_ptr<BagPluginInterface> plugin_interface;

  {
    std::shared_lock state_lock(impl_->playback_state_mtx);
    plugin_interface = impl_->plugin_interface;
  }

  if (plugin_interface) {
    plugin_interface->flush();
  }
}

void BagReader::bind_bag_interface(const std::shared_ptr<BagPluginInterface>& bag_interface) {
  std::shared_ptr<BagPluginInterface> old_plugin_interface;

  {
    std::shared_lock state_lock(impl_->playback_state_mtx);
    old_plugin_interface = impl_->plugin_interface;
  }

  if (old_plugin_interface && old_plugin_interface != bag_interface) {
    old_plugin_interface->flush();
    old_plugin_interface->register_callback({});
  }

  if VLIKELY (bag_interface) {
    bag_interface->bind_direction(BagPluginInterface::Direction::kRead);

    bag_interface->register_callback([this](const Frame& frame) {
      std::string output_url;

      if VUNLIKELY (!convert_playback_url(frame.url, output_url)) {
        return;  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
      }

      std::shared_lock callback_lock(impl_->output_callback_mtx);

      if VLIKELY (impl_->output_callback) {
        Frame out;
        out.timestamp = frame.timestamp;
        out.url = std::move(output_url);
        out.ser_type = frame.ser_type;
        out.schema_type = frame.schema_type;
        out.action_type = frame.action_type;
        out.data = Bytes::shallow_copy(frame.data.data(), frame.data.size());

        if (out.ser_type.empty() || out.schema_type == SchemaType::kUnknown) {
          fill_frame_meta(out);
        }

        impl_->output_callback(out);
      }
    });
  }

  std::unique_lock state_lock(impl_->playback_state_mtx);

  impl_->plugin_interface = bag_interface;
  impl_->playback_url_remap.clear();
  impl_->excluded_playback_urls.clear();
  impl_->playback_url_rules_enabled.store(false, std::memory_order_release);
}

void BagReader::clear_bag_interface() { bind_bag_interface(nullptr); }

void BagReader::register_status_callback(StatusCallback&& status_callback) { (void)status_callback; }

void BagReader::register_ready_callback(ReadyCallback&& ready_callback) { (void)ready_callback; }

void BagReader::register_finish_callback(FinishCallback&& finish_callback) { (void)finish_callback; }

void BagReader::register_output_callback(OutputCallback&& output_callback) {
  std::unique_lock lock(impl_->output_callback_mtx);

  impl_->output_callback = std::move(output_callback);
}

bool BagReader::open_cursor(const Config& config) {
  impl_->cursor_opened = false;
  impl_->cursor_eof = false;
  impl_->cursor_fail = false;

  if VUNLIKELY (!do_open_cursor(config)) {
    impl_->cursor_fail = true;
    return false;
  }

  impl_->cursor_opened = true;
  return true;
}

bool BagReader::open_cursor() { return open_cursor(Config{}); }

bool BagReader::read_next(Frame& out) {
  if VUNLIKELY (impl_->cursor_fail) {
    return false;
  }

  if VUNLIKELY (impl_->cursor_eof) {
    return false;
  }

  if (!impl_->cursor_opened && !open_cursor(Config{})) {
    return false;  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
  }

  bool is_error = false;

  if VLIKELY (do_read_next(out, is_error)) {
    return true;
  }

  if (is_error) {
    impl_->cursor_fail = true;  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
  } else {
    impl_->cursor_eof = true;
  }

  return false;
}

BagReader& BagReader::operator>>(Frame& out) {
  read_next(out);

  return *this;
}

bool BagReader::eof() const noexcept { return impl_->cursor_eof; }

bool BagReader::fail() const noexcept { return impl_->cursor_fail; }

BagReader::operator bool() const noexcept { return !impl_->cursor_eof && !impl_->cursor_fail; }

bool BagReader::do_open_cursor(const Config& config) {
  (void)config;

  return false;
}

bool BagReader::do_read_next(Frame& out, bool& is_error) {  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
  (void)out;

  is_error = false;  // LCOV_EXCL_LINE GCOVR_EXCL_LINE

  return false;  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
}

void BagReader::process_output(Frame& frame) {
  std::shared_ptr<BagPluginInterface> plugin_interface;

  {
    std::shared_lock state_lock(impl_->playback_state_mtx);

    plugin_interface = impl_->plugin_interface;

    if VUNLIKELY (plugin_interface && impl_->excluded_playback_urls.count(frame.url) != 0U) {
      return;
    }

    const std::string* meta_url = &frame.url;

    if (plugin_interface) {
      auto remap_iter = impl_->playback_url_remap.find(frame.url);

      if (remap_iter != impl_->playback_url_remap.end()) {
        meta_url = &remap_iter->second;
      }
    }

    if (frame.ser_type.empty()) {
      auto ser_iter = impl_->url_to_ser_map.find(*meta_url);

      if VLIKELY (ser_iter != impl_->url_to_ser_map.end()) {
        frame.ser_type = ser_iter->second;
      }
    }

    if (frame.schema_type == SchemaType::kUnknown) {
      auto schema_iter = impl_->url_to_schema_type_map.find(*meta_url);

      if VLIKELY (schema_iter != impl_->url_to_schema_type_map.end()) {
        frame.schema_type = schema_iter->second;
      }
    }
  }

  if (plugin_interface) {
    plugin_interface->on_read(frame);
  } else {
    std::shared_lock callback_lock(impl_->output_callback_mtx);

    if VLIKELY (impl_->output_callback) {
      impl_->output_callback(frame);
    }
  }
}

void BagReader::fill_frame_meta(Frame& frame) const {
  std::shared_lock state_lock(impl_->playback_state_mtx);

  if (frame.ser_type.empty()) {
    auto iter = impl_->url_to_ser_map.find(frame.url);

    if VLIKELY (iter != impl_->url_to_ser_map.end()) {
      frame.ser_type = iter->second;
    }
  }

  if (frame.schema_type == SchemaType::kUnknown) {
    auto iter = impl_->url_to_schema_type_map.find(frame.url);

    if VLIKELY (iter != impl_->url_to_schema_type_map.end()) {
      frame.schema_type = iter->second;
    }
  }
}

std::unordered_map<std::string, std::string>& BagReader::url_ser_map() { return impl_->url_to_ser_map; }

std::unordered_map<std::string, SchemaType>& BagReader::url_schema_type_map() { return impl_->url_to_schema_type_map; }

std::string BagReader::get_ser_type(const std::string& url) const {
  std::shared_lock state_lock(impl_->playback_state_mtx);

  auto iter = impl_->url_to_ser_map.find(url);

  if VLIKELY (iter != impl_->url_to_ser_map.end()) {
    return iter->second;
  }

  return {};
}

SchemaType BagReader::get_schema_type(const std::string& url) const {
  std::shared_lock state_lock(impl_->playback_state_mtx);

  auto iter = impl_->url_to_schema_type_map.find(url);

  if VLIKELY (iter != impl_->url_to_schema_type_map.end()) {
    return iter->second;
  }

  return SchemaType::kUnknown;
}

void BagReader::process_url_metas(std::vector<Info::UrlMeta>& url_metas) {
  std::shared_ptr<BagPluginInterface> plugin_interface;

  {
    std::shared_lock state_lock(impl_->playback_state_mtx);
    plugin_interface = impl_->plugin_interface;
  }

  std::unordered_map<std::string, std::string> playback_url_remap;
  std::unordered_set<std::string> excluded_playback_urls;

  if (plugin_interface) {
    url_metas.erase(
        std::remove_if(url_metas.begin(), url_metas.end(),
                       [&plugin_interface, &playback_url_remap, &excluded_playback_urls](Info::UrlMeta& meta) {
                         const std::string input_url = meta.url;

                         if VUNLIKELY (!plugin_interface->convert_url_meta(meta.url, meta.ser_type, meta.schema_type)) {
                           excluded_playback_urls.emplace(input_url);
                           return true;
                         }

                         if (meta.url != input_url) {
                           playback_url_remap[input_url] = meta.url;
                         }

                         return false;
                       }),
        url_metas.end());
  }

  {
    std::unique_lock state_lock(impl_->playback_state_mtx);

    if (impl_->plugin_interface == plugin_interface) {
      impl_->playback_url_remap = std::move(playback_url_remap);
      impl_->excluded_playback_urls = std::move(excluded_playback_urls);
      impl_->playback_url_rules_enabled.store(
          !impl_->playback_url_remap.empty() || !impl_->excluded_playback_urls.empty(), std::memory_order_release);
    }
  }
}

bool BagReader::convert_playback_url(const std::string& input_url, std::string& output_url) const {
  std::shared_lock state_lock(impl_->playback_state_mtx);

  if VUNLIKELY (impl_->excluded_playback_urls.count(input_url) != 0U) {
    return false;
  }

  auto iter = impl_->playback_url_remap.find(input_url);

  if (iter != impl_->playback_url_remap.end()) {
    output_url = iter->second;
  } else {
    output_url = input_url;
  }

  return true;
}

bool BagReader::match_playback_url_filter(std::string_view input_url,
                                          const std::unordered_set<std::string>& filter_urls) const {
  if VUNLIKELY (!input_url.data()) {
    return false;
  }

  if (filter_urls.empty() && !has_playback_url_rules()) {
    return true;
  }

  std::string output_url;

  if VUNLIKELY (!convert_playback_url(std::string(input_url), output_url)) {
    return false;
  }

  return filter_urls.empty() || filter_urls.count(output_url) != 0U;
}

bool BagReader::has_playback_url_rules() const noexcept {
  return impl_->playback_url_rules_enabled.load(std::memory_order_acquire);
}

void BagReader::rebuild_url_meta_lookup(const std::vector<Info::UrlMeta>& url_metas) {
  std::unique_lock state_lock(impl_->playback_state_mtx);

  rebuild_url_meta_maps(url_metas, impl_->url_to_ser_map, impl_->url_to_schema_type_map);
}

void BagReader::rebuild_url_meta_maps(const std::vector<Info::UrlMeta>& url_metas,
                                      std::unordered_map<std::string, std::string>& ser_map,
                                      std::unordered_map<std::string, SchemaType>& schema_type_map) {
  ser_map.clear();
  schema_type_map.clear();
  ser_map.reserve(url_metas.size());
  schema_type_map.reserve(url_metas.size());

  std::unordered_set<std::string> ser_conflict_urls;
  std::unordered_set<std::string> schema_conflict_urls;

  ser_conflict_urls.reserve(url_metas.size());
  schema_conflict_urls.reserve(url_metas.size());

  for (const auto& meta : url_metas) {
    auto& merged_ser_type = ser_map[meta.url];
    auto& merged_schema_type = schema_type_map[meta.url];

    if (ser_conflict_urls.count(meta.url) == 0U && !meta.ser_type.empty()) {
      if (merged_ser_type.empty() || merged_ser_type == "Bytes") {
        merged_ser_type = meta.ser_type;
      } else if (meta.ser_type != "Bytes" && meta.ser_type != merged_ser_type) {
        CLOG_E("BagReader: URL remap collision on %s, keeping ser_type unknown. ser [%s] vs [%s].", meta.url.c_str(),
               merged_ser_type.c_str(), meta.ser_type.c_str());
        merged_ser_type.clear();
        ser_conflict_urls.emplace(meta.url);
      }
    }

    if (schema_conflict_urls.count(meta.url) == 0U && meta.schema_type != SchemaType::kUnknown) {
      if (merged_schema_type == SchemaType::kUnknown) {
        merged_schema_type = meta.schema_type;
      } else if (merged_schema_type != meta.schema_type) {
        const auto current_label = SchemaData::convert_type(merged_schema_type);
        const auto new_label = SchemaData::convert_type(meta.schema_type);
        CLOG_E("BagReader: URL remap collision on %s, keeping schema_type unknown. schema [%.*s] vs [%.*s].",
               meta.url.c_str(), static_cast<int>(current_label.size()), current_label.data(),
               static_cast<int>(new_label.size()), new_label.data());
        merged_schema_type = SchemaType::kUnknown;
        schema_conflict_urls.emplace(meta.url);
      }
    }
  }
}

ActionType BagReader::convert_action(std::string_view str) {
  if (str == "C/Req") {
    return ActionType::kClientRequest;
  } else if (str == "C/Resp") {
    return ActionType::kClientResponse;
  } else if (str == "S/Req") {
    return ActionType::kServerRequest;
  } else if (str == "S/Resp") {
    return ActionType::kServerResponse;
  } else if (str == "Pub") {
    return ActionType::kPublish;
  } else if (str == "Sub") {
    return ActionType::kSubscribe;
  } else if (str == "Set") {
    return ActionType::kSet;
  } else if (str == "Get") {
    return ActionType::kGet;
  } else {
    return ActionType::kUnknownAction;
  }
}

}  // namespace vlink
