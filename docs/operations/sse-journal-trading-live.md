# 上海预测、策略与 TD 启动

使用 `tools/sse/run_journal_trading.py` 读取长期 live 和当日 daily，直接启动
`t0_sse_journal_predict`。程序内完成预测、策略、TD。启动时在内存中投影参数，
通过匿名临时文件的文件描述符传入主程序；没有第三份需要维护的配置，也没有
额外的生成命令。capture 独立运行，不会被此脚本停止或重启。

默认 `monitor` 常驻：读取今日 journal，运行模型和策略，连接真实 TD 并查询账户，
禁止下单和撤单。`--query-only` 仅做一次账户查询；`--live-orders` 才允许正式交易，
仍要求 live 内两个布尔门都为 true 且环境变量 `SSE_ENABLE_LIVE_ORDER=YES`。

`global_params`、`ins_params`、static_position、日期只从 daily 读取；
last_position 由 TD 查询得到。模型、私密 TD 配置路径、账户、费用预留与运行根目录
留在 live。`daily_config_pattern` 自动选今天文件，`--daily-config` 可手动覆盖。
日期不符立即失败。`td_cpu=-1` 由运行时自动选与预测不同 L3 的 CPU。

当天 daily 没有 `global_params` 时，只在内存中沿用同一文件模式下最近一个
合法历史 daily 的该字段，并记录来源日期。当天显式配置优先；显式配置非法则
失败，不覆盖。当天文件缺失或所有历史参数都不可用也失败。股票池、静态持仓
和行情静态数据不从历史文件继承，不修改 daily 原文件。

实盘 crontab 已安装 08:57 启动、15:09 停止交易服务；capture 独立在 08:55
启动、15:10 停止。交易服务默认 monitor，真实下单和撤单关闭。

每次启动在 `runtime_root/td.epoch` 追加递增代次，进程继承文件锁，防止重复启动。
monitor、正式交易、单次查询分别使用当日 `td-monitor/`、`td/`、`td-query/` 下的
OMS journal，避免观察模式恢复正式交易的待撤单状态。

```sh
python2 tools/sse/run_journal_trading.py \
  --live-config /home/zane/usagi-runtime/journal_trading.live.json
# 限时检查常驻链路：加 --duration-ms 10000
# 单次真实账户查询：加 --query-only
```

示例中的模型路径需要填实际文件。测试不连接真实 TD：

```sh
python2 tests/sse/test_journal_trading_launcher.py
```
