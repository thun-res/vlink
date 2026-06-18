# someip_flat -- VLink SOME/IP 协议 + FlatBuffers 示例

## 1. 概述

本示例演示如何使用 VLink 的 SOME/IP (`someip://`) 传输协议搭配 FlatBuffers 序列化进行通信。完整展示了三种通信模型：方法模型（RPC）、事件模型（发布/订阅）和字段模型（状态读写）。

SOME/IP 是汽车行业标准的服务发现和通信协议，FlatBuffers 是高效的零拷贝序列化格式。

## 2. 文件说明

| 文件 | 说明 |
|------|------|
| `someip_flat.cc` | 主程序，演示全部三种通信模型 |
| `someip_flat.fbs` | FlatBuffers schema 定义（Request、Message） |
| `CMakeLists.txt` | 构建配置，链接 `vlink::someip` |

## 3. 演示内容

### 3.1 方法模型 -- RPC 单向请求

- 客户端使用 `client.send(req)` 发送单向请求
- 服务端接收并打印请求内容
- 使用 `client.wait_for_connected()` 等待服务端就绪
- 注意：使用 `fbs::RequestT`（可变类型）发送，`fbs::Request*`（只读指针）接收

```cpp
Server<fbs::Request*> server("someip://0x1/0x2?method=0x3");
Client<fbs::RequestT> client("someip://0x1/0x2?method=0x3");
```

### 3.2 事件模型 -- 消息广播

- 发布者定期发布带类型和值的消息
- 订阅者接收并打印消息内容
- 使用事件组（groups）和事件 ID

```cpp
Publisher<fbs::MessageT> pub("someip://0x1/0x3?groups=0x1|0x2&event=0x3");
Subscriber<fbs::Message*> sub("someip://0x1/0x3?groups=0x1|0x2&event=0x3");
```

### 3.3 字段模型 -- 状态读写

- Setter 设置字段值
- Getter 读取最新字段值
- 使用 `field=1` 参数标识字段模式

```cpp
Setter<fbs::MessageT> setter("someip://0x1/0x4?groups=0x1|0x2&event=0x4&field=1");
Getter<fbs::MessageT> getter("someip://0x1/0x4?groups=0x1|0x2&event=0x4&field=1");
```

## 4. SOME/IP URL 格式

SOME/IP URL 使用十六进制 ID 标识服务和方法：

```
someip://服务ID/实例ID?method=方法ID                          -- RPC 方法
someip://服务ID/实例ID?groups=事件组ID&event=事件ID           -- 事件
someip://服务ID/实例ID?groups=事件组ID&event=事件ID&field=1   -- 字段
```

多个事件组用竖线（`|`）分隔：`groups=0x1|0x2`

## 5. FlatBuffers 类型约定

| 类型后缀 | 说明 | 用途 |
|----------|------|------|
| `fbs::MessageT` | Object API（可变） | 发送数据、本地修改 |
| `fbs::Message*` | 只读指针（零拷贝） | 接收数据、只读访问 |

## 6. 依赖

- VLink 库（`vlink::someip` 组件）
- FlatBuffers
- vsomeip（SOME/IP 实现）

## 7. 构建与运行

```bash
# 构建
cmake -B build -S . -DCMAKE_PREFIX_PATH=<vlink安装路径>
cmake --build build

# 运行（单进程内完成，但需要 vsomeip 守护进程）
./build/output/bin/sample_someip_flat
```

## 8. 注意事项

- SOME/IP 通信通常需要 vsomeip 路由管理器（守护进程）运行
- 如果没有 vsomeip 环境，程序可能会阻塞等待连接
