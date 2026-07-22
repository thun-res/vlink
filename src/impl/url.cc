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

#include "./impl/url.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <map>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>

#include "./base/helpers.h"
#include "./base/plugin.h"
#include "./base/utils.h"
#include "./extension/url_remap.h"
#include "./impl/conf_plugin_interface.h"
#include "./impl/url_parser.h"

#define VLINK_URL_USE_REMAP 1
#define VLINK_URL_USE_PLUGIN 1

namespace vlink {

[[maybe_unused]] static constexpr uint16_t kMaxUrlLength = 120U;

[[maybe_unused]] inline static bool equals_ignore_case(std::string_view left, std::string_view right) noexcept {
  return left.size() == right.size() &&
         std::equal(left.begin(), left.end(), right.begin(), [](unsigned char left_char, unsigned char right_char) {
           return std::tolower(left_char) == std::tolower(right_char);
         });
}

[[maybe_unused]] inline static TransportType get_transport_for_str(const std::string& str) noexcept {
  if (str == "intra") {
    return TransportType::kIntra;
  }

#if !defined(__ANDROID__)

  if (str == "shm") {
    return TransportType::kShm;
  }

  if (str == "shm2") {
    return TransportType::kShm2;
  }
#endif

  if (str == "zenoh") {
    return TransportType::kZenoh;
  }

  if (str == "dds" || str == "ddsf") {
    return TransportType::kDds;
  }

  if (str == "ddsc") {
    return TransportType::kDdsc;
  }

  if (str == "ddsr") {
    return TransportType::kDdsr;
  }

  if (str == "someip") {
    return TransportType::kSomeip;
  }

  if (str == "mqtt") {
    return TransportType::kMqtt;
  }

  if (str == "fdbus") {
    return TransportType::kFdbus;
  }

  return TransportType::kUnknown;
}

[[maybe_unused]] inline static TransportType get_dds_transport_for_str(const std::string& str) noexcept {
  if (str == "dds" || str == "ddsf") {
    return TransportType::kDds;
  }

  if (str == "ddsc") {
    return TransportType::kDdsc;
  }

  if (str == "ddsr") {
    return TransportType::kDdsr;
  }

  return TransportType::kUnknown;
}

[[maybe_unused]] inline static bool is_dds_type(const TransportType& transport) noexcept {
  return transport == TransportType::kDds || transport == TransportType::kDdsc || transport == TransportType::kDdsr;
}

[[maybe_unused]] inline static const char* get_module_for_transport(TransportType type) noexcept {
  switch (type) {
    case TransportType::kIntra:
      return "intra";

#if !defined(__ANDROID__)

    case TransportType::kShm:
      return "shm";

    case TransportType::kShm2:
      return "shm2";
#endif

    case TransportType::kZenoh:
      return "zenoh";

    case TransportType::kDds:
      return "dds";

    case TransportType::kDdsc:
      return "ddsc";

    case TransportType::kDdsr:
      return "ddsr";

    case TransportType::kSomeip:
      return "someip";

    case TransportType::kMqtt:
      return "mqtt";

    case TransportType::kFdbus:
      return "fdbus";

    default:
      return nullptr;  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
  }
}

[[maybe_unused]] inline static bool is_intra_url(std::string_view url) noexcept {
  return Helpers::has_startwith(url, "intra://");
}

[[maybe_unused]] inline static bool is_shm_url(std::string_view url) noexcept {
  return Helpers::has_startwith(url, "shm://") || Helpers::has_startwith(url, "shm2://");
}

[[maybe_unused]] inline static int get_sort_index_for_url(std::string_view url) {
  if (url.empty()) {
    return -1;
  }

  if (Helpers::has_startwith(url, "intra://")) {
    return static_cast<int>(TransportType::kIntra);
  }

#if !defined(__ANDROID__)

  if (Helpers::has_startwith(url, "shm://")) {
    return static_cast<int>(TransportType::kShm);
  }

  if (Helpers::has_startwith(url, "shm2://")) {
    return static_cast<int>(TransportType::kShm2);
  }
#endif

  if (Helpers::has_startwith(url, "zenoh://")) {
    return static_cast<int>(TransportType::kZenoh);
  }

  if (Helpers::has_startwith(url, "dds://") || Helpers::has_startwith(url, "ddsf://")) {
    return static_cast<int>(TransportType::kDds);
  }

  if (Helpers::has_startwith(url, "ddsc://")) {
    return static_cast<int>(TransportType::kDdsc);
  }

  if (Helpers::has_startwith(url, "ddsr://")) {
    return static_cast<int>(TransportType::kDdsr);
  }

  if (Helpers::has_startwith(url, "someip://")) {
    return static_cast<int>(TransportType::kSomeip);
  }

  if (Helpers::has_startwith(url, "mqtt://")) {
    return static_cast<int>(TransportType::kMqtt);
  }

  if (Helpers::has_startwith(url, "fdbus://")) {
    return static_cast<int>(TransportType::kFdbus);
  }

  return 0;
}

// GlobalModulesManager
class GlobalModulesManager final {
 public:
  static GlobalModulesManager& get() {
    static GlobalModulesManager manager;
    return manager;
  }

