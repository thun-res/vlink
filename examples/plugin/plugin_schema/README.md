# plugin_schema — Schema 插件：`SchemaPluginInterface` 与 `SchemaPluginManager`

`vlink::SchemaPluginInterface` 是 vlink 录制系统用来"动态查找 Protobuf descriptor / FlatBuffers BFBS schema"的扩展点。BagWriter 在写入时通过它把 schema 嵌入 bag 文件；BagReader / 动态解码 / Foxglove 集成都靠它在运行期还原类型信息。

读完本示例你能掌握：

- Schema 插件接口的字段语义。
- 直接 load 与通过 `SchemaPluginManager::get()` 全局单例两种使用方式。
- `VLINK_SCHEMA_PLUGIN` 环境变量。
- Schema 插件与 Basic / Runnable 插件的对比。

## 背景与适用场景

适用：

- 用 vlink 做录制并希望 bag 自描述（不依赖原始 .proto 文件即可解码）。
- 希望 Foxglove / 跨语言工具直接打开 vlink bag。
- 需要按类型名运行期创建 Protobuf Message 实例。

不适用：

- 不录制、不动态解码（普通 Publisher/Subscriber 走静态序列化即可）。
- 编译期已知所有类型（直接 link 进 host）。

vlink 默认带一个参考实现 `vlink_schema_plugin`，依赖 Protobuf；环境里没有 Protobuf 时本示例只演示 API surface。

## 接口

```cpp
class SchemaPluginInterface {
  VLINK_PLUGIN_REGISTER(SchemaPluginInterface)
 public:
  using ProtobufDescriptorPtr = void*;
  using ProtobufMessagePtr    = void*;
  using FlatbuffersSchemaPtr  = void*;
  using FlatbuffersParserPtr  = void*;

  virtual VersionInfo               get_version_info() const = 0;
  virtual ProtobufDescriptorPtr     search_protobuf_descriptor(const std::string&) = 0;
  virtual SchemaData                search_schema(const std::string&, SchemaType) = 0;
  virtual std::vector<SchemaData>   get_all_schemas(SchemaType) = 0;
  virtual ProtobufMessagePtr        create_protobuf_message(const std::string&) = 0;
  virtual FlatbuffersSchemaPtr      search_flatbuffers_schema(const std::string&) = 0;
  virtual FlatbuffersParserPtr      create_flatbuffers_parser(const std::string&) = 0;
};
```

字段语义：

- `VersionInfo`：插件版本元数据（`name`、`version`、`commit_id`）。
- `SchemaData { name, encoding, schema_type, data }`：完整的 FileDescriptorSet / BFBS payload，BagWriter 嵌入文件、BagReader 反向恢复。
- `void*` 是为了让接口头**不依赖 protobuf / flatbuffers**；调用方按需 cast。

## 直接 load vs Manager

```cpp
// 方式 1：直接 load，显式 lifetime
vlink::Plugin plugin;
auto p = plugin.load<vlink::SchemaPluginInterface>("vlink_schema_plugin", 1, 0);

// 方式 2：进程级单例（首次读 VLINK_SCHEMA_PLUGIN 环境变量）
auto& mgr = vlink::SchemaPluginManager::get();
auto iface = mgr.is_valid() ? mgr.get_interface() : nullptr;
```

`SchemaPluginManager::get()` 内部有 lazy init：首次调时读 `VLINK_SCHEMA_PLUGIN`（可以是绝对路径或 .so 名），后续调返回缓存的单例。

## void* 类型擦除

接口里用 `void*` 是为了让 vlink 核心库不依赖 protobuf。调用方 cast 到具体类型：

```cpp
auto* desc = static_cast<const google::protobuf::Descriptor*>(
    p->search_protobuf_descriptor("foo.Bar"));
if (desc != nullptr) {
  // 直接用 protobuf API
  for (int i = 0; i < desc->field_count(); ++i) {
    VLOG_I("field: ", desc->field(i)->name());
  }
}
```

