# c_pubsub — 纯 C 实现的事件发布/订阅

本示例用约 70 行 C 代码完成一对 `Publisher` / `Subscriber` 的创建、匹配等待、连续发布、强制发布、回调统计、句柄销毁全流程。它是 `vlink/external/c_api.h` 最简单的入口点，跑通它意味着 C 工程的链接、Schema 元信息、回调 ABI 三件事都已就绪。

URL 采用 `intra://` 进程内传输，无需任何外部守护进程；想换成 DDS / SHM / SOME/IP 只要把字符串前缀换掉即可，回调和句柄签名完全一致。

## 背景与适用场景

VLink 的 C++ 模板 API 通过 `vlink::Publisher<T>` / `Subscriber<T>` 描述事件流；T 可以是任意 POD、字符串、Protobuf、FlatBuffers、字节流。但模板需要 C++ 编译器、运行时异常、RTTI、`std::function` 闭包，这些在嵌入式 C 工程、Rust/Go/Zig FFI、不愿引入 C++ 运行时的旧 C 代码库中并不友好。

`vlink_create_publisher` / `vlink_create_subscriber` 把模板特化收敛为「URL + Schema 描述 + 字节缓冲 + 函数指针回调」的最小 C 接口。`vlink_schema_info_t` 用 `ser`（点分模式名）和 `schema`（枚举家族）两个字段一次性表达 wire 元信息，与 C++ 端的 `set_ser_type()` 一一映射；Discovery、Proxy、Bag、Viewer 都会消费它。

事件模型的语义保持不变：1-to-N 广播，无连接握手，订阅者在线时才能收到消息，可选 `vlink_publish_by_force` 在无订阅者时强发（对端日后晚加入也无法看到这些消息）。

## 核心 API

| API | 签名 | 说明 |
|-----|------|------|
| `vlink_create_publisher` | `int (const char* url, const vlink_schema_info_t* schema, vlink_publisher_handle_t* handle)` | 创建 Publisher 句柄；`url` 决定传输后端，`schema` 描述消息元信息 |
| `vlink_create_subscriber` | `int (const char* url, const vlink_schema_info_t* schema, vlink_subscriber_handle_t* handle, vlink_message_callback_t cb, void* user_data)` | 创建 Subscriber 并立即注册回调 |
| `vlink_publish` | `int (vlink_publisher_handle_t handle, const uint8_t* data, size_t size)` | 异步广播；无订阅者时返回 `VLINK_RET_TRANSFER_ERROR` |
| `vlink_publish_by_force` | `int (vlink_publisher_handle_t handle, const uint8_t* data, size_t size)` | 即使无订阅者也广播（对支持的传输后端） |
| `vlink_wait_for_subscribers` | `int (vlink_publisher_handle_t handle, int timeout_ms)` | 阻塞等待第一个订阅者匹配 |
| `vlink_has_subscribers` | `int (vlink_publisher_handle_t handle)` | 立即查询是否已有匹配订阅者 |
| `vlink_detect_subscribers` | `int (vlink_publisher_handle_t handle, vlink_detect_callback_t cb, void* user_data)` | 注册"订阅者上线/下线"事件回调 |
| `vlink_destroy_publisher` / `vlink_destroy_subscriber` | `int (vlink_*_handle_t* handle)` | 释放原生资源；handle 是值类型，destroy 通过指针写回清零 |

回调签名：

```c
typedef void (*vlink_message_callback_t)(const uint8_t* data, size_t size, void* user_data);
```

API 签名核对入口：`/work/vlink/include/vlink/external/c_api.h` 第 347-460 行。

## 代码导读

完整源码 `c_pubsub.c` 共 96 行；按下面四个节拍展开。

### 1. 准备回调

回调拿到的 `data/size` 仅在函数返回前有效，需保留必须 `memcpy` 出来。这里只是打印并自增计数器：

```c
static int g_received_count = 0;

static void on_message(const uint8_t* data, const size_t size, void* user_data) {
  (void)user_data;
  printf("[Subscriber] Received %zu bytes: %.*s\n", size, (int)size, (const char*)data);
  g_received_count++;
}
```

### 2. 声明 Schema 元信息

`ser="text"` 是模式名，`VLINK_SCHEMA_RAW` 表示按原始字节流走、无结构化解码。这一对元信息将随 Discovery / Proxy / Bag / Viewer 流转：

```c
const vlink_schema_info_t schema = {"text", VLINK_SCHEMA_RAW};
```

### 3. 先创建 Subscriber 再创建 Publisher

VLink 的事件模型严格 1-to-N，必须确保订阅者在线时发布者才能匹配；先建 Subscriber 是一个常用顺序。`vlink_wait_for_subscribers(2000)` 阻塞最长 2 秒等待匹配：

