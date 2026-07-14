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
 * @file bag_writer.h
 * @brief Abstract VLink bag recorder with split, compression, schema embedding and a global hook.
 *
 * @details
 * @c BagWriter is the polymorphic base for VLink offline recording.  It exposes a
 * @c push() entry point, which either enqueues serialised messages onto a private
 * @c MessageLoop or persists them synchronously according to @c Config::sync_mode.
 * Two backends ship with VLink:
 *
 * - @c VDBWriter for SQLite-backed @c .vdb / @c .vdbx containers; default codec is LZAV
 *   for @c kCompressAuto and @c kCompressLzav selectors.
 * - @c VCAPWriter for MCAP-format @c .vcap / @c .vcapx containers; @c kCompressAuto and
 *   @c kCompressZstd select Zstandard when Zstd support is compiled in.
 *
 * Default asynchronous writer state machine:
 *
 * @verbatim
 *                async_run()                push()               close()/dtor
 *   +---------+ ----------> +-----------+ ---------> +---------+ ----------> +---------+
 *   |  Open   |             |  Running  | <--------- | Pending |             |  Closed |
 *   +---------+             +-----------+   ack      +---------+             +---------+
 *                                ^                                              ^
 *                                |                                              |
 *                                +--- split_by_size / split_by_time -- rotate --+
 * @endverbatim
 *
 * On-disk layout produced by the writers:
 *
 * @verbatim
 *   +---------+----------------+---------------+----------------+--------+
 *   | Header  |  URL index     |  Schema index | Message stream | Footer |
 *   +---------+----------------+---------------+----------------+--------+
 *      tag        url_metas       schema_data       payloads      finalisation
 *      app
 *      timezone
 * @endverbatim
 *
 * Feature highlights:
 * - Writer-wide asynchronous or synchronous record policy selected by @c Config::sync_mode.
 * - File splitting by byte size and/or by wall-clock interval.
 * - Optional WAL mode for SQLite crash resilience.
 * - URL-level loss reporting via @c set_url_loss().
 * - Schema embedding through @c push_schema() for offline introspection.
 * - Process-global writer triggered by the @c VLINK_BAG_PATH environment variable.
 *
 * @par Example
 * @code
 * vlink::BagWriter::Config cfg;
 * cfg.compress      = vlink::BagWriter::kCompressAuto;
 * cfg.split_by_size = 1024LL * 1024LL * 512;            // 512 MiB per split
 *
 * auto writer = vlink::BagWriter::create("/data/drive_log.vdb", cfg);
 * writer->async_run();
 *
 * vlink::Frame frame;
 * frame.timestamp   = -1;                       // < 0 => writer auto-assigns from its clock (0 is verbatim)
 * frame.url         = "dds://camera/front";
 * frame.ser_type    = "demo.proto.Image";
 * frame.schema_type = vlink::SchemaType::kProtobuf;
 * frame.action_type = vlink::ActionType::kPublish;
 * frame.data        = bytes;
 * writer->push(frame);
 * writer->wait_for_idle();
 * writer->quit();
 * writer->wait_for_quit();
 * writer->close();
 * if (writer->fail()) { handle_recording_error(); }
 * @endcode
 *
 * @par Global writer
 * @code
 * // Set VLINK_BAG_PATH=/data/global.vdb before process launch.
 * if (auto* gw = vlink::BagWriter::global_get(); gw != nullptr) {
 *   vlink::Frame frame;
 *   frame.timestamp   = -1;                       // < 0 => auto-assign (0 would be recorded verbatim)
 *   frame.url         = "intra://debug";
 *   frame.ser_type    = "raw";
 *   frame.schema_type = vlink::SchemaType::kRaw;
 *   frame.action_type = vlink::ActionType::kPublish;
 *   frame.data        = bytes;
 *   gw->push(frame);
 * }
 * @endcode
 *
 * @note @c push() is thread-safe.  @c Config::sync_mode selects synchronous writes for the
 * writer's entire lifetime; otherwise writes are queued on the recording loop.
 */

#pragma once

#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "../base/functional.h"
#include "../base/macros.h"
#include "../base/message_loop.h"
#include "../impl/types.h"

