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

#include "./flatbuffers_runtime.h"

#include <vlink/base/helpers.h>

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <utility>
#include <vector>

#ifdef VLINK_ENABLE_VIEWER_FLATBUFFERS

[[maybe_unused]] static std::string get_base_type_name(reflection::BaseType type) {
  switch (type) {
    case reflection::None:
      return "none";
    case reflection::UType:
      return "utype";
    case reflection::Bool:
      return "bool";
    case reflection::Byte:
      return "int8";
    case reflection::UByte:
      return "uint8";
    case reflection::Short:
      return "int16";
    case reflection::UShort:
      return "uint16";
    case reflection::Int:
      return "int32";
    case reflection::UInt:
      return "uint32";
    case reflection::Long:
      return "int64";
    case reflection::ULong:
      return "uint64";
    case reflection::Float:
      return "float";
    case reflection::Double:
      return "double";
    case reflection::String:
      return "string";
    case reflection::Vector:
      return "vector";
    case reflection::Obj:
      return "object";
    case reflection::Union:
      return "union";
    case reflection::Array:
      return "array";
    case reflection::Vector64:
      return "vector64";
    default:
      return "unknown";
  }
}

[[maybe_unused]] static bool is_numeric_type(reflection::BaseType type) {
  switch (type) {
    case reflection::Bool:
    case reflection::Byte:
    case reflection::UByte:
    case reflection::Short:
    case reflection::UShort:
    case reflection::Int:
    case reflection::UInt:
    case reflection::Long:
    case reflection::ULong:
    case reflection::Float:
    case reflection::Double:
      return true;
    default:
      return false;
  }
}

[[maybe_unused]] static const flatbuffers::Table* as_table(const FlatbuffersObjectView& view) {
  return reinterpret_cast<const flatbuffers::Table*>(view.data);
}

[[maybe_unused]] static const flatbuffers::Struct* as_struct(const FlatbuffersObjectView& view) {
  return reinterpret_cast<const flatbuffers::Struct*>(view.data);
}

bool FlatbuffersRuntime::load_dir(const std::string& dir, std::string* error) {
  if (error) {
    error->clear();
  }

  if (dir.empty()) {
    clear();
    return true;
  }

  try {
    std::filesystem::path path(dir);

    if (!std::filesystem::exists(path)) {
      if (error) {
        *error = "FlatBuffers directory does not exist.";
      }
      return false;
    }

    if (!std::filesystem::is_directory(path)) {
      if (error) {
        *error = "FlatBuffers path is not a directory.";
      }
      return false;
    }
  } catch (const std::filesystem::filesystem_error& e) {
    if (error) {
      *error = e.what();
    }

    return false;
  }

  dir_ = dir;
  context_map_.clear();
  return true;
}

void FlatbuffersRuntime::clear() {
  dir_.clear();
  context_map_.clear();
}

std::shared_ptr<FlatbuffersSchemaContext> FlatbuffersRuntime::find_context(const std::string& ser, std::string* error) {
  if (error) {
    error->clear();
  }

  auto iter = context_map_.find(ser);

  if (iter != context_map_.end()) {
    return iter->second;
  }

  if (dir_.empty()) {
    return nullptr;
  }

  bool has_import = false;
  std::shared_ptr<FlatbuffersSchemaContext> ctx;

  try {
    std::filesystem::path root_dir(dir_);
    import_fbs(ctx, ser, root_dir, root_dir, has_import, 0, error);
  } catch (const std::filesystem::filesystem_error& e) {
    if (error) {
      *error = e.what();
    }

    return nullptr;
  }

  if (!has_import || !ctx || !ctx->valid()) {
    return nullptr;
  }

  context_map_[ser] = ctx;
  return ctx;
}

vlink::SchemaData FlatbuffersRuntime::search_schema(const std::string& ser, std::string* error) {
  auto ctx = find_context(ser, error);

  if (!ctx || !ctx->valid()) {
    return {};
  }

  vlink::SchemaData schema;
  schema.name = ser;
  schema.encoding = "flatbuffers";
  schema.schema_type = vlink::SchemaType::kFlatbuffers;
  schema.data = ctx->bfbs;
  return schema;
}

