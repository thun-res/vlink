/*
 * Copyright 2014 Google Inc. All rights reserved.
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

/**
 * @file flat_gen_text.h
 * @brief Private FlatBuffers text-generation helper carrying generated constants and JSON printer.
 *
 * @details
 * This is a private, non-installed header.  It vendors a customised text generator from the
 * upstream FlatBuffers project (the @c flatbuffers::custom::JsonPrinter class and the
 * @c GenText entry point) so that VLink's bag-replay tooling can render FlatBuffers payloads
 * to JSON with VLink-specific extensions:
 *
 * | Extension flag             | Effect                                                           |
 * | -------------------------- | ---------------------------------------------------------------- |
 * | @c ignore_array            | Skip vector / array fields entirely                              |
 * | @c ignore_string           | Skip string fields entirely                                      |
 * | @c print_time_string       | Render @c long / @c ulong fields named "time" as date strings    |
 * | @c print_hex_string        | Render integer fields as hexadecimal                             |
 * | @c print_enum_string       | Resolve enum values to identifiers                               |
 * | @c filter_list             | Whitelist/blacklist of struct names (paired with @c black_mode)  |
 *
 * The upstream Google copyright header above is preserved verbatim per third-party-code
 * policy.  The entire body is wrapped in @c NOLINTBEGIN / @c NOLINTEND because the vendored
 * identifiers intentionally follow upstream FlatBuffers naming conventions.
 */

// NOLINTBEGIN

#pragma once

#if __has_include(<flatbuffers/base.h>)
#include <flatbuffers/base.h>
#include <flatbuffers/flexbuffers.h>
#include <flatbuffers/idl.h>
#include <flatbuffers/util.h>

#include <algorithm>

//
#include <vlink/base/helpers.h>
//

namespace flatbuffers {

namespace custom {

struct PrintScalarTag {};
struct PrintPointerTag {};
template <typename T>
struct PrintTag {
  typedef PrintScalarTag type;
};
template <>
struct PrintTag<const void*> {
  typedef PrintPointerTag type;
};

struct JsonPrinter {
  inline static bool ignore_array{false};
  inline static bool ignore_string{false};
  inline static bool ignore_default{false};
  inline static bool use_long_repeated{false};
  inline static bool print_time_string{false};
  inline static bool print_hex_string{false};
  inline static bool print_enum_string{false};
  inline static bool black_mode{false};
  inline static std::vector<std::string>* filter_list{nullptr};

  void AddNewLine() {
    if (opts.indent_step >= 0) {
      text += '\n';
    }
  }

  void AddIndent(int ident) { text.append(ident, ' '); }

  int Indent() const { return std::max(opts.indent_step, 0); }

  void OutputIdentifier(const std::string& name) {
    if (opts.strict_json) {
      text += '\"';
    }

    text += name;

    if (opts.strict_json) {
      text += '\"';
    }
  }

  template <typename T>
  void PrintScalar(T val, const Type& type, int, const FieldDef* fd = nullptr) {
    if (IsBool(type.base_type)) {
      text += val != 0 ? "true" : "false";
      return;
    }

    if (opts.output_enum_identifiers && type.enum_def) {
      const auto& enum_def = *type.enum_def;
      if (auto ev = enum_def.ReverseLookup(static_cast<int64_t>(val))) {
        text += '\"';
        text += ev->name;
        text += '\"';
        return;
      } else if (val && enum_def.attributes.Lookup("bit_flags")) {
        const auto entry_len = text.length();
        const auto u64 = static_cast<uint64_t>(val);
        uint64_t mask = 0;
        text += '\"';
        for (auto it = enum_def.Vals().begin(), e = enum_def.Vals().end(); it != e; ++it) {
          auto f = (*it)->GetAsUInt64();
          if (f & u64) {
            mask |= f;
            text += (*it)->name;
            text += ' ';
          }
        }
        if (mask && (u64 == mask)) {
          text[text.length() - 1] = '\"';
          return;
        }
        text.resize(entry_len);
      }
    }

    if (IsInteger(type.base_type)) {
      if (print_time_string && (type.base_type == BASE_TYPE_LONG || type.base_type == BASE_TYPE_ULONG) && fd &&
          (fd->name.find("time") != std::string::npos)) {
        text += vlink::Helpers::format_date(val);
      } else {
        if (print_hex_string) {
          text += vlink::Helpers::format_hex_number(static_cast<int64_t>(val));
        } else {
          text += NumToString(val);
        }
      }
    } else {
      text += NumToString(val);
    }

    return;
  }

