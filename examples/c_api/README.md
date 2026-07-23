# 🔌 c_api/ — VLink 纯 C 绑定示例

本目录收录"在 C 语言中调用 VLink"的示例。VLink 内核支持 C++17/C++20，外层通过 `vlink/external/c_api.h` 暴露一套无异常、无 RTTI、以 ABI 稳定句柄按值传递的 C 接口，适用于嵌入式 C 工程、Rust/Go/Lua 等语言的 FFI 包装层，以及不引入 C++ 运行时的旧代码迁移。

本类目当前提供一个示例 `c_pubsub/`，演示事件模型的发布/订阅。Method、Field、安全等其余 C 接口的完整签名与用法见 `doc/13-integration.md`（C API 参考手册）。

## 📁 子示例索引

| 示例 | 主题 | 通信模型 | 关键 API 前缀 |
|------|------|---------|--------------|
| [`c_pubsub/`](c_pubsub/) | 事件发布/订阅 | Event | `vlink_create_publisher` / `vlink_create_subscriber` |

## 🧭 共同前置知识

- **Schema 元信息**：创建函数的 `vlink_schema_info_t*` 参数可传 `NULL`；若提供，则 `ser` 与 `schema` 必须成对有效。它与 C++ 侧的 `Node::set_ser_type()` 对应，并传播至 Discovery、ProxyServer、BagWriter、Viewer；错填会导致对端无法正确识别载荷。DDS CDR 使用 `VLINK_SCHEMA_CDR`，传入的原始 payload 必须包含 4 字节 encapsulation header。
- **返回码语义**：唯一成功值为 `VLINK_RET_NO_ERROR (0)`；正值表示业务态（条件未满足、参数错、内存不足、运行时异常、传输失败），负值（`VLINK_RET_UNKNOWN_ERROR = -1`）表示未分类的内部错误。
- **句柄生命周期**：所有 `vlink_*_handle_t` 必须显式调用 `vlink_destroy_*(&handle)` 析构；handle 为值类型，传指针是为了让 destroy 函数清理内部 native pointer。
- **回调缓冲生命**：`on_message` / `on_request` / `on_change` 回调中的 `data/size` 仅在回调返回前有效，需保留时自行拷贝。
- **线程安全**：单个 handle 的 `publish/invoke/set/get` 调用可在多线程发起，但 `create/destroy` 不可与其它操作并发。

## 🔗 链接配置

CMake 中链接 `vlink::c_api`（而非 `vlink::vlink`），子项目声明使用 `LANGUAGES C`：

```cmake
project(my_c_app LANGUAGES C)
find_package(vlink REQUIRED)
add_executable(my_c_app main.c)
target_link_libraries(my_c_app PRIVATE vlink::c_api)
```

`vlink::c_api` 在 vlink 静态库之上再包一层稳定 C 符号。运行时不需显式链接 C++ 运行时，但 ELF/Mach-O 仍动态依赖 `libstdc++` / `libc++`。

## 🪜 与 C++ API 的对照

| C 句柄 | C++ 类 | 头文件 |
|--------|--------|--------|
| `vlink_publisher_handle_t` | `vlink::Publisher<T>` | `include/vlink/publisher.h` |
| `vlink_subscriber_handle_t` | `vlink::Subscriber<T>` | `include/vlink/subscriber.h` |
| `vlink_server_handle_t` | `vlink::Server<Req,Resp>` | `include/vlink/server.h` |
| `vlink_client_handle_t` | `vlink::Client<Req,Resp>` | `include/vlink/client.h` |
| `vlink_setter_handle_t` | `vlink::Setter<T>` | `include/vlink/setter.h` |
| `vlink_getter_handle_t` | `vlink::Getter<T>` | `include/vlink/getter.h` |
| `vlink_security_handle_t` | `vlink::Security` | `include/vlink/extension/security.h` |

跨语言互操作（C / C++ / Python）除 URL、`ser_type`、`schema_type` 一致外，C 侧传入的原始 payload 还必须符合对端消息类型的 wire 编码与 schema；C API 不会替调用方序列化结构体。

## 📚 参考

- `doc/13-integration.md` — C API 参考手册（Event / Method / Field / 安全的完整 C 接口）
- `include/vlink/external/c_api.h` — 唯一头文件
- `python_api/vlink_python.cc` — 同类 FFI 适配实现，可作对照
