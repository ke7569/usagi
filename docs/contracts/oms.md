# OMS: 账户、订单与 TD 边界

实施日期：2026-09-06。变更记录：`openspec/changes/integrate-account-oms/`。

## 职责与实际接线

OMS 是进程内的公共模块，不把券商协议搬进策略，也不增加网络服务。一个资金账户只有一个 `oms::Engine`；沪深会话和多个策略客户端共享它。TD 是可替换的 backend，保留连接、SDK 发送、查询和报文转换。资金、成交增量、挂单占用和撤单状态只由 OMS 修改。

```text
SZE/SSE processor -> StrategySession -> ZStrategy -> ProtectedExecution
  -> OmsStrategyExecution -> Engine -> PaperBackend / AtpBackend
                               ^
          ScriptedBackend / TD normalized reports
                               |
                 strategy reads Position view
```

`apps/StreamProcessingCli.h` 已实际装配 OMS，两市 capture/replay 均使用这条链。`PaperBackend` 只记录意图，不制造受理、成交或撤单成功；活动占用不会因意图输出自动消失。脚本化 backend 通过 `publish(Report/Snapshot)` 注入明确事实，供测试宿主使用，不是撮合模拟器。CLI 没有开放任意脚本或真实 TD 的配置开关。

`ZStrategy` 原有累计成交 map、逐笔去重 set、终态释放逻辑和发送后 pending 增量已删除；策略中的仓位字段只是当前 OMS 视图的缓存。旧 `PaperExecution` 和其独立定时器已删除。`ProtectedExecution` 只保留会话门禁和路由校验，不再在发送返回后登记 request ID。旧 LF 会话回报入口返回 false，不能用缺少 epoch 的报文更新新账本。

策略意图的 `signal_id` 关联股票、会话样本序号和接收时间。`SubmitResult.accepted` 表示 OMS 已保留该意图，不是券商受理证明；Unknown 仍保留内部 ID，调用方应读取 `OrderView` 和账户门禁。每个策略客户端/会话线程使用自己的 `OmsStrategyExecution`，共享的是 Engine，不是可变的信号上下文。

## 身份与账户所有权

- 账户键：`broker + account`，真实 ATP 使用精确的资金账户号，不用股票账户替代。
- 回报作用域：账户、gateway、day、source、epoch，必须完整匹配。
- 内部 ID：同一账户日内递增；当前兼容策略接口限制在正 `int` 范围，外部只读订单使用独立高位范围。
- 意图唯一键：`owner + intent_id`；同一 owner 重复意图不发送。不同 owner 可以使用相同的意图文本，但不能撤对方订单。
- 券商 ID：在连接代次内绑定到内部订单；不按证券代码认领订单。
- 一个实际账户跨沪深/策略共享同一个 Engine。每个客户端读的是账户总持仓，不是独立利润或分配子账。

真实 backend 必须配置 `ExclusiveLocal`、非空 journal、`single_host_account=true`，并取得 `/run/usagi/oms/accounts` 下按账户编码的排他文件锁；该目录必须预先由部署管理员建立并让全部账户进程看到同一底层目录。锁文件不得在持锁期间删除。不同宿主、容器私有挂载或不经过本系统的其他交易软件不在文件锁保证内，不允许声称这些组合获得了共享账户风控；当前没有跨主机 OMS 服务。

`Simulation` 只能配模拟 backend。两个 CLI 进程中的 Paper 账户是两个独立实验，不是跨进程共享预算。真实账户日志路径必须按账户/交易日隔离；即使误用相同文件，文件锁和 journal header 也会阻止混用。Header 固定账户、gateway、source、交易日、instance、成交覆盖口径和费用预留，不能通过换配置重解释历史成交。

## 准入、数量与资金

