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
 * @brief Event-data-recorder engine: a rolling in-memory ring of every live topic, dumped to a bag on demand.
 *
 * @details
 * @c TriggerRecorder is a self-contained data-plane engine (no RPC, no config parsing -- those belong to the
 * caller).  It discovers every topic on the bus, subscribes to the raw @c Bytes of each through a
 * caller-supplied @c RawSubFactory, and keeps a rolling per-URL ring holding the most recent pre-trigger
 * window.  @c dump() persists the @b pre + @b post window around the trigger instant to a bag file, rotating
 * old files.  This is the dashcam / EDR pattern: the ring is always recording, the trigger decides what to
 * persist.
 *
 * @verbatim
 *   discovery --new URL--> RawSubFactory (caller TU) --> RawSub --Bytes--> [per-URL rolling rings]
 *                                                                                   |
 *                                     dump(): serialized, runs on the recorder loop |
 *                                                                                   v
 *   trigger plugin on_dump_finished() <-- dump_dir bag (.vdb|.vcap) <-- BagWriter <-- [bag plugin? reorder]
 * @endverbatim
 *
 * @par Life cycle
 * The recorder is a @c MessageLoop: the constructor validates the @c Config and acquires every fallible
 * resource (creates @c Config::dump_dir and constructs the discovery viewer), throwing on failure.
 * @c async_run() starts discovery + buffering; wait for @c on_begin() to complete (e.g.
 * @c invoke_task([](){}).wait()) before calling @c dump().  @c quit() stops the loop and abandons a dump still
 * waiting for its post window; wait for @c is_dumping() to become false first when a dump must be preserved.
 *
 * @par Two distinct plugin roles (never conflate them)
 * - A @b bag plugin (@c BagPluginInterface, supplied via @c bind_bag_plugin_interface()) sits @e inside the
 *   write path: its @c on_write() re-emits frames reordered by the true @b data-plane time parsed from each
 *   payload.  Without it frames are written in @b capture-time (arrival) order.
 * - A @b trigger plugin (@c TriggerPluginInterface, via @c bind_trigger_plugin_interface()) observes the
 *   recorder @e life cycle -- @c on_dump_finished() is the upload / archive hook.  It never rewrites frames.
 *
 * @par Per-URL windows
 * Each URL may override the global default @b pre / @b post window (milliseconds before / after the trigger),
 * plus a max packet size, a per-URL byte cap and @c only_front / @c only_back restrictions.  A global URL
 * whitelist / blacklist selects which topics participate.
 *
 * @par Retention model (constant retention)
 * Every enabled URL retains @c pre_u + @c max_post_all + @c 2*retention_guard of history (@c max_post_all =
 * the largest @b post across all enabled URLs), so the ingest hot path needs no "is a trigger active" branch:
 *
 * @verbatim
 *                     pre_u              post_u
 *             |<---------------->|<---------------->|         dump(T) accepted at T, written at
 *   ring   [==:==================T==================:==]      T + max_post_all + retention_guard
 *          T-pre_u-guard                     T+post_u+guard    (immediately when no URL has a post
 *             target [T-pre_u, T+post_u], filtered at write     window); the guard cushions both
 *             time from the guard-padded ring coverage          window boundaries
 * @endverbatim
 *
 * @note The subscriber callback (data into the ring) is the hot path: it runs on the transport dispatch
 *       thread(s), takes only a per-URL lock, copies the payload once with @c Bytes::deep_copy and is amortized
 *       O(1); one callback may evict multiple expired or over-limit entries.
 * @warning A single URL with a large @b post raises the retention -- and memory -- of @e every URL.
 * @warning A reordering bag plugin buffers part of the window until @c flush(): peak dump memory can approach
 *          twice the window size.
 * @warning @c busy_skip_data drops data while the bag writer is active, leaving time holes for later triggers.
 * @warning With @c destroy_on_offline, offline buffers kept for an in-flight dump can push peak memory above
 *          @c max_cache_size.
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
 * vlink::TriggerRecorder recorder(config, [](const std::string& url, vlink::InitType type) {
 *   return vlink::TriggerRecorder::RawSub::create_shared(url, type);
 * });
 * recorder.async_run();
 * recorder.invoke_task([]() {}).wait();  // wait for on_begin() so dump() is accepted
 * // ... later, on an external event ...
 * vlink::TriggerRecorder::TriggerParams params;
 * params.reason = "hard-brake";
 * recorder.dump(params);
 * @endcode
 */

#pragma once

#include <cstdint>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "../base/message_loop.h"
#include "../subscriber.h"
#include "./discovery_viewer.h"

