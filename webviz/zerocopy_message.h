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
#include <vlink/zerocopy/message_parser.h>

#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace google {

namespace protobuf {

class Message;

}  // namespace protobuf

}  // namespace google

namespace vlink {

namespace webviz {

struct PointCloudFieldView final {
  zerocopy::MessageParser::Field field;
  uint16_t offset{0};
  size_t index{0};
};

class PointCloudView final {
 public:
  explicit PointCloudView(const zerocopy::MessageParser& parser);

  explicit PointCloudView(const zerocopy::PointCloud& point_cloud);

  [[nodiscard]] bool valid() const noexcept;

  [[nodiscard]] size_t size() const noexcept;

  [[nodiscard]] size_t pack_size() const noexcept;

  [[nodiscard]] const Bytes& data() const noexcept;

  [[nodiscard]] const std::vector<PointCloudFieldView>& fields() const noexcept;

  [[nodiscard]] const PointCloudFieldView* find(std::string_view name) const noexcept;

  bool value(size_t point, const PointCloudFieldView& field, zerocopy::MessageParser::Value& out) const noexcept;

  bool numeric(size_t point, const PointCloudFieldView& field, double& out) const noexcept;

 private:
  std::vector<PointCloudFieldView> fields_;
  Bytes data_;
  size_t size_{0};
  size_t pack_size_{0};
  uint64_t extent_{0};
  bool valid_{false};
};

std::unique_ptr<google::protobuf::Message> make_zerocopy_message(const std::string& ser, const Bytes& raw,
                                                                 const std::vector<std::string>& sources);

std::unique_ptr<google::protobuf::Message> make_zerocopy_message(const zerocopy::MessageParser& parser,
                                                                 const std::vector<std::string>& sources);

}  // namespace webviz

}  // namespace vlink
