# ping_pong — Round-trip 延迟测量

经典的 ping/pong 模式：Ping 端记录时间戳、发送、收到 echo 时算 RTT；Pong 端原样 echo 回去。用原始 `Bytes` 类型（无序列化开销）以最大化延迟测量精度。

读完本示例你能掌握：

- 跨进程延迟测量的标准代码模式（`steady_clock` + `std::atomic`）。
- Pong 端"verbatim echo"的两行实现。
- 在 shm 后端下零拷贝 echo 的工程意义。
- 通过环境变量切换传输协议对比延迟。

## 文件结构

```
ping_pong/
  ping_pong_common.h     # URL 辅助（thin wrapper over ../common_transport.h）
  ping/ping.cc           # 发送端，测量
  pong/pong.cc           # 接收端，echo
```

## 运行

```bash
# 终端 1（默认 1 KiB payload）
./build/output/bin/sample_pong

# 终端 2
./build/output/bin/sample_ping              # 默认 1 KiB
./build/output/bin/sample_ping 65536        # 自定义 payload 大小（字节）
```

## 切换传输协议

| 环境变量 | 取值 |
|---------|------|
| `PING_TRANSPORT` / `PONG_TRANSPORT` | `dds` / `ddsc` / `shm` / `someip` / `fdbus` / `qnx` |
| `PING_URL` / `PONG_URL` | 完整 URL（覆盖 TRANSPORT） |

示例：

```bash
# 共享内存（需要 iox-roudi）
iox-roudi &
PING_TRANSPORT=shm PONG_TRANSPORT=shm ./sample_pong &
PING_TRANSPORT=shm PONG_TRANSPORT=shm ./sample_ping 4096
```

## 核心代码

### Ping 端（发送 + 测量）

```cpp
Publisher<Bytes>  pub(Common::get_ping_url());
Subscriber<Bytes> sub(Common::get_pong_url());

std::atomic<std::chrono::steady_clock::time_point> t0 = std::chrono::steady_clock::now();

sub.listen([&t0](const Bytes&) {
  auto rt_us = std::chrono::duration_cast<std::chrono::microseconds>(
                   std::chrono::steady_clock::now() - t0.load()).count();
  CLOG_D("Delay(ms) = %.3lf.", rt_us / 2000.0);
});

Bytes data = Bytes::create(test_size);     // 预分配，复用
Timer t;
t.attach(&loop);
t.set_interval(1000);
t.set_loop_count(Timer::kInfinite);
t.start([&]{
  t0 = std::chrono::steady_clock::now();
  pub.publish(data);
});
```

要点：

- `std::chrono::steady_clock`（monotonic）：不受 NTP 影响，跨调用差值可靠。
- `std::atomic<time_point>`：Timer 写 / Subscriber 读跨线程，atomic 保证 happens-before。
- `Bytes::create(test_size)` 一次性预分配：把"测量代码本身的分配抖动"排除在延迟数字外。
- 单向延迟 = RTT / 2（假定 ping 到 pong 与 pong 到 ping 对称）。

### Pong 端（echo）

```cpp
Subscriber<Bytes> sub(Common::get_ping_url());
Publisher<Bytes>  pub(Common::get_pong_url());

sub.listen([&pub](const Bytes& data) {
  pub.publish(data);    // verbatim echo
});
```

在 `shm://` 下，`data` 是 SHM 的零拷贝视图；`pub.publish(data)` 不复制内存。整个 pong 路径只增加几微秒回调开销。

## 参考延迟（数量级，硬件相关）

| 后端 | 1 KiB | 1 MiB |
|------|-------|-------|
| `shm://` | < 0.05 ms | < 0.1 ms（零拷贝） |
| `dds://` | 0.1–0.5 ms | 1–5 ms |
| `ddsc://` | 0.1–0.3 ms | — |
| `someip://` | 0.3–1 ms | — |
| `fdbus://` | 0.2–0.8 ms | — |

实际值受 CPU 主频、网络、系统负载、QoS 配置影响。

## 配图

![Ping Pong sequence](./images/ping-pong-rpc-sequence.png)

图中展示 Ping → Pong → Ping 的完整时序，含时间戳记录点。

## 常见陷阱

1. **Pong 先启动**：必须 pong 先 ready 否则前几条 ping 丢失。
2. **`std::chrono::system_clock` 而非 steady_clock**：NTP 跳变会让测量出现负值或巨大跳变。
3. **每次 publish 都 new Bytes**：分配开销混进延迟测量；预分配一次复用。
4. **payload 太小**：网络栈最小 MTU 会增加固定开销；< 64B 测出来意义不大。
5. **不等 wait_for_subscribers**：示例靠 Subscriber 先启 + Pong 先等；生产代码 ping 端要先 `wait_for_subscribers`。

## 设计要点

- 一对 Publisher + 一对 Subscriber 即可建一对单向通道；vlink 不内置 reply 路由。
- shm 后端的零拷贝是 ping_pong 测量到极限延迟的关键。
- ping 与 pong 拆分为两个独立可执行：演示真正的跨进程拓扑。

## 参考

- `../helloworld/` — Method + Event with Protobuf
- `../shm_raw/` — Bytes + Security 在 shm 上
- `../common_transport.h` — 共享 URL 切换 helper
- 顶层 `doc/22-examples.md` — samples 章节
