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
 * @file bag_plugin_interface.h
 * @brief Unified plugin contract for rewriting bag traffic on both playback (read) and
 *        recording (write).
 *
 * @details
 * @c BagPluginInterface is a dynamic plugin loaded through the VLink @c Plugin framework and
 * attached either to a @c BagReader (via @c BagReader::bind_bag_interface()) or to a
 * @c BagWriter (via @c BagWriter::bind_bag_interface()).  A single class serves both
 * directions; an implementation discovers which side it is bound to through
 * @c get_direction() and overrides only the hooks for that side.
 *
 * The two directions are @b symmetric: each is a frame-forwarding pipeline.  The host supplies a
 * downstream sink at bind time and the plugin re-emits every @c Frame -- optionally transformed,
 * dropped, fanned out, or @e reordered -- through that sink.  Plugins never touch the raw sink
 * member directly; they emit by calling the single @c do_callback() helper.  Read and write share
 * one callable type (@c Callback, a @c const @c Frame& sink); a plugin overrides the @c on_read() or
 * @c on_write() hook for its bound direction.
 *
 * - @b Read (playback).  Frames originate inside the reader and flow @e out to the user.
 *   @c on_read() receives each frame and re-emits it via @c do_callback().  URL/type remapping is a
 *   separate, once-per-URL hook, @c convert_url_meta(), applied when the bag is opened.  The effective
 *   @c Frame::ser_type / @c schema_type metadata is populated before @c on_read() runs.
 *
 * - @b Write (recording).  Frames originate from the caller and flow @e into the bag.  @c on_write()
 *   receives each frame -- fully populated with @c ser_type / @c schema_type because recording
 *   persists them -- and re-emits it via @c do_callback().
 *
 * Both hooks may forward unchanged, transcode (e.g. record a raw image as compressed JPEG by emitting
 * new @c Frame::data plus a new @c ser_type / @c schema_type), drop a frame (by not emitting), fan a
 * frame out into several, or buffer frames and emit them @e reordered by their true data-plane time --
 * a sliding-window reorder, identical on both sides, typically built on a @c BagProcessor.
 *
 * Plugin contract:
 *
 * | Hook                | Dir   | Purpose                                                       |
 * | ------------------- | ----- | ------------------------------------------------------------- |
 * | bind_direction()    | both  | At bind time: stored, observable via get_direction()          |
 * | register_callback() | both  | At bind time: store the forwarding sink                       |
 * | convert_url_meta()  | read  | Once per URL at open: true = keep, false = drop               |
 * | on_reset()          | read  | Before a playback session: discard retained session state     |
 * | on_read()           | read  | Every replayed frame: re-emit via do_callback()               |
 * | on_write()          | write | Every frame before persist: re-emit via do_callback()         |
 * | flush()             | both  | At a completed boundary or detach: drain buffered tail        |
 * | do_callback()       | both  | Forward one frame to the sink (drop = not call)               |
 *
 * @c on_read() and @c on_write() are both pure: an implementation defines both even when it serves a single
 * direction, leaving the unused one as a trivial pass-through (@c do_callback(frame)) or an empty body.
 *
 * Lifecycle:
 *
 * @verbatim
 *   read  : load .so -> bind_direction(kRead) -> register_callback -> convert_url_meta (per URL)
 *                    -> on_reset -> [on_read -> do_callback -> callback_ -> user] (per frame) -> flush
 *   write : load .so -> bind_direction(kWrite) -> register_callback
 *                    -> on_write (per frame) -> do_callback -> callback_ -> writer persists
 * @endverbatim
 *
 * A read pass calls @c flush() only after natural completion.  If the reader observes @c stop() or
 * @c jump() before the boundary drain begins, it skips @c flush(); the next top-level session calls
 * @c on_reset() to discard that retained tail.
 *
 * @par Read-side example (rename a topic on replay)
 * @code
 * class MyReadPlugin : public vlink::BagPluginInterface {
 *  public:
 *   bool convert_url_meta(std::string& url, std::string& ser_type,
 *                         vlink::SchemaType& schema_type) override {
 *     if (url.rfind("dds://legacy/", 0) == 0) { url.replace(0, 13, "dds://v2/"); }
 *     (void)ser_type;
 *     (void)schema_type;
 *     return true;
 *   }
 *
 *   void on_read(const vlink::Frame& frame) override {
 *     do_callback(frame);  // forward downstream (drop by not calling)
 *   }
 *
 *   void on_write(const vlink::Frame& frame) override { do_callback(frame); }  // unused on the read side
 * };
 * VLINK_PLUGIN_DECLARE(MyReadPlugin, 2, 0)
 * @endcode
 *
 * @par Write-side example (sliding-window reorder by true data-plane time before persist)
 * @code
 * class MyWritePlugin : public vlink::BagPluginInterface {
 *  public:
 *   MyWritePlugin() {
 *     processor_.register_output_callback([this](const vlink::Frame& frame) { do_callback(frame); });
 *   }
 *
 *   void on_read(const vlink::Frame& frame) override { do_callback(frame); }  // unused on the write side
 *
 *   void on_write(const vlink::Frame& frame) override {
 *     const int64_t data_timestamp = parse_header_time(frame.data);  // plugin extracts the data-plane time
 *     processor_.push(data_timestamp, frame);                        // reorder by data_timestamp; emit via
 * do_callback()
 *   }
 *
 *   void flush() override { processor_.flush(); }                    // drain the buffered tail at teardown
 *
 *  private:
 *   vlink::BagProcessor processor_;
 * };
 * VLINK_PLUGIN_DECLARE(MyWritePlugin, 2, 0)
 * @endcode
 */

