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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vlink/external/c_api.h>
#ifdef _WIN32
#include <Windows.h>
#else
#include <unistd.h>
#endif

void custom_sleep(int ms) {
#ifdef _WIN32
  Sleep(ms);
#else
  usleep(ms * 1000);
#endif
}

// event
void on_subscriber_msg(const uint8_t* data, const size_t size, void* user_data) {
  if (!data || size == 0) {
    return;
  }

  if (data != NULL && size == strlen("event") && memcmp(data, "event", strlen("event")) == 0) {
    if (user_data) {
      *(int*)user_data = 0;
    }
  }
}

int test_pub_sub(void) {
  printf("test_pub_sub...\n");
  fflush(stdout);

  int ret = 1;
  int ret2 = 1;
  const vlink_schema_info_t schema = {"text", VLINK_SCHEMA_RAW};

  vlink_subscriber_handle_t sub_handle = {0};

  ret = vlink_create_subscriber("dds://c_interface/event", &schema, &sub_handle, on_subscriber_msg, &ret2);

  vlink_publisher_handle_t pub_handle = {0};

  ret += vlink_create_publisher("dds://c_interface/event", &schema, &pub_handle);

  ret += vlink_wait_for_subscribers(pub_handle, 5000);

  ret += vlink_has_subscribers(pub_handle);

  char* msg = "event";

  ret += vlink_publish(pub_handle, (uint8_t*)msg, strlen(msg));

  custom_sleep(100);

  ret += vlink_destroy_subscriber(&sub_handle);

  ret += vlink_destroy_publisher(&pub_handle);

  return ret + ret2;
}

typedef struct {
  const char* expected;
  int got;
} expected_msg_ctx_t;

void on_expected_msg(const uint8_t* data, const size_t size, void* user_data) {
  expected_msg_ctx_t* ctx = (expected_msg_ctx_t*)user_data;

  if (!ctx || !ctx->expected || !data) {
    return;
  }

  if (size == strlen(ctx->expected) && memcmp(data, ctx->expected, size) == 0) {
    ctx->got = 0;
  }
}

int test_intra_queue_publish_buffer_reuse(void) {
  printf("test_intra_queue_publish_buffer_reuse...\n");
  fflush(stdout);

  int ret = 0;
  const vlink_schema_info_t schema = {"text", VLINK_SCHEMA_RAW};
  expected_msg_ctx_t ctx = {"queue_original", 1};

  vlink_subscriber_handle_t sub_handle = {0};
  int rc = vlink_create_subscriber("intra://c_interface/buffer_reuse#queue", &schema, &sub_handle, on_expected_msg,
                                   &ctx);
  if (rc != VLINK_RET_NO_ERROR) {
    printf("FAIL: create intra queue subscriber rc=%d\n", rc);
    return 1;
  }

  vlink_publisher_handle_t pub_handle = {0};
  rc = vlink_create_publisher("intra://c_interface/buffer_reuse#queue", &schema, &pub_handle);
  if (rc != VLINK_RET_NO_ERROR) {
    printf("FAIL: create intra queue publisher rc=%d\n", rc);
    vlink_destroy_subscriber(&sub_handle);
    return 1;
  }

  vlink_wait_for_subscribers(pub_handle, 1000);

  uint8_t msg[] = "queue_original";
  rc = vlink_publish(pub_handle, msg, strlen((const char*)msg));
  memset(msg, 'X', sizeof(msg));
  if (rc != VLINK_RET_NO_ERROR) {
    printf("FAIL: intra queue publish rc=%d\n", rc);
    ret = 1;
  }

  for (int i = 0; i < 50 && ctx.got != 0; ++i) {
    custom_sleep(20);
  }

  if (ctx.got != 0) {
    printf("FAIL: intra queue publish did not preserve payload after caller buffer reuse\n");
    ret = 1;
  } else {
    printf("PASS: intra queue publish preserves payload after caller buffer reuse\n");
  }

  vlink_destroy_publisher(&pub_handle);
  vlink_destroy_subscriber(&sub_handle);

  return ret;
}

// method

void on_server_msg(const uint8_t* req_data, const size_t req_size, void* user_data) {
  if (!req_data || req_size == 0) {
    return;
  }

  if (req_data != NULL && req_size == strlen("method_req") &&
      memcmp(req_data, "method_req", strlen("method_req")) == 0) {
    vlink_server_handle_t* handle = (vlink_server_handle_t*)user_data;

    char* msg = "method_resp";

    vlink_reply(handle, (uint8_t*)msg, strlen(msg));
  }
}