void FlatbuffersRuntime::import_fbs(std::shared_ptr<FlatbuffersSchemaContext>& ctx, const std::string& target_ser,
                                    const std::filesystem::path& root_dir, const std::filesystem::path& sub_dir,
                                    bool& has_import, int depth, std::string* error) {
  if (ctx || depth >= 100) {
    return;
  }

  std::vector<std::filesystem::directory_entry> file_list;

  try {
    for (const auto& entry : std::filesystem::directory_iterator(sub_dir)) {
      file_list.emplace_back(entry);
    }
  } catch (const std::filesystem::filesystem_error&) {
    return;
  }

  if (file_list.empty() || file_list.size() > 1000) {
    return;
  }

  auto parser = std::make_shared<flatbuffers::Parser>();
  std::string root_dir_str = root_dir.string();
  std::string sub_dir_str = sub_dir.string();
  const char* include_root_dirs[] = {root_dir_str.c_str(), nullptr};
  const char* include_dirs[] = {root_dir_str.c_str(), sub_dir_str.c_str(), nullptr};

  for (const auto& file : file_list) {
    if (file.is_regular_file() && file.path().extension() == ".fbs") {
      std::string schema_file;

      if (!flatbuffers::LoadFile(file.path().string().c_str(), false, &schema_file)) {
        continue;
      }

      const bool parsed = (root_dir == sub_dir) ? parser->Parse(schema_file.c_str(), include_root_dirs)
                                                : parser->Parse(schema_file.c_str(), include_dirs);

      if (!parsed) {
        continue;
      }

      if (!parser->LookupStruct(target_ser)) {
        continue;
      }

      if (!parser->SetRootType(target_ser.c_str())) {
        continue;
      }

      parser->Serialize();

      auto bfbs = vlink::Bytes::create(parser->builder_.GetSize());
      if (bfbs.empty()) {
        continue;
      }

      std::memcpy(bfbs.data(), parser->builder_.GetBufferPointer(), bfbs.size());

      flatbuffers::Verifier verifier(bfbs.data(), bfbs.size());
      if (!reflection::VerifySchemaBuffer(verifier)) {
        continue;
      }

      auto target = std::make_shared<FlatbuffersSchemaContext>();
      target->parser = std::move(parser);
      target->bfbs = std::move(bfbs);
      target->schema = reflection::GetSchema(target->bfbs.data());
      target->root_object = target->schema ? target->schema->root_table() : nullptr;
      target->type_name = target_ser;
      target->schema_file = target->parser->root_struct_def_ ? target->parser->root_struct_def_->file : "";

      if (!target->valid()) {
        continue;
      }

      ctx = std::move(target);
      has_import = true;
      return;
    }

    if (file.is_directory()) {
      import_fbs(ctx, target_ser, root_dir, file.path(), has_import, depth + 1, error);

      if (ctx) {
        return;
      }
    }
  }

  if (!has_import && depth == 0 && error && error->empty()) {
    *error = "No matching FlatBuffers schema found.";
  }
}

const reflection::Field* FlatbuffersObjectView::find_field(const reflection::Object& obj, const std::string& name) {
  if (!obj.fields()) {
    return nullptr;
  }

  return obj.fields()->LookupByKey(name.c_str());
}

std::string FlatbuffersObjectView::get_field_type_name(const reflection::Field& field,
                                                       const reflection::Schema& schema) {
  if (field.type()->base_type() == reflection::Vector || field.type()->base_type() == reflection::Vector64) {
    const auto element_type = field.type()->element();
    if (element_type == reflection::Obj && schema.objects()) {
      const auto* obj = schema.objects()->Get(static_cast<uint32_t>(field.type()->index()));
      if (obj && obj->name()) {
        return obj->name()->str() + "[]";
      }
    }

    if (field.type()->index() >= 0 && schema.enums()) {
      const auto* enum_def = schema.enums()->Get(static_cast<flatbuffers::uoffset_t>(field.type()->index()));
      if (enum_def && enum_def->name()) {
        return enum_def->name()->str() + "[]";
      }
    }

    return get_base_type_name(element_type) + "[]";
  }

  if (field.type()->base_type() == reflection::Obj && schema.objects()) {
    const auto* obj = schema.objects()->Get(static_cast<uint32_t>(field.type()->index()));
    if (obj && obj->name()) {
      return obj->name()->str();
    }
  }

  if (field.type()->base_type() != reflection::Obj && field.type()->index() >= 0 && schema.enums()) {
    const auto* enum_def = schema.enums()->Get(static_cast<flatbuffers::uoffset_t>(field.type()->index()));
    if (enum_def && enum_def->name()) {
      return enum_def->name()->str();
    }
  }

  return get_base_type_name(field.type()->base_type());
}

bool FlatbuffersObjectView::make_root_view(const FlatbuffersSchemaContext& ctx, const vlink::Bytes& raw,
                                           FlatbuffersObjectView& out) {
  out = {};

  if (!ctx.valid() || raw.empty()) {
    return false;
  }

  if (!flatbuffers::Verify(*ctx.schema, *ctx.root_object, raw.data(), raw.size())) {
    return false;
  }

  const auto* root_table = flatbuffers::GetAnyRoot(raw.data());

  if (!root_table) {
    return false;
  }

  out.kind = FlatbuffersViewKind::kTable;
  out.data = root_table;
  out.object = ctx.root_object;
  return true;
}

