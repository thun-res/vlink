# bytes_zerocopy — `vlink::Bytes` 的所有权模型与零拷贝语义

`vlink::Bytes` 在 vlink 中是消息载荷、loan API、零拷贝传输的统一抽象。它内部用引用计数 + 所有权标志位区分多种"所有权状态"，让同一份字节缓冲可以在拷贝、传递、跨进程引用时灵活选择是否真正复制内存。

本示例覆盖 5 种 Bytes 构造工厂以及它们对应的语义：

- `shallow_copy(ptr, n)` —— 不拥有外部缓冲，只做引用别名。
- `deep_copy(ptr, n, offset=0)` —— 从外部缓冲深拷贝；可保留前缀偏移给 header。
- `loan_internal(ptr, n)` —— Iceoryx 等共享内存模块借出的内存；vlink 不释放。
- `shallow_copy_ptr(ptr)` —— 把任意指针塞进 Bytes 容器（size=0、`is_ptr()==true`），用于跨语言或不透明对象传递。
- `create(n)` —— 内部分配、拥有内存。

读完本示例你能掌握：

- 何时用 shallow_copy / deep_copy / loan_internal。
- `is_owner` / `is_loaned` / `is_ptr` 三种状态查询。
- 拷贝构造的语义跟初始所有权"flavor"绑定。
- `deep_copy_self` 把别名升级为 owner。

## 背景与适用场景

`Bytes` 是 vlink 在性能敏感路径上传递字节缓冲的核心容器。设计目标：

- 默认零拷贝：构造、移动、传参不复制 payload。
- 显式深拷贝：必要时一次性 `deep_copy()` 复制到自有内存。
- 兼容外部内存：`shallow_copy` / `loan_internal` 让 Bytes 包装来自 Iceoryx、shm、用户态分配器等的外部内存。
- 引用计数所有权：多个 Bytes 实例可以共享同一份 owned payload。

各种构造方式的内存语义：

| 工厂 | 拥有内存 | 复制构造行为 | 用途 |
|------|---------|------------|------|
| `Bytes::create(n)` | 是 | 深拷贝 | 自己分配缓冲，写入数据 |
| `Bytes::from_string(s)` | 是 | 深拷贝 | 从 std::string 构造 |
| `Bytes::shallow_copy(p, n)` | 否（别名） | 浅拷贝 | 包装别人的可变 / 只读缓冲 |
| `Bytes::deep_copy(p, n[, offset])` | 是 | 深拷贝 | 从外部缓冲完整复制 |
| `Bytes::loan_internal(p, n)` | 否（loaned） | 浅拷贝 | Iceoryx / shm 借出的内存 |
| `Bytes::shallow_copy_ptr(p)` | 否（指针包装） | 浅拷贝 | 把不透明指针塞进 Bytes |

## 核心 API

| API | 签名 | 说明 |
|-----|------|------|
| `Bytes::shallow_copy` | `static Bytes shallow_copy(const void* data, size_t size)` / `(void*, size_t)` | 别名包装；调用方负责保证源缓冲生命周期 |
| `Bytes::deep_copy` | `static Bytes deep_copy(const void* data, size_t size, size_t offset = 0)` | 深拷贝；可预留 offset 前缀给 header |
| `Bytes::loan_internal` | `static Bytes loan_internal(void* data, size_t size)` | 包装 loaned 内存；析构不释放 |
| `Bytes::shallow_copy_ptr` | `static Bytes shallow_copy_ptr(const void* ptr)` | 包装任意指针，size=0 |
| `Bytes::deep_copy_self` | `void deep_copy_self()` | 当前 Bytes 是别名时，自分配新内存并复制 |
| `Bytes::is_owner` | `bool is_owner() const` | 是否拥有内存 |
| `Bytes::is_loaned` | `bool is_loaned() const` | 是否是 loaned 内存 |
| `Bytes::is_ptr` | `bool is_ptr() const` | 是否是指针包装 |
| `Bytes::offset` / `real_data` / `real_size` | 同名 | 处理 header offset 时使用 |
| `Bytes::to_ptr<T>` | `template <typename T> T* to_ptr() const` | 把 `shallow_copy_ptr` 的指针还原为类型 |

