# HIP FastBuffer 插件

要求 64 位 Linux、已安装的 VLink、AMD HIP Runtime 及其运行依赖和支持 IPC 的设备。

```bash
cmake -S examples/fastbuffer/hip -B build/fastbuffer-hip \
  -DCMAKE_PREFIX_PATH=/path/to/vlink -DFASTBUFFER_HIP_ROOT=/opt/rocm
cmake --build build/fastbuffer-hip
```

加载产物 `libfastbuffer_hip.so`：

```bash
export VLINK_FASTBUFFER_PLUGIN=/path/to/libfastbuffer_hip.so
```

`FastBuffer::Config` 的 Default/Device 使用 `hipMalloc`，通过 HIP IPC 共享；
Host 使用锁页内存，Virtual 使用 managed memory，二者序列化为 Host 字节。
Device 不提供 CPU map；通过原生设备地址接入 HIP 算法，或显式 `copy_to_host()`。
插件当前以设备同步等待外部任务，不提供原生分配导入。

核心 Manager 负责跨进程引用计数与最终释放；插件只导出设备标识和原生 IPC 句柄，
GPU payload 保留在 SDK 分配中。生命周期边界见 [上级说明](../README.md)。
厂商接口见 [HIP IPC](https://rocm.docs.amd.com/projects/HIP/en/latest/doxygen/html/group___device.html)。