void on_client_msg(const uint8_t* data, const size_t size, void* user_data) {
  if (!data || size == 0) {
    return;
  }

  if (data != NULL && size == strlen("method_resp") && memcmp(data, "method_resp", strlen("method_resp")) == 0) {
    if (user_data) {
      *(int*)user_data = 0;
    }
  }
}

int test_server_client(void) {
  printf("test_server_client...\n");
  fflush(stdout);

  int ret = 1;
  int ret2 = 1;
  const vlink_schema_info_t schema = {"text", VLINK_SCHEMA_RAW};

  vlink_server_handle_t server_handle = {0};

  ret = vlink_create_server("dds://c_interface/method", &schema, &server_handle, on_server_msg, &server_handle);

  vlink_client_handle_t client_handle = {0};

  ret += vlink_create_client("dds://c_interface/method", &schema, &client_handle);

  ret += vlink_wait_for_server(client_handle, 5000);

  ret += vlink_has_server(client_handle);

  char* msg = "method_req";

  ret += vlink_invoke(client_handle, (uint8_t*)msg, strlen(msg), on_client_msg, &ret2);

  custom_sleep(100);

  ret += vlink_destroy_server(&server_handle);

  ret += vlink_destroy_client(&client_handle);

  return ret + ret2;
}

// field

void on_getter_msg(const uint8_t* data, const size_t size, void* user_data) {
  if (data != NULL && size == strlen("field") && memcmp(data, "field", strlen("field")) == 0) {
    *(int*)user_data = 0;
  }
}

int test_setter_getter(void) {
  printf("test_setter_getter...\n");
  fflush(stdout);

  int ret = 1;
  int ret2 = 1;
  const vlink_schema_info_t schema = {"text", VLINK_SCHEMA_RAW};

  vlink_setter_handle_t setter_handle = {0};

  ret = vlink_create_setter("dds://c_interface/field", &schema, &setter_handle);

  vlink_getter_handle_t getter_handle = {0};

  ret += vlink_create_getter("dds://c_interface/field", &schema, &getter_handle, on_getter_msg, &ret2);

  char* msg = "field";

  ret += vlink_set(setter_handle, (uint8_t*)msg, strlen(msg));

  custom_sleep(100);

  uint8_t data[100];

  size_t size = sizeof(data);

  ret += vlink_get(getter_handle, data, &size);

  ret += !(size == strlen("field") && memcmp(data, "field", strlen("field")) == 0);

  ret += vlink_destroy_setter(&setter_handle);

  ret += vlink_destroy_getter(&getter_handle);

  return ret + ret2;
}

int test_schema_info_validation(void) {
  printf("test_schema_info_validation...\n");
  fflush(stdout);

  int ret = 0;

  vlink_publisher_handle_t pub_handle = {0};

  ret += vlink_create_publisher("dds://c_interface/schema_null", NULL, &pub_handle);
  ret += vlink_destroy_publisher(&pub_handle);

  const vlink_schema_info_t empty_schema = {"", VLINK_SCHEMA_UNKNOWN};
  ret += vlink_create_publisher("dds://c_interface/schema_empty", &empty_schema, &pub_handle);
  ret += vlink_destroy_publisher(&pub_handle);

  const vlink_schema_info_t invalid_schema = {"text", (vlink_schema_t)99};
  ret += vlink_create_publisher("dds://c_interface/schema_invalid", &invalid_schema, &pub_handle) !=
         VLINK_RET_INVALID_ERROR;

  const vlink_schema_info_t missing_schema = {"text", VLINK_SCHEMA_UNKNOWN};
  ret += vlink_create_publisher("dds://c_interface/schema_missing_schema", &missing_schema, &pub_handle) !=
         VLINK_RET_INVALID_ERROR;

  const vlink_schema_info_t missing_ser = {NULL, VLINK_SCHEMA_RAW};
  ret += vlink_create_publisher("dds://c_interface/schema_missing_ser", &missing_ser, &pub_handle) !=
         VLINK_RET_INVALID_ERROR;

  const vlink_schema_info_t empty_ser = {"", VLINK_SCHEMA_RAW};
  ret +=
      vlink_create_publisher("dds://c_interface/schema_empty_ser", &empty_ser, &pub_handle) != VLINK_RET_INVALID_ERROR;

  {
    const vlink_schema_info_t schema = {"text", VLINK_SCHEMA_RAW};
    vlink_publisher_handle_t valid_pub_handle = {0};

    ret += vlink_create_publisher("dds://c_interface/schema_detect_null_cb", &schema, &valid_pub_handle);
    ret += vlink_detect_subscribers(valid_pub_handle, NULL, NULL) != VLINK_RET_INVALID_ERROR;
    ret += vlink_destroy_publisher(&valid_pub_handle);
  }

  {
    const vlink_schema_info_t schema = {"text", VLINK_SCHEMA_RAW};
    vlink_client_handle_t client_handle = {0};

    ret += vlink_create_client("dds://c_interface/schema_detect", &schema, &client_handle);
    ret += vlink_detect_server(client_handle, NULL, NULL) != VLINK_RET_INVALID_ERROR;
    ret += vlink_destroy_client(&client_handle);
  }

  return ret;
}

