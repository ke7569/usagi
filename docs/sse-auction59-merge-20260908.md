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

## 简化决策

1. 只保留 **CSV loader**（`sse_static_YYYYMMDD.csv`，由 prepare_sse_static_metadata.sh
   生成，是生产实际输入）。JSON daily-config loader 移除：生产未用且其
   `limits_valid` 推导自相矛盾（`auction_static_ready()` 依赖的
   `limits_valid` 永远先为 false）。
2. `sse/auction` 库自包含：静态元数据内用镜像的 `is_sse_stock`，不依赖
   decoder.cpp 符号；只有 runner 需要编译 `sse_primary_decoder.cpp`（其本就要
   调用 decode/local_trading_date）。避免将来与 `sse_stream_processing` 重复定义。
3. `auction59_sidecar`（sse/factors）与实盘逐字节一致，未动。

## 主进程接入（预留、未接线）

按用户要求：当前不接入 `t0_sse_stream`/StreamProcessingCli，但保留 config 开关位。
在 SSE 处理 profile 的 `prediction` 下预留可选对象：

```json
"prediction": {
  "...": "...",
  "auction59": { "enabled": false }
}
```

当前契约只接受 `enabled:false`；置 `true` 会抛
“unsupported processing contract value”（`apps/StreamProcessingCli.h` 中
`auction59_contract`）。将来接线时：把 auction59 runner/引擎改为库内服务，
在 09:15-09:30 产出因子并喂给 snapshot auction59 模型路径，再放开该开关。

## 运行

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
