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

#include <vlink/base/plugin.h>
#include <vlink/extension/convert_plugin_interface.h>

#include <rerun.hpp>

#include "../../mapping.h"
#include "../../schema_registry.h"

namespace vlink {
namespace webviz {

struct RerunRoute final {
  bool valid{true};
  SchemaType type{SchemaType::kUnknown};
  std::string ser;
  const SourceSchema* schema{nullptr};
  std::vector<const MessageMapping*> mappings;
  bool plugin{false};
};

class RerunConverter final {
 public:
  struct Config final : ConversionConfig {
    std::string timestamp_timeline{"timestamp"};
    bool use_timestamp_timeline{true};
  };

  explicit RerunConverter(const Config& config);
  [[nodiscard]] bool valid() const { return mappings_.valid(); }
  [[nodiscard]] RerunRoute resolve(std::string_view url, SchemaType type, const std::string& ser);
  [[nodiscard]] bool convert_and_log(::rerun::RecordingStream& recording, const std::string& path,
                                     const RerunRoute& route, const Bytes& raw, int64_t fallback_timestamp_ns = -1);

 private:
  SchemaRegistry registry_;
  MappingSet mappings_;
  std::string timeline_;
  Plugin plugin_loader_;
  std::shared_ptr<ConvertPluginInterface> plugin_;
};

}  // namespace webviz
}  // namespace vlink
