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

#include "./eproto_common.h"

#ifdef VLINK_HAS_PROTOBUF_COMPILER

class CustomFieldValuePrinter final : public google::protobuf::TextFormat::FastFieldValuePrinter {
  void PrintBytes(const std::string& val, google::protobuf::TextFormat::BaseTextGenerator* generator) const override {
    (void)val;
    generator->PrintString("{...}");
  }

  void PrintString(const std::string& val, google::protobuf::TextFormat::BaseTextGenerator* generator) const override {
    FastFieldValuePrinter::PrintString(val, generator);
  }

  void PrintFieldName(const google::protobuf::Message& message, int field_index, int field_count,
                      const google::protobuf::Reflection* reflection, const google::protobuf::FieldDescriptor* field,
                      google::protobuf::TextFormat::BaseTextGenerator* generator) const override {
    FastFieldValuePrinter::PrintFieldName(message, field_index, field_count, reflection, field, generator);

    if (field->is_repeated() && field_index >= 0) {
      generator->PrintString("[" + std::to_string(field_index) + "]");
    }
  }

  void PrintFieldName(const google::protobuf::Message& message, const google::protobuf::Reflection* reflection,
                      const google::protobuf::FieldDescriptor* field,
                      google::protobuf::TextFormat::BaseTextGenerator* generator) const override {
    current_field_ = const_cast<google::protobuf::FieldDescriptor*>(field);

    FastFieldValuePrinter::PrintFieldName(message, reflection, field, generator);
  }

  void PrintMessageStart(const google::protobuf::Message& message, int field_index, int field_count,
                         bool single_line_mode,
                         google::protobuf::TextFormat::BaseTextGenerator* generator) const override {
    FastFieldValuePrinter::PrintMessageStart(message, field_index, field_count, single_line_mode, generator);
  }
#if GOOGLE_PROTOBUF_VERSION >= 3012000
  bool PrintMessageContent(const google::protobuf::Message& message, int field_index, int field_count,
                           bool single_line_mode,
                           google::protobuf::TextFormat::BaseTextGenerator* generator) const override {
    return FastFieldValuePrinter::PrintMessageContent(message, field_index, field_count, single_line_mode, generator);
  }
#endif

  void PrintMessageEnd(const google::protobuf::Message& message, int field_index, int field_count,
                       bool single_line_mode,
                       google::protobuf::TextFormat::BaseTextGenerator* generator) const override {
    FastFieldValuePrinter::PrintMessageEnd(message, field_index, field_count, single_line_mode, generator);
  }

  void PrintEnum(int32_t val, const std::string& name,
                 google::protobuf::TextFormat::BaseTextGenerator* generator) const override {
    if (print_enum_string) {
      FastFieldValuePrinter::PrintEnum(val, name, generator);
    } else {
      generator->PrintString(std::to_string(val));
    }
  }

  void PrintInt32(int32_t val, google::protobuf::TextFormat::BaseTextGenerator* generator) const override {
    if (print_time_string && current_field_) {
      if (print_hex_string) {
        generator->PrintString(vlink::Helpers::format_hex_number(static_cast<int64_t>(val)));
      } else {
        FastFieldValuePrinter::PrintInt32(val, generator);
      }
    } else {
      if (print_hex_string) {
        generator->PrintString(vlink::Helpers::format_hex_number(static_cast<int64_t>(val)));
      } else {
        FastFieldValuePrinter::PrintInt32(val, generator);
      }
    }
  }

  void PrintUInt32(uint32_t val, google::protobuf::TextFormat::BaseTextGenerator* generator) const override {
    if (print_hex_string) {
      generator->PrintString(vlink::Helpers::format_hex_number(static_cast<int64_t>(val)));
    } else {
      FastFieldValuePrinter::PrintUInt32(val, generator);
    }
  }

