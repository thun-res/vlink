# security/ — 端到端消息加密

本目录展示 vlink 的应用层加密扩展 `vlink/extension/security.h`。三个示例从对称密钥到非对称握手循序渐进，覆盖
生产环境中最常见的三种密钥模型：预共享对称密钥、自定义算法回调、RSA 混合加密。

加密层独立于传输层。无论使用 DDS 自带的 DDS-Security、SHM、Zenoh、MQTT 还是 FDBus，加密都是消息粒度的：每条消息
都会被独立封装为带 AAD 的 AEAD 信封。因此跨传输协议、跨语言的对端只要持有相同的密钥配置就能互通。

## 子示例索引

| 示例 | 主题 | 关键类 |
|------|------|--------|
| `security_basic/` | 内置 AES-128-GCM，原始密钥与 PBKDF2 派生 | `SecurityPublisher` / `SecuritySubscriber` / `Security::Config` |
| `security_custom/` | 自定义 encrypt / decrypt 回调，绕过内置 AEAD | `Security::Config::encrypt_callback` / `decrypt_callback` |
| `security_rsa/` | RSA-OAEP 混合 + 可选 RSA-PSS 发送方签名 | `Security::Config::public_key_pem` / `private_key_pem` / `advanced.signing_key_pem` |

## 推荐阅读顺序

第一遍先读 `security_basic/`，理解 `Security::Config` 的字段语义、AEAD 信封格式（版本/模式/序列号/Nonce/Tag）以及
`wait_for_subscribers()` 在握手前的角色。这是后续两个示例的公共基础。

随后阅读 `security_custom/`。当公司有自研密码库或需要对接 HSM、国密 SM4-GCM 等算法时，必须用回调形式
接入。理解了回调签名与字节边界，就能在不破坏 vlink 现有传输栈的前提下插入任意密码栈。

最后读 `security_rsa/`，掌握非对称握手与端到端身份认证。生产部署里 RSA 通常用于一次密钥交换 + 长会话 AES，
本示例在每条消息上都做一次 OAEP 封包，开销可观，但能直接展示 API 的对称性。

## 共同前置知识

- vlink 的 `Publisher<T>` / `Subscriber<T>` 基本用法（参见仓库根 `README.zh-cn.md`）。
- `vlink::Bytes` 的 `from_string` / `create` / `data` / `size`。
- 加密层要求跨进程的传输：`shm://`、`shm2://`、`zenoh://`、`mqtt://`、`fdbus://` 等。
  `intra://` 与使用 CDR 序列化的 `dds://` 不支持加密层封装。
- 启用密码学路径需要 `VLINK_ENABLE_SECURITY` 编译宏（依赖 OpenSSL）。未启用时仅自定义回调路径有效。

## 配图

各示例目录下 `images/` 都包含一张流程图：

- `security_basic/images/security-encryption-flow.png` — AEAD 信封字段与发送/接收时序
- `security_custom/images/security-custom-callback.png` — 回调路径如何旁路内置 AES
- `security_rsa/images/security-rsa-hybrid.png` — RSA-OAEP 包裹 AES 会话密钥的混合握手

## 参考

- `doc/09-security.md` — 加密章节完整设计
- `include/vlink/extension/security.h` — API 头文件
