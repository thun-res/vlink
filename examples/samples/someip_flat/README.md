# 🚗 someip_flat —— SOME/IP + FlatBuffers 三模型示例

用 `someip://`（vsomeip 后端）搭配 FlatBuffers 零拷贝序列化，在单进程内跑通 Method / Event / Field 三种通信模型。典型场景：AUTOSAR Adaptive / 车载以太网服务栈，SOME/IP 为强制协议，FlatBuffers 只读零拷贝降低单帧 CPU 开销。

## 🧩 核心 API

| API | 用途 |
|-----|------|
| `Client<ReqT>::send(req)` | 单向发送请求（Client 无 Resp 模板参数即为发送模式） |
| `Server<Req*>::listen(cb)` | 注册回调接收请求，回调入参仅回调内有效 |
| `client.wait_for_connected()` | 阻塞直到 SOME/IP 服务发现匹配到 Server |
| `Publisher<MsgT>::publish(msg)` | 发布事件 |
| `Subscriber<Msg*>::listen(cb)` | 订阅事件 |
| `Setter<T>::set(v)` / `Getter<T>::get()` | 设置 / 读取最新字段值 |

FlatBuffers 类型约定（框架自动识别，无需手动设置编解码）：`fbs::XxxT` 为值类型（可写，发送 / 本地构造用）；`fbs::Xxx*` 为只读指针（零拷贝读，接收用，仅回调内有效）。

## ⚡ 最小可运行片段

```cpp
// Method —— fire-and-forget RPC
vlink::Server<fbs::Request*> server("someip://0x1/0x2?method=0x3");
server.listen([](const fbs::Request* req) { VLOG_I("type:", req->type()); });

vlink::Client<fbs::RequestT> client("someip://0x1/0x2?method=0x3");
fbs::RequestT req;
req.type = 100;
client.wait_for_connected();   // 等待服务发现匹配
client.send(req);              // 单向发送

// Event —— Pub/Sub
vlink::Subscriber<fbs::Message*> sub("someip://0x1/0x3?groups=0x1|0x2&event=0x3");
sub.listen([](const fbs::Message* msg) { VLOG_I("event:", msg->type()); });
vlink::Publisher<fbs::MessageT> pub("someip://0x1/0x3?groups=0x1|0x2&event=0x3");

// Field —— 状态读写（追加 &field=1）
vlink::Setter<fbs::MessageT> setter("someip://0x1/0x4?groups=0x1|0x2&event=0x4&field=1");
vlink::Getter<fbs::MessageT> getter("someip://0x1/0x4?groups=0x1|0x2&event=0x4&field=1");
```

## 🔗 SOME/IP URL 格式

服务 / 实例 / 方法均用十六进制 ID，多个事件组用 `|` 分隔：

```
someip://服务ID/实例ID?method=方法ID                     —— Method（RPC）
someip://服务ID/实例ID?groups=组ID&event=事件ID          —— Event
someip://服务ID/实例ID?groups=组ID&event=事件ID&field=1  —— Field
```

## 🧭 模型选择

| 场景 | 模型 |
|------|------|
| 请求响应或单向命令下发 | Method（本例为无 Resp 的 fire-and-forget） |
| 一对多广播、订阅者随到随收 | Event |
| 仅关心最新状态值、晚加入者也立即获取 | Field（`&field=1`） |

换后端只改 URL 前缀；换 Protobuf / CDR 等只改消息类型，框架自动识别。

## 🚀 运行

```bash
cmake -B build -S . -DCMAKE_PREFIX_PATH=<vlink安装路径>
cmake --build build
./build/output/bin/sample_someip_flat   # 需先启动 vsomeip 路由管理器（vsomeipd）
```

无 vsomeip 环境时程序会阻塞等待连接。

## 📚 相关文档

- 传输后端与 URL 详解：[doc/04-transport.md](../../../doc/04-transport.md)
- samples 索引：[samples/README.md](../README.md)
