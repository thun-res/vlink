# bytes_basic — `vlink::Bytes` 的创建、视图与容量管理

`Bytes` 是 vlink 内部最常出现的字节载体：Publisher/Subscriber 的回调、Bag 写入、proxy 监控、安全模块的密文，统统以它为单位。本示例从最基础的"创建一个缓冲、读它、写它"开始，让读者熟悉 `Bytes` 的对象布局、SBO 与堆的切换边界、`offset` 预留区，以及 `resize` / `reserve` / `shrink_to` 的语义差异。读完之后你应当能回答两个问题：为什么 `Bytes` 总是 128 字节、`data()` 和 `real_data()` 的区别在哪里。

## 背景与适用场景

vlink 在通信热路径上不希望频繁触发 `malloc/free`，又不愿意把 API 写成"传入指针 + 长度"这种 C 风格。`Bytes` 的设计就是答案：它本身是一个固定 128 字节的值类型，里面塞了一个 96 字节的内联缓冲（Small Buffer Optimisation）和若干元数据；只要载荷不超过 96 字节，整个对象就放在栈上，零分配。

超过 96 字节时，`Bytes` 会向 `MemoryPool::global_instance()` 申请缓冲；池的分级配置由环境变量 `VLINK_MEMORY_LEVEL`（0..9，默认 3）决定。0 表示完全旁路，等价于直接走 `operator new`。

`Bytes` 还在缓冲前面预留了一段 `offset` 区域，用于让传输层（DDS、Iceoryx、TCP）直接在原地拼上协议头，避免再分配一次。`data()` 永远返回"用户载荷起点"（`real_data() + offset()`），而 `real_data()` 指向整段后备缓冲的真起点。这一点在写 transport 时几乎必用，普通业务读 `data()` 即可。

如果你想要的是"指针 + 长度的不可变视图"，去看 `../bytes_zerocopy/`；如果你想压缩、Base64、CRC，请看 `../bytes_advanced/`。本示例只覆盖最基础的拥有式缓冲。

## 核心 API

| API | 签名 | 说明 |
|-----|------|------|
| `Bytes::create` | `static Bytes create(size_t size, uint8_t offset = 0) noexcept` | 申请一段拥有式缓冲；内容未初始化 |
| `Bytes::from_string` | `static Bytes from_string(const std::string& str, uint8_t offset = 0) noexcept` | 深拷贝字符串内容 |
| `Bytes::stack_size` | `static constexpr uint8_t stack_size() noexcept` | 返回内联 SBO 容量（96） |
| `Bytes(const std::initializer_list<uint8_t>&)` | `Bytes(const std::initializer_list<uint8_t>& list) noexcept` | 字节字面量构造 |
| `Bytes(const std::vector<uint8_t>&)` | `explicit Bytes(const std::vector<uint8_t>& data) noexcept` | 深拷贝 vector |
| `data()` / `size()` | `uint8_t* data() noexcept` / `size_t size() const noexcept` | 用户载荷区起点和长度 |
| `real_data()` / `real_size()` | `uint8_t* real_data() noexcept` / `size_t real_size() const noexcept` | 后备缓冲整段起点和长度（含 offset 前缀） |
| `offset()` | `uint8_t offset() const noexcept` | 前缀预留区长度 |
| `capacity()` | `size_t capacity() const noexcept` | 后备缓冲实际分配字节数 |
| `is_owner()` / `empty()` | `bool is_owner() const noexcept` / `bool empty() const noexcept` | 所有权与空判断 |
| `reserve(n)` | `bool reserve(size_t new_capacity) noexcept` | 扩容（仅 owner 有效，非 owner 返回 `false`） |
| `resize(n)` | `bool resize(size_t size) noexcept` | 改变逻辑 `size`，必要时调用 `reserve`，新字节内容未初始化 |
| `shrink_to(n)` | `bool shrink_to(size_t size) noexcept` | 仅缩减逻辑 `size`，不释放底层内存 |
| `clear()` | `void clear() noexcept` | 释放并重置为空状态 |
| `to_string()` | `std::string to_string() const noexcept` | 拷贝为 `std::string` |
| `operator[]` | `uint8_t& operator[](size_t index) noexcept` | 不做边界检查 |
| `begin()` / `end()` | `uint8_t* begin/end() noexcept` | range-for 支持 |
| `operator==(const Bytes&)` | `bool operator==(const Bytes&) const noexcept` | 按内容比较 |
| `is_little_endian()` / `is_big_endian()` | `static constexpr bool is_little_endian() noexcept` | 编译期端序探测 |

