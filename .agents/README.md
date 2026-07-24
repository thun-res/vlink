# .agents 路由表

总入口是仓库根 `AGENTS.md`。本表按任务类型指明要加载哪些分册——
**只加载与当前改动相关的**,不要全量读取。

## 1. 按任务加载

| 你要做的事 | 先加载 |
| ---------- | ------ |
| 任何任务 | 根 `AGENTS.md` |
| 实现、修改、评审、验证、提交或发布 | `AI-POLICY.md` + 当前任务分册 |
| 浏览仓库 / 定位某段代码 | `REPO-REFERENCE.md` |
| 找某个功能的代码入口与文档 | `FEATURE-INDEX.md` |
| 写/改任何 C++ 代码 | `languages/CPP.md`(全文 13 节) |
| 写/改纯 C 或公开 C API | `languages/C.md` + `languages/CPP.md` 的上位约束 |
| 写/改 Shell 脚本 | `languages/SHELL.md` |
| 写/改 Batch 脚本 | `languages/BATCH.md` |
| 写/改 PowerShell 脚本 | `languages/POWERSHELL.md` |
| 写/改 CMake | `languages/CMAKE.md` |
| 写/改 Python / nanobind 导出 | `languages/PYTHON.md` |
| 写/改 Doxygen 注释 | `languages/CPP.md` §10 |
| 改 `doc/`、README、Wiki、drawio 插图 | `DOCS-AND-FORMATTING.md` |
| 在 `test/` 下新增/修改测试 | `testing/UNIT-TESTS.md` |
| 代码评审 / 提 PR / 查 CI | `CI-AND-PR.md` |
| 改 GitHub Actions workflow / Dockerfile | `CI-AND-PR.md` §4 + `REPO-REFERENCE.md` |
| 改 Qt `.ui` / Wiki HTML、CSS、JS | `DOCS-AND-FORMATTING.md` §7 + 相邻文件 |
| 改公开 API、功能入口或文档结构(同步义务) | `/sync-review` |
| 协议/传输后端级改动 | `REPO-REFERENCE.md` + `languages/CPP.md` + 对应 doc 章节 |

## 2. 分册清单

| 文件 | 内容 |
| ---- | ---- |
| [AI-POLICY.md](AI-POLICY.md) | AI 贡献责任、验证与复核政策 |
| [REPO-REFERENCE.md](REPO-REFERENCE.md) | 目录 → 职责 → `doc/` 章节映射;`doc/` 章节总表;工程设施(lint/CI/脚本/版本);相关仓库 |
| [FEATURE-INDEX.md](FEATURE-INDEX.md) | 全功能平铺索引(功能 → 代码入口 → doc 小节,渐进式披露) |
| [languages/CPP.md](languages/CPP.md) | C++ 规范:克制、复用、许可证、命名、内存、并发、多平台、性能、注释、Lint 与流程 |
| [languages/C.md](languages/C.md) | 纯 C 的 ABI、类型与内存、错误处理、回调并发和同步义务 |
| [languages/SHELL.md](languages/SHELL.md) | Shell 版式、参数/路径、错误传播、清理、缓存、安全和隔离测试 |
| [languages/BATCH.md](languages/BATCH.md) | Batch 命名、参数/路径、子程序、errorlevel、延迟展开和安全 |
| [languages/POWERSHELL.md](languages/POWERSHELL.md) | PowerShell 参数、编码、错误传播、外部命令与平台兼容 |
| [languages/CMAKE.md](languages/CMAKE.md) | CMake target 作用域、命名、平台、安装/导出和 cmake-format |
| [languages/PYTHON.md](languages/PYTHON.md) | Python 风格、nanobind 导出同步、所有权、异常和测试 |
| [DOCS-AND-FORMATTING.md](DOCS-AND-FORMATTING.md) | 文档语言与行文、emoji 场景、Doxygen 指向、drawio 插图风格与导出、wiki 落地页同步 |
| [testing/UNIT-TESTS.md](testing/UNIT-TESTS.md) | doctest 布局、NOLINT 包裹、用例习惯、运行策略 |
| [CI-AND-PR.md](CI-AND-PR.md) | 评审语言与重点、PR/commit 规范指向、CI 工作流与排查 |

## 3. 可执行 Skill（`.agents/skills/`，按任务触发）

首次使用执行 `bash tools/install_skills.sh`,将 `.agents/skills` 链接到
`.claude/skills` 与 `.codex/skills`,并创建
`CLAUDE.md -> AGENTS.md`(均被 gitignore)。skill 目录不支持符号链接的
环境可用 `--copy` 安装 skills;`CLAUDE.md` 始终要求真实符号链接。
Git Bash 必须先启用原生符号链接;目标不是本脚本安装的内容时拒绝覆盖。
link 模式会立即看到新增 skill;copy 模式是安装时快照,skill 变化后需
重新运行脚本刷新。已启动的 Agent 会话不保证热刷新 skill 清单;首次
安装或新增 skill 后若未显示,重新加载或开启新会话。

| Skill | 用途 |
| ----- | ---- |
| `/format` | `tools/format.sh`:clang-format + cmake-format + 行尾符 |
| `/check` | `tools/check.sh`:cpplint(120 列,项目过滤集) |
| `/clang-tidy` | 指定文件或全仓 clang-tidy(WarningsAsErrors) |
| `/test` | 按功能运行普通 `vlink-test` 全量/定向测试并诊断失败 |
| `/mock` | 按功能测试 CLI、Proxy、Viewer、WebViz 与 bag 数据集场景 |
| `/asan` | `ENABLE_TEST_SANITIZE=ON` 构建并跑 ASan 单元测试 |
| `/coverage` | `ENABLE_TEST_COVERAGE=ON` 生成 lcov 覆盖率报告 |
| `/bench` | `vlink-bench` 性能基准(showcase/quick/full) |
| `/deep-review` | 按范围、审查维度、对抗复核与报告分工执行重度评审 |
| `/sync-review` | 按 API 使用面和横向易漂移项执行全仓同步审计 |
| `/report` | 结合仓库与当前行业证据分析 VLink 定位、前景和建议 |
| `/commit` | 强制通过 `/format` 与 `/check` 后,按模块/功能拆分并创建规范 commit |
| `/pr` | 提交 dev 到 master,用中文填写云端内容并创建或更新 PR |
| `/release` | 在 master 校验版本和 CI,用中文填写云端内容并发布 Release |
| `/cicd` | gh CLI:dispatch 工作流、查看状态与日志、重跑 CI |
