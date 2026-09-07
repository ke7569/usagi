# sse-dev 合并说明（2026-09-07 至 09-08）

本分支以 `origin/main` 的 `b21742a` 为主线，合并此前上海分支的
`a06531a`（包含 `7d0e321` 及本次内存流水线、L3 隔离、模型压测工作）。
共同祖先为 `bdd7a5f`。合并保留两边 Git 历史，并按新目录和运行语义逐项迁入。
旧完整工作树保留在 `/home/zane/usagi-experiment`；新工作树为
`/home/zane/usagi-sse-dev`，分支名 `sse-dev`。

## 差异与取舍

| 项目 | 旧上海分支 | main / 本次合并后的正式入口 |
| --- | --- | --- |
| 目录 | `sse-t0`、`src/t0-main`、`modules/deepwin_guoxin` | 保留 `common`、`sse`、`sze`、`apps`、`adapters` 的职责划分 |
| 行情传递 | 多接收线程、标准化 Event、每 shard 内存队列 | 保留 `MarketDataStream`：一个接收线程处理所有订阅，内存队列交给串行计算回调，同时异步录制 |
| CPU | 按 L3 分配接收线程及实验 workers | 上海正式 capture 的 receive / dispatch / writer 各占不同 L3；observer 的 receive / dispatch 各占不同 L3 |
| 股票分片 | ChannelNo 1–6 内按股票数量均分、固定归属 | 分配器迁入 `sse/runtime`；完整多 shard 计算保留在默认关闭的实验中，尚未接入正式 processor |
| 存储 | 实验 `.szej` 保存 504 字节标准化 Event | 正式路径保留 `.t0md` 原始 UDP、顺序与 idle 事件；录制独立于计算，计算无需等待落盘 |
| 采样/数量单位 | 旧 gate、旧 book 单位及 prediction consumer | 保留 main 的 `sse-per-instrument-v2`、严格大于 100us 的逐股静默截止和原有单位转换 |
| 模型 | 旧独立 consumer 和定长内核优化 | 正式路径保留 main hybrid model；旧优化和压测保存在实验目标中 |
| OMS / TD | 旧 SSE get_obj、直接交易和 SDK 初始化分支 | 保留 main 的统一 OMS、异步审计及 paper-only 边界；不恢复旧交易旁路 |
| FPGA 2.0 地址 | 华润 `238.127.1.1:12020`、卡园 `238.125.1.1:12002` | 迁入新目录下的上海配置样例和启动脚本，另提供符合正式 StreamInputConfig 的样例 |

旧 `.szej` 和新 `.t0md` 不可直接互读。本次未将旧采样、EOF 行为或数量转换
覆盖到 main；它们不只是文件重命名。旧实验没有丢弃，但也不能用其测试结果
证明新版正式入口已实现分片或满足低延迟要求。

## 上海 CPU 行为

`t0_sse_stream capture` 使用既有输入配置中的 `receive_cpu`、`dispatch_cpu`、
`writer_cpu`。`-1` 自动选择，显式 CPU 优先保留。读取 Linux sysfs 的实际 L3
共享域，遵守进程允许的 CPU 集合；同一 L3 的普通核和 SMT 线程均视为冲突。
缺少三个可用 L3 域、显式指定同域或绑定失败会导致启动失败。
最终状态中的 `cpu_affinity` 列出角色、CPU 和 L3 域。

租约保持到线程结束，与同用户的其他 usagi SSE 进程协作排他，并避开检测到的
旧 SSE 线程占用域。这不是操作系统 CPU 独占机制，不能阻止不参与租约的普通
进程调度到这些核上。辅助生命周期线程和 OMS 日志线程不在本次三线程分配范围。
replay 不申请这组三核租约，深圳运行逻辑和配置不变。

`sse_udp_observer` 仍是输出前缀 JSONL 的诊断工具；不是完整行情录制入口。
其 `--cpu-list RECEIVE,DISPATCH` 对应两个线程，不再按订阅数量列 CPU。
正式录制使用 `t0_sse_stream capture`，参见
`config/examples/sse/stream_capture_fpga20.example.json`；部署前填入网卡 IP、
选取所用站点通道和新的录制目录，并提供匹配日期的处理配置。

## 构建与验证

正式目标默认构建；历史实验仅在 `USAGI_BUILD_SSE_EXPERIMENTS=ON` 时加入，
目标与测试统一使用 `sse_experimental_` 前缀。实验运行方式见
`sse/experimental/memory_pipeline/README.md`。

```sh
cmake -S . -B build/sse-dev-main -DCMAKE_BUILD_TYPE=Release
cmake --build build/sse-dev-main -j 8
ctest --test-dir build/sse-dev-main --output-on-failure

cmake -S . -B build/sse-dev-experiments -DUSAGI_BUILD_SSE_EXPERIMENTS=ON
cmake --build build/sse-dev-experiments -j 8
ctest --test-dir build/sse-dev-experiments -R sse_experimental_ --output-on-failure
```

正式默认构建在服务器 GCC 4.8.5 上通过，34 项已注册测试通过，包括实际线程
CPU/L3 绑定、同域配置拒绝、停止后租约复用，以及 observer 双订阅回环收包。
observer 测试在 socket 实际绑定后发包，避免将拓扑检查的启动耗时误判为丢包。
服务器没有 Python 3，原 main 中需要 Python 3 的集成测试未在服务器执行；
上述两项新增集成验证支持 Python 2.7。

正式沪深目标与实验目标同时开启的构建通过。17 项实验测试全部通过，包括真实 tick 权重与 20260904 静态配置
下的 consumer 一致性、live/replay、并发队列及 CPU 租约测试。并发队列测试的
独立目标补齐了旧目标传递提供的 `pthread` 链接依赖。
真实权重测试通过 `SSE_EXPERIMENT_TEST_TICK_MODEL`、
`SSE_EXPERIMENT_TEST_STATIC_JSON`、`SSE_EXPERIMENT_TEST_TRADING_DAY`
三个 CMake 参数注册；未配置时不会注册该项。
合并后的完整源码在本地 Python 3.11 下通过 5 项源布局检查。
没有启动生产行情、连接交易账户或部署二进制。