## 代码导读

### 1. SBO 路径：64 字节，零堆分配

申请 64 字节小于 `kStackSize`（96），整段载荷就放在 `Bytes` 对象内部的 `stack_data_` 数组里，`data()` 与 `real_data()` 都指向这段栈内存。后面对 `sbo[i]` 的写入只是普通的栈写，析构时也不会触发 `bytes_free`。

```cpp
auto sbo = Bytes::create(64);
print_info("SBO(64)", sbo);

for (size_t i = 0; i < sbo.size(); ++i) {
  sbo[i] = static_cast<uint8_t>(i & 0xFF);
}
```

`stack_size()` 是 `static constexpr`，可以放进 `static_assert`，便于在自己的类型上做容量裁剪。

### 2. 堆路径：256 字节超过阈值，向 `MemoryPool` 申请

```cpp
auto heap = Bytes::create(256);
print_info("Heap(256)", heap);
std::memset(heap.data(), 0xAB, heap.size());
```

打印中 `capacity()` 通常会大于 `size()`，因为分级池给出的是"覆盖该层的最大块"，并不是精确 256。`is_owner()` 仍然返回 `true`，析构时会归还给池。

### 3. `offset` 预留区：transport 的常见用法

```cpp
auto buf = Bytes::create(100, /*offset=*/8);
VLOG_I("real_size=", buf.real_size(),
       " data() == real_data()+offset: ",
       (buf.real_data() + buf.offset() == buf.data()));
```

这一段在传输层经常出现：业务侧写 100 字节载荷到 `data()`，传输层在前面 8 字节里写包头，整段 `real_data() + real_size()` 直接发出，零拷贝。

### 4. 从字符串 / 容器构造

```cpp
auto bytes = Bytes::from_string("Hello, VLink Bytes!");
std::vector<uint8_t> vec = {0x01, 0x02, 0x03, 0x04, 0x05};
Bytes from_vec(vec);
Bytes magic{0xCA, 0xFE, 0xBA, 0xBE};
```

三种构造都是深拷贝；`initializer_list` 版本特别适合写测试中的 magic 值。

### 5. range-for 与等值比较

`Bytes` 提供了 `begin()`/`end()`，可以直接用 range-for；`operator==` 按内容比较，长度不同直接 false，避免误把 `vector<uint8_t>` 与 `Bytes` 混用时出现假等。

```cpp
Bytes bytes{10, 20, 30, 40, 50};
for (uint8_t b : bytes) { /* ... */ }

auto a = Bytes::from_string("test");
auto b = Bytes::from_string("test");
VLOG_I("a == b: ", (a == b));
```

### 6. `resize` / `reserve` / `shrink_to` 三件套

这三个方法只对 owner 有效：

- `reserve(n)`：保证 `capacity() >= n`，逻辑 `size()` 不变。
- `resize(n)`：直接把逻辑 `size` 改成 `n`，必要时先 `reserve`；新增字节内容未初始化。
- `shrink_to(n)`：把 `size` 缩到 `n`，**不会**释放底层内存，方便复用同一段缓冲。

```cpp
auto buf = Bytes::create(10);
buf.reserve(200);             // 容量提升到 200，size 还是 10
(void)buf.resize(150);        // size 改为 150
(void)buf.shrink_to(50);      // size 缩到 50，capacity 仍然 >= 200
```

