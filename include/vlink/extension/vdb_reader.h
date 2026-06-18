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
 * @file vdb_reader.h
 * @brief SQLite-backed playback for VLink @c .vdb / @c .vdbx bag files.
 *
 * @details
 * @c VDBReader is the concrete @c BagReader implementation for the SQLite container that VLink
 * uses for recordings that require true random-access indexing, in-place repair, and tag-based
 * navigation.  Splits are described by a @c .vdbx JSON manifest pointing at one or more sibling
 * @c .vdb files; this reader follows the manifest transparently.
 *
 * @par VDB schema
 *
 * | Table             | Key columns                            | Purpose                                  |
 * | ----------------- | -------------------------------------- | ---------------------------------------- |
 * | @c messages       | id, url_id, ts_us, action, data        | recorded payloads ordered by timestamp   |
 * | @c urls           | id, url, ser_type, schema_type         | per-topic identity dictionary            |
 * | @c schemas        | id, ser_type, schema_type, blob        | embedded schema descriptors              |
 * | @c metadata       | key, value                             | tag name, version, capture machine, etc. |
 * | @c stats          | url_id, count, bytes, loss             | per-URL aggregate counters               |
 * | @c index          | ts_us, message_id                      | timestamp -> message random-access index |
 *
 * @par Reader states
 * @code
 *      +---------+   play()         +---------+   pause()        +---------+
 *      | stopped | ---------------> | playing | ---------------> | paused  |
 *      +---------+                  +---------+                  +---------+
 *           ^                            |  ^                          |
 *           |                            |  | resume() / pause_to_next()|
 *           +--- stop() / EOF -----------+  +--------------------------+
 *                                        |
 *                                        |  jump(time, rate, times)
 *                                        v
 *                                    +---------+
 *                                    | seeking |  SQL index lookup
 *                                    +---------+
 * @endcode
 *
 * @par Example
 * @code
 *   auto reader = vlink::BagReader::create("/data/recording.vdb");
 *
 *   reader->register_output_callback([](const vlink::Frame& frame) {
 *     dispatch(frame.url, frame.data);
 *   });
 *
 *   reader->async_run();
 *   reader->play({});
 * @endcode
 *
 * @see BagReader, VCAPReader
 */

#pragma once

#include <future>
#include <memory>
#include <string>
#include <vector>

#include "./bag_reader.h"

namespace vlink {

/**
 * @class VDBReader
 * @brief Concrete SQLite-backed @c BagReader implementation with full random-access support.
 *
 * @details
 * Prefer @c BagReader::create() for format-agnostic instantiation; instantiate this class
 * directly only when a SQLite-specific feature is required.
 */
class VLINK_EXPORT VDBReader final : public BagReader {
 public:
  /**
   * @brief Opens a SQLite bag file for playback.
   *
   * @param path        Filesystem path of the @c .vdb or @c .vdbx file.
   * @param read_only   @c true opens the database without write capability (default).
   * @param try_to_fix  Reserved hook for table-rebuild recovery when table checks are enabled.
   */
  explicit VDBReader(const std::string& path, bool read_only = true, bool try_to_fix = false);

  /**
   * @brief Stops playback and closes the SQLite database handle.
   */
  ~VDBReader() override;

  /**
   * @brief Attaches a @c BagPluginInterface for custom URL or type rewriting.
   *
   * @param plugin_interface  Plugin instance; pass @c nullptr to detach.
   */
  void bind_plugin_interface(const std::shared_ptr<BagPluginInterface>& plugin_interface) override;

  /**
   * @brief Registers a callback invoked on every playback state transition.
   *
   * @param status_callback  Receives the new @c Status value.
   */
  void register_status_callback(StatusCallback&& status_callback) override;

  /**
   * @brief Registers a callback fired once the database is open and parsed.
   *
   * @param ready_callback  Invoked when the reader transitions out of opening state.
   */
  void register_ready_callback(ReadyCallback&& ready_callback) override;

  /**
   * @brief Registers a callback fired when playback ends naturally or is interrupted.
   *
   * @param finish_callback  Receives a flag indicating whether playback was interrupted.
   */
  void register_finish_callback(FinishCallback&& finish_callback) override;