  void AddComma() {
    if (!opts.protobuf_ascii_alike) {
      text += ',';
    }
  }

  template <typename Container, typename SizeT = typename Container::size_type>
  const char* PrintContainer(PrintScalarTag, const Container& c, SizeT size, const Type& type, int indent,
                             const uint8_t*) {
    const auto elem_indent = indent + Indent();
    text += '[';
    AddNewLine();
    for (SizeT i = 0; i < size; i++) {
      if (i) {
        AddComma();
        AddNewLine();
      }

      AddIndent(elem_indent);
      PrintScalar(c[i], type, elem_indent);
    }
    AddNewLine();
    AddIndent(indent);
    text += ']';
    return nullptr;
  }

  template <typename Container, typename SizeT = typename Container::size_type>
  const char* PrintContainer(PrintPointerTag, const Container& c, SizeT size, const Type& type, int indent,
                             const uint8_t* prev_val) {
    const auto is_struct = IsStruct(type);
    const auto elem_indent = indent + Indent();
    text += '[';
    AddNewLine();
    for (SizeT i = 0; i < size; i++) {
      if (i) {
        AddComma();
        AddNewLine();
      }

      AddIndent(elem_indent);
      auto ptr = is_struct ? reinterpret_cast<const void*>(c.Data() + type.struct_def->bytesize * i) : c[i];
      auto err = PrintOffset(ptr, type, elem_indent, prev_val, static_cast<soffset_t>(i));

      if (err) {
        return err;
      }
    }
    AddNewLine();
    AddIndent(indent);
    text += ']';
    return nullptr;
  }

  template <typename T, typename SizeT = uoffset_t>
  const char* PrintVector(const void* val, const Type& type, int indent, const uint8_t* prev_val) {
    typedef Vector<T, SizeT> Container;
    typedef typename PrintTag<typename Container::return_type>::type tag;
    auto& vec = *reinterpret_cast<const Container*>(val);
    return PrintContainer<Container>(tag(), vec, vec.size(), type, indent, prev_val);
  }

  template <typename T>
  const char* PrintArray(const void* val, uint16_t size, const Type& type, int indent) {
    typedef Array<T, 0xFFFF> Container;
    typedef typename PrintTag<typename Container::return_type>::type tag;
    auto& arr = *reinterpret_cast<const Container*>(val);
    return PrintContainer<Container>(tag(), arr, size, type, indent, nullptr);
  }

