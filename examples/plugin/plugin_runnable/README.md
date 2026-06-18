# plugin_runnable — 自带 MessageLoop 的插件：`RunablePluginInterface`

`vlink::RunablePluginInterface` 是一种"自带事件循环"的插件：插件类直接继承 `MessageLoop`，可以注册 Timer、Subscriber、Server 等需要 loop 驱动的对象。host 负责加载与生命周期管理；插件在自己的线程里跑。

读完本示例你能掌握：

- `RunablePluginInterface` 与基础 plugin 的差异。
- `on_init()` / `on_deinit()` 的语义和典型用法。
- host 端的 async_run / on_init / on_deinit / quit / wait_for_quit 调用顺序。
- 与 `ProxyServer::Config::runnable_list` 的集成。

## 背景与适用场景

适用：

- 长跑后台模块（监控、采集、心跳、桥接）。
- 需要 Timer / Subscriber 但希望独立线程跑。
- 想统一管理一组运行时插件的服务端（配合 `proxy/proxy_runnable_plugin/`）。

不适合：

- 无状态的函数库（用基础 plugin）。
- 需要 host 直接调用的 RPC 风格接口。

## 文件结构

| 文件 | 角色 |
|------|------|
| `monitor_plugin.cc` | 插件：500ms Timer 计数；编译为 `libmonitor_plugin.so` |
| `plugin_runnable.cc` | host：load、run、shutdown |

## 接口

`RunablePluginInterface : public MessageLoop` 在 MessageLoop 基础上加两个生命周期方法：

| 方法 | 调用时机 | 用途 |
|------|---------|------|
| `on_init()` | host 调 `async_run()` 之后 | 创建 Timer、Subscriber 等 |
| `on_deinit()` | host 调 `quit()` 之前 | 释放资源 |

## 代码导读

### 1. 插件实现

```cpp
class MonitorPlugin : public vlink::RunablePluginInterface {
  VLINK_PLUGIN_REGISTER(vlink::RunablePluginInterface)
 public:
  void on_init() override {
    timer_ = std::make_unique<vlink::Timer>(this, 500, vlink::Timer::kInfinite, [this]() {
      ++tick_;
      VLOG_I("tick #", tick_);
    });
    timer_->start();
  }

  void on_deinit() override {
    timer_->stop();
    timer_.reset();
  }

 private:
  int tick_ = 0;
  std::unique_ptr<vlink::Timer> timer_;
};

VLINK_PLUGIN_DECLARE(MonitorPlugin, 1, 0)
```

Timer 构造时 `this` 是 `RunablePluginInterface*`，也是 `MessageLoop*`；timer 回调跑在插件自己的 loop 线程上。

### 2. Host

```cpp
vlink::Plugin plugin;
auto p = plugin.load<vlink::RunablePluginInterface>("monitor_plugin", 1, 0);
if (!p) return 1;

p->async_run();        // 启动 loop 后台线程
p->on_init();           // 创建插件内部资源

// host 主线程做自己的事...
std::this_thread::sleep_for(2s);

p->on_deinit();         // 释放插件资源
p->quit();              // 请求 loop 退出
p->wait_for_quit();     // 等真正退出
```

### 3. ProxyServer 集成

```cpp
vlink::ProxyServer::Config cfg;
cfg.runnable_list = {"monitor_plugin"};   // 多个 runnable 插件
vlink::ProxyServer server(cfg);
server.start();
```

`runnable_list` 让 ProxyServer 自动 load、init、run、deinit；详见 `../../proxy/proxy_runnable_plugin/`。

## 运行

```bash
./build/output/bin/example_plugin_runnable
```

预期输出（节选）：

```
tick #1
tick #2
tick #3
tick #4
(2 秒后退出)
```

## 常见陷阱

1. **on_init/on_deinit 不在 loop 线程上**：它们在 host 调用线程上跑；如要在 loop 线程做事用 `post_task`。
2. **忘记 on_deinit**：插件资源没释放；host 必须按 `async_run → on_init → ... → on_deinit → quit → wait_for_quit` 顺序。
3. **`wait_for_quit` 不带超时**：可能永远阻塞；生产代码设超时然后 kill。
4. **shared_ptr<RunablePlugin> 跨多 owner**：所有 owner 都释放后才真正 destroy + dlclose。
5. **on_init 抛异常**：vlink 不处理；host 必须包 try/catch。

## 设计要点

- `RunablePluginInterface` 继承 `MessageLoop`，所有 MessageLoop API 都可用。
- 插件可以注册 Subscriber、Timer、Server 等需要 loop 的组件。
- `wait_for_quit` 会等待 loop 真的退出（包括内部 timer 析构）。

## 配图

![Runnable lifecycle](./images/runnable-lifecycle.png)

图中展示 host 与 plugin 之间的生命周期时序：load → async_run → on_init → 业务运行 → on_deinit → quit → wait_for_quit → unload。

## 参考

- `../plugin_basic/` — 基础 plugin 机制（先看）
- `../plugin_schema/` — Schema 插件
- `../../proxy/proxy_runnable_plugin/` — RunablePlugin 与 ProxyServer 的集成
- `vlink/include/vlink/extension/runnable_plugin_interface.h` — 接口
- 顶层 `doc/19-extensions.md` — 插件系统章节