namespace vlink {

class BagPluginInterface;
class TriggerPluginInterface;

/**
 * @class TriggerRecorder
 * @brief @c MessageLoop-based rolling in-memory recorder that dumps a pre/post window to a bag on trigger.
 *
 * @details
 * Construct with a @c Config and @c RawSubFactory, call @c async_run(), then wait for @c on_begin() to complete
 * (for example with @c invoke_task([](){}).wait()) before using @c dump().  Dumps are serialised and execute on
 * the recorder loop.  Wait for @c is_dumping() to become false before shutdown when an accepted dump must be
 * preserved; @c quit() abandons a dump that is still waiting for its post-trigger window.
 */
class VLINK_EXPORT TriggerRecorder : public MessageLoop {
 public:
  /**
   * @brief Raw byte subscriber owned by the recorder for one discovered URL.
   */
  using RawSub = Subscriber<Bytes>;

  /**
   * @brief Caller-side constructor for raw subscribers.
   *
   * @details
   * The factory must return a fresh subscriber for @p url using the supplied @p type.  It must not initialize,
   * configure or start listening on the subscriber; the recorder applies getter semantics, loss tracking,
   * schema metadata, discovery and transport properties before it calls @c init() and @c listen().  The callable
   * runs synchronously on the discovery-viewer thread and therefore must be short, non-blocking and must not
   * re-enter this recorder.
   *
   * Keeping construction in the caller's translation unit is significant: the transport modules linked by the
   * caller propagate their @c VLINK_SUPPORT_* definitions there, allowing the header-only URL dispatcher to select
   * those linked backends.
   */
  using RawSubFactory = Function<std::shared_ptr<RawSub>(const std::string& url, InitType type)>;

  /**
   * @brief Maximum accepted pre / post / retention-guard window length in milliseconds.
   *
   * @details
   * Chosen so that the largest retention sum, @c pre + @c max_post_all + @c 2*retention_guard (four terms,
   * each at most this bound), still converts to microseconds without overflowing @c int64_t.  @c Config
   * values and per-trigger @c TriggerParams windows beyond this bound are rejected; control-plane frontends
   * (e.g. @c vlink-trigger) validate user input against the same constant.
   */
  static constexpr int64_t kMaxWindowMs = std::numeric_limits<int64_t>::max() / 4000;

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
   * @brief Recorder-wide configuration; passed once to the constructor and read-only afterwards.
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
    bool busy_skip_data{false};                 ///< Drop incoming data while a bag is being written.
    bool destroy_on_offline{false};             ///< Destroy offline subscribers; an in-flight dump keeps their data.
    OverflowPolicy overflow{kCoverOldest};      ///< Byte-cap overflow policy.
    int64_t sleep_interval{4LL * 1024 * 1024};  ///< Dump I/O throttle: sleep after this many bytes written.
    int64_t sleep_time_ms{0};                   ///< Dump I/O throttle: sleep duration in ms (0 disables).
    std::string dds_ip;                         ///< When non-empty, bind subscribers to this DDS IP (native mode).
    DiscoveryViewer::FilterType discovery_filter{DiscoveryViewer::kFilterAvailable};  ///< Discovery filter.
    std::vector<std::string> whitelist;                        ///< If non-empty, only these exact URLs are recorded.
    std::vector<std::string> blacklist;                        ///< These exact URLs are never recorded.
    std::unordered_map<std::string, UrlConfig> url_overrides;  ///< Per-URL window / limit overrides.

    Config() {}  // NOLINT(modernize-use-equals-default)
  };

  /**
   * @struct TriggerParams
   * @brief Per-trigger parameters; a pure data struct with no RPC or protobuf dependency.
   */
  struct TriggerParams final {
    std::string reason;     ///< Human-readable trigger reason; stored as the bag tag.
    std::string name_hint;  ///< Optional base name; a numeric suffix is added rather than replacing an existing bag.
    std::string out_file;   ///< Explicit output path; when empty a name under @c dump_dir is generated.
    int64_t pre_ms{-1};     ///< Per-trigger pre window; <0 uses each URL's configured pre (may only shrink it).
    int64_t post_ms{-1};    ///< Per-trigger post window; <0 uses each URL's configured post (may only shrink it).
    std::unordered_set<std::string> filter_urls;  ///< If non-empty, only these exact URLs are dumped this trigger.
    std::string filter_str;  ///< With empty @c filter_urls, comma/space-separated substrings select by @c black_mode.
    bool black_mode{false};  ///< With @c filter_str: false keeps matches (whitelist), true drops matches (blacklist).

