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

#include <vlink/external/c_api.h>
#include <vlink/vlink.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <future>
#include <thread>

extern "C" int test_schema_type_mapping(void) {
  struct Case final {
    const char* url;
    const char* ser;
    vlink_schema_t schema;
    vlink::SchemaType expected;
  };

  const Case cases[] = {
      {"dds://c_interface/schema_type/protobuf", "demo.proto.PointCloud", VLINK_SCHEMA_PROTOBUF,
       vlink::SchemaType::kProtobuf},
      {"dds://c_interface/schema_type/flatbuffers", "demo.fbs.PointCloud", VLINK_SCHEMA_FLATBUFFERS,
       vlink::SchemaType::kFlatbuffers},
      {"dds://c_interface/schema_type/raw", "text", VLINK_SCHEMA_RAW, vlink::SchemaType::kRaw},
      {"dds://c_interface/schema_type/zerocopy", "vlink::zerocopy::RawData", VLINK_SCHEMA_ZEROCOPY,
       vlink::SchemaType::kZeroCopy},
      {"dds://c_interface/schema_type/cdr", "dds::CInterfaceMessage", VLINK_SCHEMA_CDR, vlink::SchemaType::kCdr},
  };

  int ret = 0;
  std::printf("test_schema_type_mapping...\n");
  std::fflush(stdout);

  for (const auto& test_case : cases) {
    vlink_publisher_handle_t pub_handle{};
    const vlink_schema_info_t schema_info = {test_case.ser, test_case.schema};

    ret += vlink_create_publisher(test_case.url, &schema_info, &pub_handle);

    auto* publisher = static_cast<vlink::Publisher<vlink::Bytes>*>(pub_handle.native_handle);

    if (publisher == nullptr || publisher->get_schema_type() != test_case.expected ||
        publisher->get_ser_type() != test_case.ser) {
      ++ret;
    }

    if (pub_handle.native_handle != nullptr) {
      ret += vlink_destroy_publisher(&pub_handle);
    }
  }

  return ret;
}

extern "C" int test_ssl_create_options_properties(void) {
  std::printf("test_ssl_create_options_properties...\n");
  std::fflush(stdout);

  int ret = 0;
  const vlink_schema_info_t schema_info = {"text", VLINK_SCHEMA_RAW};

  vlink_ssl_options_t opt;
  vlink_ssl_options_init(&opt);
  opt.verify_peer = 0;
  opt.ca_file = "ca.pem";
  opt.cert_file = "cert.pem";
  opt.key_file = "key.pem";
  opt.key_password = "secret";
  opt.server_name = "mqtt.example.test";
  opt.ciphers = "TLS_AES_128_GCM_SHA256";

  vlink_publisher_handle_t pub_handle{};
  int rc =
      vlink_create_publisher_with_ssl_options("intra://c_interface/ssl_properties", &schema_info, &pub_handle, &opt);

  if (rc != VLINK_RET_NO_ERROR) {
    std::printf("FAIL: create_publisher_with_ssl_options rc=%d\n", rc);
    return 1;
  }

  auto* publisher = static_cast<vlink::Publisher<vlink::Bytes>*>(pub_handle.native_handle);

  if (publisher == nullptr || publisher->get_property("ssl.verify") != "0" ||
      publisher->get_property("ssl.ca") != "ca.pem" || publisher->get_property("ssl.cert") != "cert.pem" ||
      publisher->get_property("ssl.key") != "key.pem" || publisher->get_property("ssl.key_password") != "secret" ||
      publisher->get_property("ssl.server_name") != "mqtt.example.test" ||
      publisher->get_property("ssl.ciphers") != "TLS_AES_128_GCM_SHA256") {
    std::printf("FAIL: create-time ssl properties were not applied before init\n");
    ret = 1;
  } else {
    std::printf("PASS: create-time ssl properties are applied before init\n");
  }

  if (pub_handle.native_handle != nullptr) {
    ret += vlink_destroy_publisher(&pub_handle);
  }

  return ret;
}