int test_security_standalone(void) {
  printf("test_security_standalone...\n");
  fflush(stdout);

  int ret = 0;

  vlink_security_config_t cfg;
  vlink_security_config_init(&cfg);
  cfg.key = "test_key";

  vlink_security_handle_t sender = vlink_security_create(&cfg);
  if (!sender) {
    printf("FAIL: vlink_security_create sender returned NULL\n");
    return 1;
  }

  vlink_security_handle_t receiver = vlink_security_create(&cfg);
  if (!receiver) {
    printf("FAIL: vlink_security_create receiver returned NULL\n");
    vlink_security_destroy(sender);
    return 1;
  }

  const char* plain = "hello vlink security";
  size_t plain_size = strlen(plain);

  uint8_t* cipher = NULL;
  size_t cipher_size = 0;
  int rc = vlink_security_encrypt(sender, (const uint8_t*)plain, plain_size, &cipher, &cipher_size);
  if (rc != VLINK_RET_NO_ERROR || cipher == NULL || cipher_size == 0) {
    printf("FAIL: vlink_security_encrypt rc=%d cipher=%p size=%zu\n", rc, (void*)cipher, cipher_size);
    vlink_security_free_buffer(cipher);
    vlink_security_destroy(sender);
    vlink_security_destroy(receiver);
    return 1;
  }

  uint8_t* recovered = NULL;
  size_t recovered_size = 0;
  rc = vlink_security_decrypt(receiver, cipher, cipher_size, &recovered, &recovered_size);
  if (rc != VLINK_RET_NO_ERROR || recovered == NULL || recovered_size != plain_size ||
      memcmp(recovered, plain, plain_size) != 0) {
    printf("FAIL: vlink_security_decrypt rc=%d size=%zu (expected %zu)\n", rc, recovered_size, plain_size);
    ret = 1;
  } else {
    printf("PASS: encrypt/decrypt round-trip\n");
  }

  vlink_security_free_buffer(cipher);
  vlink_security_free_buffer(recovered);

  uint8_t* tmp = NULL;
  size_t tmp_size = 0;
  rc = vlink_security_encrypt(NULL, (const uint8_t*)plain, plain_size, &tmp, &tmp_size);
  if (rc != VLINK_RET_INVALID_ERROR) {
    printf("FAIL: encrypt(NULL handle) rc=%d expected INVALID\n", rc);
    ret = 1;
  } else {
    printf("PASS: encrypt rejects null handle\n");
  }

  rc = vlink_security_encrypt(sender, (const uint8_t*)plain, plain_size, NULL, &tmp_size);
  if (rc != VLINK_RET_INVALID_ERROR) {
    printf("FAIL: encrypt(NULL out) rc=%d expected INVALID\n", rc);
    ret = 1;
  } else {
    printf("PASS: encrypt rejects null out\n");
  }

  vlink_security_config_t bad_cfg;
  vlink_security_config_init(&bad_cfg);
  bad_cfg.key = "wrong_key";
  vlink_security_handle_t bad_receiver = vlink_security_create(&bad_cfg);
  if (!bad_receiver) {
    printf("FAIL: vlink_security_create bad_receiver returned NULL\n");
    ret = 1;
  } else {
    uint8_t* cipher2 = NULL;
    size_t cipher2_size = 0;
    rc = vlink_security_encrypt(sender, (const uint8_t*)plain, plain_size, &cipher2, &cipher2_size);
    if (rc == VLINK_RET_NO_ERROR && cipher2 != NULL) {
      uint8_t* bad_plain = NULL;
      size_t bad_plain_size = 0;
      rc = vlink_security_decrypt(bad_receiver, cipher2, cipher2_size, &bad_plain, &bad_plain_size);
      if (rc == VLINK_RET_NO_ERROR && bad_plain != NULL && bad_plain_size == plain_size &&
          memcmp(bad_plain, plain, plain_size) == 0) {
        printf("FAIL: decrypt with wrong key recovered the plaintext\n");
        ret = 1;
      } else {
        printf("PASS: decrypt with wrong key fails\n");
      }
      vlink_security_free_buffer(bad_plain);
    }
    vlink_security_free_buffer(cipher2);
    vlink_security_destroy(bad_receiver);
  }

  vlink_security_destroy(sender);
  vlink_security_destroy(receiver);

  return ret;
}

