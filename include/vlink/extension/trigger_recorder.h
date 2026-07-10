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
 * @file trigger_recorder.h
 * @brief Event-data-recorder engine: a rolling in-memory ring of every live topic, dumped to a bag on trigger.
 *
 * @details
 * @c TriggerRecorder is a self-contained data-plane engine (no RPC, no config parsing -- those belong to the
 * caller).  It continuously discovers every topic on the bus, subscribes to the raw @c Bytes of each, and keeps
 * a rolling per-URL ring buffer holding the most recent @e pre-trigger window of data.  When @c trigger() is
 * called it captures the @b pre + @b post window around the trigger instant and writes the frames to a bag
 * file, rotating old files.  This is the dashcam / EDR pattern: the ring is always recording, the trigger
 * decides what to persist.
 *
 * @par Two distinct plugin roles (never conflate them)
 * The engine accepts two @e different plugin interfaces, bound through @e different methods:
 * - A @b bag plugin (@c BagPluginInterface, bound via @c bind_bag_plugin_interface() or loaded from a shared
 *   library by @c Config::bag_plugin_lib) sits @e inside the write path: its @c on_write() hook parses the true
 *   @b data-plane time out of each payload and re-emits frames reordered by that time -- a sliding-window
 *   reorder, exactly the mechanism a live @c BagWriter uses.  Without a bag plugin the engine writes frames in
 *   @b capture-time order (the monotonic arrival clock), which needs no payload parsing.
 * - A @b trigger plugin (@c TriggerPluginInterface, bound via @c bind_trigger_plugin_interface()) observes the
 *   recorder @e life cycle -- most importantly @c on_dump_finished() once a bag is written, the place to upload
 *   or archive it.  It never rewrites frames.
 *
 * @par Per-URL windows
 * Each URL may override the global default @b pre (milliseconds before the trigger) and @b post (milliseconds
 * after the trigger) window, plus a max packet size, a per-URL byte cap and @c only_front / @c only_back
 * restrictions.  A global URL whitelist / blacklist selects which topics participate.
 *
 * @par Retention model (constant retention)
 * Every URL retains @c pre_u + @c max_post_all + @c 2*retention_guard of history, where @c max_post_all is the
 * largest @b post across all URLs.  A trigger at time @e T dumps at @e T + @c max_post_all + @c retention_guard
 * (immediately when no URL has a post window),
 * so both window boundaries carry a @c retention_guard cushion: the ring covers @c [T-pre_u-guard, T+post_u+..]
 * at dump time, a superset of the target @c [T-pre_u, T+post_u] that is then filtered.  This keeps the ingest
 * hot path branch-free (no "is a trigger active" check).
 *
 * @note The subscriber callback (data into the ring) is the hot path: it runs on the transport dispatch
 *       thread(s), takes only a per-URL lock, copies the payload once with @c Bytes::deep_copy and is O(1).
 * @warning Constant retention couples memory globally: a single URL configured with a large @b post raises the
 *          retention -- and therefore the memory -- of @e every URL.  Cap @b post where memory is tight.
 * @warning When a bag plugin performs a sliding-window reorder it holds part of the window buffered until its
 *          @c flush(), so the peak memory during a dump can approach twice the window size.
 * @warning @c busy_skip_data drops incoming data while a dump is in flight, leaving time holes in the ring that
 *          affect the completeness of subsequent triggers.
 * @warning With @c destroy_on_offline, buffers of URLs going offline while a dump is in flight are kept aside
 *          for that dump, so peak memory can temporarily exceed @c max_cache_size by the retained amount.
 *
 * @par Usage
 * @code
 * vlink::TriggerRecorder::Config config;
 * config.dump_dir        = "/data/edr";
 * config.default_pre_ms  = 60'000;
 * config.default_post_ms = 5'000;
 *
 * vlink::TriggerRecorder::UrlConfig camera;
 * camera.pre_ms  = 15'000;  // pre=15s
 * camera.post_ms = 0;       // post=0
 * config.url_overrides["dds://camera/front"] = camera;
 *
 * vlink::TriggerRecorder recorder(config);
 * recorder.start();
 * // ... later, on an external event ...
 * vlink::TriggerRecorder::TriggerParams params;
 * params.reason = "hard-brake";
 * recorder.trigger(params);
 * @endcode
 */

#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "./discovery_viewer.h"

