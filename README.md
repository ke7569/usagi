# Usagi

Usagi 是面向沪深两市的统一行情、策略与运行时源码。`common/` 放公共实现，`sze/` 与 `sse/` 以平级、对称的目录承载市场差异。两市共用录制回放、策略和执行基础，模型引擎按实际结构归类；尚未证明等价的采样、因子和路由保留独立实现。

## 从这里开始

- [架构总览](docs/architecture.md)：目录职责、公共/市场边界和构建产物。
- [配置总览](docs/configuration.md)：统一配置、旧格式、profile 与模型锁。
- [运维总览](docs/operations.md)：构建、离线 capture/replay、恢复和当前限制。

详细契约：

- [行情流与录制格式](docs/contracts/market-data-stream.md)
- [统一配置工具](docs/contracts/unified-config.md)
- [策略流运行时](docs/contracts/strategy-execution.md)
- [市场运行入口 API](docs/contracts/runtime-api.md)

## 当前边界

新 runtime 目前是 paper-only 的行情/策略处理入口，不是真实 TD 交易入口；不会因为离线成功而获得实盘资格。旧 SSE `get_obj` 仍明确拒绝，未切换旧宿主。生产模型全天运行、真实账户/TD 回报协议和 SDK 验收尚未完成。

本地源码目录为 `/home/usagi`。构建保留既有 CLI 与 ABI 名称，包括 `t0_sze_stream`、`t0_sse_stream`、`libt0_sze_runtime.so` 等；输出使用 `build/<variant>`。迁移范围与验收见 [OpenSpec 迁移记录](openspec/changes/migrate-usagi-layout/design.md#verification)。

## 约束

- 不提交密码、账户凭据、行情 journal、CSV 输出或私钥。
- 部署时注入 TD 凭据；替换二进制前检查 source-id、journal 容量、ABI、模型哈希和配置哈希。
- 不把 Paper 配置快照当作真实账户对账，也不把合成模型/loopback 结果当作生产收益或全天验收。