准入在同一账户锁内完成规则检查、预占、唯一 ID 登记和动作排队。检查现金、T+1 可卖量、活动订单、净持仓上限、价格区间、tick、lot、单笔数量/金额、日内身份容量及账户报撤配额；对向挂单价格交叉会拒绝，包含外部订单和待撤未确认订单。配额在实际延迟派发时再次校验；尚未交给 TD 的动作可以明确 NotSent 拒绝，不会当成发送未知。

价格/金额使用 `int64`，单位为人民币万分之一；数量为股，时间为单调纳秒。计算检查溢出。规则来自装配配置，策略的既有仓位公式不改。

| 事实 | 处理 |
| --- | --- |
| 本地拒绝或 TD 明确 NotSent | 不发送或仅释放本订单占用；保留统一错误及原始 code/type |
| Submitted | 只表示发送动作已交给通道，不代表已受理或已成交 |
| Unknown、发送抛异常 | 保留本单占用，关闭账户新单，等待对账；不重发 |
| 累计 Order 与 Trade | 每单维护累计高水位、稳定 Trade ID、成交覆盖和已应用增量 |
| 累计量回退 | 记录可观测的回退，不能重复增加 leaves 或释放占用 |
| 终态后新增成交 | 接纳可确认的新事实、标记异常并关闭新单；不能再次释放其他订单 |

正常数量满足 `original = filled + working + canceled + rejected`；超过原始数量等矛盾保留异常事实，不静默截断为正常状态。

`DualFromOrigin/TradesFromOrigin` 只能用于已证明从订单起点完整、无缺口且按成交顺序交付的逐笔流。否则需要逐笔 `cumulative_after`，或声明 `UnknownOverlap` 并进入对账；不能把缺失明细后的新 Trade 简单叠加在 Order 累计量上。缺稳定 Trade ID 不编造去重键。重复成交可补充此前未知的价格/费用，但不能改写已有不同事实。

成交数量确认不代表价格、金额和费用已完整：

- 买单未定价成交继续按限价保留成本占用；不把委托价格记作实际成交价。
- 已知成交价按实际金额记入现金；卖单未知价不产生可花费的卖出收入。
- `fee_reserve_per_order` 是显式风险预算，不是伪造成交费用；没有完整费用证明时仍保留该预算。
- 买入不增加当日可卖量。`Position.sellable` 是扣除已成交卖单、但扣除未成交卖单前的可卖量；可再卖量为 `sellable - working_sell`。
- 超预算、矛盾事实、未知交叠或容量耗尽关闭新单；终态和正常成交回报仍可进入，已知归属的减险撤单不因新单健康门禁而丢失。

## 串行化、撤单和停止

状态锁保护账本；独立 dispatcher 串行调用 backend，调用 SDK 时不持有状态锁。同步回调或其他线程早到回报可以进入 OMS；重入的 `drain()` 不会抢占正在发送的 dispatcher。发送前登记订单，因此不依赖 SDK 返回后才建立关联。只有券商 ID 的早到回报进入有界隔离队列，发送返回后重新关联；超时或无法关联继续禁止新单。

`NativeFak` 与 `LimitThenCancel` 是不同能力。当前策略仍采用限价后撤，延迟 1001ms、从 dispatcher 派发时所见的 OMS 时钟计时；核心也支持从受理回报应用时的 OMS 时钟计时。未记录的计算/存储墙钟耗时不被编造到 Paper 回放中。原生 FAK 不支持时明确拒绝，不悄悄降级。

每单最多一个撤单定时器。缺券商 ID 时保留待撤意图；重复请求合并；Temporary、RateLimited、MissingId、Unknown 按受控时钟重试。默认最多 5 次发送、30 秒等待，终态立即移除定时器。发送撤单不释放原单，AlreadyFinal 类错误若没有终态事实也不能擅自释放。次数耗尽需要对账/人工处置，不无限自动重试。Paper 不产生撤单确认，也不因此无限重复输出撤单意图。

