# Example 4：多层级 TOML 嵌套编排（`[[include]]` + 嵌套参数表）

本示例演示 VLink Launcher 对 **多级递归文件嵌套（`[[include]]`）** 与 **多层嵌套参数表（`[params.*]`）** 的完整支持，直接复用 `demo_launcher_components` 插件库运行真实数据流。

## 嵌套目录结构（3 层递归包含）

```text
nested_includes/
├── system_root.toml                      # 第 1 层：根清单（namespace = "fleet"、全局 [urls]/[params.safety]/[[scheduler]]）
└── subsystems/
    ├── diagnostics.toml                  # 第 2 层：条件包含（condition = "${arg.enable_diagnostics} == true"）
    ├── control.toml                      # 第 2 层：接收 [include.args] 字符串+浮点传参，含嵌套 [component.params.pid]
    ├── perception.toml                   # 第 2 层：跨子树绝对依赖 /fleet/control/controller + for_each 包含第 3 层
    └── sensors/
        └── camera_unit.toml              # 第 3 层：可复用叶子模板（展开为 fleet/perception/cam_<id>/sensor）
```

## 覆盖的嵌套特性

1. **多级递归 `[[include]]`**（最多支持 16 层，自动检测循环包含）：`system_root.toml` → `subsystems/perception.toml` → `subsystems/sensors/camera_unit.toml`。
2. **命名空间逐级叠加**：根 `fleet` + 第 2 层 `perception` + 第 3 层 `cam_${item}` 自动生成 `fleet/perception/cam_front/sensor`、`fleet/perception/cam_rear/sensor`。
3. **`for_each` + `[include.args]` 模板批量展开**：支持向子清单传递字符串、整数、浮点数与布尔参数（如 `--arg cameras=front,rear,left` 动态扩充为 3 路相机）。
4. **条件包含 `condition`**：通过 `--arg enable_diagnostics=false` 动态裁剪整个 `diagnostics.toml` 子系统。
5. **跨子树绝对路径依赖与规范 URL 共享**：子清单中用 `/fleet/control/controller` 与 `/fleet/perception/fusion` 引用兄弟命名空间中的单元，并用 `${arg.transport}:///fleet/...`（`://` 后带前导 `/` 跳过当前单元 `ns` 自动前缀）让 3 层嵌套内的传感器、融合、控制与监控共享同一套规范话题。
6. **多层嵌套 `[params]` 子表**：`[params.safety]`、`[component.params.pid]`、`[component.params.filter]`、`[component.params.calibration]` 自动展平为点分键（`safety.max_speed_mps`、`pid.kp`、`filter.alpha`、`calibration.gain`），组件可直接用 `ctx.get_param_double("pid.kp")` 读取，也可通过 `vlink-launcher ctl params` 查询或在线修改。

## 运行步骤（在 `build/output/bin` 目录）

```bash
# 1. 静态校验与导出 3 层展开后的依赖图 & 数据流拓扑
./vlink-launcher validate nested_includes/system_root.toml
./vlink-launcher graph nested_includes/system_root.toml --format ascii

# 2. 动态扩展为 3 路相机并关闭诊断子系统，查看展开结果
./vlink-launcher inspect nested_includes/system_root.toml \
  --arg cameras=front,rear,left --arg enable_diagnostics=false --json

# 3. 实际启动完整 3 层嵌套系统
./vlink-launcher run nested_includes/system_root.toml --arg cameras=front,rear

# 4. 另开终端查看嵌套命名空间单元状态与展平后的嵌套参数
./vlink-launcher ctl --namespace nested_fleet status
./vlink-launcher ctl --namespace nested_fleet params fleet/control/controller
./vlink-launcher ctl --namespace nested_fleet params fleet/perception/cam_front/sensor
```
