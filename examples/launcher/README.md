# Launcher 节点复用示例

演示核心理念：**Write Once, Run In-Process or Standalone**。

同一份 [sensor_node.h](sensor_node.h)（继承 `vlink::LauncherComponent`，含 `on_start`/`on_stop`/`on_reload`/`on_param_changed`）通过 1 行胶水代码分别构建为：

1. **同进程组件插件**（[sensor_plugin.cc](sensor_plugin.cc) -> `liblauncher_demo_sensor_plugin.so`）：`VLINK_LAUNCHER_EXPORT_SINGLE_COMPONENT(SensorNode)`。
2. **独立进程可执行文件**（[sensor_standalone.cc](sensor_standalone.cc) -> `launcher_demo_sensor_node`）：`VLINK_LAUNCHER_COMPONENT_MAIN(SensorNode)`，仅链接 `vlink::launcher_api`。

## 构建与运行

```bash
cmake -B build -DENABLE_EXAMPLES=ON -DENABLE_EXAMPLES_ALL=ON -DENABLE_LAUNCHER=ON
cmake --build build --target vlink-launcher launcher_demo_sensor_plugin launcher_demo_sensor_node

# 查看插件导出的组件类
./build/output/bin/vlink-launcher plugin ./build/output/lib/liblauncher_demo_sensor_plugin.so

# 预览解析后的 DAG 启动顺序
./build/output/bin/vlink-launcher run ./build/output/bin/launcher_demo.toml --dry-run

# 模式 1 + 模式 2 同栈编排运行（编译后已自动拷贝 launcher_demo.toml 至 output/bin/）
./build/output/bin/vlink-launcher run ./build/output/bin/launcher_demo.toml

# 另开终端控制与观测
./build/output/bin/vlink-launcher status
./build/output/bin/vlink-launcher set-param sensor_inproc rate_hz=10
./build/output/bin/vlink-launcher reload @sensors
./build/output/bin/vlink-launcher events

# 模式 3：零 main() 直接独立运行插件 .so
./build/output/bin/vlink-launcher exec ./build/output/lib/liblauncher_demo_sensor_plugin.so SensorNode \
  --name lidar_exec --param rate_hz=10 --param url=dds://sensor/exec/event/0
```
