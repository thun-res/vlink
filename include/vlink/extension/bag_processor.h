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
 * @file bag_processor.h
 * @brief Data-plane-time sliding-window reorder buffer, a helper for @c BagPluginInterface plugins.
 *
 * @details
 * @c BagProcessor is the building block a @c BagPluginInterface plugin uses to reorder frames by
 * their @e true data-plane time before re-emitting them -- on the write side before a frame is
 * persisted, on the read side before it is replayed.  It is symmetric: the same engine serves both
 * directions because both push frames in and forward the reordered output through @c do_callback().
 *
 * Two distinct times are at play, which can disagree (out-of-order recording, async I/O, sender-side
 * batching, transport requeue):
 *
 * - @c Frame::timestamp -- the canonical record / playback time the frame carries downstream.
 * - the @b data-plane time -- the true event time carried inside the payload header, passed @e
 *   separately as the @c push() reorder key.  @c BagProcessor emits frames in ascending data-plane-time
 *   order while @c Frame::timestamp rides along unchanged.
 *
 * @c BagProcessor is a @e pure reorder buffer.  The plugin extracts the data-plane time from the
 * header (parsing or deserialising the payload as needed) and passes it to @c push(); @c BagProcessor
 * never inspects the payload nor touches the serialisation layer.  Any payload transform (compress /
 * decompress, URL / serialisation-type rewrite) is likewise the plugin's job, applied before @c push()
 * or inside the output callback before it forwards the frame.
 *
 * The oldest cached frame is released only once the data-plane-time span between the oldest and
 * newest cached frames reaches @c Config::min_cache_time, giving a late-but-earlier frame a chance to
 * slot ahead of already-cached later frames.  A wall-clock fallback drains the cache when a producer
 * goes silent, so the stream can always make progress.
 *
 * @par Write-side example (reorder by header time, then persist)
 * @code
 * class MyWritePlugin : public vlink::BagPluginInterface {
 *  public:
 *   MyWritePlugin() {
 *     processor_.register_output_callback([this](const vlink::Frame& f) { do_callback(f); });
 *   }
 *
 *   vlink::BagPluginInterface::VersionInfo get_version_info() const override {
 *     return {"my-write", "1.0.0", __DATE__, "", ""};
 *   }
 *
 *   void on_write(const vlink::Frame& frame) override {
 *     const int64_t data_timestamp = parse_header_time(frame.data);  // plugin extracts the data-plane time
 *     processor_.push(data_timestamp, frame);                        // buffered + reordered by data_timestamp
 *   }
 *
 *   void flush() override { processor_.flush(); }                    // drain the buffered tail at teardown
 *
 *  private:
 *   vlink::BagProcessor processor_;
 * };
 * @endcode
 */

#pragma once

#include <cstdint>
#include <memory>
#include <mutex>

#include "../impl/types.h"

namespace vlink {

/**
 * @class BagProcessor
 * @brief Time-sorted relay buffer keyed on the data-plane time, shared by read- and write-side plugins.
 *
 * @details
 * Thread-safe in the sense that @c push() may be called concurrently from the host's loop thread(s);
 * delivery to the @c OutputCallback happens on a dedicated worker thread owned by the processor.
 */
class VLINK_EXPORT BagProcessor {
 public:
  /**
   * @brief Sink receiving one @c Frame in data-plane-time order on the worker thread.
   *
   * @details
   * Invoked once @c Config::min_cache_time of data-plane time has accumulated ahead of the candidate
   * frame (or a wall-clock drain has fired).  The frame is moved out of the cache into the callback.
   */
  using OutputCallback = FrameCallback;

  /**
   * @struct Config
   * @brief Tunables controlling the reorder buffer behaviour.
   */
  struct Config final {
    int64_t min_cache_time{500};                    ///< Data-plane-time span (ms) to accumulate before flushing.
    int64_t max_cache_size{1024LL * 1024LL * 256};  ///< Maximum total payload bytes held (default 256 MiB).

    Config() {}  // NOLINT(modernize-use-equals-default)
  };

  /**
   * @brief Builds the processor and spawns its worker thread.
   *
   * @param config Cache time-window and memory-budget tunables.
   */
  explicit BagProcessor(const Config& config = Config());

  /**
   * @brief Drains remaining frames in data-plane-time order and joins the worker thread.
   */
  ~BagProcessor();

  /**
   * @brief Sets the single sink receiving reordered frames.
   *
   * @details
   * Replaces any previously registered callback; only the most recent registration remains effective.
   *
   * @param output_callback Sink invoked once per frame on the worker thread.
   */
  void register_output_callback(OutputCallback&& output_callback);

  /**
   * @brief Inserts a frame into the data-plane-time-sorted cache.
   *
   * @details
   * Safe to call from any thread.  Frames are ordered by @p data_timestamp -- the true data-plane time
   * the caller extracted from the payload header -- which is kept separate from @c Frame::timestamp (the
   * canonical record / playback time the frame carries downstream).  An entry leaves the cache only
   * after the @c Config::min_cache_time window is reached or after a wall-clock timeout when the
   * producer has gone quiet.  The frame is moved into the owning cache.
   *
   * @param data_timestamp Reorder key in microseconds (the data-plane time).
   * @param frame          Frame to cache (copied into the owning buffer).
   */
  void push(int64_t data_timestamp, const Frame& frame);

  /**
   * @brief Synchronously drains every currently-buffered frame to the sink, in data-plane-time order.
   *
   * @details
   * Blocks until the cache is empty: the worker thread emits each queued frame through the registered
   * output callback, then wakes the caller.  A plugin forwards this from @c BagPluginInterface::flush()
   * so the host can flush buffered tail frames @e before it unbinds and tears down, instead of losing
   * them.  Frames pushed after @c flush() begins are not guaranteed to be included; a no-op once the
   * processor is shutting down.
   */
  void flush();

 private:
  bool on_check();

  void on_output(std::unique_lock<std::mutex>& lock, bool at_end);

  void on_run();

  void on_exec(bool at_end);

  struct Impl;
  std::unique_ptr<Impl> impl_;

  VLINK_DISALLOW_COPY_AND_ASSIGN(BagProcessor)
};

}  // namespace vlink
