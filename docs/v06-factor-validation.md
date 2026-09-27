# V06 因子与采样核对

`StaticInputs::v06_baseline` 默认为 `false`，保留原 A3 采样与因子行为。
配置启用 V06 后，使用更新包 `source/feature_generation` 中 commit
`9a42177be7f01289d9fc257d38e1611483df9de7` 的 baseline 契约。
订单年龄继续使用微秒精度增量维护，`tanh` 使用标准精确实现。

## 与旧模式的差异

- 连续交易采样时段为 `(09:30, 11:30]`、`[13:00, 14:56)`；旧模式的下午截止为 14:57。
- 非连续交易时段清除采样窗口，下一连续时段的首个 cut 建立新窗口。
- V06 不使用旧模式的“每毫秒最多接受一个采样点”门槛；只要求 cut 严格推进、当前 exchange time 与窗口起点不同。
- amount、100 秒 time、mid 变化且成交增加至少 100 股的 change 三种触发取 OR。
- 单边/锁涨跌停簿的 spread 清零、market flow 的对侧价格有效性检查、订单流 fallback、距离 band 的边界与空侧处理按新源码执行。
- V06 要求 `free_share` 为有限正数。因子顺序仍严格采用包内 `factors/factors.txt` 的 50 个 Float32 输入，不使用 label 字段或外部 scaler。

## 原始行情回放结果

输入为更新包 `raw/20260401/stocks/000807.SZ/` 中的全部委托与成交，
共 306,725 条，按 AppSeq 合并；保留盘前订单簿构建。Level2 不参与触发。

| 核对项 | 结果 |
|---|---:|
| Native 实时语义产生的采样 | 12,927 |
| 包内可比较的 golden 采样 | 12,852 |
| golden 前缀的行键差异 | 0 |
| golden 前缀的触发原因差异 | 0 |
| 642,600 个因子值超容差数 | 0 |
| 因子最大绝对误差 | 约 9.09e-13 |
| 研究导出未包含的尾部实时采样 | 75 |

逐行键包含 exchange time、AppSeq、cut_index，以及窗口起点的三个对应字段。
因子容差为 `2e-6 + 2e-6 * abs(expected)`，独立于模型 FP32 推理容差。

## 为什么 Native 比 golden 多 75 行

这是更新包研究导出的未来标签等待行为，不是多触发了不合法采样。

更新包源码证据：

1. `source/feature_generation/apps/hermespro-features/src/replay/mod.rs` 的
   `maybe_emit_sample` 将已生成的因子行放入 `state.pending`，设定
   `label_due_active_micros = active_trading_time_micros(cut.ex_time_micros) + LABEL_60S_HORIZON_ACTIVE_MICROS`。
2. 同目录 `clock.rs` 定义 `LABEL_60S_HORIZON_ACTIVE_MICROS = 60 * 1_000_000 - 10`，
   并将 14:56 以后的 active time 封顶。
3. 同目录 `labels.rs::fill_due_pending` 只有在未来 cut 的 active time 达到 label due 时，
   才把已生成的样本从 pending 搬到导出 `rows`。
4. `mod.rs::run_timeline` 返回 `state.rows`，没有在结束时导出尚未成熟的 pending 样本。

因此 `14:55:00.000010` 之后产生的样本无法成熟 60 秒标签，未进入包内 `features.arrow`。
本次 golden 最后一行为 AppSeq `42550578`、`14:55:00.000000`；
Native 随后的 75 行从 AppSeq `42562266`、`14:55:01.340000` 开始，
至 AppSeq `42827729`、`14:55:57.500000` 结束。

在线 Runtime 保留这 75 个因子样本并允许更新模型 hidden state；不能使用未来标签可用性控制在线采样。
测试严格要求额外行只出现在上述标签截止之后且在 14:56 之前，
cut/AppSeq 单调、窗口接续、触发标记有效、因子有限。
这 75 行没有包内因子 golden，因此不宣称其数值经过 golden 对照。
策略是否允许报单由交易时间规则独立决定。

## 复现

```bash
python3 tools/model/export_v06_factor_fixture.py \
  --bundle-root /path/to/v06-extracted --output /tmp/v06-factors.bin
g++ -std=c++11 -O2 -I. tests/sze/v06_factor_golden_test.cpp \
  sze/sampling/mix153060_runtime.cpp -o /tmp/v06-factor-test
/tmp/v06-factor-test
/tmp/v06-factor-test /tmp/v06-factors.bin /tmp/v06-factor-actual.csv
```

导出工具只需要 PyArrow，兼容服务器的 Python 3.6 / PyArrow 6。
不带 fixture 的测试覆盖默认旧模式、V06 同毫秒重复采样、14:56 边界与 free_share 验证。
