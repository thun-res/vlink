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

#include "./dump_features.h"

#ifdef VLINK_HAS_PROTOBUF_COMPILER

#include <google/protobuf/message.h>

#include <memory>
#include <string>
#include <unordered_map>

#include "./dump_schema.h"

namespace vlink::dump {

class ProtoMessageCache final {
 public:
  explicit ProtoMessageCache(ProtoRuntime runtime);

  bool ready() const;

  google::protobuf::Message* get(const std::string& ser);

 private:
  const google::protobuf::Descriptor* lookup_descriptor(const std::string& ser);

  ProtoRuntime runtime_;
  std::unordered_map<std::string, std::unique_ptr<google::protobuf::Message>> cache_;
};

}  // namespace vlink::dump

#endif
