# CUDA FastBuffer 插件

要求 64 位 Linux、已安装的 VLink、CUDA Toolkit 和支持 CUDA IPC 的设备。

```bash
cmake -S examples/fastbuffer/cuda -B build/fastbuffer-cuda \
  -DCMAKE_PREFIX_PATH=/path/to/vlink -DCUDAToolkit_ROOT=/path/to/cuda
cmake --build build/fastbuffer-cuda
```

加载产物 `libfastbuffer_cuda.so`：

```bash
export VLINK_FASTBUFFER_PLUGIN=/path/to/libfastbuffer_cuda.so
```

`FastBuffer::Config` 的 Default/Device 使用 `cudaMalloc`，通过 CUDA IPC 共享；
Host 使用锁页内存，Virtual 使用 managed memory，二者序列化为 Host 字节。
Device 不提供 CPU map；通过原生设备地址接入 CUDA 算法，或显式 `copy_to_host()`。
插件当前以设备同步等待外部任务，不提供原生分配导入。

核心 Manager 负责跨进程读者表、generation 与最终释放；插件只导出设备标识和原生 IPC 句柄，
GPU payload 保留在 SDK 分配中。生命周期边界见 [上级说明](../README.md)。
厂商接口见 [CUDA IPC](https://docs.nvidia.com/cuda/cuda-runtime-api/group__CUDART__DEVICE.html)。
