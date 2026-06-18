# security_custom — 自定义加密回调旁路内置 AEAD

当默认的 AES-128-GCM 不能满足合规要求（国密 SM4-GCM、HSM 后端、企业自研密码栈），需要让 vlink 把
「加密」与「解密」当作两个用户函数调用。本示例展示三种典型用法：自由函数指针、Lambda 捕获、以及在
`Publisher` / `Subscriber` 之外的独立 `Security` 实例。

## 背景与适用场景

`Security::Config::encrypt_callback` 与 `decrypt_callback` 一旦同时设置，内置 AEAD 路径会被完全旁路。
vlink 不再触碰密文格式，只负责把 `Bytes in` 喂给回调并把 `Bytes out` 当作线缆负载发出去。这给了集成方
完全的控制权：可以选择只做完整性签名、可以包入自研协议头、也可以串联多种算法。

适用场景：对接 HSM/SE 硬件、国密合规、与既有密码协议互通。不适用场景：单纯出于「我想用 ChaCha20-Poly1305」
的需求——把它放进 `advanced` 字段并不会让协议改变；这是回调 API 的旁路目的，不是用来调整 cipher suite 的旋钮。

注意：示例里的 XOR 与 ROT-N 仅用于演示。任何生产部署都必须替换为正经 AEAD（AES-GCM、ChaCha20-Poly1305、
SM4-GCM、或 HSM-backed wrap）。否则线上的「加密」实际上只是混淆。

## 核心 API

| API | 签名 | 说明 |
|-----|------|------|
| `Security::Callback` | `Function<bool(const Bytes& in, Bytes& out)>` | 加密/解密回调类型别名 |
| `Security::Config::encrypt_callback` | `Callback` | 加密回调；与 `decrypt_callback` 必须成对设置 |
| `Security::Config::decrypt_callback` | `Callback` | 解密回调 |
| `vlink::Security` | `Security(const Config&)` / `Security(Config&&)` | 独立加密实例，可不依附于 pub/sub |
| `Security::encrypt` | `bool encrypt(const Bytes& in, Bytes& out)` | 直接对字节加密 |
| `Security::decrypt` | `bool decrypt(const Bytes& in, Bytes& out)` | 直接对字节解密 |
| `Security::is_configured` | `bool is_configured() const noexcept` | 是否有可用的密码槽 |
| `Security::can_encrypt` | `bool can_encrypt() const noexcept` | 是否具备加密能力 |
| `Security::can_decrypt` | `bool can_decrypt() const noexcept` | 是否具备解密能力 |
| `vlink::Bytes::from_string` | `static Bytes from_string(std::string_view)` | 字符串字面量转字节 |
| `vlink::Bytes::create` | `static Bytes create(size_t)` | 分配可写字节缓冲 |

## 代码导读

### 1. 自由函数 XOR 回调

示例先定义一个无状态的字节流变换函数（演示用 XOR），同时作为加密与解密回调（XOR 自反）：

```cpp
static constexpr uint8_t kXorKey[] = {0xDE, 0xAD, 0xBE, 0xEF, 0xCA, 0xFE, 0xBA, 0xBE};

bool xor_transform(const vlink::Bytes& in, vlink::Bytes& out) {
  if (in.empty()) {
    return false;
  }

  out = vlink::Bytes::create(in.size());
  for (size_t i = 0; i < in.size(); ++i) {
    out[i] = in.data()[i] ^ kXorKey[i % sizeof(kXorKey)];
  }

  return true;
}
```

回调签名固定：`bool(const Bytes& in, Bytes& out)`，必须始终把结果写入 `out` 并返回 `true` 表示成功，
否则上层会丢弃这条消息。

### 2. Lambda 捕获状态的 ROT-N

更接近生产场景：把密钥/密码学上下文作为捕获状态，让加解密成为两个不同的可调用对象：

```cpp
static constexpr uint8_t kRotation = 13;

auto rot_encrypt = [](const vlink::Bytes& in, vlink::Bytes& out) -> bool {
  out = vlink::Bytes::create(in.size());
  for (size_t i = 0; i < in.size(); ++i) {
    out[i] = in.data()[i] + kRotation;
  }
  return true;
};

auto rot_decrypt = [](const vlink::Bytes& in, vlink::Bytes& out) -> bool {
  out = vlink::Bytes::create(in.size());
  for (size_t i = 0; i < in.size(); ++i) {
    out[i] = in.data()[i] - kRotation;
  }
  return true;
};
```

