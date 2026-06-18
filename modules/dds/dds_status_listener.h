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

#include "./dds_factory.h"

namespace vlink {

// DdsWriterListener
class DdsWriterListener : public dds::DataWriterListener {
 public:
  explicit DdsWriterListener(NodeImpl* impl);

  ~DdsWriterListener() override;

  NodeImpl* get_impl() const { return impl_; }

  static Status::BasePtr get_status(dds::DataWriter* writer, Status::Type type);

 protected:
  void on_publication_matched(dds::DataWriter*, const dds::PublicationMatchedStatus& status) override;

  void on_offered_deadline_missed(dds::DataWriter*, const dds::OfferedDeadlineMissedStatus& status) override;

  void on_offered_incompatible_qos(dds::DataWriter*, const dds::OfferedIncompatibleQosStatus& status) override;

  void on_liveliness_lost(dds::DataWriter*, const dds::LivelinessLostStatus& status) override;

 private:
  NodeImpl* impl_{nullptr};
};

// DdsReaderListener
class DdsReaderListener : public dds::DataReaderListener {
 public:
  explicit DdsReaderListener(NodeImpl* impl);

  ~DdsReaderListener() override;

  NodeImpl* get_impl() const { return impl_; }

  static Status::BasePtr get_status(dds::DataReader* reader, Status::Type type);

 protected:
  void on_subscription_matched(dds::DataReader*, const dds::SubscriptionMatchedStatus& status) override;

  void on_requested_deadline_missed(dds::DataReader*, const dds::RequestedDeadlineMissedStatus& status) override;

  void on_liveliness_changed(dds::DataReader*, const dds::LivelinessChangedStatus& status) override;

  void on_sample_rejected(dds::DataReader*, const dds::SampleRejectedStatus& status) override;

  void on_requested_incompatible_qos(dds::DataReader*, const dds::RequestedIncompatibleQosStatus& status) override;

  void on_sample_lost(dds::DataReader*, const dds::SampleLostStatus& status) override;

 private:
  NodeImpl* impl_{nullptr};
};

}  // namespace vlink