停止先关闭新单；已经越过 dispatch 检查的动作属于在途，不承诺回滚。停止仍允许已有订单回报和合法撤单；析构不推进时钟、不补发定时撤单、不发送新单。回放速度和 EOF 不能生成交易。实盘定时服务仍需由未来获准的 live host 驱动，原始行情录制不是完整账户事件回放。

## 恢复与持久性

首次启动或重连都需要递增 epoch；查询 token 递增且作用域完整。`begin_reconcile` 关闭新单，记录账户活动水位；资金/持仓/委托/所需成交必须显式成功结束。查询期间有回报、发送或连接活动会拒绝该快照，重新查询，不尝试猜测如何拼接非一致快照。

快照的现金和可卖量是券商冻结后的可用量；OMS 加回快照中已证实的活动买单本金、活动卖单数量，再重建统一占用。费用预留另行保留。配置股票必须全部有明确持仓结果（包含零持仓）；不得以少量 `isLast` 回调或只有第一页结果充当完整快照。

历史自有订单须同时匹配内部 ID、owner、证券、方向、价格、原始数量及券商身份。外部订单只读，但加入现金、可卖量和自成交检查。未知发送若不在查询结果中，除非通道明确具备完整历史缺席证明，否则仍然未知且保留占用。快照成交水位结算前的金额不会因重复明细再扣一次；跨水位而不可判断的成交会再次关闭门禁。

Journal 是有版本、连续序号、长度上限和 CRC 的分帧记录，默认单帧 1 MiB、单文件 1 GiB。真实发送前，意图必须已 `fdatasync`；该持久意图本身表示崩溃后“可能发送”。dispatch、发送结果和回报随后记账，下一个持久意图/快照同步会覆盖之前记录。不得因缺少发送结果或 dispatch 记录就认定未发送。正常析构同步已有记录，不产生交易。它不是普通异步日志的可靠性承诺，也不承诺磁盘硬件没有违反同步语义。

快照分块记录并以同步 commit 收尾，未完成的快照不替换已提交状态。恢复后活动订单先标 Unknown，禁止自动重发，重新查询；冷启动至少观察一个完整报撤窗口后才允许新单，不能跨进程启动沿用旧单调时间定时器。

CRC/截断/序号/版本/身份错误不会自动修复或截尾；保留文件并关闭新单。记录失败前未发送的新单不得发送，已有归属的撤单允许继续但显式保留持久化故障。异步实现中 `audit_sequence` 是已接收记录水位，`written_sequence` 是后台已写水位，`durable_sequence` 是已同步水位；`durable=true` 要求当前接收记录全部同步。`journal_pending` 和 `audit_sink_pending` 分别显示记录队列和可选文本输出队列的近似积压。损坏日志的有效前缀仅是已读事实，不被伪称已同步。恢复正常文件时重新同步后再报告 durable 水位。

故障处置顺序：关新单、保留原日志与错误证据、取得完整账户查询、处理未决和外部订单，再通过新的查询 token 恢复。不要删除日志、重置 ID、清空 pending 或复制其他账户的资金来消除报警。日志硬故障需人工恢复到经核对的副本，当前实现不提供自动修复按钮。

## ATP 支持矩阵

| 部分 | 当前状态 |
| --- | --- |
| 公共 OMS、共享账户、多客户端、活动单恢复 | 已实现并离线测试，不链接券商 SDK |
| SZE/SSE 实际策略 capture/replay | 已接入 OMS；Paper 只输出意图，不填单 |
| ScriptedBackend | 核心、策略与故障测试使用实际实现注入归一化回报 |
| ATP 发送/撤单和 Rsp/Rtn 转换 | `make_oms_backend` 绑定实际 TDEngine；使用现有 SDK 方法，两个市场 TD 构建通过 |
| ATP 真实报文序列、资金口径和恢复 | 受限，`complete_snapshot=false`，新单不可开启；没有生产账号/报文验收证据 |
| 旧普通 TD / direct C API / Framework 发送 | 明确拒绝；启动自动撤全部订单拒绝配置并禁用，不能绕过 OMS |
| 旧 SSE get_obj、生产 host、国君协议 | 未开放/未选择；没有部署、实盘连接或真实报撤 |

