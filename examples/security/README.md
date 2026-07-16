# 🔒 security — 端到端消息加密

本目录演示 vlink 的应用层加密扩展 `vlink/extension/security.h`，聚焦最常用的对称密钥模型：原始密钥与 PBKDF2 口令派生。自定义算法回调与 RSA 混合握手等进阶密钥模型不单列示例，统一收录于 `doc/07-security.md` 专文。

加密层位于传输层之上：在受支持的后端中，每条消息都独立封装为带 AAD 的 AEAD 信封。通信双方仍须使用相同后端、相同地址语义和兼容的序列化格式；相同密钥配置只保证双方能够加解密，并不会让不同传输协议直接互通。

## 📑 示例索引

| 示例 | 主题 | 关键类 |
|------|------|--------|
| [`security_basic/`](security_basic/) | 内置 AES-128-GCM；原始密钥与 PBKDF2 派生 | `SecurityPublisher` / `SecuritySubscriber` / `Security::Config` |

## 🧭 阅读顺序

先读 [`security_basic/`](security_basic/)，掌握 `Security::Config` 的字段语义、AEAD 信封格式（版本 / 模式 / 发送方 ID / 序列号 / Nonce / Tag）以及 `wait_for_subscribers()` 的传输端点匹配语义。内置对称加密不增加独立的安全握手。

进阶密钥模型见 `doc/07-security.md`：

| 模型 | 配置入口 | 适用场景 |
|------|----------|----------|
| 自定义 encrypt / decrypt 回调 | `Config::encrypt_callback` / `decrypt_callback` | 对接自研密码库、HSM、国密 SM4-GCM；在不改动传输栈的前提下插入任意密码栈 |
| RSA-OAEP 混合加密 + 可选 RSA-PSS 签名 | `Config::public_key_pem` / `private_key_pem` / `advanced.signing_key_pem` | 每条消息生成 AES 密钥并以 RSA-OAEP 包装；可选签名用于消息来源认证 |

## 📋 前置知识

- `Publisher<T>` / `Subscriber<T>` 的基本用法（参见仓库根 [`README.md`](../../README.md)）。
- `vlink::Bytes` 的 `from_string` / `create` / `data` / `size`。
- 加密层要求走受支持的序列化传输路径；`shm://`、`shm2://`、`zenoh://`、`mqtt://`、`fdbus://` 等可用，而 `intra://` 与使用 CDR 序列化的 `dds://` 不支持消息级安全封装。是否部署为单进程不改变后端能力。
- 启用密码学路径需要 `VLINK_ENABLE_SECURITY` 编译宏（依赖 OpenSSL）；未启用时仅自定义回调路径有效。

## 🖼️ 配图

`security_basic/images/security-encryption-flow.png` — 对称模式的 AEAD 信封字段与发送 / 接收时序。回调旁路与 RSA 混合加密说明收录于 `doc/07-security.md`。

## 📚 参考

- `doc/07-security.md` — 加密章节完整设计
- `include/vlink/extension/security.h` — API 头文件