`shrink_to` 失败的常见原因是入参大于当前 `size()`（这时它不会"变大"）；想增大改用 `resize`。

### 7. 端序探测

`is_little_endian()` / `is_big_endian()` 是 `constexpr`，根据预定义宏在编译期决定。`Bytes` 本身不做端序转换，需要时配合 `reverse_order()`（见 `bytes_advanced/`）。

## 运行

```bash
./build/output/bin/example_bytes_basic
```

预期输出节选：

```
[INFO] === Bytes Basic Example ===
[INFO] SBO(64): size=64 capacity=96 offset=0 is_owner=1 empty=0
[INFO] stack_size=96 first=0 last=63
[INFO] Heap(256): size=256 capacity=256 offset=0 is_owner=1 empty=0
[INFO] first byte: 0xab
[INFO] create(100, offset=8): size=100 capacity=128 offset=8 is_owner=1 empty=0
[INFO] real_size=108 data() == real_data()+offset:  1
[INFO] from_string: size=19 capacity=96 offset=0 is_owner=1 empty=0
[INFO] to_string="Hello, VLink Bytes!"
[INFO] is_little_endian=1 is_big_endian=0
```

具体 `capacity` 数值与 `VLINK_MEMORY_LEVEL` 有关，但 SBO/堆切换的边界（96）是编译期常量。

## 常见陷阱

- 在非 owner（`shallow_copy` / `loan_internal`）上调用 `resize` / `reserve` / `shrink_to` 都会返回 `false`，需要先 `deep_copy_self()` 转成 owner。
- `Bytes` 的拷贝构造**总是**深拷贝；想保持别名语义必须显式调用 `Bytes::shallow_copy(const Bytes&)`，详见 `../bytes_zerocopy/`。
- `operator[]` 不做边界检查，越界访问行为未定义；测试时配合 sanitizer。
- `offset` 类型是 `uint8_t`，单次最多预留 255 字节前缀，超过这个量级请直接堆叠多个 `Bytes`。
- `data()` 在 `empty()` 时返回 `nullptr`，遍历前先判空，否则 range-for 会得到 `nullptr..nullptr` 退化为不进入循环（恰好"安全"，但隐藏 bug）。

## 设计要点

- `Bytes` 之所以固定 128 字节，是为了让它能整体走值传递（passing in registers + cache lines），同时把 96 字节的内联缓冲塞下 ROS2/DDS 中常见的小消息（标量、姿态、状态字段）。
- SBO 边界 96 是工程权衡：再大会让对象超过两个 cache line；再小就会让大量小消息掉到堆上。
- `resize` 不清零新字节，是为了避免一次额外的 `memset`；如果你需要 zero-init，自己 `std::memset` 或者编译时开启 `VLINK_BYTES_MEM_RESET`。
- `clear()` 永远走"释放并置空"，不是 `shrink_to(0)`；想保留容量只缩 size，请用 `shrink_to(0)`（不过对 0 没意义，通常是 `resize(0)`）。

## 配图

![Bytes SBO 与堆切换](./images/bytes-sbo-vs-heap.png)

图中标注了 128 字节对象布局：96 字节 SBO 区 + 元数据（`is_owner` / `is_loaned` / `offset` / `data_` / `size_` / `capacity_`）。`size <= 96` 时 `data_` 指向 `stack_data_`；超过时指向 `MemoryPool` 借来的堆块。

## 参考

- `../bytes_advanced/` —— 压缩、Base64、CRC、十六进制解析
- `../bytes_zerocopy/` —— 五种所有权模式与零拷贝包装
- `../memory_pool/` —— `Bytes` 默认走的分级池
- `include/vlink/base/bytes.h` —— `Bytes` 公共头
- `doc/10-zerocopy.md` —— 零拷贝设计说明
