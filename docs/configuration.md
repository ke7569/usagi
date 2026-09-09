# 配置总览

配置入口分为统一配置、市场 profile 和旧格式兼容层。统一 schema v1 保留市场、策略、预测、行情、交易、账户、部署、环境及迁移信息；它不会自动补全缺失策略参数、账户能力或模型能力。

## 路径与工具

- 公共配置工具：`tools/config/unified_config.py`
- 行情处理 profile：`tools/config/prepare_stream_processing.py`
- 深市 daily 生成器：`deploy/sze/daily/prepare_sze_runtime.py`
- 示例配置：`config/examples/sze`、`config/examples/sse`

`config/examples/sze` 和 `config/examples/sse` 保留的是 legacy 输入示例，供旧入口或统一迁移工具读取，不是两套新的 schema。统一配置和生成的 daily/profile 写入 `build/configs/<run>`。不要把旧格式审计输出误当作新的 replay 启动配置，也不要把凭据内容搬进统一 JSON。完整字段、迁移、绑定和锁校验见[统一配置工具契约](contracts/unified-config.md)。

## 工作流

1. 从原始市场配置生成统一配置并校验。
2. 绑定 `live` 或 `replay` 环境，明确时钟、录制格式和执行状态。
3. 使用 `prepare_stream_processing.py` 投影行情/采样/因子/模型 profile。
4. 对模型、缩放器、路由文件和显式二进制生成并验证 artifact/run lock。

默认和当前验收边界是 `execution=disabled`。`--strategy-intents` 只产生 Paper 订单意图；`--factors-only` 只验证处理链，不代表模型或策略已验收。配置指纹和文件哈希是来源约束，不是网络、TD 或全天收益等价证明。

## OMS 预算

OMS 意图模式要求显式的统一配置 `trading.oms`，legacy 对应顶层 `oms`：

```json
{"simulation_cash": 1000000, "fee_reserve_per_order": 0}
```

以上仅为测试预算示例，不是实盘资金。两字段使用人民币，非负且精确到万分之一元；不接受未知键、布尔值、非有限值或超过 `1e12` 元。缺少预算时生成器拒绝 `--strategy-intents`，不推断无限资金。旧三字段 `strategy_runtime` profile 需要重新生成，新 profile 还携带 `oms`。账户所有权、资金口径和 ATP 限制见 [OMS 契约](contracts/oms.md)。

## 市场差异

深市继续使用既有 daily/system 生成流程，恢复输入和十档映射保持原语义。沪市保留 Snapshot/Tick 选择、同 ExTime 去重及市场路由规则。共享配置结构不抹平这些差异；模型架构、权重和 feature contract 也必须分别锁定。

统一配置 v1 的迁移字段仍以保守标记记录来源和字段存在性；这不等于完整交易运行时已经绑定。相对地，已支持的 stream profile 和 Paper strategy-intents 投影可以被 `t0_sze_stream`/`t0_sse_stream` 消费，但仍要求 `execution=disabled`，不能声称真实 TD ready。

## 沪市多核推理 pipeline

`sse-hardware-batch-v3` 的可选 `pipeline` 仍向后兼容旧的五字段格式：
`book_cpus`、`inference_cpus`、`ingress_capacity`、`inference_capacity` 和
`output_capacity`。两个 CPU 列表可以长度不同；每个非负 CPU 必须属于独立且未占用的
L3 域，`-1` 由租约分配器自动选择。示例从 2 个 book worker 和 4 个 inference worker
起步，属于容量起点，不是峰值吞吐承诺。

可选 `inference_frequency_weights` 是六位股票代码到正有限数字的对象，表示历史预期
采样频率。缺省权重为 `1.0`，只影响首次 inference owner 的 ChannelNo 内负载均衡；
股票一旦分配 owner，后续 tick、snapshot 和 ChannelNo 注册都不会迁移它。snapshot-first
股票可以先保留 owner，首个 tick 到达时只补记 ChannelNo。生成器与 CLI 对未知字段、非法
代码、字符串/布尔值、非有限值以及非正权重严格拒绝；省略该对象保持旧行为。

运行状态会保留 `inference_channel_shard_counts`（ChannelNo 主序）和可选权重，并报告每个
inference worker 的当前队列、队列高水位、样本数、queue-wait P50/P99/最大值、batch-completion
P50/P99/最大值以及汇总 BatchEnd completion。分位数是固定 bucket 的 inclusive upper bound，
最大值精确；serial 模式没有 inference queue wait 数组。统计使用本地 monotonic clock，
只用于观测该离线/受控 pipeline 的排队和交付边界，不是交易时延 SLO，也不构成生产部署说明。
