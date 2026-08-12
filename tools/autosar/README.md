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

查看可选目标：

```bash
python3 tools/autosar/arxml_to_vlink_someip.py input.arxml --list-types
python3 tools/autosar/arxml_to_vlink_someip.py input.arxml --list-prototypes
```

## 支持范围

- Implementation、Standard/Custom C++ Implementation 及 Application 数据类型。
- Application 到 Implementation 类型映射和跨文件引用。
- 标量、枚举、结构体、别名、固定/变长/多维数组、vector、UTF-8 字符串和 `vlink::Bytes`。
- event、field、method call/return 及细粒度 data-prototype deployment。
- prototype `INIT-VALUE`；结构体生成静态 `make_default()`，其他类型保留独立 factory。
- `VLINK_SOMEIP_ENDIAN_BIG/LITTLE`。
- `VLINK_SOMEIP_LENGTH` 的 `0/1/2/4` 字节长度字段。
- `VLINK_SOMEIP_ARRAY_LENGTH` 的多维数组长度字段。
- `VLINK_SOMEIP_STRUCT_LENGTH` 的 `0/1/2/4` 字节结构长度字段。

当前 VLink serializer 不支持 TLV、optional、union、variant、bitfield、固定长度字符串、
UTF-16/UCS-2、字段级混合字节序和递归 by-value 类型。自动化生成建议使用 `--strict`。

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
