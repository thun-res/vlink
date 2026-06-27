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

#include "./ddst_factory.h"

namespace vlink {

// DdstWriterListener
class DdstWriterListener : public ddst::DataWriterListener {
 public:
  explicit DdstWriterListener(NodeImpl* impl);

  ~DdstWriterListener() override;

  NodeImpl* get_impl() const { return impl_; }

  static Status::BasePtr get_status(ddst::DataWriter* writer, Status::Type type);

 protected:
  void on_publication_matched(ddst::DataWriter*, const ddst::PublicationMatchedStatus& status) override;

  void on_offered_deadline_missed(ddst::DataWriter*, const ddst::OfferedDeadlineMissedStatus& status) override;

  void on_offered_incompatible_qos(ddst::DataWriter*, const ddst::OfferedIncompatibleQosStatus& status) override;

  void on_liveliness_lost(ddst::DataWriter*, const ddst::LivelinessLostStatus& status) override;

 private:
  NodeImpl* impl_{nullptr};
};

// DdstReaderListener
class DdstReaderListener : public ddst::DataReaderListener {
 public:
  explicit DdstReaderListener(NodeImpl* impl);

  ~DdstReaderListener() override;

  NodeImpl* get_impl() const { return impl_; }

  static Status::BasePtr get_status(ddst::DataReader* reader, Status::Type type);

 protected:
  void on_subscription_matched(ddst::DataReader*, const ddst::SubscriptionMatchedStatus& status) override;

  void on_requested_deadline_missed(ddst::DataReader*, const ddst::RequestedDeadlineMissedStatus& status) override;

  void on_liveliness_changed(ddst::DataReader*, const ddst::LivelinessChangedStatus& status) override;

  void on_sample_rejected(ddst::DataReader*, const ddst::SampleRejectedStatus& status) override;

  void on_requested_incompatible_qos(ddst::DataReader*, const ddst::RequestedIncompatibleQosStatus& status) override;

  void on_sample_lost(ddst::DataReader*, const ddst::SampleLostStatus& status) override;

 private:
  NodeImpl* impl_{nullptr};
};

}  // namespace vlink
