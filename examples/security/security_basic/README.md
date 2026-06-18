# security_basic — 内置 AES-128-GCM 端到端加密

本示例展示 vlink 加密扩展的最常见用法：使用 `SecurityPublisher` 与 `SecuritySubscriber` 自动完成 AES-128-GCM
认证加密。示例包含四个独立场景：原始对称密钥、PBKDF2 口令派生、密钥不匹配的失败演示、以及 `vlink::Bytes`
载荷加密。最后用一段日志总结当前编译选项下支持/不支持的传输类型。

## 背景与适用场景

vlink 提供两层安全机制：传输层 TLS（由 `SslOptions` 控制）与应用层 AEAD（由本扩展提供）。前者保证链路在
线缆/网络上的机密性，后者保证消息在跨进程、跨节点、跨语言路由过程中的完整性与机密性。本示例覆盖第二层。

`SecurityPublisher<T>` 与 `SecuritySubscriber<T>` 是 `Publisher<T>` / `Subscriber<T>` 的安全包装类，构造时多接收
一个 `Security::Config`。发送侧每条消息都会被封装成「AAD 信封 + 12 字节 nonce + 密文 + 16 字节 GCM tag」，
接收侧通过 AAD 上下文与序列号防止重放，所有这些都隐藏在 `publish()` / `listen()` 调用背后。

什么时候选它：跨进程拓扑、多语言生态、需要审计每条消息独立完整性的场景。什么时候不用它：单进程
（`intra://`）内部通信无需加密、或者对端是不支持本扩展的纯 DDS-CDR 服务（应该改用 DDS-Security）。

## 核心 API

| API | 签名 | 说明 |
|-----|------|------|
| `vlink::SecurityPublisher<T>` | `SecurityPublisher(std::string url, Security::Config cfg)` | 加密版 `Publisher<T>` |
| `vlink::SecuritySubscriber<T>` | `SecuritySubscriber(std::string url, Security::Config cfg)` | 解密版 `Subscriber<T>` |
| `Security::Config::key` | `std::string` | 原始对称种子；SHA-256 截断到 16 字节作为 AES-128 密钥 |
| `Security::Config::passphrase` | `std::string` | 低熵口令；与 `pbkdf2_salt` / `pbkdf2_iterations` 合并经 PBKDF2-HMAC-SHA256 派生 |
| `Security::Config::pbkdf2_salt` | `vlink::Bytes` | 部署级盐值，长度 ≥ 16 字节，需带外共享 |
| `Security::Config::pbkdf2_iterations` | `uint32_t` | PBKDF2 迭代次数（默认 200000） |
| `Publisher::wait_for_subscribers` | `bool wait_for_subscribers(std::chrono::milliseconds = kDefault)` | 等待订阅方就绪后再发布 |
| `Subscriber::listen` | `void listen(callback)` | 注册消息回调 |
| `vlink::Bytes::from_string` | `static Bytes from_string(std::string_view)` | 从字符串构造字节缓冲 |
| `vlink::Bytes::create` | `static Bytes create(size_t)` | 分配指定大小的可写缓冲 |

## 代码导读

### 1. 原始对称密钥

发送方和接收方共享一个 16 字节种子串，两端构造完全对称的 `Security::Config`：

```cpp
vlink::Security::Config sub_cfg;
sub_cfg.key = "my-secret-key-16";
vlink::SecuritySubscriber<std::string> sub("dds://security_basic/raw_key", sub_cfg);
sub.listen([&](const std::string& msg) { /* ... */ });

vlink::Security::Config pub_cfg;
pub_cfg.key = "my-secret-key-16";
vlink::SecurityPublisher<std::string> pub("dds://security_basic/raw_key", pub_cfg);
pub.wait_for_subscribers();
pub.publish("Hello with AES-128-GCM!");
```

`key` 字段被 SHA-256 哈希后取前 16 字节作为最终的 AES-128 密钥，所以两端 `key` 必须完全一致（包括尾部空白
与编码方式）。每条消息使用单调递增的 96-bit nonce，避免重放。

### 2. PBKDF2 口令派生

人类记忆的口令熵太低，不能直接喂给 AES。本节用 PBKDF2-HMAC-SHA256 把口令拉伸到 16 字节密钥：