int test_security_passphrase(void) {
  printf("test_security_passphrase...\n");
  fflush(stdout);

  int ret = 0;

  uint8_t salt[32];
  for (size_t i = 0; i < sizeof(salt); ++i) {
    salt[i] = (uint8_t)(i * 7u + 3u);
  }

  vlink_security_config_t cfg;
  vlink_security_config_init(&cfg);
  cfg.passphrase = "correct horse battery staple";
  cfg.pbkdf2_salt = salt;
  cfg.pbkdf2_salt_size = sizeof(salt);
  cfg.pbkdf2_iterations = 1000;

  vlink_security_handle_t sender = vlink_security_create(&cfg);
  vlink_security_handle_t receiver = vlink_security_create(&cfg);
  if (!sender || !receiver) {
    printf("FAIL: passphrase security_create returned NULL\n");
    vlink_security_destroy(sender);
    vlink_security_destroy(receiver);
    return 1;
  }

  const char* plain = "passphrase round trip payload";
  size_t plain_size = strlen(plain);

  uint8_t* cipher = NULL;
  size_t cipher_size = 0;
  int rc = vlink_security_encrypt(sender, (const uint8_t*)plain, plain_size, &cipher, &cipher_size);
  if (rc != VLINK_RET_NO_ERROR || cipher == NULL) {
    printf("FAIL: passphrase encrypt rc=%d\n", rc);
    vlink_security_free_buffer(cipher);
    vlink_security_destroy(sender);
    vlink_security_destroy(receiver);
    return 1;
  }

  uint8_t* recovered = NULL;
  size_t recovered_size = 0;
  rc = vlink_security_decrypt(receiver, cipher, cipher_size, &recovered, &recovered_size);
  if (rc != VLINK_RET_NO_ERROR || recovered_size != plain_size || memcmp(recovered, plain, plain_size) != 0) {
    printf("FAIL: passphrase decrypt rc=%d size=%zu\n", rc, recovered_size);
    ret = 1;
  } else {
    printf("PASS: passphrase round-trip\n");
  }

  vlink_security_free_buffer(cipher);
  vlink_security_free_buffer(recovered);
  vlink_security_destroy(sender);
  vlink_security_destroy(receiver);

  uint8_t short_salt[8];
  for (size_t i = 0; i < sizeof(short_salt); ++i) {
    short_salt[i] = (uint8_t)i;
  }

  vlink_security_config_t bad_cfg;
  vlink_security_config_init(&bad_cfg);
  bad_cfg.passphrase = "any";
  bad_cfg.pbkdf2_salt = short_salt;
  bad_cfg.pbkdf2_salt_size = sizeof(short_salt);
  bad_cfg.pbkdf2_iterations = 1000;

  vlink_security_handle_t bad = vlink_security_create(&bad_cfg);
  uint8_t* bad_cipher = NULL;
  size_t bad_cipher_size = 0;
  rc = vlink_security_encrypt(bad, (const uint8_t*)plain, plain_size, &bad_cipher, &bad_cipher_size);
  if (rc == VLINK_RET_NO_ERROR && bad_cipher != NULL && bad_cipher_size > 0) {
    printf("FAIL: short salt was accepted (encrypt succeeded)\n");
    ret = 1;
  } else {
    printf("PASS: short salt is rejected\n");
  }
  vlink_security_free_buffer(bad_cipher);
  vlink_security_destroy(bad);

  return ret;
}

void on_secure_sub_msg(const uint8_t* data, const size_t size, void* user_data) {
  if (!data || size == 0) {
    return;
  }
  const char* expected = "secure_event";
  if (size == strlen(expected) && memcmp(data, expected, size) == 0) {
    *(int*)user_data = 0;
  }
}

