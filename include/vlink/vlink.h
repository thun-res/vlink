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

/**
 * @file vlink.h
 * @brief Single umbrella entry point for the public VLink communication SDK.
 *
 * @details
 * Including @c <vlink/vlink.h> pulls in every primitive of the three VLink
 * communication models together with the @c MessageLoop dispatcher and a small
 * set of base utilities.  Application code should rarely need to include any
 * other VLink header directly; transport-specific configuration types are
 * brought in transitively through the primitive headers.
 *
 * @par Pulled-in Headers and Mapped Primitives
 * | Header                  | Public Primitive(s)                | Model     | Role                       |
 * | ----------------------- | ---------------------------------- | --------- | -------------------------- |
 * | @c client.h             | @c Client\<ReqT,RespT\>            | Method    | RPC caller side            |
 * | @c server.h             | @c Server\<ReqT,RespT\>            | Method    | RPC handler side           |
 * | @c publisher.h          | @c Publisher\<MsgT\>               | Event     | Topic emitter              |
 * | @c subscriber.h         | @c Subscriber\<MsgT\>              | Event     | Topic listener             |
 * | @c getter.h             | @c Getter\<ValueT\>                | Field     | Latest-value reader        |
 * | @c setter.h             | @c Setter\<ValueT\>                | Field     | Latest-value writer        |
 * | @c base/message_loop.h  | @c MessageLoop                     | Dispatch  | Callback re-routing        |
 * | @c base/utils.h         | @c sleep_for, env helpers, etc.    | Utilities | General-purpose helpers    |
 *
 * @par Three Communication Models Side-by-side
 * @verbatim
 *   +----------------------+   +-----------------------+   +-------------------------+
 *   |     EVENT MODEL      |   |     METHOD MODEL      |   |       FIELD MODEL       |
 *   |  (pub/sub stream)    |   |  (request/response)   |   |   (latest-value sync)   |
 *   +----------------------+   +-----------------------+   +-------------------------+
 *   |  Publisher<MsgT>     |   |  Client<Req,Resp>     |   |  Setter<ValueT>         |
 *   |       |              |   |        |              |   |        |                |
 *   |       v  publish()   |   |        v  invoke()    |   |        v  set()         |
 *   |    transport         |   |     transport         |   |     transport           |
 *   |       |              |   |        |              |   |   (retains latest)      |
 *   |       v              |   |        v              |   |        |                |
 *   |  Subscriber<MsgT>    |   |  Server<Req,Resp>     |   |        v                |
 *   |   on each msg        |   |   handler fills resp  |   |  Getter<ValueT>         |
 *   |                      |   |        |              |   |     get() / listen()    |
 *   |                      |   |        v reply        |   |                         |
 *   |                      |   |    Client receives    |   |                         |
 *   +----------------------+   +-----------------------+   +-------------------------+
 * @endverbatim
 *
 * @par Transport Selection via URL Prefix
 * The transport back-end is selected by the URL scheme.  All API calls remain
 * identical when the prefix is changed; this is the central design tenet of
 * VLink.
 *
 * | Prefix         | Back-end                          | Typical use case                       |
 * | -------------- | --------------------------------- | -------------------------------------- |
 * | @c intra://    | In-process pub/sub                | Same-process zero-copy fan-out         |
 * | @c shm://      | Iceoryx shared memory             | Inter-process zero-copy on one host    |
 * | @c shm2://     | Native VLink shared memory        | Lightweight inter-process IPC          |
 * | @c dds://      | FastDDS (CDR)                     | Standards-compliant DDS distribution   |
 * | @c ddsc://     | CycloneDDS                        | CycloneDDS-based deployments           |
 * | @c ddsr://     | RTI Connext DDS                   | RTI-licensed deployments               |
 * | @c zenoh://    | Eclipse Zenoh                     | Pub/sub and query/reply, edge to cloud |
 * | @c someip://   | SOME/IP                           | Automotive AUTOSAR adaptive services   |
 * | @c fdbus://    | FDBus                             | Linux/Android service bus              |
 * | @c mqtt://     | MQTT                              | Telemetry to cloud brokers             |
 *
 * @par Quick-start Example -- one process, three models
 * @code
 * #include <vlink/vlink.h>
 *
 * // ---- Event model ---------------------------------------------------------
 * vlink::Subscriber<MyMsg> sub("dds://vehicle/speed");
 * sub.listen([](const MyMsg& msg) { handle_event(msg); });
 *
 * vlink::Publisher<MyMsg> pub("dds://vehicle/speed");
 * pub.publish(MyMsg{100});
 *
 * // ---- Method model (synchronous RPC) -------------------------------------
 * vlink::Server<Req, Resp> svr("dds://compute/sum");
 * svr.listen([](const Req& q, Resp& r) { r.value = q.a + q.b; });
 *
 * vlink::Client<Req, Resp> cli("dds://compute/sum");
 * Resp r;
 * if (cli.invoke(Req{1, 2}, r)) { use(r); }
 *
 * // ---- Field model (latest-value sync) ------------------------------------
 * vlink::Setter<int> setter("shm://vehicle/gear");
 * setter.set(3);
 *
 * vlink::Getter<int> getter("shm://vehicle/gear");
 * if (auto gear = getter.get()) { use(*gear); }
 * @endcode
 *
 * @note Each primitive has a @c Security* counterpart (@c SecurityPublisher,
 *       @c SecuritySubscriber, @c SecurityClient, @c SecurityServer,
 *       @c SecuritySetter, @c SecurityGetter) that transparently encrypts and
 *       decrypts the payload using a @c Security::Config aggregate.
 *
 * @see publisher.h, subscriber.h, client.h, server.h, getter.h, setter.h
 */

#pragma once

// NOLINTBEGIN

// method
#include "./client.h"
#include "./server.h"

// event
#include "./publisher.h"
#include "./subscriber.h"

// field
#include "./getter.h"
#include "./setter.h"

// message_loop
#include "./base/message_loop.h"

// utils
#include "./base/utils.h"

// NOLINTEND