```cpp
const vlink::Bytes shared_salt = vlink::Bytes::from_string("vlink-example-salt-v1");

vlink::Security::Config cfg;
cfg.passphrase = "correct horse battery staple";
cfg.pbkdf2_salt = shared_salt;
cfg.pbkdf2_iterations = 200000U;
```

盐值与迭代次数必须两端一致。盐至少 16 字节，建议每次部署独立生成；迭代次数 100k–500k 通常足够，要求
两端在派生密钥时使用相同参数，不然会得到完全不同的 AES 密钥。

### 3. 密钥不匹配演示

人为构造一组不同的 16 字节种子，观察接收回调是否会触发：

```cpp
sub_cfg.key = "beta--key-16byte";
pub_cfg.key = "alpha-key-16byte";
pub.publish("This message should fail GCM authentication");
```

GCM 的认证 tag 不匹配时，解密在底层即返回 `false`，订阅回调不会被调用。日志里 `received == 0` 即为预期行为。

### 4. 字节载荷加密

`SecurityPublisher<vlink::Bytes>` 模板专门用于已经序列化好的二进制流（如 zerocopy 容器、Proto 编码后字节等）：

```cpp
vlink::Bytes data = vlink::Bytes::create(256);
for (size_t i = 0; i < 256; ++i) {
  data[i] = static_cast<uint8_t>(i);
}
pub.publish(data);
```

接收端通过 `msg.size()` 与 `msg.data()` 获取明文字节。

### 5. 传输限制总结

第 5 段只是打印一段文本提醒：`intra://` 与 CDR-DDS 不支持加密层；可用的传输为 `shm://`、`shm2://`、`zenoh://`、
`mqtt://`、`fdbus://` 等需要序列化/反序列化语义独立完成的协议。

## 运行

```bash
./build/output/bin/example_security_basic
```

预期输出（节选）：

```
[1] Symmetric Raw Key (AES-128-GCM)
[Raw Key] Received: Hello with AES-128-GCM!
[Raw Key] Received: Authenticated encryption is automatic
Raw key: received 3 messages
[2] PBKDF2 Passphrase (AES-128-GCM)
[Passphrase] Received: Passphrase-derived AES key in use
Passphrase: received 2 messages
[3] Key Mismatch Failure Demo
Key mismatch: received 0 messages (expected 0)
[5] Security Limitations
  Not supported: intra://, dds:// with CDR serialization
Security basic example complete.
```

示例使用 `dds://` URL 是为了让示例脱离 SHM 也能跑通；生产中应使用支持加密的传输（见上文）。

## 常见陷阱

1. `key` 字符串末尾的换行/空格会改变 SHA-256 结果，进而得到不同的 AES 密钥；务必把 16 字节种子当成字节序列对待。
2. PBKDF2 的 `pbkdf2_salt` 不可省略；空盐会导致两端派生密钥失败，`is_configured()` 返回 false。
3. 不要把 `Security::Config` 在构造完 `SecurityPublisher` 后再修改：配置是构造期一次性快照，运行时改 cfg 无效。
4. `wait_for_subscribers()` 之后再 `publish()`，否则发布过早可能直接被传输层丢弃，看上去像加密失败。
5. AAD 上下文 `advanced.aad_context` 也是认证范围的一部分，两端必须一致；本示例未显式设置（即空字符串）。

## 设计要点

- AEAD 是首选：相比单独 AES-CTR + HMAC，AES-GCM 把加密与认证合并为一次操作，CPU 与代码路径都更短。
- 配置一次性：`Security` 内部用互斥锁保护构造期解析，运行期只做查表式快速路径。
- 防重放：默认 4096 大小的滑动窗口足以对抗常见的乱序重传，可通过 `advanced.replay_window` 调整或关闭。
- 与传输解耦：AEAD 是消息级的，所以同一个加密配置可以同时在 SHM、Zenoh、MQTT 之间互通。

## 配图

![AEAD 信封流程](./images/security-encryption-flow.png)

图示展示了 AAD 信封头（版本/模式/序列号）、Nonce、密文与 GCM tag 在线缆上的字节布局，以及两端各自的派生
路径（原始 key 走 SHA-256，passphrase 走 PBKDF2）。

## 参考

- `../security_custom/` — 自定义算法回调
- `../security_rsa/` — RSA-OAEP 混合握手
- `vlink/include/vlink/extension/security.h` — `Security` 与 `Security::Config` 头文件
- `doc/09-security.md` — 加密章节完整说明
