# fdbus_proto -- VLink FDBus 协议 + Protobuf 示例

## 1. 概述

本示例演示如何使用 VLink 的 FDBus (`fdbus://`) 传输协议搭配 Protobuf 序列化进行通信。完整展示了 VLink 的三种通信模型：方法模型（RPC）、事件模型（发布/订阅）和字段模型（状态读写）。

## 2. 文件说明

| 文件 | 说明 |
|------|------|
| `fdbus_proto.cc` | 主程序，演示全部三种通信模型 |
| `fdbus_proto.proto` | Protobuf 消息定义（Request、Response、Message） |
| `CMakeLists.txt` | 构建配置，链接 `vlink::fdbus` |

## 3. 演示内容

### 3.1 方法模型 -- RPC 调用

模拟电话服务：
- 客户端发送请求（type = 10086）
- 服务端返回响应（value = "calling..."）
- URL 格式使用 `?event=` 参数指定事件标识

```cpp
Server<pb::Request, pb::Response> server("fdbus://phone?event=req");
Client<pb::Request, pb::Response> client("fdbus://phone?event=req");
```

### 3.2 事件模型 -- 时间戳广播

每秒发布一条时间消息：
- 发布者在 `wait_for_subscribers()` 后连续发布 "00:00" 到 "00:03"
- 订阅者实时接收并打印

```cpp
Publisher<pb::Message> pub("fdbus://phone?event=time");
Subscriber<pb::Message> sub("fdbus://phone?event=time");
```

### 3.3 字段模型 -- 电话号码存取

演示 Setter/Getter 字段读写：
- Setter 设置电话号码字段值为 "119"
- Getter 等待片刻后读取最新值

```cpp
Setter<pb::Message> setter("fdbus://phone?event=msg");
Getter<pb::Message> getter("fdbus://phone?event=msg");
auto ret = getter.get();  // 返回 std::optional
```

## 4. FDBus URL 格式

FDBus 使用查询参数区分同一服务下的不同话题：

```
fdbus://服务名?event=话题名
```

例如 `fdbus://phone?event=req` 和 `fdbus://phone?event=time` 是同一 FDBus 服务下的不同话题。

## 5. 依赖

- VLink 库（`vlink::fdbus` 组件）
- Protobuf
- FDBus

## 6. 构建与运行

```bash
# 构建
cmake -B build -S . -DCMAKE_PREFIX_PATH=<vlink安装路径>
cmake --build build

# 运行（单进程内完成）
./build/output/bin/sample_fdbus_proto
```
