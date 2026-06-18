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

/**
 * @file plugin.h
 * @brief Strongly-typed shared-library plugin loader with ID and version verification.
 *
 * @details
 * @c vlink::Plugin wraps the host platform's dynamic-library API (@c dlopen / @c LoadLibrary)
 * and resolves the @c vlink_plugin_create / @c vlink_plugin_destroy entry points exported
 * by every plugin built with @c VLINK_PLUGIN_DECLARE.  Plugin implementations are bound to
 * an abstract interface type so the loader can verify the ABI contract before any virtual
 * call crosses the library boundary.
 *
 * Plugin lifecycle observed by the loader:
 *
 * @verbatim
 *     load() ---> open() ---> create() ---> in use ---+
 *       ^                                             |
 *       |                                             v
 *     clear() <--- close() <--- destroy() <--- unload()
 * @endverbatim
 *
 * Interface / implementation contract:
 *
 * | Side               | Required macro                              | Result                       |
 * | ------------------ | ------------------------------------------- | ---------------------------- |
 * | Abstract interface | @c VLINK_PLUGIN_REGISTER                    | Plugin ID = demangled name   |
 * | Abstract interface | @c VLINK_PLUGIN_REGISTER_BY_ID(_, "id")     | Plugin ID = literal string   |
 * | Concrete impl .cc  | @c VLINK_PLUGIN_DECLARE(Impl, major, minor) | Exports create/destroy ABI   |
 *
 * @par Version verification
 * @c process_plugin_internal() runs inside the plugin entry point and verifies that the
 * plugin ID matches and that the plugin's major version equals the host's required major
 * and the plugin's minor is no lower than the host's required minor.  Mismatches return
 * @c nullptr from @c vlink_plugin_create() so an incompatible binary never crosses the
 * vtable boundary.
 *
 * @par Example
 * @code
 * // Interface (header):
 * class MyPlugin {
 *   VLINK_PLUGIN_REGISTER(MyPlugin)
 *  public:
 *   virtual ~MyPlugin() = default;
 *   virtual void do_work() = 0;
 * };
 *
 * // Implementation (.cc):
 * class MyPluginImpl : public MyPlugin {
 *   VLINK_PLUGIN_REGISTER(MyPlugin)
 *  public:
 *   void do_work() override { ... }
 * };
 * VLINK_PLUGIN_DECLARE(MyPluginImpl, 1, 0)
 *
 * // Host:
 * vlink::Plugin plugin;
 * if (auto impl = plugin.load<MyPlugin>("my_plugin_impl", 1, 0)) {
 *   impl->do_work();
 * }
 * @endcode
 */

#pragma once

#include <cstdint>
#include <deque>
#include <memory>
#include <string>
#include <string_view>

#include "./logger.h"
#include "./macros.h"
#include "./name_detector.h"

/**
 * @def VLINK_PLUGIN_CREATE_FUNC_NAME
 * @brief Symbol name of the plugin construction entry point exported by @c VLINK_PLUGIN_DECLARE.
 */
#define VLINK_PLUGIN_CREATE_FUNC_NAME vlink_plugin_create

/**
 * @def VLINK_PLUGIN_DESTROY_FUNC_NAME
 * @brief Symbol name of the plugin destruction entry point exported by @c VLINK_PLUGIN_DECLARE.
 */
#define VLINK_PLUGIN_DESTROY_FUNC_NAME vlink_plugin_destroy

namespace vlink {

struct PluginEntry;

/**
 * @class Plugin
 * @brief Manager that loads, tracks and unloads shared-library plugins by interface type.
 *
 * @details
 * A single @c Plugin instance can host multiple distinct interface types simultaneously
 * and tracks each loaded library so repeated @c load() calls share the underlying
 * @c dlopen handle.  All operations are thread safe through the internal implementation.
 */
class VLINK_EXPORT Plugin final {
 public:
  /**
   * @brief Opaque handle to a loaded shared library; treated as a token by the public API.
   */
  using Handle = void*;

  /**
   * @brief Constructs an empty plugin manager with no libraries loaded.
   */
  Plugin();

  /**
   * @brief Destroys the plugin manager and unloads every still-resident library via @c clear().
   */
  ~Plugin();