int test_security_callback_pubsub(void) {
  printf("test_security_callback_pubsub...\n");
  fflush(stdout);

  int ret = 0;
  int got = 1;
  const vlink_schema_info_t schema = {"text", VLINK_SCHEMA_RAW};

  vlink_security_config_t cfg;
  vlink_security_config_init(&cfg);
  cfg.key = "shared_pubsub_key";

  vlink_subscriber_handle_t sub_handle = {0};
  int rc = vlink_create_secure_subscriber("dds://c_interface/secure_event", &schema, &sub_handle, on_secure_sub_msg,
                                          &got, &cfg);
  if (rc != VLINK_RET_NO_ERROR) {
    printf("FAIL: create_secure_subscriber rc=%d\n", rc);
    return 1;
  }

  vlink_publisher_handle_t pub_handle = {0};
  rc = vlink_create_secure_publisher("dds://c_interface/secure_event", &schema, &pub_handle, &cfg);
  if (rc != VLINK_RET_NO_ERROR) {
    printf("FAIL: create_secure_publisher rc=%d\n", rc);
    vlink_destroy_subscriber(&sub_handle);
    return 1;
  }

  vlink_wait_for_subscribers(pub_handle, 5000);

  const char* msg = "secure_event";
  rc = vlink_publish(pub_handle, (const uint8_t*)msg, strlen(msg));
  if (rc != VLINK_RET_NO_ERROR) {
    printf("FAIL: publish rc=%d\n", rc);
    ret = 1;
  }

  for (int i = 0; i < 50 && got != 0; ++i) {
    custom_sleep(100);
  }

  if (got != 0) {
    printf("FAIL: encrypted message did not arrive at subscriber callback\n");
    ret = 1;
  } else {
    printf("PASS: encrypted pub/sub round-trip\n");
  }

  vlink_destroy_publisher(&pub_handle);
  vlink_destroy_subscriber(&sub_handle);

  return ret;
}

typedef struct {
  vlink_server_handle_t* handle;
  const char* expected_req;
  const char* response;
  int saw_request;
  int reply_rc;
} relocated_server_ctx_t;

static void on_relocated_server_req(const uint8_t* data, const size_t size, void* user_data) {
  relocated_server_ctx_t* ctx = (relocated_server_ctx_t*)user_data;

  if (!ctx || !ctx->handle || !ctx->expected_req || !ctx->response) {
    return;
  }

  if (data && size == strlen(ctx->expected_req) && memcmp(data, ctx->expected_req, size) == 0) {
    ctx->saw_request = 1;
  }

  ctx->reply_rc = vlink_reply(ctx->handle, (const uint8_t*)ctx->response, strlen(ctx->response));
}

int test_security_handle_storage_relocation(void) {
  printf("test_security_handle_storage_relocation...\n");
  fflush(stdout);

  int ret = 0;
  const vlink_schema_info_t schema = {"text", VLINK_SCHEMA_RAW};

  vlink_security_config_t cfg;
  vlink_security_config_init(&cfg);
  cfg.key = "secure_relocated_handle_key";

  expected_msg_ctx_t sub_ctx = {"relocated_event", 1};
  vlink_subscriber_handle_t* tmp_sub = (vlink_subscriber_handle_t*)malloc(sizeof(*tmp_sub));
  if (!tmp_sub) {
    printf("FAIL: malloc tmp_sub\n");
    return 1;
  }
  memset(tmp_sub, 0, sizeof(*tmp_sub));

  int rc = vlink_create_secure_subscriber("dds://c_interface/secure_relocated_event", &schema, tmp_sub,
                                          on_expected_msg, &sub_ctx, &cfg);
  if (rc != VLINK_RET_NO_ERROR) {
    printf("FAIL: create relocated secure subscriber rc=%d\n", rc);
    free(tmp_sub);
    return 1;
  }

  vlink_subscriber_handle_t sub = *tmp_sub;
  memset(tmp_sub, 0, sizeof(*tmp_sub));

  vlink_publisher_handle_t pub = {0};
  rc = vlink_create_secure_publisher("dds://c_interface/secure_relocated_event", &schema, &pub, &cfg);
  if (rc != VLINK_RET_NO_ERROR) {
    printf("FAIL: create relocated secure publisher rc=%d\n", rc);
    vlink_destroy_subscriber(&sub);
    free(tmp_sub);
    return 1;
  }

  vlink_wait_for_subscribers(pub, 5000);
  rc = vlink_publish(pub, (const uint8_t*)sub_ctx.expected, strlen(sub_ctx.expected));
  if (rc != VLINK_RET_NO_ERROR) {
    printf("FAIL: relocated secure publish rc=%d\n", rc);
    ret = 1;
  }

  for (int i = 0; i < 50 && sub_ctx.got != 0; ++i) {
    custom_sleep(100);
  }

  if (sub_ctx.got != 0) {
    printf("FAIL: relocated secure subscriber did not receive message\n");
    ret = 1;
  }

  vlink_destroy_publisher(&pub);
  vlink_destroy_subscriber(&sub);
  free(tmp_sub);

  vlink_server_handle_t* tmp_server = (vlink_server_handle_t*)malloc(sizeof(*tmp_server));
  if (!tmp_server) {
    printf("FAIL: malloc tmp_server\n");
    return ret + 1;
  }
  memset(tmp_server, 0, sizeof(*tmp_server));

  vlink_server_handle_t server = {0};
  relocated_server_ctx_t server_ctx = {&server, "relocated_req", "relocated_resp", 0, VLINK_RET_UNKNOWN_ERROR};
  rc = vlink_create_secure_server("intra://c_interface/secure_relocated_rpc#queue", &schema, tmp_server,
                                  on_relocated_server_req, &server_ctx, &cfg);
  if (rc != VLINK_RET_NO_ERROR) {
    printf("FAIL: create relocated secure server rc=%d\n", rc);
    free(tmp_server);
    return ret + 1;
  }

  server = *tmp_server;
  memset(tmp_server, 0, sizeof(*tmp_server));

  expected_msg_ctx_t resp_ctx = {"relocated_resp", 1};
  vlink_client_handle_t client = {0};
  rc = vlink_create_secure_client("intra://c_interface/secure_relocated_rpc#queue", &schema, &client, &cfg);
  if (rc != VLINK_RET_NO_ERROR) {
    printf("FAIL: create relocated secure client rc=%d\n", rc);
    vlink_destroy_server(&server);
    free(tmp_server);
    return ret + 1;
  }

  vlink_wait_for_server(client, 2000);
  rc = vlink_invoke(client, (const uint8_t*)server_ctx.expected_req, strlen(server_ctx.expected_req), on_expected_msg,
                    &resp_ctx);
  if (rc != VLINK_RET_NO_ERROR) {
    printf("FAIL: relocated secure invoke rc=%d\n", rc);
    ret = 1;
  }

  for (int i = 0; i < 50 && resp_ctx.got != 0; ++i) {
    custom_sleep(20);
  }

  if (!server_ctx.saw_request || server_ctx.reply_rc != VLINK_RET_NO_ERROR || resp_ctx.got != 0) {
    printf("FAIL: relocated secure RPC saw_request=%d reply_rc=%d got=%d\n", server_ctx.saw_request,
           server_ctx.reply_rc, resp_ctx.got);
    ret = 1;
  } else {
    printf("PASS: secure callbacks survive handle storage relocation\n");
  }

  vlink_destroy_client(&client);
  vlink_destroy_server(&server);
  free(tmp_server);

  return ret;
}