SDK **存在**资金、持仓、委托及 `ReqCashTradeOrderQuery`，不是缺 SDK。旧 TD 缺成交查询接线及委托/成交的完整分页；`isLast` 不能单独证明跨查询一致性、重连回放结束或准确交易日覆盖。还需用本账户真实协议确认资金可用字段、冻结口径、分页索引/结束语义和重连边界，再实现并认证完整查询适配，才能把能力改为 true。不能仅因头文件有方法而开放交易。

ATP 仅以真实 `LastQty/ExecId/LastPx/CumQty` 生成成交事实；没有 LastPx 时保留未知，不回退到委托价格。异常 send 返回按 Unknown 保守处理，直到错误码语义得到证明。backend 生命周期绑定一个 SDK generation；关闭后不重绑旧 SDK 对象。TD 路由缓存仅用于协议关联，不能充当第二份持仓或风险账本。当前 ATP account unit 只允许其配置的市场，不能拿深市证券账户字段猜测沪市路由；实际跨市场证券账户路由也必须在开放共享真实账户前核实。

构建 SDK 目标后，本机检查需让加载器找到授权 SDK，例如设置 `LD_LIBRARY_PATH=/home/usagi/adapters/td/atp/api/lib`；不复制或发布厂商文件。旧插件路径和导出名保留用于明确失败，不代表旧宿主仍能下单。

### ATP Query Bridge Candidate

The adapter now dispatches fund, share, order, and trade queries with OMS
scope/token correlation and collects their normalized results into a snapshot.
Query-all requests use the installed SDK's ReturnNum=0 form; an intermediate
callback does not trigger another page request. Account identity, sides,
duplicate rows, and late callbacks are checked. Query dispatch is serialized
with transport close, and synchronous snapshot publication is supported.

This bridge retains complete_snapshot=false and false all-day coverage flags.
Query rows are external (id=0); an ATP batch number does not prove OMS ownership.
Fee values do not imply final fee settlement. Own-order attribution, complete
zero-position coverage, consistent query boundaries, and reconnect/day coverage
must be established before certification and real-host integration.

## 验证与性能

基线为迁移后的 `/home/usagi`，原有 34 个 CTest 全通过后开始 OMS；保存于 `/home/ref/usagi-oms-baseline-20260906-Egyu7w/source.tar.gz`，SHA-256 `af96a1bf377c84886e377f521bc7b7125ef1523d73e5ad224e5f19665e3832d6`。与基线逐目录比较确认 `common/{factors,model}` 和两市 `{factors,sampling,market_data}` 未变。

新增 `oms_test`、`oms_recovery_test`、`journal_test`、`atp_boundary_test` 使用始终执行的业务检查，涵盖早到/重复/回退/迟到回报、未知资金、费用补全、共享预算、账户归属、撤单、停机、受理时钟、孤儿回报、容量、活动单恢复、外部订单、查询重叠、损坏日志与真实子进程崩溃。后者还模拟丢失完整非持久 dispatch 帧，验证不释放未知订单。ATP boundary 用受控 sender，不代表 SDK 线协议验收。

预期行为修复：旧无预算配置拒绝；非法 tick/lot、超预算及自成交意图拒绝；不再重复扣 pending 或忽略 Order 后的 Trade。沪市合成夹具原有 9.999/10.001 报价不符合股票 tick，已改合法价位，并降低该测试的策略 offset 以保持确定触发；不改生产模型或策略公式。

复现命令（当前环境 CentOS 7.9、GCC 4.8.5、C++11）：

```bash
cmake3 --build build/verification -j4
ctest3 --output-on-failure  # 在 build/verification 内执行
cmake3 --build build/verification --target oms_benchmark -j2
build/verification/oms_benchmark
cmake3 --build build/verify-td --target sze_td sse_td -j2
cmake3 --build build/verify-deepwin --target t0_strategy_sze t0_strategy_sse -j2
```

