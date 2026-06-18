# c_api: c_security — C 接口的应用层加密

本示例用纯 C 调用 vlink 的应用层加密：独立 `vlink_security_create` / `_encrypt` / `_decrypt` 完成 round-trip；以及 `vlink_create_secure_publisher` / `_subscriber` / `_server` / `_client` / `_setter` / `_getter` 6 种安全节点的一站式构造。

读完本示例你能掌握：

- C API 的 Security handle 生命周期。
- `vlink_security_config_t` 各字段语义（与 C++ `Security::Config` 一一对应）。
- 在 C 里使用 AES-128-GCM、PBKDF2、RSA-OAEP、自定义回调。
- 6 种安全节点构造模式。

## 背景与适用场景

适用：

- 纯 C 项目接入 vlink 加密能力。
- 嵌入式 / 实时系统不能用 C++。
- 跨语言绑定（Python / Rust ffi）的中间层。

加密算法：

- 默认：AES-128-GCM（AEAD）+ envelope AAD + 序列号 nonce + 16 字节 tag。
- RSA-OAEP hybrid：填 `public_key_pem` / `private_key_pem`。
- RSA-PSS 签名：填 `advanced.signing_key_pem` / `verify_key_pem`。

## 核心 API

| API | 说明 |
|-----|------|
| `vlink_security_config_init(cfg)` | 初始化 Config 为默认值 |
| `vlink_security_create(cfg)` | 构造 handle |
| `vlink_security_destroy(handle)` | 销毁 |
| `vlink_security_encrypt(handle, plain, n, &cipher, &n_cipher)` | 加密；buffer 由 vlink 分配 |
| `vlink_security_decrypt(handle, cipher, n, &plain, &n_plain)` | 解密 |
| `vlink_security_free_buffer(buf)` | 释放 vlink 分配的 buffer |
| `vlink_create_secure_publisher(url, schema, sec_cfg, &handle)` | 一步式安全 publisher |
| `vlink_create_secure_subscriber(url, schema, sec_cfg, &handle, cb, ud)` | 安全 subscriber |
| 同上 `_server / _client / _setter / _getter` | 其它 4 种安全节点 |

## Config 字段

```c
typedef struct {
  const char* aad_context;
  uint32_t    replay_window;        // default 4096; 0 disables
  const char* signing_key_pem;
  const char* verify_key_pem;
} vlink_security_advanced_config_t;

typedef struct {
  const char*    key;                 // raw symmetric seed
  const char*    passphrase;          // PBKDF2 passphrase
  const uint8_t* pbkdf2_salt;
  size_t         pbkdf2_salt_size;
  uint32_t       pbkdf2_iterations;
  const char*    public_key_pem;
  const char*    private_key_pem;
  vlink_security_encrypt_callback encrypt_callback;
  vlink_security_decrypt_callback decrypt_callback;
  void*          callback_user_data;
  vlink_security_advanced_config_t advanced;
} vlink_security_config_t;
```

字段语义与 C++ `Security::Config` 一一对应；详见 `security/security_basic/README.md`。

## 代码导读

### 1. 独立 encrypt / decrypt

```c
vlink_security_config_t cfg;
vlink_security_config_init(&cfg);
cfg.key = "my-secret-key-16";

vlink_security_handle_t sec = vlink_security_create(&cfg);
if (!sec) {
  printf("create failed\n");
  return 1;
}

const char* plain = "hello vlink";
size_t plain_size = strlen(plain);

uint8_t* cipher = NULL;
size_t cipher_size = 0;
int ret = vlink_security_encrypt(sec, (const uint8_t*)plain, plain_size, &cipher, &cipher_size);

uint8_t* recovered = NULL;
size_t recovered_size = 0;
ret = vlink_security_decrypt(sec, cipher, cipher_size, &recovered, &recovered_size);
printf("recovered: %.*s\n", (int)recovered_size, recovered);

vlink_security_free_buffer(cipher);
vlink_security_free_buffer(recovered);
vlink_security_destroy(sec);
```

