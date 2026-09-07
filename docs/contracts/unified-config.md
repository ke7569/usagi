# 统一配置工具与 replay/live 约束

2026-09-06，首个代码交付。工具为 `tools/config/unified_config.py`，只依赖 Python 3.6+ 标准库。

本步实现两市策略配置的共同结构、迁移校验、旧格式审计还原、环境声明及版本锁。迁移字段保守记录来源和字段存在性，不连接行情、加载 TD、报单或替换共享库；完整交易运行时绑定仍明确带 `runtime_binding_implemented=false`。

后续增量已增加 `prepare_stream_processing.py`：从已绑定且禁用执行的统一配置生成两市原生行情处理 profile，由 `t0_sze_stream/t0_sse_stream` 消费。它只绑定解码/订单簿/采样/因子/模型这一部分，不加载完整策略或 TD；因此迁移字段仍保持保守的来源/字段存在性标记。stream profile 和 Paper strategy-intents 已支持，但不代表真实 TD ready。具体入口和剩余限制见[公共行情层](market-data-stream.md)。

## 已确定的运行时方向

live 与 replay 使用同一版本的解码、订单簿、采样、因子、模型和策略实现；替换输入 driver、时钟 driver 和执行 driver。采样和超时判断应在共用应用层完成，输入 driver 不再另算因子或预测。国信/国君属于协议适配维度，live/replay 属于输入维度。

完整精度的回放需要原始 datagram 或足够完整的 canonical event，并保存原始频道、事件顺序、批次边界、接收时间及时间域。虚拟时钟按记录推进，调度与 live 相同的回调；同刻事件/定时器先后顺序、严格 `>100us`、跨午休/收盘以及 EOF 是否推进尾部定时器，都必须在后续公共调度契约中明确并测试。

仅有 exchange time 的 CSV 会缺少接收间隔等信息，不能证明与 live 的 100us 静默采样等价。现有 `sz_hp_replay.cpp` 与 live recovery 在输入、批尾和模型接线方面仍有差异，本次没有把它们声明为统一完成。

## 统一结构 v1

| 部分 | 字段归属 |
| --- | --- |
| `schema_version`, `market` | 版本 1，市场 `SZ` / `SH` |
| `daily` | `trading_day`、可选 `source_date`、`instruments` 中的行情静态数据；统一股票键带 `.SZ/.SH` |
| `strategy` | `name`、可选 `parameters`、`warmup_signals`、`instrument_parameters` 中的数量规则 |
| `prediction` | 模型/缩放器路径、已声明模型哈希、book mode、采样、模型路由和捕获设置 |
| `market_data` | MD `source_ids`、快照 source、recovery 和 feed health 设置 |
| `trading` | TD `source_ids`、共同的 `routing` / `test_order`、运行/授权标志 |
| `account` | 显式 `reference`；`positions` 保存 `static_position` 和 `last_position`，不与行情静态数据混放 |
| `deployment` | 原框架入口、CPU 分配、vtd、日志路径等已识别运行字段 |
| `environment` | 输入模式、时钟来源和执行 driver 声明 |
| `legacy`, `migration` | 有限的旧数组/注释、原字段及股票键映射、来源哈希和组合信息，支持无损审计 |

初始环境为 `{"mode":"unbound","execution":"disabled"}`。绑定 live 选择 host clock，绑定 replay 选择 virtual clock；两者都默认禁用执行。replay 的 `execution=live` 被拒绝。环境声明不覆盖或抹除旧配置中的策略/交易参数，未来运行时必须独立落实执行 driver 的约束。

本版从旧配置迁移生成结构，不自动填补缺失的策略参数，也不推断券商、账户和模型能力。`migration` 保存字段存在性，避免把“没有字段，使用旧代码默认值”改成“新增显式值”。未知顶层及股票字段、互相矛盾的别名/数组、非法日期、非有限数值、错误类型和嵌入凭据均拒绝；不完整模板不当成有效运行配置。

已有嵌套 recovery/采样等参数保留原值，检查已知控制字段类型；此阶段不是所有 SDK 参数的完整运行时校验器。MD 网络配置、TD 认证/账户文件仍由既有适配器管理，后续按第 3 节边界接入 profile 校验，不把原始凭据搬入统一 JSON。

## 两市加载规则

深市 `migrate-sze` 调用选定版本的原 `prepare_sze_runtime.py`，验证 system/daily，在临时目录生成配置，再转换指定 trade 或 worker。它不重新实现分片、过滤和参数覆盖逻辑。默认使用仓库内生成器，`--generator` 可显式选择已审查的固定版本。记录 system、daily、generator 的 SHA-256；不读取 `credentials_path` 指向的内容。空分片被拒绝，避免输出没有股票的交易实例。

沪市 `migrate --runtime ... --daily ...` 按原 `sse_get_obj.cpp` 的白名单执行 daily 覆盖：`trading_day/static_data_source_date/ins_params/global_params/static_position`。日期必须来自明确的 `--day` 或 live 中固定日期；daily 哈希存在时必须匹配。解析后移除 `daily_config_path`，冻结该次有效配置，避免下次启动又按机器日期覆盖一次。daily 中无法映射的额外字段会报错。

