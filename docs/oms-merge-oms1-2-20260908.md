# OMS 合并项：OMS-1 / OMS-2 落地记录（2026-09-08）

用户拍板（OMS 语义评审）：
- 3.1 成交/回报幂等 → 补在 **OMS Engine**（多数已在 `apply_report`：trade_id 冲突冻结、
  cumulative 回退按 stale 忽略、终端后迟到执行冻结）。本轮补齐回归测试固化。
- 3.2 single-flight（每标的一笔在飞）→ **策略层**：SSE Session 在信号进入策略内核前，
  向 OMS 查询该标的是否仍有 working 单；有则抑制新信号（单笔在飞）。
- 3.3 重启先撤全部遗留单 → 归 OMS-3 启动/恢复流，未在本轮实现（已记录）。
- 3.4 风控/限价映射：沿用 OMS InstrumentRules/lot/band/notional，无新代码。
- 3.5 OMS 只做限价单开/平：OMS 通道 submit 固定限价（引擎侧已是 kFixedNew）。

## 代码改动（OMS-1）
- `common/oms/Oms.h/.cpp`：新增只读原语 `Engine::has_working_order(Instrument)`；
  `apply_report` 幂等语义已存在，未改动行为。
- 新增回归测试 `tests/oms/oms_merge_test.cpp`（手工编译运行，`oms_merge_test: ok`）：
  重复同值 Accepted/Filled/trade 幂等、终端后 CancelResult 忽略、现金只结算一次、
  `has_working_order` 状态迁移、AtpBackend 闭环 echo 单次投递 + 断连通知。
  （ctest 注册待 tests/CMakeLists 由并行会话落定后再补。）
- `common/contracts/StrategyExecution.h`：接口加默认 false 的
  `has_working_order(instrument)`（legacy/unmanaged 执行器不受影响）。

## 代码改动（OMS-2）
- `OmsStrategyExecution`：`has_working_order` 按 SSE/SZE 两市场查询 engine。
- `ProtectedExecution`（StreamStrategyExecution）：同一标的在 universe 内则透传。
- `sse/runtime/sse_strategy_session.{h,cpp}`：SSE Session 默认开启 single-flight
  （processing config 可 `"sse_single_flight": false` 关闭）；process_output 在
  `core_->on_signal` 前查询 working 单并抑制。

## 验证
- 全量重建 + ctest：**43/43 通过**（含 oms_test/oms_recovery_test/atp_boundary/
  sse_strategy_session_test 等；另一会话 journal 集成测试同时转绿）。
- 手工 `oms_merge_test` 通过。

## 未做（记录）
- OMS-3：AtpBackend.query/资金/持仓/恢复认证；重启撤遗留单（3.3）在此项落地。
- oms_merge_test 的 ctest 注册（等 tests/CMakeLists 稳定）。
