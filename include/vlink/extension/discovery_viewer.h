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
 * @file discovery_viewer.h
 * @brief Live aggregator of VLink endpoint announcements emitted by @c DiscoveryReporter.
 *
 * @details
 * @c DiscoveryViewer is the listening counterpart of @c DiscoveryReporter.  It joins the
 * discovery UDP multicast group, decodes incoming announcements, and maintains an
 * in-memory snapshot of every URL/process currently advertised on the host or network.
 * It powers @c vlink-cli, dashboards and any custom monitoring tool that needs a live
 * topology view.
 *
 * Filter modes select which announcements survive into the snapshot:
 *
 * | Value               | Description                                                                  |
 * | ------------------- | ---------------------------------------------------------------------------- |
 * | @c kFilterNone      | Keep every announcement regardless of origin                                 |
 * | @c kFilterAvailable | Drop remote announcements for local-only URL schemes (@c intra, @c shm, ...) |
 * | @c kFilterNative    | Keep only announcements emitted by the same host as the viewer               |
 *
 * Viewer interaction:
 *
 * @verbatim
 *   DiscoveryReporter --(UDP multicast)--> +----------------+    Callback(info_list)
 *                                          |    Viewer      | ---------------------> user code
 *   Heartbeat timeout         ----+--->    |  (MessageLoop) | <--- get_info_list()
 *   process_offline()         ----+--->    +----------------+
 * @endverbatim
 *
 * @par Example
 * @code
 * vlink::DiscoveryViewer viewer(vlink::DiscoveryViewer::kFilterAvailable);
 * viewer.register_callback([](const std::vector<vlink::DiscoveryViewer::Info>& list) {
 *   for (const auto& entry : list) {
 *     VLOG_I("url=", entry.url, " ser=", entry.ser_type,
 *            " hosts=", entry.process_list.size());
 *   }
 * });
 * viewer.async_run();
 * @endcode
 *
 * @note The callback runs on the viewer's @c MessageLoop thread; copy the snapshot if it
 * needs to outlive the call.  Endpoints whose heartbeat lapses are pruned automatically.
 */

#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "../base/functional.h"
#include "../base/macros.h"
#include "../base/message_loop.h"
#include "../impl/types.h"

namespace vlink {

/**
 * @class DiscoveryViewer
 * @brief @c MessageLoop-based aggregator of live VLink endpoint announcements.
 *
 * @details
 * Construct with the desired filter and start with @c async_run().  The viewer
 * continuously rebuilds an @c Info list and notifies the registered callback every time
 * the topology changes.
 */
class VLINK_EXPORT DiscoveryViewer : public MessageLoop {
 public:
  /**
   * @brief Selects which announcements contribute to the live snapshot.
   *
   * | Value             | Effect                                                                |
   * | ----------------- | --------------------------------------------------------------------- |
   * | kFilterNone       | All announcements visible                                             |
   * | kFilterAvailable  | Drop remote announcements for local-only URL schemes                  |
   * | kFilterNative     | Keep only announcements from the same host                            |
   */
  enum FilterType : uint8_t {
    kFilterNone = 0,       ///< No filtering applied.
    kFilterAvailable = 1,  ///< Drop remote announcements for local-only URL schemes.
    kFilterNative = 2,     ///< Keep only same-host announcements.
  };

  /**
   * @struct Process
   * @brief Identity of a process that hosts at least one endpoint for a URL.
   */
  struct VLINK_EXPORT Process final {
    uint32_t type{0};     ///< Bitmask of @c ImplType kinds advertised by this process.
    std::string host;     ///< Host name of the process.
    uint32_t pid{0};      ///< Process identifier.
    std::string name;     ///< Process or application name.
    std::string ip;       ///< IP address of the host.
    double profiler{-1};  ///< Most recent CPU usage sample (-1 when unavailable).

    /**
     * @brief Defines a stable ordering between two process descriptors.
     *
     * @details
     * Sort key is type, then host, IP, name and finally PID.
     *
     * @param target Right-hand operand.
     * @return @c true when @c *this should appear before @p target.
     */
    bool operator<(const Process& target) const noexcept;
  };

