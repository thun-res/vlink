# dds_dynamic -- VLink DDS 动态数据类型示例

## 1. 概述

本示例演示 VLink 的 `DynamicData` 动态数据类型系统。`DynamicData` 允许在同一个话题上传输不同类型的消息，通过类型标签（type string）区分，无需为每种消息类型创建独立的话题。

使用 DDS (`dds://`) 传输协议和 Protobuf 序列化。

## 2. 文件说明

| 文件 | 说明 |
|------|------|
| `dds_dynamic.cc` | 主程序，演示 DynamicData 的 RPC 和事件通信 |
| `dds_dynamic.proto` | Protobuf 消息定义 |
| `CMakeLists.txt` | 构建配置，链接 `vlink::dds` |

## 3. 演示内容

### 3.1 方法模型 -- 多类型 RPC

服务端根据请求的类型标签返回不同类型的响应：

- 类型标签 `"type1"`：请求为 `pb::Request`，响应为 `std::string`
- 类型标签 `"type2"`：请求为 `pb::Message`，响应为 `int`

```cpp
// 加载动态数据：DynamicData().load("类型标签", 数据)
auto resp = client.invoke(DynamicData().load("type1", req1));

// 提取特定类型：resp.as<std::string>()
```

### 3.2 事件模型 -- 多类型消息

在同一个话题上发布不同类型的消息：

```cpp
// 发布 Request 类型
pub.publish(DynamicData().load("Request", req));

// 发布 Response 类型
pub.publish(DynamicData().load("Response", resp));

// 订阅端根据类型标签分发
sub.listen([](const DynamicData& msg) {
    if (msg.get_type() == "Request") { ... }
    else if (msg.get_type() == "Response") { ... }
});
```

## 4. DynamicData 核心 API

| 方法 | 说明 |
|------|------|
| `load(type, data)` | 加载数据并设置类型标签 |
| `get_type()` | 获取类型标签字符串 |
| `as<T>()` | 将内部数据转换为指定类型 |

## 5. 依赖

- VLink 库（`vlink::dds` 组件）
- Protobuf
- FastDDS（`dds://` 后端的 DDS 实现）

## 6. 构建与运行

```bash
# 构建
cmake -B build -S . -DCMAKE_PREFIX_PATH=<vlink安装路径>
cmake --build build

# 运行（单进程内完成）
./build/output/bin/sample_dds_dynamic
```
