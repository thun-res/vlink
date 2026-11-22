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

#include "./ddsr_factory.hpp"

namespace vlink {

// DdsrWriterListener
class DdsrWriterListener {
 public:
  explicit DdsrWriterListener(NodeImpl* impl);

  virtual ~DdsrWriterListener();

  NodeImpl* get_impl() const { return impl_; }

  DDS_DataWriterListener* get_ptr() { return &listener_; }

  static Status::BasePtr get_status(DDS_DataWriter* writer, Status::Type type);

  virtual void on_publication_matched(NodeImpl* impl, const DDS_PublicationMatchedStatus& status);

 private:
  NodeImpl* impl_{nullptr};
  DDS_DataWriterListener listener_ = DDS_DataWriterListener_INITIALIZER;
};

// DdsrReaderListener
class DdsrReaderListener {
 public:
  explicit DdsrReaderListener(NodeImpl* impl);

  virtual ~DdsrReaderListener();

  NodeImpl* get_impl() const { return impl_; }

  DDS_DataReaderListener* get_ptr() { return &listener_; }

  static Status::BasePtr get_status(DDS_DataReader* reader, Status::Type type);

  virtual void on_subscription_matched(NodeImpl* impl, const DDS_SubscriptionMatchedStatus& status);

  virtual void on_data_available(NodeImpl* impl, DDS_DataReader* reader);

 private:
  NodeImpl* impl_{nullptr};
  DDS_DataReaderListener listener_ = DDS_DataReaderListener_INITIALIZER;
};

}  // namespace vlink
