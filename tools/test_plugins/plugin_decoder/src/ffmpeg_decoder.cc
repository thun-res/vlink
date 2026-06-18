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

#include "./ffmpeg_decoder.h"

#include <vlink/base/condition_variable.h>
#include <vlink/base/elapsed_timer.h>
#include <vlink/base/logger.h>

#include <utility>

#ifdef VLINK_ENABLE_VIEWER_FFMPEG

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
}
#endif

[[maybe_unused]] static constexpr size_t kMaxTaskSize = 100000U;

struct VideoDecoder::Impl {
#ifdef VLINK_ENABLE_VIEWER_FFMPEG
  const AVCodec* codec{nullptr};
  AVCodecParserContext* parser_ctx{nullptr};
  AVCodecContext* codec_ctx{nullptr};
  SwsContext* sws_ctx{nullptr};
  AVFrame* in_frame{nullptr};
  AVFrame* out_frame{nullptr};
  AVFrame* hw_frame{nullptr};
  AVPacket* packet{nullptr};
  AVPixelFormat in_format{AV_PIX_FMT_NONE};
  AVPixelFormat out_format{AV_PIX_FMT_NONE};
#endif

  FFmpegDecoder::Config config;
  int src_width{0};
  int src_height{0};
  int out_width{0};
  int out_height{0};
  int64_t frame_num{0};
  size_t buffer_size{0};
  std::atomic<int64_t> cost_cnt{0};
  std::atomic<int64_t> cost_total{0};

  vlink::ElapsedTimer elapsed_timer;
  vlink::ElapsedTimer freq_timer;
  FFmpegDecoder::DataCallback image_callback;
  FFmpegDecoder::ErrorCallback error_callback;
  vlink::ConditionVariable cv;
  std::mutex mtx;
};

FFmpegDecoder::FFmpegDecoder(const Config& config) : impl_(std::make_unique<Impl>()) {
  impl_->config = config;

  if (impl_->config.in_type == InType::kUnknown) {
    VLOG_W("FfmpegDecoder: Input type is unknown.");
  }

  if (impl_->config.out_type == OutType::kUnknown) {
    VLOG_W("FfmpegDecoder: Output type is unknown.");
  }

  impl_->out_width = impl_->config.width * impl_->config.scale;
  impl_->out_height = impl_->config.height * impl_->config.scale;

  set_name("FFmpegDecoder");

  async_run();
}

FFmpegDecoder::~FFmpegDecoder() {
  quit(true);

  impl_->cv.notify_all();

  wait_for_quit();
}

bool FFmpegDecoder::is_valid() {
#ifdef VLINK_ENABLE_VIEWER_FFMPEG
  return true;
#else
  return false;
#endif
}

void FFmpegDecoder::register_handler(DataCallback&& callback) { impl_->image_callback = std::move(callback); }

void FFmpegDecoder::register_error_handler(ErrorCallback&& error_callback) {
  impl_->error_callback = std::move(error_callback);
}

