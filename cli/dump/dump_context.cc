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

#include "./dump_context.h"

#include <vlink/extension/bag_plugin_interface.h>

#include <iostream>

namespace vlink::dump {

DumpContext& DumpContext::get() {
  static DumpContext ctx;
  return ctx;
}

bool DumpContext::invoke_callback(int64_t timestamp, const std::string& url, const std::string& ser,
                                  vlink::SchemaType schema_type, const vlink::Bytes& bytes) {
  std::lock_guard lock(dump_callback_mtx);

  if VUNLIKELY (!callback_has_set || !dump_callback) {
    return false;
  }

  dump_callback(timestamp, url, ser, schema_type, bytes);
  return true;
}

void DumpContext::request_stop() {
  if (dump_for_bag) {
    if (bag_player) {
      bag_player->clear_bag_interface();
      bag_player->stop();
    }
  } else if (discovery_viewer) {
    discovery_viewer->quit(true);
  }
}

bool DumpContext::prepare_bag_plugin() {
  if (bag_plugin_name.empty()) {
    return true;
  }

  bag_plugin_interface = bag_plugin.load<vlink::BagPluginInterface>(bag_plugin_name, 2, 0);

  if VUNLIKELY (!bag_plugin_interface) {
    std::cerr << "Failed to load plugin (" << bag_plugin_name << ")." << std::endl;
    return false;
  }

  return true;
}

void DumpContext::bind_bag_plugin(const std::shared_ptr<vlink::BagReader>& reader) {
  if (reader && bag_plugin_interface) {
    reader->bind_bag_interface(bag_plugin_interface);
  }
}

}  // namespace vlink::dump
