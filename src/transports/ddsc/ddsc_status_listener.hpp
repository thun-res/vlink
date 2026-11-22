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

#include <memory>
#include <optional>
#include <utility>
#include <vector>

#include "./ddsc_factory.hpp"

namespace vlink {

// DdscWriterListener
class DdscWriterListener {
 public:
  explicit DdscWriterListener(NodeImpl* impl);

  virtual ~DdscWriterListener();

  NodeImpl* get_impl() const { return impl_; }

  dds_listener_t* get_ptr() const { return listener_; }

  static Status::BasePtr get_status(dds_entity_t writer, Status::Type type);

  virtual void on_publication_matched(NodeImpl* impl, const dds_publication_matched_status_t& status);

 private:
  NodeImpl* impl_{nullptr};
  dds_listener_t* listener_{nullptr};
};

// DdscReaderListener
class DdscReaderListener {
 public:
  explicit DdscReaderListener(NodeImpl* impl);

  virtual ~DdscReaderListener();

  NodeImpl* get_impl() const { return impl_; }

  dds_listener_t* get_ptr() const { return listener_; }

  static Status::BasePtr get_status(dds_entity_t reader, Status::Type type);

  virtual void on_subscription_matched(NodeImpl* impl, const dds_subscription_matched_status_t& status);

  virtual void on_data_available(NodeImpl* impl, dds_entity_t reader);

 private:
  NodeImpl* impl_{nullptr};
  dds_listener_t* listener_{nullptr};
};

}  // namespace vlink
