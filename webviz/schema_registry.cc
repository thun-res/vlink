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

#include "./schema_registry.h"

#include <flatbuffers/util.h>
#include <google/protobuf/descriptor.pb.h>
#include <google/protobuf/util/json_util.h>
#include <vlink/base/helpers.h>
#include <vlink/base/logger.h>
#include <vlink/extension/schema_plugin_manager.h>

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <limits>
#include <unordered_set>

namespace vlink {
namespace webviz {

static std::vector<std::filesystem::path> schema_files(const std::string& directory, std::string_view extension) {
  std::vector<std::filesystem::path> result;

  if (directory.empty()) {
    return result;
  }

  std::error_code error;
  std::filesystem::recursive_directory_iterator iter(directory, error);

  for (; !error && iter != std::filesystem::recursive_directory_iterator{}; iter.increment(error)) {
    if (iter->is_regular_file(error) && iter->path().extension() == extension) {
      result.push_back(iter->path());
    }
  }

  if (error) {
    MLOG_E("Cannot enumerate schemas in {}: {}", directory, error.message());
  }

  std::sort(result.begin(), result.end());
  return result;
}

static void append_proto_dependencies(const google::protobuf::FileDescriptor& file,
                                      google::protobuf::FileDescriptorSet& output,
                                      std::unordered_set<const google::protobuf::FileDescriptor*>& seen) {
  if (!seen.insert(&file).second) {
    return;
  }

  for (int i = 0; i < file.dependency_count(); ++i) {
    append_proto_dependencies(*file.dependency(i), output, seen);
  }

  file.CopyTo(output.add_file());
}

SchemaRegistry::SchemaRegistry(const ConversionConfig& config) {
  auto& manager = SchemaPluginManager::get(config.schema_plugin_path);

  if (manager.is_valid()) {
    plugin_ = manager.get_interface();
  }

#ifdef VLINK_HAS_PROTO_COMPILER
  if (!config.proto_dir.empty()) {
    source_tree_.MapPath("", config.proto_dir);
    importer_ = std::make_unique<google::protobuf::compiler::Importer>(&source_tree_, nullptr);

    for (const auto& path : schema_files(config.proto_dir, ".proto")) {
      const auto relative = Helpers::path_to_string(path.lexically_relative(config.proto_dir));
      const auto* file = importer_->Import(relative);

      if (!file) {
        MLOG_E("Cannot import protobuf schema: {}", relative);
        continue;
      }

      for (int i = 0; i < file->message_type_count(); ++i) {
        add_proto(*file->message_type(i));
      }
    }
  }
#endif

  load_flatbuffers(config.fbs_dir);
}

void SchemaRegistry::add_proto(const google::protobuf::Descriptor& fallback) {
  const auto* preferred = plugin_ ? static_cast<const google::protobuf::Descriptor*>(
                                        plugin_->search_protobuf_descriptor(std::string(fallback.full_name())))
                                  : nullptr;
  const auto& descriptor = preferred ? *preferred : fallback;
  if (proto_schemas_.count(std::string(descriptor.full_name())) != 0U) {
    return;
  }

  auto entry = std::make_unique<SourceSchema>();
  entry->name = std::string(descriptor.full_name());
  entry->prototype = factory_.GetPrototype(&descriptor);
  google::protobuf::FileDescriptorSet files;
  std::unordered_set<const google::protobuf::FileDescriptor*> seen;
  append_proto_dependencies(*descriptor.file(), files, seen);
  if (!files.SerializeToString(&entry->data)) {
    MLOG_E("Cannot serialize protobuf schema: {}", entry->name);
    return;
  }
  proto_schemas_[entry->name] = std::move(entry);

  for (int i = 0; i < descriptor.nested_type_count(); ++i) {
    add_proto(*descriptor.nested_type(i));
  }
}

void SchemaRegistry::load_flatbuffers(const std::string& directory) {
  for (const auto& file : schema_files(directory, ".fbs")) {
    std::string text;
    const auto path = Helpers::path_to_string(file);

    if (!flatbuffers::LoadFile(path.c_str(), false, &text)) {
      MLOG_E("Cannot read FlatBuffers schema: {}", path);
      continue;
    }

    flatbuffers::Parser parser;
    const auto parent = Helpers::path_to_string(file.parent_path());
    const char* includes[] = {parent.c_str(), directory.c_str(), nullptr};

    if (!parser.Parse(text.c_str(), includes, path.c_str())) {
      MLOG_E("Cannot parse FlatBuffers schema {}: {}", path, parser.error_);
      continue;
    }

    for (const auto* object : parser.structs_.vec) {
      if (object->fixed) {
        continue;
      }

      const auto name = object->defined_namespace->GetFullyQualifiedName(object->name);

      if (fbs_schemas_.count(name) || !parser.SetRootType(name.c_str())) {
        continue;
      }

      parser.Serialize();
      auto entry = std::make_unique<SourceSchema>();
      entry->name = name;
      entry->data.assign(reinterpret_cast<const char*>(parser.builder_.GetBufferPointer()), parser.builder_.GetSize());
      entry->flatbuffer = reflection::GetSchema(entry->data.data());
      fbs_schemas_.emplace(name, std::move(entry));
    }
  }
}

const SourceSchema* SchemaRegistry::find(const std::string& name, SchemaType type) {
  if (type != SchemaType::kProtobuf && type != SchemaType::kFlatbuffers) {
    return nullptr;
  }

  const std::lock_guard lock(mutex_);
  auto& entries = type == SchemaType::kProtobuf ? proto_schemas_ : fbs_schemas_;
  auto found = entries.find(name);

  if (found != entries.end()) {
    return found->second.get();
  }

  if (!plugin_) {
    return nullptr;
  }

  if (type == SchemaType::kProtobuf) {
    const auto* descriptor =
        static_cast<const google::protobuf::Descriptor*>(plugin_->search_protobuf_descriptor(name));

    if (!descriptor) {
      return nullptr;
    }

    add_proto(*descriptor);
    const auto added = proto_schemas_.find(name);
    return added == proto_schemas_.end() ? nullptr : added->second.get();
  }

  auto schema = plugin_->search_schema(name, type);

  if (schema.data.empty()) {
    return nullptr;
  }

  flatbuffers::Verifier verifier(schema.data.data(), schema.data.size());

  if (!reflection::VerifySchemaBuffer(verifier)) {
    MLOG_E("Invalid BFBS supplied by schema plugin: {}", name);
    return nullptr;
  }

  flatbuffers::Parser parser;
  if (!parser.Deserialize(schema.data.data(), schema.data.size()) || !parser.SetRootType(name.c_str())) {
    MLOG_E("BFBS plugin schema has no root table: {}", name);
    return nullptr;
  }
  parser.Serialize();
  auto entry = std::make_unique<SourceSchema>();
  entry->name = name;
  entry->data.assign(reinterpret_cast<const char*>(parser.builder_.GetBufferPointer()), parser.builder_.GetSize());
  entry->flatbuffer = reflection::GetSchema(entry->data.data());
  const auto* result = entry.get();
  entries.emplace(name, std::move(entry));
  return result;
}

bool SchemaRegistry::encode_json(const std::string& name, SchemaType type, const Bytes& json, Bytes& output) {
  const auto* schema = find(name, type);

  if (!schema) {
    return false;
  }

  const std::string text(reinterpret_cast<const char*>(json.data()), json.size());

  if (type == SchemaType::kProtobuf) {
    std::unique_ptr<google::protobuf::Message> message(schema->prototype->New());
    const auto result = google::protobuf::util::JsonStringToMessage(text, message.get());

    if (!result.ok()) {
      MLOG_W("Invalid protobuf JSON for {}: {}", name, result.ToString());
      return false;
    }

    const auto size = message->ByteSizeLong();

    if (size > static_cast<size_t>(std::numeric_limits<int>::max())) {
      return false;
    }

    output = Bytes::create(size);
    return message->SerializeToArray(output.data(), static_cast<int>(size));
  }

  flatbuffers::Parser parser;
  parser.opts.strict_json = true;

  if (!parser.Deserialize(reinterpret_cast<const uint8_t*>(schema->data.data()), schema->data.size()) ||
      !parser.ParseJson(text.c_str())) {
    MLOG_W("Invalid FlatBuffers JSON for {}: {}", name, parser.error_);
    return false;
  }

  output = Bytes::create(parser.builder_.GetSize());
  std::memcpy(output.data(), parser.builder_.GetBufferPointer(), output.size());
  return true;
}

bool SchemaRegistry::decode_json(const std::string& name, SchemaType type, const Bytes& raw, Bytes& output) {
  const auto* schema = find(name, type);
  if (!schema) {
    return false;
  }
  std::string text;
  if (type == SchemaType::kProtobuf) {
    std::unique_ptr<google::protobuf::Message> message(schema->prototype->New());
    if (raw.size() > INT_MAX || !message->ParseFromArray(raw.data(), static_cast<int>(raw.size())) ||
        !google::protobuf::util::MessageToJsonString(*message, &text).ok()) {
      return false;
    }
  } else {
    if (!schema->flatbuffer->root_table() ||
        !flatbuffers::Verify(*schema->flatbuffer, *schema->flatbuffer->root_table(), raw.data(), raw.size())) {
      return false;
    }
    flatbuffers::Parser parser;
    parser.opts.strict_json = true;
    if (!parser.Deserialize(reinterpret_cast<const uint8_t*>(schema->data.data()), schema->data.size()) ||
        flatbuffers::GenText(parser, raw.data(), &text)) {
      return false;
    }
    const auto json = nlohmann::json::parse(text, nullptr, false);
    if (json.is_discarded()) {
      return false;
    }
    text = json.dump();
  }
  output = Bytes::create(text.size());
  std::memcpy(output.data(), text.data(), text.size());
  return true;
}

bool DecodedMessage::decode(const SourceSchema* schema, SchemaType type, const std::string& ser, const Bytes& raw) {
  view_ = MessageView{};

  if (type == SchemaType::kZeroCopy) {
    if (!zero_.parse(ser, raw)) {
      return false;
    }

    view_ = MessageView(zero_);
    return true;
  }

  if (schema && type == SchemaType::kProtobuf && schema->prototype) {
    proto_.reset(schema->prototype->New());

    if (raw.size() > static_cast<size_t>(std::numeric_limits<int>::max()) ||
        !proto_->ParseFromArray(raw.data(), static_cast<int>(raw.size()))) {
      return false;
    }

    view_ = MessageView(*proto_);
    return true;
  }

  if (schema && type == SchemaType::kFlatbuffers && schema->flatbuffer && schema->flatbuffer->root_table()) {
    if (!flatbuffers::Verify(*schema->flatbuffer, *schema->flatbuffer->root_table(), raw.data(), raw.size())) {
      return false;
    }

    view_ = MessageView(raw.data(), *schema->flatbuffer);
    return true;
  }

  if (ser == "json") {
    json_ = nlohmann::json::parse(raw.data(), raw.data() + raw.size(), nullptr, false);

    if (json_.is_discarded()) {
      return false;
    }

    view_ = MessageView(json_);
    return true;
  }

  return false;
}

MessageView DecodedMessage::view() const { return view_; }

std::string DecodedMessage::text() const { return view_.text(); }

}  // namespace webviz
}  // namespace vlink