namespace vlink {

class BagPluginInterface;
class SchemaPluginInterface;

/**
 * @class BagWriter
 * @brief Asynchronous VLink message recorder built on top of @c MessageLoop.
 *
 * @details
 * Construct via @c create() (or directly) and call @c async_run() to start the recording
 * thread, then push messages with @c push().  Concrete subclasses implement every virtual
 * persistence operation; the base class owns the shared bookkeeping and the loop wiring.
 */
class VLINK_EXPORT BagWriter : public MessageLoop {
 public:
  /**
   * @brief Compression codec selector understood by the writer backends.
   *
   * | Value           | Algorithm | Notes                                                  |
   * | --------------- | --------- | ------------------------------------------------------ |
   * | kCompressNone   | none      | Payloads stored as raw bytes                           |
   * | kCompressAuto   | backend   | Uses the backend default (LZAV for VDB, Zstd for MCAP) |
   * | kCompressZstd   | Zstandard | Active for MCAP when Zstd support is available         |
   * | kCompressLz4    | LZ4       | Reserved selector; not currently used by built-ins     |
   * | kCompressLzav   | LZAV      | Active for SQLite-backed VDB recordings                |
   */
  enum CompressType : uint8_t {
    kCompressNone = 0,  ///< Store payloads uncompressed.
    kCompressAuto = 1,  ///< Defer codec choice to the active backend.
    kCompressZstd = 2,  ///< Force Zstandard codec where supported.
    kCompressLz4 = 3,   ///< Reserved selector; no built-in writer emits LZ4 today.
    kCompressLzav = 4,  ///< Force LZAV codec where supported.
  };

  /**
   * @struct Config
   * @brief Recording behaviour, split policy and resource budgets.
   *
   * @details
   * Sizes are expressed in bytes and durations in milliseconds unless explicitly stated.
   */
  struct Config final {
    std::string tag_name;                                ///< Optional tag stored in the bag header.
    CompressType compress{CompressType::kCompressNone};  ///< Compression codec selector.
    bool wal_mode{false};                                ///< Enable SQLite WAL for crash resilience.
    bool enable_limit{false};                            ///< When true, evict oldest rows at the row/byte limit.
    bool split_name_by_time{false};                      ///< Append a timestamp suffix to split filenames.
    bool sync_mode{false};                   ///< Write synchronously and disable the VDB periodic cache-flush timer.
    bool optimize_on_exit{false};            ///< Run VACUUM/OPTIMIZE while closing the file.
    int64_t max_row_count{5'000'000'000LL};  ///< SQLite row cap; either evicts or fails new writes.
    int64_t max_bytes_size{1024LL * 1024LL * 1024LL * 512LL};  ///< SQLite byte cap; either evicts or fails new writes.
    int64_t split_by_size{1024LL * 1024LL * 1024LL * 1LL};     ///< Split threshold in bytes (0 disables).
    int64_t split_by_time{0};                                  ///< Split interval in milliseconds (0 disables).
    int64_t begin_time{0};                                     ///< Anchor (ms) used by time-based splits.
    int64_t cache_size{1024LL * 1024LL * 4};                   ///< VDB commit chunk / MCAP chunk size in bytes.
    int64_t compress_start_size{128};                          ///< Minimum payload size eligible for compression.
    int64_t compress_level{3};                                 ///< Codec-specific compression level.
    int64_t max_task_depth{20000};                             ///< Maximum pending writes in the loop queue.
    int64_t max_memory_size{1024LL * 1024LL * 1024LL * 2LL};   ///< Maximum in-memory cache size in bytes.
    int64_t start_timestamp{0};                                ///< Override for the wall-clock start timestamp (ms).
    std::unordered_set<std::string> ignore_compress_urls;      ///< URLs whose payloads must never be compressed.

    Config() {}  // NOLINT(modernize-use-equals-default)
  };

  /**
   * @brief Notification fired when the writer rotates to a new split file.
   *
   * @details
   * Called with the zero-based split index and the new file path.  The @c before flag of
   * @c register_split_callback() chooses whether the hook runs before or after the
   * rotation is committed.
   */
  using SplitCallback = MoveFunction<void(int split_index, const std::string& split_filename)>;

