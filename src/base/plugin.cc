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

#include "./base/plugin.h"

#include <deque>
#include <filesystem>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>

#if defined(_WIN32) || defined(_WIN64)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#define VLINK_PLUGIN_UNDEFINE_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#define VLINK_PLUGIN_UNDEFINE_NOMINMAX
#endif
#include <windows.h>
#ifdef VLINK_PLUGIN_UNDEFINE_LEAN_AND_MEAN
#undef WIN32_LEAN_AND_MEAN
#undef VLINK_PLUGIN_UNDEFINE_LEAN_AND_MEAN
#endif
#ifdef VLINK_PLUGIN_UNDEFINE_NOMINMAX
#undef NOMINMAX
#undef VLINK_PLUGIN_UNDEFINE_NOMINMAX
#endif
#else
#include <dlfcn.h>
#endif

#include "./base/logger.h"
#include "./base/utils.h"

namespace vlink {

[[maybe_unused]] static std::string get_current_dir() {
  try {
    return std::filesystem::current_path().string();
  } catch (std::filesystem::filesystem_error&) {
    return ".";
  }
}

[[maybe_unused]] static bool check_exists(const std::string& path) {
  try {
    return std::filesystem::exists(path);
  } catch (std::filesystem::filesystem_error&) {
    return false;
  }
}

// DynamicLibrary
class DynamicLibrary final {
 public:
#if defined(_WIN32) || defined(_WIN64)
  static constexpr const char* kFilenamePrefix = "";
  static constexpr const char* kFilenameSuffix = ".dll";
  using NativeHandle = HINSTANCE;
  using NativeSymbol = FARPROC;
#elif defined(__APPLE__)
  static constexpr const char* kFilenamePrefix = "lib";
  static constexpr const char* kFilenameSuffix = ".dylib";
  using NativeHandle = void*;
  using NativeSymbol = void*;
#else
  static constexpr const char* kFilenamePrefix = "lib";
  static constexpr const char* kFilenameSuffix = ".so";
  using NativeHandle = void*;
  using NativeSymbol = void*;
#endif

  static_assert(std::is_pointer_v<NativeHandle>, "Expecting native handle to be a pointer.");
  static_assert(std::is_pointer_v<NativeSymbol>, "Expecting native symbol to be a pointer.");

  class Exception : public std::runtime_error {
    using std::runtime_error::runtime_error;
  };

  class LoadError : public Exception {
    using Exception::Exception;
  };

  class SymbolError : public Exception {
    using Exception::Exception;
  };

  explicit DynamicLibrary(const std::string& path) {
    handle_ = open_library(path.c_str());

    if VUNLIKELY (!handle_) {
      throw LoadError("Could not load library \"" + path + "\"\n" + get_error_description());
    }
  }

  ~DynamicLibrary() {
    if VLIKELY (handle_) {
      close_library(handle_);
    }
  }

  DynamicLibrary(DynamicLibrary&& other) noexcept : handle_(other.handle_) { other.handle_ = nullptr; }

  DynamicLibrary& operator=(DynamicLibrary&& other) noexcept {
    if VLIKELY (this != &other) {
      std::swap(handle_, other.handle_);
    }

    return *this;
  }

  template <typename SignatureT>
  SignatureT* get_function(const std::string& name) const {
    if VUNLIKELY (!handle_) {
      throw std::logic_error("The dynamic library handle is null. This object may have been moved from.");
    }

    auto* symbol = locate_symbol(handle_, name.c_str());

    if VUNLIKELY (!symbol) {
      throw SymbolError("Could not get symbol \"" + name + "\"\n" + get_error_description());
    }

#if defined(__GNUC__) && __GNUC__ >= 8
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wcast-function-type"
#endif

    return reinterpret_cast<SignatureT*>(symbol);

#if defined(__GNUC__) && __GNUC__ >= 8
#pragma GCC diagnostic pop
#endif
  }

  NativeHandle native_handle() const noexcept { return handle_; }

 private:
  static NativeHandle open_library(const char* path) noexcept {
#if defined(_WIN32) || defined(_WIN64)
    return ::LoadLibraryA(path);
#else
    return ::dlopen(path, RTLD_NOW | RTLD_LOCAL);
#endif
  }

  static NativeSymbol locate_symbol(NativeHandle lib, const char* name) noexcept {
#if defined(_WIN32) || defined(_WIN64)
    return ::GetProcAddress(lib, name);
#else
    return ::dlsym(lib, name);
#endif
  }

  static void close_library(NativeHandle lib) noexcept {
#if defined(_WIN32) || defined(_WIN64)
    ::FreeLibrary(lib);
#else
    ::dlclose(lib);
#endif
  }

