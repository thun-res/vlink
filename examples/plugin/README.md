# plugin — vlink 运行时插件系统

vlink 提供基于 `dlopen` + 工厂宏的运行时插件机制：定义抽象接口、把实现编译为 `.so`、运行期 `Plugin::load<T>()` 加载、通过虚函数调用。结合 `RunablePluginInterface` 与 `SchemaPluginInterface`，可以构建模块化扩展架构。

读完本目录你能掌握：

- vlink 插件系统的三个核心宏（`VLINK_PLUGIN_REGISTER` / `VLINK_PLUGIN_REGISTER_BY_ID` / `VLINK_PLUGIN_DECLARE`）。
- 怎么定义自家的插件接口并写实现。
- 自带 MessageLoop 的 RunablePlugin 模式。
- Schema 插件（Protobuf descriptor / FlatBuffers BFBS 运行时查找）。

## 子示例索引

| 示例 | 主题 | 关键类 |
|------|------|--------|
| `plugin_basic/` | 定义接口、构建 .so、`Plugin::load<T>()` 加载、调虚函数 | `vlink::Plugin`、`VLINK_PLUGIN_REGISTER` |
| `plugin_runnable/` | `RunablePluginInterface` —— 插件自带 MessageLoop | `vlink::RunablePluginInterface` |
| `plugin_schema/` | `SchemaPluginInterface` —— 运行期 protobuf/flatbuffers schema 查找 | `vlink::SchemaPluginInterface` / `SchemaPluginManager` |

## 推荐阅读顺序

1. **`plugin_basic/`** —— 必看。理解插件加载的完整机制（宏展开、版本检查、ID 校验、deleter）。
2. **`plugin_runnable/`** —— 插件不仅仅是函数集合：可以自带事件循环，由 host 驱动 init/deinit。
3. **`plugin_schema/`** —— vlink 自身用 Schema 插件做录制时的 Protobuf 反射；理解后能写自己的 Schema 后端。

## 共同前置知识

- C++ 抽象接口、虚函数、`shared_ptr` 自定义 deleter。
- `dlopen` / `dlsym` 基础（或了解等价 Windows / macOS 机制）。
- `../base/` 中的 MessageLoop（runnable plugin 直接继承）。

## 三个宏速查

| 宏 | 用途 | 用在哪 |
|----|------|-------|
| `VLINK_PLUGIN_REGISTER(Interface)` | 给接口注入 `get_plugin_id` | 抽象类内部 + 实现类内部 |
| `VLINK_PLUGIN_REGISTER_BY_ID(Interface, "stable-id")` | 同上 + 显式 ID | 同上 |
| `VLINK_PLUGIN_DECLARE(Impl, Major, Minor)` | 导出 `vlink_plugin_create` / `_destroy` | 插件 .cc 末尾 |

## 配图

每个示例都有流程图：

- `plugin_basic/images/plugin-basic-flow.png` —— 接口 / 插件 / host 三方的代码与运行时关系
- `plugin_runnable/images/runnable-lifecycle.png` —— RunablePlugin 的 init/run/deinit 时序
- `plugin_schema/images/schema-plugin-flow.png` —— Schema 查找流程

## 参考

- 顶层 `doc/19-extensions.md` —— 插件系统完整设计
- `vlink/include/vlink/base/plugin.h` —— Plugin 接口
- `vlink/include/vlink/extension/runnable_plugin_interface.h` —— RunablePluginInterface
- `vlink/include/vlink/extension/schema_plugin_interface.h` —— Schema 接口
