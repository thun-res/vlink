# ddsc_proto -- VLink DDS-C 协议 + Protobuf 示例

## 1. 概述

本示例演示如何使用 VLink 的 DDS-C (`ddsc://`) 传输协议搭配 Protobuf 序列化进行通信。展示了方法模型（RPC）和事件模型（发布/订阅）的典型用法。

## 2. 文件说明

| 文件 | 说明 |
|------|------|
| `ddsc_proto.cc` | 主程序，演示 RPC 和事件通信 |
| `ddsc_proto.proto` | Protobuf 消息定义（Request、Response、Message） |
| `CMakeLists.txt` | 构建配置，链接 `vlink::ddsc` |

## 3. 演示内容

### 3.1 方法模型 -- RPC 调用

模拟电话服务场景：

- 客户端发送 `Request`（type = 10086）
- 服务端识别后返回 `Response`（value = "calling..."）
- 使用 `client.detect_connected()` 监控服务端连接状态

```cpp
Server<pb::Request, pb::Response> server("ddsc://phone/method");
Client<pb::Request, pb::Response> client("ddsc://phone/method");
auto resp = client.invoke(req);
```

### 3.2 事件模型 -- 时间戳发布

模拟时间播报场景：

- 发布者每秒发布一条时间消息（"00:00"、"00:01" ...）
- 订阅者接收并打印时间戳
- 使用 `pub.wait_for_subscribers()` 确保订阅者就绪

```cpp
Publisher<pb::Message> pub("ddsc://phone/event");
Subscriber<pb::Message> sub("ddsc://phone/event");
```

## 4. 依赖

- VLink 库（`vlink::ddsc` 组件）
- Protobuf

## 5. 构建与运行

```bash
# 构建
cmake -B build -S . -DCMAKE_PREFIX_PATH=<vlink安装路径>
cmake --build build

# 运行（单进程内完成）
./build/output/bin/sample_ddsc_proto
```