  std::shared_ptr<ConfPluginInterface> get_or_load_interface(TransportType type) {
    {
      std::shared_lock lock(mtx_);

      auto iter = ptr_map_.find(type);

      if VLIKELY (iter != ptr_map_.end()) {
        return iter->second;
      }
    }

    if VLIKELY (!plugin_autoload_enabled_) {
      return nullptr;
    }

    const char* module_name = get_module_for_transport(type);

    if VUNLIKELY (module_name == nullptr) {
      return nullptr;  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
    }

    std::lock_guard lock(mtx_);

    auto iter = ptr_map_.find(type);

    if VUNLIKELY (iter != ptr_map_.end()) {
      return iter->second;  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
    }

    const std::string lib_name = std::string("vlink-") + module_name;
    auto ptr = plugin_.load<ConfPluginInterface>(lib_name, 1, 0);

    if VUNLIKELY (!ptr) {
      return nullptr;
    }

    const auto plugin_type = ptr->get_transport_type();

    // LCOV_EXCL_START GCOVR_EXCL_START
    if VUNLIKELY (plugin_type != type) {
      VLOG_E("Plugin transport mismatch, libname: ", lib_name, ", expected: ", static_cast<int>(type),
             ", actual: ", static_cast<int>(plugin_type), ".");
      (void)plugin_.unload<ConfPluginInterface>(lib_name);
      return nullptr;
    }
    // LCOV_EXCL_STOP GCOVR_EXCL_STOP

    return ptr_map_.emplace(type, std::move(ptr)).first->second;
  }

  void init(uint16_t transport_enable_flags) {
    (void)transport_enable_flags;

    if VLIKELY (url_plugins_env_.empty() || plugin_autoload_enabled_ || equals_ignore_case(url_plugins_env_, "none")) {
      return;
    }

    auto plugins_list = Helpers::split_any(url_plugins_env_);

    std::lock_guard lock(mtx_);

    // plugin_.set_log_level(Logger::kWarn);

    for (auto libname : plugins_list) {
      Helpers::replace_string(libname, "vlink-", "");

      const auto expected_type = get_transport_for_str(libname);

      if VUNLIKELY (expected_type == TransportType::kUnknown) {
        VLOG_E("Unsupported plugin module, libname: ", libname, ".");
        continue;
      }

      const std::string plugin_lib_name = "vlink-" + libname;
      auto ptr = plugin_.load<ConfPluginInterface>(plugin_lib_name, 1, 0);

      if VLIKELY (ptr) {
        const auto plugin_type = ptr->get_transport_type();

        // LCOV_EXCL_START GCOVR_EXCL_START
        if VUNLIKELY (plugin_type != expected_type) {
          VLOG_E("Plugin transport mismatch, libname: ", plugin_lib_name,
                 ", expected: ", static_cast<int>(expected_type), ", actual: ", static_cast<int>(plugin_type), ".");
          (void)plugin_.unload<ConfPluginInterface>(plugin_lib_name);
          continue;
        }
        // LCOV_EXCL_STOP GCOVR_EXCL_STOP

        ptr_map_.emplace(expected_type, std::move(ptr));
      }
    }
  }

 private:
  GlobalModulesManager()
      : url_plugins_env_(Utils::get_env("VLINK_URL_PLUGINS")),
        plugin_autoload_enabled_(equals_ignore_case(url_plugins_env_, "auto")) {}

  ~GlobalModulesManager() {
    std::lock_guard lock(mtx_);
    ptr_map_.clear();
    plugin_.clear();
  }

  const std::string url_plugins_env_;
  const bool plugin_autoload_enabled_{false};
  Plugin plugin_;
  std::unordered_map<TransportType, std::shared_ptr<ConfPluginInterface>> ptr_map_;
  mutable std::shared_mutex mtx_;