注意：vlink 分配的 buffer 必须用 `vlink_security_free_buffer` 释放，**不要** free()。

### 2. Replay 防御

```c
ret = vlink_security_decrypt(sec, cipher, cipher_size, &replay_plain, &replay_plain_size);
// 第二次解密同一密文 → 返回 ERR_REPLAY（取决于 replay_window 配置）
```

### 3. 安全 Publisher

```c
vlink_security_config_t sec_cfg;
vlink_security_config_init(&sec_cfg);
sec_cfg.key = "topic-key-1234567";

vlink_publisher_handle_t pub;
vlink_schema_info_t schema = { "MyMsg", VLINK_SCHEMA_RAW };
int ret = vlink_create_secure_publisher("dds://demo/topic", &schema, &sec_cfg, &pub);
if (ret == VLINK_RET_NO_ERROR) {
  vlink_wait_for_subscribers(pub, 2000);
  vlink_publish(pub, (const uint8_t*)"hi", 2);
  vlink_destroy_publisher(&pub);
}
```

### 4. 安全 Subscriber

```c
void on_msg(const uint8_t* data, size_t size, void* ud) {
  printf("got: %.*s\n", (int)size, data);
}

vlink_subscriber_handle_t sub;
vlink_create_secure_subscriber("dds://demo/topic", &schema, &sec_cfg, &sub, on_msg, NULL);
// ... 一段时间后 ...
vlink_destroy_subscriber(&sub);
```

### 5. 其它 4 种节点

```c
// Server
vlink_server_handle_t srv;
vlink_create_secure_server(url, &schema, &sec_cfg, &srv, on_request_cb, ud);

// Client
vlink_client_handle_t cli;
vlink_create_secure_client(url, &schema, &sec_cfg, &cli);

// Setter / Getter
vlink_setter_handle_t setter;
vlink_create_secure_setter(url, &schema, &sec_cfg, &setter);
vlink_getter_handle_t getter;
vlink_create_secure_getter(url, &schema, &sec_cfg, &getter, on_value_cb, ud);
```

## 运行

```bash
./build/output/bin/example_c_security

# 跑包含 shm:// 的部分（需要 RouDi）
iox-roudi &
VLINK_C_SECURITY_RUN_SHM=1 ./build/output/bin/example_c_security
```

shm 部分默认跳过，避免 RouDi 缺失时 Iceoryx 直接 abort。

预期输出（节选）：

```
[encrypt/decrypt] recovered: hello vlink
[replay] second decrypt -> blocked by replay window
[publisher] dds://demo/topic sent
[subscriber] got: hi
```

## 常见陷阱

1. **buffer 用 free 而非 vlink_security_free_buffer**：堆 corruption。
2. **handle 析构后再用**：UB；按 RAII 风格 destroy 之前完成全部操作。
3. **跨进程 replay_window 不一致**：可能导致合法消息被误判 replay。
4. **PBKDF2 salt 跨端不一致**：两端派生出的密钥不同；加密成功但解密失败。
5. **C API 不接受 C++ 类型**：不要传 std::string；用 const char*。

## 设计要点

- C handle 内部封装 `std::shared_ptr<vlink::Security>`；ABI 稳定。
- encrypt/decrypt buffer 由 vlink 内部 malloc；用 `vlink_security_free_buffer` 配套释放。
- `vlink_create_secure_*` 同时构造节点 + Security，单一调用完成；适合 C 风格。

## 配图

![C API security flow](./images/c-api-security-flow.png)

图中展示 C 端从 init → create → encrypt/decrypt → destroy 的完整流程，含 secure publisher/subscriber 集成。

## 参考

- `../c_pubsub/` / `../c_rpc/` / `../c_field/` — 其它 C API 示例
- `../../security/security_basic/` — C++ 等价示例
- `vlink/include/vlink/external/c_api.h` — C API 完整头
- 顶层 `doc/09-security.md` — 加密章节
- 顶层 `doc/18-c-api.md` — C API 章节
