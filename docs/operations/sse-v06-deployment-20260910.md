# SSE v0.6 接入与验证（2026-09-10）

已在 `/home/zane/usagi-sse-dev` 实现并部署 `v06-b15-mh4-s43-sse-20260910`。当前 journal 预测启动配置选择 v0.6；旧模型、旧策略源码、独立旧版配置和上一版生产程序均保留。真实报单仍关闭。

## 版本入口

- 当前启动器：`/home/zane/usagi-sse-dev/tools/sse/run_journal_trading.py`
- 当前配置：`/home/zane/usagi-runtime/journal_trading.live.json`，`model_version=v0.6`
- v0.6 配置：`/home/zane/usagi-runtime/strategies/v06.live.json`
- 旧策略配置：`/home/zane/usagi-runtime/strategies/legacy.live.json`
- 新权重：`/home/zane/usagi-models/v06-b15-mh4-s43-sse-20260910/v06.bin`
- 生产程序：`/home/zane/usagi-bin/t0_sse_journal_predict`
- 升级前备份：`/home/zane/usagi-runtime/v06-backup-20260910/`
- 验证证据：`/home/zane/usagi-runtime/v06-validation/`

默认无 model_version 的旧配置仍使用 legacy。v0.6 的模型选择和策略选择由启动器一起生成；不允许把四头模型信号交给旧策略。旧模型文件及 snapshot/Auction59 实现未删除。

## 模型和因子

原生 C++ FP32：LayerNorm(50, eps=1e-5) → Linear(50,128) → 两层 GRU(128) → Linear(128,4)。GRU 使用 PyTorch r/z/n 顺序，new gate 的 recurrent bias 在 reset gate 内。支持 AVX2，运行时检测后选择，保留标量回退；生产推理不依赖 Python、PyTorch 或新增线程池。

输入顺序严格采用包内 factors.txt，不加外部 scaler；7 个流量因子按契约压缩。四头顺序是 15/30/60/120 秒，单位 permille，15 秒头为定价信号，不翻转、不缩放、不裁剪输出。

原始重放核对发现并在 v0.6 分支修正了成交额/成交量单位、10% 盘口距离边界、并列最大量档位选择；旧因子行为保留。开盘和午休后使用当前完整盘口初始化因子窗口；午休不清空模型 hidden state。每票每日独立状态，09:30 起持续前向、记录四头预测。

继续使用实盘现有 hardware-gap-batch 采样规则。研究 300us 只用于解释黄金检查点，没有接入人为等待；研究 240ms/1s 执行延迟也没有接成实盘等待。

## 策略

新增独立 `sse/runtime/v06_strategy.*`，旧 `ZStrategy` 路径保留。

- 四头严格有限同号才支持对应方向扩仓；零、NaN、Inf、分歧均不给扩仓许可，主预测非有限时不生成新的定价意图。
- 09:33 前禁止扩仓，但满足正常库存、预算及成交条件的已有偏离可减小；模型持续更新。
- 按静态底仓计算相对仓位，normal quantity 经过库存、T+1、日预算、position limit 约束后，再裁掉无许可的跨零部分。
- reservation 从 OMS 的 working_buy/working_sell 读取，包含尚未确认的取消；取消请求不释放预约量。部分成交先体现到真实持仓和剩余量，再进行下次决策。对失去许可的未完成开仓请求取消。
- Hit 与 Quote 均采用四头门控；继续单票 single-flight，但先执行取消检查，避免被旧 single-flight 提前 return 挡住。
- Hit 超时为 1 秒；Quote 使用 10 倍 offset，价格过时或扩仓许可丢失时申请取消。
- offset 使用已有 daily/global 配置值，不擅自把交接示例 1.0 当成新的最优值；应用交接表的分钟倍率。bias_factor=0.3、position_base_line=500000、position_limit_factor=1.0，并应用 DynamicNewBias 时间倍率。固定 skew=1bps。
- 每 10 秒按固定股票顺序同步组合 exposure；只惩罚同向扩张一侧，5% 是惩罚饱和刻度，最大 10bps，不作为硬仓位 cap。
- 静态底仓暂沿用现有 daily 的 static_position；开盘价取官方行情快照，若另行提供 Open 可从配置读取。支持 tradable/frozen 标记；冻结股票仍计入组合基准。
- 缺少必要开盘价、成交金额覆盖不全，或重启后发现无法按价格追溯的已有日内成交时，不放行扩仓，不以猜测价格构造 exposure。