namespace vlink {

class BagPluginInterface;
class TriggerPluginInterface;

/**
 * @class TriggerRecorder
 * @brief Rolling in-memory recorder that dumps a time-reordered pre/post window to a bag on trigger.
 *
 * @details
 * Construct with a @c Config, call @c start() to begin discovery + buffering, then @c trigger() to persist a
 * window.  Triggers are serialised: a second @c trigger() while one is in flight is rejected.  All heavy work
 * (the post-trigger wait, the reorder and the disk write) happens asynchronously on a dedicated loop, so
 * @c trigger() returns immediately after enqueueing.
 */
class VLINK_EXPORT TriggerRecorder {
 public:
  /**
   * @enum OverflowPolicy
   * @brief What to do when a byte cap (per-URL @c max_size or global @c max_cache_size) would be exceeded.
   *
   * @details
   * Eviction is always local to the URL receiving the incoming frame: even when the @b global cap is the one
   * exceeded, @c kCoverOldest only reclaims space from that URL's own ring, so pressure from one URL never
   * evicts another URL's buffered history.  When the ingesting URL's ring cannot free enough space, the
   * incoming frame is dropped.
   */
  enum OverflowPolicy : uint8_t {
    kCoverOldest = 0,  ///< Evict the oldest buffered frame(s) to make room for the newest.
    kDropNewest = 1,   ///< Discard the incoming frame and keep the existing buffer.
  };

  /**
   * @enum FileType
   * @brief On-disk container format for the dumped bag.
   */
  enum FileType : uint8_t {
    kVdb = 0,   ///< SQLite-backed VDB container (@c .vdb).
    kVcap = 1,  ///< MCAP container (@c .vcap).
  };

  /**
   * @struct UrlConfig
   * @brief Per-URL overrides; any field left negative falls back to the matching @c Config default.
   */
  struct UrlConfig final {
    int64_t pre_ms{-1};           ///< Pre-trigger window in ms; <0 uses @c Config::default_pre_ms.
    int64_t post_ms{-1};          ///< Post-trigger window in ms; <0 uses @c Config::default_post_ms.
    int64_t max_packet_size{-1};  ///< Drop packets larger than this many bytes; <0 uses default, 0 disables.
    int64_t max_size{-1};         ///< Per-URL ring byte cap; <0 uses default, 0 disables.
    bool only_front{false};       ///< Record only the pre-trigger side for this URL.
    bool only_back{false};        ///< Record only the post-trigger side for this URL.

    UrlConfig() {}  // NOLINT(modernize-use-equals-default)
  };