微基准是固定账户、10000 个唯一买入意图、每单 100 股、Paper observer，无 SDK/网络/撮合。它比较直接 observer 下界与 OMS 准入/登记/限价后撤，不是完整行情策略的前后实盘延迟。内存模式与持久模式不能互相冒充可靠性等价。当前本机同步存储的毫秒级耗时不能满足低延迟实盘要求；生产准入还需在目标本地持久介质、真实 CPU/账户负载上制定并验证延迟预算，不能为通过延迟指标跳过必要持久化。

2026-09-06 的本机观测（`-O3 -DNDEBUG -march=x86-64 -mtune=generic`，未绑核，共享构建机器）：

| 路径 | 次数 | p50 / p95 / p99 | 最大值 | 测量结束前 RSS |
| --- | --- | --- | --- | --- |
| 直接 Paper observer | 10000 | 0.591 / 0.601 / 0.601 us | 12.914 us | 1616 KiB |
| 内存 OMS 限价后撤登记 | 10000 | 40.385 / 44.824 / 50.505 us | 149.610 us | 13764 KiB |
| 持久 OMS，同步意图 | 100 | 2.970 / 3.163 / 5.775 ms | 8.320 ms | 3248 KiB |

内存模式结束时订单/活动单/定时器各 10000、审计尾部 20001、累计 journal 记录 30006；该基准显式提高容量，不是默认 1024 条审计尾部。持久模式对应 100/100/100、201、306。RSS 取 `/proc/self/statm`；`getrusage` 的启动器高水位在本环境不可作模块内存增量。观察时没有业务派发积压；保留活动单是 Paper 不制造成交的正常结果。

当前验收记录：38 个 CTest 通过，45 个配置单元测试通过，OpenSpec strict 校验通过；两个 ATP TD 及两个旧 Deepwin 策略目标编译通过。依赖检查在显式 SDK 库路径下无缺失，策略仍引用 `kungfu::wingchun::IWCStrategy`，没有错误的全局 `IWCStrategy` ABI。未做生产连接、全天实盘数据对照、部署、提交或推送。

## 分段诊断 (2026-09-07)

本次只改测量，不改变风控、订单状态机、发送顺序或持久性承诺。旧的约 40 us 指标包含构造意图以及发送后记账，不能直接称为 OMS 到 TD 的发送前延迟；上面的历史数值保留，不与新口径宣称优化前后等价。

### 计时口径

所有意图与字符串 ID 在计时前构造，初始化/对账和 32 单预热在计时外。测试直接调用真实 `Engine::submit`，不经过行情、模型、策略与策略适配层。固定单线程、SZE 一只股票，每单买入 100 股、价格 10 元、100 秒限价后撤；仅登记定时器，不推进事件时钟，不触发撤单，不制造成交。订单/定时器由 32 增长至 10032，属于累积活动订单负载，不是持续终结订单的稳态负载。

| 测量模式 | 边界 |
| --- | --- |
| normal / total | 预构造意图提交前到 `Engine::submit` 返回；普通 Paper backend，没有内部计时探针 |
| normal / coarse | 同一外层边界，另在 `TimedPaperBackend::submit` 入口/返回前各读一次时钟，划分 pre_backend、backend、post_backend |
| profiled / total、coarse | 同一源码诊断构建，但未启用内部采样，用于观察编译进探针后的非激活开销 |
| profiled / detailed | coarse 加启用所有内部 Scope；Session/Sample 初始化与销毁在计时外 |
| profiled / focused | 普通 Paper backend，每次仅启用一个指定 Scope，降低观测干扰；不输出 coarse 三段 |

`backend` 仅为 Paper 函数体及固定大小观察记录，绝不是真实 SDK 时间。coarse 中 backend 入口之前的调用包装开销归 pre_backend，末尾打点之后的返回包装归 post_backend。外层返回值赋值包含在 total 内；断言、分位数计算、结果 JSON、全量订单检查、对账和析构均在计时外。

