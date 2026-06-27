# 🚌 fdbus_proto —— FDBus + Protobuf 三模型示例

用 `fdbus://`（FDBus 后端）搭配 Protobuf 序列化，在单进程内跑通 VLink 全部三种通信模型：Method（RPC）、Event（pub/sub）、Field（状态读写）。

## 🧩 核心 API

| API | 用途 |
|-----|------|
| `Server<Req, Resp>` / `listen(cb)` | RPC 服务端 |
| `Client<Req, Resp>` / `invoke(req)` | RPC 客户端，返回 `std::optional<Resp>` |
| `client.detect_connected(cb)` | 异步监控服务端连接状态 |
| `Publisher<Msg>` / `Subscriber<Msg>` | 事件发布 / 订阅 |
| `pub.wait_for_subscribers()` | 发布前阻塞至订阅者就绪 |
| `Setter<Msg>::set(v)` / `Getter<Msg>::get()` | 写 / 读最新字段值（`get` 返回 `optional`） |

## ⚡ 最小片段

```cpp
// Method —— RPC
vlink::Server<pb::Request, pb::Response> server("fdbus://phone?event=req");
vlink::Client<pb::Request, pb::Response> client("fdbus://phone?event=req");
auto resp = client.invoke(req);

// Event —— Pub/Sub
vlink::Publisher<pb::Message> pub("fdbus://phone?event=time");
vlink::Subscriber<pb::Message> sub("fdbus://phone?event=time");

// Field —— 状态读写
vlink::Setter<pb::Message> setter("fdbus://phone?event=msg");
vlink::Getter<pb::Message> getter("fdbus://phone?event=msg");
auto ret = getter.get();  // std::optional
```

## 🔧 FDBus URL 格式

FDBus 用查询参数 `event=` 区分同一服务下的不同话题：

```
fdbus://服务名?event=话题名
```

例如 `fdbus://phone?event=req` 与 `fdbus://phone?event=time` 是同一 FDBus 服务下的两个话题。

## 📦 文件

| 文件 | 说明 |
|------|------|
| `fdbus_proto.cc` | 主程序，演示全部三种通信模型 |
| `fdbus_proto.proto` | Protobuf 消息定义（Request / Response / Message） |
| `CMakeLists.txt` | 构建配置，链接 `vlink::fdbus` |

## 🚀 构建与运行

```bash
cmake -B build -S . -DCMAKE_PREFIX_PATH=<vlink安装路径>
cmake --build build
fdb_name_server &                       # fdbus:// 端点依赖 FDBus name server
./build/output/bin/sample_fdbus_proto
```

依赖：`vlink::fdbus`（FDBus 后端）、Protobuf。

## 🔗 参考

- 三种通信模型详解：[doc/02-communication.md](../../../doc/02-communication.md)
- 传输后端与 URL：[doc/04-transport.md](../../../doc/04-transport.md)
- samples 索引：[samples/README.md](../README.md)
