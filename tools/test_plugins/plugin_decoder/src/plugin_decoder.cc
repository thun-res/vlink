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

#include <vlink/extension/bag_plugin_interface.h>
#include <vlink/extension/bag_processor.h>

#include <optional>
#include <string>

#include "./ffmpeg_decoder.h"

class PluginDecoder : public vlink::BagPluginInterface {
 public:
  PluginDecoder() : processor_(make_processor_config()) {
    FFmpegDecoder::Config ffmpeg_config;
    ffmpeg_config.in_type = FFmpegDecoder::InType::kH264;
    ffmpeg_config.out_type = FFmpegDecoder::OutType::kNV12;
    ffmpeg_config.width = 1920;
    ffmpeg_config.height = 1080;

    decoder_.emplace(ffmpeg_config);

    decoder_->register_handler(
        [this](int channel, int seq, int width, int height, const vlink::Bytes& img_data) { camera_data_ = img_data; });

    processor_.register_output_callback([this](const vlink::Frame& frame) { on_output(frame); });
  }

  ~PluginDecoder() override = default;

  VersionInfo get_version_info() const override {
    VersionInfo info;

    info.name = "PluginDecoder";
    info.version = "1.0.0";
    info.timestamp = "";
    info.tag = "";
    info.commit_id = "";

    return info;
  }

  bool convert_url_meta(std::string& url, std::string& ser_type, vlink::SchemaType& schema_type) override {
    (void)url;
    if (url == "shm://hal/compressed/cam_flb?depth=5") {
      url = "shm://hal/raw/cam_flb?depth=5";
      ser_type = "vlink::zerocopy::CameraFrame";
      schema_type = vlink::SchemaType::kZeroCopy;
      return true;
    }

    return false;
  }

  void on_read(const vlink::Frame& frame) override {
    vlink::Frame out;
    out.timestamp = frame.timestamp;
    out.url = frame.url;
    out.action_type = frame.action_type;

    if (frame.url == "shm://hal/compressed/cam_flb?depth=5") {
      decoder_->post_data(0, 0, frame.data);
      decoder_->wait_for_idle();
      out.data = camera_data_;
    } else {
      out.data = frame.data;
    }

    processor_.push(frame.timestamp, out);
  }

  void flush() override { processor_.flush(); }

  void on_output(const vlink::Frame& frame) {
    if (frame.url == "shm://hal/compressed/cam_flb?depth=5") {
      vlink::Frame it;
      it.timestamp = frame.timestamp;
      it.url = "shm://hal/raw/cam_flb?depth=5";
      it.action_type = frame.action_type;
      it.data = vlink::Bytes::shallow_copy(frame.data.data(), frame.data.size());
      do_callback(it);
    } else {
      do_callback(frame);
    }
  }

 private:
  static vlink::BagProcessor::Config make_processor_config() {
    vlink::BagProcessor::Config config;
    config.min_cache_time = 1000;
    config.max_cache_size = 1024UL * 1024UL * 1024 * 4;
    return config;
  }

  std::optional<FFmpegDecoder> decoder_;
  vlink::BagProcessor processor_;

  vlink::Bytes camera_data_;
};

VLINK_PLUGIN_DECLARE(PluginDecoder, 1, 0);