## 代码导读

### 1. 加载（manager 形式）

```cpp
auto& mgr = vlink::SchemaPluginManager::get();
if (!mgr.is_valid()) {
  VLOG_W("schema plugin not loaded; set VLINK_SCHEMA_PLUGIN env");
  return 0;
}

auto schema_plugin = mgr.get_interface();
auto ver = schema_plugin->get_version_info();
VLOG_I("plugin: ", ver.name, " v", ver.version, " commit=", ver.commit_id);
```

### 2. 查 Protobuf descriptor

```cpp
const std::string type_name = "example.SensorData";

auto* desc = schema_plugin->search_protobuf_descriptor(type_name);
VLOG_I("search_protobuf_descriptor(\"", type_name, "\"): ", desc ? "found" : "not found");

auto schema = schema_plugin->search_schema(type_name, vlink::SchemaType::kProtobuf);
if (!schema.data.empty()) {
  VLOG_I("schema: ", schema.name, " encoding=", schema.encoding, " ", schema.data.size(), " bytes");
}
```

### 3. 创建消息实例

```cpp
auto* msg = schema_plugin->create_protobuf_message(type_name);
VLOG_I("create_protobuf_message: ", msg ? "ok" : "not found");
// 调用方按需 cast 为 google::protobuf::Message* 使用
```

### 4. FlatBuffers schema

```cpp
auto* fbs_schema = schema_plugin->search_flatbuffers_schema("example.SensorFrame");
VLOG_I("search_flatbuffers_schema: ", fbs_schema ? "found" : "not found");
```

## 三种插件对比

| 插件类型 | 自带 MessageLoop | 生命周期方法 | 典型用途 |
|---------|----------------|-------------|---------|
| 基础（`plugin_basic`） | 无 | 无 | 通用扩展 |
| Runnable（`plugin_runnable`） | 有 | `on_init` / `on_deinit` | 自驱动组件 |
| Schema（本示例） | 无 | 无 | descriptor / schema 查找 |

## 运行

```bash
./build/output/bin/example_plugin_schema

# 指定真实的 schema plugin .so
VLINK_SCHEMA_PLUGIN=/path/to/vlink_schema_plugin.so ./build/output/bin/example_plugin_schema
```

预期输出（无 .so 时）：

```
schema plugin not loaded; set VLINK_SCHEMA_PLUGIN env
```

有 .so 时打出 plugin 版本、查找结果。

## 常见陷阱

1. **环境变量没设**：`SchemaPluginManager::get()` 返回 invalid manager；先 `is_valid()` 检查。
2. **void* cast 错类型**：UB；按 SchemaType 区分 protobuf / flatbuffers 后 cast。
3. **descriptor 生命周期**：descriptor 由插件管理；不要 delete 它。
4. **跨进程 schema 不一致**：插件 .so 编进 protobuf descriptor，host 进程必须用相同 .so（或同 schema 文件）。
5. **search_schema 返回 empty data**：表示未找到；不会抛异常。

## 设计要点

- void* 接口让 vlink 核心库零外部依赖（不强制 Protobuf）。
- BagWriter 在写入时调 `search_schema` 把 schema 嵌入；BagReader 读出后调用方再用 schema 解码。
- Manager 单例使 process-wide 共享一个 schema source。

## 配图

![Schema plugin flow](./images/schema-plugin-flow.png)

图中展示 BagWriter → SchemaPluginManager → SchemaPluginInterface → Protobuf descriptor 的查询链路。

## 参考

- `../plugin_basic/` — 基础 plugin 机制
- `../plugin_runnable/` — RunablePlugin
- `../../recording/` — Schema 插件的主要客户
- `vlink/include/vlink/extension/schema_plugin_interface.h` — 接口
- `vlink/include/vlink/extension/schema_plugin_manager.h` — Manager
- 顶层 `doc/19-extensions.md` — 插件系统章节
- 顶层 `doc/12-bag-recording.md` — Schema 怎么被 BagWriter / Reader 使用
