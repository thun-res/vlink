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

#include "dump_proto_cache.h"

#ifdef VLINK_HAS_PROTOBUF_COMPILER

#include <google/protobuf/descriptor.h>
#include <google/protobuf/dynamic_message.h>

#include <utility>

namespace vlink::dump {

ProtoMessageCache::ProtoMessageCache(ProtoRuntime runtime) : runtime_(std::move(runtime)) {}

void ProtoMessageCache::reset(ProtoRuntime runtime) {
  runtime_ = std::move(runtime);
  cache_.clear();
}

bool ProtoMessageCache::ready() const { return runtime_.factory && (runtime_.pool != nullptr || runtime_.plugin); }

const google::protobuf::Descriptor* ProtoMessageCache::lookup_descriptor(const std::string& ser) {
  if (runtime_.plugin) {
    const auto* descriptor =
        static_cast<const google::protobuf::Descriptor*>(runtime_.plugin->search_protobuf_descriptor(ser));

    if (descriptor != nullptr) {
      return descriptor;
    }
  }

  if (runtime_.pool != nullptr) {
    return runtime_.pool->FindMessageTypeByName(ser);
  }

  return nullptr;
}

google::protobuf::Message* ProtoMessageCache::get(const std::string& ser) {
  if (!runtime_.factory) {
    return nullptr;
  }

  auto iter = cache_.find(ser);

  if (iter != cache_.end()) {
    return iter->second.get();
  }

  const auto* descriptor = lookup_descriptor(ser);

  if (descriptor == nullptr) {
    cache_[ser] = nullptr;
    return nullptr;
  }

  const auto* prototype = runtime_.factory->GetPrototype(descriptor);

  if (prototype == nullptr) {
    cache_[ser] = nullptr;
    return nullptr;
  }

  auto message = std::unique_ptr<google::protobuf::Message>(prototype->New());
  auto* raw = message.get();
  cache_[ser] = std::move(message);
  return raw;
}

}  // namespace vlink::dump

#endif
