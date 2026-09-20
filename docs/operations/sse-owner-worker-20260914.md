# 09/15 SSE 固定股票 worker 改造

已于 2026-09-14 20:44 部署到 sse-live，供 09/15 定时任务启动。今晚没有重启实盘 capture/交易服务。

## 实现

- CPU 32 上的行情/策略所有者线程统一解码、检查 wire sequence、确定硬件 batch 边界。
- 股票按每日静态股票表固定分配到 14 个 worker；每条逐笔只发送给所属 worker。
- worker 使用预分配的 1,024 槽 SPSC 队列，收到记录立即消费，空闲时执行 PAUSE 自旋。
- 普通逐笔更新不进入全局结果队列。全局队列只维护快照输出及 batch 关闭结果。
- BatchEnd 只分派给该批涉及的 worker。批内预测按股票代码排序，完整交付后产生唯一 BatchEnd，策略在这个边界执行报单。
- 交易回调与 OMS 仍由一个线程串行访问；入口与交易共用 CPU 32，避免新增一次线程间交接。ATP SDK 保持 CPU 40。
- 移除预测入口的 100μs 空闲 sleep。空闲检查及状态报告只消费已经完成的结果，不等待所有输入作业完成。
- 保留恢复到 live 前的显式 drain；没有状态快照时从 journal 起点重建订单簿、因子窗口和模型状态。删除仅凭游标跳过历史的实验。

worker 核：144、152、160、168、176、184、192、200、208、216、224、232、240、248。
原 capture 的收包 CPU 8、分发 CPU 16、journal CPU 24 保持原配置。

## 正确性验证

数据：2026-09-14 的完整 capture，2,317 只股票。

| 项目 | 结果 |
|---|---:|
| journal events | 27,645,718 |
| 原始逐笔记录 | 191,201,311 |
| v0.6 预测 | 8,342,658 |
| 串行有序输出哈希 | `4b5aea948b749b51` |
| 首版分派器全日哈希 | `4b5aea948b749b51` |
| 最终版本全日哈希 | `4b5aea948b749b51` |

哈希覆盖输出顺序、采样、因子、模型头、订单簿切片、provenance 及 BatchEnd；没有减少股票或跳过历史。最终版本的逐股票 CSV 也与串行逐字节一致。

队列专项测试覆盖多股票 datagram、超过队列容量的输入、借用缓冲区复用、跨 channel batch、PHC 倒退/缺失、软件时间戳回退、重复/缺口、worker 异常、回调异常及退出。
SSE stream/session、v0.6 策略、OMS、ATP snapshot collector、journal handoff、ATP snapshot order 回归均通过。
UDP → capture → journal → v0.6 集成测试通过；该 loopback 测试使用软件时间戳回退路径，硬件 batch 路径由专项测试和全日回放覆盖。
启动器 10 项测试通过。

额外执行的旧 query CLI 负例测试存在既有失败：它期望参数错误先于每日配置文件加载错误，新旧二进制均返回 `cannot open config: /no-daily.json`。没有触发 TD 连接。

## 账户快照修正

今天 SDK 查询日志的 16 笔订单中，12 笔已撤销订单的 LeavesQty 仍包含已撤销数量。例如 688327：OrderQty=200 股、CumQty=0、LeavesQty=200、CanceledQty=200、状态已撤销。

旧适配器把 LeavesQty 直接映射成 OMS working，导致 `invalid snapshot order`。新适配器先校验原始数量，再根据明确的终结状态将 working 转为 0；已成交数量、原始数量及仍挂单订单不变。回归测试重现旧拒绝，并验证修正后 OMS ready 且无资金预留。

新增 `atp_snapshot_order` 日志记录原始 leaves/canceled、映射后 working、状态和订单身份；原报单与成交回报日志保留。

晚间只读 ATP 验证登录超时，未拿到实时账户快照。早盘连接及实际 ready 必须由启动检查确认，不能用离线测试代替。

## 性能口径

全日 parity 测试包含 journal 读取、额外原始记录计数和完整输出校验，不等于实盘交易性能。
按历史收包间隔的性能测试先连续重放 100 万 events 重建状态，再测后续 10 万 events；不连接 ATP、不报单。
内部队列指标以实际入队/执行时间衡量；软件收包到预测还包含历史 batch 关闭等待以及入口积压。二者不能混为一谈。

14 与 28 worker 已比较。28 worker 能减少 worker 排队，但该样本的收包到预测尾延迟未改善；最终已按 14 worker 验收并部署。

最终版本全日 parity 吞吐为 **23,507 journal event/s**，串行为 **9,738 event/s**，同口径约 **2.41 倍**。这不是与旧 256-event 实盘模式的直接对照；该测试有额外计数和校验开销。

最终代码按原始收包间隔重放连续交易窗口，测得 56,697 条预测：

| 指标 | 平均 | P50 | P95 | P99 |
|---|---:|---:|---:|---:|
| 入队 → worker 开始 | 38.10μs | <2μs | <257μs | <534μs |
| worker 作业计算（含普通更新及 batch 预测作业） | 9.96μs | <3μs | <6μs | <294μs |
| 入口实际发布关闭 → 完整结果交付 | 384.76μs | <449μs | <864μs | <1,095μs |
| 模拟软件收包 → 预测回调 | 3.22ms | 2.99ms | 5.54ms | 6.44ms |

内部作业分布采用 1μs 分桶，上表为分位数上界；软件收包到预测使用逐条测量值。原始采样点到 BatchEnd 的平均间隔约 161.58μs。

**固定凑批和睡眠唤醒已消除，但端到端回放仍存在毫秒级尾延迟。** 入队前的 journal 读取、解码及突发输入积压，以及 batch 内计算依赖仍会消耗时间。这些数据不包含策略、OMS、ATP 和预测审计日志开销，不能宣称实盘报单已达到微秒级，也不能用其直接推算实盘提速百分比。

## 部署

- 生产程序：`/home/zane/usagi-bin/t0_sse_journal_predict`
- ATP 插件：`/home/zane/usagi-bin/libsse_td.so`
- 修改已同步至 `/home/zane/usagi-sse-dev`；完整构建与验证工作目录保留在 `/home/zane/usagi-owner-worker-20260914`。
- 旧二进制、旧插件、修改前源文件及 override 备份：`/home/zane/usagi-runtime/owner-worker-deploy-20260914-204415`。
- 已删除 service override 中的固定 flush 阈值及实验性恢复游标设置。
- 已确认 `--live-orders`、`SSE_ENABLE_LIVE_ORDER=YES` 和原 14 worker CPU 设置仍生效，动态库依赖完整；未在晚间启动交易。
- 报单延迟与成交回报日志保留，新增账户快照原始/归一化数量日志。

## 明早启动条件

沿用原定时任务：08:50 preflight，08:55 capture，08:57 prediction/trading，08:58 startup check。
v0.6 模型与现有实盘报单授权保持；每日配置和账户 ready 校验保留。
20:45 执行 09/15 preflight，因 `config_sse_daily_20260915.json` 尚未生成而未通过。每日配置到位和早盘 ATP ready 是仍待确认的两个条件。
