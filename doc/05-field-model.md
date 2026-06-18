# 5. Field 模型（Setter / Getter）

字段模型是 VLink 三种通信模型之一，用于在节点之间**同步最新状态值**。Node 基类的通用 API（init / deinit / attach / set_property 等）请参阅 [节点基类与生命周期](02-node-lifecycle.md)。

## 目录

- [5.1 概念介绍](#51-概念介绍)
- [5.2 与 Event 模型的区别](#52-与-event-模型的区别)
- [5.3 适用场景](#53-适用场景)
- [5.4 Setter\<T\> 完整 API](#54-settert-完整-api)
- [5.5 Getter\<T\> 完整 API](#55-gettert-完整-api)
- [5.6 std::optional\<T\> 返回值说明](#56-stdoptionalt-返回值说明)
- [5.7 完整使用示例](#57-完整使用示例)
- [5.8 多 Getter 读取同一 Setter](#58-多-getter-读取同一-setter)
- [5.9 安全模式](#59-安全模式)
- [5.10 性能特性](#510-性能特性)
- [5.11 相关文档](#511-相关文档)

---

## 5.1 概念介绍

字段模型（Field Model）是 VLink 三大通信模型之一，用于在节点之间**同步最新状态值**。
与事件模型（Event Model）不同，字段模型不关心历史消息队列，只维护一个"当前最新值"。

![字段模型数据流](images/field-dataflow.png)

**核心特性：**

- **最新值缓存**：Setter 每次调用 `set()` 都会将新值存入内部缓存，并广播给所有已连接的 Getter。
- **迟到 Getter 同步（后端相关）**：Setter 会注册 `sync()` 回调；支持该机制的传输后端在新 Getter 连接后可触发缓存值重发。部分后端的 `sync()` 当前是空实现，不能假设所有传输都自动补发最新值。
- **轮询或监听两种读取方式**：Getter 支持 `get()`（主动轮询）和 `listen()`（被动回调）两种读取方式，也支持阻塞等待 `wait_for_value()`。
- **变化过滤**：Getter 支持 `set_change_reporting(true)`，当新值与上次相同时（原始字节比较），不触发回调，降低 CPU 占用。

---

## 5.2 与 Event 模型的区别

三种通信模型的对比请参阅 [Event 模型](03-event-model.md) 第 1 节。

字段模型与事件模型的核心区别在于：字段模型在 Setter 侧维护**最新值缓存**，支持 sync 的后端可在 Getter 连接后触发当前值重发；不支持或空实现 sync 的后端不会自动补发。事件模型默认不保留历史消息，迟到的 Subscriber 会错过已发布的消息，除非所选后端与 QoS 额外提供历史缓存。

---

## 5.3 适用场景

字段模型非常适合以下场景：

**1. 配置参数同步**

```cpp
vlink::Setter<int> max_speed_setter("shm://config/max_speed");
max_speed_setter.set(120);

vlink::Getter<int> max_speed_getter("shm://config/max_speed");

if (auto v = max_speed_getter.get()) {
    use_max_speed(*v);
}
```

**2. 传感器最新值**

```cpp
vlink::Setter<float> speed_setter("dds://vehicle/speed");
speed_setter.set(current_speed_kph);

vlink::Getter<float> speed_getter("dds://vehicle/speed");
speed_getter.listen([](const float& v) {
    apply_speed_control(v);
});
```

**3. 系统状态同步**

```cpp
vlink::Setter<bool> system_ready_setter("shm://system/ready");
system_ready_setter.set(true);

vlink::Getter<bool> ready_getter("shm://system/ready");

if (ready_getter.wait_for_value()) {
    if (ready_getter.get().value_or(false)) {
        start_work();
    }
}
```

**4. HMI 显示最新数据**

```cpp
vlink::Setter<DashboardData> dash_setter("dds://hmi/dashboard");

vlink::Getter<DashboardData> dash_getter("dds://hmi/dashboard");
dash_getter.set_change_reporting(true);
dash_getter.listen([](const DashboardData& d) {
    refresh_display(d);
});
```

---

## 5.4 Setter\<T\> 完整 API

### 5.4.1 类声明

```cpp
template <typename ValueT, SecurityType SecT = SecurityType::kWithoutSecurity>
class Setter : public Node<SetterImpl, SecT>;
```

- `ValueT`：字段值类型，必须满足 `Serializer::is_supported()`。
- `SecT`：安全模式，默认无安全加密。

### 5.4.2 类型别名

| 别名         | 类型                                   | 说明               |
| ------------ | -------------------------------------- | ------------------ |
| `UniquePtr`  | `std::unique_ptr<Setter<ValueT, SecT>>` | unique_ptr 别名    |
| `SharedPtr`  | `std::shared_ptr<Setter<ValueT, SecT>>` | shared_ptr 别名    |

### 5.4.3 静态常量

| 常量           | 类型                  | 值         | 说明                              |
| -------------- | --------------------- | ---------- | --------------------------------- |
| `kImplType`    | `ImplType`            | `kSetter`  | 节点角色标识                      |
| `kValueType`   | `Serializer::Type`    | 编译期推断 | ValueT 对应的序列化类型枚举       |

### 5.4.4 工厂方法

| 方法 | 说明 |
| ---- | ---- |
| `create_unique` | 创建 `unique_ptr` 包装的 Setter |
| `create_shared` | 创建 `shared_ptr` 包装的 Setter |

```cpp
[[nodiscard]] static UniquePtr create_unique(
    const std::string& url_str,
    InitType type = InitType::kWithInit);

[[nodiscard]] static SharedPtr create_shared(
    const std::string& url_str,
    InitType type = InitType::kWithInit);
```

### 5.4.5 构造函数

| 重载 | 说明 |
| ---- | ---- |
| `Setter(url_str, type)` | 从 URL 字符串构造 |
| `Setter(conf, type)` | 从传输配置对象构造（`ConfT` 必须继承 `Conf`） |

```cpp
explicit Setter(const std::string& url_str,
                InitType type = InitType::kWithInit);

template <typename ConfT, typename = std::enable_if_t<std::is_base_of_v<Conf, ConfT>>>
explicit Setter(const ConfT& conf,
                InitType type = InitType::kWithInit);
```

`InitType::kWithInit`（默认）表示构造时立即调用 `init()` 完成初始化；若传入 `kWithoutInit`，需手动调用 `init()`。

### 5.4.6 核心方法

写入新的字段值，广播给所有已连接的 Getter。

```cpp
void set(const ValueT& value);
```

`set()` 内部行为：

1. 加互斥锁，将 `value` 存入内部 `value_` 缓存。
2. 释放互斥锁。
3. 在锁外把值序列化为 `Bytes`（若启用安全，再对序列化结果加密）。
4. 通过传输层写入，通知所有已连接的 Getter。

当新 Getter 连接时，若传输后端实现了 `sync()` 触发路径，Setter 会重新发送缓存的 `value_`。DDS、DDSR、DDSC、DDST、SOME/IP 等后端当前的 `sync()` 是空实现，迟到 Getter 需要依赖后续 `set()` 或主动 `get()`。

### 5.4.7 角色切换方法

将此 Setter 角色切换为 `kPublisher`（事件发布者语义），适用于某些传输后端不区分 Setter/Publisher 的场景。

```cpp
void mark_as_publisher();
```

---

## 5.5 Getter\<T\> 完整 API

### 5.5.1 类声明

```cpp
template <typename ValueT, SecurityType SecT = SecurityType::kWithoutSecurity>
class Getter : public Node<GetterImpl, SecT>;
```

### 5.5.2 类型别名

| 别名           | 类型                                          | 说明                   |
| -------------- | --------------------------------------------- | ---------------------- |
| `UniquePtr`    | `std::unique_ptr<Getter<ValueT, SecT>>`        | unique_ptr 别名        |
| `SharedPtr`    | `std::shared_ptr<Getter<ValueT, SecT>>`        | shared_ptr 别名        |
| `MsgCallback`  | `vlink::Function<void(const ValueT&)>`         | 值变更回调函数类型     |

### 5.5.3 静态常量

| 常量           | 类型               | 值        | 说明                        |
| -------------- | ------------------ | --------- | --------------------------- |
| `kImplType`    | `ImplType`         | `kGetter` | 节点角色标识                |
| `kValueType`   | `Serializer::Type` | 编译期推断 | ValueT 对应的序列化类型枚举 |

### 5.5.4 工厂方法

```cpp
[[nodiscard]] static UniquePtr create_unique(
    const std::string& url_str,
    InitType type = InitType::kWithInit);

[[nodiscard]] static SharedPtr create_shared(
    const std::string& url_str,
    InitType type = InitType::kWithInit);
```

### 5.5.5 构造函数

```cpp
explicit Getter(const std::string& url_str,
                InitType type = InitType::kWithInit);

template <typename ConfT, typename = std::enable_if_t<std::is_base_of_v<Conf, ConfT>>>
explicit Getter(const ConfT& conf,
                InitType type = InitType::kWithInit);
```

### 5.5.6 读取方法

| 方法 | 说明 |
| ---- | ---- |
| `get()` | 主动轮询：返回最新缓存值；尚未收到任何值时返回 `std::nullopt`。内部持有 `std::mutex`，返回的是 `optional<ValueT>` 的副本 |
| `wait_for_value(timeout)` | 阻塞等待：直到收到值或超时/中断。默认超时 `Timeout::kDefaultInterval = 5'000ms`（5 秒）；`timeout == 0` 将打印警告并退化为无限等待。返回 `true` 表示有值可读；`false` 表示超时或被 `interrupt()` 中断 |

```cpp
[[nodiscard]] std::optional<ValueT> get() const;

bool wait_for_value(
    std::chrono::milliseconds timeout = Timeout::kDefaultInterval);
```

### 5.5.7 监听方法

注册值变更回调，每次 Setter 写入新值时触发。若启用了 `change_reporting`，重复值不触发回调。只能调用一次，多次调用为致命错误。

```cpp
bool listen(MsgCallback&& callback);
```

### 5.5.8 配置方法

| 方法 | 说明 |
| ---- | ---- |
| `set_change_reporting(enable)` | 启用/禁用变化过滤：仅当新值原始字节与上次不同时才触发回调 |
| `get_change_reporting()` | 返回当前变化过滤状态 |
| `set_manual_unloan(manual_unloan)` | 启用/禁用零拷贝手动 unloan 模式 |
| `set_latency_and_lost_enabled(enable)` | 启用/禁用端到端延迟和丢包统计 |

```cpp
void set_change_reporting(bool enable);

[[nodiscard]] bool get_change_reporting() const;
```

> change_reporting 的比较基于原始序列化字节，由 `last_cache_` 成员保存。线程安全由 `Getter` 内部的 `std::mutex mtx_` 保证。

```cpp
void set_manual_unloan(bool manual_unloan) override;

void set_latency_and_lost_enabled(bool enable);
```

### 5.5.9 统计/诊断方法

| 方法 | 说明 |
| ---- | ---- |
| `is_latency_and_lost_enabled()` | 返回是否已启用延迟和丢包统计 |
| `get_latency()` | 返回最近一次测量的端到端延迟（纳秒），未启用时返回 0 |
| `get_lost()` | 返回累计样本统计（`total` 预期数量，`lost` 丢失数量） |

```cpp
[[nodiscard]] bool is_latency_and_lost_enabled() const;

[[nodiscard]] int64_t get_latency() const;

[[nodiscard]] SampleLostInfo get_lost() const;
```

### 5.5.10 继承自 Node 的公共 API

Node 基类继承的公共 API（init / deinit / attach / interrupt 等）请参阅 [节点基类与生命周期](02-node-lifecycle.md)。

### 5.5.11 角色切换方法

将此 Getter 角色切换为 `kSubscriber`（事件订阅者语义）。

```cpp
void mark_as_subscriber();
```

---

## 5.6 std::optional\<T\> 返回值说明

`Getter::get()` 返回 `std::optional<ValueT>`，而非直接返回 `ValueT`。这是因为 Getter 在初始化后、Setter 首次写入值之前处于"无值状态"。

```cpp
vlink::Getter<int> getter("shm://my_field");

auto v1 = getter.get();

if (!v1.has_value()) {
}

auto v2 = getter.get();

if (v2) {
    int current = *v2;
    int current2 = v2.value();
}

int val = getter.get().value_or(0);

if (getter.wait_for_value(std::chrono::seconds(5))) {
    int val2 = *getter.get();
}
```

**注意事项：**

- `get()` 是线程安全的（内部持有互斥锁）。
- `get()` 返回缓存值的副本（一次拷贝构造）；对持有堆资源的类型（Protobuf、含 `std::string`/`std::vector` 的结构）等价于深拷贝，调用频率高时需考虑代价。
- 若只需在值变化时触发动作，优先使用 `listen()` 回调而非频繁轮询 `get()`。

---

## 5.7 完整使用示例

### 5.7.1 示例 1：基础 Setter / Getter（轮询方式）

```cpp
#include <vlink/vlink.h>
#include <thread>
#include <chrono>
#include <iostream>

int main() {
    vlink::Setter<float> setter("shm://vehicle/speed");
    setter.set(60.5f);

    vlink::Getter<float> getter("shm://vehicle/speed");

    std::this_thread::sleep_for(std::chrono::milliseconds(10));

    if (auto v = getter.get()) {
        std::cout << "当前车速: " << *v << " km/h" << std::endl;
    } else {
        std::cout << "暂无值" << std::endl;
    }

    return 0;
}
```

### 5.7.2 示例 2：listen 回调方式（变化监听）

```cpp
#include <vlink/vlink.h>
#include <thread>
#include <chrono>

int main() {
    vlink::Getter<int> getter("dds://config/max_retry");
    getter.set_change_reporting(true);

    getter.listen([](const int& v) {
        VLOG_I("max_retry 更新为:", v);
    });

    vlink::Setter<int> setter("dds://config/max_retry");

    std::thread writer([&setter]() {
        setter.set(3);
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        setter.set(3);
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        setter.set(5);
    });

    writer.join();
    return 0;
}
```

### 5.7.3 示例 3：wait_for_value 阻塞等待

```cpp
#include <vlink/vlink.h>
#include <thread>
#include <chrono>
#include <iostream>

int main() {
    vlink::Getter<std::string> getter("dds://system/config_version");

    std::thread reader([&getter]() {
        std::cout << "等待配置版本..." << std::endl;

        if (getter.wait_for_value(std::chrono::milliseconds(10000))) {
            auto v = getter.get();
            std::cout << "配置版本: " << v.value_or("unknown") << std::endl;
        } else {
            std::cout << "等待超时" << std::endl;
        }
    });

    std::this_thread::sleep_for(std::chrono::seconds(2));
    vlink::Setter<std::string> setter("dds://system/config_version");
    setter.set("v2.1.0");

    reader.join();
    return 0;
}
```

### 5.7.4 示例 4：Protobuf 类型的字段模型

```cpp
#include <vlink/vlink.h>
#include <iostream>
#include "vehicle_state.pb.h"

int main() {
    vlink::Setter<vehicle::State> state_setter("shm://vehicle/state");

    vehicle::State state;
    state.set_speed(80.0f);
    state.set_gear(3);
    state.set_engine_running(true);
    state_setter.set(state);

    vlink::Getter<vehicle::State> state_getter("shm://vehicle/state");
    state_getter.listen([](const vehicle::State& s) {
        if (s.speed() > 100.0f) {
            trigger_speed_warning();
        }
    });

    if (auto v = state_getter.get()) {
        std::cout << "Speed: " << v->speed() << std::endl;
    }

    return 0;
}
```

### 5.7.5 示例 5：使用配置对象（ShmConf）

```cpp
#include <vlink/vlink.h>
#include <vlink/modules/shm_conf.h>
#include <iostream>

int main() {
    vlink::ShmConf::init_runtime("my_app");

    vlink::ShmConf setter_conf("vehicle/gear", "", 0, 0, 1);
    vlink::Setter<int> setter(setter_conf);
    setter.set(2);

    vlink::ShmConf getter_conf("vehicle/gear", "", 0, 0, 1);
    vlink::Getter<int> getter(getter_conf);

    if (auto v = getter.get()) {
        std::cout << "Gear: " << *v << std::endl;
    }

    vlink::ShmConf::deinit_runtime();
    return 0;
}
```

### 5.7.6 示例 6：Bytes 类型的字段模型（含安全加密）

```cpp
#include <vlink/vlink.h>

int main() {
    vlink::Security::Config cfg;
    cfg.key = "my-secret";

    vlink::SecuritySetter<vlink::Bytes> setter("shm://example_raw/field", cfg);
    setter.set(vlink::Bytes{0xA, 0xB, 0xC});

    vlink::SecurityGetter<vlink::Bytes> getter("shm://example_raw/field", cfg);

    if (auto ret = getter.get()) {
        VLOG_I("Getter value:", ret.value());
    }

    return 0;
}
```

---

## 5.8 多 Getter 读取同一 Setter

字段模型天然支持 **N:N 拓扑**：同一 URL 可以有多个 Setter 和多个 Getter；最常见的形态是一个 Setter 对应多个 Getter。支持 `sync()` 的后端可在 Getter 连接后补发当前值；不支持该触发路径的后端需要等待下一次 `set()` 或由 Getter 主动读取：

```cpp
#include <vlink/vlink.h>
#include <thread>
#include <chrono>
#include <vector>

int main() {
    vlink::Setter<double> temp_setter("dds://sensor/temperature");
    temp_setter.set(25.6);

    auto start_getter = [](int id) {
        std::this_thread::sleep_for(std::chrono::milliseconds(id * 100));

        vlink::Getter<double> getter("dds://sensor/temperature");

        if (getter.wait_for_value(std::chrono::milliseconds(1000))) {
            auto v = getter.get();
            VLOG_I("Getter", id, "收到温度:", v.value_or(-1.0));
        }
    };

    std::vector<std::thread> threads;
    for (int i = 1; i <= 3; ++i) {
        threads.emplace_back(start_getter, i);
    }

    for (auto& t : threads) {
        t.join();
    }

    return 0;
}
```

**关键点：**

- 每个 Getter 实例独立维护自己的内部值缓存和回调。
- 多 Getter 并发调用 `get()` 是线程安全的（各自有独立的互斥锁）。
- 若 Setter 在多 Getter 运行期间持续 `set()`，每个 Getter 的 `listen()` 回调均独立触发。
- 若启用 `set_change_reporting(true)`，每个 Getter 实例独立判断是否"变化"（基于各自的 `last_cache_`）。

![多 Getter 扇出模式](images/field-multi-getter.png)

---

## 5.9 安全模式

| 别名 | 等价形式 |
| ---- | -------- |
| `SecuritySetter<ValueT>` | `Setter<ValueT, SecurityType::kWithSecurity>` |
| `SecurityGetter<ValueT>` | `Getter<ValueT, SecurityType::kWithSecurity>` |

```cpp
template <typename ValueT>
class SecuritySetter : public Setter<ValueT, SecurityType::kWithSecurity>;

template <typename ValueT>
class SecurityGetter : public Getter<ValueT, SecurityType::kWithSecurity>;
```

```cpp
vlink::Security::Config cfg;
cfg.key = "my-secret";

vlink::SecuritySetter<MyMsg> setter("dds://secure/field", cfg);
vlink::SecurityGetter<MyMsg> getter("dds://secure/field", cfg);
```

完整安全加密配置请参阅 [安全加密](09-security.md)。

---

## 5.10 性能特性

| 指标               | 说明                                                                     |
| ------------------ | ------------------------------------------------------------------------ |
| 延迟               | 取决于传输后端：`shm://` 微秒级，`dds://` 百微秒至毫秒级                 |
| `get()` 开销       | 一次互斥锁 + 值拷贝；大型消息频繁轮询时注意拷贝开销                      |
| `listen()` 开销    | 回调在传输线程执行，避免在回调中做耗时阻塞操作                           |
| `set_change_reporting` | 启用后增加每次到达时的字节级比较开销，但可大幅减少回调触发次数       |
| 迟到 Getter 同步   | 后端实现 `sync()` 时会重发一次缓存值，开销等同于一次普通 `set()` 调用；空实现后端不会自动补发 |
| 内存               | Getter 内部 `value_` 为 `std::optional<ValueT>`，生命期内始终持有最新值  |
| 线程安全           | `get()` / `set()` 的值访问由互斥锁保护；`listen()` 会更新回调指针和监听状态，避免与并发重配回调交错调用 |

**性能建议：**

- 对于高频更新（> 1kHz）的字段，优先使用 `shm://` 或 `intra://` 后端（`intra://` 支持字段模型，且延迟最低，但仅限同进程内使用）。
- 若多模块只需在值变化时响应，使用 `listen()` + `set_change_reporting(true)` 比定时轮询 `get()` 效率更高。
- 对于 POD 类型（如 `int`、`float`、简单结构体），`set()`/`get()` 开销极低，几乎等同于内存拷贝。
- 若需要延迟监控，调用 `set_latency_and_lost_enabled(true)` 后通过 `get_latency()` 获取纳秒级端到端延迟。

---

## 5.11 相关文档

- [节点基类与生命周期](02-node-lifecycle.md) -- Node 通用 API（init / deinit / attach / security 等）
- [Event 模型（Publisher / Subscriber）](03-event-model.md) -- 事件发布订阅通信
- [Method 模型（Client / Server）](04-method-model.md) -- RPC 请求响应通信
