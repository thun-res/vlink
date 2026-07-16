# 🔏 7. 安全加密

安全加密是 VLink 在数据路径上施加的一类非侵入能力：在不可信链路（车云通道、跨域 DDS、MQTT 等）上保护 payload 的机密性与完整性。它遵循统一的 URL 契约——业务代码仍只面对六个通信原语，能力以“节点变体 `Security*`”或“`Security::Config` 配置参数”的形式接入，调用方式不因启用而改变。从数据流位置看，加密在序列化之后、传输之前透明插入；接口可与共享内存后端组合，但安全节点会关闭 transport loan，先生成密文再由后端传输，因此不保留端到端零拷贝。

![安全加密管道](images/security-pipeline.png)

---

## 🔒 7.1 消息级安全加密：机制与数据流

VLink 在序列化层与传输层之间提供一层应用级认证加密（authenticated encryption）：消息在离开本端前被加密与认证，在进入对端业务前被验证与解密。它解决的是在不可信链路（车云通道、跨域 DDS、MQTT 等）上保护 payload 机密性与完整性的问题。加密发生在序列化之后、传输之前，解密发生在接收之后、反序列化之前，因此对上层业务与多数后端透明，且加密对象始终是完整的序列化字节流，与消息类型无关。

![序列化与加密管道位置](images/serialization-flow.png)

每个端点（六原语之一）至多持有一个安全上下文，其行为由三个不变量界定：

- **透明接入**：将普通节点类型替换为对应的 `Security*` 变体时，调用签名与业务消息类型不变；发送返回值和回调投递额外受加解密、验签与防重放结果约束。
- **初始化前配置**：可在构造时传入 `Security::Config`，也可对延迟初始化节点在 `init()` 前调用 `enable_security()`；节点初始化后不可再替换配置，更换运行中端点的密钥或回调需重建节点。
- **零侵入**：未使用 `Security*` 变体的端点不引入任何加密代码路径。

框架支持三种工作模式，按 `Config` 中实际填写的字段自动选择，优先级为**自定义回调 > 非对称 > 对称**：

| 模式 | 触发字段 | 机制 | 适用 |
| --- | --- | --- | --- |
| 对称 | `key` 或 `passphrase` | AEAD，每端预共享密钥 | 双端可安全共享密钥的高频链路 |
| 非对称 | `public_key_pem` / `private_key_pem` | 每消息生成会话密钥，以对端公钥包装后随密文传输 | 无法预共享对称密钥、需公钥分发 |
| 自定义 | `encrypt_callback` + `decrypt_callback` | 完全绕过内置算法 | 接入 SM4 / ChaCha20 / HSM / 白盒密码 |

---

## 🚀 7.2 快速开始

最常见用法：将节点类型替换为对应 `Security*` 变体，传入一个携带预共享 `key` 的 `Security::Config`。收发双端配置必须一致。完整示例见 `examples/security/security_basic`。

```cpp
#include <vlink/vlink.h>

vlink::Security::Config cfg;
cfg.key = "my-shared-secret";

vlink::SecuritySubscriber<vlink::Bytes> sub("dds://secure/topic", cfg);
sub.listen([](const vlink::Bytes& msg) { VLOG_I("recv:", msg.to_string()); });

vlink::SecurityPublisher<vlink::Bytes> pub("dds://secure/topic", cfg);
pub.wait_for_subscribers();
pub.publish(vlink::Bytes::from_string("hello"));
```

---

## 🧩 7.3 节点变体

六个 `Security*` 变体一一对应三种通信模型的收发两端，模板参数与同名普通节点完全相同；加密由发送端施加、解密由接收端施加。

| 通信模型 | 发送端（加密） | 接收端（解密） |
| --- | --- | --- |
| 事件 Event | `SecurityPublisher<T>` | `SecuritySubscriber<T>` |
| 方法 Method | `SecurityClient<Req,Resp>` | `SecurityServer<Req,Resp>` |
| 字段 Field | `SecuritySetter<T>` | `SecurityGetter<T>` |

> 表中"发送端 / 接收端"以事件、字段模型的单向数据流为准。方法模型为双向：`SecurityClient` 对请求加密、对响应解密，`SecurityServer` 对请求解密、对响应加密；因此当 `Resp` 非空时双端都同时需要 `can_encrypt()` 与 `can_decrypt()` 能力，非对称模式下两端各需配齐公钥与私钥。

