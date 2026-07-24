# GitHub 工程、展示与治理同步

全量审计清点 `.github/**` 每个文件;增量审计从变更文件沿调用、触发、
产物和文档关系检查直接派生面。文件存在不等于同步完成,必须核对字段、
参数、默认值、权限和消费者。

## 1. 工作流

逐个核对 `.github/workflows/*.yml`:

- `name`、事件类型、branch、tag、`paths`/`paths-ignore` 与实际用途一致;
  新入口能触发,纯文档或 Agent 改动不会被错误跳过。
- `workflow_dispatch`、`workflow_call` 的 input 类型、默认值、required、
  secret、output 与调用方传参逐项一致。
- 顶层和 job 级 `permissions` 遵循最小权限;写 Issue、PR、Packages、
  Pages、Release、OIDC 或 attestation 的 job 才获得对应权限。
- `concurrency` 的 group 和 `cancel-in-progress` 不会让不同 PR、branch、
  tag 或发布互相取消。
- `if`、`needs`、matrix、runner、timeout、environment 和 shell 覆盖全部
  支持平台,失败/跳过条件不会绕过必要门禁。
- checkout 的 ref、fetch-depth、submodule、LFS 和 credential 行为满足
  后续 diff、tag、构建或发布需求。
- action 版本、容器镜像、cache key、artifact 名称、retention、Pages
  路径、GHCR tag 和 release asset 名称与生产者、消费者一致。
- 每个 `run` 调用的脚本、参数、环境变量和工作目录真实存在;脚本返回码
  能传回 workflow,不得由后续成功命令掩盖。
- reusable workflow 的 `uses`、input、secret、output、artifact 传播链
  完整;`release.yml` 与各 `release-*`、coverage、docker 子流程一致。
- `ci-agent-skills.yml` 覆盖 `AGENTS.md`、`AI-POLICY.md`、`.agents/**`、
  安装/校验入口和自身,只授予 `contents: read`,不得自动创建社区内容。

新增、删除、改名或改变触发语义后,同步 `.agents/CI-AND-PR.md`、
`.agents/REPO-REFERENCE.md`、`/cicd` skill 触发矩阵及适用的贡献文档。

## 2. 工作流脚本

逐个核对 `.github/scripts/**` 及 workflow 调用关系:

- 每个被调用脚本的路径、参数顺序、默认值、env、secret 名称和退出码与
  workflow 一致;未被调用脚本确认是人工入口、复用资源还是陈旧文件。
- Linux/macOS Shell 与 Windows PowerShell/CMD 的同类流程在功能、
  artifact 布局、错误传播和清理边界上保持对应,平台特有差异有事实依据。
- `github-progress.sh`/`.ps1` 的阶段数量、名称和执行调用一致;失败步骤
  保留原始退出码。
- 下载、checksum、解压、LFS、SDK、runtime closure、打包和通知链使用
  同一文件名、目录、架构、版本和产物约定。
- 临时文件只清理自身精确目标;日志和通知不得输出 token、secret、私钥
  或带凭据 URL。
- Python 通知脚本的参数、环境变量、编码和依赖与调用 workflow 一致。

静态审计不得宣称脚本已在 runner 上通过;只有实际执行对应入口后才能
报告动态结果。

## 3. Docker、缓存与发布产物

- `.github/docker/Dockerfile` 的 base image、工具链、包、用户、工作目录
  和入口与 `docker-images.yml`、`docker-run.sh` 及 CI env 一致。
- 平台/架构 matrix、镜像 tag、GHCR 路径、cache scope 和 latest 移动
  规则一致,不遗漏 amd64/arm64 或受支持系统。
- release workflow、`conanfile.py`、`cmake/package.cmake`、`packup/**`、
  CHANGELOG、版本、tag、Release asset、checksum、Pages 文档和通知中的
  名称与目录完全对应。
- workflow 或脚本的发布写操作必须保持现有人工授权和 protected
  environment 边界;同步审计不得自行 dispatch 或发布。

