# 架构总览

Usagi 的目标是让公共处理链和沪深差异可见、可独立构建。迁移保留算法、模型算术、插件/日志格式和导出 ABI；目录职责变化不代表所有旧实现已经完成抽取。

## 目录职责

```text
common/{contracts,stream,strategy,execution,oms,factors,market_data,model}
sze/{market_data,sampling,factors,model,runtime}
sse/{market_data,sampling,factors,model,runtime}
apps/
adapters/deepwin/        adapters/td/atp/
config/examples/{sze,sse}
tools/{config,model,build,sze,sse}
tests/{common,oms,sze,sse,integration}
deploy/{sze,sse}
```

`common/` 承载共享契约、流处理、策略/执行接口及可移植模型引擎，不反向依赖某个市场。模型按架构归类：`common/model/{mix153060,legacy_midmix,snapshot_gru}`；这些目录是推理引擎，不是权重仓库，权重、特征契约和可变股票状态不能因目录相同而互换。`sse/model` 保留 `hybrid`、`ensemble` 等市场路由，`sze/model` 放模型权重 manifest。

两市都拥有 `market_data/sampling/factors/model/runtime` 位置，但 sampling/runtime 仍有耦合；结构迁移不等于所有因子或采样公式都已抽取。`apps/` 负责 CLI/Runtime 装配，`adapters/deepwin` 放 Deepwin 插件，`adapters/td/atp` 保留 ATP 适配。

## 边界与入口

公共运行入口由 `apps/` 装配；其生命周期和错误语义见[市场运行入口 API](contracts/runtime-api.md)。行情接收、队列、录制、回放和健康状态见[行情流契约](contracts/market-data-stream.md)；策略会话、Paper 执行保护和市场差异见[策略流运行时](contracts/strategy-execution.md)。

CLI 名称和库 ABI 保持兼容：`t0_sze_stream`、`t0_sse_stream` 以及 `libt0_sze_runtime.so` 等旧输出名不改。构建输出按 `build/<variant>` 隔离。`USAGI_BUILD_DEEPWIN` 默认关闭；需要 SDK/Deepwin 时显式开启对应构建选项。

## 状态声明

沪深共用进程内账户 OMS，负责订单、持仓占用、风控、撤单、审计与恢复；策略只读取其状态，TD 保留协议、连接、发送和查询职责。见[OMS 契约与支持范围](contracts/oms.md)。同一真实资金账户必须只有一个 OMS 所有者，不允许两个市场进程各持一份完整预算。

新运行时仍是 paper-only；ATP 已有 OMS 发送/回报适配并关闭旧旁路，但完整账户查询和连接恢复尚未认证，不能开新单。旧 SSE `get_obj` 继续拒绝。SDK 编译不等于实盘准入，尚未切换宿主、部署或连接生产账户。