  static std::string get_error_description() noexcept {
#if defined(_WIN32) || defined(_WIN64)
    constexpr size_t kBufferSize = 512;
    const auto error_code = ::GetLastError();

    if (!error_code) {
      return "No error reported by GetLastError";
    }

    char description[kBufferSize];

    const auto language = MAKELANGID(LANG_ENGLISH, SUBLANG_ENGLISH_US);
    const DWORD length =
        ::FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM, nullptr, error_code, language, description, kBufferSize, nullptr);

    return (length == 0) ? "Unknown error (FormatMessage failed)" : description;
#else
    const auto* description = ::dlerror();

    return (description == nullptr) ? "No error reported by dlerror" : description;
#endif
  }

  NativeHandle handle_{nullptr};

  VLINK_DISALLOW_COPY_AND_ASSIGN(DynamicLibrary)
};

// PluginEntry
struct PluginEntry final {
  std::unique_ptr<DynamicLibrary> loader;
  std::string plugin_complex_id;
  Logger::Level log_level{Logger::kTrace};
};

// Plugin::Impl
struct Plugin::Impl final {
  Logger::Level log_level{Logger::kTrace};
  std::shared_mutex mtx;
  std::unordered_map<std::string, std::shared_ptr<PluginEntry>> plugin_map;
};

// Plugin
Plugin::Plugin() : impl_(std::make_unique<Impl>()) {}

void Plugin::set_log_level(Logger::Level level) { impl_->log_level = level; }

Logger::Level Plugin::get_log_level() const { return impl_->log_level; }

Plugin::~Plugin() = default;

std::deque<std::string> Plugin::default_search_path() {
  static std::string current_dir = get_current_dir();
  static std::string plugin_dir = Utils::get_env("VLINK_PLUGIN_DIR");
  static std::string local_app_dir = Utils::get_app_dir();

  std::deque<std::string> search_path{
      current_dir,
      local_app_dir,
      local_app_dir + "/../lib64",
      local_app_dir + "/../lib",
      local_app_dir + "/lib64",
      local_app_dir + "/lib",
      "/lib64",
      "/lib",
  };

  if (!plugin_dir.empty()) {
    search_path.emplace_front(plugin_dir);
  }

  return search_path;
}

void Plugin::clear() {
  std::lock_guard lock(impl_->mtx);
  impl_->plugin_map.clear();
}

Plugin::Handle Plugin::load_and_create(const std::string& plugin_id, const std::string& lib_name,
                                       uint16_t version_major, uint16_t version_minor, const std::string& dir_name,
                                       const std::deque<std::string>& search_paths, const std::string& function_name,
                                       std::shared_ptr<PluginEntry>* plugin_entry) {
  if VUNLIKELY (plugin_id.empty()) {
    if (impl_->log_level <= Logger::kError) {
      VLOG_E("Plugin: Plugin id is empty.");
    }

    return nullptr;
  }

  if VUNLIKELY (lib_name.empty()) {
    if (impl_->log_level <= Logger::kError) {
      VLOG_E("Plugin: Lib name is empty.");
    }

    return nullptr;
  }

  std::string plugin_complex_id = lib_name + "@" + plugin_id;

  {
    std::unique_lock load_lock(impl_->mtx);

    if VUNLIKELY (impl_->plugin_map.count(plugin_complex_id) != 0U) {
      if (impl_->log_level <= Logger::kError) {
        VLOG_E("Plugin: Already loaded (", plugin_complex_id, ").");
      }

      return nullptr;
    }
  }

  std::string plugin_path;

  {
    std::string plugin_name = DynamicLibrary::kFilenamePrefix + lib_name + DynamicLibrary::kFilenameSuffix;
    std::string check_path;

    for (const auto& path : search_paths) {
      if (dir_name.empty()) {
        // NOLINTNEXTLINE(performance-inefficient-string-concatenation)
        check_path = path + "/" + plugin_name;
      } else {
        // NOLINTNEXTLINE(performance-inefficient-string-concatenation)
        check_path = path + "/" + dir_name + "/" + plugin_name;
      }

      if (check_exists(check_path)) {
        plugin_path = std::move(check_path);
        break;
      }
    }
  }

  Handle handle = nullptr;

  if VUNLIKELY (plugin_path.empty()) {
    if (impl_->log_level <= Logger::kError) {
      VLOG_E("Plugin: Cannot find plugin (", plugin_complex_id, ").");
    }

    return handle;
  }

  if (impl_->log_level <= Logger::kTrace) {
    VLOG_T("Plugin: Loading plugin: ", plugin_path, ".");
  }

  try {
    auto entry = std::make_shared<PluginEntry>();
    entry->loader = std::make_unique<DynamicLibrary>(std::move(plugin_path));
    entry->plugin_complex_id = plugin_complex_id;
    entry->log_level = impl_->log_level;

    auto create_function =
        entry->loader->get_function<Handle(const char*, const char*, uint16_t, uint16_t, uint8_t)>(function_name);

    if VUNLIKELY (!create_function) {
      if (impl_->log_level <= Logger::kError) {
        VLOG_E("Plugin: Cannot find symbol function to create (", plugin_complex_id, ").");
      }

      return handle;
    }

    handle = create_function(lib_name.c_str(), plugin_id.c_str(), version_major, version_minor, impl_->log_level);

    if VUNLIKELY (!handle) {
      if (impl_->log_level <= Logger::kError) {
        VLOG_E("Plugin: Failed to create handle (", plugin_complex_id, ").");
      }

      return handle;
    }

    if (impl_->log_level <= Logger::kTrace) {
      VLOG_T("Plugin: Loaded successfully (", plugin_complex_id, ").");
    }

    bool inserted = false;

    {
      std::unique_lock load_lock(impl_->mtx);
      auto [iter, emplaced] = impl_->plugin_map.emplace(plugin_complex_id, entry);
      (void)iter;
      inserted = emplaced;
    }

    if VUNLIKELY (!inserted) {
      destroy(entry, handle);
      return nullptr;
    }

    if (plugin_entry) {
      *plugin_entry = entry;
    }

    return handle;
  } catch (const DynamicLibrary::Exception& e) {
    if (impl_->log_level <= Logger::kError) {
      VLOG_E("Plugin: Failed to load plugin (", plugin_complex_id, "): ", e.what(), ".");
    }

    return handle;
  }
}

