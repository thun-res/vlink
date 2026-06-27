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
#include <utility>

#include "./ddsr_factory.hpp"
#include "./ddsr_status_listener.hpp"
#include "./impl/server_impl.h"

namespace vlink {

// DdsrServerImpl
class DdsrServerImpl final : public ServerImpl, public AbstractNode {
 public:
  // WriterListener
  class WriterListener : public DdsrWriterListener {
   public:
    explicit WriterListener(NodeImpl* impl);
  };

  // ReaderListener
  class ReaderListener : public DdsrReaderListener {
   public:
    explicit ReaderListener(NodeImpl* impl);

    void on_subscription_matched(NodeImpl* impl, const DDS_SubscriptionMatchedStatus& status) override;

    void on_data_available(NodeImpl* impl, DDS_DataReader* reader) override;
  };

  explicit DdsrServerImpl(const DdsrConf& conf);

  void process_message(DDS_DataReader* reader);

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

  bool has_clients() const override;

  bool listen(ReqRespCallback&& callback) override;

  bool reply(uint64_t req_id, const Bytes& resp_data, bool is_sync) override;

  std::atomic_bool quit_flag_{false};
  std::atomic<int> read_session_count_{0};

  DdsrConf conf_;
  std::shared_ptr<ddsr::DomainParticipant> participant_;
  std::shared_ptr<ddsr::Topic> topic_req_;
  std::shared_ptr<ddsr::Topic> topic_resp_;
  std::shared_ptr<ddsr::Publisher> publisher_;
  std::shared_ptr<ddsr::Subscriber> subscriber_;
  std::optional<WriterListener> writer_listener_;
  std::optional<ReaderListener> reader_listener_;
  std::shared_ptr<ddsr::DataWriter> writer_;
  std::shared_ptr<ddsr::DataReader> reader_;
  ReqRespCallback callback_;

  VLINK_DISALLOW_COPY_AND_ASSIGN(DdsrServerImpl)
};

}  // namespace vlink