`t0_oms_profiled_core` 从与正常核心相同的源文件构建，额外启用 `USAGI_OMS_PROFILE`。固定大小 TLS Sample 和栈上 Scope 保存 inclusive/exclusive 时间及次数，不写交易日志。正常 `t0_oms_core` 的宏全部编译消除，符号检查没有 `oms::profile`、`steady_clock` 或 `clock_gettime` 引用。诊断库只由测试/基准链接，未替换运行库。

inclusive 含子阶段，exclusive 排除子阶段。逐单检查全部 exclusive 之和等于 submit 根 Scope 的 inclusive；不把父子耗时重复相加。分位数来自各自分布，不可把不同阶段的 p50 相加当作某笔订单的 total。

### 分段结果

环境仍为 GCC 4.8.5，`-O3 -DNDEBUG -march=x86-64 -mtune=generic`；本轮进程固定 CPU 16，不修改系统频率/调度设置，不与本任务编译或测试并行。每组顺序运行 3 次，内存每次 10000 单，持久每次 100 单，另有各自 32 单预热。临时文件在 `/tmp`，文件系统为容器 overlay，不是生产存储验收。

下表为三次运行的 p50 范围，单位 us：

| normal 核心，内存模式 | p50 范围 | p99 范围 |
| --- | --- | --- |
| total-only，总调用 | 39.35-39.71 | 见原始 JSONL |
| coarse，发送前 | 30.68-30.91 | 33.64-34.36 |
| coarse，Paper backend | 0.63-0.64 | 0.72-0.73 |
| coarse，返回后记账 | 10.53-10.74 | 12.52-12.59 |
| coarse，总调用 | 42.01-42.31 | 见原始 JSONL |

两次额外时钟读取和 backend 包装让 coarse 总量比 total-only 高约 2.3-3.0 us；不能将差额当业务优化。全部内部探针开启后总量达到 74.78-75.15 us，说明 detailed 的全量分解只能用来定位，不能直接作为正常路径的绝对成本。

每次只测一个阶段后，内存模式的 p50 如下（单位 us；这些阶段仍包含其自身探针开销）：

| 独立采样阶段 | 每单调用次数 | p50 范围 |
| --- | --- | --- |
| 意图审计：构造业务 JSON、封套、dump、内存 append | 1 | 22.71-22.93 |
| 派发审计 | 1 | 4.68-4.70 |
| 返回结果审计 | 1 | 9.57-9.60 |
| 准入校验/风控 | 1 | 1.31-1.33 |
| 资金/持仓预占计算 | 1 | 1.02-1.03 |
| 订单登记、映射、队列/配额及注册审计事件 | 1 | 2.45-2.47 |
| 三次封套构造与 JSON dump，合计 | 3 | 21.36-21.75 |
| 三次内存 journal append，合计 | 3 | 1.92-1.93 |

最后两行已包含在前三段审计内，不能重复计入。单阶段激活比同组 inactive total 大约增加 1.0-2.2 us；每单调用三次的探针增加约 4.1-5.6 us。微小阶段接近本机读时钟的成本，不应依据这些值细分纳秒级排名，更不直接减去时钟 p50 来伪造精度。

持久模式 normal total 的 p50 为 2.972-2.990 ms，主要仍在发送前；移出意图构造和澄清计时边界没有消除同步存储等待。

### 结论与下一步

这几十微秒主要是当前实现的同步日志文本加工，并非风控必然需要几十微秒。优化顺序应先处理意图/派发/结果的 JSON 构造与格式化，再验证对象分配、复制、登记路径的剩余成本。发送后结果日志虽然不拖延当前单进入 backend，却占用 dispatcher、影响后续单吞吐，不能只盯当前单发送前指标。

分段诊断完成时尚未实施日志编码替换、异步写入和账户锁拆分。后续日志优化见下节；是否取消发送前持久确认仍是独立的可靠性选择，不能把测量结论当成已授权降低持久性。

