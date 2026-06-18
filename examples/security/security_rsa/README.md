# security_rsa — RSA-OAEP 混合加密 + 可选 RSA-PSS 签名

本示例展示 vlink 加密扩展的非对称路径：发送方用接收方公钥 RSA-OAEP-SHA256 包裹一次性 AES-128 会话密钥，
内层用 AES-128-GCM 加密载荷，可选叠加 RSA-PSS-SHA256 签名实现发送方身份认证。涵盖四个场景：纯加密、加密+签名、
错误签名密钥的拒绝演示、独立 `Security` 实例的字节往返。

## 背景与适用场景

预共享对称密钥（参见 `security_basic`）在大规模部署里很难维护：密钥轮换需要协同所有端、泄露需要一刀切。
非对称握手把发送方与接收方的「持有什么」彻底分开：接收方只要保护好自己的私钥，发送方只需获取接收方公钥。
代价是 RSA-OAEP 比一次 AES 加密慢 100 倍以上，所以 vlink 采用混合模式：每条消息生成新 AES 会话密钥，公钥仅
用于一次性密钥封装。

`advanced.signing_key_pem` 进一步引入发送方身份：用发送方私钥对 AAD 信封与密文一起做 RSA-PSS 签名，接收方
用 `advanced.verify_key_pem` 验签。这把「保密」与「认证」分成两条独立的密钥链，方便审计与权限分级。

适用：跨组织/跨车队消息、控制指令、固件指令。不适用：高频遥测（每条消息一次 RSA 开销过大，请改成定期协商
对称密钥的会话层）。

## 核心 API

| API | 签名 | 说明 |
|-----|------|------|
| `Security::Config::public_key_pem` | `std::string` | 发送方持有的对端 RSA 公钥（PEM） |
| `Security::Config::private_key_pem` | `std::string` | 接收方持有的本端 RSA 私钥（PEM） |
| `Security::Config::Advanced::signing_key_pem` | `std::string` | 发送方私钥，用于 RSA-PSS 签名 |
| `Security::Config::Advanced::verify_key_pem` | `std::string` | 接收方持有的发送方公钥，用于验签 |
| `Security::Config::Advanced::aad_context` | `std::string` | 应用层 AAD 上下文，纳入认证范围 |
| `Security::Config::Advanced::replay_window` | `uint32_t` | 防重放滑动窗口大小 |
| `vlink::SecurityPublisher<T>` | `SecurityPublisher(url, Security::Config)` | 走非对称加密的发布器 |
| `vlink::SecuritySubscriber<T>` | `SecuritySubscriber(url, Security::Config)` | 走非对称解密的订阅器 |
| `vlink::Security::encrypt` | `bool encrypt(const Bytes&, Bytes&)` | 独立实例下的非对称加密 |
| `vlink::Security::decrypt` | `bool decrypt(const Bytes&, Bytes&)` | 独立实例下的非对称解密 |

约束：所有 PEM 字段都要求 RSA 密钥 ≥ 2048 位，EC 密钥会被构造期校验拒绝。

## 代码导读

### 0. 临时密钥生成

示例用 OpenSSL EVP API 在启动时生成两组 RSA-2048 密钥对（接收方密钥 + 签名密钥）。生产环境应改为离线/受
保护方式提供 PEM：

```cpp
EVP_PKEY_CTX* gctx = EVP_PKEY_CTX_new_id(EVP_PKEY_RSA, nullptr);
EVP_PKEY_keygen_init(gctx);
EVP_PKEY_CTX_set_rsa_keygen_bits(gctx, bits);
EVP_PKEY_keygen(gctx, &pkey);
PEM_write_bio_PUBKEY(bio, pkey);
PEM_write_bio_PrivateKey(bio, pkey, nullptr, nullptr, 0, nullptr, nullptr);
```

不熟悉 OpenSSL 资源管理的读者请注意：示例用 `std::unique_ptr<..., decltype(&Free)>` 对 `EVP_PKEY_CTX`、
`EVP_PKEY`、`BIO` 各自配 deleter，避免任何分支泄露内存。

### 1. 纯混合加密

最简单的 RSA-OAEP 配置：发送侧只持有对端公钥，接收侧只持有自己的私钥。

```cpp
vlink::Security::Config sub_cfg;
sub_cfg.private_key_pem = recv_kp.private_pem;
vlink::SecuritySubscriber<std::string> sub("dds://security_rsa/hybrid", sub_cfg);

vlink::Security::Config pub_cfg;
pub_cfg.public_key_pem = recv_kp.public_pem;
vlink::SecurityPublisher<std::string> pub("dds://security_rsa/hybrid", pub_cfg);
pub.wait_for_subscribers();
pub.publish("RSA-OAEP wraps an AES-128 session key");
```

每条消息独立生成 16 字节 AES 会话密钥，由 RSA-OAEP-SHA256 封装；载荷由 AES-128-GCM 处理。

### 2. 叠加 RSA-PSS 签名

把发送方的「身份」也固化到消息里：

```cpp
sub_cfg.advanced.verify_key_pem = sign_kp.public_pem;  // 接收方校验
pub_cfg.advanced.signing_key_pem = sign_kp.private_pem;  // 发送方签名
```

