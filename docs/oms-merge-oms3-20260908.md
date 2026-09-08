# OMS 合并项：OMS-3 落地记录（2026-09-08）

承接 OMS-1/2（见 docs/oms-merge-oms1-2-20260908.md）。OMS-3 目标：
query/资金/持仓/恢复认证（AtpBackend.query not-certified）与"重启先撤遗留单"。
用户确认：代码层现在就能做，不必等账户；真实账户只在最终联测用。

## OMS-3a：重启先撤遗留单
- `common/oms/Types.h`：`Config.restart_cancel_open_orders`（默认 false）。
- `common/oms/Oms.cpp`：
  - `install_snapshot` 末尾：开启开关时，把恢复出的 owned working 单逐个
    置 `cancel_requested`（落 CancelIntent 审计）并入队撤单。
  - `ready()` 新增 `restart_open_working()` 门控：只要有"恢复出的、已请求撤、
    未终态、仍有 working"的单，账户就 **not ready**，新下单被拒；撤回报终态后
    自动放行。
- 测试：`test_restart_cancel_open_orders`（恢复快照含 working 单 → not ready →
  新单被拒 → broker 回报 Canceled → ready 恢复、无 working）。

## OMS-3b：broker query → 认证快照
- `adapters/td/atp/OmsAtpBackend.h`：
  - 新增 `QuerySender` 构造参数（默认空）+ `certified_snapshot` 参数：
    只有提供 query sender 时才声明 `capabilities.complete_snapshot = true`；
  - `query()`：有 query sender 时转发（返回其 Error，None 表示已发起、快照稍后
    经 publish_snapshot 回流）；否则维持 Unsupported 语义；
  - 新增 `publish_snapshot()`：broker 侧聚合出的认证快照经 OMS sink
    （Engine::create 已把 Snapshot sink 绑到 complete_snapshot）完成对账。
- 测试：
  - `test_atp_backend_certified_query`：capabilities/转发/快照 sink 到达；
  - `test_engine_query_then_snapshot_reconcile`：`begin_reconcile(2, true)`
    发起 query → 账户等待 → publish 认证快照 → account ready。
- 说明：ATP 插件内"资金/持仓/订单/成交分页聚合 → Snapshot"的实盘映射仍需在
  有真实 SDK/账户时联测（OMS-4），传输层与引擎生命周期已在此认证并单测。

## 验证
- 全量重建 ctest：**44/44 通过**；OMS 单测 `oms_merge_test: ok`（9 项含
  OMS-3a/3b）；TD 插件（OMS 版引擎 + 新 AtpBackend 头）重编通过。
