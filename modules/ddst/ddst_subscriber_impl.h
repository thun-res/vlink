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
#include "./ddst_status_listener.h"
#include "./impl/calculate_sample.h"
#include "./impl/subscriber_impl.h"

namespace vlink {

// DdstSubscriberImpl
class DdstSubscriberImpl final : public SubscriberImpl, public AbstractNode {
 public:
  // ReaderListener
  class ReaderListener : public DdstReaderListener {
   public:
    explicit ReaderListener(NodeImpl* impl);

    void on_data_available(ddst::DataReader* reader) override;
  };

  explicit DdstSubscriberImpl(const DdstConf& conf);

  void process_message(ddst::DataReader* reader);

 private:
  void init() override;

  void deinit() override;

  bool suspend() override;

  bool resume() override;

  bool is_suspend() const override;

  const Conf* get_conf() const override;

  const AbstractNode* get_abstract_node() const override;

  Status::BasePtr get_status(Status::Type type) const override;

  std::any get_native_handle() const override;

  bool listen(MsgCallback&& callback) override;

  void set_latency_and_lost_enabled(bool enable) override;

  bool is_latency_and_lost_enabled() const override;

  int64_t get_latency() const override;

  SampleLostInfo get_lost() const override;

  friend ReaderListener;

  std::atomic_bool quit_flag_{false};
  std::atomic<int64_t> last_latency_{0};

  DdstConf conf_;
  std::shared_ptr<ddst::DomainParticipant> participant_;
  std::shared_ptr<ddst::Topic> topic_;
  std::shared_ptr<ddst::Subscriber> subscriber_;
  std::optional<ReaderListener> listener_;
  std::shared_ptr<ddst::DataReader> reader_;
  MsgCallback callback_;
  CalculateSample calc_sample_;
  std::atomic_bool is_latency_and_lost_enabled_{false};

  VLINK_DISALLOW_COPY_AND_ASSIGN(DdstSubscriberImpl)
};

}  // namespace vlink
