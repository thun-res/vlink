# bytes_advanced — Bytes 高级操作：压缩、Base64、CRC、十六进制、内存池

本示例覆盖 `vlink::Bytes` 在性能/工具层面的高级 API：

- **LZAV 压缩 / 解压**：vlink 内置的轻量压缩算法，适合录制（`recording/`）、跨网带宽优化。
- **Base64 编解码**：把二进制数据塞进 JSON / 文本日志。
- **CRC32 / CRC64**：消息完整性校验、缓存键。
- **十六进制字符串转换**：调试和日志打印。
- **`from_user_input`**：把用户输入串（`0x...` 或字面量）解析为 Bytes。
- **`reverse_order`**：字节序反转。
- **容量调整**：`resize` / `reserve` / `shrink_to`。
- **内存池**：`Bytes::init_memory_pool()` 启用分级内存池减少分配开销。

读完本示例你能掌握：

- 何时该用压缩、Base64、CRC 这些工具方法。
- 容量调整的几个 API 的差异和扩容失败语义。
- 内存池初始化对 vlink 应用整体性能的影响。

## 背景与适用场景

`Bytes` 不只是消息容器，也是 vlink 内部一个统一的"字节工具集"。许多需要按字节操作的场景（编解码、校验、转换）都集中在 Bytes 的静态/成员方法上：

- **压缩**：LZAV 是 vlink 自带的快速字节压缩；适合录制 bag 文件、节省传输带宽。
- **Base64**：MQTT / HTTP 等文本协议里嵌入二进制 payload 时必备。
- **CRC32**：消息完整性、缓存键、去重判定（注意不是加密哈希，不能抗篡改）。
- **十六进制串**：把 binary 写进日志、配置文件、JSON。
- **内存池**：高频小 Bytes 分配场景（每秒数百万次 publish）开启后能显著降 malloc 开销。

## 核心 API

| API | 签名 | 说明 |
|-----|------|------|
| `Bytes::compress_data` | `static Bytes compress_data(const void* data, size_t size, bool high_ratio = false)` | LZAV 压缩；`high_ratio=true` 走更慢但更紧的算法 |
| `Bytes::uncompress_data` | `static Bytes uncompress_data(const void* data, size_t size, bool validate_magic = false)` | 解压；`validate_magic` 检查 LZAV 头尾魔数 |
| `Bytes::is_compress_data` | `static bool is_compress_data(const void* data, size_t size)` | 判定数据是否为 LZAV 压缩 |
| `Bytes::encode_to_base64` | `static std::string encode_to_base64(const Bytes&)` | 编码为 Base64 字符串 |
| `Bytes::decode_from_base64` | `static Bytes decode_from_base64(const std::string&)` | 反向 |
| `Bytes::get_crc_32` | `static uint32_t get_crc_32(const Bytes&)` | CRC32 |
| `Bytes::get_crc_64` | `static uint64_t get_crc_64(const void*, size_t)` | CRC64 |
| `Bytes::convert_to_hex_str` | `static std::string convert_to_hex_str(const void* data, size_t size)` | 大写十六进制串 |
| `Bytes::from_user_input` | `static Bytes from_user_input(const std::string& input, bool* ok = nullptr)` | 解析 `"0x..."` 或字面量 |
| `Bytes::reverse_order` | `static Bytes reverse_order(const Bytes&)` | 字节序反转 |
| `Bytes::resize` / `reserve` / `shrink_to` | 同名 | 容量调整 |
| `Bytes::init_memory_pool` | `static void init_memory_pool()` | 启用 vlink 内存池 |

## 代码导读

### 1. LZAV 压缩

```cpp
std::string original(500, 'A');
auto src = Bytes::from_string(original);
auto compressed = Bytes::compress_data(src.data(), src.size());
auto hi_compressed = Bytes::compress_data(src.data(), src.size(), /*high_ratio=*/true);
auto decompressed = Bytes::uncompress_data(compressed.data(), compressed.size());

MLOG_I("compress: orig={} std={} hi={} ratio={:.3f}", src.size(), compressed.size(), hi_compressed.size(),
       static_cast<double>(compressed.size()) / static_cast<double>(src.size()));
VLOG_I("is_compress_data=", Bytes::is_compress_data(compressed.data(), compressed.size()),
       " round-trip ok=", (decompressed.to_string() == original));
```

