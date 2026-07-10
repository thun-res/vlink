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
 * @file trigger_plugin_interface.h
 * @brief Plugin contract for reacting to trigger-recorder events -- above all, deciding what happens
 *        to a dumped bag @e after it is written (network upload, archival, notification, cleanup).
 *
 * @details
 * @c TriggerPluginInterface is a dynamic plugin loaded through the VLink @c Plugin framework and
 * attached to a @c TriggerRecorder via @c TriggerRecorder::bind_trigger_plugin_interface().  Unlike
 * @c BagPluginInterface -- a frame-forwarding pipeline that rewrites traffic in flight -- this
 * interface is an @b observer over the recorder's life cycle: the recorder owns the ring buffer and
 * the disk write, and the plugin is notified at each stage so it can drive the @e next step.  The
 * headline -- and only mandatory -- hook is @c on_dump_finished(), invoked once the bag file is fully
 * written and closed -- the natural place to upload the file to a backend, move it to long-term
 * storage, enqueue it for transfer, or fire an alert.
 *
 * A broad set of hooks is provided so an implementation can instrument the whole flow and override
 * only the stages it cares about; every hook other than @c on_dump_finished() carries a default no-op.
 *
 * Plugin contract:
 *
 * | Hook                | Stage / thread                    | Purpose                                     |
 * | ------------------- | --------------------------------- | ------------------------------------------- |
 * | on_start()          | engine start (caller thread)      | Acquire resources (open an upload session)  |
 * | on_stop()           | engine stop (caller thread)       | Release resources                           |
 * | on_trigger()        | trigger accepted (dump thread)    | A dump is about to run for this request     |
 * | on_dump_started()   | after the writer opens (dump)     | The output file is being written            |
 * | on_frame()          | per written frame (dump)          | Inspect / count each persisted frame        |
 * | on_dump_finished()  | after the file is closed (dump)   | Upload / archive / notify -- the next step  |
 * | on_dump_failed()    | dump aborted (dump)               | React to a failed dump                      |
 * | on_file_rotated()   | old dump removed (dump)           | A rotated-out file was deleted              |
 * | flush()             | before unbind / teardown          | Drain plugin-internal async work            |
 *
 * @note All hooks except @c on_start() / @c on_stop() run on the recorder's dump loop thread, one at a time
 *       (dumps are serialised).  A hook that performs slow work (a synchronous upload) blocks the next dump;
 *       offload to a plugin-owned worker and drain it in @c flush().  @c on_frame() fires once per frame and
 *       can be hot -- keep it cheap.
 *
 * @par Example (upload every dumped bag to a backend)
 * @code
 * class UploadPlugin : public vlink::TriggerPluginInterface {
 *  public:
 *   void on_dump_finished(const DumpResult& result) override {
 *     if (result.success) {
 *       queue_.push(result.path);  // hand off to a background uploader; drain it in flush()
 *     }
 *   }
 *
 *   void flush() override { queue_.drain(); }
 *
 *  private:
 *   UploadQueue queue_;
 * };
 * VLINK_PLUGIN_DECLARE(UploadPlugin, 1, 0)
 * @endcode
 */

#pragma once

#include <cstdint>
#include <string>

#include "../base/plugin.h"
#include "../impl/types.h"

namespace vlink {

/**
 * @class TriggerPluginInterface
 * @brief Abstract plugin base notified across a @c TriggerRecorder's life cycle and dump pipeline.
 *
 * @details
 * The host binds an instance through @c TriggerRecorder::bind_trigger_plugin_interface() and calls each
 * hook at the corresponding stage.  Every hook other than the mandatory @c on_dump_finished() has a
 * default no-op implementation, so an implementation overrides only what it needs.  Implementations
 * must be thread-compatible with the host's dump loop thread.
 */
class TriggerPluginInterface {
  VLINK_PLUGIN_REGISTER(TriggerPluginInterface)

 protected:
  TriggerPluginInterface() = default;

  virtual ~TriggerPluginInterface() = default;

 public:
  /**
   * @struct TriggerContext
   * @brief Describes an accepted trigger request, delivered to @c on_trigger().
   */
  struct TriggerContext final {
    std::string reason;            ///< Trigger reason (also written as the bag tag).
    std::string name_hint;         ///< Requested output file-name hint (may be empty).
    std::string out_file;          ///< Explicit output path requested (empty => auto-named).
    int64_t pre_ms{-1};            ///< Per-trigger pre window override in ms (<0 => configured default).
    int64_t post_ms{-1};           ///< Per-trigger post window override in ms (<0 => configured default).
    int64_t trigger_timestamp{0};  ///< Wall-clock time of the trigger instant, in microseconds.
  };

  /**
   * @struct DumpContext
   * @brief Describes the dump currently being written, delivered to @c on_dump_started() / @c on_frame().
   */
  struct DumpContext final {
    std::string reason;          ///< Trigger reason for this dump.
    std::string path;            ///< Output bag file path.
    int64_t start_timestamp{0};  ///< Wall-clock time of the window's first frame, in milliseconds.
    int64_t url_count{0};        ///< Number of URLs snapshotted into this dump (some may contribute 0 frames).
  };

