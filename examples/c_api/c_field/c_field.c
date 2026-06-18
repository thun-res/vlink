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
 * File: c_field.c
 *
 * Pure C demo of the field (state-sync) model: Setter writes the latest
 * value, Getters can either be polled or notified.
 *
 * Two flavours of Getter are shown:
 *   - PUSH mode: pass a non-NULL callback to vlink_create_getter. The
 *     callback is invoked on the delivery thread every time the underlying
 *     value transitions.
 *   - POLL mode: pass NULL for the callback. The user code calls vlink_get
 *     into a caller-owned buffer when it wants the latest value.
 *
 * Field semantics: the Setter holds the "current truth"; late-joining
 * Getters always observe the most recent value (no history replay).
 * =============================================================================
 */

#include <stdio.h>
#include <string.h>
#include <vlink/external/c_api.h>

#ifdef _WIN32
#include <windows.h>
#define sleep_ms(ms) Sleep(ms)  // NOLINT(readability-identifier-naming)
#else
#include <unistd.h>
#define sleep_ms(ms) usleep((ms) * 1000)  // NOLINT(readability-identifier-naming)
#endif

/* Counter for push-mode notifications. */
static int g_change_count = 0;

/*
 * Push-mode getter callback.
 * Trigger: every value change pushed by the Setter, including the first
 *          value observed on connection.
 * Thread:  vlink delivery thread for the field URL.
 */
static void on_value_changed(const uint8_t* data, const size_t size, void* user_data) {
  (void)user_data;
  printf("[push] Value changed: %.*s\n", (int)size, (const char*)data);
  g_change_count++;
}

int main(void) {
  int ret;
  const vlink_schema_info_t schema = {"text", VLINK_SCHEMA_RAW};

  /* Section: create the Setter (the writer side of the field). */
  vlink_setter_handle_t setter;
  ret = vlink_create_setter("intra://c_api/field", &schema, &setter);

  if (ret != VLINK_RET_NO_ERROR) {
    printf("create_setter failed: %d\n", ret);
    return 1;
  }

  /* Publish an initial value so any subsequently-created Getter sees a
   * meaningful snapshot. */
  const char* initial = "temperature=25";
  ret = vlink_set(setter, (const uint8_t*)initial, strlen(initial));
  printf("set(\"%s\") ret=%d\n", initial, ret);
  sleep_ms(50);

  /* Section: PUSH-mode getter -- non-NULL callback means "tell me when it
   * changes". */
  vlink_getter_handle_t getter_push;
  ret = vlink_create_getter("intra://c_api/field", &schema, &getter_push, on_value_changed, NULL);

  if (ret != VLINK_RET_NO_ERROR) {
    printf("create push getter failed: %d\n", ret);
    vlink_destroy_setter(&setter);
    return 1;
  }

  /* Section: POLL-mode getter -- NULL callback means "I will fetch when I
   * want via vlink_get". No delivery thread will touch user code for this
   * handle. */
  vlink_getter_handle_t getter_poll;
  ret = vlink_create_getter("intra://c_api/field", &schema, &getter_poll, NULL, NULL);

  if (ret != VLINK_RET_NO_ERROR) {
    printf("create poll getter failed: %d\n", ret);
    vlink_destroy_getter(&getter_push);
    vlink_destroy_setter(&setter);
    return 1;
  }

  /* Allow the push-mode getter to receive the initial snapshot. */
  sleep_ms(100);

  /* Section: drive a series of value updates. The push getter logs each one
   * on its delivery thread; the poll getter is unaffected until we ask. */
  const char* values[] = {"temperature=28", "temperature=30", "temperature=27", "temperature=32"};
  int count = (int)(sizeof(values) / sizeof(values[0]));

  for (int i = 0; i < count; ++i) {
    ret = vlink_set(setter, (const uint8_t*)values[i], strlen(values[i]));
    printf("set: \"%s\" (ret=%d)\n", values[i], ret);
    sleep_ms(50);
  }

  /* Drain pushes before polling. */
  sleep_ms(200);

  /* Section: poll the latest value into a caller-owned buffer. The buf_size
   * in/out parameter: caller sets capacity, callee sets actual size. Failure
   * codes:
   *   VLINK_RET_TRANSFER_ERROR -- no value has ever been set.
   *   VLINK_RET_MEMORY_ERROR   -- supplied buffer is too small. */
  uint8_t buf[256];
  size_t buf_size = sizeof(buf);
  ret = vlink_get(getter_poll, buf, &buf_size);

  if (ret == VLINK_RET_NO_ERROR) {
    printf("poll: %.*s (%zu bytes)\n", (int)buf_size, (const char*)buf, buf_size);
  } else {
    printf("poll: ret=%d (TRANSFER_ERROR=no value, MEMORY_ERROR=buffer too small)\n", ret);
  }

  printf("push callback invocations: %d\n", g_change_count);

  /* Destroy all three handles. Order between push/poll getters does not
   * matter; the Setter is destroyed last for clarity. */
  vlink_destroy_getter(&getter_poll);
  vlink_destroy_getter(&getter_push);
  vlink_destroy_setter(&setter);
  return 0;
}
