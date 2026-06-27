# 📡 ddsc_proto —— CycloneDDS + Protobuf 示例

用 `ddsc://`（CycloneDDS 后端）搭配 Protobuf 序列化，在单进程内演示 Method（RPC）与 Event（pub/sub）两种通信模型。Protobuf 消息由框架自动包装为 CDR 兼容载荷，无需注册类型或手写序列化器。

## 🧩 核心 API

| API | 用途 |
|-----|------|
| `Server<Req, Resp>` / `listen(cb)` | RPC 服务端，回调中填充 `Resp` |
| `Client<Req, Resp>` / `invoke(req)` | RPC 客户端，返回 `std::optional<Resp>` |
| `client.detect_connected(cb)` | 异步监控服务端连接状态 |
| `Publisher<Msg>` / `publish(msg)` | 事件发布 |
| `Subscriber<Msg>` / `listen(cb)` | 事件订阅 |
| `pub.wait_for_subscribers()` | 发布前阻塞至订阅者就绪，避免与发现过程竞态 |

## ⚡ 最小片段

```cpp
vlink::Server<pb::Request, pb::Response> server("ddsc://phone/method");
server.listen([](const pb::Request& req, pb::Response& resp) {
  if (req.type() == 10086) {
    resp.set_value("calling...");
  }
});

vlink::Client<pb::Request, pb::Response> client("ddsc://phone/method");
client.detect_connected([](bool connected) { VLOG_I("server status:", connected); });
auto resp = client.invoke(req);  // std::optional<pb::Response>
```

Event 用法同理：`pub.wait_for_subscribers(); pub.publish(msg);`。

## 📦 文件

| 文件 | 说明 |
|------|------|
| `ddsc_proto.cc` | 主程序，单进程内完成 RPC 与事件通信 |
| `ddsc_proto.proto` | Protobuf 消息定义（Request / Response / Message） |
| `CMakeLists.txt` | 构建配置，链接 `vlink::ddsc` |

## 🚀 构建与运行

```bash
cmake -B build -S . -DCMAKE_PREFIX_PATH=<vlink安装路径>
cmake --build build
./build/output/bin/sample_ddsc_proto
```

依赖：`vlink::ddsc`（CycloneDDS 后端）、Protobuf。

## 🔗 参考

- `../helloworld/` —— 多后端可切换的 Method + Event 综合示例
- 消息序列化机制：[doc/03-serialization.md](../../../doc/03-serialization.md)
- 传输后端与 URL：[doc/04-transport.md](../../../doc/04-transport.md)
- samples 索引：[samples/README.md](../README.md)
