# 🔒 qos_basics — 自定义 `vlink::Qos`、注册命名 profile、URL 引用

构造 `vlink::Qos` 策略，经后端 `register_qos(name, qos)` 注册为命名 profile，在 URL 中以 `?qos=name` 引用。仅 DDS 家族（`dds://` / `ddsc://` / `ddsr://` / `ddst://`）与 `zenoh://` 生效；`intra` / `shm` / `someip` / `mqtt` / `fdbus` / `qnx` 忽略 QoS。

![QoS 配置数据流](./images/qos-basic-config.png)

## 🧩 数据流

- `vlink::Qos` 聚合若干正交子策略：reliability / history / durability / publish_mode / deadline / lifespan / resource_limits。
- 默认构造的 `Qos` 是 `valid=false`，注册时会被忽略；必须先 `valid=true` 才生效。
- 子策略字段都有默认值，只填要改的几项即可；profile 按后端注册、按名引用，名字表互相独立。

## 📋 核心 API

| API | 作用 |
|-----|------|
| `vlink::Qos qos;` | 策略对象；先设 `qos.valid = true` 再注册 |
| `qos.name` (`char[20]`) | 仅供日志/展示；URL 查找键是 `register_qos(name, qos)` 的 `name` 参数 |
| `qos.reliability.kind` | `kBestEffort`（丢包不重传）/ `kReliable`（重传） |
| `qos.history.kind` / `.depth` | `kKeepLast`(保留最近 depth 条) / `kKeepAll` |
| `qos.durability.kind` | `kVolatile` / `kTransientLocal`（补送晚加入者）/ … |
| `qos.publish_mode.kind` | `kSync`（阻塞到下发）/ `kASync`（入队即返回） |
| `DdsConf::register_qos(name, qos)` | 注册命名 profile（每个 DDS/Zenoh 后端各有一个） |
| URL `?qos=<name>` | 在 topic URL 上引用已注册的 profile |

## 🚀 最小示例

```cpp
// sensor profile：高吞吐、可丢、不补送
vlink::Qos sensor_qos;
std::strncpy(sensor_qos.name, "custom_sensor", sizeof(sensor_qos.name) - 1);
sensor_qos.valid = true;
sensor_qos.reliability.kind = vlink::Qos::Reliability::kBestEffort;
sensor_qos.history.kind = vlink::Qos::History::kKeepLast;
sensor_qos.history.depth = 5;
sensor_qos.durability.kind = vlink::Qos::Durability::kVolatile;
sensor_qos.publish_mode.kind = vlink::Qos::PublishMode::kASync;

vlink::DdsConf::register_qos("custom_sensor", sensor_qos);

vlink::Publisher<std::string> pub("dds://sensor/lidar_data?qos=custom_sensor");
vlink::Subscriber<std::string> sub("dds://sensor/lidar_data?qos=custom_sensor");
```

```cpp
// command profile：启用可靠重传，并用 transient-local 补送晚加入者
vlink::Qos cmd_qos;
std::strncpy(cmd_qos.name, "custom_command", sizeof(cmd_qos.name) - 1);
cmd_qos.valid = true;
cmd_qos.reliability.kind = vlink::Qos::Reliability::kReliable;
cmd_qos.history.kind = vlink::Qos::History::kKeepAll;
cmd_qos.durability.kind = vlink::Qos::Durability::kTransientLocal;
cmd_qos.publish_mode.kind = vlink::Qos::PublishMode::kSync;

vlink::DdsConf::register_qos("custom_command", cmd_qos);
vlink::Server<std::string, std::string> server("dds://control/brake?qos=custom_command");
```

`history.depth` 越大越占内存，量级约 `topics × depth × 单条消息大小`：当前状态用 1，控制流用 5~10，传感器流用 20~50。

## 🧭 选型判据

- 传感器 / 高频流：`BestEffort + KeepLast(浅) + Volatile + ASync`，吞吐优先、可丢帧。
- 控制命令 / 关键事件：`Reliable + KeepAll + TransientLocal + Sync`，以可靠投递和晚加入同步为优先；仍须配置资源上限并处理发布失败与网络故障。
- 多数业务无需手写 Qos，可直接引用内置 `QosProfile::*` 预设（URL 写 `?qos=sensor` 等），其字段对照见顶层 `doc/05-qos.md`。

## ▶️ 运行

```bash
./build/output/bin/example_qos_basics
```

未启用 FastDDS 组件时打印 `DDS module not available; skipping registration.` 并跳过 DDS 段。

## 📚 参考

- 顶层 `doc/05-qos.md` — QoS 全部子策略、各后端映射规范，及内置 `QosProfile::*` 预设（kEvent / kMethod / kField / kSensor 等）
- `include/vlink/extension/qos.h` — `Qos` 结构定义
- `include/vlink/extension/qos_profile.h` — 内置预设
- `include/vlink/modules/dds_conf.h` — `DdsConf::register_qos` 接口