  /**
   * @struct Config
   * @brief Engine-wide configuration; passed once to the constructor and read-only afterwards.
   */
  struct Config final {
    std::string dump_dir;                                ///< Output directory; empty => {tmp}/vlink-trigger.
    FileType file_type{kVdb};                            ///< Bag container format.
    int64_t default_pre_ms{60'000};                      ///< Default pre-trigger window in ms.
    int64_t default_post_ms{5'000};                      ///< Default post-trigger window in ms.
    int64_t default_max_packet_size{4LL * 1024 * 1024};  ///< Default per-packet byte limit in bytes (0 = unlimited).
    int64_t default_max_size{0};                         ///< Default per-URL ring byte cap (0 = unlimited).
    int64_t max_cache_size{1024LL * 1024 * 1024};        ///< Global ring byte cap across all URLs.
    int64_t retention_guard_ms{300};                     ///< Extra retention margin to absorb dump-timer jitter.
    int max_dump_file_count{16};                ///< Rotation cap; only auto-named dumps trigger dump_dir rotation.
    bool enable_compress{true};                 ///< Compress the dumped bag.
    bool busy_skip_data{false};                 ///< Drop incoming data while a dump is in flight.
    bool destroy_on_offline{false};             ///< Destroy offline subscribers; an in-flight dump keeps their data.
    OverflowPolicy overflow{kCoverOldest};      ///< Byte-cap overflow policy.
    int64_t sleep_interval{4LL * 1024 * 1024};  ///< Dump I/O throttle: sleep after this many bytes written.
    int64_t sleep_time_ms{0};                   ///< Dump I/O throttle: sleep duration in ms (0 disables).
    std::string dds_ip;                         ///< When non-empty, bind subscribers to this DDS IP (native mode).
    DiscoveryViewer::FilterType discovery_filter{DiscoveryViewer::kFilterAvailable};  ///< Discovery filter.
    std::vector<std::string> whitelist;                        ///< If non-empty, only these exact URLs are recorded.
    std::vector<std::string> blacklist;                        ///< These exact URLs are never recorded.
    std::unordered_map<std::string, UrlConfig> url_overrides;  ///< Per-URL window / limit overrides.
    std::string bag_plugin_lib;    ///< Bag reorder plugin library name (no prefix/suffix); empty = capture-time order.
    std::string bag_plugin_dir;    ///< Optional subdirectory tried under each plugin search path.
    uint16_t bag_plugin_major{2};  ///< Required major version of the bag reorder plugin.
    uint16_t bag_plugin_minor{0};  ///< Required minimum minor version of the bag reorder plugin.

    Config() {}  // NOLINT(modernize-use-equals-default)
  };

  /**
   * @struct TriggerParams
   * @brief Per-trigger parameters; a pure data struct so the engine stays free of any RPC / protobuf dependency.
   */
  struct TriggerParams final {
    std::string reason;     ///< Human-readable trigger reason; stored as the bag tag.
    std::string name_hint;  ///< Optional file-name hint; when empty a timestamp name is generated.
    std::string out_file;   ///< Explicit output path; when empty a name under @c dump_dir is generated.
    int64_t pre_ms{-1};     ///< Per-trigger pre window; <0 uses each URL's configured pre (may only shrink it).
    int64_t post_ms{-1};    ///< Per-trigger post window; <0 uses each URL's configured post (may only shrink it).
    std::unordered_set<std::string> filter_urls;  ///< If non-empty, only these exact URLs are dumped this trigger.
    std::string filter_str;  ///< When @c filter_urls is empty, space-separated substrings select URLs by @c black_mode.
    bool black_mode{false};  ///< With @c filter_str: false keeps matches (whitelist), true drops matches (blacklist).

    TriggerParams() {}  // NOLINT(modernize-use-equals-default)
  };

  /**
   * @brief Builds the recorder from @p config; does not start discovery or buffering yet.
   *
   * @param config Engine-wide configuration, copied and validated internally.
   */
  explicit TriggerRecorder(const Config& config);

  /**
   * @brief Stops discovery, drains any in-flight dump and releases all subscribers and buffers.
   */
  ~TriggerRecorder();

  /**
   * @brief Starts the monotonic capture clock, the dump loop and topic discovery; idempotent.
   *
   * @return @c true when the engine transitioned to running (or was already running); @c false on failure.
   */
  bool start();

  /**
   * @brief Requests a dump of the pre/post window around the current instant.
   *
   * @details
   * Non-blocking: it timestamps the trigger, rejects the call if a dump is already in flight, and enqueues the
   * actual capture / reorder / write onto the dump loop (delayed by @c max_post_all + @c retention_guard_ms
   * when any URL has a post window).  The dump completes asynchronously.  The set of participating URLs is
   * frozen when the call is accepted: topics discovered afterwards do not contribute to this dump, and a topic
   * going offline (@c Config::destroy_on_offline) still contributes its already-buffered window.
   *
   * @param params Optional per-trigger overrides (reason, file name, shrunk windows).
   * @return @c true when the dump was accepted and enqueued; @c false when not running or already dumping.
   */
  bool trigger(const TriggerParams& params = {});

  /**
   * @brief Stops the engine in dependency order (discovery -> in-flight dump -> loop -> subscribers -> buffers).
   *
   * @details
   * Ingest halts at the stop instant; an in-flight dump is still drained (within a bounded wait), but it
   * persists only the data captured before the call -- the remainder of its post window is not awaited
   * with live data.
   */
  void stop();

  /**
   * @brief Reports whether a dump is currently in flight.
   *
   * @return @c true while a trigger's capture / write is running.
   */
  [[nodiscard]] bool is_dumping() const noexcept;

  /**
   * @brief Reports whether the engine is running (started and not yet stopped).
   *
   * @return @c true between a successful @c start() and @c stop().
   */
  [[nodiscard]] bool is_running() const noexcept;

  /**
   * @brief Binds the @b trigger plugin notified across the recorder's life cycle and dump pipeline.
   *
   * @details
   * This is the @b post-dump behaviour plugin, distinct from the bag reorder plugin bound by
   * @c bind_bag_plugin_interface().  Its hooks (see @c TriggerPluginInterface) fire as the recorder starts /
   * stops, on each trigger, and around each dump -- most importantly @c on_dump_finished() once a bag is
   * written, the place to upload or archive it.  It never rewrites frames.  Passing @c nullptr detaches the
   * current plugin.  Binding while a dump is in flight takes effect from the next dump.
   *
   * @param plugin Trigger plugin instance to bind, or @c nullptr to detach.
   */
  void bind_trigger_plugin_interface(const std::shared_ptr<TriggerPluginInterface>& plugin);

  /**
   * @brief Detaches the trigger plugin (equivalent to @c bind_trigger_plugin_interface(nullptr)).
   */
  void clear_trigger_plugin_interface();

  /**
   * @brief Binds the @b bag reorder plugin applied inside the write path of every dump.
   *
   * @details
   * This is the @b data-plane reorder plugin, distinct from the trigger plugin bound by
   * @c bind_trigger_plugin_interface().  The engine attaches it to the internal @c BagWriter of each dump via
   * @c BagWriter::bind_plugin_interface(); its @c on_write() hook parses the true data-plane time out of each
   * payload and re-emits frames reordered by that time before they are persisted.  A recorder configured with
   * @c Config::bag_plugin_lib loads such a plugin from a shared library automatically at @c start(); this method
   * is the programmatic equivalent for an already-constructed instance.  Passing @c nullptr detaches it, so
   * dumps fall back to capture-time order.  Binding while a dump is in flight takes effect from the next dump.
   *
   * @param plugin Bag reorder plugin instance to bind, or @c nullptr to detach.
   */
  void bind_bag_plugin_interface(const std::shared_ptr<BagPluginInterface>& plugin);

  /**
   * @brief Detaches the bag reorder plugin (equivalent to @c bind_bag_plugin_interface(nullptr)).
   */
  void clear_bag_plugin_interface();

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;

  VLINK_DISALLOW_COPY_AND_ASSIGN(TriggerRecorder)
};

}  // namespace vlink
