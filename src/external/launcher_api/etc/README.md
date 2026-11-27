# VLink Launcher 配置与运行速查

`vlink-launcher`（软链 `launcher`）是 VLink 的轻量进程与组件编排 CLI。

## 常用命令

```bash
# 运行编排清单（支持参数覆盖、--only/--skip 子集筛选、--dry-run 预览）
vlink-launcher run launcher_example.toml [--arg k=v]... [--only target]... [--skip target]... \
  [--dry-run] [--scheme dds|ddsc|ddsr|zenoh|intra] [--ns launcher] [--domain 0] [--no-ctrl] [--no-respawn]

# 静态校验（可选校验可执行文件存在性与可执行权限）
vlink-launcher validate launcher_example.toml [--arg k=v]... [--check-programs]

# 查看插件导出的 LauncherComponent 类名
vlink-launcher plugin libmy_node_plugin.so

# 零 main() 独立进程运行任意 LauncherComponent 插件
vlink-launcher exec libmy_node_plugin.so [ClassName] [--name node1] [--ns /robot] [--param rate_hz=20]

# 运行期控制与实时观测
vlink-launcher status [--scheme ...] [--ns ...] [--domain ...] [--timeout 3000]
vlink-launcher events [--scheme ...] [--ns ...] [--domain ...]
vlink-launcher start|stop|restart|reload <unit|@group> [ctrl opts]
vlink-launcher params [unit] [ctrl opts]
vlink-launcher set-param <unit> <key=value> [ctrl opts]
vlink-launcher shutdown [ctrl opts]
```

控制面加密密钥建议通过环境变量 `VLINK_LAUNCHER_SECURITY_KEY` 传入（或 `--key`），避免长期暴露在命令行历史中。

## 参考模板

- [launcher_example.toml](launcher_example.toml)：涵盖 `[args]`、多层嵌套 `includes` / `[[include]]`、`[[component]]` 与 `[[process]]` 的完整最小示例（运行前请替换为实际插件目录与可执行文件路径）。
- 完整架构说明见 `doc/11-launcher.md`。
