# url_guide — VLink URL 书写指南

vlink 通过 URL 把"业务话题"和"传输后端"两层概念解耦。本目录只放一个聚焦的示例 —— `url_basics/`，详细演示 URL 的解剖、`UrlParser` 用法、各传输的 URL 实例、运行时重映射。

各传输后端的完整端到端示例（DDS / 共享内存 / SOME/IP / FDBus / Zenoh / MQTT 等）放在 `../samples/` 下；每个 sample 都展示了对应后端 URL 的真实写法、所需守护进程、可调参数。

## 子示例索引

| 示例 | 主题 | 关键类 |
|------|------|--------|
| `url_basics/` | URL 解剖、`UrlParser`、静态分类器、传输无关 pub/sub | `vlink::UrlParser`、`vlink::Url::is_local_type` 等 |

## URL 通用形状

```
<transport>://<host>[/path][?key=value&key=value][#fragment]
```

| 字段 | 含义 |
|------|------|
| transport | 传输后端：`intra` / `shm` / `shm2` / `dds` / `ddsc` / `ddsr` / `ddst` / `zenoh` / `someip` / `fdbus` / `qnx` / `mqtt` |
| host | 主话题/服务标识符 |
| path | 二级话题路径 |
| query | 后端特定参数（domain、depth、qos、event、field、groups…） |
| fragment | 模式提示（`queue` / `direct` / `svc` / broker URI 等，按后端约定） |

`query` 字符串的 key 在不同后端的含义各异：

- `dds://` / `ddsc://`：`domain`、`depth`、`history`、`qos`、`reliability` …
- `shm://`：`domain`、`depth`、`history`、`wait` …
- `someip://`：`groups`、`event`、`field`、`method`、`major`、`minor` …
- `mqtt://`：`qos`（MQTT QoS 0/1/2），fragment 携带 broker 地址（如 `#tcp://broker:1883`）。
- `fdbus://`：`event`、`field`、`method`，fragment 携带 service 名（如 `#svc`）。

完整参数列表见 `doc/07-transport.md`。

## 推荐阅读顺序

1. **`url_basics/`** —— 必看。理解 URL 字段、`UrlParser` 接口、各传输的实例 URL。
2. **`samples/<transport>/`** —— 在 url_basics 看完后，进各传输的样例里看真实运行时配置：
   - `samples/shm_raw/` —— `shm://` 共享内存
   - `samples/dds_dynamic/`、`dds_idl/`、`ddsc_proto/` —— DDS 家族
   - `samples/someip_flat/` —— SOME/IP
   - `samples/fdbus_proto/` —— FDBus

## 运行时重映射（VLINK_URL_REMAP）

vlink 支持在不修改代码的前提下，通过环境变量 `VLINK_URL_REMAP` 指向一个 JSON 文件，把 URL 前缀映射到另一个：

```bash
export VLINK_URL_REMAP=/etc/vlink/remap.json
```

JSON 形如：

```json
{
  "dds://vehicle/speed": "shm://vehicle/speed",
  "ddsc://": "dds://"
}
```

应用层 Publisher/Subscriber 使用 `dds://vehicle/speed`，vlink 在创建节点时把它替换为 `shm://vehicle/speed`。典型场景：

- 灰度切换：把生产环境某 topic 从 DDS 切到 shm 不重启业务。
- 测试环境：把所有 `dds://` 重映射到 `intra://`，免去 DDS 部署。
- 调试：把 topic 重定向到本地 fallback。

## 共同前置知识

- `../quickstart/` —— vlink 三种通信原语的基础用法。
- `../communication/` —— 三种模型的进阶用法。

## 配图

无专属配图。URL 在 vlink 总体架构中的位置见 `doc/00-whitepaper.md`。

## 参考

- 顶层 `doc/07-transport.md` —— 各传输后端的 URL 规则与 query 参数完整列表。
- 顶层 `doc/21-environment-vars.md` —— `VLINK_URL_REMAP` 等运行时环境变量。
- `../samples/` —— 每个传输后端的端到端样例。
- `vlink/include/vlink/impl/url_parser.h` —— `UrlParser` 接口（内部头，仅作参考）。