  const char* PrintOffset(const void* val, const Type& type, int indent, const uint8_t* prev_val,
                          soffset_t vector_index) {
    switch (type.base_type) {
      case BASE_TYPE_UNION: {
        FLATBUFFERS_ASSERT(prev_val);
        auto union_type_byte = *prev_val;

        if (vector_index >= 0) {
          auto type_vec = reinterpret_cast<const Vector<uint8_t>*>(prev_val + ReadScalar<uoffset_t>(prev_val));
          union_type_byte = type_vec->Get(static_cast<uoffset_t>(vector_index));
        }

        auto enum_val = type.enum_def->ReverseLookup(union_type_byte, true);

        if (enum_val) {
          return PrintOffset(val, enum_val->union_type, indent, nullptr, -1);
        } else {
          return "unknown enum value";
        }
      }
      case BASE_TYPE_STRUCT:
        return GenStruct(*type.struct_def, reinterpret_cast<const Table*>(val), indent);
      case BASE_TYPE_STRING: {
        if (ignore_string) {
          return nullptr;
        }

        auto s = reinterpret_cast<const String*>(val);
        bool ok = EscapeString(s->c_str(), s->size(), &text, opts.allow_non_utf8, opts.natural_utf8);
        return ok ? nullptr : "string contains non-utf8 bytes";
      }
      case BASE_TYPE_VECTOR: {
        if (ignore_array) {
          return nullptr;
        }

        const auto vec_type = type.VectorType();
        switch (vec_type.base_type) {
#define FLATBUFFERS_TD(ENUM, IDLTYPE, CTYPE, ...)                   \
  case BASE_TYPE_##ENUM: {                                          \
    auto err = PrintVector<CTYPE>(val, vec_type, indent, prev_val); \
    if (err) {                                                      \
      return err;                                                   \
    }                                                               \
    break;                                                          \
  }
          FLATBUFFERS_GEN_TYPES(FLATBUFFERS_TD)
#undef FLATBUFFERS_TD
        }
        return nullptr;
      }
      case BASE_TYPE_ARRAY: {
        if (ignore_array) {
          return nullptr;
        }

        const auto vec_type = type.VectorType();
        switch (vec_type.base_type) {
#define FLATBUFFERS_TD(ENUM, IDLTYPE, CTYPE, ...)                           \
  case BASE_TYPE_##ENUM: {                                                  \
    auto err = PrintArray<CTYPE>(val, type.fixed_length, vec_type, indent); \
    if (err) {                                                              \
      return err;                                                           \
    }                                                                       \
    break;                                                                  \
  }
          FLATBUFFERS_GEN_TYPES_SCALAR(FLATBUFFERS_TD)
          FLATBUFFERS_GEN_TYPES_POINTER(FLATBUFFERS_TD)
#undef FLATBUFFERS_TD
          case BASE_TYPE_ARRAY:
            FLATBUFFERS_ASSERT(0);
        }
        return nullptr;
      }
      default:
        FLATBUFFERS_ASSERT(0);
        return "unknown type";
    }
  }

  template <typename T>
  static T GetFieldDefault(const FieldDef& fd) {
    T val{};
    auto check = StringToNumber(fd.value.constant.c_str(), &val);
    (void)check;
    FLATBUFFERS_ASSERT(check);
    return val;
  }

  template <typename T>
  void GenField(const FieldDef& fd, const Table* table, bool fixed, int indent) {
    if (fixed) {
      PrintScalar(reinterpret_cast<const Struct*>(table)->GetField<T>(fd.value.offset), fd.value.type, indent, &fd);
    } else if (fd.IsOptional()) {
      auto opt = table->GetOptional<T, T>(fd.value.offset);
      if (opt) {
        PrintScalar(*opt, fd.value.type, indent, &fd);
      } else {
        text += "null";
      }
    } else {
      PrintScalar(table->GetField<T>(fd.value.offset, GetFieldDefault<T>(fd)), fd.value.type, indent, &fd);
    }
  }

  const char* GenFieldOffset(const FieldDef& fd, const Table* table, bool fixed, int indent, const uint8_t* prev_val) {
    const void* val = nullptr;

    if (fixed) {
      FLATBUFFERS_ASSERT(IsStruct(fd.value.type) || IsArray(fd.value.type));
      val = reinterpret_cast<const Struct*>(table)->GetStruct<const void*>(fd.value.offset);
    } else if (fd.flexbuffer && opts.json_nested_flexbuffers) {
      auto vec = table->GetPointer<const Vector<uint8_t>*>(fd.value.offset);
      auto root = flexbuffers::GetRoot(vec->data(), vec->size());
      root.ToString(true, opts.strict_json, text);
      return nullptr;
    } else if (fd.nested_flatbuffer && opts.json_nested_flatbuffers) {
      auto vec = table->GetPointer<const Vector<uint8_t>*>(fd.value.offset);
      auto root = GetRoot<Table>(vec->data());
      return GenStruct(*fd.nested_flatbuffer, root, indent);
    } else {
      val = IsStruct(fd.value.type) ? table->GetStruct<const void*>(fd.value.offset)
                                    : table->GetPointer<const void*>(fd.value.offset);
    }

    return PrintOffset(val, fd.value.type, indent, prev_val, -1);
  }

