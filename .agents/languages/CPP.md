# C++ 编码规范

根 `AGENTS.md` 的强制规则是上位边界。其范围内整体以 Google C++ 编码
风格为基础,排版以仓库 `.clang-format` 为准。本文只补充工具无法表达的
VLink C++ 细则;与外部风格冲突时以本文为准,未覆盖之处照搬相邻代码。
人工整改只处理当前任务范围;只有维护者明确调用 `/format` 时,才按该
skill 与 `tools/format.sh` 的既定范围执行全仓机械格式化。

## 1. 总原则：克制

每行代码都必须有当前需求:

- 只处理真实可达的错误,不堆重复校验、fallback、兜底或 try/catch。
- 没有第二个真实用例不抽 helper、基类或通用接口。
- 不预留未使用的模板参数、配置项、策略和扩展点。
- 删除本次引入或直接触及的死代码,不顺手清理无关历史。

## 2. 复用优先：VLink base 基础库

语义匹配且不受第三方接口或 ABI 约束时,先查
`include/vlink/base/`(索引见 `doc/08-base-library.md` §8.1):

| 需求 | 优先考虑 | 避免重复实现 |
| ---- | ---- | ---- |
| 类型擦除回调 | `Function` / `MoveFunction` | `std::function`、手写虚接口 |
| 字节缓冲 | `Bytes`(移动或显式 `shallow_copy`) | `vector<uint8_t>` 裸传、自制 buffer |
| 日志 | `Logger` 及日志宏 | `printf` / `iostream` / 新日志库 |
| 事件循环/定时 | `MessageLoop`、`Timer` | 手写 `std::thread` + sleep 轮询 |
| 并行 | `ThreadPool`、MultiLoop、graph_task | 自建线程池 |
| 条件变量 | `vlink::ConditionVariable` | `std::condition_variable`(_any) |
| 并发原语/IPC | `base/` 并发工具、`sys_semaphore`、`sys_sharemem` | 平台裸 API |
| 格式化 / UUID / 子进程 | `format.h`、`Uuid`、`Process` | 新依赖、手搓实现 |

只有能力缺失且已有多个真实消费者时才考虑补充 `base/`。

## 3. 文件骨架

第一方且已采用 VLink 模板的 C/C++ 源码与头文件
(`.h/.hpp/.c/.cc/.cpp/.cxx`)保留下面的许可证头,**逐字节一致**
(含空格与空行),禁止短 SPDX 形式或重排版。第三方、生成代码和保留
上游版权的文件维持原头;新建第一方文件按维护者确认的年份/作者从相邻
文件复制:

```cpp
/*
 * Copyright (C) 2026 by Thun Lu. All rights reserved.
 * Author: Thun Lu <thun.lu@zohomail.cn>
 * Repo:   https://github.com/thun-res/vlink
 *  _    __   __      _           __
 * | |  / /  / /     (_) ____    / /__
 * | | / /  / /     / / / __ \  / //_/
 * | |/ /  / /___  / / / / / / / ,<
 * |___/  /_____/ /_/ /_/ /_/ /_/|_|
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
```

公开头文件(**lib**,`internal/`、`impl/` 除外)在许可证头之后紧跟
Doxygen `@file` 块,要求见第 10 节。

## 4. 命名

- 模板参数以 `T` 结尾:`MsgT`、`TypeT`、`EnumT`、`UnderlyingT`、
  `SecT`,非类型参数同样(如 `size_t SboSizeT`);**单个字母的除外**
  (`typename T` 允许,不必写成 `TT`)。例:
  `template <Type TypeT, typename T>` 两者都合规。
- `constexpr` 常量用 `kCamelCase`——**函数局部的也一样**:
  `static constexpr size_t kHeaderSize = 16;`
- 宏:`VLINK_<MODULE>_<NAME>`(如 `VLINK_SHM_MAX_SEGMENTS`),禁止
  无前缀的项目宏。
- 禁止定义 std-trait 风格的 `_v` / `_t` 别名(如 `is_foo_v`);完整
  拼写 trait 或定义 `kIsFoo` 常量。

## 5. 类型与内存

### 5.1 类型与声明

- 整数/尺寸使用 `uint*_t`、`int*_t`、`size_t` 等裸类型;非拥有字符串
  参数优先 `std::string_view`。
- 非平凡对象的只读参数优先 `const T&`;调用方转移所有权或模板完美
  转发时优先 `T&&`,原地修改用 `T&`;仅廉价标量或明确值语义按值传参。
  不机械修改已发布 API。
- 锁类型使用 CTAD。新增或实质触及的 API 中,不可丢弃的查询、状态和
  资源返回值加 `[[nodiscard]]`。
- 文件级内部链接使用 `static`,不新增匿名命名空间;不为统一风格清理
  未触及的历史代码。

