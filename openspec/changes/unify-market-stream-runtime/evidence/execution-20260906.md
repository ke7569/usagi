# 沪深公共行情链路执行记录

日期: 2026-09-06。工作区: `/home/sze-t0`。变更: `unify-market-stream-runtime`。

## 完成范围

当前已经推进到两市显式生命周期入口、共用策略会话、账户快照门禁，以及深市 journal/SHM 运行入口，但不是第四步的真实券商交易运行时验收。没有最终 merge、commit、push、交易插件部署或真实 TD 操作；既有用户改动保持原样。

| 讨论中的步骤 | 本轮产出 | 尚未完成 |
| --- | --- | --- |
| 1. 公共行情基础层 | 批量接收、有界队列、完整异步录制、时间/顺序回放、必需/可选录制故障策略 | 实际网卡峰值性能及崩溃恢复验收 |
| 2. 两市构建边界 | `t0_sze_stream`、`t0_sse_stream` 独立构建，模型不互相依赖；筛选导入并记录来源 | 旧策略插件的完整合并和独立加载 |
| 3. 沪市接入 | 共用 capture/replay、真实 ZStrategy、按股盘口；CLI 和动态库共用显式生命周期 | 旧 main 部署迁移、真实 TD、全天数值对照 |
| 4. 深市接入 | 同一原始流/恢复应用装配、共用策略会话；strict profile、账户快照和 `.szej`/SHM 校验 | 真实券商对账完成标记、在途订单接管及生产权重验收 |

新命令行程序始终禁用真实执行，两市可显式启用 `--strategy-intents` 调用已有策略决策，但只记录意图、不模拟成交。详情和限制见 `docs/strategy_stream_runtime.md`。行情层 `permits_new_risk` 仍只是必要条件，新的会话另检查账户/风险/执行就绪和停止状态。

## 显式入口增量

两市均可启用 Paper 策略意图。`libt0_sse_runtime.so` / `libt0_sze_runtime.so` 的 `t0_market_runtime_v1` 与命令行工具共用同一应用；支持显式 create/start/stop/join/destroy，不依赖旧 main 的空控制方法或非虚 stop。用法与迁移限制见 `docs/owned_market_runtime.md`。

共用 `StrategySession` 和 `AccountReconciliation`；完整模拟快照必须含账号/source/day、连接代次、全部仓位、资金字段和明确的空挂单查询完成。旧代次、缺项、非零挂单、收集期间活动及断线不能变成就绪。真实 SDK 目前缺失上层需要的完成标记，不开启真实 TD。

深市恢复 profile 直接来自 `market_data.recovery`。新增 journal/handoff CLI 和 C API 验收，包括生产者退出、错代次、坏文件、时钟/路径不符及正常停止。恢复健康来自实际恢复驱动，不再被未启动的 UDP stream 状态阻断。

线程创建失败采用测试专用 preload 注入，验证首个/第二个线程失败后的错误、状态和回收；生产运行库不包含故障开关。测试 fixture 的必要写入不再放在会被 NDEBUG 移除的 assert 中。

## 本增量最终验证

- 全量 29 项 CTest 通过。包含 11 项动态库 API 验收、6 项深市 journal/SHM 验收、2 项深市原始流到真实策略意图验收，以及已有沪市/原始流/健康/采样回归。
- 单独运行的 Python 配置回归：27 项处理 profile、28 项统一配置、12 项旧 daily，共 67 项通过，不重复累加 CTest 内的 Python 场景。
- 两市都使用临时合成权重产生真实 ZStrategy 的非零订单意图，并验证 capture/replay 的因子、预测计数和意图摘要一致；没有使用生产权重或模拟成交。
- 两份 owned-runtime `.so` 的 `ldd -r` 仅依赖系统库，无缺失/未解析符号；`nm` 未发现对方市场模型、IWCStrategy 或真实 insert_limit_order，显式导出版本化 C API。
- `git diff --check`、OpenSpec 严格校验通过。清单 19/20；剩余为旧 main/get_obj 部署兼容，不将新 API 可用等同于旧宿主已迁移。未部署、未提交或合并。

本轮 agent 分工：broker_td_boundaries 实现账户门禁、API 验收和故障注入并复核生命周期；sse_integration_map 抽公共会话并补两市策略测试；sze_integration_map 完成深市适配、恢复 profile 和 journal/SHM fixture；主代理负责 C API/启动器、应用装配、整合修复、全量回归和记录。

