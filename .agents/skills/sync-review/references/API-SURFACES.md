# API 与使用面同步

对每个受影响的公开类、方法、函数、枚举、宏、URL 参数或功能入口逐项
核对。

## 注释与文档

- Doxygen 的签名、参数名、默认值、返回值和既有异常说明与实现一致;
  新增成员不漏注释,删除成员不留残余。
- 按 `.agents/REPO-REFERENCE.md` 定位 `doc/` 章节,核对示例、参数表、
  默认值、scheme 和跨章速查表。
- 文档结构或标题变化后,同步检查站内链接、Agent 小节引用和图片说明。

## C API 与 Python API

- C API 的声明、实现、ABI、错误码、所有权说明、C 示例和测试同步。
- `languages/python_api/` 各功能 `.cc` 的绑定与 C++ 签名一致;`vlink.py` 包装、docstring、
  默认参数和导出列表同步。
- 新公开 C++ API 是否需要暴露到 C/Python 必须有明确结论;不暴露时说明
  产品或兼容性理由。
- `languages/python_api/test/` 与 `languages/python_api/examples/` 覆盖对应绑定和推荐用法。

## API、ABI 与 SemVer

- 删除或改名公开符号、修改签名/默认值、改变 enum 既有值、公开 struct
  布局、URL 参数、CLI flag、CMake target、C API 返回码或环境变量名时,
  核对 `doc/15-contributing.md` §15.17 的兼容性分类。
- 破坏性 API/ABI 变化必须与 MAJOR 版本、CHANGELOG 和 PR 说明一致;
  `zerocopy` 线格式即使升级 MAJOR 也不得改动。
- 新增公开符号、enum 末尾值或向后兼容选项至少属于 MINOR;PATCH 只能
  修复问题,不得改变 API/ABI。弃用标记须写明替代 API 和引入版本。
- 未经构建或 ABI 对比不能宣称二进制兼容;只报告静态可证结论。

## 测试与示例

- 新行为有有效断言;既有行为变化时更新旧断言,不得只删除失败用例。
- `test/`、`languages/c_api/test/`、Python 测试与公开接口层级匹配。
- `examples/` 代码和 README 使用当前推荐 API,能体现最新入口、参数和
  错误处理。
- 只静态审计时不得声称示例可编译或测试已通过。

## 变更摘要

重要特性、修复、破坏性变化、弃用和可感知性能变化检查
`CHANGELOG.md`;琐碎内部改动无需堆砌。版本标题变化不能代替内容条目。