  /**
   * @brief Schema resolver used by the writer when a previously unseen URL is recorded.
   *
   * @details
   * The writer passes the requested serialisation type together with a coarse schema
   * family hint so that families sharing a single type name (e.g. Protobuf vs Arrow) can
   * still be disambiguated.
   */
  using SchemaCallback = MoveFunction<SchemaData(const std::string& ser_type, SchemaType schema_type)>;

  /**
   * @brief System clock alias used when formatting timestamps into split file names.
   */
  using SystemClock = std::chrono::time_point<std::chrono::system_clock, std::chrono::milliseconds>;

  /**
   * @brief Builds the concrete writer matching the extension of @p path.
   *
   * @details
   * Suffix dispatch: @c .vdb / @c .vdbx select @c VDBWriter, @c .vcap / @c .vcapx select
   * @c VCAPWriter; other suffixes return @c nullptr.  The returned writer is open immediately;
   * asynchronous writers need @c async_run(), while synchronous writers do not.
   *
   * @param path   Output file path.
   * @param config Recording configuration.
   * @return Shared pointer to the new writer, or @c nullptr on unsupported suffix.
   */
  [[nodiscard]] static std::shared_ptr<BagWriter> create(const std::string& path, const Config& config = {});

  /**
   * @brief Returns the cached writer for @p path, lazily creating and starting one.
   *
   * @details
   * Looks up the process-wide writer registry.  When no entry exists, a writer is built
   * by @c create(), its loop is started with @c async_run(), and it is registered for
   * reuse.  The registry releases the entry automatically when the last shared owner
   * goes away.  Unsupported suffixes return @c nullptr and are not registered.
   *
   * @param path Output file path.
   * @return Shared pointer to a started writer, or @c nullptr on unsupported suffix.
   */
  [[nodiscard]] static std::shared_ptr<BagWriter> filter_get(const std::string& path);

  /**
   * @brief Returns the singleton writer driven by the @c VLINK_BAG_PATH environment variable.
   *
   * @details
   * On first call, the writer is created from @c VLINK_BAG_PATH and started.  Returns
   * @c nullptr when the environment variable is absent or carries an unsupported suffix.
   *
   * @return Raw pointer to the global writer, or @c nullptr.
   */
  static BagWriter* global_get();

  /**
   * @brief Constructs the base writer and opens the output file.
   *
   * @details
   * The recording loop is not yet running; call @c async_run() when @c Config::sync_mode is false.
   * A synchronous writer performs frame, schema and plugin-output writes on the calling thread and
   * does not require a recording-loop thread.
   *
   * @param path   Output file path.
   * @param config Recording configuration.
   */
  explicit BagWriter(const std::string& path, const Config& config = {});

  /**
   * @brief Halts the loop, flushes pending writes and closes the file.
   */
  virtual ~BagWriter();  // NOLINT(modernize-use-override)

  /**
   * @brief Attaches a custom frame-forwarding plugin to this writer.
   *
   * @details
   * The plugin's @c on_write() hook runs for every frame before it is persisted; it re-emits each
   * frame through @c do_callback(), and may transcode, drop, fan out, or buffer and reorder frames by
   * their true data-plane time (a sliding-window reorder) before they reach the bag.  The writer
   * supplies the record sink via @c BagPluginInterface::register_callback() and binds the plugin with
   * @c BagPluginInterface::Direction::kWrite.  Passing @c nullptr detaches and clears the previous
   * plugin's sink.
   *
   * @param bag_interface Plugin interface instance, or @c nullptr to detach the current binding.
   *
   * @see clear_bag_interface() for the named equivalent of passing @c nullptr.
   */
  virtual void bind_bag_interface(const std::shared_ptr<BagPluginInterface>& bag_interface);

  /**
   * @brief Detaches the currently bound plugin, if any.
   *
   * @details
   * Convenience wrapper equivalent to @c bind_bag_interface(nullptr): flushes the bound plugin's
   * pending frames, clears its record sink and drops the binding.  Safe to call when no plugin is
   * bound (in which case it is a no-op).
   */
  virtual void clear_bag_interface();

