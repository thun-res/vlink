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
 * primary lifecycle hook is @c on_dump_finished(), invoked once the bag file is fully
 * written and closed -- the natural place to upload the file to a backend, move it to long-term
 * storage, enqueue it for transfer, or fire an alert.
 *
 * A broad set of hooks is provided so an implementation can instrument the whole flow.  Implementations supply
 * @c init() and @c on_dump_finished(); the remaining hooks have default no-op implementations.
 *
 * Plugin contract:
 *
 * | Hook                | Stage / thread                    | Purpose                                     |
 * | ------------------- | --------------------------------- | ------------------------------------------- |
 * | init()              | plugin load (daemon thread)       | Parse the host-supplied configuration       |
 * | on_start()          | engine start (recorder loop)      | Acquire resources (open an upload session)  |
 * | on_stop()           | engine stop (recorder loop)       | Release resources                           |
 * | on_trigger()        | window captured (recorder loop)   | A dump is about to run for this request     |
 * | on_dump_started()   | writer opened (recorder loop)     | The output file is being written            |
 * | on_frame()          | frame submitted (recorder loop)   | Inspect / count each writer-accepted frame  |
 * | on_dump_finished()  | file closed (recorder loop)       | Upload / archive / notify -- the next step  |
 * | on_dump_failed()    | dump aborted (recorder loop)      | React to a failed dump                      |
 * | on_file_rotated()   | file removed (recorder loop)      | A rotated-out file was deleted              |
 * | flush()             | before unbind / teardown          | Drain plugin-internal async work            |
 *
 * @note Dump hooks and normal recorder start/stop hooks run on the recorder loop.  Bind the plugin while the
 *       recorder is stopped.  A slow hook blocks later recorder work, so offload it to a plugin-owned worker and
 *       drain it in @c flush().  @c on_frame() can be hot -- keep it cheap.
 * @note Hooks must not re-enter the owning recorder.
 *
 * @par Example (upload every dumped bag to a backend)
 * @code
 * class UploadPlugin : public vlink::TriggerPluginInterface {
 *  public:
 *   bool init(const std::string& config) override { return queue_.configure(config); }
 *
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
 * VLINK_PLUGIN_DECLARE(UploadPlugin, 2, 0)
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
 * hook at the corresponding stage.  Implementations provide @c init() and @c on_dump_finished(); the remaining
 * hooks have default no-op implementations.  Implementations must follow the threading contract documented
 * above.
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
    int64_t frame_count{0};      ///< Snapshot frames accepted by the writer; a bag plugin may alter final output count.
    int64_t dropped_count{0};    ///< Retained for compatibility; a synchronous dump aborts on failure, so this stays 0.
    int64_t byte_count{0};       ///< Total payload bytes accepted by the writer.
    int64_t url_count{0};        ///< Number of URLs snapshotted into this dump.
    int64_t start_timestamp{0};  ///< Wall-clock time of the window's first frame, in milliseconds.
    int64_t duration_us{0};      ///< Wall-clock time spent writing the dump, in microseconds.
    bool success{false};         ///< @c true when the dump completed and the file was closed.
    std::string error;           ///< Human-readable failure reason when @c success is @c false.
  };

  /**
   * @brief Initialises the plugin with an opaque configuration string.
   *
   * @details
   * Called once by a plugin-loading host before the plugin is bound to a recorder.  The string may be a file
   * path, JSON document or any other format defined by the plugin.  Programmatically constructed plugins call
   * this method themselves when configuration is required.
   *
   * @param config Plugin-defined configuration; may be empty.
   * @return @c true on success; @c false makes the host reject the plugin.
   */
  virtual bool init(const std::string& config) = 0;

  /**
   * @brief Notifies the plugin that the recorder has started.
   *
   * @details
   * Invoked on the recorder's loop thread during @c TriggerRecorder::async_run(), after discovery is running.
   * A place to acquire long-lived resources such as an upload session.  Default: no-op.
   */
  virtual void on_start();

  /**
   * @brief Notifies the plugin that the recorder is stopping.
   *
   * @details
   * Invoked on the recorder's loop thread during shutdown.  A place to release resources acquired in
   * @c on_start().  Default: no-op.
   */
  virtual void on_stop();

  /**
   * @brief Notifies the plugin that a trigger was accepted and a dump is about to run.
   *
   * @details
   * Invoked on the recorder's loop thread after the requested window has been snapshotted and before the bag
   * writer is opened.  Slow work delays persistence but does not change the captured window.  Default: no-op.
   *
   * @param context Accepted trigger request details.
   */
  virtual void on_trigger(const TriggerContext& context);

  /**
   * @brief Notifies the plugin that the output file has been opened and writing is beginning.
   *
   * @details
   * Invoked on the recorder's loop thread after the bag writer is created, before frames are written.
   * Default: no-op.
   *
   * @param context Dump-in-progress details.
   */
  virtual void on_dump_started(const DumpContext& context);

  /**
   * @brief Inspects each frame as it is submitted to the writer (hot path).
   *
   * @details
   * Invoked on the recorder's loop thread once per frame successfully handed to the writer, in ascending
   * capture-time order.  A frame the writer fails to persist aborts the dump and is @b not reported here.
   * This observes @e submission, not final persistence: a bound bag reorder
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
   * The primary lifecycle hook: invoked on the recorder's loop thread once the bag is finalised.
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
   * Invoked on the recorder's loop thread when the writer could not be created, the write failed, or an accepted
   * delayed dump was abandoned during shutdown.  Default: no-op.
   *
   * @param result Dump outcome, with @c success set to @c false and @c error describing the failure.
   */
  virtual void on_dump_failed(const DumpResult& result);

  /**
   * @brief Notifies the plugin that an old dump file was removed by rotation.
   *
   * @details
   * Invoked on the recorder's loop thread each time file rotation deletes an aged-out dump, so a plugin
   * mirroring files to a backend can mirror the deletion.  Default: no-op.
   *
   * @param path Path of the file that was deleted.
   */
  virtual void on_file_rotated(const std::string& path);

  /**
   * @brief Drains any plugin-internal asynchronous work before the host unbinds or tears down.
   *
   * @details
   * Invoked by the host while the plugin is still valid, right before it detaches this plugin (at recorder stop /
   * teardown).  A plugin that offloads work to a background worker (e.g. an upload queue) must override this to
   * finish or checkpoint that work synchronously, so pending uploads are not lost.  Default: no-op.
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