  /**
   * @brief Registers the message-delivery callback consumed during playback.
   *
   * @param output_callback  Invoked for each replayed message.
   */
  void register_output_callback(OutputCallback&& output_callback) override;

  /**
   * @brief Starts playback with the given configuration.
   *
   * @param config  Playback configuration (start / end times, rate, filters, loop count).
   */
  void play(const Config& config) override;

  /**
   * @brief Stops playback and rewinds to the recording start.
   */
  void stop() override;

  /**
   * @brief Pauses playback at the current position.
   */
  void pause() override;

  /**
   * @brief Resumes paused playback from the current position.
   */
  void resume() override;

  /**
   * @brief Emits one more message and then pauses again.
   */
  void pause_to_next() override;

  /**
   * @brief Seeks to @p begin_time and resumes playback at the new position.
   *
   * @param begin_time     Target timestamp in milliseconds, relative to recording start.
   * @param rate           Playback rate multiplier applied after the seek.
   * @param times          Number of loop iterations after the jump.
   * @param force_to_play  @c true forces play state even if currently paused.
   */
  void jump(int64_t begin_time, double rate, int times, bool force_to_play = false) override;

  /**
   * @brief Verifies the integrity of the SQLite bag file asynchronously.
   *
   * @return Future resolving to @c true when the file passes the integrity check.
   */
  std::future<bool> check() override;

  /**
   * @brief Rebuilds the SQLite index tables asynchronously.
   *
   * @return Future resolving to @c true on success.
   */
  std::future<bool> reindex() override;

  /**
   * @brief Repairs a corrupt SQLite bag file asynchronously.
   *
   * @param rebuild  @c true rebuilds the entire index from scratch.
   * @return Future resolving to @c true on success.
   */
  std::future<bool> fix(bool rebuild = false) override;

  /**
   * @brief Updates the tag column in the @c metadata table.
   *
   * @param tag_name  New tag string.
   */
  void tag(const std::string& tag_name) override;

  /**
   * @brief Returns the current playback cursor in milliseconds.
   *
   * @return Position relative to the recording start.
   */
  [[nodiscard]] int64_t get_timestamp() const override;

  /**
   * @brief Returns the timestamp of the last delivered message.
   *
   * @return Last data timestamp in milliseconds, or @c 0 when stopped.
   */
  [[nodiscard]] int64_t get_real_timestamp() const override;

  /**
   * @brief Returns the playback status.
   *
   * @return One of @c kStopped, @c kPaused, or @c kPlaying.
   */
  [[nodiscard]] Status get_status() const override;

  /**
   * @brief Returns the bag metadata populated when the file was opened.
   *
   * @return Const reference to the @c Info struct.
   */
  [[nodiscard]] const Info& get_info() const override;

  /**
   * @brief Enumerates every schema embedded in the @c schemas table.
   *
   * @return Vector of @c SchemaData descriptors.
   */
  [[nodiscard]] std::vector<SchemaData> detect_schema() override;

  /**
   * @brief Reports whether the bag is split across multiple @c .vdb files.
   *
   * @return @c true when reading a @c .vdbx manifest with multiple parts.
   */
  [[nodiscard]] bool is_split_mode() const override;

  /**
   * @brief Returns the index of the split part currently under playback.
   *
   * @return Zero-based split index.
   */
  [[nodiscard]] int get_split_index() const override;

  /**
   * @brief Reports whether a @c jump() seek is currently being processed.
   *
   * @return @c true while a seek is in flight.
   */
  [[nodiscard]] bool is_jumping() const override;

 protected:
  size_t get_max_task_count() const override;

  void on_begin() override;

  void on_end() override;

  bool do_open_cursor(const Config& config) override;

  bool do_read_next(Frame& out, bool& is_error) override;

 private:
  bool prepare_cursor_stmt(int file_index);

  void update_status(Status status);

  void do_stop();

  void do_pause();

  bool prepare_file(void* file);

  void open(const std::string& path);

  void close();

  int get_reset_index(const Config& config);

  void read(const Config& config);

  struct Impl;
  std::unique_ptr<Impl> impl_;

  VLINK_DISALLOW_COPY_AND_ASSIGN(VDBReader)
};

}  // namespace vlink
