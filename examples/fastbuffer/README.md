# FastBuffer 插件

三个目录各自包含完整插件源码和独立 CMake 工程，没有公共实现头文件或额外示例程序。

| 目录 | SDK | 默认分配 | 跨进程共享 |
| --- | --- | --- | --- |
| [cuda](cuda) | CUDA Runtime | 设备内存 | CUDA IPC |
| [hip](hip) | AMD HIP | 设备内存 | HIP IPC |
| [hbmem](hbmem) | 地平线 hbmem | 共享内存 | hbmem 导入句柄 |

在 VLink 主工程中开启 `ENABLE_EXAMPLES`、`ENABLE_EXAMPLES_ALL`，再按需选择
`ENABLE_FASTBUFFER_CUDA`、`ENABLE_FASTBUFFER_HIP`、`ENABLE_FASTBUFFER_HBMEM`。
三个开关默认关闭；只查找被选中插件的 SDK。也可直接构建各子目录。

节点及 CLI 在启动前设置同一个 `VLINK_FASTBUFFER_PLUGIN`，无需链接厂商 SDK：

```bash
export VLINK_FASTBUFFER_PLUGIN=/path/to/libfastbuffer_hip.so
```

未设置时使用普通 CPU 内存，`shm` 使用可跨进程共享的 CPU 内存。三个插件都由核心的共享内存控制块协调跨进程生命周期：generation 加读者进程身份表，发布方通过 `FastBufferPool` 复用缓冲，任何调用都不等待其他进程；hbmem 的 SDK 引用只维持导入映射的存活。共享生命周期见
[FastBufferPluginInterface](../../include/vlink/zerocopy/fast_buffer_plugin_interface.h)。

FastBuffer 不内置录制转换；需要时由独立 bag 插件显式读取并转换为可持久化消息。
接口、64 位对象布局、元数据及访问语义见 [零拷贝 §6.13](../../doc/06-zerocopy.md#-613-fastbuffer-插件缓冲区)。
基础功能测试位于 `test/zerocopy/fast_buffer_test.cc`。
