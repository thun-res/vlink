# hbmem FastBuffer 插件

要求地平线平台的 64 位 Linux、VLink 与 hbmem SDK。交叉编译时使用对应工具链文件。

```bash
cmake -S examples/fastbuffer/hbmem -B build/fastbuffer-hbmem \
  -DCMAKE_TOOLCHAIN_FILE=/path/to/toolchain.cmake \
  -DCMAKE_PREFIX_PATH=/path/to/vlink -DFASTBUFFER_HBMEM_ROOT=/path/to/hbmem
cmake --build build/fastbuffer-hbmem
```

加载产物 `libfastbuffer_hbmem.so`：

```bash
export VLINK_FASTBUFFER_PLUGIN=/path/to/libfastbuffer_hbmem.so
```

Default/Shared/DmaBuf 使用 `hb_mem_alloc_com_buf`，共享通过 `hb_mem_import_com_buf` 导入。
插件声明 `kCustom` 模式，通过 hbmem SDK 管理跨进程引用；Manager 仍统一管理本地浅拷贝引用。
`native_handle()` 返回借用的 `hb_mem_common_buf_t`；`import_native()` 导入独立引用。
`map(Read)` 失效 CPU 缓存，`unmap(Write)` 刷新 CPU 写入。

缓存维护不等待 BPU/VPU/GPU 任务，外部设备任务须由调用方完成后再共享、映射或释放。
该插件管理自身的 hbmem 模块打开/关闭，不接管外部 SDK 资源的所有权。
接口说明见 [hbmem 官方文档](https://developer.d-robotics.cc/x5_sdk_doc/multimedia_development/3-Hbmem_index_zh_CN.html)。