  VLINK_DISALLOW_COPY_AND_ASSIGN(GlobalModulesManager)
};

// GlobalUrlRemap
class GlobalUrlRemap final : public UrlRemap {
 public:
  static GlobalUrlRemap& get() {
    static GlobalUrlRemap remap;
    return remap;
  }

  std::string convert_thread_safe(const std::string& url) noexcept {
    std::lock_guard lock(mtx_);
    return convert(url);
  }

 private:
  GlobalUrlRemap() {
    const auto& url_remap_env = Utils::get_env("VLINK_URL_REMAP", "");

    if (url_remap_env.empty()) {
      return;
    }

    set_enable_log(true);

    load(url_remap_env);
  }

  ~GlobalUrlRemap() = default;

  std::mutex mtx_;

  VLINK_DISALLOW_COPY_AND_ASSIGN(GlobalUrlRemap)
};

// Protocol
Protocol::Protocol(const std::string& address) {
  if VUNLIKELY (address.length() > kMaxUrlLength) {
    CLOG_F("The URL length exceeds the character limit (Target length: %zu, Max length: %u).", address.length(),
           kMaxUrlLength);
    return;
  }

#if VLINK_URL_USE_REMAP
  std::string real_address = GlobalUrlRemap::get().convert_thread_safe(address);

  UrlParser parser(real_address);
  str = std::move(real_address);
#else
  UrlParser parser(address);
  str = address;
#endif

  transport = get_transport_for_str(parser.get_transport());

  if (is_dds_type(transport)) {
    const auto dds_bind_transport_str = Utils::get_env("VLINK_DDS_BIND", "");

    if VUNLIKELY (!dds_bind_transport_str.empty()) {
      TransportType dds_bind_transport = get_dds_transport_for_str(dds_bind_transport_str);

      if VLIKELY (dds_bind_transport != TransportType::kUnknown) {
        static std::atomic_bool dds_bind_print{false};

        if (!dds_bind_print.exchange(true, std::memory_order_relaxed)) {
          CLOG_I("Bind [dds] to [%s].", dds_bind_transport_str.c_str());
        }

        transport = dds_bind_transport;
      }
    }
  } else if (transport == TransportType::kIntra) {
    const auto intra_bind_transport_str = Utils::get_env("VLINK_INTRA_BIND", "");

    if (!intra_bind_transport_str.empty()) {
      TransportType intra_bind_transport = get_transport_for_str(intra_bind_transport_str);

      if VLIKELY (intra_bind_transport != TransportType::kIntra && intra_bind_transport != TransportType::kUnknown) {
        static std::atomic_bool intra_bind_print{false};

        if (!intra_bind_print.exchange(true, std::memory_order_relaxed)) {
          CLOG_I("Bind [intra] to [%s].", intra_bind_transport_str.c_str());
        }

        transport = intra_bind_transport;
      }
    }
  }

  host = std::move(const_cast<std::string&>(parser.get_host()));
  path = std::move(const_cast<std::string&>(parser.get_path()));
  dictionary = std::move(const_cast<std::map<std::string, std::string>&>(parser.get_query_dictionary()));
  fragment = std::move(const_cast<std::string&>(parser.get_fragment()));
}

// Url
void Url::init_plugins(uint16_t transport_enable_flags) {
#if VLINK_URL_USE_PLUGIN
  static auto& manager_instance = GlobalModulesManager::get();

  static std::once_flag flag;

  std::call_once(flag, [transport_enable_flags]() { manager_instance.init(transport_enable_flags); });
#else
  (void)transport_enable_flags;
#endif
}

std::unique_ptr<Conf> Url::load_for_plugin(TransportType type) {
#if VLINK_URL_USE_PLUGIN
  if VUNLIKELY (type == TransportType::kUnknown) {
    return nullptr;
  }

  auto ptr = GlobalModulesManager::get().get_or_load_interface(type);

  if VUNLIKELY (!ptr) {
    return nullptr;
  }

  return ptr->create();  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
#else
  (void)type;

  return nullptr;
#endif
}

int Url::get_sort_index(std::string_view url) { return get_sort_index_for_url(url); }

bool Url::is_local_type(std::string_view url) { return is_intra_url(url) || is_shm_url(url); }

bool Url::is_intra_type(std::string_view url) { return is_intra_url(url); }

bool Url::is_shm_type(std::string_view url) { return is_shm_url(url); }

std::ostream& operator<<(std::ostream& ostream, const Url& conf) noexcept {
  ostream << "Url:"
          << "[type]" << +conf.get_impl_type() << "[str]" << conf.protocol_.str;

  return ostream;
}

}  // namespace vlink