  /**
   * @brief Installs a hook fired around split rotation.
   *
   * @param callback Receives the new split index and the new file path.
   * @param before   When true, the hook fires before the new file is opened; otherwise after.
   */
  virtual void register_split_callback(SplitCallback&& callback, bool before) = 0;

  /**
   * @brief Installs the resolver invoked when an unseen serialisation type is recorded.
   *
   * @param callback Function mapping (ser_type, schema_type) to @c SchemaData.
   */
  virtual void register_schema_callback(SchemaCallback&& callback) = 0;

  /**
   * @brief Embeds a schema descriptor into the bag for downstream introspection.
   *
   * The operation follows @c Config::sync_mode: synchronous writers merge on the caller's thread;
   * asynchronous writers enqueue the merge on the recording loop.
   *
   * @param schema_data Schema descriptor to persist.
   * @return @c true on success; @c false when a synchronous merge fails or an asynchronous merge
   *         task cannot be enqueued.
   */
  virtual bool push_schema(const SchemaData& schema_data) = 0;

  /**
   * @brief Records a single frame to the bag.
   *
   * @details
   * The write follows the mode fixed at construction: @c Config::sync_mode writes on the caller's
   * thread; otherwise a task is enqueued on the recording loop.
   * Once accepted, an asynchronous frame is not evicted to admit a later frame.
   * When @c frame.timestamp is negative the writer assigns a recording-relative timestamp from its
   * elapsed clock; a non-negative @c frame.timestamp (including @c 0) is recorded verbatim.
   *
   * When a plugin is bound via @c bind_bag_interface(), the frame is handed to the plugin's
   * @c on_write() hook, which re-emits it (possibly transcoded, dropped, fanned out or reordered)
   * through the writer's record sink into the concrete @c record() implementation.  Because the
   * plugin may emit asynchronously, the return value is then the assigned timestamp rather than a
   * per-frame record result; a frame the plugin drops simply never reaches @c record().
   *
   * @param frame Frame to record.  @c url must not be empty; @c timestamp < 0 requests auto-assign.
   * @return Assigned timestamp in microseconds, or a negative value on validation/write failure or when an
   *         asynchronous write cannot be queued (for example, because a task or memory limit was reached).
   */
  int64_t push(const Frame& frame);

  /**
   * @brief Streaming shorthand for @c push(frame).
   *
   * @details
   * Records @p frame according to @c Config::sync_mode and returns the writer so calls can be chained,
   * e.g. @c *writer << frame_a << frame_b.  The per-frame timestamp that @c push() returns is not surfaced; instead,
   * a negative @c push() result (e.g. an empty URL, a queue or memory-limit rejection, or a synchronous
   * record failure forwarded by a bound plugin) latches the @c fail() state so failures are observable
   * without inspecting every return value.
   *
   * @param frame Frame to record; @c url must not be empty, @c timestamp < 0 requests auto-assign.
   * @return Reference to @c *this for chaining.
   */
  BagWriter& operator<<(const Frame& frame);

  /**
   * @brief Streaming shorthand for @c push_schema(schema_data).
   *
   * @details
   * Embeds @p schema_data according to @c Config::sync_mode and returns the writer for chaining,
   * e.g. @c *writer << schema << frame.  A @c false result -- the
   * merge task could not be enqueued, or a bound backend rejected it -- latches the @c fail() state.
   *
   * @param schema_data Schema descriptor to persist.
   * @return Reference to @c *this for chaining.
   */
  BagWriter& operator<<(const SchemaData& schema_data);

  /**
   * @brief Returns whether a stream operation, deferred backend write or finalisation has failed.
   *
   * @details
   * Latches when a stream insertion is rejected, a concrete backend cannot persist an accepted asynchronous
   * frame or schema, or @c close() cannot finalise the bag.  Callers may wait for the queue to become idle and
   * then query this method; call @c close() first when close-time metadata, footer or manifest failures must
   * also be observed.  Synchronous callers continue to use the return value from @c push() or @c push_schema().
   * Cleared by @c clear().
   */
  [[nodiscard]] bool fail() const noexcept;

