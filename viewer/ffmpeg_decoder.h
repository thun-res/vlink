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

#pragma once

#include <vlink/base/bytes.h>
#include <vlink/base/message_loop.h>
#include <vlink/version.h>

#include <memory>

class FFmpegDecoder : protected vlink::MessageLoop {
 public:
  using DataCallback =
      vlink::MoveFunction<void(int channel, int seq, int width, int height, const vlink::Bytes& img_data)>;

  using ErrorCallback = vlink::MoveFunction<void(int channel, int seq)>;

  enum class InType : uint8_t {
    kUnknown = 0,
    kJPG = 1,
    kH264 = 2,
    kH265 = 3,
    kMPEG4 = 4,
    kYUV420 = 11,
    kYUV422 = 12,
    kYUV444 = 13,
    kNV12 = 14,
    kYUYV = 15,
    kYVYU = 16,
    kUYVY = 17,
    kBGR888 = 21,
    kRGB888 = 22,
  };

  enum class OutType : uint8_t {
    kUnknown = 0,
    kBGR888 = 21,
    kRGB888 = 22,
  };

  struct Config {
    InType in_type{InType::kUnknown};
    OutType out_type{OutType::kUnknown};
    int width{0};
    int height{0};
    double scale{1.0};
    bool cache_frame{false};
    bool use_hard_codec{false};
    int max_elapsed_time{100};
    int max_codec_time{0};
  };

  explicit FFmpegDecoder(const Config& config);

  ~FFmpegDecoder() override;

  [[nodiscard]] static bool is_valid();

  void register_handler(DataCallback&& callback);

  void register_error_handler(ErrorCallback&& error_callback);

  void post_data(int channel, int seq, const vlink::Bytes& raw_data);

  bool wait_for_idle(int ms = vlink::Timer::kInfinite, bool check = true) override;

  [[nodiscard]] float get_average_decode_cost();

 protected:
  size_t get_max_task_count() const override;

  uint32_t get_max_elapsed_time() const override;

  void on_begin() override;

  void on_end() override;

#ifdef VLINK_VERSION_CHECK
  void on_task_timeout(Callback&& callback, uint32_t elapsed_time) override;
#else
  void on_task_timeout(const Callback& callback, uint32_t elapsed_time) override;
#endif

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
