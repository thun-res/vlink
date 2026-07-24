# WebViz Mock 测试

权威说明见 `doc/11-visualization.md` §11.2。`ENABLE_WEBVIZ=ON` 下按功能
选择 Foxglove 或 Rerun,不要同时启用不需要的重依赖。

## 1. Foxglove

目标:`vlink-foxglove`、`vlink-bag2mcap`。

- 对两个入口执行 `--help`/`--version` 和非法参数。
- `vlink-foxglove` 绑定 `127.0.0.1` 与独立空闲端口,使用
  `--allow_multiple`;通过监听端口和 WebSocket/Foxglove 客户端握手
  分别验证启动与协议,不能只看进程存活。
- 不直接套用仓库默认配置做安全 smoke;其中的 publish、RPC、parameters
  能力必须按本次明确场景开启。
- 使用本次生产者或 `vlink-bag play` 提供消息,核对实际 schema、channel
  和至少一帧数据。需要 proto/fbs/config 时只使用用户指定或数据集配套
  文件。
- `vlink-bag2mcap <input> -o <临时输出.mcap>` 成功后核对 MCAP 文件
  非空,并用可用的读取工具验证 channel/message;缺少读取工具时明确
  只完成产物级检查。

## 2. Rerun

目标:`vlink-rerun`、`vlink-bag2rrd`;需
`ENABLE_WEBVIZ_RERUN=ON` 与实际 Rerun SDK。

- 对两个入口执行 `--help`/`--version` 和非法参数。
- 优先使用 `save` 模式写临时 `.rrd`,避免自动拉起 GUI;核对文件非空及
  实际记录的实体/时间线。`spawn` / `connect` / `serve` 仅在对应 Rerun
  环境已提供时执行。
- `vlink-bag2rrd <input> -o <临时输出.rrd>` 成功后使用配套工具读取
  并核对实体;没有读取能力时不得声称内容正确。

## 3. Proxy 与数据集组合

实时桥接路径为:

```text
测试生产者或 vlink-bag play → vlink-proxy / local proxy mode
                              → vlink-foxglove 或 vlink-rerun
                              → 客户端/文件产物
```

分别记录每一段的就绪和数据证据。绑定地址默认只用 loopback;不自动打开
防火墙、不连接公网 Studio、不上传 MCAP/RRD。端口冲突时换本次独立端口,
不终止占用端口的其他进程。
