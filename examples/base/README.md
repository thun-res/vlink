# 🧱 base — vlink 基础库示例

`base/` 目录收录 `vlink/base/` 公共头文件下各组件的独立示例。它们不依赖任何通信后端（不出现 `intra://`、`shm://`、`dds://` 等 URL），可单独编译运行，便于在阅读 `communication/` 等上层示例之前先熟悉框架共用的基础设施。

与 `communication/` 等分类相比，`base/` 关注"在一个进程内"的能力：字节载体、日志、事件循环、定时器。绝大多数 vlink 上层 API 都建立在这些组件之上，因此即便不写消息收发代码，本目录的示例也值得通读。

## 📑 子示例索引

| 示例 | 主题 | 涉及类 / 头文件 |
|------|------|------------------|
| `bytes_basic/` | `Bytes` 创建、SBO/堆切换、offset、resize 族、内容比较 | `vlink::Bytes` |
| `logger_basic/` | 四种调用风格（`VLOG_*` / `MLOG_*` / `CLOG_*` / `SLOG_*`）+ 运行期级别切换 | `vlink::Logger` |
| `message_loop_basic/` | `async_run` / `post_task` / 生命周期 handler / `spin_once` / 状态查询 | `vlink::MessageLoop` |
| `timer/` | `attach` / `start` / `stop` / `loop_count` / `call_once` / 动态间隔切换 | `vlink::Timer` |

## 📖 推荐阅读顺序

1. `bytes_basic/` —— 字节载体的两种典型用法（拥有 / 处理），是消息载荷与序列化的底座。
2. `logger_basic/` —— 四种日志宏与控制台 / 文件级别分控；后续示例普遍使用 `VLOG_I` / `MLOG_I`。
3. `message_loop_basic/` —— 可选的应用层串行调度器；仅在后端支持节点 attach 时接管通信回调。
4. `timer/` —— 周期 / 单次 / 动态定时器，须绑定到 `MessageLoop` 才能触发回调。

`MessageLoop` 也是 `Timer` 的必需调度器，Timer 必须挂在 loop 上才会触发，因此第 3、4 项建议连续阅读。

## 🖼️ 配图

各示例自带 `images/*.png` 图示，在各自 README 中引用：

- `bytes_basic/images/bytes-sbo-vs-heap.png` —— `Bytes` 对象的内联 SBO 与堆切换边界
- `message_loop_basic/images/message-loop-architecture.png` —— 单事件循环的线程模型与任务流向
- `timer/images/timer-lifecycle.png` —— `Timer::attach/start/stop/restart` 的状态机

## 🔗 参考

- `doc/08-base-library.md` —— `vlink/base/` 整体设计文档，涵盖 zerocopy 别名 Bytes、Logger 进阶、MessageLoop 进阶、ElapsedTimer / DeadlineTimer、ThreadPool、Cancellation / TaskHandle、Process / Utils 等本目录未单列示例的组件
- `doc/00-overview.md` —— vlink 总览（含三种通信模型）
- `include/vlink/base/` —— 全部公共头文件
