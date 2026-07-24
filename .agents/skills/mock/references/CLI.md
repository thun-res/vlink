# CLI Mock 测试

权威说明见 `doc/10-cli-tools.md` §10.2,实际参数以各 `cli/*/*.cc` 中的
`argparse::ArgumentParser` 为准。先从构建目录确认实际生成的 target,
不得硬编码因可选依赖未构建的工具。

## 1. 公共入口

对每个已构建工具执行 `--help`、`--version` 和一个明确的非法参数。
再覆盖 23 个子命令的 `<tool> <subcommand> --help`:bag 8 个、bench
4 个、check 3 个、eproto 3 个、efbs 3 个、trigger 2 个。构建树只测试
完整 `vlink-*` 名称,安装阶段生成的短别名不在此冒充已覆盖。

| 工具 | 业务场景 |
| ---- | -------- |
| `vlink-info` | 默认输出、`-l` 编译选项 |
| `vlink-check` | `diag`、`env`、`test` |
| `vlink-list` | 默认、`-n`、名称/PID 过滤、进程计数 |
| `vlink-monitor` | `--plain`、URL/关键字过滤、节点/详情模式 |
| `vlink-bag` | `info`、`record`、`play`、`clone`、`check`、`reindex`、`fix`、`tag` |
| `vlink-trigger` | `daemon`、`dump` |
| `vlink-parse` | 实时解析、bag 导出、`slice`、`scan` |
| `vlink-eproto` | `pub`、`sub`、`import` |
| `vlink-efbs` | `pub`、`sub`、`import` |
| `vlink-bench` | `run`、`plot`、`pub`、`sub` 的参数入口 |

帮助、版本和非法参数只验证解析层。对每个子进程仅设置
`VLINK_LOG_FILE_LEVEL=OFF`,避免帮助命令创建文件日志;不得永久修改
调用者环境。业务场景必须另行运行并核对退出码、输出语义或产物。

## 2. 无数据集场景

- `vlink-info` 核对版本与 `version.txt`;`-l` 至少包含当前构建实际启用
  的目标开关。
- `vlink-check env` 会输出环境值,执行前确认日志不会泄露配置;`diag`
  会运行系统检查并创建可写性探针,两者都不属于无副作用参数检查。
  `vlink-check test` 属真实通信冒烟,记录被跳过的后端。
- `vlink-list -n` 在无生产者和有测试生产者两种状态运行;进程计数退出码
  是数据结果,不能机械当作执行失败。
- `vlink-monitor` 使用 `--plain` 并由本次启动的生产者提供有限数据;
  交互热键不在纯文本场景内伪称已覆盖。
- `vlink-eproto` / `vlink-efbs` 使用临时 schema 目录和本机可跨进程的
  后端 URL 配对运行 `pub`/`sub`;两个独立 CLI 进程不能用 `intra://`
  互通。`import` 会写用户配置,只有隔离其持久化位置后才能执行。
- `vlink-bench` 的性能执行交给 `/bench`;本分册只验证帮助、非法参数及
  用已有 JSON 执行 `plot` 的入口。没有合法 JSON 时标为未覆盖。

## 3. Bag 驱动场景

先按 [BAG-DATASETS.md](BAG-DATASETS.md) 建立只读输入和临时副本:

- 原始数据仅执行 `vlink-bag info` 与 `check`。
- `clone` 的目标、`parse` 的 CSV/JSON/bin/图像/点云输出、`slice`/`scan`
  产物全部写临时目录,并验证文件存在且内容非空。
- `reindex`、`fix`、`tag` 只对专门的临时副本执行,完成后再次
  `info`/`check`;不得拿健康原件冒充损坏数据验证 `fix`。
- `play` 与 `parse` 的 URL 必须来自 `vlink-bag info` 实际元数据,
  不猜 topic、序列化类型或字段路径。`play` 会向 bag 内原始 scheme
  真实发布;逐项审计 URL 并获得实时集成授权前不得执行。
- `record` 由本次测试生产者发布有限消息,输出到临时 bag;停止后执行
  `info`/`check`,核对话题、帧数和时间范围。
- `vlink-trigger daemon` 使用临时配置和输出目录,再由 `dump` 触发;
  核对最终 bag 后正常终止 daemon。没有真实消息时不得声称前后窗口
  数据已验证。

## 4. 退出与副作用

长驻命令必须等待明确就绪条件,设置有限测试窗口并保存日志。只终止本次
记录的 PID,禁止 `killall` / `taskkill /IM`。涉及网络的 URL 默认限制在
本机;SOME/IP、MQTT、FDBus 等需要外部服务的后端未获得用户授权和环境
说明时只报告未覆盖。顶层无子命令的返回行为并不统一,不得批量断言同一
退出码;`vlink-list -c` 和 `vlink-check test` 的非零值也可能是数据结果。