## 4. Issue、PR、评审与所有权

- `.github/ISSUE_TEMPLATE/*.yml` 的标题前缀、必填字段、选项、label 和
  contact link 与当前组件、平台、版本、transport/serialization 事实
  一致;模板引用的 label 应在仓库存在。
- `config.yml` 的 blank issue 和 Discussions 路由符合维护流程。
- `.github/PULL_REQUEST_TEMPLATE.md` 的章节、检查项和实际门禁与
  `/pr`、`/commit`、`doc/15`、CI workflow 一致。
- `CODEOWNERS` 的路径真实存在,owner 有效,关键 workflow、发布、API 和
  Agent 规则范围没有因目录改名失去覆盖。
- `ai-code-review.yml` 的事件、fork/draft/bot 过滤、权限、checkout、
  prompt、工具 allowlist、评论方式和超时与实际评审行为一致;PR 内容按
  不可信输入处理。
- `community-ai-reply.yml` 由 Issue/Discussion 标题、正文与评论中的
  `@codex`/`@claude` 及 PR 普通评论中的 `@codex` 触发;核对生成 job
  始终从事件固定的默认分支提交执行受信 helper、PR 独立发布 job、
  PR Claude 过滤、官方 `@codex review` 互斥、Bot/自身回复过滤、
  编辑幂等、`author_association` 可信角色门槛、非真人/AI 内容判断、
  当前 mention 与身份回读、生成/发布正文摘要一致性、上下文分页/清洗/
  限长及不可信输入边界。PR 裸 mention 必须使用 thread-only prompt,
  不得声称读取 PR diff;长线程必须保留标题、正文和最近上下文。
- Codex/Claude 凭据只进入各自只读生成 job;发布 job 不接触模型凭据,
  且只按目标获得 `issues: write`、`pull-requests: write` 或
  `discussions: write`。回复必须披露模型来源,幂等 marker 只信任
  `github-actions[bot]`,发布后核对正文、目标和 Discussion `replyTo`。
- Claude 顶层 action 当前不识别 Discussion 事件;Discussion job 只能
  使用同一固定提交的 base action,必须 `--bare`、`--tools ""`、禁用
  slash command/MCP 并使用 thread-only prompt,不得向不可信内容开放工具。
- `/issue` 与 `/discussion` skill 的授权、去重、模板/分类、创建、单条
  人工回复、mention 自动回复和回读流程与 GitHub 当前配置一致,不得
  伪装身份、批量回复或制造虚假互动。

## 5. Wiki 与前端资产

- `wiki/index.html`、CSS、JS、i18n、图片和 SVG 的引用均存在;DOM id、
  class、翻译 key 和事件绑定相互对应。
- 中文、英文、日文 key 集相同,无缺失、孤儿 key 或回退到错误语言。
- 后端、CLI、序列化、QoS、示例等计数与 `doc/` 专题事实一致;API 代码
  片段、scheme、参数、性能口径和兼容性描述不过时。
- SVG/图片的文本、模块名和数据流与当前架构一致;删除资产没有残余引用,
  新增资产已被实际消费。
- 外链、下载、文档和仓库 URL 指向有效目标;不写入本地绝对路径或凭据。

## 6. 验证与证据

在授权范围内优先执行:

- workflow:已安装的 `actionlint`;缺失时只做 YAML 语法和静态字段核对,
  明确报告未运行 actionlint。
- Shell/PowerShell/Python:对应语法检查;运行行为仍需维护者明确授权。
- 模板、CODEOWNERS、workflow 和脚本:相对路径、调用目标、label、分类、
  input/output、artifact 与 secret 名称的双向引用检查。
- Wiki:HTML/CSS/JS 资源、DOM/i18n key、三语集合和外链的静态检查。

每条发现同时给出 `.github` 事实侧和代码、doc、Agent、workflow 或脚本
派生侧证据;未执行的 runner、发布、通知、浏览器和外部写入验证单列。
