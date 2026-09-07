# 统一运行入口 v1

本增量将两市行情及 Paper 策略装配到显式管理生命周期的运行入口。它不依赖 Deepwin SDK，不连接券商，不模拟成交，也不替换已部署的 `main`。

## 入口和调用顺序

- `libt0_sse_runtime.so` / `libt0_sze_runtime.so` 导出 `t0_market_runtime_v1()`。
- C 接口声明在 `common/contracts/MarketRuntimeApi.h`。
- `t0_sse_stream` / `t0_sze_stream` 现在是同一 API 的命令行调用方，不再各自维护另一条市场处理入口。
- `t0_md_stream` 仅保留纯录制/回放功能，接收配置解析与新入口共用 `StreamInputConfig.h`。

调用顺序为 `create -> start -> request_stop -> join -> destroy`。有限回放自然结束时可直接 `join`，销毁也会停止并等待线程。每个句柄只允许启动一次；启动前停止不会创建录制目录，启动中停止会跨越底层 stream 的初始化持续生效。

调用方先检查 `abi_version`、`struct_bytes` 和 `market`。`create/start/join` 通过返回值和调用方缓冲区报告错误；`status_json` 返回包含 NUL 的所需字节数，缓冲区不足时留空，调用方需重试。`status` 和 `request_stop` 可与 `join` 并发，`destroy` 不能与任何其他句柄调用竞争；库只能在所有句柄销毁后卸载。

`ready` 仅表示输入接收/恢复入口已就绪，不是实盘账户或交易权限。完成状态才读取非原子的处理统计；运行期间状态查询只读取安全发布的状态。线程创建失败也保留 `error`、`done`、`ready=false`，不会只在首次 `start` 返回错误。

## 使用方式

原始行情命令保持不变：

```bash
BUILD/t0_sse_stream capture stream.json processing-live.json
BUILD/t0_sse_stream replay RECORDING processing-replay.json
BUILD/t0_sze_stream capture stream.json processing-live.json
BUILD/t0_sze_stream replay RECORDING processing-replay.json
```

两市均可使用 `prepare_stream_processing.py --strategy-intents`。生成的 `strategy_runtime` 包含 `mode=paper-intents`、`account_reference` 和 `legacy_config`。此前两字段的实验性策略 profile 需要重新生成；没有 strategy_runtime 的默认 profile 保持兼容。账户标识进入投影后，不再用缺省字符串代替账户归属。

所有环境的 `execution` 仍必须为 `disabled`。Paper 显式把配置中的仓位作为模拟快照，按完整对账协议初始化，汇总标记 `account_baseline=configured-paper-snapshot`；其中模拟现金字段为 0，只验证协议完整性，不是实际资金风控或账户资金证明。

## 深市恢复输入

统一配置中的 `market_data.recovery` 直接投影为处理 profile 的 `recovery`，不建立另一份策略/恢复参数方言。

```bash
python3 tools/config/prepare_stream_processing.py \
  --config unified-journal.json --output processing-journal.json \
  --recovery-input journal --factors-only
BUILD/t0_sze_stream recovery-journal JOURNAL_DIR processing-journal.json

python3 tools/config/prepare_stream_processing.py \
  --config unified-handoff.json --output processing-handoff.json \
  --recovery-input handoff --strategy-intents
BUILD/t0_sze_stream recovery-handoff JOURNAL_DIR processing-handoff.json
```

离线 journal 要求 `environment.mode=replay`、`record_format=canonical-events`、`time_basis=exchange`，只用于分析，永不获得新单资格。`environment.recording`、命令行目录和 `recovery.journal_directory` 必须一致。没有可信跨启动时钟锚点时，不伪造历史接收间隔。

handoff 要求 live 环境、显式非零 `expected_generation`、SHM 路径及严格校验。当前主机的 MONOTONIC/REALTIME 锚点由运行时读取，不写死进 JSON。只有同次系统启动、正确日期/source/代次、连续事件及生产者存活均满足后，才通过恢复门禁。恢复阶段的旧行情仍可暖模型和策略，但不允许新单。

共用约束：`enabled=true`、`trading_enabled=false`；不支持放行坏 journal。source 为正 uint16，最大 payload 为 72..65535，segment_mb 与 segment_bytes 二选一且可容纳至少一条记录；未知选项明确拒绝。handoff 初次接续超时当前固定 1000ms。

运行时根据实际输入驱动选择健康来源：raw 使用 MarketDataStream，recovery 使用 SzeRecoveryDriver/processor。不会让尚未启动的 UDP 对象挡住已经成功恢复的输入，也不会把正常主动停止误报为来源损坏。

## 账户和策略边界

公共 `StrategySession` 管理股票池、ZStrategy 实例、持有的盘口、订单归属和执行保护。沪市的 Snapshot/Tick 选择、同 ExTime 去重仍在沪市适配层；深市保持自身完整样本及十档映射，不套用沪市规则。初始化仓位必须覆盖全部配置股票，先关门禁并完整验证后才更新；已有策略信号后不允许用该接口覆盖仓位。

`AccountReconciliation` 要求账号/source/day 匹配、递增连接代次和快照 token、连接有效、资金字段合法、全部股票仓位、仓位查询完成、明确的零挂单查询完成，以及最终完成标记。收集期间出现账户/订单活动会使快照失效，旧回报不能完成新快照，断线立即取消就绪。

它目前只支持无在途订单的初始化，不提供真实券商订单账本、重连后在途订单接管或迟到成交的终态回收。现有执行保护仍保留每会话 100000 个 request 的明确失败上限。真实 TD 开放前必须补完这些约定；不能把 Paper 的配置快照当作实盘对账结果。

## 旧宿主限制

只读调查确认，已安装的旧 main 调用 `get_obj` 后不会再统一调用策略 start，其 control-center 的 start/stop 方法也没有提供所需控制；IWCStrategy 的 stop/terminate/block 非虚。旧沪市 `get_obj` 因此继续明确拒绝，提示使用新入口，不暗中启动无法回收的线程。

这意味着旧部署的 `main + libt0_strategy_sse.so` 尚未变为新入口。切换启动器、TD 回报协议补齐和真实账户/全天验收仍是独立工作。本轮不修改部署配置、remote、仓库目录或项目名；结构迁移不等于旧宿主或真实 TD 已切换。

## 验证范围

使用临时合成模型、loopback 报文、私有测试 journal/SHM 验证，不使用真实券商连接。覆盖两市 CLI/API 一致性、启动前/启动中停止、重复调用、并发状态查询、生产者退出，以及测试专用 preload 注入的线程创建失败。故障开关只存在测试 shim，不进入生产运行库。

合成模型的订单意图通过不等于生产模型收益或全天数值验证，也不意味着 raw MD 文件已经记录全部 TD 回报、故障和独立定时事件。最终构建/测试数量以执行记录为准。