bool Plugin::unload(const std::string& plugin_complex_id) {
  if VUNLIKELY (plugin_complex_id.empty()) {
    if (impl_->log_level <= Logger::kError) {
      VLOG_E("Plugin: Plugin id is empty.");
    }

    return false;
  }

  if VUNLIKELY (!has_loaded(plugin_complex_id)) {
    if (impl_->log_level <= Logger::kError) {
      VLOG_E("Plugin: Not loaded (", plugin_complex_id, ").");
    }

    return false;
  }

  {
    std::lock_guard lock(impl_->mtx);
    impl_->plugin_map.erase(plugin_complex_id);
  }

  return true;
}

bool Plugin::has_loaded(const std::string& plugin_complex_id) {
  std::shared_lock lock(impl_->mtx);
  return impl_->plugin_map.count(plugin_complex_id) != 0;
}

bool Plugin::destroy(std::shared_ptr<PluginEntry> plugin_entry, Handle handle, const std::string& function_name) {
  if VUNLIKELY (!plugin_entry || !plugin_entry->loader || !handle) {
    return false;
  }

  try {
    auto destroy_function = plugin_entry->loader->get_function<bool(Handle)>(function_name);

    if VUNLIKELY (!destroy_function) {
      if (plugin_entry->log_level <= Logger::kError) {
        VLOG_E("Plugin: Cannot find symbol function to destroy (", plugin_entry->plugin_complex_id, ").");
      }

      return false;
    }

    if VUNLIKELY (!destroy_function(handle)) {
      if (plugin_entry->log_level <= Logger::kError) {
        VLOG_E("Plugin: Failed to destroy handle (", plugin_entry->plugin_complex_id, ").");
      }

      return false;
    }
  } catch (const std::exception& e) {
    if (plugin_entry->log_level <= Logger::kError) {
      VLOG_E("Plugin: Failed to destroy handle (", plugin_entry->plugin_complex_id, "): ", e.what(), ".");
    }

    return false;
  } catch (...) {
    if (plugin_entry->log_level <= Logger::kError) {
      VLOG_E("Plugin: Failed to destroy handle (", plugin_entry->plugin_complex_id, "): non-std exception.");
    }

    return false;
  }

  return true;
}

bool Plugin::process_plugin_internal(const std::string& lib_name, const std::string& local_plugin_id,
                                     uint16_t local_version_major, uint16_t local_version_minor,
                                     const std::string& target_plugin_id, uint16_t target_version_major,
                                     uint16_t target_version_minor, uint8_t log_level) {
  if (log_level <= Logger::kInfo) {
    VLOG_I("Plugin: ", lib_name, "@", local_plugin_id, "#", local_version_major, ".", local_version_minor, ".");
  }

  if VUNLIKELY (target_plugin_id.empty() || target_plugin_id != local_plugin_id) {
    if (log_level <= Logger::kError) {
      VLOG_E("Plugin: Plugin id mismatch: expected '", local_plugin_id, "', got '", target_plugin_id, "'.");
    }

    return false;
  }

  if VUNLIKELY (target_version_major != local_version_major || target_version_minor > local_version_minor) {
    if (log_level <= Logger::kError) {
      VLOG_E("Plugin: Version mismatch: local ", local_version_major, ".", local_version_minor, ", required ",
             target_version_major, ".", target_version_minor, ".");
    }

    return false;
  }

  return true;
}

}  // namespace vlink
