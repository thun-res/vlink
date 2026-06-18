# shm_raw -- VLink 共享内存原始字节通信示例

## 1. 概述

本示例演示如何使用 VLink 的共享内存（`shm://`）传输协议进行原始字节通信。这是唯一一个在单进程内完整展示 VLink 全部六种通信原语的示例，并且使用了带安全密钥的 Security 变体。

## 2. 文件说明

| 文件 | 说明 |
|------|------|
| `shm_raw.cc` | 主程序，演示全部六种通信原语 |
| `CMakeLists.txt` | 构建配置，链接 `vlink::shm` |

## 3. 演示内容

### 3.1 方法模型 (Method) -- RPC 请求-响应

```
SecurityClient  -->  SecurityServer
  发送 {0x1, 0x2, 0x3}
  接收 1MB 响应（首字节=0xA，末字节=0xB）
```

- 使用 `SecurityServer<Bytes, Bytes>` 和 `SecurityClient<Bytes, Bytes>`
- 服务端检查请求内容，返回 1MB 响应数据
- 客户端使用同步 `invoke()` 调用

### 3.2 事件模型 (Event) -- 发布/订阅

```
SecurityPublisher  -->  SecuritySubscriber
  发送 "hello1", "hello2", "hello3"
```

- 使用 `SecurityPublisher<Bytes>` 和 `SecuritySubscriber<Bytes>`
- 构造一个 `Security::Config`（`cfg.key = "custom-key"`），作为 `SecurityPublisher` / `SecuritySubscriber` 构造函数的第二参数传入；`key` 经 SHA-256 截断为 AES-128-GCM key
- 发布者调用 `wait_for_subscribers()` 等待匹配后再发送

### 3.3 字段模型 (Field) -- 状态读写

```
SecuritySetter  -->  SecurityGetter
  设置 {0xA, 0xB, 0xC}
  读取最新值
```

- 使用 `SecuritySetter<Bytes>` 和 `SecurityGetter<Bytes>`
- Setter 设置字段值，Getter 轮询获取最新值
- `getter.get()` 返回 `std::optional`，无值时为 `std::nullopt`

## 4. Security 变体

本示例使用的 `SecurityPublisher`、`SecuritySubscriber` 等是带安全密钥验证的通信原语。只有安全密钥匹配的端点才能通信，增强了进程间通信的安全性。

## 5. 依赖

- VLink 库（`vlink::shm` 组件）

## 6. 构建与运行

```bash
# 构建
cmake -B build -S . -DCMAKE_PREFIX_PATH=<vlink安装路径>
cmake --build build

# 前置：先启动 Iceoryx RouDi 守护进程
iox-roudi &

# 运行
./build/output/bin/sample_shm_raw
```

程序在单进程内完成所有通信，无需启动多个终端，但需要 `iox-roudi` 守护进程已经在后台运行。