    TriggerParams() {}  // NOLINT(modernize-use-equals-default)
  };

  /**
   * @brief Builds the recorder and acquires every fallible resource; the loop is not running yet.
   *
   * @details
   * Validates the configuration and factory, creates @c Config::dump_dir and constructs the discovery viewer.
   * Buffering begins only after @c async_run().
   *
   * @param config  Recorder-wide configuration, copied and validated internally.
   * @param factory Factory that constructs a fresh, uninitialized subscriber for each discovered URL.
   * @throw Exception::RuntimeError When the configuration or factory is invalid, @c dump_dir cannot be created,
   *        or discovery setup fails.
   */
  TriggerRecorder(const Config& config, RawSubFactory&& factory);

  /**
   * @brief Requests quit and joins the recorder loop thread.
   */
  ~TriggerRecorder() override;

  /**
   * @brief Requests a dump of the pre/post window around the current instant.
   *
   * @details
   * Non-blocking: it timestamps the trigger, rejects the call if a dump is already in flight, and enqueues the
   * actual capture / reorder / write onto the recorder loop (delayed by @c max_post_all + @c retention_guard_ms
   * when any URL has a post window).  The dump completes asynchronously.  The set of participating URLs is
   * frozen when the call is accepted: topics discovered afterwards do not contribute to this dump, and a topic
   * going offline (@c Config::destroy_on_offline) still contributes its already-buffered window.  Calling @c quit()
   * does not drain a dump that is still waiting for its post-trigger window.
   *
   * @param params Optional per-trigger overrides (reason, file name, shrunk windows).
   * @return @c true when the dump was accepted and enqueued; @c false for an invalid window, before @c on_begin()
   *         completes, when stopped or already dumping, or when the dump task cannot be enqueued.
   */
  bool dump(const TriggerParams& params = {});

  /**
   * @brief Reports whether a dump is currently in flight.
   *
   * @return @c true while a trigger's capture / write is running.
   */
  [[nodiscard]] bool is_dumping() const noexcept;

  /**
   * @brief Binds the @b trigger plugin notified across the recorder's life cycle and dump pipeline.
   *
   * @details
   * This is the @b post-dump behaviour plugin, distinct from the bag reorder plugin bound by
   * @c bind_bag_plugin_interface().  Its hooks (see @c TriggerPluginInterface) fire as the recorder starts /
   * stops, on each trigger, and around each dump -- most importantly @c on_dump_finished() once a bag is
   * written, the place to upload or archive it.  It never rewrites frames.  Passing @c nullptr detaches the
   * current plugin.  Bind before @c async_run() or after the recorder has stopped; binding while it is running
   * is rejected so one recorder run always has one stable lifecycle observer.
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
   * @c bind_trigger_plugin_interface().  The recorder attaches it to the internal @c BagWriter of each dump via
   * @c BagWriter::bind_plugin_interface(); its @c on_write() hook parses the true data-plane time out of each
   * payload and re-emits frames reordered by that time before they are persisted.  The host owns plugin loading
   * and lifetime, then supplies the resulting interface here.  Passing @c nullptr detaches it, so dumps fall back
   * to capture-time order.  Bind before @c async_run() or after the recorder has stopped.
   *
   * @param plugin Bag reorder plugin instance to bind, or @c nullptr to detach.
   */
  void bind_bag_plugin_interface(const std::shared_ptr<BagPluginInterface>& plugin);

  /**
   * @brief Detaches the bag reorder plugin (equivalent to @c bind_bag_plugin_interface(nullptr)).
   */
  void clear_bag_plugin_interface();

 protected:
  void on_begin() override;

  void on_end() override;

 private:
  struct UrlBuffer;
  struct DumpJob;
  struct Impl;

  void handle_data(UrlBuffer& url_buffer, const Bytes& data);

  std::shared_ptr<UrlBuffer> build_url_buffer(const DiscoveryViewer::Info& info);

  std::shared_ptr<RawSub> deactivate_url_buffer(UrlBuffer& url_buffer);

  void recompute_retention();

  void handle_discovery(const std::vector<DiscoveryViewer::Info>& list);

  void sweep_evict();

  void finish_dump(DumpJob& job);

  void finish_dump_locked(DumpJob& job);

  void notify_dump_failed(const DumpJob& job, std::string_view error);

  void do_dump(DumpJob& job);

  std::unique_ptr<Impl> impl_;

  VLINK_DISALLOW_COPY_AND_ASSIGN(TriggerRecorder)
};

}  // namespace vlink