原始数据和跨模式状态检查汇总在本地 `build/oms-latency-20260907/`：`normal.jsonl`、`profiled.jsonl`、`focus-*.jsonl`、`summary.json`。所有重复运行的订单摘要、资金、可卖量、挂单、定时器和 journal 记录数一致。当前全部 42 个 CTest 通过，包含探针嵌套/异常/线程隔离、同步回调、NotSent/Unknown、诊断版原核心/恢复用例及基准输出契约测试；OpenSpec strict 校验通过。本步未构建或部署新 TD、未连接实盘。

```bash
cmake3 --build build/verification --target oms_benchmark oms_benchmark_profiled -j4
build/verification/oms_benchmark --cpu 16 --repetitions 3
build/verification/oms_benchmark_profiled --cpu 16 --repetitions 3
build/verification/oms_benchmark_profiled --cpu 16 --repetitions 3 --mode memory --focus intent_audit
```

CPU 16 仅是本次机器的选择，在其他环境需选择实际可用 CPU。可用 `--orders`、`--durable-orders`、`--journal-root` 控制负载和临时文件所在介质；所有输出为 JSONL，单位 ns，时钟开销未扣除。工具仅删除自己创建的临时 journal/lock/目录。

## 异步日志优化 (2026-09-07)

订单意图、派发、发送结果、回报、撤单与定时器日志改用 `Records` 的有版本紧凑二进制载荷。完整意图只记录一次，后续派发/结果按订单 ID 引用，不重复输出整份账户与委托。外层 Journal 的序号、CRC、长度限制不变；新版本能读取旧 JSON 及新旧混合帧，旧程序不能读取新二进制帧，回滚不得直接用旧程序续写新日志。

交易线程只编码必要字段并入有界队列，不构造/dump JSON，不执行普通日志磁盘写入。无文件也无输出 sink 的纯内存模拟跳过无用编码，保留序号和有界内存审计尾部。初始化、epoch、对账快照等冷路径仍可使用 JSON。

`AsyncJournal` 的单消费者后台线程负责磁盘写入；默认最多 8192 条、8 MiB 待消费载荷，单条仍限 1 MiB，消费后释放载荷内存。普通记录不等待消费者；空闲后台每 1ms 检查队列，持久文件有新记录时约每 10ms 发起同步，这不是硬实时落盘期限。发送前持久意图、epoch、快照 commit 与显式 sync 仍等待持久确认。撤单派发记录不再等待刷盘，缺失撤单记录不会释放原单或证明已撤成功。

可选 `Config.audit_sink` 收到原始记录，在专用输出线程运行；若同时启用 WAL，文本输出和 WAL 使用分开的队列/线程，慢打印不会串在持久确认之前。输出端可调用 `records::format(payload)` 得到精简 JSON 行，仅保留时间、事件、订单关联、核心数量/价格/状态与错误，不重复账户配置。完整恢复依据是 Journal，不是精简文本。sink 必须自持有输出资源，禁止回调 Engine 或依赖其销毁后的对象，输出失败必须抛异常；使用缓冲输出，不要逐行 `std::endl`/flush。CLI 仍是原有 Paper 装配，没有新增真实交易开关，也没有默认刷屏日志。

任一队列满或后台故障均明确关闭新单，不退化为交易线程同步打印，不静默丢日志并继续冒险交易；已知订单撤单和内存回报处理保留。正常退出等待已接收记录写完并同步；异常退出仍可能丢失尚未持久的普通记录，保留预发送持久意图与重启对账要求。输出设备永久挂起可能拖延退出清理，不能把“交易线程普通入队不等 IO”解释为“任何环境下关机无等待”。

本次不修改准入风控、预占、采样、因子、模型和策略公式。正常核心仍不包含 `oms::profile` 探针；新增后台线程自身使用时钟安排批量同步，不属于交易路径的性能探针。
