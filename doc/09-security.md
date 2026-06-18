# 9. 安全加密

## 目录

- [9.1 概述](#91-概述)
- [9.2 编译要求](#92-编译要求)
- [9.3 SecurityType 与 Security 类别名](#93-securitytype-与-security-类别名)
- [9.4 Security 类与 Config 配置](#94-security-类与-config-配置)
- [9.5 对称模式（key / passphrase）](#95-对称模式key--passphrase)
- [9.6 非对称模式（RSA 混合握手）](#96-非对称模式rsa-混合握手)
- [9.7 自定义加密回调](#97-自定义加密回调)
- [9.8 算法与 Wire Format](#98-算法与-wire-format)
- [9.9 不支持安全加密的组合](#99-不支持安全加密的组合)
- [9.10 VLINK_SSL_* 环境变量（传输层 TLS）](#910-vlink_ssl_-环境变量传输层-tls)
- [9.11 完整使用示例](#911-完整使用示例)
- [9.12 性能与最佳实践](#912-性能与最佳实践)

---

## 9.1 概述

VLink 支持在**消息层面**对传输内容加密：加密发生在序列化之后、传输之前，解密发生在接收之后、反序列化之前，因此大多数传输后端对上层透明。

核心设计：

- **透明接入**：普通节点类型换成 `Security*` 别名，或把模板参数 `SecT` 设为 `SecurityType::kWithSecurity`，业务代码不变。
- **一次配置**：所有参数通过 `Security::Config` aggregate 一次性传入 `SecurityXxx` 的构造函数；空配置使用内置默认安全槽位；没有单独的 setter，要更换配置请销毁并重新构造节点。
- **线程安全**：`Security::encrypt()` / `decrypt()` 内部互斥保护；同一实例上的调用会串行执行。
- **三种模式自动切换**：对称 AEAD / RSA 非对称混合握手 / 自定义回调。
- **按需开销**：`SecT == kWithoutSecurity`（默认）时没有任何加密代码路径。

支持的通信模型：

| 通信模型   | 发送端（加密）             | 接收端（解密）               |
| ---------- | -------------------------- | ---------------------------- |
| Event      | `SecurityPublisher<T>`     | `SecuritySubscriber<T>`      |
| Method     | `SecurityClient<Req,Resp>` | `SecurityServer<Req,Resp>`   |
| Field      | `SecuritySetter<T>`        | `SecurityGetter<T>`          |

![安全加密管道](images/security-pipeline.png)

---

## 9.2 编译要求

### 9.2.1 内置 AEAD / RSA 模式

顶层 CMake 选项 `ENABLE_SECURITY` 默认 `ON`，开启后会链接 OpenSSL 并在编译单元里定义宏 `VLINK_ENABLE_SECURITY`。

```cmake
cmake -DENABLE_SECURITY=ON ...
target_link_libraries(my_app PRIVATE vlink::vlink)
```

未定义 `VLINK_ENABLE_SECURITY` 时，对称与非对称路径不可用；`Security::encrypt()` / `decrypt()` 会打印 warning 并返回 `false`。

### 9.2.2 自定义回调模式

通过 `Config::encrypt_callback` / `decrypt_callback` 注入算法时不依赖 OpenSSL，`ENABLE_SECURITY` 可关；此时仍可以使用 `SecurityPublisher` 等类型，只要回调自身是自洽的。

---

## 9.3 SecurityType 与 Security 类别名

所有通信节点的模板签名都携带 `SecT` 参数：

```cpp
template <typename MsgT, SecurityType SecT = SecurityType::kWithoutSecurity>
class Publisher;

template <typename ReqT, typename RespT = Traits::EmptyType,
          SecurityType SecT = SecurityType::kWithoutSecurity>
class Client;

```

`Subscriber` / `Server` / `Setter` / `Getter` 的模板签名同理。

`SecurityType` 枚举（`include/vlink/impl/types.h`）：

| 值  | 枚举                           | 含义                     |
| --- | ------------------------------ | ------------------------ |
| 0   | `SecurityType::kWithoutSecurity` | 不启用加密（默认）       |
| 1   | `SecurityType::kWithSecurity`    | 启用消息级加密/解密      |

两种写法等价：

直接指定模板参数：

```cpp
vlink::Publisher<MyMsg, vlink::SecurityType::kWithSecurity> pub("shm://topic");
```

使用 `Security*` 别名（推荐，更简洁）：

```cpp
vlink::SecurityPublisher<MyMsg> pub("shm://topic");
```

别名定义于各自头文件：`publisher.h` / `subscriber.h` / `client.h` / `server.h` / `setter.h` / `getter.h`，对应 `SecurityPublisher` / `SecuritySubscriber` / `SecurityClient` / `SecurityServer` / `SecuritySetter` / `SecurityGetter`。

---

## 9.4 Security 类与 Config 配置

头文件：`include/vlink/extension/security.h`。

```cpp
class Security final {
 public:
  using Callback = Function<bool(const Bytes& in, Bytes& out)>;

  struct Config final {
    struct Advanced final {
      std::string aad_context;
      uint32_t replay_window{4096U};
      std::string signing_key_pem;
      std::string verify_key_pem;
    };

    std::string key;
    std::string passphrase;
    Bytes pbkdf2_salt;
    uint32_t pbkdf2_iterations{200000U};
    std::string public_key_pem;
    std::string private_key_pem;
    Callback encrypt_callback;
    Callback decrypt_callback;
    Advanced advanced;
  };

  Security();
  static Config from_private_key_path(const std::string& private_key_path);
  static Config from_public_key_path(const std::string& public_key_path);
  static Config from_key_paths(const std::string& public_key_path, const std::string& private_key_path);
  explicit Security(const Config& cfg);
  explicit Security(Config&& cfg);
  ~Security();
  Security(Security&&) noexcept;
  Security& operator=(Security&&) noexcept;

  bool encrypt(const Bytes& in, Bytes& out);
  bool decrypt(const Bytes& in, Bytes& out);
  bool is_configured() const noexcept;
  bool can_encrypt() const noexcept;
  bool can_decrypt() const noexcept;
};
```

`Security::Config` 通过 `SecurityXxx` 节点的**构造函数**一次性传入（不再有运行时 setter）：

```cpp
template <typename SecurityConfigT = Security::Config>
explicit SecurityPublisher(const std::string& url_str, SecurityConfigT&& sec_cfg = {},
                           InitType type = InitType::kWithInit);

template <typename ConfT, typename SecurityConfigT = Security::Config,
          typename = std::enable_if_t<std::is_base_of_v<Conf, ConfT>>>
explicit SecurityPublisher(const ConfT& conf, SecurityConfigT&& sec_cfg = {},
                           InitType type = InitType::kWithInit);
```

`SecuritySubscriber` / `SecurityServer` / `SecurityClient` / `SecuritySetter` / `SecurityGetter` 的签名与之对应（method/field 类增加 `RespT` / `ValueT` 模板形参，含义不变）。

要点：

- 配置**一次性**：`Security::Config` 只在构造时传入。内部在 `init()` 之前用候选 `Security` 验证 `is_configured()`，并按当前节点角色追加 `can_encrypt()` / `can_decrypt()` 校验，通过才安装到 `NodeImpl::security`；空 `Security::Config{}` 会在 `Security` 构造路径里使用内置默认安全槽位。
- 模式选择**自动**：自定义回调 > RSA 非对称（存在 `public_key_pem` 或 `private_key_pem`）> 对称（`key` / `passphrase` / 内置默认安全槽位）。
- 不再暴露 `enable_security()` / `security()` 给用户代码（前者已移到 `Node` 的 protected 入口，仅供 `SecurityXxx` 子类内部调用）。需要再次更换配置的场景，请销毁并重新构造节点。
- 验证失败（PEM 损坏、RSA 长度 < 2048、缺失 salt 等）会打印 warning 并把对应槽位置空；只要还有其他合法槽位即视为成功（`is_configured() == true`）。显式配置无效时不会回退到内置默认安全槽位；仅配置签名/验签 PEM 也不会自动启用默认对称通道，因为它们本身不具备加解密能力。
- **角色感知校验**：发送端节点（Publisher / Setter / Client、以及需要响应的 Server）会额外检查 `can_encrypt()`；接收端节点（Subscriber / Getter / Server、以及需要回包的 Client）会额外检查 `can_decrypt()`。仅安装 `public_key_pem` 的 Subscriber、或仅安装 `private_key_pem` 的 Publisher 会被拒绝并打印 warning。
- 自定义回调必须**成对**安装（`encrypt_callback` 与 `decrypt_callback` 同时设置）；仅设其中之一会被忽略并打印 warning。
- 不支持的传输（`intra://` 或 `dds://` CDR）会打印 warning 并忽略 `Security::Config`；安全节点随后执行 `init()` 会因没有可用 `Security` fatal 并抛 `RuntimeError`，不会自动降级为明文。

---

## 9.5 对称模式（key / passphrase）

对称模式覆盖最常见的"双端预共享"场景，也提供空配置时的内置默认安全槽位。密钥来源：

| 来源 | 派生方式 | 适用场景 |
| ---- | -------- | -------- |
| `Config::key` | SHA-256 截断为 16 字节 AES 密钥 | 已有高熵密钥（KMS / HSM 派生） |
| `Config::passphrase` | PBKDF2-HMAC-SHA256（默认 200 000 轮）+ `pbkdf2_salt` | 低熵人类口令；**必须**配 `pbkdf2_salt` |
| 空 `Security::Config{}` | 内置默认安全槽位 | 兼容默认安全节点；生产环境建议显式配置 |

调用约定：

- 双端 `Config` 必须**一致**（同一 `key`，或同一 `passphrase + salt + iterations`，或双端都使用空配置默认安全槽位），否则解密失败、消息丢弃。
- `pbkdf2_salt` 长度建议 ≥ 16 字节，需通过安全渠道共享。
- 完全空的 `Security::Config{}` 会安装内置默认安全槽位；一旦提供显式安全字段（例如 key/passphrase/PEM/回调），则按显式配置验证，不会在验证失败时自动回退。

示例：

使用预共享 key：

```cpp
vlink::Security::Config cfg;
cfg.key = "my-secret";
vlink::SecurityPublisher<MyMsg> pub("shm://secure/topic", cfg);

vlink::SecuritySubscriber<MyMsg> sub("shm://secure/topic", cfg);
sub.listen([](const MyMsg& msg) { });
```

使用 passphrase + PBKDF2：

```cpp
vlink::Security::Config cfg;
cfg.passphrase = "correct horse battery staple";
cfg.pbkdf2_salt = shared_salt;
cfg.pbkdf2_iterations = 200000;

vlink::SecurityPublisher<MyMsg> pub("shm://secure/topic", cfg);
vlink::SecuritySubscriber<MyMsg> sub("shm://secure/topic", cfg);
```

---

## 9.6 非对称模式（RSA 混合握手）

非对称模式让发送方无需预共享对称密钥；每条消息独立生成 16 字节 AES 会话密钥，用对端 RSA 公钥 OAEP 包装后随密文一起发送。可选 RSA-PSS 签名提供发送方身份认证。

所有 RSA PEM **必须 ≥ 2048 位**。

| 字段 | 角色 | 说明 |
| ---- | ---- | ---- |
| `public_key_pem` | 发送端持有对端公钥 | RSA-OAEP-SHA256 包装会话密钥 |
| `private_key_pem` | 接收端持有自身私钥 | RSA-OAEP-SHA256 解开会话密钥 |
| `advanced.signing_key_pem` | 发送端持有自身私钥 | RSA-PSS-SHA256 对 AAD（domain / context / envelope / `wrap_len_le` / wrapped key）与 `ciphertext ‖ tag` 签名（可选） |
| `advanced.verify_key_pem` | 接收端持有对端公钥 | RSA-PSS-SHA256 验签；签名缺失或失败则拒绝消息 |

示例（带发送方认证）：

```cpp
vlink::Security::Config sender_cfg;
sender_cfg.public_key_pem = peer_pub_pem;
sender_cfg.advanced.signing_key_pem = own_priv_pem;

vlink::SecurityPublisher<MyMsg> pub("dds://secure/topic", sender_cfg);

vlink::Security::Config receiver_cfg;
receiver_cfg.private_key_pem = own_priv_pem;
receiver_cfg.advanced.verify_key_pem = peer_pub_pem;

vlink::SecuritySubscriber<MyMsg> sub("dds://secure/topic", receiver_cfg);
```

也可以直接从 PEM 文件路径创建 `Config`，减少业务代码里手写读文件逻辑：

```cpp
auto sender_cfg = vlink::Security::from_public_key_path("receiver_pub.pem");
auto receiver_cfg = vlink::Security::from_private_key_path("receiver_priv.pem");
auto full_cfg = vlink::Security::from_key_paths("peer_pub.pem", "own_priv.pem");
```

Python API 暴露同名静态方法，返回 `_vlink.SecurityConfig`：

```python
sender_cfg = _vlink.Security.from_public_key_path("receiver_pub.pem")
receiver_cfg = _vlink.Security.from_private_key_path("receiver_priv.pem")
full_cfg = _vlink.Security.from_key_paths("peer_pub.pem", "own_priv.pem")
```

省略 `advanced.signing_key_pem` / `advanced.verify_key_pem` 时仍可正常加解密，只是不再校验发送方身份。

---

## 9.7 自定义加密回调

业务可同时安装 `Config::encrypt_callback` 与 `Config::decrypt_callback`，**完全绕过**内置 AEAD 与 RSA 路径，用于接入 SM4、ChaCha20、HSM、白盒密码等。

回调签名：

```cpp
using Security::Callback = Function<bool(const Bytes& in, Bytes& out)>;
```

- 发送端：`in` 为明文，写入 `out` 作为密文。
- 接收端：`in` 为密文，写入 `out` 作为明文。
- 返回 `false` 时消息被丢弃。
- 安装后 AEAD/RSA 路径不再被走，**不依赖** `VLINK_ENABLE_SECURITY`。
- 两个回调必须**同时**安装；只设一个时会被忽略并打印 warning。
- 同一 `Security` 实例上的回调会串行执行；如果多个实例共享同一回调状态，仍需由业务自行同步。

示例：

```cpp
static constexpr uint8_t kXorKey = 0xAB;

auto xor_encrypt = [](const vlink::Bytes& in, vlink::Bytes& out) -> bool {
    out = vlink::Bytes::create(in.size());
    for (size_t i = 0; i < in.size(); ++i) {
        out[i] = in[i] ^ kXorKey;
    }
    return true;
};
auto xor_decrypt = xor_encrypt;

vlink::Security::Config cfg;
cfg.encrypt_callback = xor_encrypt;
cfg.decrypt_callback = xor_decrypt;

vlink::SecurityPublisher<vlink::Bytes> pub("dds://secure/data", cfg);
vlink::SecuritySubscriber<vlink::Bytes> sub("dds://secure/data", cfg);
sub.listen([](const vlink::Bytes& msg) { });
```

---

## 9.8 算法与 Wire Format

### 9.8.1 对称 AEAD

| 参数     | 值                                                   |
| -------- | ---------------------------------------------------- |
| 算法     | AES-128-GCM（OpenSSL EVP API）                       |
| 密钥     | SHA-256(`key`) 截断为 16 字节；或 PBKDF2(`passphrase`) |
| Nonce    | 12 字节，由随机 sender nonce base 与单调 sequence 派生 |
| Tag      | 16 字节认证标签                                       |
| 线程安全 | 是（同一 `Security` 实例内部互斥保护） |

Wire format：

```
[34 B envelope header] [N B ciphertext] [16 B GCM tag]
```

常规 `Config::key` 模式的额外开销为 50 字节。Envelope header 绑定 version / mode / flags / sender_id / sequence / nonce；header 与 `aad_context` 一起作为 GCM AAD 认证。

通过 `SecurityPublisher` / `SecuritySubscriber` 等节点构造时，如果 `advanced.aad_context` 为空，VLink 会自动绑定 `url|ser_type|<schema_type 的整数值>`（schema_type 序列化为 `uint32_t`）；直接使用独立 `Security` 实例时空字符串就是实际 AAD context。

### 9.8.2 非对称 RSA 混合

每条消息：

1. `RAND_bytes()` 生成 16 字节 AES-128-GCM 会话密钥；nonce 由 envelope sequence 派生。
2. 用 `public_key_pem` 做 RSA-OAEP-SHA256 包装会话密钥。
3. AES-128-GCM 加密 payload 得到 ciphertext + tag。
4. 若设了 `advanced.signing_key_pem`，对 AAD（domain / context / envelope / wrap_len / wrapped_key）与 `ciphertext || tag` 做 RSA-PSS-SHA256 签名。签名覆盖**包含** `wrap_len_le` 与 wrapped key，**不包含** `sig_len_le`，避免签名长度自指涉。

Wire format（所有长度字段均小端）：

```
[34 B envelope header] [2 B wrap_len_le] [2 B sig_len_le] [wrap_len B wrapped session key] [sig_len B RSA-PSS signature] [N B ciphertext] [16 B GCM tag]
```

`sig_len_le == 0` 表示未携带签名；接收端若设了 `advanced.verify_key_pem` 但消息缺少签名，会拒收。

### 9.8.3 行为细节

- **空输入**：`Bytes::empty()` 时 `encrypt()` / `decrypt()` 直接返回 `false` 并清空 `out`（合法内置密文至少包含 envelope、1 字节密文与 16 字节 tag）。
- **认证失败**：tag 校验失败、wrapped key 解开失败、RSA-PSS 验签失败、或 replay 检查失败均使 `decrypt()` 返回 `false`，回调不会被触发。
- **未启用**：未定义 `VLINK_ENABLE_SECURITY` 且未安装自定义回调时，`encrypt()` / `decrypt()` 打印 `VLOG_W` 并返回 `false`。

---

## 9.9 不支持安全加密的组合

`src/impl/node_impl.cc` 中 `NodeImpl::enable_security()` 对以下传输组合会**打印 `VLOG_W` 警告并忽略 `Security::Config`**（由 `SecurityXxx` 构造路径经 `Node::enable_security()` 转发调用）：

- `intra://`：进程内直接传对象，不进入序列化/加密管道。
- `dds://` 配合 CDR 类型（`is_cdr_type == true`）：CDR 直接交给 Fast-DDS 处理，不经过 VLink 的 Bytes 管道。

这些组合下 `NodeImpl::security` 保持 `nullptr`；安全节点初始化时会触发 fatal 并抛 `RuntimeError`，不会进入正常明文收发路径。其他传输后端（shm/shm2/ddsc/ddsr/ddst/zenoh/mqtt/fdbus/someip/qnx 以及 `dds://` 的非 CDR 类型）均支持消息级加密。

如需在 CDR 链路上保护消息，请使用 DDS Security 插件（FastDDS 官方方案）或传输层 TLS。

---

## 9.10 VLINK_SSL_* 环境变量（传输层 TLS）

以下环境变量由 `src/impl/ssl_options.cc` 读取，用于为 MQTT / 代理等传输层端点提供 TLS 选项。**它们与 9.4–9.9 节的消息级加密无关**。

| 变量                | 作用                                     |
| ------------------- | ---------------------------------------- |
| `VLINK_SSL_VERIFY`  | 是否验证对端证书                         |
| `VLINK_SSL_CA`      | CA 证书文件路径                          |
| `VLINK_SSL_CERT`    | 客户端证书文件路径                       |
| `VLINK_SSL_KEY`     | 客户端私钥文件路径                       |
| `VLINK_SSL_KEY_PASS`| 私钥解密密码                             |
| `VLINK_SSL_SNI`     | TLS SNI 主机名                           |
| `VLINK_SSL_CIPHERS` | 允许的 TLS 密码套件列表                  |

`SslOptions` 的优先级：显式 API 设置 > URL 查询参数 (`ssl.ca` 等) > 环境变量 (`VLINK_SSL_*`)。

---

## 9.11 完整使用示例

### 9.11.1 示例 1：三种模型的对称加密通信

来自 `examples/samples/shm_raw/shm_raw.cc` 的参考示例：

```cpp
#include <vlink/vlink.h>
#include <thread>

int main() {
    vlink::Security::Config cfg;
    cfg.key = "custom-key";

    vlink::SecurityServer<vlink::Bytes, vlink::Bytes> server("shm://example_raw/method", cfg);
    server.listen([](const vlink::Bytes& req, vlink::Bytes& resp) {
        if (req == vlink::Bytes{0x1, 0x2, 0x3}) {
            resp = vlink::Bytes::create(1024 * 1024);
            resp[0] = 0xA;
            resp[(1024 * 1024) - 1] = 0xB;
        }
    });

    vlink::SecurityClient<vlink::Bytes, vlink::Bytes> client("shm://example_raw/method", cfg);
    auto resp = client.invoke(vlink::Bytes{0x1, 0x2, 0x3});

    if (resp.has_value()) {
        VLOG_I("invoke size:", resp.value().size());
    }

    vlink::SecuritySubscriber<vlink::Bytes> sub("shm://example_raw/event", cfg);
    sub.listen([](const vlink::Bytes& msg) { VLOG_I("received:", msg.to_string()); });

    vlink::SecurityPublisher<vlink::Bytes> pub("shm://example_raw/event", cfg);
    pub.wait_for_subscribers();
    pub.publish(vlink::Bytes::from_string("hello1"));
    pub.publish(vlink::Bytes::from_string("hello2"));
    pub.publish(vlink::Bytes::from_string("hello3"));

    vlink::SecuritySetter<vlink::Bytes> setter("shm://example_raw/field", cfg);
    setter.set(vlink::Bytes{0xA, 0xB, 0xC});

    vlink::SecurityGetter<vlink::Bytes> getter("shm://example_raw/field", cfg);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    auto ret = getter.get();

    if (ret.has_value()) {
        VLOG_I("field value:", ret.value());
    }

    return 0;
}
```

### 9.11.2 示例 2：Protobuf 消息 + passphrase + PBKDF2

```cpp
#include <vlink/publisher.h>
#include <vlink/subscriber.h>
#include "my_message.pb.h"

int main() {
    vlink::Security::Config cfg;
    cfg.passphrase = "vehicle-status-secret";
    cfg.pbkdf2_salt = shared_salt;

    vlink::SecuritySubscriber<MyMessage> sub("dds://vehicle/status", cfg);
    sub.listen([](const MyMessage& msg) {
        std::cout << "speed: " << msg.speed() << std::endl;
    });

    vlink::SecurityPublisher<MyMessage> pub("dds://vehicle/status", cfg);
    pub.wait_for_subscribers();

    MyMessage msg;
    msg.set_speed(60.0f);
    msg.set_heading(180.0f);
    pub.publish(msg);

    return 0;
}
```

### 9.11.3 示例 3：自定义 SM4 加密（示意）

```cpp
#include <vlink/publisher.h>
#include <vlink/subscriber.h>
#include <vlink/base/bytes.h>

vlink::Security::Callback make_sm4_encrypt(const std::string& key) {
    return [key](const vlink::Bytes& in, vlink::Bytes& out) -> bool {
        out = in;
        return true;
    };
}

vlink::Security::Callback make_sm4_decrypt(const std::string& key) {
    return [key](const vlink::Bytes& in, vlink::Bytes& out) -> bool {
        out = in;
        return true;
    };
}

int main() {
    const std::string sm4_key = "sm4-16-byte-key!";

    vlink::Security::Config cfg;
    cfg.encrypt_callback = make_sm4_encrypt(sm4_key);
    cfg.decrypt_callback = make_sm4_decrypt(sm4_key);

    vlink::SecurityPublisher<vlink::Bytes> pub("dds://secure/channel", cfg);
    vlink::SecuritySubscriber<vlink::Bytes> sub("dds://secure/channel", cfg);
    sub.listen([](const vlink::Bytes& msg) { });

    pub.wait_for_subscribers();
    pub.publish(vlink::Bytes::from_string("encrypted payload"));

    return 0;
}
```

### 9.11.4 示例 4：延迟初始化配合 RSA 混合握手

```cpp
#include <vlink/publisher.h>

int main() {
    vlink::Security::Config cfg;
    cfg.public_key_pem = peer_pub_pem;
    cfg.advanced.signing_key_pem = own_priv_pem;

    vlink::SecurityPublisher<std::string> pub(
        "dds://secure/log",
        cfg,
        vlink::InitType::kWithoutInit);

    pub.init();
    pub.wait_for_subscribers();
    pub.publish(std::string("secure log message"));

    return 0;
}
```

---

## 9.12 性能与最佳实践

### 9.12.1 开销来源

1. **AEAD**：AES-128-GCM 在 AES-NI / ARMv8 Crypto 扩展下吞吐量通常可达 GB/s 级。
2. **RSA**：RSA-OAEP 包装 + 可选 RSA-PSS 签名每条消息一次；2048 位 RSA 单次操作约 0.1–1 ms 量级，是非对称模式的主要瓶颈。
3. **PBKDF2**：仅在 `SecurityXxx` 构造时计算一次，默认 200 000 轮约几十毫秒；不在热路径。
4. **内存分配**：`encrypt()` / `decrypt()` 内部分配输出缓冲。

本仓库未提供官方性能基准，具体数值请在目标平台自行测量。

### 9.12.2 优化建议

- 启用 AES-NI / ARMv8 Crypto 扩展。
- 高频小消息可考虑聚合后再加密。
- 高频路径优先对称模式；非对称模式适用于"会话初始化"或低频高敏感链路。
- 接入 HSM / 安全芯片时使用 `encrypt_callback` / `decrypt_callback`。
- 对非敏感 topic 保持 `kWithoutSecurity`（默认即是，零额外开销）。

### 9.12.3 密钥管理

```
不要将密钥/PEM 硬编码在源代码中，推荐做法：
- 从环境变量读取
- 从加密配置文件读取（如 HSM 保护的密钥库）
- 使用密钥派生函数（KDF）从主密钥派生通信密钥
```

```cpp
const char* pass_env = std::getenv("VLINK_SECURITY_PASS");

if (!pass_env) {
    VLOG_E("Security passphrase not configured!");
    return -1;
}

vlink::Security::Config cfg;
cfg.passphrase = pass_env;
cfg.pbkdf2_salt = load_salt_from_secure_store();

vlink::SecurityPublisher<MyMsg> pub("dds://secure/topic", cfg);
```

### 9.12.4 双端配置一致

Publisher（或 Client/Setter）与 Subscriber（或 Server/Getter）必须使用**等价的 `Config`**：

- 对称：相同 `key` 或相同 `passphrase + salt + iterations`。
- 非对称：发送方 `public_key_pem` 对应接收方 `private_key_pem`；签名 / 验签同理。
- 自定义回调：双端使用相同算法与密钥。

配置不一致时，解密失败、消息被静默丢弃，日志会记录 GCM tag mismatch / RSA unwrap failure 等原因。

### 9.12.5 不要混用安全和非安全节点

同一 topic 上的安全节点无法与普通节点正常通信：

错误示例（一端安全，另一端不安全）：

```cpp
vlink::Security::Config cfg;
cfg.key = "shared-secret";

vlink::SecurityPublisher<vlink::Bytes> pub("dds://topic", cfg);
vlink::Subscriber<vlink::Bytes> sub("dds://topic");
```

正确做法（双端均启用安全，`Config` 一致）：

```cpp
vlink::Security::Config cfg;
cfg.key = "shared-secret";

vlink::SecurityPublisher<vlink::Bytes> pub("dds://topic", cfg);
vlink::SecuritySubscriber<vlink::Bytes> sub("dds://topic", cfg);
```

> 提醒：构造 `SecurityXxx` 时不传 `Security::Config`（或传空 `Config{}`）会使用内置默认安全槽位，不会因空配置 fatal，也不会自动 fall back 到明文。显式配置无效或传输不支持时仍会初始化失败。

### 9.12.6 不替代传输层安全

消息级加密保护 payload 内容，不保护：

- 元数据（topic 名称、发现消息、消息长度）
- 传输层握手和发现协议
- 进程重启后的持久化 replay 状态；内置滑动窗口是内存态，不替代传输层会话防护
- 传输层重发同一密文（例如可靠传输重试或 QoS redelivery）时，若 sequence 仍落在 replay window 内，接收端会按 replay 拒绝并丢弃该消息；排查“少一条消息”时请同时看 security 日志和传输重发行为

纵深防御：传输层 TLS（MQTT）、DDS Security 插件加上 VLink 消息级加密。

### 9.12.7 `intra://` 与 CDR 场景

`intra://` 和 `dds://`+CDR 组合**不生效**（见 9.9 节）。如需保护 CDR 链路，使用 DDS Security 插件或传输层 TLS。

### 9.12.8 安全测试建议

```cpp
vlink::Security::Config cfg;
cfg.key = "test-key-16-byte";
vlink::Security sec(cfg);

vlink::Bytes plain = vlink::Bytes::from_string("hello world");
vlink::Bytes cipher;
vlink::Bytes recovered;

bool enc_ok = sec.encrypt(plain, cipher);
bool dec_ok = sec.decrypt(cipher, recovered);

assert(enc_ok && dec_ok);
assert(plain == recovered);
assert(plain != cipher);
```

实际部署时通常使用独立的 sender / receiver `Security` 实例；上面的同实例自加解密只适合单次 roundtrip 验证。默认 replay window 开启时，同一份密文再次 `decrypt()` 会被按 replay 拒绝。

---

**相关文档：**

- 传输后端安全兼容性详情请参阅 [传输后端与 URL](07-transport.md)
- 序列化层与安全管道的关系请参阅 [序列化](06-serialization.md)
- Node 生命周期与延迟初始化请参阅 [Node 生命周期](02-node-lifecycle.md)
- Bytes 类的详细 API 请参阅 [基础库](11-base-library.md)
