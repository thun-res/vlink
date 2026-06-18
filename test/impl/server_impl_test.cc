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

// NOLINTBEGIN

#include "./impl/server_impl.h"

#include <doctest/doctest.h>

#include <cstdint>

#include "../common_test.h"

namespace {

class TestServer : public ServerImpl {
 public:
  TestServer() = default;
  ~TestServer() override = default;

  void init() override {}
  void deinit() override {}

  bool listen(ReqRespCallback&& callback) override {
    callback_ = std::move(callback);
    is_listened = true;
    return true;
  }

  void fire(uint64_t req_id, const Bytes& req, Bytes* resp) {
    if (callback_) {
      callback_(req_id, req, resp);
    }
  }

 private:
  ReqRespCallback callback_;
};

}  // namespace

TEST_SUITE("impl-ServerImpl") {
  TEST_CASE("constructor sets kServer role") {
    TestServer srv;

    CHECK_EQ(srv.impl_type, kServer);
  }

  TEST_CASE("public flags default to false") {
    TestServer srv;

    CHECK_FALSE(srv.is_listened);
    CHECK_FALSE(srv.is_resp_type);
    CHECK_FALSE(srv.is_sync_type);
  }

  TEST_CASE("has_clients returns false by default") {
    TestServer srv;

    CHECK_FALSE(srv.has_clients());
  }

  TEST_CASE("listen sets is_listened flag") {
    TestServer srv;

    srv.listen([](uint64_t, const Bytes&, Bytes*) {});

    CHECK(srv.is_listened);
  }

  TEST_CASE("listen callback receives request id and fires") {
    TestServer srv;
    uint64_t got_id = 0;

    srv.listen([&](uint64_t id, const Bytes&, Bytes*) { got_id = id; });

    Bytes req;
    srv.fire(42, req, nullptr);

    CHECK_EQ(got_id, 42u);
  }

  TEST_CASE("listen callback can write response bytes") {
    TestServer srv;

    srv.listen([](uint64_t, const Bytes&, Bytes* resp) {
      if (resp) {
        *resp = Bytes({0xAB, 0xCD});
      }
    });

    Bytes req;
    Bytes resp;
    srv.fire(1, req, &resp);

    CHECK_EQ(resp.size(), 2u);
  }

  TEST_CASE("reply always returns false in base implementation") {
    TestServer srv;
    Bytes resp;

    SUBCASE("synchronous path") { CHECK_FALSE(srv.reply(1, resp, true)); }

    SUBCASE("asynchronous path") { CHECK_FALSE(srv.reply(1, resp, false)); }

    SUBCASE("req_id zero") { CHECK_FALSE(srv.reply(0, resp, true)); }

    SUBCASE("req_id max") { CHECK_FALSE(srv.reply(UINT64_MAX, resp, false)); }
  }

  TEST_CASE("is_resp_type and is_sync_type are mutable") {
    TestServer srv;

    srv.is_resp_type = true;
    srv.is_sync_type = true;

    CHECK(srv.is_resp_type);
    CHECK(srv.is_sync_type);
  }

  TEST_CASE("impl_type is kServer not other roles") {
    TestServer srv;

    CHECK_NE(srv.impl_type, kPublisher);
    CHECK_NE(srv.impl_type, kSubscriber);
    CHECK_NE(srv.impl_type, kClient);
    CHECK_EQ(srv.impl_type, kServer);
  }

  TEST_CASE("set_property and get_property persist on server") {
    TestServer srv;

    srv.set_property("server.mode", "sync");
    CHECK_EQ(srv.get_property("server.mode"), "sync");
  }

  TEST_CASE("get_property returns empty for unset key") {
    TestServer srv;

    CHECK(srv.get_property("no.key").empty());
  }

  TEST_CASE("interrupt and reset_interrupted work on server") {
    TestServer srv;

    CHECK_FALSE(srv.is_interrupted());

    srv.interrupt();
    CHECK(srv.is_interrupted());

    srv.reset_interrupted();
    CHECK_FALSE(srv.is_interrupted());
  }

  TEST_CASE("listen callback receives request data correctly") {
    TestServer srv;
    Bytes received_req;

    srv.listen([&](uint64_t, const Bytes& req, Bytes*) { received_req = req; });

    Bytes req = {0x01, 0x02, 0x03};
    srv.fire(1, req, nullptr);

    REQUIRE_EQ(received_req.size(), 3u);
    CHECK_EQ(received_req[0], 0x01u);
    CHECK_EQ(received_req[1], 0x02u);
    CHECK_EQ(received_req[2], 0x03u);
  }

  TEST_CASE("listen callback fires multiple times for sequential requests") {
    TestServer srv;
    int count = 0;

    srv.listen([&](uint64_t, const Bytes&, Bytes*) { ++count; });

    Bytes req;
    for (int i = 0; i < 5; ++i) {
      srv.fire(static_cast<uint64_t>(i), req, nullptr);
    }

    CHECK_EQ(count, 5);
  }

  TEST_CASE("is_listened is false before listen is called") {
    TestServer srv;

    CHECK_FALSE(srv.is_listened);

    srv.listen([](uint64_t, const Bytes&, Bytes*) {});

    CHECK(srv.is_listened);
  }

  TEST_CASE("listen with empty callback fires without crash") {
    TestServer srv;

    srv.listen([](uint64_t, const Bytes&, Bytes*) {});

    Bytes req;
    CHECK_NOTHROW(srv.fire(0, req, nullptr));
  }

  TEST_CASE("reply with all req_id variants returns false in base") {
    TestServer srv;
    Bytes resp;

    SUBCASE("small req_id") { CHECK_FALSE(srv.reply(1, resp, true)); }

    SUBCASE("large req_id") { CHECK_FALSE(srv.reply(1000000, resp, false)); }

    SUBCASE("req_id zero async") { CHECK_FALSE(srv.reply(0, resp, false)); }
  }
}

// NOLINTEND