#pragma once

#include <cstdint>
#include <string>
#include <utility>

#include "../base/plugin.h"
#include "../impl/types.h"

namespace vlink {

/**
 * @class BagPluginInterface
 * @brief Abstract plugin base shared by bag playback and bag recording.
 *
 * @details
 * The host binds an instance through @c BagReader::bind_bag_interface() or
 * @c BagWriter::bind_bag_interface().  At bind time the host calls @c bind_direction() to
 * record which side the plugin serves and @c register_callback() to supply the forwarding sink.  The frame
 * hooks @c on_read() and @c on_write() are pure and must both be defined; @c convert_url_meta(), @c on_reset(),
 * and @c flush() carry defaults.  Implementations are expected to be thread-compatible with the host's loop thread.
 */
class BagPluginInterface {
  VLINK_PLUGIN_REGISTER(BagPluginInterface)

 protected:
  BagPluginInterface() = default;

  virtual ~BagPluginInterface() = default;

 public:
  /**
   * @brief Identifies whether the plugin is bound to a reader or a writer.
   */
  enum Direction : uint8_t {
    kRead = 0,   ///< Bound to a @c BagReader; the plugin forwards replayed frames.
    kWrite = 1,  ///< Bound to a @c BagWriter; the plugin forwards frames before they are persisted.
  };

  /**
   * @brief Forwarding sink used by @c do_callback() to re-emit a frame downstream.
   *
   * @details
   * Supplied by the host (@c BagReader or @c BagWriter) at bind time and stored internally.  A single
   * @c const @c Frame& sink serves both directions: read plugins re-emit toward playback, write plugins
   * toward persistence.
   */
  using Callback = FrameCallback;

  /**
   * @brief Records the binding direction so the plugin can branch on read vs write.
   *
   * @details
   * Invoked by the host at attach time before any other hook.  The value is observable from
   * the hooks through @c get_direction().
   *
   * @param direction Side the plugin is being bound to.
   */
  void bind_direction(Direction direction);

  /**
   * @brief Returns the side this plugin is currently bound to.
   *
   * @return @c Direction::kRead when bound to a reader, @c Direction::kWrite when bound to a writer.
   */
  [[nodiscard]] Direction get_direction() const;

  /**
   * @brief Stores the forwarding sink used by @c do_callback().
   *
   * @details
   * Invoked by the host's @c bind_bag_interface() at attach time.  The plugin keeps @p callback in
   * @c callback_ and calls it from @c do_callback() to deliver a frame downstream -- toward the user's
   * playback callback on the read side, or toward persistence on the write side.  Cleared (with an
   * empty callable) on rebind and at host teardown, so a plugin-owned worker thread cannot reach a
   * destroyed host.
   *
   * @param callback Sink that forwards a frame downstream.
   */
  void register_callback(Callback&& callback);

  /**
   * @brief Rewrites or filters a stored URL before playback begins (read side).
   *
   * @details
   * Called once per URL contained in the bag when the reader opens the file.  Implementations
   * may modify any of the three parameters in place to remap topics or override schema
   * metadata.  The default implementation keeps every URL unchanged.
   *
   * @param url         URL string; may be modified in place.
   * @param ser_type    Serialisation type; may be modified in place.
   * @param schema_type Coarse schema family; may be modified in place.
   * @return @c true to retain the URL in playback; @c false to exclude it.
   */
  virtual bool convert_url_meta(std::string& url, std::string& ser_type, SchemaType& schema_type);

  /**
   * @brief Intercepts a replayed frame on its way to the user (read side).
   *
   * @details
   * Called for every replayed frame after timing pacing.  Forward it downstream by calling
   * @c do_callback(); transforming the payload, dropping the frame (by not emitting), fanning it out
   * (emitting several), or buffering and re-emitting frames @e reordered by data-plane time (e.g. via
   * @c BagProcessor) is permitted.
   *
   * @note @c Frame::ser_type and @c Frame::schema_type contain the effective URL metadata, including
   *       overrides made by @c convert_url_meta().  The payload is a shallow view valid for the duration
   *       of the call; copy it before buffering for asynchronous emit.
   *
   * @note Prefer @c convert_url_meta() for stable URL remapping.  If this hook emits a frame under a
   *       different URL, existing type fields remain authoritative because the plugin may have renamed a
   *       TypeA payload or transcoded it intentionally.  To resolve metadata registered for the emitted
   *       URL, clear @c ser_type and set @c schema_type to @c SchemaType::kUnknown before calling
   *       @c do_callback(); otherwise update both fields to describe the emitted payload explicitly.
   *
   * @param frame Replayed frame.
   */
  virtual void on_read(const Frame& frame) = 0;