  void PrintInt64(int64_t val, google::protobuf::TextFormat::BaseTextGenerator* generator) const override {
    if (print_time_string && current_field_) {
      if (current_field_->name().find("time") != std::string::npos) {
        generator->PrintString(vlink::Helpers::format_date(val));
      } else {
        if (print_hex_string) {
          generator->PrintString(vlink::Helpers::format_hex_number(val));
        } else {
          FastFieldValuePrinter::PrintInt64(val, generator);
        }
      }
    } else {
      if (print_hex_string) {
        generator->PrintString(vlink::Helpers::format_hex_number(val));
      } else {
        FastFieldValuePrinter::PrintInt64(val, generator);
      }
    }
  }

  void PrintUInt64(uint64_t val, google::protobuf::TextFormat::BaseTextGenerator* generator) const override {
    if (print_time_string && current_field_) {
      if (current_field_->name().find("time") != std::string::npos) {
        generator->PrintString(vlink::Helpers::format_date(val));
      } else {
        if (print_hex_string) {
          generator->PrintString(vlink::Helpers::format_hex_number(val));
        } else {
          FastFieldValuePrinter::PrintUInt64(val, generator);
        }
      }
    } else {
      if (print_hex_string) {
        generator->PrintString(vlink::Helpers::format_hex_number(val));
      } else {
        FastFieldValuePrinter::PrintUInt64(val, generator);
      }
    }
  }

  void PrintFloat(float val, google::protobuf::TextFormat::BaseTextGenerator* generator) const override {
    FastFieldValuePrinter::PrintFloat(val, generator);
  }

  void PrintDouble(double val, google::protobuf::TextFormat::BaseTextGenerator* generator) const override {
    FastFieldValuePrinter::PrintDouble(val, generator);
  }

  void PrintBool(bool val, google::protobuf::TextFormat::BaseTextGenerator* generator) const override {
    FastFieldValuePrinter::PrintBool(val, generator);
  }

 private:
  mutable google::protobuf::FieldDescriptor* current_field_{nullptr};
};

#if GOOGLE_PROTOBUF_VERSION >= 3006000

[[maybe_unused]] static bool is_no_presence_default_scalar(const google::protobuf::Message& message,
                                                           const google::protobuf::FieldDescriptor* field) {
  if VUNLIKELY (field == nullptr || field->is_repeated() ||
                field->cpp_type() == google::protobuf::FieldDescriptor::CPPTYPE_MESSAGE) {
    return false;
  }

#if GOOGLE_PROTOBUF_VERSION >= 3012000
  if (field->has_presence()) {
    return false;
  }
#else
  if (field->is_extension() || field->containing_oneof() != nullptr ||
      field->file()->syntax() != google::protobuf::FileDescriptor::SYNTAX_PROTO3) {
    return false;
  }
#endif

  if (field->type() == google::protobuf::FieldDescriptor::TYPE_BYTES ||
      (ignore_string && field->type() == google::protobuf::FieldDescriptor::TYPE_STRING)) {
    return false;
  }

  const auto* reflection = message.GetReflection();

  if (reflection->HasField(message, field)) {
    return false;
  }

  switch (field->cpp_type()) {
    case google::protobuf::FieldDescriptor::CPPTYPE_INT32:
      return reflection->GetInt32(message, field) == field->default_value_int32();
    case google::protobuf::FieldDescriptor::CPPTYPE_INT64:
      return reflection->GetInt64(message, field) == field->default_value_int64();
    case google::protobuf::FieldDescriptor::CPPTYPE_UINT32:
      return reflection->GetUInt32(message, field) == field->default_value_uint32();
    case google::protobuf::FieldDescriptor::CPPTYPE_UINT64:
      return reflection->GetUInt64(message, field) == field->default_value_uint64();
    case google::protobuf::FieldDescriptor::CPPTYPE_DOUBLE:
      return reflection->GetDouble(message, field) == field->default_value_double();
    case google::protobuf::FieldDescriptor::CPPTYPE_FLOAT:
      return reflection->GetFloat(message, field) == field->default_value_float();
    case google::protobuf::FieldDescriptor::CPPTYPE_BOOL:
      return reflection->GetBool(message, field) == field->default_value_bool();
    case google::protobuf::FieldDescriptor::CPPTYPE_STRING:
#if GOOGLE_PROTOBUF_VERSION >= 6030000
      return reflection->GetString(message, field) == std::string(field->default_value_string());
#else
      return reflection->GetString(message, field) == field->default_value_string();
#endif
    case google::protobuf::FieldDescriptor::CPPTYPE_ENUM:
      return reflection->GetEnum(message, field) == field->default_value_enum();
    case google::protobuf::FieldDescriptor::CPPTYPE_MESSAGE:
      return false;
  }

  return false;
}