void FFmpegDecoder::post_data(int channel, int seq, const vlink::Bytes& raw_data) {
  if VUNLIKELY (raw_data.empty() || !impl_->image_callback) {
    return;
  }

#ifdef VLINK_ENABLE_VIEWER_FFMPEG
  if VUNLIKELY (!impl_->codec_ctx) {
    return;
  }

  post_task([this, channel, seq, raw_data]() {
    impl_->elapsed_timer.restart();

    size_t pos = 0;
    int interval = 0;
    int freq = 0;
    int ret = 0;

    while (pos < raw_data.size()) {
      if (impl_->config.cache_frame && impl_->parser_ctx) {
        ret = av_parser_parse2(impl_->parser_ctx, impl_->codec_ctx, &impl_->packet->data, &impl_->packet->size,
                               raw_data.data() + pos, raw_data.size() - pos, AV_NOPTS_VALUE, AV_NOPTS_VALUE,
                               AV_NOPTS_VALUE);

        if VUNLIKELY (ret < 0) {
          continue;
        }

        pos += ret;

        if (impl_->packet->size == 0) {
          continue;
        }
      } else {
        if (impl_->packet) {
          av_packet_free(&impl_->packet);
        }

        impl_->packet = av_packet_alloc();
        impl_->packet->data = const_cast<uint8_t*>(raw_data.data() + pos);
        impl_->packet->size = raw_data.size() - pos;

        pos += impl_->packet->size;
      }

      ret = avcodec_send_packet(impl_->codec_ctx, impl_->packet);

      if VUNLIKELY (ret < 0) {
        if (impl_->error_callback) {
          impl_->error_callback(channel, seq);
        }

        continue;
      }

      AVFrame* src_frame = nullptr;

      while (avcodec_receive_frame(impl_->codec_ctx, impl_->in_frame) == 0) {
        src_frame = impl_->in_frame;

        if (impl_->codec_ctx->hw_device_ctx) {
          ret = av_hwframe_transfer_data(impl_->hw_frame, impl_->in_frame, 0);

          if VUNLIKELY (ret != 0) {
            if (impl_->error_callback) {
              impl_->error_callback(channel, seq);
            }

            continue;
          }

          src_frame = impl_->hw_frame;
        }

        if (impl_->src_width != impl_->in_frame->width || impl_->src_height != impl_->in_frame->height) {
          impl_->src_width = impl_->in_frame->width;
          impl_->src_height = impl_->in_frame->height;

          impl_->out_width = impl_->src_width * impl_->config.scale;
          impl_->out_height = impl_->src_height * impl_->config.scale;

          impl_->buffer_size = av_image_get_buffer_size(impl_->out_format, impl_->out_width, impl_->out_height, 1);

          av_image_alloc(impl_->out_frame->data, impl_->out_frame->linesize, impl_->out_width, impl_->out_height,
                         impl_->out_format, 1);

          if (impl_->sws_ctx) {
            sws_freeContext(impl_->sws_ctx);
          }

          impl_->sws_ctx = sws_getContext(
              impl_->src_width, impl_->src_height, static_cast<AVPixelFormat>(src_frame->format), impl_->out_width,
              impl_->out_height, impl_->out_format, SWS_FAST_BILINEAR, nullptr, nullptr, nullptr);
        }

        sws_scale(impl_->sws_ctx, src_frame->data, src_frame->linesize, 0, impl_->src_height, impl_->out_frame->data,
                  impl_->out_frame->linesize);

        impl_->freq_timer.start();

        freq = impl_->codec_ctx->framerate.num;

        if (freq <= 0) {
          freq = 30;
        }

        interval = impl_->frame_num++ * 1000 / freq - impl_->freq_timer.get();

        if (std::abs(interval) >= 200) {  // fps <= 5, reset
          impl_->frame_num = 0;
          impl_->freq_timer.restart();
        } else if (interval > 0) {
          std::unique_lock lock(impl_->mtx);
          impl_->cv.wait_for(lock, std::chrono::milliseconds(interval));
        }

        if VUNLIKELY (is_ready_to_quit()) {
          return;
        }

        auto cost = impl_->elapsed_timer.get();

        if VUNLIKELY (impl_->config.max_codec_time > 0 && cost > impl_->config.max_codec_time) {
          return;
        }

        impl_->cost_total += cost;
        ++impl_->cost_cnt;

        impl_->image_callback(channel, seq, impl_->out_width, impl_->out_height,
                              vlink::Bytes::shallow_copy(impl_->out_frame->data[0], impl_->buffer_size));
      }
    }
  });
#else
  impl_->image_callback(channel, seq, 0, 0, vlink::Bytes());
#endif
}

bool FFmpegDecoder::wait_for_idle(int ms) { return vlink::MessageLoop::wait_for_idle(ms); }

float FFmpegDecoder::get_average_decode_cost() {
  float cost = -1;

  if (impl_->cost_cnt > 0) {
    cost = static_cast<float>(impl_->cost_total) / impl_->cost_cnt;
  }

  impl_->cost_total = 0;
  impl_->cost_cnt = 0;

  return cost;
}

size_t FFmpegDecoder::get_max_task_count() const { return kMaxTaskSize; }

uint32_t FFmpegDecoder::get_max_elapsed_time() const { return impl_->config.max_elapsed_time; }

