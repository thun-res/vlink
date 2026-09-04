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
 * @file vcap_writer.h
 * @brief Recorder that serialises VLink traffic into the MCAP container (@c .vcap / @c .vcapx).
 *
 * @details
 * @c VCAPWriter materialises a @c BagWriter that emits Foxglove-compatible MCAP files.  It
 * inherits the cross-format split-recording machinery from @c BagWriter and produces files
 * that any MCAP-aware tool can replay.  Compression flips between no-compression and Zstd
 * when the build links @c libzstd; other selectors degrade to plain chunks.
 *
 * @par File-format diagram
 * @code
 *  push_schema()  -->  schema record
 *  push(frame)    -->  channel record (once per URL)
 *                 -->  message chunk (Zstd optional)
 *  close()        -->  attachments + summary section + footer (index of summary)
 * @endcode
 *
 * @par Writer states
 * @code
 *   +----------+   on_begin()  +---------+   push()/push_schema()   +-----------+
 *   |  closed  | ------------> | opening | -----------------------> | recording |
 *   +----------+               +---------+                          +-----------+
 *        ^                                                                 |
 *        |                                                                 v split triggered
 *        |                                                          +-------------+
 *        |                                                          | rotating    |
 *        |                                                          | (new .vcap) |
 *        |                                                          +-------------+
 *        |                                                                 |
 *        +-------------------- close() / dtor ---------------------- finalising
 *                                                                          |
 *                                                                          v
 *                                                                       sealed
 * @endcode
 *
 * @par Example
 * @code
 *   auto writer = vlink::BagWriter::create("/data/recording.vcap");
 *
 *   writer->async_run();
 *   writer->push_schema(my_schema);
 *
 *   vlink::Frame frame;
 *   frame.timestamp   = -1;  // < 0 => writer auto-assigns from its clock (0 is recorded verbatim)
 *   frame.url         = "dds://lidar/front";
 *   frame.ser_type    = "demo.proto.PointCloud";
 *   frame.schema_type = vlink::SchemaType::kProtobuf;
 *   frame.action_type = vlink::ActionType::kPublish;
 *   frame.data        = serialized_bytes;
 *   writer->push(frame);
 *
 *   writer->wait_for_idle();
 *   writer->quit();
 *   writer->wait_for_quit();
 *   writer->close();
 * @endcode
 *
 * @see BagWriter, VDBWriter
 */

#pragma once

#include <memory>
#include <string>

#include "./bag_writer.h"

namespace vlink {

/**
 * @class VCAPWriter
 * @brief Concrete MCAP-format @c BagWriter implementation with optional Zstd compression.
 *
 * @details
 * Prefer @c BagWriter::create() for format-agnostic instantiation; instantiate this class
 * directly when an MCAP-specific feature is required.
 */
class VLINK_EXPORT VCAPWriter final : public BagWriter {
 public:
  /**
   * @brief Opens or creates an MCAP file for recording.
   *
   * @param path    Filesystem path of the @c .vcap or @c .vcapx target.
   * @param config  Recording configuration (split policy, compression, etc.).
   */
  explicit VCAPWriter(const std::string& path, const Config& config = {});

  /**
   * @brief Finalises the MCAP footer and flushes outstanding writes.
   */
  ~VCAPWriter() override;

  /**
   * @brief Writes the channel metadata and MCAP footer, then closes the file; idempotent.
   */
  void close() override;

  /**
   * @brief Registers a callback invoked at each file-split boundary.
   *
   * @param callback  Receives (split_index, filename) before or after the split.
   * @param before    @c true fires before the new file is opened; @c false fires after.
   */
  void register_split_callback(SplitCallback&& callback, bool before) override;

  /**
   * @brief Registers a resolver that maps a serialisation-type string to @c SchemaData.
   *
   * @param callback  Function consulted before writing a channel record.
   */
  void register_schema_callback(SchemaCallback&& callback) override;

  /**
   * @brief Embeds @p schema_data in the MCAP file for offline introspection.
   *
   * @param schema_data Schema descriptor to embed.
   * @return @c false when a synchronous merge fails or an asynchronous merge cannot be queued.
   */
  bool push_schema(const SchemaData& schema_data) override;

  /**
   * @brief Returns the current value of the internal dumping flag.
   *
   * @return @c true while messages are actively being persisted.
   */
  [[nodiscard]] bool is_dumping() const override;

  /**
   * @brief Reports whether split-file recording is active.
   *
   * @return @c true when emitting a @c .vcapx manifest with multiple parts.
   */
  [[nodiscard]] bool is_split_mode() const override;

  /**
   * @brief Returns the index of the split part currently being written.
   *
   * @return Zero-based split index.
   */
  [[nodiscard]] int get_split_index() const override;

 protected:
  int64_t record(const Frame& frame, int64_t timestamp) override;

  int64_t get_record_timestamp() const override;

  size_t get_max_task_count() const override;

  void on_begin() override;

  void on_end() override;

 private:
  void open(const std::string& path);

  bool open_split(const std::string& path);

  void close_segment();

  bool merge_schema(SchemaData& schema_data);

  bool load_schema(const std::string& ser_type, SchemaType& schema_type, SchemaData& schema_data);

  bool write(const std::string& url, const std::string& ser_type, SchemaType schema_type, ActionType action_type,
             const Bytes& data, int64_t microseconds_timestamp);

  bool write_filex(bool complete = true);

  struct Impl;
  std::unique_ptr<Impl> impl_;

  VLINK_DISALLOW_COPY_AND_ASSIGN(VCAPWriter)
};

}  // namespace vlink