  /**
   * @brief Reports whether the streaming write state is still good (no latched failure).
   *
   * @details
   * Returns @c true while no write failure has been latched, so @c if (*writer << frame) tests the
   * post-write state.
   */
  explicit operator bool() const noexcept;

  /**
   * @brief Clears a latched @c fail() state so streaming writes can resume being observed.
   */
  void clear() noexcept;

  /**
   * @brief Returns the backend-specific "dump in progress" flag.
   */
  [[nodiscard]] virtual bool is_dumping() const = 0;

  /**
   * @brief Returns whether split mode is currently in effect.
   *
   * @return @c true when the bag uses a splittable multi-file container
   *         (e.g. a @c .vdbx / @c .vcapx suffix), in which case
   *         @c split_by_size / @c split_by_time control the rotation timing;
   *         @c false otherwise, regardless of the @c split_by_* values.
   */
  [[nodiscard]] virtual bool is_split_mode() const = 0;

  /**
   * @brief Returns the zero-based index of the active split file.
   *
   * @return Active split index, or 0 outside split mode.
   */
  [[nodiscard]] virtual int get_split_index() const = 0;

  /**
   * @brief Records the expected loss ratio for @p url as bag metadata.
   *
   * @details
   * Loss values feed offline diagnostics so that intentional drops can be distinguished
   * from unexpected loss.
   *
   * @param url  Topic URL.
   * @param loss Loss ratio; values greater than 1.0 are normalised to -1.
   */
  virtual void set_url_loss(const std::string& url, double loss);

 protected:
  virtual int64_t record(const Frame& frame, int64_t timestamp) = 0;

  virtual int64_t get_record_timestamp() const = 0;

 public:
  /**
   * @brief Finalizes the backend file (final commit, metadata, footer) and latches any failure.
   *
   * @details
   * Idempotent; invoked automatically at destruction.  Callers that must verify the close-time
   * writes call it explicitly and then query @c fail() while the writer is still alive.  It is not
   * synchronised against the recording loop.  After producers have stopped, detach any bound write plugin with
   * @c clear_bag_interface() so its buffered tail is emitted while the loop still accepts tasks; then call
   * @c wait_for_idle(), @c quit() and @c wait_for_quit() before calling @c close() from another thread.
   *
   * @note Declared after the original virtual interface to preserve its vtable slot ordering.
   */
  virtual void close();

  /**
   * @brief Formats a wall-clock timestamp with millisecond precision.
   *
   * @param current     Time point to format; @c nullptr formats the current system time.
   * @param file_format When true, produces the file-name-safe form @c YYYY-MM-DD_hh-mm-ss-mmm shared by
   *                    generated bag names; otherwise the log form @c YYYY/MM/DD hh:mm:ss:mmm.
   * @return Formatted timestamp string.
   */
  static std::string get_format_date(SystemClock* current = nullptr, bool file_format = false);

 protected:
  std::string convert_recorded_url(const std::string& url) const;

  std::vector<std::string> recorded_urls_for_origin(const std::string& url) const;

  std::string recover_recorded_url(const std::string& url) const;

  void get_url_meta(const std::string& url, const std::string& ser, int& url_index, int& ser_index) const;

  void get_url_meta(int url_index, int ser_index, std::string& url, std::string& ser) const;

  std::mutex& sample_mutex();

  std::unordered_map<std::string, double>& url_loss_map_ref();

  std::unordered_map<std::string, double>& total_url_loss_map_ref();

  static const std::string& get_default_tag_name();

  static const std::string& get_default_app_name();

  static SchemaPluginInterface* get_schema_interface();

  static int32_t get_default_timezone_diff();

  static std::string_view convert_action(ActionType type);

  void flush_plugin();

  void detach_plugin();

  bool post_persistent_task(Callback&& callback);

  /**
   * @brief Latches the writer failure state from a concrete backend.
   */
  void set_fail() noexcept;

 private:
  void learn_recorded_url(const std::string& origin_url, const std::string& recorded_url);

  struct Impl;
  std::unique_ptr<Impl> impl_;

  VLINK_DISALLOW_COPY_AND_ASSIGN(BagWriter)
};

}  // namespace vlink
