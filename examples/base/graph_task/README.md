# graph_task — `vlink::GraphTask` DAG 依赖执行

`vlink::GraphTask` 让你以声明式写法定义"任务节点 + 依赖边"组成的 DAG，独立节点并行执行、有依赖的节点等前驱完成后再跑。支持条件分支、状态回调、DOT 导出。

读完本示例你能掌握：

- 构造 DAG 的两种节点类型（普通节点、条件节点）。
- `precede` / `succeed` 声明依赖。
- 多前驱合流的 wait 策略（all / any）。
- 状态回调跟踪节点 lifecycle。
- DOT 导出用于可视化调试。

## 背景与适用场景

适用：

- 数据处理流水线：A → B → {C, D} → E。
- 多阶段计算：感知 → 预测 → 规划，每阶段有多个子任务。
- 任务图建模业务流程：可视化、性能分析、并行优化。

不适合：

- 简单两个任务的依赖（直接 future 链接更轻）。
- 节点频繁动态增删（GraphTask 假定 DAG 在 execute 前构造完）。

## 核心 API

| API | 签名 | 说明 |
|-----|------|------|
| `GraphTask::create` | `static std::shared_ptr<GraphTask> create(std::string name, Function<void()>&&)` | 普通任务节点 |
| `GraphTask::create_condition` | `static std::shared_ptr<GraphTask> create_condition(std::string name, Function<int()>&&)` | 条件节点；返回 int 选下一个分支 |
| `precede` | `void precede(const std::shared_ptr<GraphTask>&)` | A.precede(B)：A 完成后激活 B |
| `succeed` | `void succeed(const std::shared_ptr<GraphTask>&)` | 反向 |
| `set_policy` | `void set_policy(ExecutionPolicy)` | `kPolicyWaitAll` / `kPolicyWaitAny` |
| `register_status_callback` | `void register_status_callback(Function<void(Status)>&&)` | 监听节点状态变化 |
| `execute` | `template <typename Engine> std::shared_ptr<TaskHandle> execute(Engine&)` | 提交到 MultiLoop / ThreadPool |
| `has_cycle` | `bool has_cycle() const` | DAG 校验 |
| `export_to_dot` | `std::string export_to_dot() const` | 导出 graphviz DOT 字符串 |

## 代码导读

### 1. 线性 DAG

```cpp
auto a = GraphTask::create("A", []() { VLOG_I("  A"); });
auto b = GraphTask::create("B", []() { VLOG_I("  B"); });
auto c = GraphTask::create("C", []() { VLOG_I("  C"); });

a->precede(b);
b->precede(c);

MultiLoop engine(2);
engine.async_run();
a->execute(engine)->wait();
```

A → B → C 顺序执行。

### 2. 菱形 DAG + WaitAll

```cpp
auto a = GraphTask::create("A", []() { /* ... */ });
auto b = GraphTask::create("B", []() { /* ... */ });
auto c = GraphTask::create("C", []() { /* ... */ });
auto d = GraphTask::create("D", []() { /* ... */ });

a->precede(b);
a->precede(c);
b->precede(d);
c->precede(d);
d->set_policy(GraphTask::kPolicyWaitAll);   // D 等 B、C 都完成

a->execute(engine)->wait();
```

`kPolicyWaitAll`（默认）：D 等所有前驱完成。`kPolicyWaitAny`：任一前驱完成就激活 D。

### 3. 条件分支

```cpp
auto cond = GraphTask::create_condition("decide", []() -> int {
  return /* 0 or 1 */;
});
auto path_a = GraphTask::create("path_a", []() { /* ... */ });
auto path_b = GraphTask::create("path_b", []() { /* ... */ });
cond->precede(path_a);   // index 0
cond->precede(path_b);   // index 1
```

condition 节点返回 int，框架按索引激活对应 successor。

### 4. 状态回调

```cpp
task->register_status_callback([](GraphTask::Status status) {
  VLOG_I("  status: ", status.name, " state=", static_cast<int>(status.state));
});
```

观察节点 `kInActive` → `kPending` → `kRunning` → `kDone` 的完整生命周期。

### 5. 校验 + DOT 导出

```cpp
if (root->has_cycle()) {
  VLOG_E("DAG has cycle, abort");
  return 1;
}

std::string dot = root->export_to_dot();
VLOG_I("DOT graph:\n", dot);
// 复制到 https://dreampuf.github.io/GraphvizOnline/ 可视化
```

## 运行

```bash
./build/output/bin/example_graph_task
```

预期输出（节选）：

```
=== Linear DAG ===
  A
  B
  C
=== Diamond DAG ===
  A
  B
  C
  D (waited B+C)
=== Condition branch ===
  decide -> 0
  path_a
=== Status callback ===
  status: A state=1 (Pending)
  status: A state=2 (Running)
  status: A state=3 (Done)
=== DOT export ===
digraph G {
  "A" -> "B";
  "B" -> "C";
  ...
}
```

## 常见陷阱

1. **execute 前未 has_cycle 检查**：循环依赖会让某些节点永远 Pending。
2. **状态回调跑在 engine 线程上**：阻塞会卡 DAG 推进。
3. **condition 节点返回越界**：vlink 行为按实现可能跳过或拒绝；用 valid range。
4. **node 析构早于 execute 结束**：execute 持引用，shared_ptr 不会被回收太早；但用户代码若释放 shared_ptr 也无害。
5. **kPolicyWaitAny 与 kPolicyWaitAll 混用**：每节点独立设；不要假设全局策略。

## 设计要点

- GraphTask 内部用 atomic 计数追踪前驱完成度。
- engine 可以是 MultiLoop / ThreadPool / 任何实现 `post_task` 的对象。
- DOT 导出格式标准 graphviz；可以串接 PR 流程做可视化。

## 配图

![GraphTask DAG](./images/graph-task-dag.png)

图中展示菱形 DAG 的执行时序：A 触发 B、C 并行，D 等 B+C 后启动。

## 参考

- `../multi_loop/` — 推荐的 engine
- `../thread_pool/` — 另一种 engine
- `../task_handle/` — execute 返回 TaskHandle
- `vlink/include/vlink/base/graph_task.h` — GraphTask 接口
