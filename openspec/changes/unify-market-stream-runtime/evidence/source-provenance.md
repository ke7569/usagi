# 市场流处理代码来源

核对日期: 2026-09-06。范围: 本次独立市场流处理构建所采用的源码来源与局部行为对齐。本文不表示两个分支已完成合并，也不表示现有交易插件已完成迁移或可发布。

## 固定来源

来源信息与[重构计划](multi_market_multi_broker_refactor_plan.md)一致，并已执行本节及下一节的只读校验。

| 来源 | 固定标识 |
| --- | --- |
| 深市 | `5354734db64c80c59dc5af68ea0272df11700826` |
| 深市审查副本 | `/home/ref/sze-sse-review-20260906-XLiZYg/sze-t0-live` |
| 沪市源码包 | `/home/ref/sse-t0-live-source-20260906.tar.gz` |
| 源码包 SHA-256 | `9c033c6bf82c5b7ebfb730cc3da0be08d4d433b02caff9cbd06468bc00bcf3b1` |
| 沪市审查副本 | `/home/ref/sze-sse-review-20260906-XLiZYg/sze-t0-feature-sse-t0-latest` |
| 本次工作区 | `/home/sze-t0`，既有 `feature/sse-t0` 工作区上增量修改 |

沪市包没有 Git 历史，无法据此确定父提交。包遗漏的 `reference/snapshot_legacy15` 不视为删除意图；当前工作区已存在这份依赖。本次没有整包覆盖工作区，也没有将已有未提交修改归为上游源码。

## 逐字导入的沪市文件

最初 10 个文件直接来自固定沪市副本，导入时分别执行 `sha256sum`，两边全部一致；随后补入 Auction59 sidecar 两个文件。下表记录源文件哈希，路径均相对仓库根目录。解码器后续的局部修改另列于表后，不再声称它当前与来源逐字一致。

| 文件 | 来源 SHA-256 |
| --- | --- |
| `sse-t0/market_data/sse_primary_decoder.h` | `2e21eac5f3e73703853645cd0ffed6b8866aaf1a4fe74e330e5e416153f6efa5` |
| `sse-t0/market_data/sse_primary_decoder.cpp` | `7cfc96cc68bbfbfb9a6e7b26888f0e56d5f99bc7b485958bc1b731923304d21b` |
| `sse-t0/market_data/sse_tick_order_book.h` | `b7c5e60fe619f6deeb0e9199cd11165d33164ffc26c65527381188babf6ec503` |
| `sse-t0/market_data/sse_tick_order_book.cpp` | `8185244353df9c1681e807e38fa1b86f72f4582b271021ec3eb201c7a1953646` |
| `sse-t0/market_data/sse_tick_factors.h` | `29631aff342648e0e6446c922118494632c998a7f542aef89496ef51b18705d8` |
| `sse-t0/market_data/sse_tick_factors.cpp` | `a29a9597ef4a3358502097e7dbacd3dda65d3bc4b431588efb103cf7f361ba15` |
| `sse-t0/market_data/sse_tick_static_metadata.h` | `023c2ebf71c923e9a9ade96aef43e638c36c49a969e2986619b2f5bee319fbec` |
| `sse-t0/market_data/sse_tick_static_metadata.cpp` | `a506b67b16454383bbe58474600f4c52046a7b9a3c8fac60a3c65a17723c5294` |
| `sse-t0/model/snapshot36.h` | `94f889a59ed7c9189cdedcc96c0f5f36acb26abd1f87dfd9831fa31e38ff1eff` |
| `sse-t0/model/snapshot36.cpp` | `72434db9ac98f452498664ee75cf0fb3d27eb2deee0aebe924d8f71724021f49` |
| `sse-t0/model/auction59_sidecar.h` | `d4d5341b42d6eaffe50fc9d02be086423351b7af1c7eafc4c59ddde27f5654ec` |
| `sse-t0/model/auction59_sidecar.cpp` | `5046ac40fe3a271d7fa532bdf58c892eabb3fab2e1e3d9e974b879b4abcf4215` |

`sse_primary_decoder.{h,cpp}` 新增默认保持原行为的 `equities_only=true` 参数。公共处理器显式使用 false，先解码合法的频道内非目标证券以维护序列连续性，再过滤股票范围；这避免把未订阅股票/ETF 误当全流损坏。没有把这些证券送入股票模型。

## 局部行为对齐

- `src/t0-main/predictor/mix153060_runtime.cpp`: 在 `Runtime::Impl::maybe_emit` 引入深市最新版本的 9 行开盘边界处理。当窗口起点恰为 09:30 且首个开盘后事件到达时，更新窗口起点、清空流量窗口并返回，不额外产生一个模型样本。此修改影响相对于旧工作区的采样及 GRU 推进；修改后该文件与固定深市副本 `diff -u` 无差异。
- `sse-t0/model/snapshot_ensemble.cpp`: 从旧工作区的 09:35 生成截止，对齐固定沪市源码实际采用的 `34860000000` 微秒，即 09:41。当前与固定沪市副本的差异仅为截止时间注释和错误消息中将误写的 `09:40` 更正为 `09:41`，计算和路由逻辑相同。
- `sse-t0/model/sse_hybrid_model.cpp`: 对齐固定沪市源码的预测重叠窗口，Snapshot 在 `[09:30,09:41)` 继续生成，交易来源选择仍在 09:35 切换至 tick。当前与固定沪市副本仅有说明注释差异，执行逻辑相同。生成时间和选用时间不能混为一个条件。

这里的来源一致性通过源码哈希和差异检查确认，不等于已完成全天数据的因子、预测或订单意图数值验收。新处理器与测试属于本次新增实现，不计入上述逐字导入清单；它们的行为验证由相应实现任务记录。

