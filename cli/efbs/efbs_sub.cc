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

#include "./efbs_common.h"
#include "private/flat_gen_text.h"

class ParserLoop : public vlink::MessageLoop {
 public:
  ParserLoop() {
    set_name("ParserLoop");
    set_strategy(vlink::MessageLoop::kPopStrategy);
  }

 protected:
  size_t get_max_task_count() const override { return kMaxTaskSize; }

  uint32_t get_max_elapsed_time() const override { return kMaxElapsedTime; }
};

// NOLINTNEXTLINE(google-readability-function-size)
int start_efbs_sub(const std::string& url, const std::string& fbs_dir, const std::string& ser,
                   vlink::SchemaType schema_type, bool use_blob_encoding, bool native_mode, const std::string& filter,
                   bool use_getter) {
  const std::string native_ip = native_mode ? vlink::Utils::get_env("VLINK_DDS_NATIVE_IP", "127.0.0.1") : std::string();

  if VUNLIKELY (!has_intra_bind && vlink::Url::is_intra_type(url)) {
    std::cerr << "Cannot sub intra url." << std::endl;
    has_quit = true;
    return -1;
  }

  std::shared_ptr<vlink::DiscoveryViewer> discovery_viewer;
  std::shared_ptr<ParserLoop> parser_loop;
  std::shared_ptr<flatbuffers::Parser> parser;
  const reflection::Schema* fbs_schema = nullptr;
  const reflection::Object* fbs_root_object = nullptr;
  std::optional<std::shared_ptr<RawSub>> raw_sub;
  std::optional<std::shared_ptr<RawGetter>> raw_getter;

  if VUNLIKELY (url.empty()) {
    std::cerr << "Url is empty." << std::endl;
    has_quit = true;
    return -1;
  }

  bool has_explicit_ser = !ser.empty();
  std::string target_ser = ser;

  if (target_ser.empty()) {
    try {
      vlink::DiscoveryViewer::FilterType filter_type = vlink::DiscoveryViewer::kFilterAvailable;

      if (native_mode) {
        filter_type = vlink::DiscoveryViewer::kFilterNative;
      }

      discovery_viewer = std::make_shared<vlink::DiscoveryViewer>(filter_type);
    } catch (vlink::Exception::RuntimeError& e) {
      std::cerr << e.what() << std::endl;
      has_quit = true;
      return -1;
    }

    discovery_viewer->async_run();
  }

  parser_loop = std::make_shared<ParserLoop>();
  parser_loop->async_run();

  auto quit_function = [&discovery_viewer, &parser_loop](int) {
    if VUNLIKELY (has_quit) {
      return;
    }

    has_quit = true;

    if (discovery_viewer) {
      discovery_viewer->quit(true);
    }

    parser_loop->quit(true);
  };

  vlink::Utils::register_terminate_signal(quit_function);
  uint32_t target_type = 0;
  auto target_schema_type = schema_type;

  if (target_ser.empty()) {
    VLINK_TERM_OUT << "Information Collecting, Please Wait...";
    VLINK_TERM_OUT.flush();

    discovery_viewer->wait_for_quit(kCollectInterval);

    VLINK_TERM_OUT << "\033[2K\r";
    VLINK_TERM_OUT.flush();

    target_ser = discovery_viewer->get_ser_type(url);

    for (const auto& info : discovery_viewer->get_info_list()) {
      if (info.url == url) {
        target_type = info.type;
        break;
      }
    }

    if VUNLIKELY (target_ser.empty()) {
      std::cerr << "Cannot find ser for discovery." << std::endl;
      has_quit = true;
      return -1;
    }
  }

  if (target_schema_type == vlink::SchemaType::kUnknown && discovery_viewer) {
    target_schema_type = vlink::SchemaData::resolve_type(discovery_viewer->get_schema_type(url), target_ser);
  }

  if (target_schema_type == vlink::SchemaType::kUnknown) {
    target_schema_type = vlink::SchemaData::infer_ser_type(target_ser);
  }

  if (target_schema_type == vlink::SchemaType::kUnknown) {
    target_schema_type = vlink::SchemaType::kFlatbuffers;
  }

  const auto inferred_schema_type = vlink::SchemaData::infer_ser_type(target_ser);

  if VUNLIKELY (target_schema_type != vlink::SchemaType::kRaw && inferred_schema_type != vlink::SchemaType::kUnknown &&
                inferred_schema_type != target_schema_type) {
    std::cerr << "ser_type and encoding do not match." << std::endl;
    has_quit = true;
    return -1;
  }

  if VUNLIKELY (target_schema_type == vlink::SchemaType::kProtobuf) {
    std::cerr << "efbs sub does not support protobuf schema_type." << std::endl;
    has_quit = true;
    return -1;
  }

  if VUNLIKELY (discovery_viewer && discovery_viewer->is_ready_to_quit()) {
    has_quit = true;

    raw_sub.reset();
    raw_getter.reset();
    parser.reset();
    discovery_viewer.reset();
    parser_loop.reset();

    return 0;
  }

  filter_list = vlink::Helpers::split_any(filter);
  bool is_blob_type = target_schema_type == vlink::SchemaType::kRaw && use_blob_encoding;
  bool is_text_type = !is_blob_type && (target_schema_type == vlink::SchemaType::kRaw || is_text_ser_type(target_ser));
  auto schema_interface = vlink::SchemaPluginManager::get().get_interface();

  if (target_schema_type == vlink::SchemaType::kZeroCopy) {
    is_fbs_type = false;
  } else if (target_schema_type == vlink::SchemaType::kRaw) {
    is_fbs_type = false;
  } else if (target_schema_type == vlink::SchemaType::kFlatbuffers) {
    is_fbs_type = true;

    bool has_import = import_fbs_from_plugin(parser, schema_interface, target_ser);

    if (!has_import) {
      if VUNLIKELY (fbs_dir.empty()) {
        std::cerr << "Must set fbs dir [-d], set env 'VLINK_FBS_DIR', run 'vlink-efbs import <dir>', or load "
                     "VLINK_SCHEMA_PLUGIN."
                  << std::endl;
        has_quit = true;
        return -1;
      }

      try {
#ifdef _WIN32
        auto fbs_path = std::filesystem::path(vlink::Helpers::string_to_wstring(fbs_dir));
#else
        auto fbs_path = std::filesystem::path(fbs_dir);
#endif

        import_fbs(parser, target_ser, fbs_path, fbs_path, has_import);
      } catch (std::filesystem::filesystem_error& e) {
        std::cerr << e.what() << std::endl;
        has_quit = true;
        return -1;
      }
    }

    if VUNLIKELY (!parser || !has_import) {
      std::cerr << "Import flatbuffers schema failed." << std::endl;
      has_quit = true;
      return -1;
    }

    parser->Serialize();

    flatbuffers::Verifier schema_verifier(parser->builder_.GetBufferPointer(), parser->builder_.GetSize());

    if VUNLIKELY (!reflection::VerifySchemaBuffer(schema_verifier)) {
      std::cerr << "Verify flatbuffers schema failed." << std::endl;
      has_quit = true;
      return -1;
    }

    fbs_schema = reflection::GetSchema(parser->builder_.GetBufferPointer());
    fbs_root_object = fbs_schema != nullptr ? fbs_schema->root_table() : nullptr;

    if VUNLIKELY (fbs_schema == nullptr || fbs_root_object == nullptr) {
      std::cerr << "Load flatbuffers schema failed." << std::endl;
      has_quit = true;
      return -1;
    }
  } else {
    std::cerr << "Unsupported schema_type for efbs sub: " << static_cast<int>(target_schema_type) << std::endl;
    has_quit = true;
    return -1;
  }

  std::atomic<int> parse_ret = 0;

  vlink::Bytes current_bytes;
  std::mutex bytes_mtx;
  std::atomic_bool has_new_data{false};
  std::atomic<int64_t> frame_seq{0};

  std::vector<std::string> print_list;
  std::vector<int> line_list;
  std::string print_str;
  std::string current_str;
  int current_line = 0;
  bool is_url_title_with_dot = false;
  bool has_rendered_url_title = false;
  bool rendered_url_title_with_dot = false;
  double current_frame_rate = 0.0;
  double rendered_frame_rate = -1.0;
  std::deque<int64_t> frame_seq_buffer;
  std::atomic<uint64_t> last_frame_timestamp_ms{0};
  int current_rate_color = 33;
  int rendered_rate_color = -1;

  auto redraw_url_title = [&url, &current_frame_rate, &current_rate_color](bool show_data_dot, bool restore_cursor) {
    const char* rate_color = "\033[33m";

    switch (current_rate_color) {
      case 31:
        rate_color = "\033[31m";
        break;
      case 32:
        rate_color = "\033[32m";
        break;
      default:
        break;
    }

    if (restore_cursor) {
      VLINK_TERM_OUT << "\033[s";
    }

    VLINK_TERM_OUT << "\033[2;1H";
    VLINK_TERM_OUT << "\033[K";

    VLINK_TERM_OUT << "\033[34;1;4m" << url << "\033[0m ";

    if (show_data_dot) {
      VLINK_TERM_OUT << rate_color << "\u25CF " << vlink::Helpers::double_to_string(current_frame_rate) << "Hz\033[0m";
    } else {
      VLINK_TERM_OUT << rate_color << "  " << vlink::Helpers::double_to_string(current_frame_rate) << "Hz\033[0m";
    }

    if (restore_cursor) {
      VLINK_TERM_OUT << "\033[u";
    } else {
      VLINK_TERM_OUT << std::endl;
    }
  };

  auto mark_url_title_rendered = [&has_rendered_url_title, &rendered_url_title_with_dot, &rendered_frame_rate,
                                  &rendered_rate_color, &current_frame_rate, &current_rate_color](bool show_data_dot) {
    has_rendered_url_title = true;
    rendered_url_title_with_dot = show_data_dot;
    rendered_frame_rate = current_frame_rate;
    rendered_rate_color = current_rate_color;
  };

  auto should_redraw_url_title = [&has_rendered_url_title, &rendered_url_title_with_dot, &rendered_frame_rate,
                                  &rendered_rate_color, &current_frame_rate, &current_rate_color](bool show_data_dot) {
    return !has_rendered_url_title || rendered_url_title_with_dot != show_data_dot ||
           rendered_rate_color != current_rate_color || rendered_frame_rate != current_frame_rate;
  };

  auto update_terminal_function = [&url, &target_ser, &current_bytes, &bytes_mtx, &print_str, &print_list, &line_list,
                                   &current_str, &current_line, &has_new_data, &is_url_title_with_dot,
                                   &redraw_url_title, &mark_url_title_rendered, &should_redraw_url_title,
                                   &discovery_viewer, &parser_loop, &quit_function, &parse_ret, is_text_type,
                                   is_blob_type, parser, fbs_schema, fbs_root_object]() {
    auto target_terminal_size = get_terminal_size();
    bool show_data_dot = has_new_data.exchange(false);
    const bool terminal_size_changed = terminal_size != target_terminal_size;

    if VUNLIKELY (terminal_size_changed) {
      terminal_size = target_terminal_size;
      is_changed = true;
    }

    if VUNLIKELY (terminal_size.first <= 0 || terminal_size.second <= 0) {
      return;
    }

    if VUNLIKELY (!has_printed) {
      if VLIKELY (!force_update) {
        return;
      }

      VLINK_TERM_OUT << "\033[H\033[J";

      if (is_paused) {
        VLINK_TERM_OUT << "\033[33m"
                       << "Message Parsed by vlink-efbs (Wait For Message, Paused):"
                       << "\033[0m" << std::endl;
      } else {
        VLINK_TERM_OUT << "Message Parsed by vlink-efbs (Wait For Message)... " << std::endl;
      }

      VLINK_TERM_OUT << "\033[34;1;4m" << url << "\033[0m" << std::endl;
      VLINK_TERM_OUT.flush();

      force_update = false;
      return;
    }

    if VLIKELY (!is_changed.exchange(false, std::memory_order_acq_rel)) {
      if (!is_paused && should_redraw_url_title(show_data_dot)) {
        redraw_url_title(show_data_dot, true);
        mark_url_title_rendered(show_data_dot);
        VLINK_TERM_OUT.flush();
      }

      is_url_title_with_dot = false;
      force_update = false;
      return;
    }

    if (is_fbs_type) {
      {
        if VUNLIKELY (!parser) {
          return;
        }

        std::lock_guard lock(bytes_mtx);

        if VUNLIKELY (current_bytes.empty()) {
          force_update = false;
          return;
        }

        const bool fbs_verified =
            parser->opts.size_prefixed
                ? flatbuffers::VerifySizePrefixed(*fbs_schema, *fbs_root_object, current_bytes.data(),
                                                  current_bytes.size())
                : flatbuffers::Verify(*fbs_schema, *fbs_root_object, current_bytes.data(), current_bytes.size());

        if VUNLIKELY (!fbs_verified) {
          force_update = false;
          return;
        }

        print_str.clear();

        flatbuffers::custom::JsonPrinter::ignore_array = ignore_array;
        flatbuffers::custom::JsonPrinter::ignore_string = ignore_string;
        flatbuffers::custom::JsonPrinter::ignore_default = ignore_default;
        flatbuffers::custom::JsonPrinter::use_long_repeated = use_long_repeated;
        flatbuffers::custom::JsonPrinter::print_time_string = print_time_string;
        flatbuffers::custom::JsonPrinter::print_hex_string = print_hex_string;
        flatbuffers::custom::JsonPrinter::print_enum_string = print_enum_string;
        flatbuffers::custom::JsonPrinter::black_mode = black_mode;
        flatbuffers::custom::JsonPrinter::filter_list = &filter_list;

        parser->opts.output_enum_identifiers = print_enum_string;
        parser->opts.force_defaults = !ignore_default;
        parser->opts.output_default_scalars_in_json = !ignore_default;

        // NOLINTNEXTLINE(readability-redundant-smartptr-get)
        const auto* error_chars = flatbuffers::custom::GenText(*parser.get(), current_bytes.data(), &print_str);

        if VUNLIKELY (error_chars) {
          std::cerr << "Failed to gen fbs text(" << error_chars << ")." << std::endl;
          quit_function(0);

          parse_ret = 1;

          force_update = false;
          return;
        }
      }

      if VUNLIKELY ((discovery_viewer && discovery_viewer->is_ready_to_quit()) || parser_loop->is_ready_to_quit()) {
        force_update = false;
        return;
      }
    } else {
      {
        std::unique_lock lock(bytes_mtx);

        const auto target_zerocopy_type = vlink::zerocopy::MessageParser::detect_type(target_ser);
        vlink::zerocopy::MessageParser message_parser;
        bool zerocopy_parse_succeeded = false;

        if (!is_blob_type && !is_text_type && !current_bytes.empty() &&
            target_zerocopy_type != vlink::zerocopy::MessageParser::Type::kUnknown) {
          zerocopy_parse_succeeded = message_parser.parse(target_zerocopy_type, current_bytes);
        }

        if (is_blob_type) {
          print_str.clear();

          int max_line_chars = target_terminal_size.first - 2;

          if (max_line_chars < 2) {
            max_line_chars = 2;
          }

          int per_line = (max_line_chars + 1) / 3;

          if (per_line > 50) {
            per_line = 50;
          }

          if (per_line >= 10) {
            per_line = (per_line / 10) * 10;
          }

          if (per_line <= 0) {
            per_line = 10;
          }

          const size_t per_rows = current_bytes.empty() ? 0
                                                        : (current_bytes.size() + static_cast<size_t>(per_line) - 1) /
                                                              static_cast<size_t>(per_line);

          print_str += std::string("per_line: ") + std::to_string(per_line) + "\n";
          print_str += std::string("per_rows: ") + std::to_string(per_rows) + "\n";
          print_str += std::string("data_size: ") + std::to_string(current_bytes.size()) + "\n";
          print_str += std::string("data_blob:\n");

          const auto hex_str = vlink::Bytes::convert_to_hex_str(current_bytes.data(), current_bytes.size());
          size_t pos = 0;

          for (size_t i = 0; i < per_rows; ++i) {
            const size_t remain_bytes = current_bytes.size() - i * static_cast<size_t>(per_line);
            const size_t line_bytes = std::min<size_t>(static_cast<size_t>(per_line), remain_bytes);
            const size_t line_chars = line_bytes == 0 ? 0 : line_bytes * 3 - 1;

            print_str.append(hex_str, pos, line_chars);
            print_str += "\n";

            pos += line_chars;

            if (pos < hex_str.size() && hex_str[pos] == ' ') {
              ++pos;
            }
          }
        } else if (is_text_type) {
          std::string text_payload = current_bytes.to_string();

          if (!format_json_text(text_payload, print_str)) {
            print_str = std::move(text_payload);
          }
        } else if VUNLIKELY (current_bytes.empty()) {
          force_update = false;
          return;
        } else if (target_zerocopy_type != vlink::zerocopy::MessageParser::Type::kUnknown) {
          if VUNLIKELY (!zerocopy_parse_succeeded) {
            std::cerr << "Failed to parse " << vlink::zerocopy::MessageParser::type_name(target_zerocopy_type)
                      << " message." << std::endl;
            quit_function(0);

            parse_ret = 1;

            force_update = false;
            return;
          }

          vlink::zerocopy::MessageFormatOptions format_options;
          format_options.hex = print_hex_string;
          format_options.date = print_time_string;
          format_options.enum_name = print_enum_string;
          format_options.expand_arrays = !ignore_array;

          bool format_truncated = false;
          print_str = vlink::zerocopy::format_message(message_parser, format_options, &format_truncated);

          if (format_truncated) {
            is_out_of_range = true;
          }

        } else {
          std::cerr << "Unsupported type." << std::endl;
          quit_function(0);

          parse_ret = 1;

          force_update = false;
          return;
        }
      }
    }

    if VUNLIKELY ((discovery_viewer && discovery_viewer->is_ready_to_quit()) || parser_loop->is_ready_to_quit()) {
      force_update = false;
      return;
    }

    if (is_paused && !force_update && !terminal_size_changed) {
      VLINK_TERM_OUT << "\033[H";
      VLINK_TERM_OUT.flush();

      total_page = print_list.size();

      if (current_page > total_page - 1) {
        current_page = total_page - 1;
      }

      if (current_page < 0) {
        current_page = 0;
      }

      if (!print_list.empty() && !line_list.empty()) {
        current_str = print_list.at(current_page);
        current_line = line_list.at(current_page);
      }

      VLINK_TERM_OUT << "\033[K";

      VLINK_TERM_OUT << "\033[33m"
                     << "Message Parsed by vlink-efbs (Paused):"
                     << "\033[0m" << std::endl;

      redraw_url_title(show_data_dot, false);
      is_url_title_with_dot = show_data_dot;
      mark_url_title_rendered(show_data_dot);

      if (!current_str.empty()) {
        VLINK_TERM_OUT << "\033[32m";

        auto print_split_view_list = vlink::Helpers::split_view(current_str, '\n');

        for (size_t i = 0; i < print_split_view_list.size(); ++i) {
          VLINK_TERM_OUT << "\033[K";
          VLINK_TERM_OUT << print_split_view_list[i];

          if (i < print_split_view_list.size() - 1) {
            VLINK_TERM_OUT << "\n";

            if (i > 0 && i % kFlushMinLine == 0) {
              VLINK_TERM_OUT.flush();
              if constexpr (kFlushMinSleep > 0) {
                std::this_thread::sleep_for(std::chrono::microseconds(kFlushMinSleep));
              }
            }
          }
        }

        VLINK_TERM_OUT << "\033[0m";
        VLINK_TERM_OUT << "\033[K";
        VLINK_TERM_OUT << std::endl;
      }

      VLINK_TERM_OUT.flush();

    } else {
      if VUNLIKELY ((discovery_viewer && discovery_viewer->is_ready_to_quit()) || parser_loop->is_ready_to_quit()) {
        force_update = false;
        return;
      }

      if VUNLIKELY (print_str.size() > max_str_count) {
        VLINK_TERM_OUT << "\033[H\033[J";
        VLINK_TERM_OUT.flush();
        std::cerr << "The Message is too large to display." << std::endl;
        std::cerr.flush();

        force_update = false;
        return;
      }

      auto split_str_list = vlink::Helpers::split(print_str, '\n');

      if VUNLIKELY ((discovery_viewer && discovery_viewer->is_ready_to_quit()) || parser_loop->is_ready_to_quit()) {
        force_update = false;
        return;
      }

      std::string page_str;
      int line_count = 0;

      print_list.clear();
      line_list.clear();

      print_list.reserve(split_str_list.size() + 5);
      line_list.reserve(split_str_list.size() + 5);

      const auto line_width = static_cast<size_t>(terminal_size.first);

      for (const auto& str : split_str_list) {
        size_t offset = 0;
        size_t display_width = 0;

        auto complete_line = [&page_str, &line_count, &print_list, &line_list]() {
          page_str += "\n";
          ++line_count;

          if (line_count >= terminal_size.second - 3) {
            page_str.pop_back();
            print_list.emplace_back(page_str);
            line_list.emplace_back(line_count);
            page_str.clear();
            line_count = 0;
          }
        };

        while (offset < str.size()) {
          uint32_t code_point = 0;
          size_t bytes = 0;
          decode_terminal_utf8(str, offset, code_point, bytes);

          if (code_point == '\t') {
            size_t spaces = 8U - display_width % 8U;
            offset += bytes;

            while (spaces > 0) {
              if (display_width >= line_width) {
                complete_line();
                display_width = 0;
              }

              const size_t count = std::min(spaces, line_width - display_width);
              page_str.append(count, ' ');
              display_width += count;
              spaces -= count;
            }

            continue;
          }

          const auto width = static_cast<size_t>(terminal_codepoint_width(code_point));

          if (width > 0 && display_width > 0 && display_width + width > line_width) {
            complete_line();
            display_width = 0;
          }

          page_str.append(str, offset, bytes);
          display_width += width;
          offset += bytes;
        }

        complete_line();
      }

      if (!page_str.empty()) {
        page_str.pop_back();
      }

      if (!page_str.empty()) {
        print_list.emplace_back(page_str);
        line_list.emplace_back(line_count);
      }

      total_page = std::min(print_list.size(), static_cast<size_t>(5000));

      if (current_page > total_page - 1) {
        current_page = total_page - 1;
      }

      if (current_page < 0) {
        current_page = 0;
      }

      if (!print_list.empty() && !line_list.empty()) {
        current_str = print_list.at(current_page);
        current_line = line_list.at(current_page);
      }

      VLINK_TERM_OUT << "\033[H\033[K";

      if (is_paused) {
        VLINK_TERM_OUT << "\033[33m"
                       << "Message Parsed by vlink-efbs (Paused):"
                       << "\033[0m" << std::endl;
      } else {
        VLINK_TERM_OUT << "Message Parsed by vlink-efbs:" << std::endl;
      }

      redraw_url_title(show_data_dot, false);
      is_url_title_with_dot = show_data_dot;
      mark_url_title_rendered(show_data_dot);
      VLINK_TERM_OUT.flush();

      if (!current_str.empty()) {
        if (!current_str.empty()) {
          VLINK_TERM_OUT << "\033[32m";

          auto print_split_view_list = vlink::Helpers::split_view(current_str, '\n');

          for (size_t i = 0; i < print_split_view_list.size(); ++i) {
            VLINK_TERM_OUT << "\033[K";
            VLINK_TERM_OUT << print_split_view_list[i];

            if (i < print_split_view_list.size() - 1) {
              VLINK_TERM_OUT << "\n";

              if (i > 0 && i % kFlushMinLine == 0) {
                VLINK_TERM_OUT.flush();
                if constexpr (kFlushMinSleep > 0) {
                  std::this_thread::sleep_for(std::chrono::microseconds(kFlushMinSleep));
                }
              }
            }
          }

          VLINK_TERM_OUT << "\033[0m";
          VLINK_TERM_OUT << "\033[K";
          VLINK_TERM_OUT << std::endl;
        }
      }
    }

    if (current_line < terminal_size.second - 3) {
      for (int i = 0; i < terminal_size.second - 3 - current_line; ++i) {
        VLINK_TERM_OUT << "\033[K";
        VLINK_TERM_OUT << std::endl;
      }
    }

    std::string last_line_str;

    last_line_str = std::string("\033[44;37;1m") + std::string("<") +
                    (total_page == 0 ? std::string("0") : std::to_string(current_page + 1)) + std::string("/") +
                    ((total_page >= 5000 || is_out_of_range) ? std::string("5000+") : std::to_string(total_page)) +
                    std::string(">") + std::string("\033[0m [ ");

    if (print_enum_string) {
      last_line_str += "\033[4mE\033[0m ";
    } else {
      last_line_str += "\033[0mE\033[0m ";
    }

    if (ignore_array) {
      last_line_str += "\033[4mR\033[0m ";
    } else {
      last_line_str += "\033[0mR\033[0m ";
    }

    if (ignore_string) {
      last_line_str += "\033[4mT\033[0m ";
    } else {
      last_line_str += "\033[0mT\033[0m ";
    }

    if (print_time_string) {
      last_line_str += "\033[4mY\033[0m ";
    } else {
      last_line_str += "\033[0mY\033[0m ";
    }

    if (print_hex_string) {
      last_line_str += "\033[4mU\033[0m ";
    } else {
      last_line_str += "\033[0mU\033[0m ";
    }

    if (ignore_default) {
      last_line_str += "\033[4mO\033[0m ";
    } else {
      last_line_str += "\033[0mO\033[0m ";
    }

    if (use_long_repeated) {
      last_line_str += "\033[4mP\033[0m ";
    } else {
      last_line_str += "\033[0mP\033[0m ";
    }

    last_line_str += std::string("] ");

    VLINK_TERM_OUT << "\033[K";

    if VLIKELY (last_line_str.size() <= static_cast<size_t>(terminal_size.first + 69)) {
      VLINK_TERM_OUT << last_line_str;
    } else {
      VLINK_TERM_OUT << last_line_str.substr(0, terminal_size.first + 69);
    }

    VLINK_TERM_OUT.flush();

    is_out_of_range = false;
    force_update = false;
  };

  auto listen_bytes_function = [&parser_loop, &update_terminal_function, &current_bytes, &bytes_mtx, &has_new_data,
                                &frame_seq, &last_frame_timestamp_ms](const vlink::Bytes& bytes) {
    if VUNLIKELY (has_quit) {
      return;
    }

    std::lock_guard lock(bytes_mtx);

    if VLIKELY (!is_paused) {
      current_bytes = bytes;
      has_new_data = true;
      ++frame_seq;
      last_frame_timestamp_ms.store(vlink::ElapsedTimer::get_cpu_timestamp(vlink::ElapsedTimer::kMilli));
      is_changed = true;

      if (!has_printed) {
        has_printed = true;
        parser_loop->post_task(update_terminal_function);
      }
    }
  };

  bool should_use_getter = use_getter || (!has_explicit_ser && ((target_type & vlink::kSetter) != 0));

  try {
    if (should_use_getter) {
      raw_getter.emplace(std::make_shared<RawGetter>(url, vlink::InitType::kWithoutInit));

      if (native_mode) {
        (*raw_getter)->set_property("dds.ip", native_ip);
      }

      (*raw_getter)->set_ser_type(target_ser, target_schema_type);
      (*raw_getter)->init();
      (*raw_getter)->listen(listen_bytes_function);
    } else {
      raw_sub.emplace(std::make_shared<RawSub>(url, vlink::InitType::kWithoutInit));

      if (native_mode) {
        (*raw_sub)->set_property("dds.ip", native_ip);
      }

      (*raw_sub)->set_ser_type(target_ser, target_schema_type);
      (*raw_sub)->init();
      (*raw_sub)->listen(listen_bytes_function);
    }
  } catch (vlink::Exception::RuntimeError& e) {
    std::cerr << e.what() << std::endl;
    has_quit = true;
    return -1;
  }

  auto reset_frame_rate_state = [&has_new_data, &frame_seq, &frame_seq_buffer, &current_frame_rate, &current_rate_color,
                                 &last_frame_timestamp_ms]() {
    has_new_data = false;
    frame_seq.store(0);
    frame_seq_buffer.clear();
    current_frame_rate = 0.0;
    current_rate_color = 33;

    if (last_frame_timestamp_ms.load() == 0) {
      last_frame_timestamp_ms.store(vlink::ElapsedTimer::get_cpu_timestamp(vlink::ElapsedTimer::kMilli));
    }
  };

  vlink::Utils::start_detect_keyboard(
      [&parser_loop, &update_terminal_function, &quit_function, &reset_frame_rate_state](const std::string& key) {
        // VLINK_TERM_OUT << "key:" << key << std::endl;

        if (key == "q" || key == "esc") {
          quit_function(0);
        } else if (key == " ") {
          if (is_paused) {
            is_paused = false;
            is_changed = true;
            force_update = true;
            parser_loop->post_task(update_terminal_function);
          } else {
            is_paused = true;
            parser_loop->post_task([&reset_frame_rate_state, &update_terminal_function]() {
              reset_frame_rate_state();
              is_changed = true;
              force_update = true;
              update_terminal_function();
            });
          }
        } else if (key == "left") {
          if (current_page >= 1) {
            --current_page;
            is_changed = true;
            parser_loop->post_task(update_terminal_function);
          }
        } else if (key == "right") {
          if (current_page < total_page - 1) {
            ++current_page;
            is_changed = true;
            parser_loop->post_task(update_terminal_function);
          }
        } else if (key == "up") {
          if (current_page >= 10) {
            current_page -= 10;
            is_changed = true;
            parser_loop->post_task(update_terminal_function);
          } else if (current_page >= 1) {
            current_page = 0;
            is_changed = true;
            parser_loop->post_task(update_terminal_function);
          }
        } else if (key == "down") {
          if (current_page < total_page - 10) {
            current_page += 10;
            is_changed = true;
            parser_loop->post_task(update_terminal_function);
          } else if (current_page < total_page - 1) {
            current_page = total_page - 1;
            is_changed = true;
            parser_loop->post_task(update_terminal_function);
          }
        } else if (key == "pgup") {
          if (current_page >= 100) {
            current_page -= 100;
            is_changed = true;
            parser_loop->post_task(update_terminal_function);
          } else if (current_page >= 1) {
            current_page = 0;
            is_changed = true;
            parser_loop->post_task(update_terminal_function);
          }
        } else if (key == "pgdown") {
          if (current_page < total_page - 100) {
            current_page += 100;
            is_changed = true;
            parser_loop->post_task(update_terminal_function);
          } else if (current_page < total_page - 1) {
            current_page = total_page - 1;
            is_changed = true;
            parser_loop->post_task(update_terminal_function);
          }
        } else if (key == "home") {
          if (current_page != 0) {
            current_page = 0;
            is_changed = true;
            parser_loop->post_task(update_terminal_function);
          }
        } else if (key == "end") {
          if (current_page != total_page - 1) {
            current_page = total_page - 1;
            is_changed = true;
            parser_loop->post_task(update_terminal_function);
          }
        } else if (key == "e") {
          print_enum_string = !print_enum_string;
          is_changed = true;
          force_update = true;
          parser_loop->post_task(update_terminal_function);
        } else if (key == "r") {
          ignore_array = !ignore_array;
          is_changed = true;
          force_update = true;
          parser_loop->post_task(update_terminal_function);
        } else if (key == "t") {
          ignore_string = !ignore_string;
          is_changed = true;
          force_update = true;
          parser_loop->post_task(update_terminal_function);
        } else if (key == "y") {
          print_time_string = !print_time_string;
          is_changed = true;
          force_update = true;
          parser_loop->post_task(update_terminal_function);
        } else if (key == "u") {
          print_hex_string = !print_hex_string;
          is_changed = true;
          force_update = true;
          parser_loop->post_task(update_terminal_function);
        } else if (key == "o") {
          ignore_default = !ignore_default;
          is_changed = true;
          force_update = true;
          parser_loop->post_task(update_terminal_function);
        } else if (key == "p") {
          use_long_repeated = !use_long_repeated;
          is_changed = true;
          force_update = true;
          parser_loop->post_task(update_terminal_function);
        }
      });

  vlink::Timer terminal_timer;
  terminal_timer.set_interval(kTerminalInterval);
  terminal_timer.set_loop_count(vlink::Timer::kInfinite);
  terminal_timer.attach(parser_loop.get());
  terminal_timer.set_callback(update_terminal_function);
  terminal_timer.start();

  vlink::Timer stats_timer;
  stats_timer.set_interval(kCollectInterval);
  stats_timer.set_loop_count(vlink::Timer::kInfinite);
  stats_timer.attach(parser_loop.get());
  stats_timer.set_callback(
      [&frame_seq, &frame_seq_buffer, &current_frame_rate, &current_rate_color, &last_frame_timestamp_ms]() {
        double freq = 0;
        int weight = 1;
        int total_weight = 0;

        if VUNLIKELY (!has_printed) {
          return;
        }

        frame_seq_buffer.emplace_back(frame_seq.exchange(0));
        uint64_t now_ms = vlink::ElapsedTimer::get_cpu_timestamp(vlink::ElapsedTimer::kMilli);
        uint64_t last_frame_ms = last_frame_timestamp_ms.load();

        while (frame_seq_buffer.size() > kCounterCache) {
          frame_seq_buffer.pop_front();
        }

        for (auto seq : frame_seq_buffer) {
          freq += seq * weight;
          total_weight += weight;
          weight *= kCounterWeight;
        }

        if (total_weight > 0) {
          current_frame_rate = freq / total_weight;
        } else {
          current_frame_rate = 0.0;
        }

        if (frame_seq_buffer.back() > 0 && frame_seq_buffer.size() >= kCounterCache) {
          current_rate_color = 32;
        } else if (last_frame_ms > 0 && (now_ms - last_frame_ms) > kCollectInterval * kCounterCache) {
          current_frame_rate = 0.0;
          frame_seq_buffer.clear();
          current_rate_color = 31;
        } else {
          current_rate_color = 33;
        }
      });
  stats_timer.start();

  vlink::Timer::call_once(parser_loop.get(), 250, [&parser_loop, &update_terminal_function]() {
    if VUNLIKELY (has_quit) {
      return;
    }

    if VLIKELY (!has_printed) {
      force_update = true;
      is_changed = true;
      parser_loop->post_task(update_terminal_function);
    }
  });

  if (discovery_viewer) {
    discovery_viewer->wait_for_quit();
  }

  parser_loop->wait_for_quit();

  has_quit = true;

  vlink::Utils::stop_detect_keyboard();

  if (parse_ret == 0) {
    // VLINK_TERM_OUT << std::endl;
    VLINK_TERM_OUT << "\033[H\033[J";
    VLINK_TERM_OUT.flush();
  }

  raw_sub.reset();
  raw_getter.reset();
  parser.reset();
  discovery_viewer.reset();
  parser_loop.reset();

  return parse_ret;
}
