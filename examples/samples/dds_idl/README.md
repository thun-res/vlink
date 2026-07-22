# 🧱 dds_idl —— FastDDS 原生 IDL 类型互操作示例

演示 VLink 与 FastDDS 原生 IDL 类型的互操作：由 IDL 编译器 `fastddsgen` 从 `.idl` 文件生成 C++ 类型（`Request` / `Response` / `Message`），直接用作 VLink `Publisher` / `Subscriber` / `Server` / `Client` 的消息类型。框架自动识别 IDL 生成类型并走 CDR 序列化路径。

> ⚠️ 本示例默认不构建。`examples/samples/CMakeLists.txt` 中 `# add_subdirectory(dds_idl)` 处于注释状态，因为需要本地安装 FastDDS 与 `fastddsgen` 工具链。

通信内容：

- Method：`Client.invoke(Request{type:100})` → Server 返回 `Response{value:"AA"}`；`type:200` → `"BB"`
- Event：`Publisher.publish(Message{value:"hello"})` → Subscriber 收到后退出

## 🔧 启用构建

```bash
fastddsgen --version                              # 确认工具链已安装

# 取消 examples/samples/CMakeLists.txt 中该行的注释：
#   # add_subdirectory(dds_idl)  →  add_subdirectory(dds_idl)

cmake -B build -S . -DENABLE_EXAMPLES=ON -DSKIP_DDS=OFF
cmake --build build --target sample_dds_idl
./build/output/bin/sample_dds_idl
```

## 🧩 关键点

1. URL 注册：IDL 类型须在 `init()` 前用 `DdsConf::register_url<PubSubType>(...)` 把 URL 与 FastDDS 的 `TypeSupport` 绑定。

   ```cpp
   vlink::DdsConf::register_url<dds::MessagePubSubType>("dds://hello/event");
   vlink::DdsConf::register_url<dds::RequestPubSubType, dds::ResponsePubSubType>("dds://hello/method");
   ```

2. CDR 序列化：框架自动识别 IDL 生成类型并走 CDR 路径，无需用户手写序列化器。
3. 安全限制：CDR 类型不支持 `SecurityPublisher`；`enable_security()` 会告警并返回 `false`，默认安全节点随后因没有可用安全上下文而拒绝初始化，不会降级为明文。需消息级加密请改用受支持的非 CDR 类型，或使用 DDS Security / TLS。

## 📊 与其他 sample 的区别

| sample | 序列化 | 类型来源 | 工具链依赖 |
|--------|--------|----------|------------|
| `helloworld` | Protobuf | `.proto` + `protoc` | Protobuf |
| `dds_idl` | CDR | `.idl` + `fastddsgen` | FastDDS |
| `ddsc_proto` | Protobuf | `.proto` + CycloneDDS | Protobuf + CycloneDDS |
| `dds_dynamic` | DynamicData | 运行时类型擦除 | FastDDS |

## 📦 文件

| 文件 | 说明 |
|------|------|
| `dds_idl.cc` | 主示例源码 |
| `dds_idl.idl` | OMG IDL 类型定义（Request / Response / Message） |
| `CMakeLists.txt` | 构建配置，含 `fastddsgen` 调用生成 `dds_idlPubSubTypes.h` 等头 |

## 🔗 参考

- 消息序列化与 CDR 路径：[doc/03-serialization.md](../../../doc/03-serialization.md)
- `dds://`（FastDDS）配置：[doc/04-transport.md](../../../doc/04-transport.md)
- samples 索引：[samples/README.md](../README.md)
- `include/vlink/modules/dds_conf.h` —— `DdsConf::register_url` 完整签名
