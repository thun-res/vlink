# Proxy Mock 测试

权威说明见 `doc/12-observability.md` §12.8–§12.19,入口为
`proxy/proxy.cc` 和 target `vlink-proxy`。

## 1. 参数与边界

- 执行 `--help`、`--version`。
- 对 `domain_id` 的负值和大于 255、非法 `max_packet_size` 等实现中
  已验证的边界分别检查非零退出和错误文本;仅在构建启用
  `VLINK_SUPPORT_SHM` 时检查非法 `iox_monitoring`。
- 不臆造源码未实现的严格校验;边界集合以当前 `proxy.cc` 为准。

## 2. 生命周期

使用与单元测试 runner 相同的最小本机入口:

```bash
vlink-proxy -c -n -m off
```

将 stdout/stderr 写入临时日志,记录 PID,确认进程未提前退出。随后启动
第二个同配置实例,核对 singleton 拒绝;最后向第一个实例发送正常终止
信号并等待退出。只操作本次记录的 PID。

## 3. 发现与数据链

按从轻到重执行:

1. 启动本次测试生产者/订阅者,用 `vlink-list -n` 核对节点和 URL。
2. 用 `vlink-monitor --plain` 核对目标 URL 出现并产生有限统计。
3. 使用用户选择的本机后端验证 Proxy 能看到载荷;记录 URL、序列化类型、
   帧数与代理配置。
4. 需要 bag 时,由 `vlink-bag play` 回放指定数据集,再核对发现、监控或
   WebViz 消费端收到数据。

第 4 项不是默认安全步骤:`play` 会按 bag 元数据中的原始 URL 发布。执行
前审计全部 scheme,并把非本机后端放入用户明确授权的隔离测试环境。

默认不启用公网绑定、跨网段 peer、TLS key、runnable 插件或外部 DDS/
MQTT/SOME-IP 服务。用户明确要求这些场景时,逐项说明地址、凭据来源、
外部进程与清理边界;secret 不写日志。

## 4. 判定

进程存活只是启动冒烟通过,不等于发现或数据转发通过。报告分别给出参数
解析、生命周期、发现、数据转发和关闭五类结果;缺少生产者或数据集时
不得把后两类写成通过。