`compress_data` 对 > 1 MiB 输入会返回空 Bytes（vlink 设计上认为应用自己分片处理大消息）；高压缩模式压缩率更高但慢约 3-5 倍。

### 2. Base64 / CRC / 十六进制

```cpp
auto src = Bytes::from_string("Hello, VLink Base64!");
std::string encoded = Bytes::encode_to_base64(src);
auto decoded = Bytes::decode_from_base64(encoded);

uint32_t crc = Bytes::get_crc_32(Bytes::from_string("VLink CRC test"));
std::string hex = Bytes::convert_to_hex_str(data.data(), data.size());
```

### 3. from_user_input 与字节翻转

```cpp
bool ok = false;
auto hex_bytes = Bytes::from_user_input("0x48656C6C6F", &ok);     // "Hello"
auto bad      = Bytes::from_user_input("not_hex", &ok);            // empty + ok=false

Bytes original{0x01, 0x02, 0x03, 0x04};
auto reversed = Bytes::reverse_order(original);                    // {0x04, 0x03, 0x02, 0x01}
```

### 4. 容量调整

```cpp
auto buf = Bytes::create(20);
bool ok = buf.reserve(500);     // 预留 500 字节
ok = buf.resize(300);           // size = 300
ok = buf.shrink_to(100);        // size = 100
ok = buf.shrink_to(200);        // size 300 → 200 是缩容操作；此处实际语义按实现
```

`reserve` 不改变 size 只调容量；`resize` 改变 size；`shrink_to` 是只缩不扩。

### 5. 内存池

```cpp
Bytes::init_memory_pool();
auto pooled = Bytes::create(200);
```

`init_memory_pool` 启用 vlink 的分级金字塔内存池（详见 `../memory_pool/`）。在高频小 Bytes 分配下能省 30-70% malloc 时间。生产代码应该在 main 开头调一次。

## 运行

```bash
./build/output/bin/example_bytes_advanced
```

预期输出（节选）：

```
=== Bytes Advanced Example ===
compress: orig=500 std=12 hi=12 ratio=0.024
is_compress_data=1 round-trip ok=1
base64 enc=SGVsbG8sIFZMaW5rIEJhc2U2NCE=
base64 dec="Hello, VLink Base64!" round-trip=1
crc32=0x9D7CCAFD
crc consistency=1 differs=1
hex=DEADBEEF
hex-input ok=1 parsed="Hello"
bad-input ok=0 empty=1
orig=01020304 reversed=04030201
...
pooled alloc size=200 (VLINK_MEMORY_LEVEL governs tiers)
operator<< : ...
=== Bytes Advanced Example Complete ===
```

## 常见陷阱

1. **压缩 > 1 MiB**：`compress_data` 返回空 Bytes，不报错；需要分片或自己用更通用的压缩库。
2. **`validate_magic=false` 时解压非压缩数据**：返回未定义内容，不报错；建议总是 `validate_magic=true` 在不可信源上。
3. **CRC32 用作哈希**：不是加密哈希，不能抗篡改；用 BLAKE3 / SHA-256 等。
4. **`from_user_input` 容错弱**：非法输入返回 empty + ok=false；调用方必须检查 `ok`。
5. **memory pool 初始化时机**：必须在 main 早期、所有 vlink 通信原语创建之前；后期 init 不影响已分配的 Bytes。

## 设计要点

- LZAV 是 vlink 内置的轻量压缩算法（不是 zlib、不是 zstd）；优势是嵌入式友好、不需要外部依赖。
- Base64 实现遵循 RFC 4648；编码 `+` `/` `=`。
- 内存池受环境变量 `VLINK_MEMORY_LEVEL` 控制 tier 数量（详见 `memory_pool/`）。
- `Bytes::create` 在内存池未 init 时回退到 malloc。

## 参考

- `../bytes_basic/` — 构造、SBO、访问器
- `../bytes_zerocopy/` — 所有权与零拷贝
- `../memory_pool/` — 分级内存池细节
- `../../recording/record_compression/` — 录制路径上的压缩
- `vlink/include/vlink/base/bytes.h` — Bytes 完整接口