## 代码导读

### 1. shallow_copy 可变别名

```cpp
uint8_t external_buf[32];
std::memset(external_buf, 0x42, sizeof(external_buf));

auto alias = Bytes::shallow_copy(external_buf, sizeof(external_buf));
print_ownership("shallow_copy mut", alias);
alias[0] = 0xFF;
VLOG_I("external_buf[0] after alias write: 0x", std::hex, static_cast<int>(external_buf[0]));
```

`alias` 指向 `external_buf`，写入 `alias[0]` 会真的改 `external_buf[0]`。调用方必须保证 `external_buf` 在 `alias` 使用期间一直有效。

### 2. shallow_copy 只读别名

```cpp
const uint8_t read_only[] = {0x10, 0x20, 0x30};
auto alias = Bytes::shallow_copy(read_only, sizeof(read_only));
```

const 重载返回只读视图；不能 `alias[i] = ...`。

### 3. deep_copy 拥有副本

```cpp
uint8_t external_buf[] = {0xAA, 0xBB, 0xCC, 0xDD};
auto owned = Bytes::deep_copy(external_buf, sizeof(external_buf));
VLOG_I("distinct memory=", (owned.data() != external_buf), " content ok=", (owned[0] == 0xAA && owned[3] == 0xDD));
```

`owned.data() != external_buf` —— 新分配了一块内存。`external_buf` 之后可以自由释放。

### 4. deep_copy 带 offset

```cpp
uint8_t payload[] = {0x01, 0x02, 0x03};
auto buf = Bytes::deep_copy(payload, sizeof(payload), /*offset=*/4);
VLOG_I("offset=", static_cast<int>(buf.offset()), " size=", buf.size(), " real_size=", buf.real_size());
```

offset=4 在 payload 前预留 4 字节空间。`buf.data()` 跳过 offset 后指向 payload；`buf.real_data()` 指向 offset 起点。用于传输层在 payload 前插入 header 的场景。

### 5. 实例方法 deep_copy / shallow_copy

```cpp
auto original = Bytes::from_string("original data");

Bytes deep;
deep.deep_copy(original);
VLOG_I("distinct memory=", (deep.data() != original.data()));

Bytes alias;
alias.shallow_copy(original);
VLOG_I("same memory=", (alias.data() == original.data()));
```

实例方法版本：把当前 Bytes 转为另一个 Bytes 的深拷贝 / 浅别名。

### 6. deep_copy_self 升级别名为 owner

```cpp
uint8_t ext[] = {0x11, 0x22, 0x33};
auto alias = Bytes::shallow_copy(ext, sizeof(ext));
VLOG_I("before deep_copy_self is_owner=", alias.is_owner());

alias.deep_copy_self();
VLOG_I("after deep_copy_self is_owner=", alias.is_owner());
ext[0] = 0xFF;
VLOG_I("ext mutated, alias[0]=0x", std::hex, static_cast<int>(alias[0]));
```

很有用的"懒拷贝"模式：先 shallow_copy 收消息（零拷贝路径），需要长期保留时再 `deep_copy_self()` 一次性获得自有内存。

### 7. shallow_copy_ptr / to_ptr

```cpp
int my_object = 42;
auto ptr_bytes = Bytes::shallow_copy_ptr(&my_object);
print_ownership("shallow_copy_ptr", ptr_bytes);

int* recovered = ptr_bytes.to_ptr<int>();
VLOG_I("to_ptr<int>=", *recovered, " same address=", (recovered == &my_object));
```

`shallow_copy_ptr` 把任意指针塞进 Bytes：size=0、`is_ptr()==true`。`to_ptr<T>()` 反向取回。用于在 vlink 通信路径上传递不透明对象（跨语言、handle、引用计数对象等）。

### 8. loan_internal（模拟 SHM）

```cpp
uint8_t simulated_shm[64];
std::memset(simulated_shm, 0x99, sizeof(simulated_shm));
auto loaned = Bytes::loan_internal(simulated_shm, sizeof(simulated_shm));
print_ownership("loan_internal", loaned);
```