  /**
   * @brief Intercepts a frame before it is persisted (write side).
   *
   * @details
   * Called for every frame handed to the writer, before it is recorded.  Re-emit it by calling
   * @c do_callback().  Transcoding (rewrite @c frame.data plus @c frame.ser_type / @c schema_type, e.g.
   * raw image to JPEG), dropping the frame (by not emitting), fanning it out, or buffering and
   * re-emitting frames @e reordered by data-plane time (a sliding-window reorder, e.g. via
   * @c BagProcessor) is permitted.
   *
   * @note The payload is a view valid for the duration of the call; a plugin that emits
   *       asynchronously must copy it before buffering.
   *
   * @note When a plugin renames the URL, the recorder learns the source-to-recorded mapping only for
   *       @e synchronous emits (within this @c on_write() call) and only when the rewrite is one-to-one,
   *       so URL-level metadata such as loss stays correctly attributed.  A plugin that both renames
   *       and emits asynchronously is responsible for any loss attribution itself.
   *
   * @note @c BagWriter::push() resolves a negative @c Frame::timestamp to the writer clock @e before
   *       calling this hook; that auto-assignment does @b not re-run on the frames a plugin emits.  A
   *       re-emitted frame is persisted with its own @c Frame::timestamp verbatim, so a plugin that
   *       constructs a fresh frame must set a resolved (non-negative) timestamp on it.
   *
   * @param frame Frame to persist.
   */
  virtual void on_write(const Frame& frame) = 0;

  /**
   * @brief Discards state retained from an earlier read-side playback session.
   *
   * @details
   * Called synchronously before a reader starts each top-level playback session and before its ready
   * callback.  A plugin that buffers frames must override this to discard the cache and reset all time
   * anchors without emitting frames, typically with @c processor_.reset().  This isolates a new play or
   * jump from frames retained when the preceding session was interrupted.  The default implementation is
   * a no-op for synchronous plugins.  The writer does not call this hook.
   */
  virtual void on_reset();

  /**
   * @brief Drains any internally-buffered frames downstream before the host unbinds and tears down.
   *
   * @details
   * Called by the host on its own thread, while its sink is still valid, after each naturally completed
   * read-side playback pass and right before either side detaches the plugin.  An interrupted pass skips
   * this boundary call.  A plugin that buffers frames for
   * asynchronous re-emit (e.g. a @c BagProcessor reorder buffer) must override this to flush those frames
   * synchronously -- typically @c processor_.flush() -- so a buffered tail is recorded / replayed instead
   * of dropped and cannot leak into the next playback pass.  The default implementation is a no-op (a
   * synchronous plugin holds nothing back).  On detach, after @c flush() returns, the host stops delivering
   * this plugin's emitted frames, so any frame produced afterwards is ignored.
   */
  virtual void flush();

  /**
   * @brief Forwards one frame downstream through the registered sink.
   *
   * @details
   * The emit helper a plugin calls -- typically from @c on_read() / @c on_write() or from a reorder
   * buffer's output callback -- to deliver a frame downstream without touching @c callback_ directly.
   * Invokes @c callback_ when one is registered and is otherwise a no-op.
   *
   * @param frame Frame to forward to the sink.
   */
  void do_callback(const Frame& frame);

 protected:
  Direction direction_{Direction::kRead};

 private:
  Callback callback_;

  VLINK_DISALLOW_COPY_AND_ASSIGN(BagPluginInterface)
};

////////////////////////////////////////////////////////////////
/// Details
////////////////////////////////////////////////////////////////

inline void BagPluginInterface::bind_direction(Direction direction) { direction_ = direction; }

inline BagPluginInterface::Direction BagPluginInterface::get_direction() const { return direction_; }

inline void BagPluginInterface::register_callback(Callback&& callback) { callback_ = std::move(callback); }

inline bool BagPluginInterface::convert_url_meta(std::string& url, std::string& ser_type,
                                                 SchemaType& schema_type) {  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
  (void)url;
  (void)ser_type;
  (void)schema_type;

  return true;  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
}

inline void BagPluginInterface::on_reset() {}

inline void BagPluginInterface::flush() {}

inline void BagPluginInterface::do_callback(const Frame& frame) {
  if (callback_) {
    callback_(frame);
  }
}

}  // namespace vlink