  const char* GenStruct(const StructDef& struct_def, const Table* table, int indent) {
    if (indent > 0 && filter_list && !filter_list->empty()) {
      bool skip = black_mode ? false : true;

      std::string left_str = struct_def.name;
      std::transform(left_str.begin(), left_str.end(), left_str.begin(), [](char& c) { return std::tolower(c); });
      for (const auto& f : *filter_list) {
        if (f.empty()) {
          continue;
        }

        std::string right_str = f;
        std::transform(right_str.begin(), right_str.end(), right_str.begin(), [](char& c) { return std::tolower(c); });

        if (left_str.find(right_str) != std::string::npos) {
          skip = black_mode ? true : false;
          break;
        }
      }

      if (skip) {
        return nullptr;
      }
    }

    text += '{';
    int fieldout = 0;
    const uint8_t* prev_val = nullptr;
    const auto elem_indent = indent + Indent();
    for (auto it = struct_def.fields.vec.begin(); it != struct_def.fields.vec.end(); ++it) {
      FieldDef& fd = **it;
      auto is_present = struct_def.fixed || table->CheckField(fd.value.offset);
      auto output_anyway =
          (opts.output_default_scalars_in_json || fd.key) && IsScalar(fd.value.type.base_type) && !fd.deprecated;

      if (is_present || output_anyway) {
        if (fd.value.type.base_type != BASE_TYPE_STRUCT && indent <= 0 && filter_list && !filter_list->empty()) {
          continue;
        }

        if (fieldout++) {
          AddComma();
        }

        AddNewLine();
        AddIndent(elem_indent);
        OutputIdentifier(fd.name);

        if (!opts.protobuf_ascii_alike ||
            (fd.value.type.base_type != BASE_TYPE_STRUCT && fd.value.type.base_type != BASE_TYPE_VECTOR))
          text += ':';
        text += ' ';
        switch (fd.value.type.base_type) {
#define FLATBUFFERS_TD(ENUM, IDLTYPE, CTYPE, ...)              \
  case BASE_TYPE_##ENUM: {                                     \
    GenField<CTYPE>(fd, table, struct_def.fixed, elem_indent); \
    break;                                                     \
  }
          FLATBUFFERS_GEN_TYPES_SCALAR(FLATBUFFERS_TD)
#undef FLATBUFFERS_TD
#define FLATBUFFERS_TD(ENUM, ...) case BASE_TYPE_##ENUM:
          FLATBUFFERS_GEN_TYPES_POINTER(FLATBUFFERS_TD)
          FLATBUFFERS_GEN_TYPE_ARRAY(FLATBUFFERS_TD)
#undef FLATBUFFERS_TD
          {
            auto err = GenFieldOffset(fd, table, struct_def.fixed, elem_indent, prev_val);

            if (err) {
              return err;
            }

            break;
          }
        }

        if (struct_def.fixed) {
          prev_val = reinterpret_cast<const uint8_t*>(table) + fd.value.offset;
        } else {
          prev_val = table->GetAddressOf(fd.value.offset);
        }
      }
    }
    AddNewLine();
    AddIndent(indent);
    text += '}';
    return nullptr;
  }

  JsonPrinter(const Parser& parser, std::string& dest) : opts(parser.opts), text(dest) { text.reserve(1024); }

  const IDLOptions& opts;
  std::string& text;
};

static const char* GenerateTextImpl(const Parser& parser, const Table* table, const StructDef& struct_def,
                                    std::string* _text) {
  JsonPrinter printer(parser, *_text);
  auto err = printer.GenStruct(struct_def, table, 0);

  if (err) {
    return err;
  }

  printer.AddNewLine();
  return nullptr;
}

const char* GenText(const Parser& parser, const void* flatbuffer, std::string* _text) {
  FLATBUFFERS_ASSERT(parser.root_struct_def_);
  auto root = parser.opts.size_prefixed ? GetSizePrefixedRoot<Table>(flatbuffer) : GetRoot<Table>(flatbuffer);

  if (!root) {
    return nullptr;
  }

  return flatbuffers::custom::GenerateTextImpl(parser, root, *parser.root_struct_def_, _text);
}

}  // namespace custom

}  // namespace flatbuffers

#endif

// NOLINTEND
