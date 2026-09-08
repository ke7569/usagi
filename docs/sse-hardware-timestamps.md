# 上海硬件接收时间戳

硬件时间戳需要设备与接收 socket 两层同时启用。上海 journal 采集保持独立进程，
预测和交易无需负责配置网卡；journal + SHM 两阶段接续继续使用原有容器协议。

## 设备与接收程序

`sse_hwstamp_ctl INTERFACE show` 读取网卡能力和当前打戳设置。
`sse_hwstamp_ctl INTERFACE enable-rx-all` 显式启用全部接收包的硬件打戳，
保留既有发送设置。设备设置影响同一网卡上的其他 socket，因此由运维入口执行，
采集器本身只检查接口/IP、硬件能力和已启用状态，不反复修改设备。

`common/stream/MarketDataStream.cpp` 在指定 `hardware_timestamp_interface` 时，
通过 `SO_TIMESTAMPING` 同时请求 RX_HARDWARE、RAW_HARDWARE、RX_SOFTWARE 和
SOFTWARE。接收调用读取 `SCM_TIMESTAMPING` 的软件与原始硬件两个时钟。
该模式不同时启用 `SO_TIMESTAMPNS`，以免缺失软件时间时被合成的取包时间混淆。
未配置硬件接口时，继续使用原来的 `SO_TIMESTAMPNS`。

## 时间字段与兼容性

| 字段 | 含义 |
| --- | --- |
| `realtime_ns` | 内核软件接收时间；若回退到应用时间，以来源标志明确标记 |
| `hardware_ns` | 网卡硬件接收时间，来自 PHC；零表示没有返回 |
| `application_realtime_ns` | 接收调用返回后的系统实时时间；软件模式未保存时为零 |
| `monotonic_ns` / journal `receive_mono_ns` | 接收调用返回后的应用单调时间 |
| `hardware_clock_index` | 网卡 PHC 编号，对应本机 `/dev/ptpN` |

每次接收调用的应用时间由该批包共享。内核与硬件时间来自各包的控制消息。
硬件模式下标记 `kHardwareTimestampRequested`；实际收到硬件时间才标记
`kHardwareReceiveTimestamp`。状态输出记录 `hardware_timestamps`、
`missing_hardware_timestamps` 和 `software_timestamp_fallbacks`，不会拿软件时间
填充缺失的硬件字段。

上海 `payload_format=sse-stream-v2` 使用 80 字节带版本号的 payload 头，
保存扩展时钟和原始 UDP 字节；journal 与 SHM 携带相同 payload。
`sse-stream-v1` 的 48 字节头仍按原方式写入和读取。新录制必须使用新的
journal 目录、SHM 路径和 generation，不能将两种配置混用于同一录制。
旧读者会拒绝 v2；相关采集和预测二进制须一起重建。

`.t0md` v1 没有硬件时间字段，本次不扩展它。公共流若同时请求硬件时间和
T0MD 录制会明确拒绝，避免采集时有时间戳而落盘丢失。深圳默认采集选项、
既有软件时间与采样规则不变；本次也没有切换上海采样使用的时钟。

硬件 PHC 与系统 CLOCK_REALTIME 不保证同步。比较两个包在同一 PHC 上的
间隔可以直接相减；计算硬件到内核的绝对延迟前，必须测量时钟偏差和误差范围。
已有历史数据不含硬件时间，不能事后补出。

## 本机配置

在新的上海 journal 配置中，使用：

```json
{
  "payload_format": "sse-stream-v2",
  "hardware_timestamp_interface": "hqh-p1-k2"
}
```

其余字段沿用当前 journal 配置，包括真实 `boot_id`、交易日、订阅和新的
generation/journal/SHM 路径。设备已配置后可先作最长 60 秒的独立探测：

```sh
BUILD/sse_hwstamp_ctl hqh-p1-k2 show
BUILD/sse_hwstamp_ctl hqh-p1-k2 enable-rx-all
BUILD/sse_timestamp_probe hqh-p1-k2 11.11.11.11 239.35.80.9 37109 10000
BUILD/t0_sse_journal_capture /path/to/new-capture-v2.json
```

本机能力查询报告 `sfc` 驱动、PHC 0、RX hardware 和 `HWTSTAMP_FILTER_ALL`。
能力报告不等于实际收到硬件时间，必须以探测结果和录制中的字段为准。

## 20260908 验证

- GCC 4.8.5 完整构建通过；43 项已注册 CTest 验证通过，包括软件默认路径、
  沪深处理器、硬件/软件控制消息解析、v1/v2 编解码、journal/SHM 传递和重启追赶。
- 14:53 的 10 秒实机探测收到 12,826 个行情 UDP，全部带硬件及内核软件时间，
  缺失硬件时间、内核报告丢包、接收队列溢出均为零。PHC 为 0。
- 收盘附近的独立 journal 采集收到 18,924 个带硬件时间的包，正常排空并关闭；
最终版本另作短录制并经 JournalReader 校验提交标记/CRC 后读回了三个时钟。

## 上海批次结束与策略栅栏

带硬件时间戳的上海 UDP 流按相邻包 PHC 时间差识别市场批次：差值小于
5 μs 继续属于当前批次，差值大于等于 5 μs 时关闭上一批。关闭时集中完成
订单簿切面、因子和推理，然后输出 `kBatchEndOutput`。策略收到带硬件时间戳的
结果后暂存信号，直到收到该标记才提交计算结果；`receive_batch` 仍仅表示一次
`recvmmsg()` 调用边界。没有 PHC 字段的旧 v1/replay 输入继续走兼容路径。
  探测为短时验证，不代表全天峰值延迟或完整性验收。
- 网卡 RX filter 从 NONE 改为 ALL，TX 保持 OFF。原全天 v1 采集 PID 230597
  没有重启，状态仍正常；它不会因设备开启而自动获得新增硬件字段。
- 修正已有 journal 集成测试中的 superblock 字段偏移、收包就绪等待及采样输入，
  并修复其暴露的 SIGTERM 可能直接终止 journal 线程的问题。新版先屏蔽工作线程
  的停止信号，再由控制线程触发排空和正常关闭。