bool FlatbuffersObjectView::get_child_view(const FlatbuffersObjectView& parent, const reflection::Field& field,
                                           const reflection::Schema& schema, FlatbuffersObjectView& out) {
  out = {};

  if (!parent.valid() || field.type()->base_type() != reflection::Obj || !schema.objects()) {
    return false;
  }

  const auto* obj = schema.objects()->Get(static_cast<uint32_t>(field.type()->index()));

  if (!obj) {
    return false;
  }

  if (parent.kind == FlatbuffersViewKind::kTable) {
    const auto* table = as_table(parent);

    if (obj->is_struct()) {
      const auto* sub_struct = flatbuffers::GetFieldStruct(*table, field);
      if (!sub_struct) {
        return false;
      }

      out.kind = FlatbuffersViewKind::kStruct;
      out.data = sub_struct;
      out.object = obj;
      return true;
    }

    const auto* sub_table = flatbuffers::GetFieldT(*table, field);
    if (!sub_table) {
      return false;
    }

    out.kind = FlatbuffersViewKind::kTable;
    out.data = sub_table;
    out.object = obj;
    return true;
  }

  if (!parent.object || !parent.object->is_struct() || !obj->is_struct()) {
    return false;
  }

  const auto* sub_struct = flatbuffers::GetFieldStruct(*as_struct(parent), field);

  if (!sub_struct) {
    return false;
  }

  out.kind = FlatbuffersViewKind::kStruct;
  out.data = sub_struct;
  out.object = obj;
  return true;
}

bool FlatbuffersObjectView::get_vector_elem_view(const FlatbuffersObjectView& parent, const reflection::Field& field,
                                                 size_t index, const reflection::Schema& schema,
                                                 FlatbuffersObjectView& out) {
  out = {};

  if (!parent.valid() || parent.kind != FlatbuffersViewKind::kTable ||
      (field.type()->base_type() != reflection::Vector && field.type()->base_type() != reflection::Vector64) ||
      field.type()->element() != reflection::Obj || !schema.objects()) {
    return false;
  }

  const auto* obj = schema.objects()->Get(static_cast<uint32_t>(field.type()->index()));
  const auto* vec = flatbuffers::GetFieldAnyV(*as_table(parent), field);

  if (!obj || !vec || index >= vec->size()) {
    return false;
  }

  if (obj->is_struct()) {
    const auto* sub_struct =
        flatbuffers::GetAnyVectorElemAddressOf<const flatbuffers::Struct>(vec, index, obj->bytesize());

    if (!sub_struct) {
      return false;
    }

    out.kind = FlatbuffersViewKind::kStruct;
    out.data = sub_struct;
    out.object = obj;
    return true;
  }

  const auto* sub_table = flatbuffers::GetAnyVectorElemPointer<const flatbuffers::Table>(vec, index);

  if (!sub_table) {
    return false;
  }

  out.kind = FlatbuffersViewKind::kTable;
  out.data = sub_table;
  out.object = obj;
  return true;
}

size_t FlatbuffersObjectView::get_vector_size(const FlatbuffersObjectView& parent, const reflection::Field& field) {
  if (!parent.valid() || parent.kind != FlatbuffersViewKind::kTable ||
      (field.type()->base_type() != reflection::Vector && field.type()->base_type() != reflection::Vector64)) {
    return 0;
  }

  const auto* vec = flatbuffers::GetFieldAnyV(*as_table(parent), field);
  return vec ? vec->size() : 0;
}

std::optional<double> FlatbuffersObjectView::get_numeric(const FlatbuffersObjectView& parent,
                                                         const reflection::Field& field) {
  if (!parent.valid() || !is_numeric_type(field.type()->base_type())) {
    return std::nullopt;
  }

  if (parent.kind == FlatbuffersViewKind::kTable) {
    return flatbuffers::GetAnyFieldF(*as_table(parent), field);
  }

  return flatbuffers::GetAnyFieldF(*as_struct(parent), field);
}

std::string FlatbuffersObjectView::get_string(const FlatbuffersObjectView& parent, const reflection::Field& field,
                                              const reflection::Schema* schema) {
  if (!parent.valid()) {
    return {};
  }

  if (parent.kind == FlatbuffersViewKind::kTable) {
    return flatbuffers::GetAnyFieldS(*as_table(parent), field, schema);
  }

  return flatbuffers::GetAnyFieldS(*as_struct(parent), field);
}

