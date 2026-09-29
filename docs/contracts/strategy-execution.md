# 策略接线与离线验收

当前入口是 `t0_sse_stream` 和 `t0_sze_stream`，默认仍只计算行情/预测。显式使用 `--strategy-intents` 生成处理 profile 后，同一 capture/replay 链路继续调用真实 `ZStrategy`，输出订单意图统计。两者已复用显式生命周期 API，详见 `runtime-api.md`。它们不是券商交易入口，不模拟成交，不将离线成功视为实盘批准。

## 代码边界

| 部分 | 位置 | 职责 |
| --- | --- | --- |
| 既有策略决策 | `common/strategy/ZStrategy.*` | 仓位偏移、理论价、买卖数量、原有预热/风险拒单冷却 |
| 执行/时钟接口 | `common/contracts/StrategyExecution.h` | 下单、撤单、延时撤单、时钟、日志 |
| 共用策略会话 | `common/strategy/StrategySession.*` | 每股策略与视图、市场/配置校验；旧 LF 回报入口拒绝修改账户 |
| 账户 OMS | `common/oms/*` | 唯一订单/资金/持仓占用账本、风控、撤单、身份、恢复与持久审计 |
| OMS 客户端与执行保护 | `common/execution/{OmsStrategyExecution,StreamStrategyExecution}.*` | 就绪/健康门禁、意图与只读状态转换；无第二份订单或定时器账本 |
| 旧框架适配 | `adapters/deepwin/strategy/StrategyExecutionFramework.*` | 保留日志/时钟兼容，未托管报撤全部关闭 |
| 沪市会话 | `sse/runtime/sse_strategy_session.*` | 每股独立策略和盘口、预测来源选择、回报路由 |
| 深市会话 | `sze/runtime/SzeStrategySession.*` | 保留深市样本/十档映射，不引入沪市时段或去重规则 |
| 配置与装配 | `tools/config/prepare_stream_processing.py`、`apps/StreamProcessingCli.h` | 复用统一配置的兼容投影，不另建策略参数方言 |
| 深市恢复驱动 | `sze/runtime/sze_recovery_driver.*` | 原 journal/SHM 接入 `SzeStreamProcessor`，不改变存储 ABI |

流向：原始录制/实时接收 → 同一市场处理器 → 当次样本与预测 → 同一策略 → 执行保护 → OMS 准入/登记 → backend。归一化回报直接更新 OMS，策略在信号线程读取状态。CLI 装配非成交的 `PaperBackend`，测试另用 `ScriptedBackend` 注入回报；不会加载真实 TD。完整口径见 [OMS 契约](oms.md)。

## 使用范围

```bash
python3 tools/config/prepare_stream_processing.py \
  --config unified-live.json --output processing-live.json --strategy-intents
BUILD/t0_sse_stream capture stream.json processing-live.json

python3 tools/config/prepare_stream_processing.py \
  --config unified-replay.json --output processing-replay.json --strategy-intents
BUILD/t0_sse_stream replay RECORDING processing-replay.json
```

两份统一配置应由相同业务配置分别绑定 live/replay 环境，业务指纹相同。两侧 `environment.execution` 均必须为 `disabled`；原始流 replay 必须绑定相同录制路径、`t0md-v1` 和 `recorded-receive`。意图模式支持 SH/SZ prediction，不接受 factors-only。

额外策略字段为 `strategy_runtime.mode=paper-intents`、`account_reference`、`legacy_config=export_legacy(config)` 和 `oms`。旧 profile 需要重新生成，并显式提供 `trading.oms.simulation_cash`、`fee_reserve_per_order`。策略参数、预热和数量决策公式保留；挂单量、持仓及可卖量改读 OMS，不再由策略根据回报猜测和累加。无 TD source 时使用本地纸面 source 1，有唯一明确 source 时保留它，均不注册券商。

JSON 汇总新增 `processing.strategy`：信号数、订单意图数、撤单意图数、意图 CRC32、`fills_simulated=false`。CRC 用来做确定性回归，不是密码学审计、完整订单流水或生产 artifact lock 验证。

## 保护语义

- 执行保护初始关闭；账户、风险、执行三项均就绪，且当前行情/处理/录制条件有效，才允许新单。CLI 只为已验证配置的 Paper 执行器设置就绪，真实 TD 不能照抄这一做法。
- 提交时重新读取健康状态，覆盖异步录制失败。必需录制失败阻断新单；可选录制失败仅降级，输入丢包/溢出/解析失败仍使处理失效。
- 新单绑定账户、source、交易所、股票池和 owner。归一化回报必须带完整账户/网关/交易日/epoch；旧 LF 会话回报入口返回 false。撤单由 OMS 校验 owner、原订单及其通道，不受新单健康门禁影响。
- `begin_stop()` 不可恢复。信号停止先关会话门禁再停接收；接收期限到达也公开 stopping 状态。允许已接收数据排空，但停止后不再增加新单。正常结束不派发新采样或人为推进撤单时钟。
- 行情和策略视图由会话线程使用。回报直接进入线程安全 OMS；SDK 调用不持有 OMS 状态锁，允许同步回调及其他线程的早到回报。TD 不调用 `ZStrategy` 修改持仓；会话停止与 OMS 停止分别关闭新意图和账户发送。
- 09:35 前使用同次 Snapshot 的五档盘口；之后使用同次 Tick cut 的十档盘口。各股视图独立持有，不能借用上一份 Snapshot 或接收回调中的临时内存。
- 原策略 1001ms 延时撤单使用录制单调时钟：只有后续时间观察达到截止时刻才发撤单意图。无行情/idle 观察时不会仅因墙钟流逝而触发；若要求完整实盘定时器复现，后续需记录独立运行时定时事件。

