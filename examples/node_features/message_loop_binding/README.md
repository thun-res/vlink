# message_loop_binding — `attach(&loop)` 把节点回调绑定到指定 MessageLoop

`Node::attach(MessageLoop*)` 让节点的回调（Subscriber listen、Server listen、Getter listen 等）跑在指定 loop 的线程上。这是 vlink 控制回调线程、做无锁串行处理的核心机制。

读完本示例你能掌握：

- attach 的 4 种典型组合（不绑、单 loop、共享 loop、独立 loop）。
- 何时回调跑在 transport 线程、何时跑在 loop 线程。
- 多节点 attach 同一 loop 时回调天然串行的工程意义。
- Publisher / Server / Setter 也可以 attach 控制 connect/match 等回调线程。

## 背景与适用场景

适用：

- 几乎所有"业务回调"场景：你想知道、想控制"回调在哪个线程跑"。
- 多个 Subscriber 共享同一 loop → 回调串行，业务代码无需加锁。
- 分散到多个 loop → 并行处理。

不适合：

- 完全独立的传输内部回调（vlink 不允许直接控制内部线程）。
- 对线程切换零开销有要求的场景（attach 增加一次 MessageLoop 调度）。

## attach 4 种组合

| 模式 | 代码 | 回调线程 | 并发 |
|------|------|---------|------|
| 不绑 | 不调 `attach()` | transport 内部 | 不可控 |
| 单 loop | `sub.attach(&loop)` | loop 线程 | 串行 |
| 共享 loop | 多个节点 attach 同 loop | 同 loop 线程 | 串行（无锁安全） |
| 独立 loop | 节点 attach 不同 loop | 各自 loop 线程 | 并行 |

## 核心 API

| API | 签名 | 说明 |
|-----|------|------|
| `Node::attach` | `void attach(MessageLoop* loop)` | 绑定到指定 loop |
| `Node::detach` | `void detach()` | 解绑 |
| `Subscriber::listen` | `bool listen(MsgCallback&&)` | **必须先 attach 再 listen** |
| `Server::listen` | 同上 | 同 |
| `Getter::listen` | 同上 | 同 |
| `MessageLoop::wait_for_idle` | `bool wait_for_idle(uint32_t ms)` | 等回调队列空 |

## 代码导读

### 1. 单 loop

```cpp
vlink::MessageLoop loop;
loop.set_name("worker");
loop.async_run();

vlink::Subscriber<std::string> sub("dds://topic");
sub.attach(&loop);
sub.listen([](const std::string& msg) {
  VLOG_I("got: ", msg);   // 在 worker 线程跑
});

// shutdown
loop.quit();
loop.wait_for_quit();
```

### 2. 共享 loop（无锁串行）

```cpp
vlink::MessageLoop loop;
loop.async_run();

vlink::Subscriber<int> sub_a("dds://topic_a");
sub_a.attach(&loop);
sub_a.listen([](int v) { /* 跑在 loop 线程 */ });

vlink::Subscriber<int> sub_b("dds://topic_b");
sub_b.attach(&loop);
sub_b.listen([](int v) { /* 同样跑在 loop 线程；与 sub_a 回调串行 */ });
```

两个订阅者共享一个 loop，回调天然串行；共享可变状态无需加锁。

### 3. 独立 loop（并行）

```cpp
vlink::MessageLoop loop_a, loop_b;
loop_a.async_run();
loop_b.async_run();

vlink::Subscriber<int> sub_a("dds://topic_a");
sub_a.attach(&loop_a);
sub_a.listen([](int v) { /* loop_a 线程 */ });

vlink::Subscriber<int> sub_b("dds://topic_b");
sub_b.attach(&loop_b);
sub_b.listen([](int v) { /* loop_b 线程，与 sub_a 并行 */ });
```

CPU 密集型业务 fan-out 到多 loop 提高吞吐。

### 4. Publisher / Server / Setter 的 attach

```cpp
vlink::Publisher<std::string> pub("dds://topic");
pub.attach(&loop);
pub.detect_subscribers([](bool has) { /* 跑在 loop 线程 */ });

vlink::Server<Req, Resp> srv(url);
srv.attach(&loop);
srv.listen([](const Req& req, Resp& resp) { /* loop 线程 */ });
```

Publisher 的 `detect_subscribers` 回调、Server 的 `listen` 回调等都按 attach 决定线程。

## 运行

```bash
./build/output/bin/example_message_loop_binding
```

预期输出（节选）：

```
Unbound mode: callback thread is transport-internal
Single loop: callback on worker thread
Shared loop: sub_a and sub_b serialize on same thread
Independent loops: sub_a in loop_a, sub_b in loop_b
```

## 常见陷阱

1. **attach 在 listen 之后**：listen 已注册回调到旧的 dispatcher；attach 不生效。**必须先 attach 再 listen**。
2. **回调里阻塞**：阻塞 loop 线程，所有共享该 loop 的回调都被推迟。
3. **多线程并发 attach / detach**：vlink 不保证；按 single-owner 调用。
4. **loop 没 run**：attach 后 loop 没 async_run / run，回调永远不触发。
5. **loop 早于 node 析构**：node 析构时尝试 detach 已死 loop → UB。生命周期上 loop 必须比所有 attach 它的 node 晚析构。

## 设计要点

- attach 内部只是注册一个 `loop` 指针；listen 时把回调包装成 task post 到 loop。
- detach 在 deinit / 析构时被自动调用；不需要业务显式调（除非显式重 attach）。
- 一个节点一个时刻只能 attach 到一个 loop；要换 loop 先 detach。

## 配图

![Node loop binding](./images/node-loop-binding.png)

图中展示 4 种 attach 组合下的线程拓扑。

## 参考

- `../../base/message_loop_basic/` — MessageLoop 入门
- `../lifecycle/` — 节点生命周期
- 顶层 `doc/03-message-loop.md` — MessageLoop 章节
- `vlink/include/vlink/node.h` — Node::attach 接口