int test_security_getter_poll_reuses_cached_plaintext(void) {
  printf("test_security_getter_poll_reuses_cached_plaintext...\n");
  fflush(stdout);

  int ret = 0;
  const vlink_schema_info_t schema = {"text", VLINK_SCHEMA_RAW};

  vlink_security_config_t cfg;
  vlink_security_config_init(&cfg);
  cfg.key = "secure_field_poll_key";

  vlink_setter_handle_t setter = {0};
  int rc = vlink_create_secure_setter("intra://c_interface/secure_field_poll", &schema, &setter, &cfg);
  if (rc != VLINK_RET_NO_ERROR) {
    printf("FAIL: create_secure_setter rc=%d\n", rc);
    return 1;
  }

  vlink_getter_handle_t getter = {0};
  rc = vlink_create_secure_getter("intra://c_interface/secure_field_poll", &schema, &getter, NULL, NULL, &cfg);
  if (rc != VLINK_RET_NO_ERROR) {
    printf("FAIL: create_secure_getter rc=%d\n", rc);
    vlink_destroy_setter(&setter);
    return 1;
  }

  const char* msg = "secure_field_poll";
  rc = vlink_set(setter, (const uint8_t*)msg, strlen(msg));
  if (rc != VLINK_RET_NO_ERROR) {
    printf("FAIL: secure vlink_set rc=%d\n", rc);
    ret = 1;
  }

  custom_sleep(100);

  for (int i = 0; i < 2; ++i) {
    uint8_t buf[64];
    size_t size = sizeof(buf);
    rc = vlink_get(getter, buf, &size);
    if (rc != VLINK_RET_NO_ERROR || size != strlen(msg) || memcmp(buf, msg, strlen(msg)) != 0) {
      printf("FAIL: secure vlink_get #%d rc=%d size=%zu\n", i + 1, rc, size);
      ret = 1;
    }
  }

  if (ret == 0) {
    printf("PASS: secure getter polling reuses cached plaintext without replay rejection\n");
  }

  vlink_destroy_getter(&getter);
  vlink_destroy_setter(&setter);

  return ret;
}