每个变体提供与普通节点一致的两种构造路径，区别仅在于追加一个 `Security::Config` 参数：

| 构造路径 | 形式 |
| --- | --- |
| 直接构造 | `SecurityXxx(url, sec_cfg = {}, InitType type = kWithInit)` |
| 工厂构造 | `SecurityXxx::create_unique(url, sec_cfg = {}, type)` / `create_shared(...)` |

`sec_cfg` 缺省时（编译启用 `VLINK_ENABLE_SECURITY`）落入内置默认密钥，仅供开发（见 [7.6](#-76-约束与边界条件)）。`type` 控制是否在构造时即调用 `init()`，语义同普通节点（见 [通信模型](02-communication.md)）。

---

## 🔑 7.4 Security::Config 字段

`Config` 各字段独立解析：空字符串、空 `Bytes`、空回调表示"留空"，非空字段在构造时校验并安装。下表为高频字段。

| 字段 | 模式 | 语义 |
| --- | --- | --- |
| `key` | 对称 | 原始高熵密钥种子（来自 KMS/HSM），框架以 SHA-256 截断派生 AES-128 密钥 |
| `passphrase` + `pbkdf2_salt` | 对称 | 低熵口令；必须配 salt（必须 ≥ 16 字节，否则密钥派生失败导致节点构造期 fatal，须经安全渠道共享） |
| `public_key_pem` | 非对称 | 发送端持有的对端公钥 |
| `private_key_pem` | 非对称 | 接收端持有的自身私钥 |
| `encrypt_callback` + `decrypt_callback` | 自定义 | 必须成对设置 |

低频策略集中在 `Config::advanced`：`signing_key_pem` / `verify_key_pem`（均为 RSA 私钥 / 公钥 PEM）用于非对称模式下的发送方身份签名与验签（RSA-PSS-SHA256，仅在非对称加密路径生效，对称模式不签名）；`aad_context`（≤ 65535 字节，留空时框架自动以 `url|序列化类型|schema 类型` 填充并绑定进 AEAD AAD）、`replay_window`（默认 `4096`，滑动防重放窗口消息数，`0` 关闭防重放）用于绑定通道上下文与防重放。按需填写，不影响主流程。

RSA 相关 PEM 可经工厂方法从文件直接加载：`from_public_key_path(path)`、`from_private_key_path(path)`、`from_key_paths(pub, priv)` 均返回预填好的 `Config`。

---

## 🔐 7.5 三种模式

### 7.5.1 对称模式

适用于双端可安全预共享密钥的最常见场景。`key` 直接提供原始密钥；`passphrase` 适合人类口令，必须搭配 `pbkdf2_salt`。

```cpp
vlink::Security::Config cfg;
cfg.key = "my-secret";

vlink::Security::Config cfg2;
cfg2.passphrase = "correct horse battery staple";
cfg2.pbkdf2_salt = shared_salt;
```

边界条件：`passphrase` 路径下双端的 `passphrase`、`pbkdf2_salt`、`pbkdf2_iterations` 三者必须全部一致，否则派生出的密钥不同，解密失败。

### 7.5.2 非对称模式

发送方无需预共享对称密钥；每条消息独立生成会话密钥，以对端公钥包装后随密文发送。所有 RSA PEM 必须 ≥ 2048 位。可选地启用发送方签名以校验消息来源。

```cpp
auto sender_cfg = vlink::Security::from_public_key_path("peer_pub.pem");
sender_cfg.advanced.signing_key_pem = own_priv_pem;
vlink::SecurityPublisher<MyMsg> pub("dds://secure/topic", sender_cfg);

auto receiver_cfg = vlink::Security::from_private_key_path("own_priv.pem");
receiver_cfg.advanced.verify_key_pem = peer_pub_pem;
vlink::SecuritySubscriber<MyMsg> sub("dds://secure/topic", receiver_cfg);
```

边界条件：省略 `signing_key_pem` / `verify_key_pem` 时仍可正常加解密，但不校验发送方身份；设置 `verify_key_pem` 后，未携带有效签名的消息将被拒绝。

### 7.5.3 自定义回调

成对安装 `encrypt_callback` 与 `decrypt_callback`，完全绕过内置算法，用于接入 SM4、ChaCha20、HSM、白盒密码等。回调签名为 `Function<bool(const Bytes& in, Bytes& out)>`（即 `Security::Callback`）：发送端读 `in` 写出密文，接收端反之；实现必须填充 `out` 并在成功时返回 `true`，返回 `false` 视为失败——发送方 `encrypt` 失败使 `publish` / `invoke` / `set` 直接返回 `false`（消息不发送），接收方 `decrypt` 失败则在进入业务回调前丢弃该消息。仅安装单个回调（缺另一个）会被告警并双双忽略，以避免加解密不对称。

```cpp
auto xor_codec = [](const vlink::Bytes& in, vlink::Bytes& out) -> bool {
  out = vlink::Bytes::create(in.size());
  for (size_t i = 0; i < in.size(); ++i) {
    out[i] = in[i] ^ 0xAB;
  }
  return true;
};

vlink::Security::Config cfg;
cfg.encrypt_callback = xor_codec;
cfg.decrypt_callback = xor_codec;
vlink::SecurityPublisher<vlink::Bytes> pub("dds://secure/data", cfg);
vlink::SecuritySubscriber<vlink::Bytes> sub("dds://secure/data", cfg);
```

自定义回调不依赖 OpenSSL，因此在关闭 `ENABLE_SECURITY` 时仍然可用。

---

## 🚧 7.6 约束与边界条件

- **双端配置必须一致**。对称模式同 `key`，或同 `passphrase + salt + iterations`；非对称模式发送方 `public_key_pem` 须对应接收方 `private_key_pem`，签名/验签同理；自定义回调双端须使用相同算法与密钥。不一致时解密失败，消息被静默丢弃（日志记录 tag 不匹配等原因）。
- **安全节点与普通节点不可混用**。同一 topic 上一端为 `SecurityPublisher`、另一端为普通 `Subscriber` 将无法通信；双端必须同时启用安全且配置一致。
- **空配置仅供开发**。在 `VLINK_ENABLE_SECURITY` 已编译时，缺省 `Config{}` 落入内置默认对称槽位，`is_configured()` 为 `true`，构造不会 fatal，且不会回退到明文；生产环境必须显式配置。一旦填入显式字段，密钥校验失败时也不回退到默认密钥（失败字段直接留空）。若关闭 `VLINK_ENABLE_SECURITY`，所有内置算法字段被忽略并告警，空配置使 `is_configured()` 返回 `false`，`enable_security()` 打印 `Security::Config has no usable slot.` 并返回 `false`、不安装安全对象；此时安全节点变体默认随构造执行的 `init()` 仍因缺少可用 `Security` 抛出 `Exception::RuntimeError`（仅自定义回调模式可在该编译配置下正常工作）。
- **不保护元数据**。topic 名、发现消息、消息长度与传输层握手均不在保护范围；敏感场景须叠加传输层 TLS（[7.8](#-78-传输层-tls)）或 DDS Security 插件。

---

## 🧱 7.7 编译开关与不支持的传输

内置 AEAD / RSA 模式依赖 OpenSSL，由顶层 CMake 选项 `ENABLE_SECURITY` 控制（默认 `ON`，对应宏 `VLINK_ENABLE_SECURITY`）：

```cmake
cmake -DENABLE_SECURITY=ON ...
target_link_libraries(my_app PRIVATE vlink::vlink)
```

关闭该选项后，内置对称 / 非对称路径不可用，仅自定义回调模式仍可工作。

以下传输组合不支持消息级加密：构造时 `enable_security()` 打印警告（`Security::Config will ignore intra/dds(cdr) transport.`）并返回 `false`，**不安装任何安全对象**。由于安全节点变体默认以 `InitType::kWithInit` 构造，紧随其后的 `init()` 检测到无可用 `Security`，会以 `VLOG_F` 致命级日志抛出 `Exception::RuntimeError`（即构造期 fatal）。须由调用方自行避免在不可信链路上对这些传输使用 `Security*` 变体；若需在 `init()` 前自查配置，可用 `InitType::kWithoutInit` 延迟初始化并在内部 `Security` 上检查 `can_encrypt()` / `can_decrypt()`。机制原因与替代方案见 [传输后端与 URL](04-transport.md)：

- `intra://`：当前实现显式排除整个 intra 路径；其中 `shared_ptr<IntraDataType>` 专用对象路径还会对安全模板触发编译期 `static_assert`。普通 intra 消息虽经 Bytes 编解码，也不因此启用安全层。
- `dds://` 配合 CDR 类型：CDR 直接交由 DDS 处理，应改用 DDS Security 插件或传输层 TLS。

其余后端（`shm://`、`shm2://`、`ddsc://`、`ddsr://`、`ddst://`、`zenoh://`、`mqtt://`、`fdbus://`、`someip://`、`qnx://`，以及 `dds://` 的非 CDR 类型）均支持。

![后端选择决策树](images/transport-decision-tree.png)

---

## 🛡️ 7.8 传输层 TLS

`VLINK_SSL_*` 系列环境变量（`VLINK_SSL_CA` / `_CERT` / `_KEY` / `_KEY_PASS` / `_VERIFY` / `_SNI` / `_CIPHERS`，分别映射到 `ssl.ca` / `ssl.cert` / `ssl.key` / `ssl.key_password` / `ssl.verify` / `ssl.server_name` / `ssl.ciphers` 属性）为支持 TLS 的传输端点提供 CA、客户端证书、证书校验等配置；显式属性优先于环境变量。具备原生 TLS 机制的后端为 `mqtt://`（Paho `MQTTClient_SSLOptions`，自动将 `tcp://` 升级为 `ssl://`）、`dds://`（Fast-DDS `TCPv4TransportDescriptor::tls_config`）、`ddsc://`（CycloneDDS，需编译期 `DDS_HAS_SSL`）、`ddsr://`（RTI Connext DDS，`SslOptions::parse_from` 写入 `dds.transport.tcp.tcp1.tls.*` 属性并启用 `NDDS_TRANSPORT_CLASSID_TLSV4_LAN`）、`zenoh://`（zenoh-c；zenoh-pico 需以 `Z_FEATURE_LINK_TLS=1` 构建）。除手动 `set_ssl_options()` / `set_property("ssl.ca", ...)` 外，`ca_file` 或 `cert_file` 任一非空即自动启用 TLS，无独立开关；DDS/CycloneDDS 启用 TLS 会强制改用 TCP 传输。Zenoh 1.x 映射 CA、连接/监听证书与私钥、mTLS 和 hostname 校验，但不支持 VLink 的 `ssl.server_name`、`ssl.key_password`、`ssl.ciphers` 覆盖。这属于链路层安全，与本章的消息级加密是两个独立层次，可叠加形成纵深防御：消息级加密保护 payload 端到端机密性，TLS 保护链路握手与传输通道。完整变量清单见 [集成与环境](13-integration.md)。

---

## 📊 7.9 性能特征与实践判据

| 维度 | 对称（AES-128-GCM） | 非对称（RSA 混合） |
| --- | --- | --- |
| 每消息开销 | 一次 AEAD 加密/解密与认证 | 每消息生成 AES 密钥并做 RSA-OAEP 封装/解封；启用签名时再做 RSA-PSS 签名/验签 |
| 适用频率 | 通常适合较高频消息；仍需按目标 CPU/OpenSSL/载荷实测 | 计算开销更高，是否满足频率要求须按密钥位数、签名配置和目标平台实测 |
| 密钥分发 | 需安全渠道预共享 | 公钥可公开分发 |

- **PBKDF2 仅算一次**：在节点构造时计算，不在热路径；`pbkdf2_iterations` 默认 `200000`，按目标硬件调整。
- **密钥不得硬编码**：从环境变量或受保护密钥库读取，或经 KDF 从主密钥派生。
- **非敏感 topic 使用普通节点**：避免不必要的加密开销。

```cpp
const char* pass = std::getenv("VLINK_SECURITY_PASS");

if (pass == nullptr) {
  VLOG_E("security passphrase not configured");
  return -1;
}

vlink::Security::Config cfg;
cfg.passphrase = pass;
cfg.pbkdf2_salt = load_salt_from_secure_store();
vlink::SecurityPublisher<MyMsg> pub("dds://secure/topic", cfg);
```

---

## 🔗 相关文档

- [通信模型](02-communication.md) —— Publisher / Subscriber 收发用法与节点生命周期
- [消息序列化](03-serialization.md) —— 序列化类型、零拷贝读写与加密管道的关系
- [传输后端与 URL](04-transport.md) —— 各后端加密兼容性与不支持的传输组合
- [零拷贝](06-zerocopy.md) —— 借贷接口与所有权约束；启用安全节点时 transport loan 被关闭
- [基础库](08-base-library.md) —— `Bytes` 类 API 与基础组件
- [集成与环境](13-integration.md) —— `VLINK_SSL_*` 等环境变量清单
