# 🧩 dds_dynamic —— DDS 动态数据类型示例

用 `dds://`（FastDDS 后端）演示 `DynamicData` 运行时类型擦除载荷容器。`DynamicData` 允许在同一话题上传输不同类型的消息，由类型标签区分，无需为每种消息单独建话题。本示例覆盖 Method（多类型 RPC）与 Event（多类型消息）两种通信模型。

## 🧩 DynamicData 核心 API

| API | 用途 |
|-----|------|
| `DynamicData().load(type, data)` | 序列化并写入类型标签 |
| `dd.get_type()` | 读取类型标签字符串 |
| `dd.as<T>()` | 将内部数据反序列化为指定类型 `T` |

载荷以 `Server<DynamicData, DynamicData>` / `Client<DynamicData, DynamicData>`、`Publisher<DynamicData>` / `Subscriber<DynamicData>` 作为消息类型；具体 `T` 由 `load`/`as` 在运行时决定。

## ⚡ 最小片段

Method —— 服务端按类型标签分发，应答类型可逐请求不同：

```cpp
vlink::Server<vlink::DynamicData, vlink::DynamicData> server("dds://dynamic/method");
server.listen([](const vlink::DynamicData& req, vlink::DynamicData& res) {
  if (req.get_type() == "type1") {
    res = vlink::DynamicData().load("type1", std::string("I love you"));
  } else if (req.get_type() == "type2") {
    res = vlink::DynamicData().load("type2", 1314);
  }
});

vlink::Client<vlink::DynamicData, vlink::DynamicData> client("dds://dynamic/method");
auto resp = client.invoke(vlink::DynamicData().load("type1", req1));  // std::optional
if (resp) {
  VLOG_I("resp1:", resp->as<std::string>());
}
```

Event —— 同一话题上发布异构消息，订阅端按标签分发：

```cpp
vlink::Publisher<vlink::DynamicData> pub("dds://dynamic/event");
pub.publish(vlink::DynamicData().load("Request", req));
pub.publish(vlink::DynamicData().load("Response", resp));

vlink::Subscriber<vlink::DynamicData> sub("dds://dynamic/event");
sub.listen([](const vlink::DynamicData& msg) {
  if (msg.get_type() == "Request") {
    VLOG_I("msg1:", msg.as<pb::Request>().type());
  } else if (msg.get_type() == "Response") {
    VLOG_I("msg2:", msg.as<pb::Response>().value());
  }
});
```

## 📦 文件

| 文件 | 说明 |
|------|------|
| `dds_dynamic.cc` | 主程序，演示 DynamicData 的 RPC 与事件通信 |
| `dds_dynamic.proto` | Protobuf 消息定义 |
| `CMakeLists.txt` | 构建配置，链接 `vlink::dds` |

## 🚀 构建与运行

```bash
cmake -B build -S . -DCMAKE_PREFIX_PATH=<vlink安装路径>
cmake --build build
./build/output/bin/sample_dds_dynamic
```

依赖：`vlink::dds`（FastDDS 后端）、Protobuf。

## 🔗 参考

- 消息序列化与类型识别：[doc/03-serialization.md](../../../doc/03-serialization.md)
- 传输后端与 URL：[doc/04-transport.md](../../../doc/04-transport.md)
- samples 索引：[samples/README.md](../README.md)
