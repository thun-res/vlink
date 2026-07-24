# Bag 数据集测试

支持格式以当前代码为准:`.vdb`、`.vdbx`、`.vcap`、`.vcapx`。仓库当前
没有可直接假定存在的完整 bag fixture;默认只使用用户指定的数据集。
只有用户明确选择录制或合成数据场景时,才可在本次临时目录生成数据集。

## 1. 数据集清单

每个输入至少记录:

- 路径、格式、文件大小和数据集版本/来源说明;
- `vlink-bag info` / `check` 结果;
- 开始/结束时间、URL、消息数、序列化类型;
- 配套 proto/fbs、插件、加密 key 或外部依赖是否齐全;
- 是否允许复制,以及是否包含敏感或生产数据。

不把数据集内容或绝对路径写入仓库。含敏感数据时只报告去标识化统计,
不回显 payload。

## 2. 功能划分

按数据能力选择场景,不要求一个 bag 覆盖全部功能:

| 数据集 | 最低内容 | 主要验证 |
| ------ | -------- | -------- |
| 基础消息 | 至少一个稳定 URL 与若干帧 | info/check/play/clone、Proxy、List/Monitor |
| Protobuf | schema 元数据或配套 `.proto` | eproto、parse CSV/JSON、Foxglove |
| FlatBuffers | schema 元数据或配套 `.fbs` | efbs、parse、Foxglove |
| CameraFrame | 合法图像 payload | Viewer/Player、parse 图像导出 |
| PointCloud | 合法点云字段 | Viewer/Player、parse PCD |
| 时间序列 | 可解析数值字段 | Analyzer、parse CSV/scan |
| 异常副本 | 明确的截断/索引损坏方式 | check/fix/reindex 错误路径 |

## 3. 只读与变异规则

1. 原始数据先做只读 `info`/`check`,记录基线。
   大型数据集默认不对 `info` 加逐帧扫描选项。
2. 用独立临时目录复制后再执行 `tag`、`fix`、`reindex` 或破坏注入;
   每类变异使用单独副本,不得让前一场景污染后一场景。
3. `clone`、`slice`、`scan`、MCAP/RRD 和解析输出都写临时目录。
4. 比较前后 `info`/`check`、话题/帧数/时间范围和目标格式,不能只判断
   退出码或文件存在。
5. 结束后报告临时目录;没有用户许可不删除需要复查的失败样本,也不把
   大型数据集加入 Git。

## 4. 缺失与损坏

- 不存在文件、错误后缀、空文件、截断文件、错误索引分别是不同场景。
- 不从生产数据随意截断构造样本;只对临时副本操作并记录精确变异方式。
- `fix` 只在实现宣称可修复的损坏类型上期待成功;不可修复输入返回失败
  是正确结果。
- 缺 schema、插件或 key 时验证清晰失败/跳过,不得临时猜测类型或关闭
  校验来求通过。