OMS 保留当日订单与逐笔身份直到显式容量上限，不在首个终态回报后删除归属。默认单账户最多 100000 个订单，订单总量、活动量、逐笔去重、撤单尝试、队列和审计尾部均有界。发送未知、账本异常及恢复未完成会阻断账户新单；不能用清空本地挂单量恢复交易。

## 当日调仓执行

配置的 `external_delta` 为当天净买卖目标；PE 初始化为其相反数，随后由 OMS 中执行归属的
累计成交更新：`PE = -external_delta + external_bought - external_sold`。
实际持仓满足 `static_position + PI + PE`。T0 读取扣除 PE 的持仓视图，以及仅属于 T0 的
挂单量、成交股数和成交金额；OMS 风控始终使用完整的实际持仓、资金及挂单占用。

同轮先运行 T0。若 T0 有新单，公共执行适配器随后判断执行：同方向合成一张真实订单，
数量为 T0 数量加全部待执行量，价格为买入卖三、卖出买三；反方向只提交 T0 单。
若 T0 无新单，执行独立提交剩余量。第三档缺失、盘口无效时不提交执行部分，不退回一档。
合单被 OMS 拒绝时再尝试原价格、原数量的 T0 单。每张合单的累计成交先分配给 T0，
超过 T0 原始数量的部分才更新 PE；重复回报由 OMS 去重，成交金额按逐笔成交前缀分配。

执行在 09:30–11:30、13:00–14:57 连续交易时段每分钟第一次检查时决策，午休和收盘集合竞价
不新增执行单。行情事件触发同轮检查；实时 owner 线程的 poll 还会按市场时钟推进分钟检查，
不依赖模型预测有效。分钟时钟沿用实盘入口的本地市场时区设置和交易日校验。
timer 使用最近盘口，超过 5 秒或市场门禁不通过时只处理已有执行单的撤单，不使用旧盘口补单。

纯执行单为普通限价单，到下一分钟撤换。合单沿用 T0 单原有的类型和撤单时限；例如
T0 的 1001ms 撤单会撤掉整张合单，执行余量留到下一分钟继续处理。撤单请求不是终态确认，
必须等待 OMS 确认旧单终态，再根据包括撤单期间成交在内的最新 PE 补单。
确认等待可以跨分钟；本分钟确认后由后续行情或 poll 继续补单，不能同时挂两份执行余量。

OMS 校验自成交、账户预算、板块数量规则和执行目标占用。执行归属和原始目标随订单写入
journal；含执行量的订单在报单前强制持久化，即使普通 T0 意图使用异步模式。
重启查询与 journal 合并后恢复成交归属和挂单占用，不重新执行完整调仓量。
沪深会话共用此模块，沪市 legacy 与 v0.6 均接入；深市真实 TD 的入口范围仍遵循现有部署边界。
本次扩展了内部 C++ 的 Intent、Position、OrderView 和执行接口；上线时 runtime 与 TD 插件
必须使用同一套头文件和编译器重新构建，不能加载此前编译的 TD 插件。

## 深市恢复

`SzeRecoveryDriver::replay` 先完整检查 journal 再处理，可显式选同次系统启动的单调接收时间映射，或仅用于分析的交易所时间。后者不会获得 live_ready。

`replay_handoff` 必须提供非零 expected_generation、明确 source/day/SHM 路径，以及完整 same-boot 时钟锚点。通过既有 ReplayHandoffConsumer 只读追赶，在连续事件、代次、生产者存活和行情连续性均满足且消费了有效 ring 事件后，才报告恢复就绪。后续 poll 会持续校验；它不是账户或交易就绪证明。

筛选后的 `.szej` 不要求原 wire sequence 连续，否则会把未订阅股票误判为丢包；检查的是连续 canonical event_id。raw capture 仍按原频道连续性校验，单个 processor 不允许混用两个输入模式。两个模式复用相同归一化、订单簿、因子和模型调用，不产生 EOF 样本。

当前旧 ABI 的存活证明是 PID 加代次/连续性，不包含独立 boot ID；调用方必须保证 same-boot 锚点来源真实。`t0_sze_stream` 的 `recovery-journal` 和 `recovery-handoff` 共用策略与 OMS。公共 OMS 已覆盖活动/外部/未知订单恢复；CLI 仅装配明确的模拟快照，真实 ATP 的完整查询恢复和生产权重数值验收仍未开放。

## 尚未开放

旧沪市 `get_obj()` 仍关闭。已核实 `IWCStrategy::stop/terminate/block` 非虚，不能靠新增子类来确保它们停止新 raw-stream 线程。当前已经提供独立的显式生命周期 API 和调用它的 CLI，但没有把旧 main 部署切换到该入口，也没有接入真实 TD 的完整回报协议。未部署插件、未接券商、未 merge/push。

确定性合成模型和 loopback 验收不代替全天生产数据、真实账户、实际网卡峰值或尾延迟验证。停止/故障状态和 TD 回报尚未完整录入 raw MD 文件，因此不能声称该文件可复现全部实盘交易行为。

结构目录已迁移到 `common/`、`sze/`、`sse/`；这不表示所有 sampling/runtime 或 TD 集成已经完成。
