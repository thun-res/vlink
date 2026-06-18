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

#include <map>
#include <memory>
#include <utility>

#include "./ddsr_factory.hpp"
#include "./ddsr_status_listener.hpp"
#include "./impl/ack_manager.h"
#include "./impl/client_impl.h"

namespace vlink {

// DdsrClientImpl
class DdsrClientImpl final : public ClientImpl, public AbstractNode {
 public:
  // WriterListener
  class WriterListener : public DdsrWriterListener {
   public:
    explicit WriterListener(NodeImpl* impl);

    void on_publication_matched(NodeImpl* impl, const DDS_PublicationMatchedStatus& status) override;
  };

  // ReaderListener
  class ReaderListener : public DdsrReaderListener {
   public:
    explicit ReaderListener(NodeImpl* impl);

    void on_subscription_matched(NodeImpl* impl, const DDS_SubscriptionMatchedStatus& status) override;

    void on_data_available(NodeImpl* impl, DDS_DataReader* reader) override;
  };

  explicit DdsrClientImpl(const DdsrConf& conf);

  void process_message(DDS_DataReader* reader);

 private:
  void init() override;

  void deinit() override;

  void interrupt() override;

  const Conf* get_conf() const override;

  const AbstractNode* get_abstract_node() const override;

  Status::BasePtr get_status(Status::Type type) const override;

  std::any get_native_handle() const override;

  bool is_connected() const override;

  bool call(const Bytes& req_data, MsgCallback&& callback, std::chrono::milliseconds timeout) override;

  std::atomic<int> write_session_count_{0};
  std::atomic<int> read_session_count_{0};
  alignas(64) std::atomic<uint32_t> seq_{0};
  std::atomic_bool quit_flag_{false};

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
  std::recursive_mutex param_mtx_;
  std::map<uint64_t, MsgCallback> callbacks_;
  AckManager ack_manager_;

  VLINK_DISALLOW_COPY_AND_ASSIGN(DdsrClientImpl)
};

}  // namespace vlink