bool FlatbuffersObjectView::get_bytes(const FlatbuffersObjectView& parent, const reflection::Field& field,
                                      vlink::Bytes& out) {
  out.clear();

  if (!parent.valid() || parent.kind != FlatbuffersViewKind::kTable) {
    return false;
  }

  if (field.type()->base_type() == reflection::String) {
    const auto* str = flatbuffers::GetFieldS(*as_table(parent), field);

    if (!str || str->size() == 0) {
      return false;
    }

    out = vlink::Bytes::shallow_copy(reinterpret_cast<const uint8_t*>(str->c_str()), str->size());
    return true;
  }

  if (field.type()->base_type() == reflection::Vector &&
      (field.type()->element() == reflection::UByte || field.type()->element() == reflection::Byte)) {
    const auto* vec = flatbuffers::GetFieldV<uint8_t>(*as_table(parent), field);

    if (!vec || vec->size() == 0) {
      return false;
    }

    out = vlink::Bytes::shallow_copy(vec->data(), vec->size());
    return true;
  }

  return false;
}

bool FlatbuffersObjectView::is_bytes_field(const reflection::Field& field) {
  return field.type()->base_type() == reflection::String ||
         ((field.type()->base_type() == reflection::Vector || field.type()->base_type() == reflection::Vector64) &&
          (field.type()->element() == reflection::Byte || field.type()->element() == reflection::UByte));
}

std::optional<double> get_vector_numeric(const FlatbuffersObjectView& parent, const reflection::Field& field,
                                         size_t index) {
  if (!parent.valid() || parent.kind != FlatbuffersViewKind::kTable ||
      (field.type()->base_type() != reflection::Vector && field.type()->base_type() != reflection::Vector64) ||
      !is_numeric_type(field.type()->element())) {
    return std::nullopt;
  }

  const auto* vec = flatbuffers::GetFieldAnyV(*as_table(parent), field);

  if (!vec || index >= vec->size()) {
    return std::nullopt;
  }

  return flatbuffers::GetAnyVectorElemF(vec, field.type()->element(), index);
}

std::string FlatbuffersObjectView::get_vector_string(const FlatbuffersObjectView& parent,
                                                     const reflection::Field& field, size_t index) {
  if (!parent.valid() || parent.kind != FlatbuffersViewKind::kTable ||
      (field.type()->base_type() != reflection::Vector && field.type()->base_type() != reflection::Vector64) ||
      field.type()->element() != reflection::String) {
    return {};
  }

  const auto* vec = flatbuffers::GetFieldAnyV(*as_table(parent), field);

  if (!vec || index >= vec->size()) {
    return {};
  }

  return flatbuffers::GetAnyVectorElemS(vec, field.type()->element(), index);
}

bool FlatbuffersObjectView::is_enum_field(const reflection::Field& field, const reflection::Schema& schema) {
  const auto base_type = field.type()->base_type();

  if (base_type == reflection::Obj) {
    return false;
  }

  if ((base_type == reflection::Vector || base_type == reflection::Vector64) &&
      field.type()->element() == reflection::Obj) {
    return false;
  }

  return field.type()->index() >= 0 && schema.enums() &&
         static_cast<flatbuffers::uoffset_t>(field.type()->index()) < schema.enums()->size();
}

std::string FlatbuffersObjectView::get_enum_value_name(const reflection::Field& field, const reflection::Schema& schema,
                                                       int64_t value) {
  if (!is_enum_field(field, schema)) {
    return {};
  }

  const auto* enum_def = schema.enums()->Get(static_cast<flatbuffers::uoffset_t>(field.type()->index()));

  if (!enum_def || !enum_def->values()) {
    return {};
  }

  for (flatbuffers::uoffset_t i = 0; i < enum_def->values()->size(); ++i) {
    const auto* ev = enum_def->values()->Get(i);

    if (ev && ev->value() == value && ev->name()) {
      return ev->name()->str();
    }
  }

  return {};
}

int FlatbuffersObjectView::get_fbs_edit_type(reflection::BaseType base_type) {
  switch (base_type) {
    case reflection::Bool:
      return 7;
    case reflection::Byte:
    case reflection::Short:
    case reflection::Int:
      return 1;
    case reflection::Long:
      return 2;
    case reflection::UByte:
    case reflection::UShort:
    case reflection::UInt:
      return 3;
    case reflection::ULong:
      return 4;
    case reflection::Float:
      return 6;
    case reflection::Double:
      return 5;
    case reflection::String:
      return 9;
    default:
      return 0;
  }
}

bool FlatbuffersObjectView::split_indexed_token(const std::string& token, std::string& name, int& index) {
  name = token;
  index = -1;

  const auto pos_left = token.find('[');
  const auto pos_right = token.find(']');

  if (pos_left == std::string::npos || pos_right == std::string::npos || pos_right <= pos_left) {
    return false;
  }

  name = token.substr(0, pos_left);

  try {
    index = std::stoi(token.substr(pos_left + 1, pos_right - pos_left - 1));
  } catch (const std::exception&) {
    index = -1;
    return false;
  }

  return true;
}

#endif

// NOLINTEND
