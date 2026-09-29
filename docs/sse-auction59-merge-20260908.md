# Auction59 功能包合并记录（2026-09-08）

将实盘 `/home/zane/sse` 的「集合竞价开盘」功能作为一个包并入 sse-dev 分支。
拍板规则（用户）：开盘边界 + auction59 引擎合并为一个功能包；健康检查类
（readiness/startup_health/opening 审计 python）本轮不做，留到全部合并完成后再议。

## 落位与内容

新增目录 `sse/auction/`（正式代码，默认构建）：

| 文件 | 作用 | 来源实盘文件 |
| --- | --- | --- |
| `sse_opening_boundary.{h,cpp}` | 逐股 09:25-09:30 开盘状态(S)质量判定 | `src/market_data/sse_opening_boundary.*` |
| `auction_static_metadata.{h,cpp}` | 每日拍卖静态元数据装载 + 快照 pre_close 对账 | `src/model/auction_static_metadata.*` |
| `auction59_engine.{h,cpp}` | AuctionAccumulator 重建开盘撮合 → 62 canonical / 59 因子 | `src/model/auction59_engine.*` |
| `sse_auction59_live.cpp` | 独立 replay/--follow runner（读 channel_*/records.bin） | `src/strategy/sse_auction59_live.cpp` |

测试（`tests/sse/`，注册进 ctest，默认构建运行）：
`auction59_engine_test`（含逐位浮点校验）、`auction_static_metadata_test`、
`sse_opening_boundary_test`。三测全过；全量默认构建 41 项中 40 项通过，
唯一失败 `sse_journal_integration_test` 属另一条在途 journal 工作线，与本次无关。

追加（同批后续拍板）：`audit_snapshot_static_metadata` 生产门禁工具迁入
`sse/auction/`（依赖本包的 CSV loader + reconcile_snapshot_pre_close），CMake 默认
构建同名可执行。用法：
`audit_snapshot_static_metadata SNAPSHOT_ROOT STATIC_CSV YYYYMMDD OUTPUT_JSON`；
出现 pre_close 冲突/无效快照记录返回 1（Auction59 保持禁用）。

未迁移（用户拍板：后续重建该流程）：`sse_extract_security`（单标的切片）、
`events_csv_to_tick_bin.py`（scanner-CSV 回放调试链）暂不搬。

## 简化决策

1. 只保留 **CSV loader**（`sse_static_YYYYMMDD.csv`，由 prepare_sse_static_metadata.sh
   生成，是生产实际输入）。JSON daily-config loader 移除：生产未用且其
   `limits_valid` 推导自相矛盾（`auction_static_ready()` 依赖的
   `limits_valid` 永远先为 false）。
2. `sse/auction` 库自包含：静态元数据内用镜像的 `is_sse_stock`，不依赖
   decoder.cpp 符号；只有 runner 需要编译 `sse_primary_decoder.cpp`（其本就要
   调用 decode/local_trading_date）。避免将来与 `sse_stream_processing` 重复定义。
3. `auction59_sidecar`（sse/factors）与实盘逐字节一致，未动。

## 主进程接入（2026-09-09）

`t0_sse_stream` 和 `t0_sse_journal_predict` 共用的 processor 已接入
`auction59_session`。同一份解码逐笔同时用于盘口和竞价计算，收到开盘结束状态
S 及有效快照后，将 59 个因子直接交给 Snapshot 模型，不再读取 Auction59 CSV。
旧 `snapshot_auction59_factors_path` 不再使用，配置可以删去该字段。

程序可以在 09:15 前启动。09:25 或 09:30 的快照用于核对昨收和开盘价；
09:25 至 09:30 之前还核对竞价成交量，09:30 起累计量可能包含连续交易，不做此比较。
快照先到或 S 先到均可。59 个因子全部有效后冻结，不用零值补缺。

静态价格直接复用 daily：`Close`、`HpUpperPrice`、`HpLowerPrice`。
快照没有涨跌停价或上市日期；可选 `listing_date` / `is_ipo_first_day` 由 daily
提供。缺少首日信息且触发既有首日价格范围特例时，该股票跳过 Auction59。
普通股票无需为此另造静态 CSV。

首次 09:30 后快照到达仍未就绪的股票，永久跳过当天 Snapshot，逐笔继续预热，
09:35 后才向策略提供信号；没有快照时在 09:35 首笔逐笔确定降级。
单股失败不拖住整个股票池。昨收冲突等公共静态错误同时禁止该股逐笔信号；
策略在批末真正派发前重新检查，已缓存的旧预测也不能绕过这一限制。
日志记录 `sse_auction59 code=... mode=tick_only reason=...` 或
`mode=snapshot_and_tick reason=ready`；公共错误记录 `sse_prediction_gate ... mode=blocked`。

默认启用在线竞价计算。处理 profile 中可显式设置
`prediction.auction59.enabled=false`，整个运行只用逐笔；不设置则默认 true。
旧版 profile 如带 `enabled:false`，需删除或改 true 才会运行 Snapshot。

采集必须订阅逐笔和快照，并保留 09:15 起完整历史及顺序。
金桥现有源为逐笔 `239.35.80.9:37109`、快照 `239.35.80.5:37105`，同一网卡
`11.11.11.11`。`journal_capture_hardware.example.json` 已包含两条通道。
09-08 的运行配置只有逐笔。09-09 已部署双频道 journal capture，实际逐笔与快照
收包、硬件时间戳和落盘已在盘中确认；操作入口见
[上海 journal 采集](operations/sse-journal-capture-live.md)。

构建与回归：

```sh
cmake -S . -B build/sse-dev-auction-wire -DCMAKE_BUILD_TYPE=Release -DT0_BUILD_SZE_STREAM_PROCESSOR=OFF
cmake --build build/sse-dev-auction-wire -j 6
ctest --test-dir build/sse-dev-auction-wire --output-on-failure
```

新增 session/processor 测试覆盖原始竞价事件至模型预测、两种到达顺序、单位错误、
缺少 S、按股降级、09:35 切换和公共静态冲突；CLI 测试覆盖无 CSV 的盘前启动。
真实行情完整性及实际有效因子覆盖率须在相同入口消费当天行情后确认。

## 独立 CSV 工具（离线使用，不是主程序前置步骤）

```sh
cmake -S . -B build/sse-dev-auction -DCMAKE_BUILD_TYPE=Release
cmake --build build/sse-dev-auction -j 8
ctest --test-dir build/sse-dev-auction -R 'auction59_engine_test|auction_static_metadata_test|sse_opening_boundary_test'
# runner（replay 示例）
./build/sse-dev-auction/sse_auction59_live --tick-root <parsed_tick_root> \
  --static-csv <sse_static_YYYYMMDD.csv> --date YYYYMMDD \
  --output-csv out/auction59.csv --quality-csv out/auction59_quality.csv \
  --metrics-json out/auction59_metrics.json [--workers N] [--follow] [--stop-time HH:MM:SS]
```