### 5.2 内存与所有权

- 能放栈上或作为成员就不上堆;无共享语义不用 `shared_ptr`,也不用它
  包装 `MoveFunction` 掩盖所有权问题。
- `Impl`/PIMPL 的命名、声明与定义位置、`final` 和 `impl_` 持有方式
  照搬当前模块相邻代码,不机械统一既有形态。`Impl` 结构体尽量只保存
  状态,业务方法优先保留在外层类;特殊场景沿用相邻实现。

### 5.3 错误与异常

- 可预期失败沿用现有返回语义;只在既有契约允许且调用方可处理时抛出。
- 异常不得穿过 C ABI、线程入口或未允许异常的回调;禁止空 `catch`、
  丢失上下文或用异常做普通控制流。

## 6. 并发

- 禁止 `std::condition_variable` 及 `_any`,使用
  `vlink::ConditionVariable` 统一跨平台语义。
- 原子操作显式使用够用的最弱内存序:独立计数用 `relaxed`,发布/读取用
  `release`/`acquire`,双向 RMW 用 `acq_rel`;`seq_cst` 必须说明理由,
  不使用 `consume`。每个 `acquire` 必须有明确配对的 `release`。
- 不跨回调持锁,不在持锁区做 I/O 或分配;多个字段的共同不变量用 mutex,
  不强行拼接难以证明的无锁协议。

## 7. 语言标准与多平台

- 基线是 C++17;C++20 特性必须有 C++17 回退门控,优先复用
  `base/macros.h` / `base/helpers.h`,不得裸用高版本特性。
- 支持 Windows、Linux、macOS、QNX、Android。平台代码使用既有宏和
  `base/` 抽象,不得无门控调用单平台 API;核对编译器与类型差异。

## 8. 控制流版式

- 每个 `if` / `else if` / `else` 体必须带大括号,单行也不例外。
- `if` 的语法、初始化语句与条件表达式必须在 C++17 下编译,不得使用
  C++20 专属语法、类型或 API。`if constexpr` 属于 C++17,可以沿用。
- 用空行隔离独立逻辑段(获取 → 校验 → 变换 → 提交)。`if` /
  `else if` / `else` 连续分支视为一个控制流块,块前后与相邻语句之间
  必须留空行;紧邻作用域起止大括号时不额外增加空行。

## 9. 热路径性能

- 热路径包括 publish/subscribe、序列化/反序列化、`zerocopy` loan、
  `modules/*` 收发回调以及 `Bytes`、`Function`、`MessageLoop` 等
  `base/` 内核。
- 稳态数据路径优先做到无额外分配、无多余复制、无重复解析、无无条件
  格式化;新增工作必须能说明必要性。
- 改动时逐行审视分配、拷贝、缓存局部性、锁竞争、间接调用和分支倾向,
  不以“代码更方便”为由增加每消息固定开销。

### 9.1 拷贝纪律

- 多余拷贝按缺陷处理。`Bytes` 的拷贝构造、拷贝赋值与 `deep_copy`
  都是深拷贝;传递优先移动,零拷贝别名显式使用 `shallow_copy`。只有
  确需独立所有权且不能转移或复用缓冲时才深拷贝,并说明理由。
- 帧与其他载荷结构体按实际类型一律以 `const T&` 传参(需原地改写时
  `T&`),禁止按值传参。
- 检查 range-for 是否缺少引用、lambda 是否捕获整个载荷、返回值是否
  多余物化,以及 `Bytes`、string、vector 之间是否发生重复转换。
- `std::move` 只用于明确的所有权转移;不得对 `const` 对象使用后误认为
  已消除复制。

### 9.2 热路径自查清单

- 避免每消息堆分配和临时容器;复用缓冲或内存池,提前 `reserve`。
- 顺序处理的数据优先连续存储;没有稳定查找需求不引入节点容器、重复
  hash 或字符串键转换。
- 避免在每消息路径新增 `shared_ptr` 引用计数、临时所有权包装和重复
  原子操作;已有对象池或复用机制优先沿用。
- 将不随消息变化的解析、查表、formatter 创建和容量计算移出循环;
  不在收发路径重复构造相同状态。
- 缩小持锁区,不新增全局争用点;能用模板或 `if constexpr` 分发时避免
  虚调用和间接跳转,无证据不引入复杂无锁结构。
- 能合并的传输调用、系统调用和批量处理不拆成逐元素往返;同时保持现有
  错误语义与时序。
- 不在热路径无条件格式化日志,也不重复校验上游已保证的不变量;校验放
  在边界,内部用调试断言。

#### 9.2.1 分支预测

- 稳定成功路径合理、充分使用 `VLIKELY`;罕见错误、溢出和降级路径
  使用 `VUNLIKELY`。只标记有依据的明显偏斜分支。
