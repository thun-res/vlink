# serialization — 自动序列化派发的四种典型路径

vlink 的通信原语 `Publisher<T>` / `Subscriber<T>` / `Client<Req,Resp>` / `Server<Req,Resp>` / `Setter<T>` / `Getter<T>` 都是模板类，模板参数 `T` 在编译期决定序列化策略。vlink 用 SFINAE / `if constexpr` 链在编译期从最具体到最通用逐级匹配，最终落到 `Serializer::Type` 中的某一个枚举值。

本目录收录四个示例，分别覆盖最常见的四类序列化路径，覆盖了大多数业务场景；少数特殊路径（Protobuf、FlatBuffers、CDR）放在 `samples/` 因为它们需要外部工具链。

## 子示例索引

| 示例 | 主题 | 关键 API |
|------|------|---------|
| `basic_types/` | Bytes、std::string、POD 结构三种基础路径 | `Serializer::get_type_of<T>`、`Serializer::serialize`、`Bytes`、`std::string`、POD |
| `custom_type/` | 自定义 `operator>>` / `operator<<` 接入 `kCustomType` | `Bytes::create`、用户类型的两个 operator |
| `dynamic_data/` | 类型擦除容器 `DynamicData`，同话题携带多类型 | `DynamicData::load`、`as<T>`、`get_type` |
| `intra_data/` | 进程内零拷贝消息 | `VLINK_INTRA_DATA_DECLARE`、`MyIntra::create()` |

## 推荐阅读顺序

1. **`basic_types/`** —— 必须先看。理解 vlink 的"编译期类型派发"是怎么工作的：`is_trivial && is_standard_layout` 的 POD 走 `kStandardType` memcpy，`std::string` 走 `kStringType` 长度前缀，`Bytes` 走 `kBytesType` 零开销。
2. **`custom_type/`** —— 大多数业务结构会包含 `std::string`、`std::vector` 等非 trivial 字段，必须用 `operator>>` / `operator<<` 自定义编码。
3. **`dynamic_data/`** —— 同话题想跨多类型时使用；理解 wire 格式（前 20 字节类型名 + payload）。
4. **`intra_data/`** —— 进程内零拷贝场景：用 `VLINK_INTRA_DATA_DECLARE` 生成 shared_ptr 包装类，配合 `intra://` 完全跳过序列化。

## 共同前置知识

- `../quickstart/` — VLink 三种通信原语的基础用法。
- `../base/bytes_basic/`、`../base/bytes_advanced/` — `vlink::Bytes` 的完整 API；自定义 operator 几乎一定会用到。
- C++17 模板基础：`if constexpr`、`std::is_trivial_v`、`std::is_standard_layout_v`、`std::enable_if`。

## 各路径的工程权衡

| 路径 | 类型枚举 | 编码开销 | 跨语言 | 适用 |
|------|---------|--------|--------|------|
| `Bytes` | `kBytesType` | 零 | 不直接（自定义协议） | 自定义二进制协议、性能极致路径 |
| `std::string` | `kStringType` | 4 字节长度前缀 | 强（UTF-8 文本） | JSON、命令、Log |
| POD struct | `kStandardType` | 零（含 padding） | 弱（ABI 依赖） | 同 ABI 同语言、高性能 |
| 自定义 `operator>>` | `kCustomType` | 用户决定 | 用户决定 | 复杂结构、紧凑编码、跨语言兼容 |
| Protobuf | `kProtoType` | 中等 | 强 | 跨语言、向后兼容 |
| FlatBuffers | `kFlatTableType` | 低 | 强 | 大对象 + 零拷贝读取 |
| CDR | `kCdrType` | 中等 | 强（DDS 标配） | DDS 互通 |
| DynamicData | `kDynamicType` | +20 字节固定开销 | 否 | 同话题多类型 |
| IntraData | `kIntraDataType` | 进程内零；跨进程走 ValueT 的 operator | 进程内才有意义 | 同进程多模块共享大对象 |

## 配图

![Serialization dispatch chain](basic_types/images/serialization-type-chain.png)

上图位于 `basic_types/images/`，给出 `Serializer::get_type_of<T>()` 在编译期的链式判定流程，建议结合 `basic_types/README.md` 一起看。

## 参考

- 顶层 `doc/06-serialization.md` — 序列化机制全景，含全部 `Serializer::Type` 枚举与 wire 格式细节。
- `vlink/include/vlink/serializer.h` — `Serializer` 完整接口。
- `samples/helloworld/` — Protobuf 序列化端到端样例。
- `samples/pub_sub_fbs/` — FlatBuffers 序列化端到端样例。
- `samples/dds_idl/` — DDS IDL (CDR) 序列化样例（需 FastDDS IDL 工具链）。
