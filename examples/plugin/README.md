# 🧩 plugin — vlink 运行时插件系统

vlink 提供基于 `dlopen` + 工厂宏的运行时插件机制：定义抽象接口、把实现编译为 `.so`、运行期 `Plugin::load<T>()` 加载、通过虚函数调用。该机制适用于"核心 + 多个独立 `.so` 插件分团队维护、运行时按需加载、第三方扩展"等模块化扩展架构。

读完本目录可掌握：

- vlink 插件系统的三个核心宏：`VLINK_PLUGIN_REGISTER`、`VLINK_PLUGIN_REGISTER_BY_ID`、`VLINK_PLUGIN_DECLARE`。
- 如何定义插件接口、编写实现、构建为共享库并在 host 中加载调用。

> Schema 插件（Protobuf descriptor / FlatBuffers BFBS 运行时查找）与自带 MessageLoop 的 `RunablePluginInterface`，请参阅顶层 `doc/13-integration.md`。

## 📑 子示例索引

| 示例 | 主题 | 关键类 |
|------|------|--------|
| `plugin_basic/` | 定义接口、构建 `.so`、`Plugin::load<T>()` 加载、调用虚函数 | `vlink::Plugin`、`VLINK_PLUGIN_REGISTER` |

## 🔧 三个宏概览

| 宏 | 用途 | 用在哪 |
|----|------|--------|
| `VLINK_PLUGIN_REGISTER(Interface)` | 给接口注入 `get_plugin_id` | 抽象类内部 + 实现类内部 |
| `VLINK_PLUGIN_REGISTER_BY_ID(Interface, "stable-id")` | 同上，并指定显式 ID | 同上 |
| `VLINK_PLUGIN_DECLARE(Impl, Major, Minor)` | 导出 `vlink_plugin_create` / `_destroy` | 插件 `.cc` 末尾 |

## 📋 前置知识

- C++ 抽象接口、虚函数、`shared_ptr` 自定义 deleter。
- `dlopen` / `dlsym` 基础（或等价的 Windows / macOS 机制）。

## 🖼️ 配图

- `plugin_basic/images/plugin-basic-flow.png` —— 接口 / 插件 / host 三方的代码与运行时关系

## 📚 参考

- 顶层 `doc/13-integration.md` —— 插件系统完整设计（含 Schema 插件与 RunablePlugin）
- `include/vlink/base/plugin.h` —— `Plugin` 接口
- `include/vlink/extension/runnable_plugin_interface.h` —— `RunablePluginInterface`
- `include/vlink/extension/schema_plugin_interface.h` —— Schema 插件接口