  /**
   * @struct DumpResult
   * @brief Final outcome of a dump, delivered to @c on_dump_finished() / @c on_dump_failed().
   */
  struct DumpResult final {
    std::string reason;          ///< Trigger reason for this dump.
    std::string path;            ///< Output bag file path.
    int64_t frame_count{0};      ///< Number of frames accepted by the writer (submitted minus rejected).
    int64_t dropped_count{0};    ///< Frames the writer rejected on submission (e.g. queue overflow).
    int64_t byte_count{0};       ///< Total payload bytes accepted by the writer.
    int64_t url_count{0};        ///< Number of URLs snapshotted into this dump.
    int64_t start_timestamp{0};  ///< Wall-clock time of the window's first frame, in milliseconds.
    int64_t duration_us{0};      ///< Wall-clock time spent writing the dump, in microseconds.
    bool success{false};         ///< @c true when the dump completed and the file was closed.
    std::string error;           ///< Human-readable failure reason when @c success is @c false.
  };

  /**
   * @brief Notifies the plugin that the recorder has started.
   *
   * @details
   * Invoked on the caller's thread from @c TriggerRecorder::start(), after discovery and the dump loop
   * are running.  A place to acquire long-lived resources such as an upload session.  Default: no-op.
   */
  virtual void on_start();

  /**
   * @brief Notifies the plugin that the recorder is stopping.
   *
   * @details
   * Invoked on the caller's thread from @c TriggerRecorder::stop(), after any in-flight dump has
   * finished.  A place to release resources acquired in @c on_start().  Default: no-op.
   */
  virtual void on_stop();

  /**
   * @brief Notifies the plugin that a trigger was accepted and a dump is about to run.
   *
   * @details
   * Invoked on the dump loop thread at the start of the dump, before the window is snapshotted.
   * Default: no-op.
   *
   * @param context Accepted trigger request details.
   */
  virtual void on_trigger(const TriggerContext& context);

  /**
   * @brief Notifies the plugin that the output file has been opened and writing is beginning.
   *
   * @details
   * Invoked on the dump loop thread after the bag writer is created, before frames are written.
   * Default: no-op.
   *
   * @param context Dump-in-progress details.
   */
  virtual void on_dump_started(const DumpContext& context);

  /**
   * @brief Inspects each frame as it is submitted to the writer (hot path).
   *
   * @details
   * Invoked on the dump loop thread once per frame successfully handed to the writer, in ascending
   * capture-time order.  Frames the writer rejects outright are @b not reported here (see
   * @c DumpResult::dropped_count).  This observes @e submission, not final persistence: a bound bag reorder
   * plugin may still reorder or drop the frame downstream before it reaches disk.  Runs for potentially many
   * frames, so keep the implementation cheap; it cannot alter or drop the frame.  Default: no-op.
   *
   * @param frame   Submitted frame (payload is a shallow view valid for the call).
   * @param context Dump-in-progress details.
   */
  virtual void on_frame(const Frame& frame, const DumpContext& context);

  /**
   * @brief Notifies the plugin that the dump file has been fully written and closed.
   *
   * @details
   * The headline -- and only mandatory -- hook: invoked on the dump loop thread once the bag is finalised.
   * The place to upload, archive, or notify.  Slow work here blocks the next dump -- offload to a worker and
   * drain it in @c flush().
   *
   * @param result Final dump outcome, with @c success set to @c true.
   */
  virtual void on_dump_finished(const DumpResult& result) = 0;

  /**
   * @brief Notifies the plugin that a dump was aborted before completion.
   *
   * @details
   * Invoked on the dump loop thread when the writer could not be created or the write failed.  Default:
   * no-op.
   *
   * @param result Dump outcome, with @c success set to @c false and @c error describing the failure.
   */
  virtual void on_dump_failed(const DumpResult& result);

  /**
   * @brief Notifies the plugin that an old dump file was removed by rotation.
   *
   * @details
   * Invoked on the dump loop thread each time file rotation deletes an aged-out dump, so a plugin
   * mirroring files to a backend can mirror the deletion.  Default: no-op.
   *
   * @param path Path of the file that was deleted.
   */
  virtual void on_file_rotated(const std::string& path);

  /**
   * @brief Drains any plugin-internal asynchronous work before the host unbinds or tears down.
   *
   * @details
   * Invoked by the host on its own thread, while it is still valid, right before it detaches this
   * plugin (at recorder stop / teardown).  A plugin that offloads work to a background worker (e.g. an
   * upload queue) must override this to finish or checkpoint that work synchronously, so pending
   * uploads are not lost.  Default: no-op.
   */
  virtual void flush();

 private:
  VLINK_DISALLOW_COPY_AND_ASSIGN(TriggerPluginInterface)
};

////////////////////////////////////////////////////////////////
/// Details
////////////////////////////////////////////////////////////////

inline void TriggerPluginInterface::on_start() {}

inline void TriggerPluginInterface::on_stop() {}

inline void TriggerPluginInterface::on_trigger(const TriggerContext& context) { (void)context; }

inline void TriggerPluginInterface::on_dump_started(const DumpContext& context) { (void)context; }

inline void TriggerPluginInterface::on_frame(const Frame& frame, const DumpContext& context) {
  (void)frame;
  (void)context;
}

inline void TriggerPluginInterface::on_dump_failed(const DumpResult& result) { (void)result; }

inline void TriggerPluginInterface::on_file_rotated(const std::string& path) { (void)path; }

inline void TriggerPluginInterface::flush() {}

}  // namespace vlink
