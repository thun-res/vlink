# c_api/ — VLink 纯 C 绑定示例集

本目录收录所有"在 C 语言里调用 VLink"的示例。VLink 内部是 C++17/20 模板库，外层通过 `vlink/external/c_api.h` 暴露出一套无异常、无 RTTI、按 ABI 稳定句柄传值的 C 接口，适用于嵌入式 C 工程、Rust/Go/Lua/其他语言的 FFI 包装层，以及不愿引入 C++ 运行时的旧代码迁移。

四个子示例覆盖三种通信模型，外加一份独立的应用层安全示例，便于按需挑选阅读起点。

## 子示例索引

| 示例 | 主题 | 通信模型 | 关键 API 前缀 |
|------|------|---------|--------------|
| [`c_pubsub/`](c_pubsub/) | 事件订阅/发布 | Event | `vlink_create_publisher` / `vlink_create_subscriber` |
| [`c_rpc/`](c_rpc/) | RPC 请求/响应 | Method | `vlink_create_server` / `vlink_create_client` |
| [`c_field/`](c_field/) | 状态字段读写（含轮询模式） | Field | `vlink_create_setter` / `vlink_create_getter` |
| [`c_security/`](c_security/) | 独立加解密 + 安全节点构造 | 跨三种模型 | `vlink_security_create` / `vlink_create_secure_*` |

四个示例使用同一套返回码枚举（`VLINK_RET_*`）和 Schema 描述结构（`vlink_schema_info_t`）。建议先读 `c_pubsub/`，确认链接、句柄、回调三件事都跑通；其余三个示例都是在它之上的延伸。

## 推荐阅读顺序

1. **`c_pubsub/`** — 起步示例，确认 `vlink_create_publisher` / `vlink_create_subscriber` / `vlink_publish` 三件套以及回调签名。
2. **`c_rpc/`** — 在 PubSub 基础上理解 `vlink_reply()` 必须在回调内调用的约束。
3. **`c_field/`** — Push（回调）+ Poll（`vlink_get` 输出缓冲）两种使用方式，注意 `VLINK_RET_MEMORY_ERROR` 时缓冲扩容协议。
4. **`c_security/`** — 独立加解密 + 六种 `vlink_create_secure_*` 节点构造，含 PBKDF2 / RSA / AAD 上下文等高级配置。

## 共同前置知识

- **Schema 元信息**：所有创建函数都要求传入 `vlink_schema_info_t { const char* ser; vlink_schema_type_t schema; }`，它和 C++ 侧的 `Node::set_ser_type()` 一一对应，会传播给 Discovery、ProxyServer、BagWriter、Viewer。**这不是提示，是真实的 wire 元数据**：错填会导致跨语言/跨进程对端无法识别。
- **返回码语义**：唯一成功值是 `VLINK_RET_NO_ERROR (0)`；正值表示业务态（条件未满足、参数错、内存不足、运行时异常、传输失败），负值（`VLINK_RET_UNKNOWN_ERROR = -1`）表示未分类的内部错误。
- **句柄生命周期**：所有 `vlink_*_handle_t` 的析构必须显式调用 `vlink_destroy_*(&handle)`；handle 是值类型，传指针只是为了让 destroy 函数能清掉内部 native pointer。
- **回调缓冲生命**：`on_message` / `on_request` / `on_change` 回调里的 `data/size` 仅在回调返回前有效。需要保留必须自行拷贝。
- **线程安全**：单个 handle 的 `publish/invoke/set/get` 调用可在多线程发起，但 `create/destroy` 不可与其它操作并发。

## 链接

CMake 中链接 `vlink::c_api`（不是 `vlink::vlink`），且子项目语句使用 `LANGUAGES C`：

```cmake
project(my_c_app LANGUAGES C)
find_package(vlink REQUIRED)
add_executable(my_c_app main.c)
target_link_libraries(my_c_app PRIVATE vlink::c_api)
```

`vlink::c_api` 是把整个 vlink 静态库再包一层稳定 C 符号的 shim 目标。运行时不需要 C++ 运行时显式链接，但 ELF/Mach-O 会动态依赖 `libstdc++` / `libc++`。

## 与 C++ API 的对照

| C 句柄 | C++ 类 | 头文件 |
|--------|--------|--------|
| `vlink_publisher_handle_t` | `vlink::Publisher<T>` | `include/vlink/publisher.h` |
| `vlink_subscriber_handle_t` | `vlink::Subscriber<T>` | `include/vlink/subscriber.h` |
| `vlink_server_handle_t` | `vlink::Server<Req,Resp>` | `include/vlink/server.h` |
| `vlink_client_handle_t` | `vlink::Client<Req,Resp>` | `include/vlink/client.h` |
| `vlink_setter_handle_t` | `vlink::Setter<T>` | `include/vlink/setter.h` |
| `vlink_getter_handle_t` | `vlink::Getter<T>` | `include/vlink/getter.h` |
| `vlink_security_handle_t` | `vlink::Security` | `include/vlink/extension/security.h` |

跨语言互操作（C / C++ / Python）只要 URL、`ser_type`、`schema_type` 三元组一致即可直接对接。

## 参考

- 顶层 `doc/18-c-api.md` — C API 参考手册
- `include/vlink/external/c_api.h` — 唯一头文件
- `python_api/vlink_python.cc` — 类似的 FFI 适配实现，可作为对照
