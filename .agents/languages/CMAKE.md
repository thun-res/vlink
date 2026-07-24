# CMake 编码规范

适用于 `CMakeLists.txt`、`cmake/` 与工具链文件。现有
`.cmake-format` 是排版唯一配置:2 空格缩进、120 列、控制语句与函数名
不留额外空格、长调用使用悬挂右括号。

## 1. 目标与作用域

- 人工整改不触及第三方、预构建、构建目录与生成的 CMake 文件;显式
  调用 `/format` 时仅按该 skill 和 `tools/format.sh` 的既定范围处理。
  根 `cmake_minimum_required` 与 policy 基线由项目统一维护,子目录
  不得自行降低或覆盖。
- 优先使用 target 级命令:`target_link_libraries`、
  `target_include_directories`、`target_compile_definitions` 与
  `target_compile_options`;禁止新增 `include_directories`、
  `link_libraries`、全局 `add_definitions` 等目录级编译污染。
- `PUBLIC`/`PRIVATE`/`INTERFACE` 必须反映真实传播关系。公开头文件所需
  依赖才可 `PUBLIC`,实现依赖保持 `PRIVATE`。
- 函数内部变量保持局部;需要回传时明确 `PARENT_SCOPE`。缓存变量只用于
  用户配置或工具链入口,写明类型和说明。
- 已存在的导出、namespace、install component、runtime/devel 拆分必须
  保持一致,不得只让 build tree 可用而破坏 install tree。

## 2. 命名与控制流

- 项目选项、缓存变量和跨目录变量使用 `UPPER_SNAKE_CASE`;内部临时变量
  沿用相邻函数命名。VLink helper 以 `vlink_` 开头。
- 平台判断沿用 `WIN32`、`APPLE`、`QNX`、`ANDROID` 和编译器 ID 的
  既有分支;新平台行为不得悄悄落入错误的 `else`。
- 缺少必需依赖使用 `FATAL_ERROR`;可选功能明确 `STATUS`/`WARNING`
  并提前返回。`execute_process` 的结果影响后续正确性时检查
  `RESULT_VARIABLE` 和必要输出。
- 条件、列表和路径变量按相邻代码引用。单值路径加双引号;列表是否加
  引号取决于被调用命令期望单个列表值还是多个参数,不得机械统一。
- CPM/FetchContent 依赖沿用仓库集中声明、版本固定与离线缓存策略;
  模块内不得私自下载第二份同名依赖。

## 3. 文件、安装与验证

- 新增源文件时沿用所在模块当前的显式列表或 `GLOB CONFIGURE_DEPENDS`,
  不在同一模块混用两套策略;使用 GLOB 时必须带
  `CONFIGURE_DEPENDS`,保证新增/删除文件触发重配置。
- 安装规则同步头文件、运行库、开发库、CMake export、namespace、
  version/config 文件与许可证组件;同时验证 build tree 和 install tree。
- 公开模块、选项或生成物变化按影响面同步 `doc/`、examples、
  `languages/python_api` 与 package 配置(如适用);公共安装/export 变化必须验证
  下游消费者能从 install tree 正常 `find_package` 和链接。
- 只有维护者明确要求时才对本次触及的 CMake 文件运行
  `cmake-format`;显式调用 `/format` 时运行全仓 `tools/format.sh`,
  configure/build 也需维护者明确要求。
- AI 在当前工作区获得配置或构建授权后,所有 `cmake -B` 目录必须位于
  仓库根 `build-ai/` 的独立任务子目录。不得复用维护者、IDE 或 CI 的
  其他构建目录,也不得直接把 `build-ai` 作为构建目录。
  可能启动 Python 时,`PYTHONPYCACHEPREFIX` 必须指向
  `{project}/build-ai/<task_name>/__pycache__`。
- 编译前按根 `AGENTS.md` 第 6 条取得真实物理核心数,将
  `max(物理核心数 - 1, 1)` 作为显式数值传给 `cmake --build --parallel`;
  获取失败时使用 1。禁止让 CMake/生成器自行采用全部逻辑 CPU,也禁止
  裸 `--parallel`、裸 `-j`、固定高并行度或同时启动多个本地构建。