签名作用于 AAD 信封 + 密文 + GCM tag 的拼接，覆盖整个认证范围。接收方先验签，验签失败立即丢弃，不进入解密。

### 3. 错误签名密钥的拒绝

为了演示验签的实际效果，示例临时生成一个「冒充者」密钥对，让发送侧用 impostor 私钥签名，但接收侧仍按
原来配置 `verify_key_pem`：

```cpp
const RsaKeyPair impostor_kp = generate_rsa_keypair(2048);
pub_cfg.advanced.signing_key_pem = impostor_kp.private_pem;
```

回调不会被调用，日志 `received 0 messages` 即为预期。这正是 RSA-PSS 提供的发送方认证能力。

### 4. 独立 Security 实例

不通过 pub/sub，直接构造对称的发送方 / 接收方实例并做一次字节往返：

```cpp
vlink::Security::Config sender_cfg;
sender_cfg.public_key_pem = recv_kp.public_pem;
sender_cfg.advanced.signing_key_pem = sign_kp.private_pem;
vlink::Security sender(sender_cfg);

vlink::Security::Config receiver_cfg;
receiver_cfg.private_key_pem = recv_kp.private_pem;
receiver_cfg.advanced.verify_key_pem = sign_kp.public_pem;
vlink::Security receiver(receiver_cfg);

sender.encrypt(plaintext, ciphertext);
receiver.decrypt(ciphertext, recovered);
```

这种用法适合工具链：例如在 bag 落盘前用非对称密钥包封数据，或在网关边界做格式转换。

### 5. 配置速查

最后一段日志列出四个 PEM 字段的角色与算法：发送方需 `public_key_pem`，可选 `signing_key_pem`；接收方需
`private_key_pem`，可选 `verify_key_pem`。

## 运行

```bash
./build/output/bin/example_security_rsa
```

预期输出（节选）：

```
Generating ephemeral RSA-2048 key pairs (receiver + signing)
[1] RSA-OAEP Hybrid (AES-128-GCM payload)
[RSA-Hybrid] Received: RSA-OAEP wraps an AES-128 session key
RSA hybrid: received 3 messages
[2] RSA Hybrid + RSA-PSS Signed (sender authenticated)
[RSA-Signed] Received: Signed with RSA-PSS-SHA256
RSA signed: received 2 messages
[3] Wrong Signing Key -- Verification Failure
Impostor: received 0 messages (expected 0 due to RSA-PSS failure)
[4] Standalone vlink::Security with RSA Hybrid
Roundtrip verification: PASS
Security RSA hybrid example complete.
```

依赖：编译时需启用 `VLINK_ENABLE_SECURITY`（链接 OpenSSL）。否则 PEM 字段验证会失败、`is_configured()` 返回
`false`，所有 `publish/listen` 都会被静默丢弃。

## 常见陷阱

1. EC 或 ≤2048 位 RSA 密钥会被拒：`Security` 构造期校验失败后该 slot 被留空，但实例本身不会抛异常。务必
   构造后 `can_encrypt()` / `can_decrypt()` 自检。
2. PEM 字符串末尾换行：`-----END ...-----` 后必须有一个 `\n`，否则 OpenSSL 解析失败。`PEM_write_bio_*` 自动满足，
   外部读取时需要注意。
3. 公钥 / 私钥角色搞反：把对端公钥放到 `private_key_pem` 不会立刻报错，但会在第一次 `decrypt()` 时静默失败。
4. 签名密钥与解密私钥不应同一对：示例里用两套独立密钥对（`recv_kp` 与 `sign_kp`），是为了说明保密与身份是两件事。
5. 每条消息都做 RSA-OAEP 与 RSA-PSS 的开销可观（CPU 单核约几千 op/s）。高频通道请改用会话层 + 对称密钥。

## 设计要点

- 混合加密：RSA-OAEP 仅用于每条消息的 AES 会话密钥包装，载荷始终用 AES-128-GCM；带宽与 CPU 开销可控。
- 身份分层：保密由 `public/private_key_pem` 提供，身份认证由 `advanced.signing_key_pem` / `verify_key_pem` 提供；
  两条密钥链可独立运维。
- 防重放与 AAD：与 `security_basic` 一致，AAD 信封 + 序列号 + 滑动窗口都生效；非对称握手并不放弃这些机制。
- 一次性 Config：所有密钥都在构造期解析，运行期热替换需要构造新实例并由上层迁移连接。

## 配图

![RSA 混合加密时序](./images/security-rsa-hybrid.png)

图中标出 AES 会话密钥的一次性生成、RSA-OAEP 包装、AES-128-GCM 加密载荷、以及可选的 RSA-PSS 签名相对于
AAD/密文/Tag 的覆盖范围。

## 参考

- `../security_basic/` — 预共享对称密钥
- `../security_custom/` — 自定义回调旁路
- `vlink/include/vlink/extension/security.h` — `Security::Config` 与 `Advanced` 的精确定义
- `doc/09-security.md` — 加密章节完整设计，含 §9.6 非对称握手