  /**
   * @brief Sets the verbosity level used for plugin diagnostic messages.
   *
   * @param level  Logger level to apply to plugin load/unload tracing.
   */
  void set_log_level(Logger::Level level);

  /**
   * @brief Returns the verbosity level currently used for plugin diagnostics.
   *
   * @return Current logger level.
   */
  [[nodiscard]] Logger::Level get_log_level() const;

  /**
   * @brief Returns the default ordered search path used when locating plugin libraries.
   *
   * @details
   * The list contains, in priority order, the executable directory, the system library
   * directories appropriate for the platform, and the current working directory.
   *
   * @return Deque of directory paths searched left to right.
   */
  [[nodiscard]] static std::deque<std::string> default_search_path();

  /**
   * @brief Loads a shared library that implements interface @c T and returns a tracked handle.
   *
   * @details
   * The loader appends the platform's library prefix/suffix to @p lib_name, scans
   * @p search_paths, opens the first matching file, invokes the @p function_name entry
   * point with the caller's ID and version, and finally wraps the returned object pointer
   * in a @c shared_ptr<T> whose deleter invokes @c vlink_plugin_destroy.
   *
   * @tparam T             Interface type carrying @c get_plugin_id() (added by
   *                       @c VLINK_PLUGIN_REGISTER or @c VLINK_PLUGIN_REGISTER_BY_ID).
   * @param lib_name       Library file name without prefix/suffix.
   * @param version_major  Required interface major version.
   * @param version_minor  Required interface minor version.
   * @param dir_name       Optional directory searched before @p search_paths.
   * @param search_paths   Ordered fallback search list.  Default: @c default_search_path().
   * @param function_name  Symbol name of the construction entry point.
   * @return @c shared_ptr<T> owning the plugin instance, or @c nullptr on failure.
   */
  template <class T>
  [[nodiscard]] std::shared_ptr<T> load(
      const std::string& lib_name, uint16_t version_major, uint16_t version_minor, const std::string& dir_name = "",
      const std::deque<std::string>& search_paths = default_search_path(),
      const std::string& function_name = VLINK_MACRO_STRING_GET(VLINK_PLUGIN_CREATE_FUNC_NAME));

  /**
   * @brief Removes a previously loaded plugin from the registry.
   *
   * @details
   * The shared library is finally unmapped once every @c shared_ptr returned by
   * @c load() has been destroyed; this call only releases the tracker entry.
   *
   * @tparam T       Interface type used during the original @c load() call.
   * @param lib_name Library file name passed to @c load().
   * @return @c true when the registry entry existed and was removed.
   */
  template <class T>
  bool unload(const std::string& lib_name);

  /**
   * @brief Reports whether a plugin for interface @c T is currently registered.
   *
   * @tparam T       Interface type used during the original @c load() call.
   * @param lib_name Library file name passed to @c load().
   * @return @c true when the registry entry is present.
   */
  template <class T>
  [[nodiscard]] bool has_loaded(const std::string& lib_name);

  /**
   * @brief Builds the composite key used internally to identify a (library, interface) pair.
   *
   * @details
   * The key has the form @c lib_name + "@" + T::get_plugin_id() so the same shared library
   * can be loaded twice when consumed via two different interfaces.
   *
   * @tparam T       Interface type.
   * @param lib_name Library file name.
   * @return Composite identifier string.
   */
  template <class T>
  [[nodiscard]] std::string get_plugin_complex_id(const std::string& lib_name);

  /**
   * @brief Unloads every library currently tracked by this manager.
   */
  void clear();

