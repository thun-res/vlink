# Viewer Mock 测试

权威说明见 `doc/11-visualization.md` §11.1。`ENABLE_VIEWER=ON` 后构建
`vlink-viewer`、`vlink-player`、`vlink-analyzer`;Qt、Protobuf、
FlatBuffers 及可选 FFmpeg/OSG 依赖以 `viewer/CMakeLists.txt` 为准。

## 1. 环境分层

- **无图形会话**:只验证 target 构建和动态库闭包,不得用进程短暂存活
  冒充 GUI 通过。
- **可用图形会话**:在隔离的用户配置目录启动,同时隔离 QSettings 与
  `$HOME/.vlink_proto_dir` / `.vlink_fbs_dir`,防止覆盖维护者配置。
  记录显示后端、Qt 版本、FFmpeg/OSG 开关和截图或窗口日志。
- 不自行安装 Xvfb、Qt 插件、显卡驱动或 OSG 资产;缺失即报告未覆盖。

## 2. vlink-viewer

`viewer/main.cc` 没有项目自定义 argparse 入口,启动后先进入设置对话框;
不要机械套用 CLI 的 `--help`/`--version` 判定。

依次验证:

1. 设置对话框能创建并取消,取消退出码为 0。
2. Controller/Listener 按用户指定 domain 启动,同 domain Controller
   singleton 行为符合预期。
3. 由本次生产者或 bag 回放提供图像、点云、目标等实际存在的 URL,
   核对列表发现、选中、首帧渲染和关闭。
4. 没有对应消息类型的数据集时只验证实际具备的视图,不伪造全类型覆盖。

## 3. vlink-player / vlink-analyzer

两者接受第一个位置参数作为 bag 路径。使用只读原始数据集:

- 合法 `.vdb/.vdbx/.vcap/.vcapx`:核对加载完成、元数据/话题列表、
  时间轴和首个可显示数据;Player 再验证播放/暂停/定位,Analyzer 验证
  实际存在字段的曲线或导出入口。
- 不存在路径、错误后缀、损坏副本、缺 schema 数据分别核对错误提示,
  不得在原始数据上制造损坏。
- 应用可能写用户设置,必须使用本次临时配置目录;bag 本体保持只读。

## 4. 进程清理

Viewer 相关代码存在终止其他 `vlink-bag` 进程的历史逻辑。测试前不得让
无关的用户 `vlink-bag` 与 Player 共用环境;清理时只关闭本次 GUI 和本次
回放进程,不得执行全局进程名终止命令。