- 保留既有提示;无证据不得批量添加、删除或反转。只使用项目宏。

### 9.3 `zerocopy` 线格式冻结

`include/vlink/zerocopy/` 结构体线格式已冻结:`sizeof` 即契约,无前后
兼容层。禁止增删、重排、改变字段大小;新设计必须一次性枚举全部字段,
并显式预留 `reserved` 字节。

### 9.4 日志宏与等级

- 新代码默认优先 `VLOG_*`;复杂嵌入格式推荐 `CLOG_*`;`MLOG_*` 只在
  相邻模块已使用 `{}` 格式时沿用。
- Trace/Debug 用于诊断,Info 用于低频正常状态,Warn 用于可恢复异常或
  降级,Error 用于当前操作失败;不得随意升降等级。
- 高频日志使用现有 `VLOG_*_EVERY_MS` 限频,逐帧热路径不打印 Info
  及以上日志。
- Fatal 只用于无法安全继续的错误,不得代替参数校验、可恢复失败或
  断言。

## 10. 注释

公开 API 的契约说明使用 Doxygen;除 `examples/` 外,`.cc` 不写说明性
注释。块形态先照搬相邻公开头文件。

### 10.1 块形态

- 复杂契约使用 `/** ... */` 多行块;简单 `@brief` 沿用相邻形态。
- `= delete` 的拷贝/移动成员不写注释。
- 标签组之间留空行,每行不超过 120 字符;参数描述、表格和 ASCII 图
  对齐。
- Doxygen Markdown 表格的分隔符与各列逐行对齐;超出 120 字符或被
  `clang-format` 重排时,缩短表头、单元格措辞或拆表,禁止使用
  `clang-format off/on` 规避。

### 10.2 标签顺序与用法

顺序:`@file`/`@class` → `@brief` → `@details` → `@par` →
`@code` → `@tparam` → `@param` → `@return` → `@note`/`@warning`。
`@brief` 是以句号结尾的单句;`@details` 只补充必要契约。回调别名也需
记录签名参数。

### 10.3 行内标记

- 代码标识符用 `@c`,参数用 `@p`,强调用 `@em`;模板尖括号转义。
- 正文使用英文,句间两个空格,破折号写 `--`。

### 10.4 作用域要求

- `include/vlink/` 除 `internal/`、`impl/` 外的公共头文件及其公开实体
  使用英文 Doxygen。
- 文件头 `@file` 块内容与复杂度相称,不为简单头文件堆叠表格或图。
- `private` 与纯实现 `protected` 不强制;派生类可见的扩展契约需说明。
- 除 `examples/` 外,`.cc` 不新增说明性注释、注释掉的代码或横幅;
  许可证头、`NOLINT`、`LCOV_EXCL_*`、`GCOVR_EXCL_*` 等工具必需指令
  保留。历史注释未进入当前改动范围时不得为统一风格机械清理。
- `examples/` 豁免上述 `.cc` 说明性注释禁令,可写必要的英文教学注释;
  测试不强制 Doxygen,以 `TEST_CASE` 命名表达意图。

## 11. Lint 指令

- 除非维护者明确要求修改配置,不得通过改 `.clang-tidy` /
  `.clang-format` 的检查项或参数来消音。
- **lib**:
  1. 能按仓库惯用写法修好的,改代码;
  2. 误报或有意为之的写法,优先在目标行前使用
     `// NOLINTNEXTLINE(<检查名>)`;只有不适合放在前一行时才使用同行
     `// NOLINT(<检查名>)`。两者都必须带具体检查名;
  3. 宏定义区/生成式代码块用 `// NOLINTBEGIN` ... `// NOLINTEND`
     成片包裹,范围缩到最小。
- **test**:每个 `test/**/*.cc` 在许可证头后用
  `NOLINTBEGIN`/`NOLINTEND` 包裹全文;该豁免仅限 `test/`。

## 12. `examples/` 专属规则

- 项目符号显式使用 `vlink::`,禁止项目命名空间的 using-directive;
  `std::chrono_literals` 等字面量命名空间可沿用相邻示例。helper 使用
  `static`,不写匿名命名空间。
- `.cc` 可写必要的英文教学注释,详细背景与完整流程写入对应 README。

## 13. Agent 工作流程

### 13.1 工作方式

- 大规模重构、构建与运行的授权边界以根 `AGENTS.md` 规则 3、8 为准,
  本分册不重复维护。
- 写公开头文件 Doxygen/头部块之前,先打开相邻文件照搬其形态;新增
  某类构件(宏、trait、helper)之前,先 grep 仓库里已有的做法。

### 13.2 API 改动的同步义务

同步触发与处置遵循根 `AGENTS.md` 规则 9;完整范围只在 `/sync-review`
维护,本分册不重复列举。
