# 运维总览

本页只描述当前可操作边界，不替代详细契约，也不重复执行日志。构建和验证应在 `build/<variant>` 中进行，避免覆盖外部部署产物。

## 构建与离线运行

从源码根目录构建两市可移植运行时，不链接券商 SDK：

```bash
cmake3 -S . -B build/dev -DUSAGI_BUILD_DEEPWIN=OFF
cmake3 --build build/dev -- -j4
(cd build/dev && ctest3 --output-on-failure)
```

只构建一个市场时，显式设置 `T0_BUILD_SZE_STREAM_PROCESSOR` 和 `T0_BUILD_SSE_STREAM_PROCESSOR` 的 ON/OFF 组合。旧 Deepwin 插件另用 `USAGI_BUILD_DEEPWIN=ON` 并传入 `-DRUNTIME=/validated/runtime/root`；TD 插件通过 `SZE_BUILD_TD`、`SSE_BUILD_TD` 独立选择，`SZE_TD_API_DIR` 指向含 `include/`、`lib/` 的 SDK 根目录。交付旧 ABI 产物前使用 `tools/build/check_centos79_build_env.sh` 检查 CentOS 7.9/GCC 4.8.5 环境。

保留的入口名称为 `t0_sze_stream`、`t0_sse_stream`；运行时库保留 `libt0_sze_runtime.so` 等既有 ABI 名称。常用流程是：准备统一配置，生成处理 profile，再执行 capture/replay。命令参数和录制完整性要求见[行情流契约](contracts/market-data-stream.md)，策略意图模式见[策略流运行时](contracts/strategy-execution.md)。

深市恢复入口支持 journal 与 handoff，但恢复就绪不等于账户或交易就绪。handoff 的 same-boot 时钟、代次、source/day、连续事件和生产者存活检查必须全部满足；journal 仅用于分析时不得获得新单资格。

## 运行安全边界

- `execution` 必须显式禁用；Paper 快照不是真实资金或订单账本。
- 录制失败、丢包、队列溢出和解析失败按不同健康级别处理；新风险门禁不能只看 `ready`。
- 停止顺序先关闭新决策，再排空已接收输入；不要把 EOF 当成额外样本。
- 旧 SSE `get_obj` 继续拒绝，旧 main 尚未切换到新生命周期入口。

## 未完成验收

当前新 runtime 是 paper-only，未接入真实 TD。真实账户对账、在途订单/成交回报终态、重连接管、生产模型全天运行、SDK 验收和实际网卡峰值/尾延迟验证仍待完成。合成模型、loopback、离线回放和已有测试不能替代这些验收。

源码已迁移至 `/home/usagi`，生产安装路径没有更改。旧插件在本地的构建、符号和依赖检查不替代券商联调或生产切换。详细生命周期约束见[市场运行入口 API](contracts/runtime-api.md)，配置锁和绑定见[统一配置工具](contracts/unified-config.md)。

`deploy/sse/build_live_package.sh` 只打包旧 JSON 观察器，不是公共行情流的完整录制入口；它接受 `USAGI_BUILD_DIR` 和 `USAGI_PACKAGE_DIR`，默认 `build/dev`、`build/packages`。实盘与离线共用的完整录制使用 `t0_md_stream` 或市场 stream 入口。旧 SSE daily 脚本需要显式提供 `SSE_STATIC_METADATA_VALIDATOR`；该外部校验器未提供时，在生成输出前失败。旧宿主 runtime 包要求显式指定策略、TD、main、模型包和依赖包，不能自动选用历史日期的构建产物。