typedef struct {
  vlink_server_handle_t* handle;
  int reply_mode;
  int saw_request;
  int reply_rc;
} secure_empty_reply_ctx_t;

static void on_secure_empty_reply_req(const uint8_t* data, const size_t size, void* user_data) {
  secure_empty_reply_ctx_t* ctx = (secure_empty_reply_ctx_t*)user_data;
  const char* expected = "empty_reply_req";

  if (ctx && data && size == strlen(expected) && memcmp(data, expected, size) == 0) {
    ctx->saw_request = 1;
  }

  if (ctx && ctx->reply_mode != 0) {
    ctx->reply_rc = vlink_reply(ctx->handle, NULL, 0);
  }
}

static void on_secure_empty_reply_resp(const uint8_t* data, const size_t size, void* user_data) {
  int* got_empty = (int*)user_data;

  if (got_empty && data == NULL && size == 0) {
    *got_empty = 0;
  }
}

static int run_security_empty_reply_case(const char* url, int reply_mode) {
  int ret = 0;
  int got_empty = 1;
  const vlink_schema_info_t schema = {"text", VLINK_SCHEMA_RAW};

  vlink_security_config_t cfg;
  vlink_security_config_init(&cfg);
  cfg.key = "secure_rpc_empty_key";

  vlink_server_handle_t server = {0};
  secure_empty_reply_ctx_t ctx = {&server, reply_mode, 0, VLINK_RET_UNKNOWN_ERROR};
  int rc = vlink_create_secure_server(url, &schema, &server, on_secure_empty_reply_req, &ctx, &cfg);
  if (rc != VLINK_RET_NO_ERROR) {
    printf("FAIL: create_secure_server rc=%d\n", rc);
    return 1;
  }

  vlink_client_handle_t client = {0};
  rc = vlink_create_secure_client(url, &schema, &client, &cfg);
  if (rc != VLINK_RET_NO_ERROR) {
    printf("FAIL: create_secure_client rc=%d\n", rc);
    vlink_destroy_server(&server);
    return 1;
  }

  vlink_wait_for_server(client, 2000);

  const char* req = "empty_reply_req";
  rc = vlink_invoke(client, (const uint8_t*)req, strlen(req), on_secure_empty_reply_resp, &got_empty);
  if (rc != VLINK_RET_NO_ERROR) {
    printf("FAIL: secure empty reply invoke rc=%d\n", rc);
    ret = 1;
  }

  custom_sleep(100);

  if (!ctx.saw_request) {
    printf("FAIL: secure empty reply request callback not reached\n");
    ret = 1;
  }

  if (reply_mode != 0 && ctx.reply_rc != VLINK_RET_NO_ERROR) {
    printf("FAIL: vlink_reply(NULL, 0) rc=%d\n", ctx.reply_rc);
    ret = 1;
  }

  if (got_empty != 0) {
    printf("FAIL: secure empty reply response callback did not receive empty payload\n");
    ret = 1;
  }

  vlink_destroy_client(&client);
  vlink_destroy_server(&server);

  return ret;
}

int test_security_empty_reply(void) {
  printf("test_security_empty_reply...\n");
  fflush(stdout);

  int ret = 0;

  ret += run_security_empty_reply_case("intra://c_interface/secure_empty_no_reply", 0);
  ret += run_security_empty_reply_case("intra://c_interface/secure_empty_explicit_reply", 1);

  if (ret == 0) {
    printf("PASS: secure RPC preserves empty response protocol\n");
  }

  return ret;
}

