# uuid_basic — `vlink::Uuid` 用法：生成、解析、比较、序列化

`vlink::Uuid` 是 vlink 提供的 RFC 4122 UUID 实现，覆盖随机 v4 生成、字符串/字节序列化、相等比较、hash、constexpr 上下文使用。它在 vlink 内部用于消息 ID、请求 ID、节点身份标识等场景。

读完本示例你能掌握：

- 默认构造 / 数组构造 / 迭代器构造 / 随机生成 / 字符串解析的全部入口。
- UUID 三种字符串形式（canonical、braced、compact）的互转。
- 一致性 hash + `unordered_set<Uuid>` 用法。
- variant / version 字段语义。
- `kByteSize=16` / `kStringSize=36` 等常量。

## 背景与适用场景

适用场景：

- 全局唯一 ID（跨进程、跨机器，几乎无碰撞概率）。
- 业务对象主键、日志关联 ID、trace span ID。
- vlink 内部的 request_id（Method 模型）、session ID 等。

不适合：

- 短数字 ID（用 64-bit 自增即可）。
- 强密码学场景（v4 是随机 UUID，熵 122 bit；不是 cryptographic random 的替代）。

## 核心 API

| API | 签名 | 说明 |
|-----|------|------|
| `Uuid` | 默认构造 | nil UUID（全 0） |
| `Uuid(std::array<uint8_t,16>)` | 构造 | 从 16 字节数组构造 |
| `Uuid(const uint8_t[16])` | 构造 | 从 C 数组构造 |
| `Uuid(Iter begin, Iter end)` | 构造 | 从迭代器范围；非 16 字节构造出 nil |
| `Uuid::generate_random` | `static Uuid generate_random()` 或 `(Engine&)` | 生成随机 v4；可选传 std::mt19937 等 |
| `Uuid::from_string` | `static std::optional<Uuid> from_string(std::string_view)` | 解析三种形式 |
| `Uuid::is_valid` | `static bool is_valid(std::string_view)` | 判定字符串是否合法 |
| `to_string` | `std::string to_string() const` | canonical 形式 `xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx` |
| `to_compact_string` | 同上 | 紧凑形式（无 `-`） |
| `bytes` | `const std::array<uint8_t,16>& bytes() const` | 取原始字节 |
| `is_nil` / `variant` / `version` | 同名 | RFC 字段查询 |
| `swap` | `void swap(Uuid&)` | 交换 |
| `Uuid::random_bytes` / `random_hex` | `static Bytes/std::string` | 随机字节 / 随机 hex 工具 |
| `kByteSize` / `kStringSize` | constexpr size_t | 16 / 36 |

## 代码导读

### 1. 构造方式

```cpp
Uuid id1;                                                      // nil
Uuid id2{std::array<uint8_t,16>{0x47, 0xac, ...}};             // 数组
Uuid id3{raw_uint8_array_16};                                  // C array
Uuid id4{vec.begin(), vec.end()};                              // iterator range
Uuid id5 = Uuid::generate_random();                            // 随机 v4
auto opt = Uuid::from_string("47ac10b8-58cc-4a3c-8c5b-...");   // optional
```

迭代器范围非 16 字节时构造出 nil UUID，**不抛异常**；调用方必须自己检查。

### 2. 三种字符串形式

```cpp
auto parsed_canonical = Uuid::from_string("47ac10b8-58cc-4a3c-8c5b-0e778899aabb");
auto parsed_braced    = Uuid::from_string("{47ac10b8-58cc-4a3c-8c5b-0e778899aabb}");
auto parsed_compact   = Uuid::from_string("47ac10b858cc4a3c8c5b0e778899aabb");
auto parsed_bad       = Uuid::from_string("not-a-uuid");   // nullopt
```

`is_valid` 是不构造 Uuid 的快速判定，用于校验用户输入。

### 3. 随机生成与确定性引擎

```cpp
Uuid id = Uuid::generate_random();   // 内部用线程安全随机源

std::mt19937 e1(0xdeadbeefU);
std::mt19937 e2(0xdeadbeefU);
Uuid a = Uuid::generate_random(e1);
Uuid b = Uuid::generate_random(e2);
Uuid c = Uuid::generate_random(e1);
// a == b（相同 seed）；c != a（e1 被推进了）
```

测试场景里给定 seed 的 engine 能产出可复现的 UUID 序列。

### 4. variant / version / hash 容器

```cpp
Uuid id = Uuid::generate_random();
VLOG_I("variant=", static_cast<int>(id.variant()), " version=", static_cast<int>(id.version()));
// variant=1 (RFC)，version=4 (RandomBased)

std::unordered_set<Uuid> set;
for (int i = 0; i < 10; ++i) {
  set.insert(Uuid::generate_random());
}
```

`std::hash<vlink::Uuid>` 已特化，可以直接用作 unordered 容器 key。

### 5. constexpr

```cpp
constexpr Uuid nil_id;
static_assert(nil_id.is_nil(), "default Uuid must be nil");
constexpr std::array<uint8_t, 16> sample{0x47, 0xac, ...};
constexpr Uuid sample_id{sample};
static_assert(sample_id.bytes()[0] == 0x47U, "constexpr bytes() must work");
```

默认构造、数组构造、字节访问都是 constexpr。

## 运行

```bash
./build/output/bin/example_uuid_basic
```

预期输出（节选）：

```
default: is_nil=1 to_string=00000000-0000-0000-0000-000000000000
from array: canonical=47ac10b8-58cc-4a3c-8c5b-0e778899aabb compact=47ac10b858cc4a3c8c5b0e778899aabb
random: <some uuid> variant=1 version=4
seeded same engine: a==b=1 c!=a after engine advanced=1
braced parsed=1 compact parsed=1 bad parsed=0
10 random UUIDs in unordered_set: size=10
constexpr static_asserts passed
```

## 常见陷阱

1. **迭代器构造长度不对**：得到 nil UUID，不报错；务必检查 `is_nil()`。
2. **大小写差异**：`from_string` 大小写不敏感；`to_string` 输出小写。round-trip 大写时比较会不等。
3. **`generate_random()` 内部随机源** vs 自传 engine：自传 engine 时 vlink 不保证线程安全；并发使用必须各自持有 engine。
4. **`mt19937` 不是密码学安全**：长期密钥请用 OpenSSL `RAND_bytes`。
5. **`constexpr to_string` 不支持**：只有 bytes()/is_nil() 等是 constexpr。

## 设计要点

- v4 的随机熵约 122 bit；工程上视为永不碰撞。
- Uuid 的相等比较是 16 字节比较；hash 是 FNV 类（非加密）。
- 内部生成路径有三级 fallback，保证 `generate_random()` `noexcept`。

## 配图

无专属配图。

## 参考

- `vlink/include/vlink/base/uuid.h` — Uuid 接口
- `../utils/` — 其它平台工具
- RFC 4122 — UUID 规范