  /**
   * @brief Internal version/ID gate invoked from the @c VLINK_PLUGIN_DECLARE entry point.
   *
   * @details
   * Compares the plugin's exported ID and version against the host's expectations, emitting
   * informational or error diagnostics gated by @p log_level.  User code should never call
   * this function directly.  @p log_level only filters this function's own output and is
   * not propagated into the plugin module's runtime logger.
   *
   * @param lib_name             Library file name (used as a tag in diagnostic output).
   * @param local_plugin_id      Plugin ID compiled into the plugin binary.
   * @param local_version_major  Major version compiled into the plugin binary.
   * @param local_version_minor  Minor version compiled into the plugin binary.
   * @param target_plugin_id     Plugin ID required by the host caller.
   * @param target_version_major Major version required by the host caller.
   * @param target_version_minor Minor version required by the host caller.
   * @param log_level            Threshold used to filter this function's own diagnostics.
   * @return @c true when IDs match and @c local_major @c == @c target_major and
   *         @c local_minor @c >= @c target_minor.
   */
  static bool process_plugin_internal(const std::string& lib_name, const std::string& local_plugin_id,
                                      uint16_t local_version_major, uint16_t local_version_minor,
                                      const std::string& target_plugin_id, uint16_t target_version_major,
                                      uint16_t target_version_minor, uint8_t log_level);

 private:
  Handle load_and_create(const std::string& plugin_id, const std::string& lib_name, uint16_t version_major,
                         uint16_t version_minor, const std::string& dir_name,
                         const std::deque<std::string>& search_paths, const std::string& function_name,
                         std::shared_ptr<PluginEntry>* plugin_entry);

  bool unload(const std::string& plugin_complex_id);

  bool has_loaded(const std::string& plugin_complex_id);

  static bool destroy(std::shared_ptr<PluginEntry> plugin_entry, Handle handle,
                      const std::string& function_name = VLINK_MACRO_STRING_GET(VLINK_PLUGIN_DESTROY_FUNC_NAME));

  struct Impl;
  std::unique_ptr<Impl> impl_;

  VLINK_DISALLOW_COPY_AND_ASSIGN(Plugin)
};

////////////////////////////////////////////////////////////////
/// Details
////////////////////////////////////////////////////////////////

template <class T>
inline std::shared_ptr<T> Plugin::load(const std::string& lib_name, uint16_t version_major, uint16_t version_minor,
                                       const std::string& dir_name, const std::deque<std::string>& search_paths,
                                       const std::string& function_name) {
  static_assert(!T::get_plugin_id().empty(), "Plugin id can not be empty.");

  std::shared_ptr<PluginEntry> plugin_entry;
  auto* handle = load_and_create(T::get_plugin_id().data(), lib_name, version_major, version_minor, dir_name,
                                 search_paths, function_name, &plugin_entry);

  if VUNLIKELY (!handle) {
    return nullptr;
  }

  return std::shared_ptr<T>(static_cast<T*>(handle), [plugin_entry = std::move(plugin_entry)](T* interface_ptr) {
    destroy(std::move(plugin_entry), interface_ptr);
  });
}

template <class T>
inline bool Plugin::unload(const std::string& lib_name) {
  static_assert(!T::get_plugin_id().empty(), "Plugin id can not be empty.");

  return unload(get_plugin_complex_id<T>(lib_name));
}

template <class T>
inline bool Plugin::has_loaded(const std::string& lib_name) {
  static_assert(!T::get_plugin_id().empty(), "Plugin id can not be empty.");

  return has_loaded(get_plugin_complex_id<T>(lib_name));
}

template <class T>
inline std::string Plugin::get_plugin_complex_id(const std::string& lib_name) {
  static_assert(!T::get_plugin_id().empty(), "Plugin id can not be empty.");

  return lib_name + "@" + T::get_plugin_id().data();
}

}  // namespace vlink

////////////////////////////////////////////////////////////////
/// Macro Definitions
////////////////////////////////////////////////////////////////

#if defined(_WIN32) || defined(__CYGWIN__)
#define VLINK_PLUGIN_EXPORT __declspec(dllexport)
#else
#define VLINK_PLUGIN_EXPORT __attribute__((visibility("default")))
#endif

/**
 * @def VLINK_PLUGIN_REGISTER(InterfaceType)
 * @brief Declares a plugin's identity from the demangled name of its abstract interface.
 *
 * @details
 * Injects a @c static @c constexpr @c get_plugin_id() member that returns the demangled
 * name of @p InterfaceType.  Static assertions enforce that the interface is abstract
 * and exposes a virtual destructor so polymorphic delete across the library boundary
 * is well defined.
 *
 * @param InterfaceType  Abstract interface class the plugin implements.
 */