```c
vlink_subscriber_handle_t sub;
ret = vlink_create_subscriber("intra://c_api/pubsub", &schema, &sub, on_message, NULL);

vlink_publisher_handle_t pub;
ret = vlink_create_publisher("intra://c_api/pubsub", &schema, &pub);

ret = vlink_wait_for_subscribers(pub, 2000);
printf("wait_for_subscribers: %s\n", ret == VLINK_RET_NO_ERROR ? "matched" : "timeout");
```

### 4. 连续发布 + 强制发布

主循环连续发 5 条文本消息，每条间隔 50 ms。`vlink_publish_by_force` 用于演示无订阅者时的强制路径——本例其实仍有订阅者，行为与普通 publish 一致：

```c
for (int i = 1; i <= 5; ++i) {
  char msg[64];
  int len = snprintf(msg, sizeof(msg), "Hello from C API #%d", i);
  ret = vlink_publish(pub, (const uint8_t*)msg, (size_t)len);
  sleep_ms(50);
}

const char* force_msg = "forced message";
ret = vlink_publish_by_force(pub, (const uint8_t*)force_msg, strlen(force_msg));
```

### 5. 清理

`vlink_destroy_*` 写回 handle 的 native 指针，再访问该 handle 是未定义行为：

```c
vlink_destroy_publisher(&pub);
vlink_destroy_subscriber(&sub);
```

## 运行

```bash
./build/output/bin/example_c_pubsub
```

预期输出（顺序可能因调度略有不同）：

```
wait_for_subscribers: matched
has_subscribers: yes
publish: "Hello from C API #1" (ret=0)
[Subscriber] Received 19 bytes: Hello from C API #1
publish: "Hello from C API #2" (ret=0)
[Subscriber] Received 19 bytes: Hello from C API #2
... (3 more)
publish_by_force ret=0
[Subscriber] Received 14 bytes: forced message
Total received: 6
```

`intra://` 不依赖任何守护进程，跑完即退。若要换其它传输，将两个 URL 同步换为 `dds://...`、`shm://...`（需要 `iox-roudi &`）或 `someip://0xN/0xM?...`（需要 `vsomeipd`）即可，代码无须改动。

## 常见陷阱

1. **`vlink_schema_info_t` 不是 hint**：跨进程/跨语言对端必须使用相同 `ser` + `schema`，错填导致 `VLINK_RET_TRANSFER_ERROR` 或对端解码失败。
2. **回调缓冲的生命周期**：`data/size` 在回调返回后立即失效；如需异步处理必须 `memcpy` 到自管缓冲。
3. **handle 销毁顺序**：先析构 Publisher 再析构 Subscriber 不会出错，但若回调里持有 Subscriber 的 `user_data` 引用 Publisher，要避免引用悬空。
4. **发布无订阅者**：默认 `vlink_publish` 在无匹配订阅时返回 `VLINK_RET_TRANSFER_ERROR`；需要"先发后等"语义请用 `vlink_publish_by_force`，但请注意它不会"回放"给后到的订阅者。
5. **多线程销毁**：禁止在另一线程 `vlink_publish` 的同时 `vlink_destroy_publisher`，需自行序列化。

## 设计要点

- **Schema 元信息一次表达**：`vlink_schema_info_t` 同时填 `ser` 和 `schema`，避免 `set_ser_type` 多次调用导致 wire 元信息漂移。
- **Push 模型回调**：订阅端只暴露回调注册一种方式，避免 C 端被迫维护轮询线程；如确实需要可在回调内入队后再让别处消费。
- **handle 句柄值传**：所有 handle 是按值传递的小结构体；creator 写入内部 native 指针，destroyer 通过 `&handle` 接收并清零，避免悬空指针。
- **连接状态可观测**：`vlink_detect_subscribers` 允许 C 端拿到 connect/disconnect 事件，无需借助 ProxyServer。

## 配图

![C API PubSub 流程](./images/c-api-pubsub-flow.png)

图示了 `vlink_create_subscriber` 注册回调、`vlink_create_publisher` 创建发布端、`vlink_wait_for_subscribers` 匹配后由 `vlink_publish` 触发 `on_message` 的完整时序。

## 参考

- `../c_rpc/` — Method 模型 C 实现，注意 `vlink_reply` 的回调内约束
- `../c_field/` — Field 模型 C 实现，含 Poll 与 Push 两种 Getter
- `../c_security/` — 在 PubSub 基础上叠加 AES-128-GCM 加密
- `vlink/include/vlink/external/c_api.h` — 唯一头文件，签名与返回码均见此
- 顶层 `doc/18-c-api.md` — C API 参考手册
