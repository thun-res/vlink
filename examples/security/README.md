# 🔒 security — 端到端消息加密

本目录演示 vlink 的应用层加密扩展 `vlink/extension/security.h`，聚焦最常用的对称密钥模型：原始密钥与 PBKDF2 口令派生。自定义算法回调与 RSA 混合握手等进阶密钥模型不单列示例，统一收录于 `doc/07-security.md` 专文。

加密层独立于传输层。无论底层为 DDS-Security、SHM、Zenoh、MQTT 还是 FDBus，加密均以消息为粒度：每条消息被独立封装为带 AAD 的 AEAD 信封。因此跨传输协议、跨语言的对端只要持有相同密钥配置即可互通。

## 📑 示例索引

| 示例 | 主题 | 关键类 |
|------|------|--------|
| [`security_basic/`](security_basic/) | 内置 AES-128-GCM；原始密钥与 PBKDF2 派生 | `SecurityPublisher` / `SecuritySubscriber` / `Security::Config` |

## 🧭 阅读顺序

先读 [`security_basic/`](security_basic/)，掌握 `Security::Config` 的字段语义、AEAD 信封格式（版本 / 模式 / 序列号 / Nonce / Tag）以及 `wait_for_subscribers()` 在握手前的角色。这是理解加密层的公共基础。

进阶密钥模型见 `doc/07-security.md`：

| 模型 | 配置入口 | 适用场景 |
|------|----------|----------|
| 自定义 encrypt / decrypt 回调 | `Config::encrypt_callback` / `decrypt_callback` | 对接自研密码库、HSM、国密 SM4-GCM；在不改动传输栈的前提下插入任意密码栈 |
| RSA-OAEP 混合握手 + 可选 RSA-PSS 签名 | `Config::public_key_pem` / `private_key_pem` / `advanced.signing_key_pem` | 非对称密钥分发与端到端身份认证；生产部署常用一次密钥交换 + 长会话 AES |

## 📋 前置知识

- `Publisher<T>` / `Subscriber<T>` 的基本用法（参见仓库根 [`README.md`](../../README.md)）。
- `vlink::Bytes` 的 `from_string` / `create` / `data` / `size`。
- 加密层要求跨进程传输：`shm://`、`shm2://`、`zenoh://`、`mqtt://`、`fdbus://` 等；`intra://` 与使用 CDR 序列化的 `dds://` 不支持加密层封装。
- 启用密码学路径需要 `VLINK_ENABLE_SECURITY` 编译宏（依赖 OpenSSL）；未启用时仅自定义回调路径有效。

## 🖼️ 配图

`security_basic/images/security-encryption-flow.png` — AEAD 信封字段与发送 / 接收时序。回调旁路与 RSA 混合握手的流程图收录于 `doc/07-security.md`。

## 📚 参考

- `doc/07-security.md` — 加密章节完整设计
- `include/vlink/extension/security.h` — API 头文件
