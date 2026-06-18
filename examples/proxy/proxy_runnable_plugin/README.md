# proxy_runnable_plugin — ProxyServer 加载 RunablePlugin 的集成模式

本示例演示如何把 `RunablePluginInterface` 插件交给 `ProxyServer` 统一管理：ProxyServer 在启动时自动 load、async_run、on_init；停止时自动 on_deinit、quit、unload。同时示例也演示了独立 load（不通过 ProxyServer）的对照路径。

读完本示例你能掌握：

- ProxyServer 怎么管理一组 RunablePlugin 的 lifecycle。
- 怎么把自己的 RunablePlugin 接入 ProxyServer（写法约定 + Config 字段）。
- `VLINK_PLUGIN_DIR` 环境变量在 ProxyServer 集成中的角色。

## 背景与适用场景

适用：

- ProxyServer 既要做远程观察，又要在 server 进程内挂几个常驻业务模块。
- 想用 Server 的进程作为"插件 host"，把插件 lifecycle 与 server 的 start/stop 绑定。
- 自家的"插件总线"架构：Server 是入口，插件实现各自业务。

不适合：

- 简单的 standalone 插件（直接用 `vlink::Plugin::load`，见 `../../plugin/plugin_runnable/`）。
- 不需要 Proxy 远程观察能力。

## 插件要求

RunablePlugin 要满足：

1. 继承 `vlink::RunablePluginInterface`。
2. 类内：`VLINK_PLUGIN_REGISTER(vlink::RunablePluginInterface)`。
3. 覆写 `on_init()` / `on_deinit()`。
4. 文件末尾：`VLINK_PLUGIN_DECLARE(ClassName, Major, Minor)`。

参考实现：`../../plugin/plugin_runnable/monitor_plugin.cc`，编译为 `libmonitor_plugin.so`。

## ProxyServer 集成

```cpp
vlink::ProxyServer::Config cfg;
cfg.runnable_list           = {"monitor_plugin", "analysis_plugin"};   // 库名，不带前缀后缀
cfg.runnable_version_major  = 1;
cfg.runnable_version_minor  = 0;
cfg.runnable_prefix         = "";       // 可选库名前缀，默认空
```

Server 启动后会对每个插件：

```
load → async_run → on_init → ... → on_deinit → quit → wait_for_quit → unload
```

## 核心 API

| 字段 | 类型 | 说明 |
|------|------|------|
| `ProxyServer::Config::runnable_list` | `std::vector<std::string>` | 要加载的插件库名（不带 `lib` / `.so`） |
| `ProxyServer::Config::runnable_version_major` / `_minor` | `uint16_t` | 期望版本 |
| `ProxyServer::Config::runnable_prefix` | `std::string` | 可选前缀（一些项目用统一前缀） |

## 代码导读

### 1. 通过 ProxyServer 加载

```cpp
vlink::ProxyServer::Config cfg;
cfg.dds_impl = "dds";
cfg.domain_id = 0;
cfg.runnable_list = {"monitor_plugin"};
cfg.runnable_version_major = 1;
cfg.runnable_version_minor = 0;

vlink::ProxyServer server(cfg);
if (!server.start()) {
  VLOG_E("failed to start server (monitor_plugin not found?)");
  return 1;
}

// 主程序做事
std::this_thread::sleep_for(3s);

server.stop();   // 自动 deinit 所有插件
```

### 2. 独立 load（对照）

```cpp
vlink::Plugin plugin;
auto p = plugin.load<vlink::RunablePluginInterface>("monitor_plugin", 1, 0);
if (p) {
  p->async_run();
  p->on_init();
  std::this_thread::sleep_for(3s);
  p->on_deinit();
  p->quit();
  p->wait_for_quit();
}
```

这条路径上插件 lifecycle 由调用方手动管理；ProxyServer 集成则统一管理。

## 运行

```bash
./build/output/bin/example_proxy_runnable_plugin
```

如果 `libmonitor_plugin.so` 不在 search path：

```bash
VLINK_PLUGIN_DIR=/work/vlink/build/output/lib ./build/output/bin/example_proxy_runnable_plugin
```

预期输出（节选）：

```
ProxyServer started with 1 runnable plugins
tick #1
tick #2
...
ProxyServer stopping, deiniting plugins
```

## 最佳实践

1. **所有回调绑到插件自己的 loop**：Timer / Subscriber 都用 `this` 作为 loop 指针。
2. **on_init 不阻塞**：长任务用 `post_task` 异步派到 loop。
3. **on_deinit 释放所有资源**：Timer.stop、Subscriber/Server 析构、线程 join。
4. **VLINK_PLUGIN_DIR 控制搜索路径**：CI 部署时显式指定，避免依赖系统默认路径。
5. **版本匹配**：runnable_version_major/minor 与插件 `VLINK_PLUGIN_DECLARE` 必须一致。

## 设计要点

- ProxyServer 持有插件的 shared_ptr<RunablePluginInterface>；引用计数 → 0 时 unload。
- 多个 runnable 插件按 list 顺序顺序 load / init；deinit 是逆序。
- 如果某个插件 load 失败（version 不匹配 / .so 不存在），`server.start()` 返回 false。

## 配图

![ProxyServer runnable plugin](./images/proxy-runnable-plugin.png)

图中展示 ProxyServer 与多个 RunablePlugin 之间的 lifecycle 协作。

## 参考

- `../../plugin/plugin_runnable/` — 构建 monitor_plugin.so
- `../proxy_server_basic/` — ProxyServer 基础
- 顶层 `doc/16-proxy.md` — Proxy 章节
- 顶层 `doc/19-extensions.md` — 插件系统章节
