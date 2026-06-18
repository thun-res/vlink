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
 * File: c_rpc.c
 *
 * Pure C demo of the request/response (method) model.
 *
 * Server side:
 *   - on_request fires on the delivery thread for every incoming invocation.
 *     The handler MUST call vlink_reply() before returning -- the reply path
 *     ends with that call. Calling vlink_reply outside on_request results in
 *     VLINK_RET_TRANSFER_ERROR because no in-flight request context exists.
 *   - user_data is the address of the server handle so vlink_reply can find
 *     the underlying transport.
 *
 * Client side:
 *   - vlink_invoke takes the request bytes plus a per-call response callback;
 *     on_response is invoked on the delivery thread when the matching reply
 *     arrives.
 *   - Returns immediately (asynchronous); use the callback to detect
 *     completion.
 * =============================================================================
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vlink/external/c_api.h>

#ifdef _WIN32
#include <windows.h>
#define sleep_ms(ms) Sleep(ms)  // NOLINT(readability-identifier-naming)
#else
#include <unistd.h>
#define sleep_ms(ms) usleep((ms) * 1000)  // NOLINT(readability-identifier-naming)
#endif

/* Counts completed responses so main() can summarise at the end. */
static int g_resp_received = 0;

/*
 * Server-side request handler.
 * Trigger:  every vlink_invoke from a matched client.
 * Thread:   vlink server delivery thread for "intra://c_api/rpc".
 * Contract: vlink_reply() MUST be called synchronously inside this function.
 *           Skipping it leaves the client waiting (it eventually times out).
 */
static void on_request(const uint8_t* data, const size_t size, void* user_data) {
  /* user_data was set to &server in vlink_create_server below so the handler
   * can reach the handle without globals. */
  vlink_server_handle_t* server = (vlink_server_handle_t*)user_data;

  printf("[Server] Request: %.*s (%zu bytes)\n", (int)size, (const char*)data, size);

  char resp[128];

  // NOLINTNEXTLINE(clang-analyzer-security.insecureAPI.DeprecatedOrUnsafeBufferHandling)
  int resp_len = snprintf(resp, sizeof(resp), "REPLY: %.*s", (int)size, (const char*)data);

  int ret = vlink_reply(server, (const uint8_t*)resp, (size_t)resp_len);
  printf("[Server] Reply sent (ret=%d)\n", ret);
}

/*
 * Client-side response callback.
 * Trigger:  reply arrival for a specific vlink_invoke; one call per invoke.
 * Thread:   vlink client delivery thread. data may be NULL on transport-level
 *           failures (size == 0 in that case).
 */
static void on_response(const uint8_t* data, const size_t size, void* user_data) {
  (void)user_data;

  if (data != NULL && size > 0) {
    printf("[Client] Response: %.*s\n", (int)size, (const char*)data);
  } else {
    printf("[Client] Empty response\n");
  }

  g_resp_received++;
}

int main(void) {
  int ret;
  const vlink_schema_info_t schema = {"text", VLINK_SCHEMA_RAW};

  /* Section: create the server first so it is discoverable when the client
   * appears. user_data is &server so on_request can reach it. */
  vlink_server_handle_t server;
  ret = vlink_create_server("intra://c_api/rpc", &schema, &server, on_request, &server);

  if (ret != VLINK_RET_NO_ERROR) {
    printf("create_server failed: %d\n", ret);
    return 1;
  }

  /* Section: create the client on the same URL. */
  vlink_client_handle_t client;
  ret = vlink_create_client("intra://c_api/rpc", &schema, &client);

  if (ret != VLINK_RET_NO_ERROR) {
    printf("create_client failed: %d\n", ret);
    vlink_destroy_server(&server);
    return 1;
  }

  /* Section: wait for the discovery handshake before issuing the first
   * invoke. Without this the first invoke may race the discovery and time
   * out. */
  ret = vlink_wait_for_server(client, 2000);
  printf("wait_for_server: %s\n", ret == VLINK_RET_NO_ERROR ? "connected" : "timeout");

  /* Section: issue three invocations. Each invoke returns immediately; the
   * reply arrives later on the client delivery thread via on_response. */
  for (int i = 1; i <= 3; ++i) {
    char req[64];

    // NOLINTNEXTLINE(clang-analyzer-security.insecureAPI.DeprecatedOrUnsafeBufferHandling)
    int req_len = snprintf(req, sizeof(req), "request_%d", i);

    ret = vlink_invoke(client, (const uint8_t*)req, (size_t)req_len, on_response, NULL);
    printf("invoke(\"%s\") ret=%d\n", req, ret);
    sleep_ms(100);
  }

  /* Drain so every on_response has a chance to fire before destroy. */
  sleep_ms(200);
  printf("Responses received: %d\n", g_resp_received);

  /* Pair every create with a destroy. */
  vlink_destroy_client(&client);
  vlink_destroy_server(&server);
  return 0;
}