## 策略接线增量

- 抽取 `InsParams`、执行/时钟接口，旧 SZE StrategyBase 使用框架适配器；Deepwin 隔离构建成功，`nm` 确认 namespaced IWCStrategy，`ldd` 无缺失库。未替换运行库。
- 沪市增加按股持有的 Session；模型只选择同次有效采样，Snapshot/Tick 不共用过期盘口。真实 ZStrategy 正常数量公式保留，另防护非有限信号及浮点转整数越界。
- 新单在执行边界复查行情/录制健康和就绪；停止先关门禁。撤单及属于本会话的回报继续处理，request ID 不允许跨 source/股票串入。
- 新增完整 loopback → 录制 → replay → 因子/预测 → 真实策略 → 订单/撤单意图对照，必须实际产生非零意图，使用临时合成权重而非生产业绩数据。
- 深市新增严格 journal replay 和只读 SHM handoff，包含数据预检查、所选事件连续性、代次/日期/source、生产者存活和明确 same-boot 时间。分析时钟或未指定代次不能变为恢复就绪。
- `usagi` 及 `common/sze/sse` 目录迁移已经加入总重构计划第 8 节；本次不更名。

旧 SSE `get_obj()` 没有假装完成：框架 `stop/terminate/block` 非虚，不能安全覆盖新采集线程的生命周期，仍需统一宿主接线。当前 100000 request/session 保留容量也不是生产订单账本，券商终态/迟到成交与回收协议须在 TD 接入前完成。

## 上一策略增量验证

- 上一策略增量全量 23 项 CTest 通过，包括新策略核心、执行保护、沪市会话、深市恢复，以及三组 CLI 回归。沪市会话测试明确使用 `-UNDEBUG`，此前断言被关闭的运行不计为验证证据。
- 新策略 CLI 含 5 项验收，使用合成模型产生非零订单及撤单意图；capture/replay 的信号、订单、撤单和意图/因子 CRC 一致，异常执行 source 明确拒绝。该组额外连续运行 3 次通过。
- Python 单独回归：处理 profile 23 项、统一配置 28 项、旧 daily 生成器 12 项，全部通过。统计不再重复累加 CTest 内运行的 CLI 用例。
- `t0_sse_stream` 的 `ldd` 只有系统库，`nm` 未发现 Deepwin IWCStrategy、TD insert_limit_order 或深市 mix153060 符号。
- 该次 OpenSpec 严格校验、`git diff --check` 通过；当时清单 16/18 项完成，旧沪市框架生命周期和深市统一交易宿主装配保留未完成。
- 最后补测恢复时钟溢出/下溢、UTC+8 日期不符、时钟倒退及中途替换锚点：均在错误处失效，journal 预检查在回调前拒绝，不静默切换为交易所时间。

## 当前采样修订

用户已确认沪市规则，无需再从历史实现中选择采样基准。当前 `sse-per-instrument-v2` 每股独立判断静默严格 `>100us`；首个交易所时间 `>=09:30`、更新后形成有效双边盘口的逐笔事件立即初始化窗口，不发样本，也不等待 quiet cut。后续采样使用 OR 条件：成交额增量 `>=HistoryAmount/8000`，交易所时间差 `>=100` 秒，或者 mid 变化绝对值 `>1e-6` 且成交增量 `>=100` 股。同股同 ExTime 至多一次，拒绝同时间 cut 不清流量、不重置窗口。

采样器使用配置股票列表和有界索引堆，每股至多一个 deadline；其他股票与 Snapshot 推进观察时钟，但不延长该股票静默期。传输层的全局 idle 记录仅提供时钟观察，回调在观察到到期记录时间时运行，不是硬性 100us 实时调度保证。原独立 `MonotonicOneShotTimer` 不再是生产路径；归档内 `PredictionEngine` 仅作历史审计，不提供兼容模式。

## 修订后验证结果

