# 变更范围与事实基准

## 内容层级

| 内容 | 位置 | 审计定位 |
|---|---|---|
| API 签名与实现行为 | `include/vlink/` 与对应实现 | 代码事实 |
| 产品与架构语义 | `doc/00`–`15` | 文档事实 |
| C/Python API、Doxygen | `languages/c_api/`、`languages/python_api/`、公开头 | 派生面 |
| 测试与示例 | `test/`、`examples/`、`languages/python_api/{test,examples}/` | 验证与用法 |
| 构建、安装与打包 | CMake、`conanfile.py`、`Android.bp`、vcpkg、`packup/**` | 工程派生面 |
| Agent 索引与规则 | `AGENTS.md`、`AI-POLICY.md`、`.agents/` | 路由与约束 |
| Wiki、README、CHANGELOG | 对应文件 | 展示与变更摘要 |
| GitHub 工程与治理 | `.github/**` | CI、发布、模板、Wiki 与仓库治理派生面 |

代码与文档矛盾时不能机械认定一方正确:签名、默认值和实际控制流以代码
为证据;产品承诺与架构意图以 `doc/` 为证据,冲突本身必须报告。

## 增量审计

同时收集提交区间、暂存区、工作树和未跟踪文件:

```bash
git diff --name-only <base>...HEAD
git diff --name-only --cached
git diff --name-only
git ls-files --others --exclude-standard
```

将变更映射到公开符号、功能入口、配置项、文档章节和测试。不能只审改动
文件;其直接派生面即使未出现在 diff 中也必须检查。

## 全量审计

全量审计不依赖 diff,按 `.agents/REPO-REFERENCE.md` 和
`.agents/FEATURE-INDEX.md` 建立功能全集,清点构建打包入口与
`.github/**` 每个文件,逐项核对所有派生面。记录审计基线、实际文件范围
和任何排除项。
