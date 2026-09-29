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

## 当日调仓

沪深共用 `common/execution/ExternalExecutionController`。legacy 每股 `ins_params` 可增加
`external_delta`，统一配置对应 `account.positions.<symbol>.external_delta`；省略字段保持原行为。
正数为买入股数，负数为卖出股数，`static_position` 必须已经是调仓后的目标底仓：

```json
{
  "000001.SZ": {"static_position": 100, "last_position": 0, "external_delta": 100},
  "000002.SZ": {"static_position": 100, "last_position": 0, "external_delta": -100}
}
```

上述配置对应调仓前持仓 0、200 股。`last_position` 仍表示 T0 的隔夜偏移；Paper 初始持仓为
`static_position + last_position - external_delta`，实盘持仓以账户查询为准。
字段必须为整数，买卖数量和部分成交后的余量由 OMS 按所属板块的报单规则校验。
同一交易日的调仓目标不可变；重启须保留同一配置和账户 journal。

执行规则、成交分配及分钟检查见[策略执行契约](contracts/strategy-execution.md#当日调仓执行)。

## 市场差异

深市继续使用既有 daily/system 生成流程，恢复输入和十档映射保持原语义。沪市保留 Snapshot/Tick 选择、同 ExTime 去重及市场路由规则。共享配置结构不抹平这些差异；模型架构、权重和 feature contract 也必须分别锁定。

统一配置 v1 的迁移字段仍以保守标记记录来源和字段存在性；这不等于完整交易运行时已经绑定。相对地，已支持的 stream profile 和 Paper strategy-intents 投影可以被 `t0_sze_stream`/`t0_sse_stream` 消费，但仍要求 `execution=disabled`，不能声称真实 TD ready。