- 14 项相关 CTest 全通过，包括配置 guard、原始流 CLI 和市场处理 CLI。两组 CLI 分别覆盖 13 和 10 个用例，后者明确拒绝旧全市场 `sse-batch-end-v1` profile。
- 同一股票连续更新 10000 次的压力用例通过，验证每股至多一个 deadline；实际处理器的 A 股票静默、B 股票持续更新用例通过。
- 同 ExTime 不重复采样且流量继续累积、开盘前不初始化以及首次有效开盘盘口更新立即初始化的用例通过。
- Python: 处理 profile 19 项、统一配置 28 项、原 daily 生成器 12 项通过。
- 生产采样路径和相关测试已移除独立 `MonotonicOneShotTimer`，CMake 同步去掉其旧链接；未增加 EOF 派发、TD 操作或部署行为。

本节保留采样修订阶段的局部验证，策略增量的最终结果另行记录；历史数量不累加，也不代替全天数值及真实网卡性能验收。

## 修订前验证记录

- 13 项相关 CTest 全通过，包含两市真实 UDP 接收到录制再回放、采样/模型边界、录制故障、旧序列恢复/健康及命令行测试。两组命令行内部各有 13 和 9 项测试。
- 深市 1000 条真实格式报文回放对照通过，比较因子、完整样本盘口、时间及来源信息。接收就绪确认替代固定启动等待后，另连续跑了 3 次，全部通过。
- 公共接收层的 1000 包突发及跨文件轮转测试通过。此结果不等同于沪市完整模型在实盘峰值下的延迟承诺。
- 沪市使用合成权重验证了实际模型接线、来源切换、缺失 Auction59 拒绝及不完整因子不得选为交易信号；Snapshot36 的单边盘口过滤、开盘前后衔接及累计量回退有检查。
- Python: 统一配置 28 项、处理 profile 18 项、原 daily 生成器 12 项通过。
- `nm` 未发现新二进制依赖另一市场模型、`StrategyBase` 或 TD；`ldd` 仅系统库。OpenSpec 严格校验和 `git diff --check` 通过。

构建目录: `/home/ref/md-stream-build-20260906-rYn4jN`。只构建相关 target，不运行仓库默认 `all` 来替换/部署插件。

## 历史模型补充检查

检查 Git 忽略目录后，找到旧沪市候选模型及 Deepwin handoff 依赖，纠正了“默认路径缺失”等同于“本机无资产”的判断。来源哈希见 `market_stream_source_provenance.md`。

历史候选的 native hybrid 加载和零输入推理通过：opening tick shadow `-0.40501976`，opening snapshot selected `0.0942039043`，post-switch tick selected `-0.405053377`。Tick 8 行零输入及最终 hidden 与旧参考文件逐字一致。上述输入不是实际订单簿因子，结果不能作为全天生产模型验收。

补充产物: `/home/ref/market-stream-acceptance-20260906-mQnDhT`，仅解出两份 scaler JSON，保存本轮 Tick 输出；未覆盖已有模型。候选包仍标为 `production_approval=false`，不自动升级为实盘模型。深市要求的模型文件在本轮检索中尚未找到。

## 下一次从这里继续

1. 使用已通过局部回归的 `sse-per-instrument-v2` 固定实际模型、静态数据和行情录制，建立全天新行为基线。归档 `PredictionEngine`、包内 `StrategyBase` 和此前全局采样器的不同规则仅作历史差异说明，不再是待选方案。
2. 在已经完成的两市 owned-runtime/共同策略接口上接真实券商适配，补足 query completion、账户归属、在途订单与迟到回报的对账。恢复就绪不等于交易就绪，也不能把新 `.t0md` 当作旧 journal ABI。
3. 确定旧 main 的部署迁移或兼容方案后，使用显式生命周期入口替代旧 SSE get_obj；保留同一 Session，不重新引入归档中的另一份模型/采样实现。取消/回报通道不能随“禁止新增风险”一起关闭。
4. 固定实际使用的模型、静态数据和录制，显式验证 artifact/run lock，做全天逐层数值对照，然后才进行性能、券商组合与发布验收。
5. 上述接线稳定后，再单独执行 `usagi` 的名称及目录迁移，不与行为变化混在一个改动中。

本轮使用 OpenSpec 将任务、验收与未决项分开记录；Luna 承担有界接线、配置映射、测试及来源清点，主代理负责并发/时钟/错误策略、整合和复核。继续工作先读本文、任务清单及需要修改的接口，不重复加载整个 review 和所有源码。
