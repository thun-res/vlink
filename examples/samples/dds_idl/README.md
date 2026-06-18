# VLink Sample: dds_idl

## 1. 概述

演示 VLink 与 **FastDDS 原生 IDL** 类型的互操作——用 IDL 编译器(`fastddsgen`)从 `.idl` 文件生成 C++ 类型(`Request` / `Response` / `Message`),再直接用作 VLink `Publisher` / `Subscriber` / `Server` / `Client` 的消息类型。VLink 自动识别 IDL 生成类型走 CDR 序列化。

通信内容:

- **Method 模型**: `Client.invoke(Request{type:100})` → Server 返回 `Response{value:"AA"}`;`type:200` → `"BB"`
- **Event 模型**: `Publisher.publish(Message{value:"hello"})` → Subscriber 收到后设置退出标志

## 2. 文件说明

| 文件 | 说明 |
|------|------|
| `dds_idl.cc` | 主示例源码 |
| `dds_idl.idl` | OMG IDL 类型定义 (Request / Response / Message) |
| `CMakeLists.txt` | 构建配置,**包含 `fastddsgen` 调用生成 `dds_idlPubSubTypes.h` 等头** |

## 3. 启用构建

⚠️ **本示例默认不构建**——`examples/samples/CMakeLists.txt:21` 注释了 `# add_subdirectory(dds_idl)`,因为需要本地装好 FastDDS + IDL 工具链。

启用步骤:

```bash
# 1. 确保系统已装 FastDDS 与 fastddsgen
fastddsgen --version

# 2. 取消 examples/samples/CMakeLists.txt:21 行的注释
#    把  # add_subdirectory(dds_idl)
#    改成   add_subdirectory(dds_idl)

# 3. 构建
cmake -B build -S . -DENABLE_EXAMPLES=ON -DSELECT_DDS_BACKEND=fast-dds
cmake --build build --target sample_dds_idl
./build/output/bin/sample_dds_idl
```

## 4. 与其他 sample 的区别

| sample | 序列化 | 类型来源 | 工具链依赖 |
|--------|--------|----------|----------|
| `helloworld` | Protobuf | `.proto` 文件 + `protoc` | Protobuf |
| `dds_idl` | **CDR** | **`.idl` 文件 + `fastddsgen`** | **FastDDS** |
| `ddsc_proto` | Protobuf | `.proto` + CycloneDDS | Protobuf + CycloneDDS |
| `dds_dynamic` | DynamicData | 运行时类型擦除 | FastDDS |

## 5. 关键点

1. **URL 注册**: IDL 类型用 `DdsConf::register_url<PubSubType>(...)` 在 `init()` 前注册,把 URL 与 FastDDS 的 `TypeSupport` 绑定。

   ```cpp
   DdsConf::register_url<dds::MessagePubSubType>("dds://hello/event");
   DdsConf::register_url<dds::RequestPubSubType, dds::ResponsePubSubType>("dds://hello/method");
   ```

2. **CDR 序列化**: VLink 通过类型 `IsCdrType<T>` 自动识别 IDL 生成类型,走 CDR 路径(无需用户手写 serializer)。

3. **安全限制**: CDR 类型**不**支持 `SecurityPublisher`（构造时传入 `Security::Config` 会被忽略并打 warning）,如需加密请改用 Protobuf 或 FlatBuffers。

## 6. 相关文档

- [doc/06-serialization.md](../../../doc/06-serialization.md) — CDR 序列化路径
- [doc/07-transport.md](../../../doc/07-transport.md) — `dds://` (FastDDS) 配置
- [doc/22-examples.md](../../../doc/22-examples.md) — Samples 总览
- `include/vlink/modules/dds_conf.h` — `DdsConf::register_url` 完整签名