#ifdef VLINK_ENABLE_SECURITY
extern "C" int test_security_getter_concurrent_read(void) {
  std::printf("test_security_getter_concurrent_read...\n");
  struct Context final {
    std::promise<void> entered;
    std::shared_future<void> release;
    bool wait_for_b{true};
    size_t received{0};
  } context;
  std::promise<void> release;
  context.release = release.get_future().share();
  auto entered = context.entered.get_future();
  vlink_msg_callback_t callback = [](const uint8_t* data, size_t size, void* user) {
    auto& ctx = *static_cast<Context*>(user);
    ++ctx.received;

    if (size == 2 && data[0] == 'B' && ctx.wait_for_b) {
      ctx.wait_for_b = false;
      ctx.entered.set_value();
      ctx.release.wait_for(std::chrono::seconds(5));
    }
  };

  const char* url = "intra://c_interface/secure_concurrent#direct";
  const vlink_schema_info_t schema = {"text", VLINK_SCHEMA_RAW};
  vlink_security_config_t cfg;
  vlink_security_config_init(&cfg);
  cfg.key = "secure_concurrent";
  vlink_setter_handle_t setter{};
  vlink_getter_handle_t getter{};
  vlink_getter_handle_t poller{};
  const uint8_t first[] = {'A', 'a'};
  const uint8_t second[] = {'B', 'b'};
  uint8_t output[2]{};
  size_t size = sizeof(output);
  int ret = 0;

  if (vlink_create_secure_setter(url, &schema, &setter, &cfg) != VLINK_RET_NO_ERROR) {
    return 1;
  }

  ret += vlink_set(setter, first, sizeof(first)) != VLINK_RET_NO_ERROR;

  if (vlink_create_secure_getter(url, &schema, &getter, callback, &context, &cfg) != VLINK_RET_NO_ERROR) {
    vlink_destroy_setter(&setter);
    return 1;
  }

  ret += context.received != 1;
  ret += vlink_get(getter, output, &size) != VLINK_RET_NO_ERROR || size != sizeof(first) ||
         std::memcmp(output, first, sizeof(first)) != 0;
  int set_result = VLINK_RET_UNKNOWN_ERROR;
  std::thread sender([&] { set_result = vlink_set(setter, second, sizeof(second)); });

  if (entered.wait_for(std::chrono::seconds(5)) == std::future_status::ready) {
    size = 1;
    ret += vlink_get(getter, output, &size) != VLINK_RET_MEMORY_ERROR || size != sizeof(second);
    ret += vlink_get(getter, output, &size) != VLINK_RET_NO_ERROR || std::memcmp(output, second, sizeof(second)) != 0;
  } else {
    ++ret;
  }

  release.set_value();
  sender.join();
  ret += set_result != VLINK_RET_NO_ERROR;

  if (vlink_create_secure_getter(url, &schema, &poller, nullptr, nullptr, &cfg) == VLINK_RET_NO_ERROR) {
    size = sizeof(output);
    ret += vlink_get(getter, output, &size) != VLINK_RET_NO_ERROR || size != sizeof(second) ||
           std::memcmp(output, second, sizeof(second)) != 0;

    std::atomic<int> errors{0};
    auto read_values = [&] {
      for (size_t i = 0; i < 1000; ++i) {
        uint8_t value[2]{};
        size_t capacity = sizeof(value);

        if (vlink_get(poller, value, &capacity) != VLINK_RET_NO_ERROR || capacity != sizeof(value)) {
          errors.fetch_add(1, std::memory_order_relaxed);
        }
      }
    };
    std::thread reader1(read_values);
    std::thread reader2(read_values);

    for (uint8_t i = 0; i < 100; ++i) {
      const uint8_t value[] = {i, i};
      ret += vlink_set(setter, value, sizeof(value)) != VLINK_RET_NO_ERROR;
    }

    reader1.join();
    reader2.join();
    ret += errors.load(std::memory_order_relaxed) != 0;

    auto* raw_setter = static_cast<vlink::Setter<vlink::Bytes>*>(setter.native_handle);
    raw_setter->set(vlink::Bytes{0x01});
    size = sizeof(output);
    ret += vlink_get(getter, output, &size) != VLINK_RET_TRANSFER_ERROR;
    ret += vlink_get(poller, output, &size) != VLINK_RET_TRANSFER_ERROR;
    vlink_destroy_getter(&poller);
  } else {
    ++ret;
  }

  vlink_destroy_getter(&getter);
  vlink_destroy_setter(&setter);

  cfg.encrypt_callback = [](const uint8_t* in, size_t length, uint8_t** out, size_t* out_size, void*) {
    *out = static_cast<uint8_t*>(std::malloc(length));

    if (!*out) {
      return 1;
    }

    std::memcpy(*out, in, length);
    *out_size = length;
    return 0;
  };
  cfg.decrypt_callback = [](const uint8_t*, size_t, uint8_t** out, size_t* out_size, void*) {
    *out = static_cast<uint8_t*>(std::malloc(1));
    *out_size = 0;
    return *out ? 0 : 1;
  };

  if (vlink_create_secure_setter(url, &schema, &setter, &cfg) != VLINK_RET_NO_ERROR) {
    return ret + 1;
  }

  ret += vlink_set(setter, first, sizeof(first)) != VLINK_RET_NO_ERROR;

  for (bool push : {false, true}) {
    if (vlink_create_secure_getter(url, &schema, &getter, push ? callback : nullptr, &context, &cfg) !=
        VLINK_RET_NO_ERROR) {
      ++ret;
      continue;
    }

    for (int i = 0; i < 2; ++i) {
      size = sizeof(output);
      ret += vlink_get(getter, output, &size) != VLINK_RET_NO_ERROR || size != 0;
    }

    vlink_destroy_getter(&getter);
  }

  vlink_destroy_setter(&setter);
  std::printf("%s: secure getter initial / concurrent / invalid / empty values\n", ret == 0 ? "PASS" : "FAIL");
  return ret;
}
#endif
