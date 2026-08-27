# ARXML to VLink SOME/IP

`arxml_to_vlink_someip.py` 将 AUTOSAR ARXML 数据类型及 SOME/IP deployment 生成 C++17
序列化结构体。工具仅依赖 Python 标准库。

## 使用

以下命令生成 [generated_someip_types.h](test/generated_someip_types.h)：

```bash
python3 tools/autosar/arxml_to_vlink_someip.py \
  tools/autosar/test/autosar_r25_11_someip_types.arxml \
  --prototype /VLink/ServiceInterfaces/VehicleStateService/VehicleStateEvent \
  --namespace vlink::autosar \
  --byte-arrays-as-bytes \
  --strict \
  --output tools/autosar/test/generated_someip_types.h
```

定长字符串与 TLV 组合 fixture 对应生成 [generated_someip_features.h](test/generated_someip_features.h)：

```bash
python3 tools/autosar/arxml_to_vlink_someip.py \
  tools/autosar/test/autosar_r25_11_someip_features.arxml \
  --prototype /Features/Service/Event \
  --prototype /Features/Service/TlvEvent \
  --prototype /Features/Service/StaticTlvEvent \
  --namespace vlink::autosar::features \
  --strict \
  --output tools/autosar/test/generated_someip_features.h
```

108 组 length width、端序与 alignment 闭环矩阵的基准头可这样重生成：

```bash
python3 tools/autosar/arxml_to_vlink_someip.py \
  tools/autosar/test/autosar_r25_11_someip_matrix.arxml \
  --prototype /Matrix/ServiceInterfaces/MatrixService/SimpleEvent \
  --namespace vlink::autosar::matrix \
  --strict \
  --output tools/autosar/test/generated_someip_matrix.h
```

UTF-16、map、union、optional TLV 与多维 TLV 的组合 fixture 对应生成
[generated_someip_advanced.h](test/generated_someip_advanced.h)：

```bash
python3 tools/autosar/arxml_to_vlink_someip.py \
  tools/autosar/test/autosar_r25_11_someip_advanced.arxml \
  --prototype /Advanced/ServiceInterfaces/AdvancedService/Event \
  --prototype /Advanced/ServiceInterfaces/AdvancedService/TlvEvent \
  --namespace vlink::autosar::advanced \
  --strict \
  --output tools/autosar/test/generated_someip_advanced.h
```

生成指定类型：

```bash
python3 tools/autosar/arxml_to_vlink_someip.py input.arxml \
  --type /VLink/ImplementationTypes/VehicleState \
  --namespace vlink::autosar \
  --output generated_someip_types.h
```

跨文件引用时依次传入所有 ARXML。`--type` 和 `--prototype` 均可重复使用，也接受唯一的
`SHORT-NAME`。

```bash
python3 tools/autosar/arxml_to_vlink_someip.py types.arxml service.arxml \
  --prototype /VLink/ServiceInterfaces/VehicleStateService/VehicleStateEvent \
  --output generated_someip_types.h
```

默认生成单个头文件。按类型或 AUTOSAR package 拆分时，`--output` 指向专用的已存在空目录；工具同时生成
`vlink_someip_types.h` 汇总入口，并让上层头只包含其直接依赖头。

```bash
python3 tools/autosar/arxml_to_vlink_someip.py types.arxml service.arxml deployment.arxml \
  --prototype /VLink/ServiceInterfaces/VehicleStateService/VehicleStateEvent \
  --split-by type \
  --output generated/

python3 tools/autosar/arxml_to_vlink_someip.py types.arxml \
  --type /VLink/ImplementationTypes/VehicleState \
  --split-by package \
  --output generated/
```

查看可选目标：

```bash
python3 tools/autosar/arxml_to_vlink_someip.py input.arxml --list-types
python3 tools/autosar/arxml_to_vlink_someip.py input.arxml --list-prototypes
```

## 支持范围

- Implementation、Standard/Custom C++ Implementation 及 Application 数据类型。
- AP service-interface 的无上下文歧义 Application 到 Implementation 类型映射和跨文件引用。
- 标量、枚举、结构体、别名、固定/变长/多维数组、vector、关联 map、union/variant、定长/变长
  UTF-8/UTF-16 字符串、`std::optional` TLV 成员和 `vlink::Bytes`。
- event、field、method call/return 及直接 ROOT/TARGET 的细粒度 data-prototype deployment；带
  `CONTEXT-DATA-PROTOTYPE-REFS` 的实例路径 deployment 会明确拒绝；prototype 顶层必须解析为结构体。
- prototype `INIT-VALUE`；结构体生成带来源注释的静态 `make_default()`，其他已支持的值规格保留带来源注释的独立
  factory；map、optional 和 variant 值规格暂不生成。
- `VLINK_SOMEIP_ENDIAN_BIG/LITTLE`。
- `VLINK_SOMEIP_LENGTH` 的 `1/2/4` 字节长度字段；固定 array 还可用 `0` 省略长度。
- `VLINK_SOMEIP_ARRAY_LENGTH` 的多维数组长度字段。
- `VLINK_SOMEIP_STRUCT_LENGTH` 的 `0/1/2/4` 字节结构长度字段。
- `VLINK_SOMEIP_UTF16_BE/LE`、`VLINK_SOMEIP_UNION`，以及含多维数组的 dynamic/static length TLV deployment。

部署细节见 `doc/03-serialization.md` §3.5.1。method deployment 会应用到各 argument 类型；完整 request/response
仍需调用方按方向和声明顺序聚合。TLV argument 的 tag 位于参数层，工具会直接拒绝；serializer 与工具均不支持
bitfield、字段级混合字节序/对齐和递归 by-value 类型。TLV union 仅支持默认 4 字节 selector；省略 length 的
union 当前仅支持等宽标量或枚举 alternatives。
带 `PORT-INTERFACE-TO-DATA-TYPE-MAPPING` 作用域的 Application 映射会明确拒绝，可显式选择
Implementation 类型。
CP `SOMEIP-TRANSFORMATION-I-SIGNAL-PROPS`、availability bitfield、variable-array profile，以及会隐藏
size indicator 或 selector 的 canonical mapped string/array/union wrapper 也会明确拒绝；工具不会把这些模型
当作普通结构体静默生成。
生成器会注释类型/字段的 AUTOSAR 引用、结构与字段 deployment、固定字符串 wire size，以及可表达的数组、
`vlink::Bytes` 和字符串上限；直接字段的 maximum 同时用于接收，字符串超限会拒绝，vector/Bytes 超限会保留前 N
项/字节并跳过余量。结构体存在容器上限时还会生成 `check_available()`，序列化前自动拒绝超限值；多维容器
maximum 暂不表达。
自动化生成建议使用 `--strict`。

完整参数见：

```bash
python3 tools/autosar/arxml_to_vlink_someip.py --help
```

## 测试

```bash
mkdir -p build-ai/arxml_to_vlink_someip/__pycache__
PYTHONPYCACHEPREFIX="$PWD/build-ai/arxml_to_vlink_someip/__pycache__" \
  python3 -m unittest tools/autosar/test/arxml_to_vlink_someip_test.py
```

设置 `AUTOSAR_R25_11_XSD=/path/to/AUTOSAR_00054.xsd` 后，测试会先用 `xmllint` 校验四份 committed
fixture 及 108 组部署矩阵。通过 CMake/CTest 运行时，矩阵与高级组合 fixture 还会链接当前构建的 `vlink`，
检查生成代码的精确 wire bytes、反序列化 round-trip、optional TLV 缺失语义和截断拒绝；直接运行 Python
unittest 时仍执行生成与 C++17 语法编译。
