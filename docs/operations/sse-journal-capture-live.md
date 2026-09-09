# 上海逐笔、快照采集

正式入口是 `sse-journal-capture.service`。它只保存原始 UDP 行情，不读取
daily 策略配置、模型或 Auction59 CSV。预测可以之后从 journal 重跑。

订阅固定在 `deploy/sse/journal_capture.live.json`：

| 数据 | 组播地址 | 本地网卡/IP |
| --- | --- | --- |
| 逐笔 | 239.35.80.9:37109 | hqh-p1-k2 / 11.11.11.11 |
| 快照 | 239.35.80.5:37105 | hqh-p1-k2 / 11.11.11.11 |

数组顺序对应 journal 的 channel_id：逐笔 0，快照 1。
保存格式是二进制 journal，payload 为 sse-stream-v2，保留原始包与接收时间戳。
不是预测 CSV，也不会按股票池或预测有效性过滤行情。

机器安装位置：

- 程序：`/home/zane/usagi-bin/t0_sse_journal_capture`
- 启动脚本：`/home/zane/usagi-sse-dev/tools/sse/run_journal_capture.py`
- 数据：`/home/zane/usagi-runtime/YYYYMMDD/capture-*/journal/`
- 当天当前配置和日志：`/home/zane/usagi-runtime/YYYYMMDD/current/`

启动脚本自动填日期、boot_id 和新 epoch 路径，不需要研究机每天生成采集配置。
每次启动保留独立配置和日志；重启前的 journal 不覆盖、不删除。旧共享内存 ring
是临时缓存，会在进程退出后清理；历史数据以 journal 为准。重启导致的时间缺口
不能凭新进程启动成功认定为补齐。

systemd 开机启动、异常退出后 30 秒重启。工作日 08:55 切换当天目录，15:10
正常停止并刷盘。使用 `deploy/sse/journal_capture.crontab` 替换旧采集/并行预测
定时入口，避免旧流程重复占用 CPU 和磁盘；旧交易重启定时项一并停用。

查看当前状态：

```sh
systemctl status sse-journal-capture.service
tail -n 3 /home/zane/usagi-runtime/$(date +%Y%m%d)/current/capture.stderr
```

`ready=true` 表示订阅和写盘已就绪；开盘后还需看到 `channels` 中逐笔、快照的
`datagrams` 都增加，并且实时状态中的 journal_errors、journal_overflows 为零。
正常停止后的最终 JSON 还要检查 `stats.kernel_drops` 为零。总包数增加不能单独
证明两路行情都到达。行情完整性还取决于上游发包及频道序号连续性；不能用凌晨
无行情时的启动检查替代全天验收。

日常只用三个命令，采集不依赖模型、daily 配置或 TD：

```sh
# 确认 sse-dev 分支，编译并测试同一份产物，然后安装；不会重启正在采集的进程。
bash deploy/sse/install_journal_capture.sh --build build/sse-capture-release
# 只编译和验证，不安装时加 --check-only。

python2 deploy/sse/check_journal_capture.py intraday --root /home/zane/usagi-runtime
# preopen 检查进程、ready、两频道和写盘状态；intraday 再比较间隔 6 秒的两路计数。
# closed 检查当天所有 epoch 的最终 clean、写盘/溢出错误和 kernel drops。

python2 tools/sse/prune_capture_days.py --root /home/zane/usagi-runtime --keep-days 5 --apply
```

每天 08:56 做 preopen，09:16、13:01 做 intraday，15:11 做 closed，15:20 清理旧日。
保留最近五个已有采集数据的日期（包含当天），默认不删除，加 `--apply` 才执行。
当前日、active_capture 所在日、被进程打开的目录始终保留；拒绝越界或异常符号链接。
旧日即使缺少正常结束日志，也不会因此无限期保留；完整性由收盘验收报告。
当前磁盘不足时不会删掉保护范围内的数据。正常的单机采集状态不能证明上游没有漏发行情。

保留策略仅清理采集数据，同日 TD/OMS 记录、模型与配置保留。
策略进程内的 TD 接入及实际查询结果见 [TD 接入说明](sse-journal-td-live.md)。