#define VLINK_PLUGIN_REGISTER(InterfaceType)                                                                         \
 public:                                                                                                             \
  static constexpr std::string_view get_plugin_id() {                                                                \
    static_assert(std::is_abstract_v<InterfaceType>, "Plugin interface must be abstract class.");                    \
    static_assert(std::has_virtual_destructor_v<InterfaceType>, "Plugin interface must have a virtual destructor."); \
    return vlink::NameDetector::get<InterfaceType>();                                                                \
  }

/**
 * @def VLINK_PLUGIN_REGISTER_BY_ID(InterfaceType, PluginID)
 * @brief Declares a plugin's identity from an explicit literal string.
 *
 * @details
 * Same contract as @c VLINK_PLUGIN_REGISTER but @c get_plugin_id() returns @p PluginID
 * instead of the demangled type name, which is useful when the plugin ID must remain
 * stable across refactors that rename the interface class.
 *
 * @param InterfaceType  Abstract interface class the plugin implements.
 * @param PluginID       Literal string used as the plugin identity.
 */
#define VLINK_PLUGIN_REGISTER_BY_ID(InterfaceType, PluginID)                                                         \
 public:                                                                                                             \
  static constexpr std::string_view get_plugin_id() {                                                                \
    static_assert(std::is_abstract_v<InterfaceType>, "Plugin interface must be abstract class.");                    \
    static_assert(std::has_virtual_destructor_v<InterfaceType>, "Plugin interface must have a virtual destructor."); \
    return PluginID;                                                                                                 \
  }

/**
 * @def VLINK_PLUGIN_DECLARE(ImplementType, VersionMajor, VersionMinor)
 * @brief Emits the @c extern @c "C" construction and destruction entry points exported by a plugin module.
 *
 * @details
 * The construction entry point validates the plugin ID and major/minor version against
 * the caller's expectations via @c Plugin::process_plugin_internal() and returns a new
 * instance of @p ImplementType only when the contract holds.  The destruction entry point
 * deletes the implementation pointer.
 *
 * @param ImplementType  Concrete class implementing the abstract interface.
 * @param VersionMajor   Major version exposed by this plugin binary.
 * @param VersionMinor   Minor version exposed by this plugin binary.
 */
#define VLINK_PLUGIN_DECLARE(ImplementType, VersionMajor, VersionMinor)                                         \
  extern "C" {                                                                                                  \
  VLINK_PLUGIN_EXPORT void* VLINK_PLUGIN_CREATE_FUNC_NAME(const char* lib_name, const char* plugin_id,          \
                                                          uint16_t version_major, uint16_t version_minor,       \
                                                          uint8_t log_level) {                                  \
    static_assert(std::is_default_constructible_v<ImplementType>,                                               \
                  "Plugin implementation must have default constructible");                                     \
    static_assert(!ImplementType::get_plugin_id().empty(), "Plugin id can not be empty.");                      \
    static_assert(!std::is_abstract_v<ImplementType>, "Plugin implementation cannot be an abstract class.");    \
                                                                                                                \
    /*NOLINTBEGIN*/                                                                                             \
    if VUNLIKELY (!vlink::Plugin::process_plugin_internal(lib_name, ImplementType::get_plugin_id().data(),      \
                                                          VersionMajor, VersionMinor, plugin_id, version_major, \
                                                          version_minor, log_level)) {                          \
      return nullptr;                                                                                           \
    }                                                                                                           \
                                                                                                                \
    return new ImplementType;                                                                                   \
    /*NOLINTEND*/                                                                                               \
  }                                                                                                             \
                                                                                                                \
  VLINK_PLUGIN_EXPORT bool VLINK_PLUGIN_DESTROY_FUNC_NAME(void* handle) {                                       \
    if VUNLIKELY (!handle) {                                                                                    \
      return false;                                                                                             \
    }                                                                                                           \
                                                                                                                \
    /*NOLINTBEGIN*/                                                                                             \
    delete static_cast<ImplementType*>(handle);                                                                 \
                                                                                                                \
    return true;                                                                                                \
    /*NOLINTEND*/                                                                                               \
  }                                                                                                             \
  }