void FFmpegDecoder::on_begin() {
#ifdef VLINK_ENABLE_VIEWER_FFMPEG
  if VUNLIKELY (impl_->codec) {
    return;
  }

  av_log_set_level(AV_LOG_FATAL);

  switch (impl_->config.in_type) {
    case InType::kJPG:
      impl_->in_format = AV_PIX_FMT_NONE;
      impl_->codec = avcodec_find_decoder(AV_CODEC_ID_MJPEG);
      break;
    case InType::kH264:
      impl_->in_format = AV_PIX_FMT_NONE;
      impl_->codec = avcodec_find_decoder(AV_CODEC_ID_H264);
      break;
    case InType::kH265:
      impl_->in_format = AV_PIX_FMT_NONE;
      impl_->codec = avcodec_find_decoder(AV_CODEC_ID_HEVC);
      break;
    case InType::kMPEG4:
      impl_->in_format = AV_PIX_FMT_NONE;
      impl_->codec = avcodec_find_decoder(AV_CODEC_ID_MPEG4);
      break;
    case InType::kYUV420:
      impl_->in_format = AV_PIX_FMT_YUV420P;
      impl_->codec = avcodec_find_decoder(AV_CODEC_ID_RAWVIDEO);
      break;
    case InType::kYUV422:
      impl_->in_format = AV_PIX_FMT_YUV422P;
      impl_->codec = avcodec_find_decoder(AV_CODEC_ID_RAWVIDEO);
      break;
    case InType::kYUV444:
      impl_->in_format = AV_PIX_FMT_YUV444P;
      impl_->codec = avcodec_find_decoder(AV_CODEC_ID_RAWVIDEO);
      break;
    case InType::kNV12:
      impl_->in_format = AV_PIX_FMT_NV12;
      impl_->codec = avcodec_find_decoder(AV_CODEC_ID_RAWVIDEO);
      break;
    case InType::kYUYV:
      impl_->in_format = AV_PIX_FMT_YUYV422;
      impl_->codec = avcodec_find_decoder(AV_CODEC_ID_RAWVIDEO);
      break;
    case InType::kYVYU:
      impl_->in_format = AV_PIX_FMT_YVYU422;
      impl_->codec = avcodec_find_decoder(AV_CODEC_ID_RAWVIDEO);
      break;
    case InType::kUYVY:
      impl_->in_format = AV_PIX_FMT_UYVY422;
      impl_->codec = avcodec_find_decoder(AV_CODEC_ID_RAWVIDEO);
      break;
    case InType::kBGR888:
      impl_->in_format = AV_PIX_FMT_BGR24;
      impl_->codec = avcodec_find_decoder(AV_CODEC_ID_RAWVIDEO);
      break;
    case InType::kRGB888:
      impl_->in_format = AV_PIX_FMT_RGB24;
      impl_->codec = avcodec_find_decoder(AV_CODEC_ID_RAWVIDEO);
      break;
    default:
      impl_->codec = avcodec_find_decoder(AV_CODEC_ID_MJPEG);
      return;
  }

  switch (impl_->config.out_type) {
    case OutType::kYUV420:
      impl_->out_format = AV_PIX_FMT_YUV420P;
      break;
    case OutType::kYUV422:
      impl_->out_format = AV_PIX_FMT_YUV422P;
      break;
    case OutType::kYUV444:
      impl_->out_format = AV_PIX_FMT_YUV444P;
      break;
    case OutType::kNV12:
      impl_->out_format = AV_PIX_FMT_NV12;
      break;
    case OutType::kYUYV:
      impl_->out_format = AV_PIX_FMT_YUYV422;
      break;
    case OutType::kYVYU:
      impl_->out_format = AV_PIX_FMT_YVYU422;
      break;
    case OutType::kUYVY:
      impl_->out_format = AV_PIX_FMT_UYVY422;
      break;
    case OutType::kBGR888:
      impl_->out_format = AV_PIX_FMT_BGR24;
      break;
    case OutType::kRGB888:
      impl_->out_format = AV_PIX_FMT_RGB24;
      break;
    default:
      impl_->out_format = AV_PIX_FMT_NONE;
      return;
  }

  if VUNLIKELY (!impl_->codec) {
    VLOG_W("FfmpegDecoder: avcodec_find_decoder error.");
    return;
  }

  if (impl_->codec->id == AV_CODEC_ID_RAWVIDEO) {
    if (impl_->config.width == 0 || impl_->config.height == 0) {
      VLOG_W("FfmpegDecoder: Raw video must set width and height.");
      return;
    }
  } else {
    impl_->parser_ctx = av_parser_init(impl_->codec->id);

    if VUNLIKELY (!impl_->parser_ctx) {
      VLOG_W("FfmpegDecoder: av_parser_init error.");
      return;
    }
  }

  impl_->codec_ctx = avcodec_alloc_context3(impl_->codec);

  if VUNLIKELY (!impl_->codec_ctx) {
    VLOG_W("FfmpegDecoder: avcodec_alloc_context3 error.");
    return;
  }

  impl_->codec_ctx->strict_std_compliance = FF_COMPLIANCE_UNOFFICIAL;
  impl_->codec_ctx->pix_fmt = impl_->in_format;
  impl_->codec_ctx->width = impl_->config.width;
  impl_->codec_ctx->height = impl_->config.height;

  if (impl_->config.use_hard_codec) {
    AVHWDeviceType type = AV_HWDEVICE_TYPE_NONE;
    AVBufferRef* hw_device_ctx = nullptr;
    for (;;) {
      type = av_hwdevice_iterate_types(type);

      if (type == AV_HWDEVICE_TYPE_NONE) {
        break;
      }

      if (type == AV_HWDEVICE_TYPE_VDPAU) {
        continue;
      }

      if (av_hwdevice_ctx_create(&hw_device_ctx, type, nullptr, nullptr, 0) == 0) {
        break;
      } else {
        hw_device_ctx = nullptr;
      }
    }

    if VLIKELY (hw_device_ctx) {
      // VLOG_I("FFmpeg hw codec: ", av_hwdevice_get_type_name(type));
      impl_->codec_ctx->hw_device_ctx = av_buffer_ref(hw_device_ctx);
    }
  }

  if VUNLIKELY (avcodec_open2(impl_->codec_ctx, impl_->codec, nullptr) < 0) {
    VLOG_W("FfmpegDecoder: avcodec_open2 error.");
    return;
  }

  impl_->in_frame = av_frame_alloc();

  impl_->out_frame = av_frame_alloc();

  impl_->hw_frame = av_frame_alloc();

  impl_->packet = av_packet_alloc();
#endif

  vlink::MessageLoop::on_begin();
}