[[maybe_unused]] static bool field_name_passes_filter(const google::protobuf::FieldDescriptor* field) {
  if (filter_list.empty()) {
    return true;
  }

  bool skip = black_mode ? false : true;

#if GOOGLE_PROTOBUF_VERSION >= 6030000
  std::string left_str = std::string(field->name());
#else
  std::string left_str = field->name();
#endif

  std::transform(left_str.begin(), left_str.end(), left_str.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

  for (const auto& f : filter_list) {
    if (f.empty()) {
      continue;
    }

    std::string right_str = f;
    std::transform(right_str.begin(), right_str.end(), right_str.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

    if (left_str.find(right_str) != std::string::npos) {
      skip = black_mode ? true : false;
      break;
    }
  }

  return !skip;
}

static void print_message_with_defaults(const google::protobuf::TextFormat::Printer* printer,
                                        const google::protobuf::TextFormat::FastFieldValuePrinter* name_printer,
                                        const google::protobuf::Message& message, bool single_line_mode,
                                        bool root_level, google::protobuf::TextFormat::BaseTextGenerator* generator);

[[maybe_unused]] static bool map_entry_key_less(const google::protobuf::FieldDescriptor* key_field,
                                                const google::protobuf::Message* lhs,
                                                const google::protobuf::Message* rhs) {
  const auto* lhs_reflection = lhs->GetReflection();
  const auto* rhs_reflection = rhs->GetReflection();

  switch (key_field->cpp_type()) {
    case google::protobuf::FieldDescriptor::CPPTYPE_BOOL:
      return !lhs_reflection->GetBool(*lhs, key_field) && rhs_reflection->GetBool(*rhs, key_field);
    case google::protobuf::FieldDescriptor::CPPTYPE_INT32:
      return lhs_reflection->GetInt32(*lhs, key_field) < rhs_reflection->GetInt32(*rhs, key_field);
    case google::protobuf::FieldDescriptor::CPPTYPE_INT64:
      return lhs_reflection->GetInt64(*lhs, key_field) < rhs_reflection->GetInt64(*rhs, key_field);
    case google::protobuf::FieldDescriptor::CPPTYPE_UINT32:
      return lhs_reflection->GetUInt32(*lhs, key_field) < rhs_reflection->GetUInt32(*rhs, key_field);
    case google::protobuf::FieldDescriptor::CPPTYPE_UINT64:
      return lhs_reflection->GetUInt64(*lhs, key_field) < rhs_reflection->GetUInt64(*rhs, key_field);
    case google::protobuf::FieldDescriptor::CPPTYPE_STRING:
      return lhs_reflection->GetString(*lhs, key_field) < rhs_reflection->GetString(*rhs, key_field);
    default:
      return false;
  }
}

[[maybe_unused]] static void collect_sorted_map_entries(const google::protobuf::Reflection* reflection,
                                                        const google::protobuf::Message& message,
                                                        const google::protobuf::FieldDescriptor* field,
                                                        std::vector<const google::protobuf::Message*>* entries) {
  const int size = reflection->FieldSize(message, field);

  entries->reserve(static_cast<size_t>(size));

  for (int j = 0; j < size; ++j) {
    entries->push_back(&reflection->GetRepeatedMessage(message, field, j));
  }

  const google::protobuf::FieldDescriptor* key_field = field->message_type()->field(0);

  std::stable_sort(entries->begin(), entries->end(),
                   [key_field](const google::protobuf::Message* lhs, const google::protobuf::Message* rhs) {
                     return map_entry_key_less(key_field, lhs, rhs);
                   });
}

[[maybe_unused]] static void print_one_field(const google::protobuf::TextFormat::Printer* printer,
                                             const google::protobuf::TextFormat::FastFieldValuePrinter* name_printer,
                                             const google::protobuf::Message& message,
                                             const google::protobuf::Reflection* reflection,
                                             const google::protobuf::FieldDescriptor* field, bool single_line_mode,
                                             google::protobuf::TextFormat::BaseTextGenerator* generator) {
  if (!use_long_repeated && field->is_repeated() &&
      field->cpp_type() != google::protobuf::FieldDescriptor::CPPTYPE_STRING &&
      field->cpp_type() != google::protobuf::FieldDescriptor::CPPTYPE_MESSAGE) {
    const int size = reflection->FieldSize(message, field);

    name_printer->PrintFieldName(message, -1, size, reflection, field, generator);
    generator->PrintLiteral(": [");

    for (int i = 0; i < size; ++i) {
      if (i > 0) {
        generator->PrintLiteral(", ");
      }

      std::string value;
      printer->PrintFieldValueToString(message, field, i, &value);
      generator->PrintString(value);
    }

    if (single_line_mode) {
      generator->PrintLiteral("] ");
    } else {
      generator->PrintLiteral("]\n");
    }

    return;
  }

  int count = 0;

  if (field->is_repeated()) {
    count = reflection->FieldSize(message, field);
  } else if (reflection->HasField(message, field) || field->containing_type()->options().map_entry()) {
    count = 1;
  }

  const bool is_map = field->is_map();
  std::vector<const google::protobuf::Message*> sorted_map_entries;

  if (is_map) {
    collect_sorted_map_entries(reflection, message, field, &sorted_map_entries);
  }

  for (int j = 0; j < count; ++j) {
    const int field_index = field->is_repeated() ? j : -1;

    name_printer->PrintFieldName(message, field_index, count, reflection, field, generator);

    if (field->cpp_type() == google::protobuf::FieldDescriptor::CPPTYPE_MESSAGE) {
      const google::protobuf::Message* sub_message = nullptr;

      if (!field->is_repeated()) {
        sub_message = &reflection->GetMessage(message, field);
      } else if (is_map) {
        sub_message = sorted_map_entries[j];
      } else {
        sub_message = &reflection->GetRepeatedMessage(message, field, j);
      }

      name_printer->PrintMessageStart(*sub_message, field_index, count, single_line_mode, generator);
      generator->Indent();
      print_message_with_defaults(printer, name_printer, *sub_message, single_line_mode, false, generator);
      generator->Outdent();
      name_printer->PrintMessageEnd(*sub_message, field_index, count, single_line_mode, generator);
    } else {
      std::string value;
      printer->PrintFieldValueToString(message, field, field_index, &value);

      generator->PrintLiteral(": ");
      generator->PrintString(value);

      if (single_line_mode) {
        generator->PrintLiteral(" ");
      } else {
        generator->PrintLiteral("\n");
      }
    }
  }
}

[[maybe_unused]] static void print_message_with_defaults(
    const google::protobuf::TextFormat::Printer* printer,
    const google::protobuf::TextFormat::FastFieldValuePrinter* name_printer, const google::protobuf::Message& message,
    bool single_line_mode, bool root_level, google::protobuf::TextFormat::BaseTextGenerator* generator) {
  const auto* descriptor = message.GetDescriptor();
  const auto* reflection = message.GetReflection();

  if VUNLIKELY (descriptor == nullptr || reflection == nullptr) {
    return;
  }

  if (descriptor->options().map_entry()) {
    print_one_field(printer, name_printer, message, reflection, descriptor->field(0), single_line_mode, generator);
    print_one_field(printer, name_printer, message, reflection, descriptor->field(1), single_line_mode, generator);

    return;
  }

  std::vector<const google::protobuf::FieldDescriptor*> fields;

  reflection->ListFields(message, &fields);

  if (!ignore_default && !single_line_mode) {
    for (int i = 0; i < descriptor->field_count(); ++i) {
      const auto* field = descriptor->field(i);

      if (!is_no_presence_default_scalar(message, field)) {
        continue;
      }

      if (root_level && !field_name_passes_filter(field)) {
        continue;
      }

      fields.push_back(field);
    }

    std::stable_sort(fields.begin(), fields.end(),
                     [](const google::protobuf::FieldDescriptor* lhs, const google::protobuf::FieldDescriptor* rhs) {
                       return lhs->number() < rhs->number();
                     });
  }

  for (const auto* field : fields) {
    if (is_no_presence_default_scalar(message, field)) {
      name_printer->PrintFieldName(message, -1, 1, reflection, field, generator);
      generator->PrintLiteral(": ");

      std::string value;
      printer->PrintFieldValueToString(message, field, -1, &value);
      generator->PrintString(value);
      generator->PrintLiteral("\n");
    } else {
      print_one_field(printer, name_printer, message, reflection, field, single_line_mode, generator);
    }
  }
}

class DefaultScalarMessagePrinter final : public google::protobuf::TextFormat::MessagePrinter {
 public:
  DefaultScalarMessagePrinter(const google::protobuf::TextFormat::Printer* printer,
                              const google::protobuf::TextFormat::FastFieldValuePrinter* name_printer)
      : printer_(printer), name_printer_(name_printer) {}

  void Print(const google::protobuf::Message& message, bool single_line_mode,
             google::protobuf::TextFormat::BaseTextGenerator* generator) const override {
    print_message_with_defaults(printer_, name_printer_, message, single_line_mode, true, generator);
  }

 private:
  const google::protobuf::TextFormat::Printer* printer_{nullptr};
  const google::protobuf::TextFormat::FastFieldValuePrinter* name_printer_{nullptr};
};

#endif  // GOOGLE_PROTOBUF_VERSION >= 3006000

[[maybe_unused]] bool load_proto_for_file(const std::string& filename, ::google::protobuf::Message* message) {
  if (message == nullptr) {
    return false;
  }

  std::ifstream file(filename);

  if (!file) {
    return false;
  }

  google::protobuf::io::IstreamInputStream input(&file);
  return google::protobuf::TextFormat::Parse(&input, message);
}

[[maybe_unused]] bool load_proto_for_string(const std::string& content, ::google::protobuf::Message* message) {
  if (message == nullptr) {
    return false;
  }

  return google::protobuf::TextFormat::ParseFromString(content, message);
}

[[maybe_unused]] bool convert_proto_to_txt(std::string& content, ::google::protobuf::Message* message) {
  if (message == nullptr) {
    return false;
  }

  google::protobuf::TextFormat::Printer printer;
  auto* value_printer = new CustomFieldValuePrinter;

  printer.SetDefaultFieldValuePrinter(value_printer);
  printer.SetHideUnknownFields(true);
  printer.SetUseShortRepeatedPrimitives(!use_long_repeated);

#if GOOGLE_PROTOBUF_VERSION >= 3006000
  printer.RegisterMessagePrinter(message->GetDescriptor(), new DefaultScalarMessagePrinter(&printer, value_printer));
#endif

  return printer.PrintToString(*message, &content);
}

#ifdef VLINK_HAS_PROTOBUF_JSON_UTIL
[[maybe_unused]] bool load_proto_for_json_string(const std::string& content, ::google::protobuf::Message* message,
                                                 std::string* error) {
  if (message == nullptr) {
    return false;
  }

  message->Clear();

  google::protobuf::util::JsonParseOptions options;
  auto status = google::protobuf::util::JsonStringToMessage(content, message, options);

  if (!status.ok()) {
    if (error != nullptr) {
      *error = std::string(status.ToString());
    }

    return false;
  }

  return true;
}

[[maybe_unused]] bool load_proto_for_json_file(const std::string& filename, ::google::protobuf::Message* message,
                                               std::string* error) {
  std::string content;

  if (!load_text_for_file(filename, content)) {
    if (error != nullptr) {
      *error = "Cannot open JSON file.";
    }

    return false;
  }

  return load_proto_for_json_string(content, message, error);
}

[[maybe_unused]] bool convert_proto_to_json(std::string& content, const ::google::protobuf::Message* message,
                                            std::string* error) {
  if (message == nullptr) {
    return false;
  }

  google::protobuf::util::JsonPrintOptions options;
  options.add_whitespace = true;
  options.always_print_enums_as_ints = !print_enum_string;
  options.preserve_proto_field_names = true;

  std::string json_output;
  auto status = google::protobuf::util::MessageToJsonString(*message, &json_output, options);

  if (!status.ok()) {
    if (error != nullptr) {
      *error = std::string(status.ToString());
    }

    return false;
  }

  content = std::move(json_output);
  return true;
}
#endif

[[maybe_unused]] void set_proto_value_to_default(google::protobuf::Message* message) {
  const google::protobuf::Descriptor* descriptor = message->GetDescriptor();
  const google::protobuf::Reflection* reflection = message->GetReflection();

  for (int i = 0; i < descriptor->field_count(); ++i) {
    const google::protobuf::FieldDescriptor* field = descriptor->field(i);

    if (field->is_map()) {
      continue;
    }

    if (field->is_repeated()) {
      int count = reflection->FieldSize(*message, field);

      for (int j = 0; j < count; ++j) {
        if (field->cpp_type() == google::protobuf::FieldDescriptor::CPPTYPE_MESSAGE) {
          set_proto_value_to_default(reflection->MutableRepeatedMessage(message, field, j));
        }
      }

      continue;
    }

    if (field->containing_oneof()) {
      continue;
    }

    if (reflection->HasField(*message, field) &&
        field->cpp_type() != google::protobuf::FieldDescriptor::CPPTYPE_MESSAGE) {
      continue;
    }

    switch (field->cpp_type()) {
      case google::protobuf::FieldDescriptor::CPPTYPE_INT32:
        reflection->SetInt32(message, field, field->default_value_int32());
        break;
      case google::protobuf::FieldDescriptor::CPPTYPE_INT64:
        reflection->SetInt64(message, field, field->default_value_int64());
        break;
      case google::protobuf::FieldDescriptor::CPPTYPE_UINT32:
        reflection->SetUInt32(message, field, field->default_value_uint32());
        break;
      case google::protobuf::FieldDescriptor::CPPTYPE_UINT64:
        reflection->SetUInt64(message, field, field->default_value_uint64());
        break;
      case google::protobuf::FieldDescriptor::CPPTYPE_DOUBLE:
        reflection->SetDouble(message, field, field->default_value_double());
        break;
      case google::protobuf::FieldDescriptor::CPPTYPE_FLOAT:
        reflection->SetFloat(message, field, field->default_value_float());
        break;
      case google::protobuf::FieldDescriptor::CPPTYPE_BOOL:
        reflection->SetBool(message, field, field->default_value_bool());
        break;
      case google::protobuf::FieldDescriptor::CPPTYPE_STRING:
#if GOOGLE_PROTOBUF_VERSION >= 6030000
        reflection->SetString(message, field, std::string(field->default_value_string()));
#else
        reflection->SetString(message, field, field->default_value_string());
#endif
        break;
      case google::protobuf::FieldDescriptor::CPPTYPE_ENUM:
        reflection->SetEnum(message, field, field->default_value_enum());
        break;
      case google::protobuf::FieldDescriptor::CPPTYPE_MESSAGE:
        if (reflection->HasField(*message, field)) {
          set_proto_value_to_default(reflection->MutableMessage(message, field));
        }

        break;
    }
  }
}

#endif
