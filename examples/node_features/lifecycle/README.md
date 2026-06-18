# lifecycle — vlink 节点生命周期：延迟初始化、init/deinit、interrupt

vlink 节点（Publisher / Subscriber / Server / Client / Setter / Getter）默认在构造时就完成底层 transport 的创建（`InitType::kWithInit`）。但工程上常需要**先构造对象、配置 property、再 init**。本示例覆盖这套延迟初始化的全部 API。

读完本示例你能掌握：

- `kWithInit` vs `kWithoutInit` 两种构造模式。
- `init()` / `deinit()` 的手动控制。
- `interrupt()` 的语义（与 `deinit()` 的关键区别）。
- `has_inited()` 状态查询。
- 推荐的"构造 → set_property → init → 使用 → deinit"流程。

## 背景与适用场景

适用：

- 需要在 init 之前调 `set_property` 配置 QoS / SchemaType。
- 需要"先把节点交给某模块持有、稍后启动"的所有权架构。
- 测试 / 仿真：动态启停节点。

不适合：

- 简单脚本（直接默认构造即可）。
- 节点配置完全不变的场景。

## Init 模式对比

| 模式 | 构造 | 行为 |
|------|------|------|
| Auto | `Publisher<T>("url")` | 构造时立即创建 transport |
| Deferred | `Publisher<T>("url", InitType::kWithoutInit)` | 不创建 transport；等 `init()` |

延迟模式让你可以在 init 之前配置 QoS / ser_type / discovery 等只在 init 时被 transport 读取的字段。

## 核心 API

| API | 签名 | 说明 |
|-----|------|------|
| `Publisher<T>(url, InitType)` 等 | `InitType { kWithInit, kWithoutInit }` | 选择是否构造期自动 init |
| `Node::init` | `virtual bool init()` | 创建底层 transport；幂等（多次调用安全） |
| `Node::deinit` | `virtual bool deinit()` | 释放 transport；publish/listen 等返回 false |
| `Node::has_inited` | `bool has_inited() const` | 状态查询 |
| `Node::interrupt` | `virtual void interrupt()` | 唤醒所有 `wait_for_*` 阻塞调用 |

## 代码导读

### 1. 构造期自动 init（默认）

```cpp
vlink::Publisher<std::string> pub("dds://topic");   // 构造完即可 publish
pub.publish("hello");
```

### 2. 延迟 init + property 配置

```cpp
vlink::Publisher<std::string> pub("dds://topic", vlink::InitType::kWithoutInit);
pub.set_property("qos.reliability.kind", "1");
pub.set_property("qos.history.depth", "50");
pub.set_ser_type("MyType", vlink::SchemaType::kRaw);

bool ok = pub.init();      // 此时按上面配置创建 transport
if (!ok) {
  VLOG_E("init failed");
}
```

### 3. has_inited / 重新 init

```cpp
VLOG_I("inited: ", pub.has_inited());    // 1

pub.deinit();
VLOG_I("inited: ", pub.has_inited());    // 0

pub.init();
VLOG_I("inited: ", pub.has_inited());    // 1
```

`init()` 是幂等的：已 init 时再调返回 true、不重复创建 transport。

### 4. interrupt 唤醒阻塞

```cpp
vlink::Subscriber<std::string> sub("dds://topic");
std::thread bg([&sub]() {
  // 阻塞等订阅消息
  sub.wait_for_value(100s);
});

// 主线程做其它事
std::this_thread::sleep_for(100ms);

sub.interrupt();   // 立刻唤醒所有 wait_for_*
bg.join();
```

`interrupt()` 只设 interrupted flag、唤醒 cv；**不会**停回调、不释放 transport。要真正停用必须 `deinit()`。

### 5. 推荐流程

```
construct (kWithoutInit) -> set_property() -> init() -> listen() / publish() / ... -> deinit()
```

适用所有六种原语。

## 运行

```bash
./build/output/bin/example_lifecycle
```

预期输出（节选）：

```
auto init: ok
deferred init: configured, then init() = 1
has_inited=1
deinit: ok, has_inited=0
re-init: ok, has_inited=1
interrupt: wait_for_value returned early
```

## 常见陷阱

1. **kWithoutInit 后忘记 init**：`publish` / `listen` 返回 false。
2. **init 之后改 property**：大多数 property 在 init 期被 transport 读取，之后改不生效。
3. **deinit 然后再 publish**：返回 false，不报错。
4. **interrupt 与 deinit 混淆**：interrupt 仅唤醒；deinit 才释放。
5. **多线程并发 init / deinit**：vlink 不保证线程安全；按一个 owner 串行调用。

## 设计要点

- Node<ImplT, SecT> 是模板基类；六种通信原语都继承它。
- `init()` 在内部构造 `ImplT`（底层 transport 实现）；`deinit()` 析构。
- `interrupt()` 设 atomic flag + notify_all；用于"主动取消等待"场景。
- 不同的传输后端 init 开销不同：intra 几乎零开销；shm/dds 需要 discovery，可能数毫秒。

## 配图

![Node lifecycle](./images/node-lifecycle.png)

图中展示节点状态机：constructed → inited → in-use → deinited → re-inited 等。

## 参考

- `../properties/` — init 前能配的 property
- `../message_loop_binding/` — attach loop
- 顶层 `doc/02-node-lifecycle.md` — 完整章节
- `vlink/include/vlink/node.h` — Node 基类接口