`loan_internal` 标记的 Bytes 析构时**不会释放**底层内存；专为 Iceoryx 等"借出 + 归还"模型设计。生产代码里通常配合 `Subscriber::set_manual_unloan(true)` 显式管理生命周期。

### 9. 拷贝构造保留所有权 flavor

```cpp
auto owner = Bytes::from_string("owned");
Bytes copy_of_owner(owner);
VLOG_I("copy of owner: is_owner=", copy_of_owner.is_owner(),
       " distinct memory=", (copy_of_owner.data() != owner.data()));

uint8_t ext[] = {1, 2, 3};
auto alias = Bytes::shallow_copy(ext, 3);
Bytes copy_of_alias(alias);
VLOG_I("copy of alias: is_owner=", copy_of_alias.is_owner(),
       " same memory=", (copy_of_alias.data() == alias.data()));
```

owner 的拷贝是深拷贝；alias 的拷贝是另一个 alias。这种"保留 flavor"语义让用户不必担心"拷贝时会不会偷偷分配内存"。

### 10. 移动构造

```cpp
auto source = Bytes::from_string("moveable");
Bytes dest(std::move(source));
VLOG_I("after move: dest.size=", dest.size(), " source.empty=", source.empty(), " dest=\"", dest.to_string(), "\"");
```

移动后 source 变空，dest 接管所有状态。

## 运行

```bash
./build/output/bin/example_bytes_zerocopy
```

预期输出（节选）：

```
shallow_copy mut: size=32 is_owner=0 is_loaned=0 is_ptr=0 empty=0
external_buf[0] after alias write: 0xff
shallow_copy const: size=3 is_owner=0 is_loaned=0 is_ptr=0 empty=0
deep_copy ext: size=4 is_owner=1 is_loaned=0 is_ptr=0 empty=0
distinct memory=1 content ok=1
deep_copy offset=4: size=3 is_owner=1 ...
offset=4 size=3 real_size=7
...
loan_internal: size=64 is_owner=0 is_loaned=1 is_ptr=0 empty=0
copy of owner: is_owner=1 distinct memory=1
copy of alias: is_owner=0 same memory=1
after move: dest.size=8 source.empty=1 dest="moveable"
```

## 常见陷阱

1. **shallow_copy 后源缓冲提前释放**：alias 仍引用已释放的内存 —— UAF。永远确认源缓冲生命周期足够。
2. **shallow_copy 跨进程**：进程间的指针没有意义，shallow_copy 只能在同进程用。
3. **loan_internal + manual_unloan(false)**：vlink 默认收到 loaned Bytes 后自动归还；如果不归还（手动模式），调用方必须显式 `return_loan()`，否则 Iceoryx 的缓冲池会耗尽。
4. **deep_copy_self 太频繁**：每次都分配新内存，失去零拷贝优势；只在确实需要"接管所有权"时调一次。
5. **`is_ptr()` 与 `size()`**：is_ptr() 的 Bytes 永远 size=0；不能用 `b.size()` 判断是否为指针包装。

## 设计要点

- Bytes 内部用 control block 跟踪所有权 + 引用计数；多个 owner Bytes 共享同一 payload 通过引用计数协调释放。
- 别名拷贝零开销（不动 control block 引用计数？取决于实现细节，多数是 nop）。
- `loan_internal` 是"vlink 不要管"语义；调用方完全负责归还。
- `to_ptr<T>` 是 `reinterpret_cast`：不检查类型，错用会 UB。

## 配图

无专属配图。Bytes 内部结构图在 `../bytes_basic/images/bytes-sbo-vs-heap.png`。

## 参考

- `../bytes_basic/` — Bytes 构造、SBO、访问器
- `../bytes_advanced/` — 压缩、Base64、CRC、十六进制
- `../../zerocopy/` — 共享内存零拷贝传输示例
- `vlink/include/vlink/base/bytes.h` — Bytes 完整接口
- 顶层 `doc/10-zerocopy.md` — 零拷贝机制全景
