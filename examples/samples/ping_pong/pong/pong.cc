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

// Pong side of a latency benchmark.
//
// Trivial echo node: subscribes to the ping topic and republishes every payload
// verbatim on the pong topic. Pairs with ping.cc to form a round-trip latency
// rig over any transport selected by ping_pong_common.h. Typical engineering
// scenario: deploy on the peer host/process of the system under test.

#include <vlink/base/message_loop.h>
#include <vlink/base/utils.h>
#include <vlink/vlink.h>

#include <chrono>

#include "../ping_pong_common.h"

using namespace std::chrono_literals;  // NOLINT(build/namespaces, google-build-using-namespace)

int main(int argc, char* argv[]) {
  (void)argc;
  (void)argv;

  // Park on the loop until Ctrl+C; ensures pub/sub destructors run cleanly.
  vlink::MessageLoop message_loop;
  vlink::Utils::register_terminate_signal([&message_loop](int) { message_loop.quit(); });

  vlink::Subscriber<vlink::Bytes> sub(Common::get_ping_url());
  vlink::Publisher<vlink::Bytes> pub(Common::get_pong_url());

  // Echo: callback runs on transport worker thread; pub.publish() is safe to
  // call from any thread, so we forward directly without bouncing onto the
  // MessageLoop -- this minimises the added latency contribution of pong.
  sub.listen([&pub](const vlink::Bytes& data) { pub.publish(data); });

  message_loop.run();
  return 0;
}
