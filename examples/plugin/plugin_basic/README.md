# plugin_basic — vlink 插件系统端到端：定义接口、构建 .so、加载、调用

本示例完整演示 vlink 插件机制：

1. 在共享头文件 `greeter_interface.h` 定义抽象接口。
2. 在 `greeter_plugin.cc` 实现该接口，编译为 `libgreeter_plugin.so`。
3. 在 `plugin_basic.cc` 用 `vlink::Plugin::load<T>()` 加载、调用、卸载。

读完本示例你能掌握：

- 插件接口的写法（虚析构、`VLINK_PLUGIN_REGISTER` 宏）。
- 插件实现导出的宏（`VLINK_PLUGIN_DECLARE`）。
- `Plugin::load<T>(name, major, minor)` 的版本/ID 校验。
- search path、显式目录、`VLINK_PLUGIN_DIR` 环境变量。
- 用 stable-id 解耦接口名变更。

## 背景与适用场景

适用：

- 模块化架构：核心 + 多个独立 .so 插件分别由不同团队维护。
- 运行时动态加载：按业务需要选择性 dlopen。
- 第三方扩展：让用户基于 vlink 接口写自己的实现。

不适合：

- 静态编译进单一可执行（直接 link 即可）。
- 跨进程边界（用 vlink 通信原语）。

## 三方文件结构

| 文件 | 角色 |
|------|------|
| `greeter_interface.h` | host 和 plugin 共享的抽象接口头 |
| `greeter_plugin.cc` | 具体实现，编译为 `libgreeter_plugin.so` |
| `plugin_basic.cc` | host 程序：load / call / destroy |

## 核心 API

### 接口侧（共享头）

```cpp
class GreeterInterface {
  VLINK_PLUGIN_REGISTER(GreeterInterface)
 public:
  virtual ~GreeterInterface() = default;
  virtual std::string greet(const std::string& name) = 0;
};
```

要求：

- 至少一个纯虚函数。
- 虚析构。
- `VLINK_PLUGIN_REGISTER(InterfaceName)` 宏（合成 `get_plugin_id()`）。

### 插件侧

```cpp
class GreeterImpl : public GreeterInterface {
  VLINK_PLUGIN_REGISTER(GreeterInterface)   // 接口名，不是实现名
 public:
  std::string greet(const std::string& name) override { return "Hi " + name; }
};

VLINK_PLUGIN_DECLARE(GreeterImpl, /*major=*/1, /*minor=*/0)
```

`VLINK_PLUGIN_DECLARE` 导出 C 风格的 `vlink_plugin_create` / `vlink_plugin_destroy`，host 通过 dlsym 调用。

### Host 侧

```cpp
vlink::Plugin plugin;
auto greeter = plugin.load<GreeterInterface>("greeter_plugin", /*major=*/1, /*minor=*/0);
if (greeter) {
  VLOG_I(greeter->greet("VLink"));
}
```

`load<T>()` 内部：

1. `dlopen` 搜索 `lib<name>.so` / `<name>.dll`。
2. `dlsym` 找 `vlink_plugin_create` / `_destroy`。
3. 调 `vlink_plugin_create()` 拿实例。
4. 校验 plugin_id 与 host 的 `GreeterInterface::get_plugin_id()` 匹配。
5. 校验版本：major 必须一致，minor 必须 ≥ host 期望。
6. 返回 `shared_ptr<T>` + 自定义 deleter（deleter 调 `vlink_plugin_destroy`）。

## 代码导读

### 1. 接口头

```cpp
// greeter_interface.h
#include <vlink/base/plugin.h>
#include <string>

class GreeterInterface {
  VLINK_PLUGIN_REGISTER(GreeterInterface)
 public:
  virtual ~GreeterInterface() = default;
  virtual std::string greet(const std::string& name) = 0;
};
```

### 2. 插件实现

```cpp
// greeter_plugin.cc
#include "./greeter_interface.h"

class GreeterImpl : public GreeterInterface {
  VLINK_PLUGIN_REGISTER(GreeterInterface)
 public:
  std::string greet(const std::string& name) override { return "Hi " + name; }
};

VLINK_PLUGIN_DECLARE(GreeterImpl, 1, 0)
```

CMake 把它编为共享库 `libgreeter_plugin.so`。

### 3. Host

```cpp
// plugin_basic.cc
vlink::Plugin plugin;
auto g = plugin.load<GreeterInterface>("greeter_plugin", 1, 0);
if (!g) {
  VLOG_E("failed to load plugin");
  return 1;
}
VLOG_I(g->greet("VLink"));
```

### 4. 搜索路径

```cpp
plugin.load<T>("name", 1, 0);                       // 默认搜索路径
plugin.load<T>("name", 1, 0, "/custom/dir");         // 显式目录
plugin.load<T>("name", 1, 0, {"/dir1", "/dir2"});    // 路径列表
```

默认 search path 含：可执行文件目录、`/usr/local/lib`、`/usr/lib`、cwd。
环境变量 `VLINK_PLUGIN_DIR` 可追加。

### 5. Stable ID（避免接口名变更引起的不兼容）

```cpp
class GreeterInterface {
  VLINK_PLUGIN_REGISTER_BY_ID(GreeterInterface, "com.example.greeter.v1")
};
```

`_BY_ID` 让 plugin_id 不依赖类型名（默认是 typeid name，跨编译器可能不一致）。

## 运行

```bash
./build/output/bin/example_plugin_basic
# 期望输出: Hi VLink
```

如果 .so 不在搜索路径：

```
[E] failed to load plugin
```

## 常见陷阱

1. **`VLINK_PLUGIN_REGISTER` 用错类**：接口和实现都要带，且参数都是接口名。
2. **`VLINK_PLUGIN_DECLARE` 重复**：一个 .so 只能 declare 一次。
3. **shared_ptr 还活着就 unload**：用悬空指针；先 reset 所有 shared_ptr。
4. **version 不兼容**：major 不一致 / minor 太低 → load 返回 nullptr，无声失败。检查返回值。
5. **跨编译器**：接口 ABI（v-table 布局、name mangling）必须兼容；推荐 host + plugin 用同一编译器同一标准。

## 设计要点

- 通过 C 函数 `vlink_plugin_create` 跨 ABI 边界；接口本身是 C++ 虚类，要求双方编译器 ABI 兼容。
- `shared_ptr<T>` 的自定义 deleter：所有引用释放后才 destroy 实例、close .so。
- typeid name 在不同编译器下可能不同；跨编译器用 `_BY_ID` 形式。

## 配图

![Plugin basic flow](./images/plugin-basic-flow.png)

图中展示接口头 / plugin .so / host 三方在编译期和运行期的关系。

## 参考

- `../plugin_runnable/` — 自带 MessageLoop 的插件
- `../plugin_schema/` — Schema 插件
- 顶层 `doc/19-extensions.md` — 插件系统完整章节
- `vlink/include/vlink/base/plugin.h` — Plugin 接口
