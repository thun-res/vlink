# helloworld — 多协议 Method + Event 综合示例

vlink samples 里最综合的入门样例。同一份代码可以在 6 种传输后端（dds / ddsc / shm / someip / fdbus / qnx）上跑，由环境变量切换。同时演示 Method（RPC）与 Event（pub/sub）两种通信模型，使用 Protobuf 序列化。

读完本示例你能掌握：

- 单进程内 Server + Publisher + Timer 同时存在的标准骨架。
- Client + Subscriber 两种模式（订阅模式 `sub` / 设置模式 `set`）。
- 通过环境变量切换传输协议的工程模式。
- `Utils::check_singleton` 防重复启动、`register_terminate_signal` 优雅退出的生产范式。

## 文件结构

```
helloworld/
  helloworld_common.h         # transport URL 辅助（thin wrapper over ../common_transport.h）
  proto/helloworld.proto      # Request / Response / Message 三个 Proto 消息
  server/helloworld_server.cc # Server + Publisher + Timer
  client/helloworld_client.cc # Client + Subscriber（sub / set 模式）
```

## 演示内容

- **Method**（`Server` / `Client`）：客户端发两个 int，server 返回它们的和。
- **Event**（`Publisher` / `Subscriber`）：server 每 100ms 发一条递增编号的消息。

## 运行

```bash
# 默认 dds 传输（终端 1）
./build/output/bin/sample_helloworld_server

# 终端 2：订阅事件消息
./build/output/bin/sample_helloworld_client sub

# 终端 3：发起 RPC 调用
./build/output/bin/sample_helloworld_client set 10 20
# 输出: Receive sum: 30
```

## 切换传输协议

| 后端 | 环境变量 | 前置条件 |
|------|---------|---------|
| `dds`（默认） | — | — |
| `ddsc` | `METHOD_TRANSPORT=ddsc EVENT_TRANSPORT=ddsc` | — |
| `shm` | `METHOD_TRANSPORT=shm EVENT_TRANSPORT=shm` | `iox-roudi &` |
| `someip` | `METHOD_TRANSPORT=someip EVENT_TRANSPORT=someip` | vsomeip routing manager |
| `fdbus` | `METHOD_TRANSPORT=fdbus EVENT_TRANSPORT=fdbus` | `fdb_name_server &` |
| `qnx` | `METHOD_TRANSPORT=qnx EVENT_TRANSPORT=qnx` | QNX target |

也可以用 `METHOD_URL` / `EVENT_URL` 直接传完整 URL，覆盖 `*_TRANSPORT` 选项：

```bash
METHOD_URL="someip://0x01/0x02?method=0x1" \
EVENT_URL="someip://0x01/0x02?groups=0x1&event=0x2" \
  ./build/output/bin/sample_helloworld_server
```

## 核心 API

| API | 说明 |
|-----|------|
| `Server<Request, Response>` | 服务端，listen 处理请求 |
| `Client<Request, Response>` | 客户端，invoke 发起调用 |
| `Publisher<Message>` | 事件发布 |
| `Subscriber<Message>` | 事件订阅 |
| `Timer` | 周期触发 publish |
| `Utils::check_singleton(name)` | 同名进程互斥锁 |
| `Utils::register_terminate_signal(cb)` | SIGINT/SIGTERM 回调 |
| `vlink_generate_cpp(PROTO ...)` | CMake 助手，编译 .proto 为 .h/.cc |

## 代码导读

### 1. Server

```cpp
int main(int argc, char* argv[]) {
  if (!Utils::check_singleton("helloworld_server")) {
    std::cerr << "Program has started." << std::endl;
    return 1;
  }

  MessageLoop message_loop;
  Utils::register_terminate_signal([&message_loop](int) { message_loop.quit(); });

  // Method 服务
  Server<Helloworld::Request, Helloworld::Response> server(Common::get_method_url());
  server.listen([](const Helloworld::Request& req, Helloworld::Response& resp) {
    resp.set_sum(req.left() + req.right());
    CLOG_D("[Server] Receive left = %d, right = %d.", req.left(), req.right());
  });

  // Event 发布
  Publisher<Helloworld::Message> pub(Common::get_event_url());
  Timer timer;
  timer.attach(&message_loop);
  timer.set_interval(100);
  timer.set_loop_count(Timer::kInfinite);
  int index = 0;
  timer.start([&pub, &index]() {
    ++index;
    Helloworld::Message msg;
    msg.set_detail("hello_world_" + std::to_string(index));
    pub.publish(msg);
  });

  message_loop.run();
  return 0;
}
```

### 2. Client（sub 模式）

```cpp
Subscriber<Helloworld::Message> sub(Common::get_event_url());
sub.listen([](const Helloworld::Message& msg) {
  CLOG_D("[Subscriber] Receive: %s", msg.detail().c_str());
});
message_loop.run();
```

### 3. Client（set 模式）

```cpp
Client<Helloworld::Request, Helloworld::Response> client(Common::get_method_url());
client.wait_for_connected();

Helloworld::Request req;
req.set_left(std::stoi(argv[2]));
req.set_right(std::stoi(argv[3]));

Helloworld::Response resp;
if (client.invoke(req, resp, 3s)) {
  CLOG_I("[Client] Receive sum: %d.", resp.sum());
}
```

## 构建

```bash
# 从 vlink 源码树构建
cd /work/vlink
cmake -B build -S . -DENABLE_EXAMPLES=ON
cmake --build build -j$(nproc)

# 产物
ls build/output/bin/sample_helloworld_*
```

`vlink_generate_cpp(PROTO ${PROTO_SRCS})` 在 CMakeLists.txt 中自动调用 protoc 生成 `helloworld.pb.h` / `.pb.cc`。

## 常见陷阱

1. **`shm` 后端没启 RouDi**：server 启动失败；先 `iox-roudi &`。
2. **`someip` 后端没启 vsomeipd**：discovery 不工作；先启路由 manager。
3. **fdbus 没启 fdb_name_server**：同上。
4. **重复启动 server**：`check_singleton` 拒绝；第二个进程退出。
5. **Client invoke 在 server 启动前**：超时返回；先 `wait_for_connected`。

## 设计要点

- 同一份代码切换 6 种传输 = vlink 设计目标的最强体现。
- check_singleton 用 vlink Utils 跨平台实现（/tmp 文件锁）。
- Timer + MessageLoop + register_terminate_signal 是 vlink 长跑业务的标准三件套。

## 配图

无专属配图。samples 的整体关系图见 `../images/samples-relationship.png`。

## 参考

- `../ping_pong/` — Bytes 形式的延迟测量
- `../shm_raw/` — 在 shm 上叠加 Security
- `../common_transport.h` — 共享的 URL 切换 helper
- 顶层 `doc/22-examples.md` — samples 章节