void FFmpegDecoder::on_end() {
#ifdef VLINK_ENABLE_VIEWER_FFMPEG
  if VLIKELY (impl_->packet) {
    av_packet_free(&impl_->packet);
  }

  if VLIKELY (impl_->in_frame) {
    av_frame_free(&impl_->in_frame);
  }

  if VLIKELY (impl_->out_frame) {
    av_frame_free(&impl_->out_frame);
  }

  if VLIKELY (impl_->hw_frame) {
    av_frame_free(&impl_->hw_frame);
  }

  if VLIKELY (impl_->sws_ctx) {
    sws_freeContext(impl_->sws_ctx);
  }

  if VLIKELY (impl_->codec_ctx) {
    avcodec_free_context(&impl_->codec_ctx);
  }

  if VLIKELY (impl_->parser_ctx) {
    av_parser_close(impl_->parser_ctx);
  }

  impl_->codec = nullptr;
  impl_->parser_ctx = nullptr;
  impl_->codec_ctx = nullptr;
  impl_->sws_ctx = nullptr;
  impl_->in_frame = nullptr;
  impl_->out_frame = nullptr;
  impl_->hw_frame = nullptr;
  impl_->packet = nullptr;
#endif

  vlink::MessageLoop::on_end();
}

#ifdef VLINK_VERSION_CHECK
void FFmpegDecoder::on_task_timeout(Callback&& callback, uint32_t elapsed_time) {
#else
void FFmpegDecoder::on_task_timeout(const Callback& callback, uint32_t elapsed_time) {
#endif
  (void)callback;
  (void)elapsed_time;
}