  /**
   * @struct Info
   * @brief One row of the discovery snapshot, describing a URL and its publishers/subscribers.
   *
   * @details
   * Each row aggregates the communication kinds, serialisation type, schema family and
   * the list of processes that have announced a matching endpoint.
   */
  struct VLINK_EXPORT Info final {
    int sort_index{-1};                            ///< Stable sort key assigned internally by the viewer.
    uint32_t type{0};                              ///< Bitmask of @c ImplType kinds for this URL.
    std::string url;                               ///< Fully-qualified VLink URL.
    std::string ser_type;                          ///< Serialisation type name announced for this URL.
    SchemaType schema_type{SchemaType::kUnknown};  ///< Coarse schema family derived from announcements.
    std::vector<Process> process_list;             ///< Processes currently hosting this URL.

    /**
     * @brief Defines a stable ordering between two snapshot rows.
     *
     * @details
     * Sort key is type, then sort_index, URL, schema family, serialisation type and
     * finally the process list.
     *
     * @param target Right-hand operand.
     * @return @c true when @c *this should appear before @p target.
     */
    bool operator<(const Info& target) const noexcept;
  };

  /**
   * @brief Callback signature delivered whenever the snapshot changes.
   *
   * @details
   * Invoked on the viewer's @c MessageLoop thread with the freshly built list.
   */
  using Callback = Function<void(const std::vector<Info>& info_list)>;

  /**
   * @brief Translates a discovery role token to the corresponding @c ImplType bit.
   *
   * @param str Role token: @c "Ser", @c "Cli", @c "Pub", @c "Sub", @c "Set" or @c "Get".
   * @return Matching @c ImplType, or 0 when the token is not recognised.
   */
  [[nodiscard]] static ImplType convert_type(std::string_view str);

  /**
   * @brief Formats an @c ImplType bitmask as a human-readable label.
   *
   * @param type ImplType bitmask.
   * @return Display string suitable for tooling output.
   */
  [[nodiscard]] static std::string convert_type_to_view(uint32_t type);

  /**
   * @brief Formats an @c ImplType bitmask together with its process list.
   *
   * @param type         ImplType bitmask.
   * @param process_list Processes to include in the display.
   * @return Combined display string.
   */
  [[nodiscard]] static std::string convert_type_to_view(uint32_t type, const std::vector<Process>& process_list);

  /**
   * @brief Returns the UDP multicast/broadcast address used by the discovery subsystem.
   */
  [[nodiscard]] static std::string get_listen_address();

  /**
   * @brief Returns the process-global viewer, created on first call with @c kFilterNone.
   *
   * @return Raw pointer to the global viewer.
   */
  static DiscoveryViewer* global_get();

  /**
   * @brief Builds the viewer with the requested filter mode.
   *
   * @param type Filter selecting which announcements survive (default: @c kFilterNone).
   */
  explicit DiscoveryViewer(FilterType type = kFilterNone);

  /**
   * @brief Stops the viewer loop and releases resources.
   */
  ~DiscoveryViewer() override;

  /**
   * @brief Registers the callback notified on every snapshot change.
   *
   * @details
   * Replaces any previously installed callback; only the most recent registration
   * remains effective.
   *
   * @param callback Function invoked with the new @c Info list.
   */
  void register_callback(Callback&& callback);

  /**
   * @brief Returns a copy of the live snapshot at the moment of the call.
   *
   * @return Vector of @c Info rows.
   */
  [[nodiscard]] std::vector<Info> get_info_list();

  /**
   * @brief Resolves the announced serialisation type for @p url.
   *
   * @param url Topic URL.
   * @return Announced serialisation type, or an empty string when unknown.
   */
  [[nodiscard]] std::string get_ser_type(const std::string& url) const;

  /**
   * @brief Resolves the announced coarse schema family for @p url.
   *
   * @param url Topic URL.
   * @return @c SchemaType, or @c SchemaType::kUnknown when unavailable.
   */
  [[nodiscard]] SchemaType get_schema_type(const std::string& url) const;

 protected:
  size_t get_max_task_count() const override;

  uint32_t get_max_elapsed_time() const override;

  void on_begin() override;

  void on_end() override;

 private:
  void process_timeout();

  void process_offline(std::string_view hostname, uint32_t pid, std::string_view process_name);

  void sort_url();

  void report_list();

  struct Impl;
  std::unique_ptr<Impl> impl_;

  VLINK_DISALLOW_COPY_AND_ASSIGN(DiscoveryViewer)
};

}  // namespace vlink