记录 `v06_prediction`、`v06_decision`、`v06_global_skew`、`v06_cancel_request`、`v06_order_terminal`，包括四头值、agreement、相对持仓、reservation、requested/allowed、取消请求与终态。取消终态以 OMS 为准，不使用缺少 epoch 的旧 LF 回报维护第二套账。

生产审计文件为 `/home/zane/usagi-runtime/YYYYMMDD/v06/audit-<epoch>.jsonl`，由启动器设置 `SSE_V06_AUDIT_PATH`。原生进程缓冲写入，并在状态输出和结束时 flush，避免逐预测依赖 journald 限流。审计写入错误使运行明确失败，不静默丢记录；该文件不是逐行 fsync 的掉电耐久日志，原始行情仍以 capture journal 为准。

## 验证

| 验证 | 结果 |
|---|---|
| 原包 SHA256SUMS | 110 文件匹配 |
| 原包 tests/validate_bundle.py | 通过；4,017 样本、50 因子、4 输出，AppSeq/采样金额条件/CPU分块与逐条/TorchScript/策略例子/校验和全部通过 |
| 原始 order/trade → 原生盘口与因子，按黄金检查点 | 4,017 × 50 全部在约定容差内，原始事件拒绝数 0 |
| 上述实际因子 → 原生模型 → 黄金四头 | 最大预测误差约 1.1e-6 permille，agreement 差异 0，最终 hidden 在容差内 |
| 原生单行推理 | 本机约 107 微秒/行（含测试逐层比较）；不是全市场吞吐或实盘延迟保证 |
| v06_strategy_test | 四头、跨零、取消未确认、部分成交、开盘限制、冻结/开盘价保护通过 |
| 启动器测试 | 10 项通过，含不依赖旧 snapshot 权重的 v0.6 选择与未知版本拒绝 |
| 旧策略/逐笔处理/journal 集成 | 回归通过 |
| 回环 UDP → journal → v0.6 | 通过，09:30–09:33 warm-up 预测实际产出并记录 |
| 生产二进制 + 2,317 股票配置 + 真实 capture，无 TD | 通过：v0.6、live、processing_valid=true、producer_alive=true |
| 生产 capture + 预测 + TD | capture 正常；20:23 左右 ATP 登录超时，未通过 TD ready 验收，已停止重试 |

黄金容差为 atol=2e-5、rtol=2e-5。原生模型比较使用更严格的最大绝对误差 <2e-5；研究 BF16 与部署 FP32 不要求逐位一致，不增加 epsilon 改变同向判定。

可复查工具：`tools/sse/export_v06.py`、`tools/sse/export_v06_replay.py`、`tests/sse/v06_model_golden_test.cpp`、`tests/sse/v06_factor_replay.cpp`、`tests/sse/test_v06_journal.py`、`tests/sse/v06_strategy_test.cpp`。

## 明日状态和操作

00:05 原日配同步、08:50 preflight、08:55 capture、08:57 预测、08:58 check-startup 继续使用既有计划。启动检查已兼容 v0.6 单模型路径，并检查运行中 model_version 与所选版本一致。

20:20 左右核对时，研究端和实盘端仍未发现 `config_sse_daily_20260911.json`；目前不能把明日日配或实际开盘启动标记为已验证。明早需日配到位、TD 登录/对账成功、运行版本 v0.6、双通道收包和数据连续性健康。global skew 的开盘价覆盖另由运行日志给出，缺失时阻止扩仓。

真正使用不同实盘采样序列时，GRU 状态轨迹可能与研究黄金序列不同，不能要求逐点预测相同。09:25 突发容量、整日预测产出和丢包情况仍应查看上一任务新增的资源监控。

切回旧策略时，停止预测，使用 `--live-config /home/zane/usagi-runtime/strategies/legacy.live.json`，或将该配置原子替换为 active 配置后启动；不删除 v0.6。切换配置不会修复已出现的数据缺口，也不应在有在途订单时直接切换策略。升级前完整程序及源码可从备份恢复。

本次未开启真实报单，未改变原策略的报单授权开关。源码改动以工作区差异保留，部署记录和测试结果另行保存。

盘后测试进程已停止，今日 active_capture/current 已恢复到 `/home/zane/usagi-runtime/20260910/capture-085501-ZI6zPk`。最后一轮无 TD 的生产路径检查进入 live 并消费 388 个事件；原始盘中 journal 未改写。v0.6 配置保留供明早定时启动。