`compare` 针对已解析的旧 runtime JSON 比较完整 canonical JSON，而不仅比较字段个数或哈希格式。它包含股票范围、持仓、模型、交易开关及其余保留参数。它证明配置迁移等价，不证明旧程序没有实现错误。

## 常用命令

以下从仓库根目录执行，输出路径可自行指定；命令不会运行策略。

```bash
python3 -B tools/config/unified_config.py migrate-sze \
  --system deploy/sze/daily/sze_system.json \
  --daily deploy/sze/daily/config_sze_daily_example.json \
  --account-ref guoxin-example --component trade --output build/config-unified/sze.json

python3 -B tools/config/unified_config.py migrate \
  --runtime /path/to/resolved-sse-config.json \
  --account-ref guoxin-example --output build/config-unified/sse.json

python3 -B tools/config/unified_config.py validate \
  --config build/config-unified/sse.json

python3 -B tools/config/unified_config.py compare \
  --config build/config-unified/sse.json --runtime /path/to/resolved-sse-config.json

python3 -B tools/config/unified_config.py bind \
  --config build/config-unified/sse.json --mode replay \
  --recording /path/to/capture.datagrams --record-format raw-datagrams \
  --time-basis recorded-receive --output build/config-unified/sse-replay.json

python3 -B tools/config/unified_config.py bind \
  --config build/config-unified/sse.json --mode live \
  --output build/config-unified/sse-live.json
```

`bind/validate` 允许录制文件尚未落盘，仅检查配置；不会将它标为已验证输入。`record-format` 支持声明 `raw-datagrams/canonical-events/csv/t0md-v1`，`time-basis` 为 `recorded-receive/exchange`。`t0md-v1` 必须使用接收时间，输入为连续分段的完整录制目录；run manifest 锁定全部分段内容，C++ [公共行情层](market-data-stream.md) 负责解码容器和完整性校验。`audit-legacy --config ... --output ...` 是原格式参数审计输出，保留原交易标志，不能当成已隔离 TD 的 replay 启动配置。

## 两类版本检查

`processing_sha256` 锁定参数、模型引用、股票/账户及原字段存在性，刻意排除 `environment`，用于检查只更换 driver 时处理配置未变。这不是一次完整实验的标识，换录制文件或时钟必须额外检查 run manifest。

artifact lock 对模型、缩放器、routing/sidecar 文件及显式提供的二进制计算实际文件哈希；模型声明了 SHA-256 时还必须与文件一致。文件必须存在且是常规文件。相关共享库/SDK 需要作为额外 `--binary` 明确列入，工具不声称自动覆盖所有动态依赖。

```bash
python3 -B tools/config/unified_config.py lock \
  --config build/config-unified/sse.json --binary /path/to/libt0_strategy_sse.so \
  --binary /path/to/libsse_md.so --output build/config-unified/artifacts.lock.json

python3 -B tools/config/unified_config.py verify-lock \
  --config build/config-unified/sse-replay.json --lock build/config-unified/artifacts.lock.json

python3 -B tools/config/unified_config.py lock-run \
  --config build/config-unified/sse-replay.json \
  --artifacts-lock build/config-unified/artifacts.lock.json \
  --output build/config-unified/replay.manifest.json

python3 -B tools/config/unified_config.py verify-run \
  --config build/config-unified/sse-replay.json \
  --artifacts-lock build/config-unified/artifacts.lock.json \
  --manifest build/config-unified/replay.manifest.json
```

run manifest 另锁定 environment（含格式、时钟及录制路径）和录制文件内容，验收时重新计算文件哈希。所有 manifest 均保留 `runtime_parity_verified=false`，文件未变不等于网络、调度、实盘回报或仿真成交与真实成交一致。相对 artifact 路径按命令工作目录解析，应与旧程序的运行目录一致。

## 本次验证与剩余工作

- 新增 28 项单元/集成测试：两市 roundtrip、字段校验、daily 覆盖、原深市生成器对照、账户仓位拆分、别名冲突、环境绑定、模型/二进制漂移及录制/时钟漂移，含分段目录变更/缺口校验。
- 现有深市 daily runtime 12 项测试通过。
- 最新深市固定副本的 trade 配置迁移通过；沪市源码包 2,314 只股票的配置经 `compare` 与原 runtime 完整一致，replay/live 的处理配置指纹相同。
- 实测产物在 `/home/ref/unified-config-s1-20260906-pB23Yu/`。其中录制路径为声明示例，未执行该路径的行情回放；实际模型/SDK 尚未齐备，未生成“实盘已验收”的版本锁。
- 下一步在同一构建产物中接入统一运行时与虚拟时钟，复用 live 的解码/采样/推理路径；消除既有 CSV replay 分叉，并验证等时事件、定时器、EOF、恢复及停机。不能在旧离线专用路径修好后就认为 live 已同步。

测试命令：

```bash
python3 -B -m unittest discover -s tests/common -p 'test_unified_config*.py' -v
python3 -B tests/sze/test_prepare_sze_runtime.py
```
