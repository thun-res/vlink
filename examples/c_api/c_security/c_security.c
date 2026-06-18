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
 * File: c_security.c
 *
 * Pure C demo of vlink's security layer (AES-128-GCM, AEAD with replay
 * protection).
 *
 * Two cryptographic configurations:
 *   1. Raw 16-byte symmetric key (cfg.key). Length is checked; an
 *      additional-authenticated-data (AAD) string lets you scope a key to a
 *      specific topic so the same key cannot decrypt a different topic's
 *      payloads.
 *   2. PBKDF2-derived key (cfg.passphrase + cfg.pbkdf2_*). The key is
 *      stretched from a passphrase with the supplied salt and iteration
 *      count.
 *
 * Memory ownership: encrypt/decrypt allocate output buffers and hand
 * ownership to the caller. Those buffers MUST be released with
 * vlink_security_free_buffer() -- they are NOT malloc'd in a way that free()
 * can release (they may come from a pool / arena inside vlink core), so
 * using free() invokes UB.
 *
 * Replay defence: the AEAD layer remembers nonces it has seen and rejects
 * any ciphertext that reuses a nonce within the session. The second
 * decrypt of the same ciphertext below returns VLINK_RET_TRANSFER_ERROR to
 * demonstrate this guard.
 *
 * Transport binding: the second half of main() shows the six secure_*
 * constructors that wrap any URL scheme (here shm://) with the same
 * security config so payloads are encrypted before they leave the process.
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

/* Count decrypted messages so main() can print a summary. */
static int g_received_count = 0;

/*
 * Subscriber callback for the shm:// secure section below.
 * Trigger:  every successfully decrypted message.
 * Thread:   vlink delivery thread for the secure_subscriber.
 */
static void on_message(const uint8_t* data, const size_t size, void* user_data) {
  (void)user_data;
  printf("[Subscriber] Decrypted %zu bytes: %.*s\n", size, (int)size, (const char*)data);
  g_received_count++;
}

/* Debug helper: prints the first and last byte of a buffer so the user can
 * see that encrypt/decrypt actually produced different bytes. */
static void print_hex_preview(const char* label, const uint8_t* data, size_t size) {
  printf("  %s size=%zu", label, size);

  if (size > 0) {
    printf(" first=0x%02x last=0x%02x", data[0], data[size - 1]);
  }

  printf("\n");
}

int main(void) {
  int ret;

  /*
   * Section 1: standalone encrypt / decrypt round-trip with a raw symmetric
   * key. No transport involved -- just the security primitive.
   */
  {
    vlink_security_config_t cfg;
    /* Always init the struct so unset future fields get defaults. */
    vlink_security_config_init(&cfg);
    cfg.key = "my-secret-key-16";                    /* must be 16 bytes for AES-128 */
    cfg.advanced.aad_context = "c_api/security/raw"; /* topic-scoped AAD */

    vlink_security_handle_t sec = vlink_security_create(&cfg);

    if (sec == NULL) {
      printf("vlink_security_create failed.\n");
      return 1;
    }

    const char* plain = "Hello with AES-128-GCM via C API!";
    const size_t plain_size = strlen(plain);

    /* Encrypt: cipher buffer is owned by vlink and must be freed with
     * vlink_security_free_buffer (NOT free()). */
    uint8_t* cipher = NULL;
    size_t cipher_size = 0;
    ret = vlink_security_encrypt(sec, (const uint8_t*)plain, plain_size, &cipher, &cipher_size);
    printf("encrypt ret=%d\n", ret);
    print_hex_preview("cipher", cipher, cipher_size);

    /* Decrypt: same ownership rule. */
    uint8_t* recovered = NULL;
    size_t recovered_size = 0;
    ret = vlink_security_decrypt(sec, cipher, cipher_size, &recovered, &recovered_size);
    printf("decrypt ret=%d\n", ret);

    if (ret == VLINK_RET_NO_ERROR && recovered_size == plain_size && memcmp(recovered, plain, plain_size) == 0) {
      printf("Roundtrip: PASS (\"%.*s\")\n", (int)recovered_size, (const char*)recovered);
    } else {
      printf("Roundtrip: FAIL\n");
    }

    /* Replay attempt: feeding the same ciphertext twice must fail because
     * the AEAD layer tracks consumed nonces. */
    uint8_t* replay_plain = NULL;
    size_t replay_plain_size = 0;
    ret = vlink_security_decrypt(sec, cipher, cipher_size, &replay_plain, &replay_plain_size);
    printf("replay decrypt ret=%d (expected %d)\n", ret, VLINK_RET_TRANSFER_ERROR);
    /* Even on failure, replay_plain MAY have been allocated -- always free. */
    vlink_security_free_buffer(replay_plain);

    /* Free every output buffer with the SDK helper -- never with free(). */
    vlink_security_free_buffer(cipher);
    vlink_security_free_buffer(recovered);
    vlink_security_destroy(sec);
  }

  /*
   * Section 2: passphrase-derived key (PBKDF2-HMAC-SHA256). Higher iteration
   * count = stronger but slower at startup. The same salt+iterations+
   * passphrase MUST be configured on both peers.
   */
  {
    static uint8_t salt[] = "vlink-c-example-salt-v1";

    vlink_security_config_t cfg;
    vlink_security_config_init(&cfg);
    cfg.passphrase = "correct horse battery staple";
    cfg.pbkdf2_salt = salt;
    cfg.pbkdf2_salt_size = sizeof(salt) - 1;
    cfg.pbkdf2_iterations = 200000U;

    vlink_security_handle_t sec = vlink_security_create(&cfg);

    if (sec == NULL) {
      printf("vlink_security_create failed.\n");
      return 1;
    }

    const char* plain = "Passphrase-derived AES key";
    const size_t plain_size = strlen(plain);

    uint8_t* cipher = NULL;
    size_t cipher_size = 0;
    ret = vlink_security_encrypt(sec, (const uint8_t*)plain, plain_size, &cipher, &cipher_size);

    (void)ret;

    uint8_t* recovered = NULL;
    size_t recovered_size = 0;
    ret = vlink_security_decrypt(sec, cipher, cipher_size, &recovered, &recovered_size);

    if (ret == VLINK_RET_NO_ERROR && recovered_size == plain_size && memcmp(recovered, plain, plain_size) == 0) {
      printf("PBKDF2 roundtrip: PASS\n");
    } else {
      printf("PBKDF2 roundtrip: FAIL\n");
    }

    vlink_security_free_buffer(cipher);
    vlink_security_free_buffer(recovered);
    vlink_security_destroy(sec);
  }

  /*
   * Section 3: secure transport (shm:// in this demo). Six secure_*
   * constructors mirror the non-secure ones:
   *   vlink_create_secure_publisher / _subscriber
   *   vlink_create_secure_server    / _client
   *   vlink_create_secure_setter    / _getter
   * Each takes the same args as its non-secure counterpart plus a
   * vlink_security_config_t* describing the key material.
   *
   * shm:// requires iceoryx RouDi to be running on the host; gating this
   * section behind an env var keeps the rest of the demo runnable in a
   * minimal CI environment.
   */
  printf("\n[shm secure] set VLINK_C_SECURITY_RUN_SHM=1 with iox-roudi running to enable\n");

#ifdef _WIN32
  char* env = NULL;
  size_t len = 0;
  _dupenv_s(&env, &len, "VLINK_C_SECURITY_RUN_SHM");

  if (env == NULL) {
    printf("skipping shm transport section\n");
    goto section3_end;
  }

  free(env);
#else
  if (getenv("VLINK_C_SECURITY_RUN_SHM") == NULL) {
    printf("skipping shm transport section\n");
    goto section3_end;
  }
#endif

  {
    const vlink_schema_info_t schema = {"text", VLINK_SCHEMA_RAW};

    /* Subscriber side security config -- same shared key as the publisher. */
    vlink_security_config_t sub_cfg;
    vlink_security_config_init(&sub_cfg);
    sub_cfg.key = "shm-shared-key-1";

    vlink_subscriber_handle_t sub;
    ret = vlink_create_secure_subscriber("shm://c_api/security", &schema, &sub, on_message, NULL, &sub_cfg);

    if (ret != VLINK_RET_NO_ERROR) {
      printf("create_secure_subscriber returned %d (RouDi missing?)\n", ret);
      goto section3_end;
    }

    /* Publisher side -- key MUST match the subscriber. */
    vlink_security_config_t pub_cfg;
    vlink_security_config_init(&pub_cfg);
    pub_cfg.key = "shm-shared-key-1";

    vlink_publisher_handle_t pub;
    ret = vlink_create_secure_publisher("shm://c_api/security", &schema, &pub, &pub_cfg);

    if (ret != VLINK_RET_NO_ERROR) {
      printf("create_secure_publisher returned %d\n", ret);
      vlink_destroy_subscriber(&sub);
      goto section3_end;
    }

    vlink_wait_for_subscribers(pub, 2000);

    const char* messages[] = {"encrypted msg 1", "encrypted msg 2", "encrypted msg 3"};

    /* Publish ciphertext over shared memory; the subscriber's delivery
     * thread decrypts in place and hands plaintext to on_message. */
    for (int i = 0; i < 3; ++i) {
      vlink_publish(pub, (const uint8_t*)messages[i], strlen(messages[i]));
      sleep_ms(50);
    }

    sleep_ms(200);
    printf("decrypted messages received: %d\n", g_received_count);

    vlink_destroy_publisher(&pub);
    vlink_destroy_subscriber(&sub);
  }

section3_end:
  printf("=== Example complete ===\n");
  return 0;
}