## 当前沪市采样口径与历史差异

2026-09-06 用户已明确沪市唯一采样口径，新入口命名为 `sse-per-instrument-v2`，替代此前的 `sse-batch-end-v1`。每只配置股票各自判断严格大于 100000 ns 的静默，其他股票和 Snapshot 仅推进观察时间，不延长它的期限。全局接收 idle 记录是传输时钟观察，不是沪市采样决策；回调在处理到满足条件的记录时间时执行，不承诺硬性 100us 墙钟延迟。

首个交易所时间不早于 09:30、更新后形成有效双边盘口的逐笔事件立即初始化窗口，不产生样本，也不等待首个 quiet cut。窗口记录 mid、累计成交额、累计成交股数和交易所时间，并清除初始化前流量。后续静默 cut 满足任一条件才采样：金额增量 `>= HistoryAmount/8000`，交易所时间差 `>= 100000000` 微秒即 100 秒，或者 mid 绝对变化 `> 1e-6` 且成交增量 `>= 100` 股。金额及 mid 使用归一化货币单位；成交量门槛使用股，因子内部仍保留既有线协议数量单位。

每股同一交易所时间至多产生一次样本；拒绝同时间 cut 不清空流量、不重置窗口。只有初始化或接受样本才推进窗口。`BatchEndSampler` 由股票 ID 列表构造，使用每股至多一个节点的有界索引堆；`advance_to_event/on_timer` 返回到期批次列表，`commit_applied_event(id, candidate, healthy)` 只更新对应股票。生产路径没有并行的 `MonotonicOneShotTimer` 实现。

固定沪市包的 `sse_tick_prediction_engine.cpp::sample_tick` 使用过 09:25 前收盘种子和不同触发条件，包内 `StrategyBase` 也有独立 quiet 判断；此前全局采样器还有首个 quiet cut 初始化的行为。这些均已被上述用户规则替代，不是待选择的运行模式。旧 `PredictionEngine` 仅保留在不可变审查归档中用于审计，归档文件保持原样。当前修正是有意的行为变化，不能声称与全部历史运行结果数值等价；全天对照及旧插件迁移仍需完成。

对应声明为 `activity_scope=per-instrument-sse-book-update`、`same_exchange_time_policy=at-most-one-sample`、`initial_window=first-valid-book-at-or-after-open`。配置与原生入口拒绝冲突声明及旧 stream profile。修订后的 14 项相关 CTest 已通过，包括逐股静默、同 ExTime 累积流量、开盘初始化与旧 profile 拒绝；完整结果和验证范围见执行记录，源码来源一致性仍不等于全天模型数值验收。

## 现有插件边界

当前 `src/t0-main/strategy/StrategyBase.{h,cpp}` 尚未导入沪市包中的 `SseHybridRuntimeState` 及 hybrid 接线，因此没有原 review F1 中新增的 SZE 对 SSE hybrid 模型未定义符号依赖。旧的 `SzePredictor` / `SsePredictor` 仍在共享源列表中，这层既有耦合尚未拆除。

当前 `sse-t0/strategy/sse_get_obj.cpp` 仍明确提示 hybrid 行情/因子未接线并返回空指针，保持禁用。独立流处理目标使用各自市场的订单簿、采样、因子和模型实现，并不替换这两个 Deepwin 策略插件的入口，也不能据此声称插件已经独立加载成功。

已用现有两种宏配置对当前 `StrategyBase.cpp` 执行 `c++ -fsyntax-only`，均通过。当前默认 `runtime_so` 目录不存在，但后续检查 Git 忽略目录后，在 `/home/bse-t0/runtime_so` 及本项目旧 handoff 包中找到了依赖；不能据此前置检查声称本机完全缺少 SDK/模型。`sse-deepwin-deps-20260819/SHA256SUMS` 的 29 项均通过。本次没有重建或部署交易 `.so`，没有加载交易插件、启动 TD 或登录接口。完整插件集成仍需处理原 review 中的问题，不能以独立工具通过替代。

## 已找到的历史候选模型

`src/t0-main/build/configs` 被 Git 忽略，但包含历史模型候选。以下不是本次合成测试权重：

| 文件 | SHA-256 |
| --- | --- |
| `sse-tick-v04-20260817/ssemodl1.bin` | `b504a0a987d9c4b5adb5677da30f1535ba73f852682f96152320ce7d9dec1139` |
| `sse-snapshot-gru-20260817/baseline.ssegru` | `196830886906c633c75e3d64e72d3261269ffee2b70db94a1657eb54fa890537` |
| `sse-snapshot-gru-20260817/auction59.ssegru` | `231b5dd0b5de0d9cd868e4f6620d6bab6cf8d897475166d7fb36c3659ddaaa2c` |

Snapshot 的 `sse-snapshot-gru-20260817-final.tar.gz` 归档 SHA 为 `a7516c1245cfe59af1a8752fb7328c5c467750b7b7d93a794df6e290e578d7ff`，内含 `sse-snapshot-gru-20260817-final/models/baseline.json` 和 `auction59.json` 两份 scaler。其 SHA 分别为 `ce38021a04361b5bf6f83ef1f29a069b84a6ec5f3413f54a1110a8cdd2277cb2`、`fe5b923d831ae1c382ece98fd7abbba406dc7a7a798bfa68cb70905af5956a0c`。

三份候选权重的加载/零输入推理已通过，Tick 的 8 行零输入预测及最终 hidden 与旧 `zero.predhidden.f32` 字节一致。包内状态仍是 `candidate_requires_independent_validation`、`production_approval=false`；本次未重做原训练权重到二进制的完整转换核验，也未证明它们就是当前券商生产机使用的版本。详见本轮执行记录。
