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

/*
 * =============================================================================
 * File: c_pubsub.c
 *
 * Pure C demo of the publisher / subscriber model via vlink's C API.
 *
 * Handle types (vlink_publisher_handle_t, vlink_subscriber_handle_t, ...) are
 * opaque to the user; internally each one is a small struct that owns a
 * shared_ptr<vlink::Publisher<...>> / shared_ptr<vlink::Subscriber<...>>.
 * The C++ object is destroyed when vlink_destroy_*() is called (which drops
 * the shared_ptr), so as long as we pair every create with a matching
 * destroy, no leaks.
 *
 * Schema info: vlink_schema_info_t carries the registered "type name" plus
 * the schema family enum (VLINK_SCHEMA_RAW, VLINK_SCHEMA_PROTOBUF, etc.). For
 * raw bytes the type name is informational; for typed transports it lets
 * peers reject mismatched ser_type.
 * =============================================================================
 */

#include <stdio.h>
#include <string.h>
#include <vlink/external/c_api.h>

/* Cross-platform millisecond sleep helper. */
#ifdef _WIN32
#include <windows.h>
#define sleep_ms(ms) Sleep(ms)  // NOLINT(readability-identifier-naming)
#else
#include <unistd.h>
#define sleep_ms(ms) usleep((ms) * 1000)  // NOLINT(readability-identifier-naming)
#endif

/* File-scope counter so the on_message callback can communicate with main().
 * No mutex needed: the intra:// transport delivers serially per subscriber. */
static int g_received_count = 0;

/*
 * Subscriber data callback.
 * Trigger:  every successful publish on "intra://c_api/pubsub" once the
 *           subscriber has been matched to the publisher.
 * Thread:   vlink's intra-process delivery worker (NOT main). Treat user_data
 *           as read-only unless you serialise access yourself.
 */
static void on_message(const uint8_t* data, const size_t size, void* user_data) {
  (void)user_data;
  printf("[Subscriber] Received %zu bytes: %.*s\n", size, (int)size, (const char*)data);
  g_received_count++;
}

int main(void) {
  int ret;
  /* Raw byte schema. "text" is just a label; VLINK_SCHEMA_RAW disables any
   * peer-side typed deserialisation. */
  const vlink_schema_info_t schema = {"text", VLINK_SCHEMA_RAW};

  /* Section: create the subscriber first so it is ready when the publisher
   * announces itself; this avoids losing the first few messages. */
  vlink_subscriber_handle_t sub;
  ret = vlink_create_subscriber("intra://c_api/pubsub", &schema, &sub, on_message, NULL);

  if (ret != VLINK_RET_NO_ERROR) {
    printf("create_subscriber failed: %d\n", ret);
    return 1;
  }

  /* Section: create the publisher on the same URL. The scheme intra://
   * keeps everything in-process; for shared-memory use shm://, for DDS use
   * dds://, etc. */
  vlink_publisher_handle_t pub;
  ret = vlink_create_publisher("intra://c_api/pubsub", &schema, &pub);

  if (ret != VLINK_RET_NO_ERROR) {
    printf("create_publisher failed: %d\n", ret);
    vlink_destroy_subscriber(&sub);
    return 1;
  }

  /* Section: wait for discovery. wait_for_subscribers blocks until at least
   * one subscriber is matched, or the timeout elapses. */
  ret = vlink_wait_for_subscribers(pub, 2000);
  printf("wait_for_subscribers: %s\n", ret == VLINK_RET_NO_ERROR ? "matched" : "timeout");
  printf("has_subscribers: %s\n", vlink_has_subscribers(pub) == VLINK_RET_NO_ERROR ? "yes" : "no");

  /* Section: publish a small burst. sleep_ms(50) gives the subscriber's
   * delivery thread time to log each message in order, purely for demo
   * readability -- it is not required by the API. */
  for (int i = 1; i <= 5; ++i) {
    char msg[64];

    // NOLINTNEXTLINE(clang-analyzer-security.insecureAPI.DeprecatedOrUnsafeBufferHandling)
    int len = snprintf(msg, sizeof(msg), "Hello from C API #%d", i);

    ret = vlink_publish(pub, (const uint8_t*)msg, (size_t)len);
    printf("publish: \"%s\" (ret=%d)\n", msg, ret);
    sleep_ms(50);
  }

  /* Section: publish_by_force sidesteps the "no subscribers? drop" optimisation
   * and writes the payload onto the wire regardless. Useful for late-joining
   * subscribers when combined with a durable history QoS. */
  const char* force_msg = "forced message";
  ret = vlink_publish_by_force(pub, (const uint8_t*)force_msg, strlen(force_msg));
  printf("publish_by_force ret=%d\n", ret);

  /* Final drain so the subscriber's delivery thread can flush before we
   * destroy the handles. */
  sleep_ms(200);
  printf("Total received: %d\n", g_received_count);

  /* Pair every create with a destroy; destroy drops the inner shared_ptr,
   * triggering teardown of the C++ Publisher/Subscriber. */
  vlink_destroy_publisher(&pub);
  vlink_destroy_subscriber(&sub);
  return 0;
}
