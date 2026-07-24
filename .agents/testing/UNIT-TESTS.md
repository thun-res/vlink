# 单元测试规范

测试与贡献的通用说明见 `doc/15-contributing.md`;本文只收录 agent 写
测试时的硬约定。

## 1. 布局与框架

- 框架:**doctest**(不是 gtest)。入口 `test/main.cc`,公共设施
  `test/common_test.h`,安全相关辅助 `test/security_test_helpers.h`。
- 单一可执行 `vlink-test`(`test/CMakeLists.txt` GLOB 全目录),CTest 按
  test-suite 拆分注册:每个 suite 一条 `add_test`,运行时传
  `--test-suite=<名>`。
- 组织方式:
  - 每个传输后端一个根级文件:`intra_test.cc`、`shm_test.cc`、
    `dds_test.cc`、`zenoh_test.cc` 等——新增后端必须配套同名测试;
  - 子系统放子目录:`base/`、`extension/`、`impl/`、`modules/`、
    `zerocopy/`;
  - IDL/消息定义在 `test/idl/`。

## 2. 文件硬规则

- 22 行 Apache 许可证头照搬(同所有源文件)。
- 每个 `test/**/*.cc` 在许可证头之后、其余内容之前放置
  `// NOLINTBEGIN`,并在文件末尾用 `// NOLINTEND` 包裹全文——这是
  `test/` 独有的全文件豁免;lib 只能按 `.agents/languages/CPP.md`
  第 11 节做最小范围、带检查名的定点压制。
- 其余编码规则(命名、大括号、空行隔离、C++17 基线)与 lib 相同。

## 3. 写用例的习惯

- 新用例先模仿同后端/同子系统现有 `TEST_CASE` 的结构与命名口吻,
  suite 归属跟随所在文件。
- 涉及时序/调度的断言要容忍调度抖动(参照既有提交
  "make timer interval test scheduler-tolerant"的做法):断言区间而
  非精确值,禁止依赖 sleep 的确切时长。
- 跨进程/网络后端用例注意资源清理(topic 名加随机后缀思路参考现有
  用例),避免用例间串扰。
- 改 C++ 公开 API 时同步检查 `languages/python_api/test/` 是否需要跟进。

## 4. 运行

- 默认**不运行**测试——CI(`ci-test.yml`,三平台,Linux 含 ASan)与
  维护者 IDE 负责执行。
- 用户明确要求时按 skill 走:`/test`(普通全量或定向)、`/asan`(ASan 全量)、
  `/coverage`(覆盖率)。本地单跑某 suite:
  `<build>/output/bin/vlink-test --test-suite=<名>`(仅限用户要求时)。