注意 ROT-N 不再是自反的，所以加解密必须分开实现。在真实产品里这通常是一个调用了 OpenSSL `EVP_EncryptUpdate`
或 HSM SDK 的 thin wrapper。

### 3. 独立 Security 实例

不通过 pub/sub，直接对一段字节做加密往返：

```cpp
vlink::Security::Config sec_cfg;
sec_cfg.encrypt_callback = xor_transform;
sec_cfg.decrypt_callback = xor_transform;
vlink::Security security(sec_cfg);

vlink::Bytes plaintext = vlink::Bytes::from_string("Hello, Security!");
vlink::Bytes ciphertext;
const bool enc_ok = security.encrypt(plaintext, ciphertext);

vlink::Bytes recovered;
const bool dec_ok = security.decrypt(ciphertext, recovered);
```

这条路径对工具链很有用：例如在落盘前对 bag 做静态加密、在 RPC 网关做边界转译、或者在测试代码里做轮转
验证。

### 4. 回调签名提醒

最后一段日志只是再次强调签名要求与配置一次性约束：`encrypt_callback` 与 `decrypt_callback` 都必须设置才能
旁路内置 AES；`Security::Config` 是构造期快照，运行期不能改。

## 运行

```bash
./build/output/bin/example_security_custom
```

预期输出（节选）：

```
[1] Custom XOR Cipher via Security::Config callbacks
[XOR] Received: Hello with XOR cipher!
[XOR] Received: Custom encryption bypasses AES
XOR cipher: received 2 messages
[2] Lambda-based Custom Cipher (ROT-N)
[ROT-N] Received: Lambda ROT-13 encrypted
Lambda cipher: received 1 messages
[3] Direct Security Class Usage
Encrypt success: 1 plaintext_size: 16 cipher_size: 16
Decrypt success: 1 recovered: Hello, Security!
Roundtrip verification: PASS
Security custom cipher example complete.
```

无外部依赖，直接运行即可；与 `security_basic` 不同的是即便未编译 OpenSSL 也能跑（回调路径不依赖 AEAD 实现）。

## 常见陷阱

1. 只设置 `encrypt_callback` 不设置 `decrypt_callback`（或反之）：内置 AEAD 不会被旁路，但回调那一侧会缺失，
   实际行为难以预期。`is_configured()` / `can_encrypt()` / `can_decrypt()` 用于在构造后立刻断言。
2. 回调里 `out = in` 而不 `create`：会形成共享视图，多线程下可能竞争；务必 `Bytes::create(size)` 拷贝出独立缓冲。
3. 把回调函数对象捕获的状态写成非线程安全：vlink 内部可能在多个线程并发调用回调；如需可变状态请加锁。
4. 空输入：示例约定空输入返回 `false`，与内置 AEAD 行为一致；自定义实现需要明确这一边界。
5. 误以为 XOR/ROT-N 可以上生产：示例代码注释里反复强调演示性质，请不要照搬。

## 设计要点

- 回调比 cipher suite 旋钮更灵活：用户控制密文边界，可以引入 nonce/replay 头、ASN.1 信封等。
- `Function` 类型是可拷贝的：`Config` 因此可以按 const 引用传递，不会丢失目标。
- `Security` 与 `Publisher` 解耦：同一个 Config 既可装入 pub/sub，也可单独用于工具链；这是把
  「加密」当作纯字节函数的设计回报。
- 没有运行期 setter：所有字段在构造期解析，避免运行期重新派生密钥造成的瞬时不一致。

## 配图

![自定义回调路径](./images/security-custom-callback.png)

图中展示了当 `encrypt_callback` / `decrypt_callback` 同时存在时，vlink 内部如何跳过 AES-128-GCM 路径，
直接把字节交给用户函数。

## 参考

- `../security_basic/` — 内置 AES-128-GCM 路径
- `../security_rsa/` — RSA-OAEP 混合握手
- `vlink/include/vlink/extension/security.h` — 头文件中 `Callback` 与 `Config` 的精确定义
- `doc/09-security.md` — 加密章节完整说明