int test_security_tamper(void) {
  printf("test_security_tamper...\n");
  fflush(stdout);

  int ret = 0;

  vlink_security_config_t cfg;
  vlink_security_config_init(&cfg);
  cfg.key = "tamper_test_key";

  vlink_security_handle_t sec = vlink_security_create(&cfg);
  if (!sec) {
    printf("FAIL: vlink_security_create returned NULL\n");
    return 1;
  }

  uint8_t plain[64];
  for (size_t i = 0; i < sizeof(plain); ++i) {
    plain[i] = (uint8_t)i;
  }

  uint8_t* cipher = NULL;
  size_t cipher_size = 0;
  int rc = vlink_security_encrypt(sec, plain, sizeof(plain), &cipher, &cipher_size);
  if (rc != VLINK_RET_NO_ERROR || cipher == NULL || cipher_size < sizeof(plain) + 50u) {
    printf("FAIL: encrypt rc=%d size=%zu\n", rc, cipher_size);
    vlink_security_free_buffer(cipher);
    vlink_security_destroy(sec);
    return 1;
  }

  /* Flip exactly one bit at three structural positions: envelope start, ciphertext mid, tag tail. */
  const size_t positions[3] = {0u, cipher_size / 2u, cipher_size - 1u};
  for (size_t pi = 0; pi < 3u; ++pi) {
    uint8_t* tampered = (uint8_t*)malloc(cipher_size);
    if (!tampered) {
      printf("FAIL: malloc failed for tampered buffer\n");
      ret = 1;
      continue;
    }
    memcpy(tampered, cipher, cipher_size);
    tampered[positions[pi]] ^= 0x01u;

    uint8_t* recovered = NULL;
    size_t recovered_size = 0;
    rc = vlink_security_decrypt(sec, tampered, cipher_size, &recovered, &recovered_size);
    if (rc == VLINK_RET_NO_ERROR) {
      printf("FAIL: tampered byte at offset %zu produced rc=NO_ERROR\n", positions[pi]);
      ret = 1;
    } else {
      printf("PASS: tampered byte at offset %zu is rejected (rc=%d)\n", positions[pi], rc);
    }
    vlink_security_free_buffer(recovered);
    free(tampered);
  }

  vlink_security_free_buffer(cipher);
  vlink_security_destroy(sec);

  return ret;
}

int test_ssl_options_init(void) {
  printf("test_ssl_options_init...\n");
  fflush(stdout);

  int ret = 0;

  vlink_ssl_options_t opt;
  memset(&opt, 0xAB, sizeof(opt));
  vlink_ssl_options_init(&opt);

  if (opt.verify_peer != 1) {
    printf("FAIL: verify_peer=%d expected 1\n", opt.verify_peer);
    ret = 1;
  } else {
    printf("PASS: verify_peer default is 1\n");
  }

  if (opt.ca_file != NULL) {
    printf("FAIL: ca_file not NULL\n");
    ret = 1;
  } else {
    printf("PASS: ca_file is NULL\n");
  }

  if (opt.cert_file != NULL || opt.key_file != NULL || opt.key_password != NULL || opt.server_name != NULL ||
      opt.ciphers != NULL) {
    printf("FAIL: one or more string fields not NULL\n");
    ret = 1;
  } else {
    printf("PASS: all string fields are NULL\n");
  }

  vlink_ssl_options_init(NULL);
  printf("PASS: vlink_ssl_options_init(NULL) is a no-op\n");

  opt.ca_file = "ca.pem";

  const vlink_schema_info_t schema = {"text", VLINK_SCHEMA_RAW};
  vlink_publisher_handle_t pub = {0};
  int rc = vlink_create_publisher_with_ssl_options("intra://c_interface/ssl_create", &schema, &pub, &opt);
  if (rc != VLINK_RET_NO_ERROR) {
    printf("FAIL: create_publisher_with_ssl_options rc=%d\n", rc);
    ret = 1;
  } else {
    printf("PASS: create-time ssl publisher succeeds\n");
    vlink_destroy_publisher(&pub);
  }

  memset(&pub, 0, sizeof(pub));
  rc = vlink_create_publisher("intra://c_interface/ssl_late_setter", &schema, &pub);
  if (rc != VLINK_RET_NO_ERROR) {
    printf("FAIL: create publisher for late ssl setter rc=%d\n", rc);
    ret = 1;
  } else {
    rc = vlink_publisher_set_ssl_options(&pub, &opt);
    if (rc != VLINK_RET_RUNTIME_ERROR) {
      printf("FAIL: late publisher ssl setter rc=%d expected %d\n", rc, VLINK_RET_RUNTIME_ERROR);
      ret = 1;
    } else {
      printf("PASS: late ssl setter reports runtime error\n");
    }
    vlink_destroy_publisher(&pub);
  }

  return ret;
}

int test_schema_type_mapping(void);
int test_ssl_create_options_properties(void);

int main(int argc, char* argv[]) {
  (void)argc;
  (void)argv;

  int ret = 1;

  ret = test_pub_sub();

  ret += test_intra_queue_publish_buffer_reuse();

  ret += test_server_client();

  ret += test_setter_getter();

  ret += test_schema_info_validation();

  ret += test_schema_type_mapping();

#ifdef VLINK_ENABLE_SECURITY
  ret += test_security_standalone();
  ret += test_security_passphrase();
  ret += test_security_tamper();
  ret += test_security_callback_pubsub();
  ret += test_security_handle_storage_relocation();
  ret += test_security_getter_poll_reuses_cached_plaintext();
  ret += test_security_empty_reply();
  ret += test_ssl_options_init();
  ret += test_ssl_create_options_properties();
#endif

  return ret;
}

// NOLINTEND
