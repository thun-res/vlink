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
#include <unordered_map>
#include <utility>
#include <vector>

#include "./dds_factory.h"
#include "./dds_status_listener.h"
#include "./impl/server_impl.h"

namespace vlink {

// DdsServerImpl
class DdsServerImpl final : public ServerImpl, public AbstractNode {
 public:
  // WriterListener
  class WriterListener : public DdsWriterListener {
   public:
    explicit WriterListener(NodeImpl* impl);

   private:
    friend class DdsServerImpl;
  };

  // ReaderListener
  class ReaderListener : public DdsReaderListener {
   public:
    explicit ReaderListener(NodeImpl* impl);

    void on_subscription_matched(dds::DataReader* reader, const dds::SubscriptionMatchedStatus& status) override;

    void on_data_available(dds::DataReader* reader) override;

   private:
    friend class DdsServerImpl;
  };

  explicit DdsServerImpl(const DdsConf& conf);

  void process_message(dds::DataReader* reader);

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

  friend ReaderListener;

  std::atomic<int> read_session_count_{0};
  std::atomic_bool quit_flag_{false};

  DdsConf conf_;
  std::shared_ptr<dds::DomainParticipant> participant_;
  std::shared_ptr<dds::Topic> topic_req_;
  std::shared_ptr<dds::Topic> topic_resp_;
  std::shared_ptr<dds::Publisher> publisher_;
  std::shared_ptr<dds::Subscriber> subscriber_;
  std::optional<WriterListener> writer_listener_;
  std::optional<ReaderListener> reader_listener_;
  std::shared_ptr<dds::DataWriter> writer_;
  std::shared_ptr<dds::DataReader> reader_;
  dds::TypeSupport type_support_req_;
  alignas(64) std::atomic<uint64_t> cdr_seq_{0};
  std::unordered_map<uint64_t, rtps::WriteParams> cdr_id_map_;
  std::mutex param_mtx_;
  ReqRespCallback callback_;

  VLINK_DISALLOW_COPY_AND_ASSIGN(DdsServerImpl)
};

}  // namespace vlink
