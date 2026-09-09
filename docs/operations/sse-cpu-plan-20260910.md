# 上海 2026-09-10 配核与代码交接

建议初始配置为 **4 个订单簿/因子 worker + 20 个推理 worker**。
这是根据 9 月 9 日真实行情回放选择的起点，不是已验证的最优实盘配置。
目标机器为双路 EPYC 9755：256 个在线 CPU、32 个 L3 域，每域 8 个 CPU。
上海每个低延迟工作线程独占一个 L3 域；深圳配置不变。

## CPU 分配

| 职责 | CPU 编号 | L3 域数 |
|---|---|---:|
| 采集接收 | 0 | 1 |
| 采集分发 | 8 | 1 |
| journal 写入 | 16 | 1 |
| 预测入口、解析、路由和输出轮询 | 24 | 1 |
| TD | 40 | 1 |
| 订单簿更新和因子计算 | 32, 48, 56, 64 | 4 |
| 推理 | 72, 80, 88, 96, 104, 112, 120, 128, 136, 144, 152, 160, 168, 176, 184, 192, 200, 208, 216, 224 | 20 |
| 备用 | 232, 240, 248 | 3 |

共使用 29 个 L3 域，保留 3 域。这里的 24 个计算核是 4+20，
不是 24 个推理核。若使用 24 个推理核，在保留采集、入口和 TD 的
5 个域后，最多只能配置 3 个订单簿核，且不剩备用域。
线程数量不等于进程数量：采集独立进程，计算 worker 位于预测进程内；
现有生产集成的 TD 仍是预测进程中的独立线程。

这是跨 NUMA 的方案。L3 租约可防止遵守该机制的上海线程共享 L3，
并不隔离操作系统、IRQ 或其它后台任务。指定 CPU 忙时应让启动明确失败，
不要改用同一 L3 的其它 CPU 绕过检查。

## 配置接入

使用仓库文件
`config/examples/sse/sse_hardware_pipeline.epyc9755.4book20infer.json`。
它只包含计算管线配置，不是 capture 或 trading launcher 的完整配置。
生成 processing profile 时显式传入：

```sh
python3 tools/config/prepare_stream_processing.py \
  --config /path/to/current-daily-config.json \
  --output /path/to/generated-processing-profile.json \
  --sse-contract sse-hardware-batch-v3 \
  --sse-pipeline config/examples/sse/sse_hardware_pipeline.epyc9755.4book20infer.json
```

每日配置和模型输入必须使用当天有效文件；不要直接复用回放实验的配置。
生产 launcher 如果自行生成 profile，必须把上述 JSON 对象保留在生成结果的
顶层 `pipeline` 字段，并保留 `sse-hardware-batch-v3` 合同；单独复制文件不会启用 worker。
采集 transport 配置分别设置 `receive_cpu=0`、`dispatch_cpu=8`、
`journal_cpu=16`、`prediction_cpu=24`；TD 的 CPU40 由生产 TD 集成配置。
不要使用 `taskset -c 24` 限制整个 predictor：进程启动时须可见全部所需 CPU，
之后入口和每个 worker 分别绑定自己的核。

每股订单簿固定归属一个 shard；ChannelNo 1–6 内尽量均分。
推理有独立的固定 owner，维持每股递归模型顺序；snapshot-first 股票后续
登记 ChannelNo 时不迁移模型状态。此配置不启用未经校准的频率权重。
BatchEnd 后才提交该批采样计算，已封批结果按序交付，后续更新可与推理重叠。
纯采集继续写 `.szej` journal 并发布 SHM；预测仍通过 journal 追赶后切换 SHM。

## 选择依据和测量边界

真实 09:30–09:31 逐笔采样批次的样本数 P50/P90/P99/max 为
16/28/40/52。共有 8,125 个逐笔批次，其中 7,813 个非空，共 131,142 个样本。

| 独立逐笔推理 worker 数 | 整批完成 P50（us） | P99（us） | 最大值（us） |
|---:|---:|---:|---:|
| 8 | 218.662 | 498.065 | 4,778.967 |
| 16 | 163.322 | 1,613.746 | 5,146.760 |
| 20 | 161.071 | 1,481.765 | 5,784.818 |
| 24 | 142.621 | 340.283 | 42,594.110 |

这些是预先加载真实因子的独立逐笔推理实验，采用 SPSC 任务队列，
排除接收、订单簿、因子、快照模型和交易；每配置只测了一分钟。
不同核数的长尾波动明显，不能据此保证线上延迟或断言 24 核最优。
20 核保留更多订单簿和备用资源，适合作为初始分配。

实际管线的 factors-only 回放中，4 个订单簿核的批末到输出 P50 约
459us，8 核约 443us，未见明显收益；对应 P99 约 9.55ms 和 13.35ms。
该测量包括前端、排队、批屏障、输出及回放观测开销，不能叫做单次因子耗时。
订单簿与推理来自分开的实验，不能相加其分位数作为端到端延迟。
完整开盘混合模型测试尚缺可加载的 Auction59 因子输入，未用零值代替。
因此尚无“4+20 全链路实盘已通过”的结论。

## 本次提交与生产目录的关系

GitHub 分支为 `sse-latency-20260909`，包含快速解析、增量订单簿、
秒级订单年龄、流式 flow/盘口因子、tanh 插值、逐笔模型优化、
独立订单簿/推理 worker、ChannelNo 分配和队列/批完成观测。
此前供深圳参考的 `sse-factor-opt-20260909` 停在 `9129ed3`，
不包含其后的模型与推理分配改动。

当前优化模型编译使用 `-march=native` 和 AVX2/FMA；请在目标机器上构建，
不要把不同 CPU 机器生成的二进制直接部署到这里。

当前生产维护目录 `/home/zane/usagi-sse-dev` 基于 `77c8f87`，
另有其它会话尚未提交的 Auction59/TD/launcher 改动。
本分支没有替这些改动提交，也不是该生产目录的完整镜像。
准备交易时，需要维护会话先保存这些改动，再合并本分支并处理重叠文件，
特别是 `StreamProcessingCli.h`、`sse_journal_prediction.cpp`、
`sse_stream_processor.*`、`sse_strategy_session.*` 和 profile 生成逻辑。
不能直接用本分支二进制替换现有 production trading launcher 所依赖的二进制。

交接时需确认最终 profile 实际包含 4+20 个 CPU、启动后租约/亲和性符合上表、
两阶段追赶能够进入 live、当天 Auction59 和快照模型有效。
当前 service 默认 monitor、订单关闭；是否开启订单仍由当天交易准备流程决定。
本次工作不更改 production service、开盘定时任务或实盘订单开关。
