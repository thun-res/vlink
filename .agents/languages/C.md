# C 编码规范

适用于 `.c` 与纯 C API。通用性能、许可证、平台和流程约束仍以
`.agents/languages/CPP.md` 为上位规则;本页只补充 C 特有约定。

## 1. 文件与版式

- 人工整改不触及第三方、预构建、构建目录与生成代码;显式调用
  `/format` 时仅按该 skill 和 `tools/format.sh` 的既定范围处理。
- 每个 `.c` 保留 `.agents/languages/CPP.md` §3 的 22 行许可证头,
  逐字节一致。
- 使用仓库 `.clang-format` 的 Google 基线:2 空格缩进、120 列。
- 函数/变量使用 `snake_case`,宏与枚举常量使用 `UPPER_SNAKE_CASE`,
  typedef 沿用公开 C API 的 `_t` 后缀。
- 头文件顺序沿用相邻文件和 `.clang-format`;平台差异用最小
  `#ifdef _WIN32` 分支隔离。
- `if`、`else if`、`else` 分支体必须使用大括号,单行也不例外。

## 2. API、类型与内存

- 现有公开 ABI 的 `bool`、`size_t`、枚举、回调和 handle 布局均视为
  已冻结契约,不得为追求新风格改签名或改布局。新增 API 默认沿用现有
  handle 家族;只有全新且兼容策略已确认的 ABI 才考虑 opaque handle。
  新增整数优先固定宽度;不得把 C++ 类型、异常、模板或新的编译器相关
  布局泄漏到 C 边界。
- 公开 C 头使用 `#ifdef __cplusplus` / `extern "C"` 保护;导出符号、
  调用约定和可见性统一复用现有宏,不得直接写平台专用属性。
- 跨 ABI 结构体沿用已发布的字段宽度、顺序、对齐和 reserved 契约;
  新增或触及跨 ABI POD 时按各目标架构契约补 C/C++ 兼容的
  `sizeof`/`offsetof` 断言;不得把已有 POD handle 擅自改成 opaque
  handle,也不得为了满足断言改变现有 packing 或已发布布局。
- 输入缓冲区使用 `const uint8_t*` 与 `size_t`;回调签名、所有权、有效期
  和线程上下文必须与 `include/vlink/external/c_api.h` 一致。
- 每个 create/open 成功路径必须有对应 destroy/close;部分失败按逆序
  清理已经取得的资源。
- 转型沿用相邻 C API 的显式 C cast;不得通过 cast 丢弃 `const` 后写入。
- 检查长度、返回码和空指针的边界应贴近真实 API 契约,不堆叠无意义的
  重复判断。

## 3. 错误、并发与测试

- C 边界不得传播 C++ 异常;实现层转换为 `vlink_ret_t`/既有错误码。
- 回调可能运行在工作线程;共享 `user_data` 需要由调用者约定或显式同步,
  示例必须说明线程语义。
- 不用累加错误码代替逐步检查;首个失败应保留原始状态并进入统一清理。
- 新增/修改公开 C API 时同步 C 头、实现、C 示例、`c_api/test`、文档与
  Python 映射(如适用),并审查 ABI 兼容性。
- 修改后按用户明确要求运行 `clang-format`/cpplint;只有维护者明确要求
  时才构建或运行 C API 测试。
