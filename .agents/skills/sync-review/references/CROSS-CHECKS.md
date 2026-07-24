# 横向易漂移项

全量审计逐项覆盖;增量审计检查受影响项并记录判定依据。

## 文档与展示

- `doc/images/` 的 `.drawio` 与同名 `.png` 是否成对,架构、流程和字段名
  是否与当前代码一致。
- `.github/wiki/index.html` 与 `assets/js/i18n.js` 的三语特性、后端
  数量、scheme、API 片段是否与 `doc/00`、`doc/04` 一致。
- README.md、README.en.md 与 `doc/01-started.md` 的快速上手入口和
  示例是否同步。
- `doc/04-transport.md` 的 scheme、参数和默认值是否对应 `modules/`
  当前解析代码。
- `doc/10-cli-tools.md` 是否覆盖 `cli/` 当前入口和真实 `--help` 参数。

## 计数型传播链

计数以源码与专题 doc 为基准。修改下列事实时逐项核对完整传播链:

- **CLI 工具数量**:README 中英文版、`doc/00-whitepaper.md`、
  `doc/00-overview.md`、`doc/01-started.md` 的 `ENABLE_CLI_*` 列表、
  `doc/10-cli-tools.md`、`doc/14-reference.md`、`cli-tools-overview`、
  `overview-architecture`、`foreword-*` 图源与 PNG、Wiki 三语落地页、
  CHANGELOG。
- **transport 模块数量**:上述传播面中所有适用位置,另加
  `doc/04-transport.md`。
- **序列化类型数量**:`doc/03-serialization.md`、README、
  `doc/14-reference.md`。
- **QoS 预设数量**:`doc/05-qos.md`、`doc/14-reference.md`、CHANGELOG。
- **base 组件数量**:`doc/08-base-library.md` 表格、
  `doc/00-overview.md` 对应表。
- **示例数量或类别**:`doc/01-started.md`、各类别的
  `examples/<类别>/README.md`。
- **环境变量**:`doc/13-integration.md`、`doc/14-reference.md`。
- **CMake 选项**:`doc/01-started.md` §1.4、第一方面向用户的
  `option()` 与 cache 配置。

## 工程与版本

- `doc/01` §1.4 的用户 CMake 选项是否对应第一方 `option()` 与 cache
  配置;排除 `cmake/cpm.cmake` 的上游内部选项。
- CLI 子命令或顶层选项变化时,同步对应
  `cli/<工具>/etc/completions/*.bash` 与 `*.zsh`,并核对两者命令集合。
- 环境变量变化时同步 `doc/13-integration.md`;新增或改名后再核对
  `doc/14-reference.md`。
- CMake 导出 target 变化时同步 `doc/01-started.md` 的目标列表。
- `version.txt` 是否与 `include/vlink/version.h`、`conanfile.py`、
  可选的 `tools/vcpkg/vcpkg.json`、README 徽章和 `CHANGELOG.md`
  一致。
- 安装、包名、目标名或 workflow 入口变化时,检查文档、示例和发布说明
  中的对应名称。

## 引用边界

- 公共 API 的调用约束放在 Doxygen,语义、动机和使用模式放在 `doc/`;
  不把整段 Doxygen 复制进教程。
- `doc/` 内部链接使用相对路径与 Markdown 锚点,不得改成 GitHub 直链或
  项目根绝对路径。

## Agent 索引与规则

- `.agents/FEATURE-INDEX.md` 的代码入口、doc 章节和小节仍存在且语义相符。
- `.agents/REPO-REFERENCE.md` 的目录职责、工程设施和章节映射与仓库一致。
- `AGENTS.md`、`.agents/AI-POLICY.md`、`.agents/README.md`、语言分册与
  skill 不重复维护冲突的规则。
- 每个 skill 的 frontmatter、`agents/openai.yaml` 和 README 清单保持
  同一定位;reference 链接和文件名大小写必须有效。
