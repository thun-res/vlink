# 🔒 shm_raw —— 共享内存 + 加密的全六原语示例

单进程内用 `shm://`（Iceoryx 零拷贝共享内存）一次性演示 VLink 全部六种通信原语，且全部走 `Security*` 加密变体。适用场景：同主机沙箱进程间通信，即使数据不出本机也要求机密性。

## 🧩 核心 API

| API | 用途 |
|-----|------|
| `SecurityServer<Req,Resp>` / `SecurityClient<Req,Resp>` | 加密 RPC（Method 模型） |
| `SecurityPublisher<T>` / `SecuritySubscriber<T>` | 加密发布 / 订阅（Event 模型） |
| `SecuritySetter<T>` / `SecurityGetter<T>` | 加密状态字段（Field 模型） |
| `Security::Config{ .key = "..." }` | 第二个构造参数，两端 key 一致即可通信 |
| `client.invoke(req)` | 同步调用，返回 `std::optional<Resp>` |
| `pub.wait_for_subscribers()` / `pub.publish(msg)` | 等订阅者就绪后发布 |
| `setter.set(v)` / `getter.get()` | 写 / 读最新字段值（`get` 返回 `optional`） |

`Security*` 是各原语的加密替身，构造参数与调用方式与普通版本完全一致，仅多传一个 `Security::Config`。

## ⚡ 最小片段

```cpp
vlink::Security::Config cfg;
cfg.key = "rpc-shared-key";  // 两端 key 一致即可互通

vlink::SecurityServer<vlink::Bytes, vlink::Bytes> server("shm://example_raw/method", cfg);
server.listen([](const vlink::Bytes& req, vlink::Bytes& resp) {
  if (req == vlink::Bytes{0x1, 0x2, 0x3}) {
    resp = vlink::Bytes::create(1024 * 1024);
  }
});

vlink::SecurityClient<vlink::Bytes, vlink::Bytes> client("shm://example_raw/method", cfg);
auto resp = client.invoke(vlink::Bytes{0x1, 0x2, 0x3});  // std::optional<Bytes>
```

Event / Field 用法同理：`pub.wait_for_subscribers(); pub.publish(...)`；`setter.set(...); getter.get()`。

## 🚀 构建与运行

```bash
cmake -B build -S . -DCMAKE_PREFIX_PATH=<vlink安装路径>
cmake --build build

iox-roudi &                        # shm:// 端点依赖 Iceoryx RouDi 守护进程
./build/output/bin/sample_shm_raw  # 单进程内完成全部通信
```

## 🧭 模型选择

| 场景 | 选型 |
|------|------|
| 同主机进程间通信、要求低延迟与机密性 | `shm://` + `Security*` |
| 无需加密 | 去掉 `Security` 前缀即为普通 `Publisher` / `Client` / `Setter` |
| 跨主机或需服务发现 | 将 URL 前缀换成 `dds://`、`zenoh://` 等，代码不变 |

## 🔗 参考

- 三种通信模型对比：[samples 索引](../README.md)
- 零拷贝与加密通信详解：[doc/06-zerocopy.md](../../../doc/06-zerocopy.md)
