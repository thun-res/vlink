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

// NOLINTBEGIN

#pragma once

#include <vlink/base/bytes.h>
#include <vlink/external/proxy_api.h>
#include <vlink/zerocopy/camera_frame.h>

#include "./flatbuffers_runtime_compat.h"

#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wcpp"
#endif
#include <google/protobuf/descriptor.h>
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic pop
#endif

#include <QCloseEvent>
#include <QDialog>
#include <QElapsedTimer>
#include <QImage>
#include <QResizeEvent>
#include <QVector3D>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "./ffmpeg_decoder.h"
#include "./projectiondialog.h"

namespace Ui {
class CameraDialog;
}

class CameraDialog : public QDialog {
  Q_OBJECT

 public:
  enum State : uint8_t {
    kNoImage = 0,
    kNoSupport = 1,
    kParseFailed = 2,
    kLoadFailed = 3,
    kLoadSucceed = 4,
  };

  explicit CameraDialog(QWidget* parent = nullptr);

  ~CameraDialog();

 protected:
  void showEvent(class QShowEvent* event) override;

  void hideEvent(class QHideEvent* event) override;

  void closeEvent(QCloseEvent* event) override;

  void resizeEvent(class QResizeEvent* event) override;

  void keyPressEvent(class QKeyEvent* event) override;

  void keyReleaseEvent(class QKeyEvent* event) override;

 private slots:
  void update_ui_for_proto(const QVariant& variant, const QElapsedTimer& timer);

  void update_ui_for_flatbuffers(const QVariant& variant, const QElapsedTimer& timer);

  void update_ui_for_zero_copy_types(const QVariant& variant, const QElapsedTimer& timer);

  void update_ui_for_unknown_types(const QVariant& variant, const QElapsedTimer& timer);

  void on_checkBox_display_clicked(bool checked);

  void on_pushButton_close_clicked();

  void on_pushButton_yuv_clicked();

  void on_checkBox_cache_clicked(bool checked);

  void on_checkBox_hard_toggled(bool checked);

  void on_pushButton_point3d_clicked();

  void on_pushButton_projection_clicked();

  void on_pushButton_pause_clicked();

  void process_image(const QString& url, int width, int height, int bytes_per_line, const QByteArray& img_data,
                     bool use_codec, int decoder_seq);

  void process_error(const QString& url, int decoder_seq);

 private:
  struct Detail;

  void process_qimage(const QString& url, const QImage& image, bool scaled_by_decoder);

  void process_frame(const std::string& url, Detail& detail, const vlink::zerocopy::CameraFrame& frame);

  void process_raw(const std::string& url, Detail& detail, const vlink::Bytes& raw_data,
                   FFmpegDecoder::InType decoder_type);

  void create_decoder(const std::string& url, FFmpegDecoder::InType type, int width = 0, int height = 0);

  FFmpegDecoder::InType get_decoder_type() const;

  bool has_yuv_format() const;

  int get_number_for_msg(const google::protobuf::Message* msg, const google::protobuf::FieldDescriptor* field);

  void process_projection();

 private:
  Ui::CameraDialog* ui;

  class MainWindow* window_{nullptr};

  friend class Point3DDialog;
  friend class CameraLabel;

  QTimer* timer_{nullptr};

  struct Detail {
    int channel{0};
    class CameraLabel* label{nullptr};
    float frame_count{0};
    float last_frame_count{0};
    int64_t total_rate{0};
    State state{kNoImage};
    std::optional<FFmpegDecoder> decoder;
    FFmpegDecoder::InType decoder_type{FFmpegDecoder::InType::kUnknown};
    int decoder_width{0};
    int decoder_height{0};
    int decoder_seq{0};
    QImage img;

    Detail() = default;
    ~Detail() = default;

    Detail(const Detail& detail) noexcept {
      channel = detail.channel;
      label = detail.label;
      frame_count = detail.frame_count;
      last_frame_count = detail.last_frame_count;
      total_rate = detail.total_rate;
      state = detail.state;
      decoder_type = detail.decoder_type;
      decoder_width = detail.decoder_width;
      decoder_height = detail.decoder_height;
      decoder_seq = detail.decoder_seq;
    }

    Detail& operator=(const Detail& detail) noexcept {
      channel = detail.channel;
      label = detail.label;
      frame_count = detail.frame_count;
      last_frame_count = detail.last_frame_count;
      total_rate = detail.total_rate;
      state = detail.state;
      decoder_type = detail.decoder_type;
      decoder_width = detail.decoder_width;
      decoder_height = detail.decoder_height;
      decoder_seq = detail.decoder_seq;

      return *this;
    }
  };

  bool multi_mode_{false};

  std::atomic_bool pause_flag_{false};

  std::unordered_map<std::string, Detail> camera_detail_map_;
  std::unordered_map<int, std::string> channel_map_;

  google::protobuf::Message* target_msg_{nullptr};

  std::vector<std::pair<const google::protobuf::Message*, const google::protobuf::FieldDescriptor*>> msg_list_;
  std::shared_ptr<FlatbuffersSchemaContext> target_fbs_context_;
  std::vector<std::string> fbs_field_list_;

  class QGridLayout* camera_layout_{nullptr};

  double display_quality_{1.0};

  std::string current_url_;
  std::unordered_set<std::string> select_urls_;

  class Point3DDialog* point3d_dialog_{nullptr};
  vlink::ProxyAPI::DataCallback data_callback_;
  std::mutex data_mutex_;

  ProjectionDialog* projection_dialog_{nullptr};
  ProjectionDialog::Params proj_params_;
  QVector<QVector3D> projection_points_;
  Eigen::Matrix<float, 3, 4> projection_matrix_;

  std::atomic_bool quit_flag_{false};
};

// NOLINTEND
