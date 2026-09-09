# 上海 usagi 采集与 TD 接入（2026-09-09）

运行结构保持两部分：独立 capture 保存逐笔和快照；交易主程序读取 journal/共享内存，在同一进程里完成预测、策略、OMS 和 ATP TD。TD 是主程序加载的 `libsse_td.so`，不是另起一个独立报单工具。

## 采集与日常运行

代码目录：`/home/zane/usagi-sse-dev`，发布目录：`/home/zane/usagi-bin`。

- `sse-journal-capture.service` 已启用，08:55 切换当天采集，15:10 停止并刷盘。
- `sse-journal-trading.service` 在 08:57 启动、15:09 停止，已装入实盘 crontab。默认 monitor：预测、策略和 TD 查询运行，下单和撤单关闭。
- 08:56 检查启动，09:16、13:01 检查逐笔和快照两路计数增长，15:11 检查收盘结果；结果写 `/home/zane/usagi-runtime/ops.log`。
- 15:20 清理过期行情，保留最近五个已有采集数据的日期，包含当天。当前采集及进程正在打开的数据保留；TD/OMS 记录、模型和配置不随行情清理。
- 旧 daily/parallel16/交易重启定时入口已停用，旧 main 和遗留聚合进程已退出。

统一采集发布命令：

```sh
cd /home/zane/usagi-sse-dev
bash deploy/sse/install_journal_capture.sh --build build/sse-capture-release
```

它编译、验证并安装同一份采集产物，不重启当前 capture。TD 发布不依赖这个采集发布动作。

## TD 已接通的部分

`t0_sse_journal_predict` 已能在策略进程内加载 ATP TD，查询资金、持仓、委托、成交，组成 OMS 账户快照。策略实际持仓从查询和回报获得，不要求配置 `last_position`。SDK 回调由策略线程领取，无行情时也持续处理回报和定时器。

实盘 query-only 已通过：账户连接和对账 ready，股票池 2316 只，非零持仓、委托、成交均为 0。未发送真实委托。

收尾修复了同日重启时查询 token=1 被旧日志拒绝的问题，连续三次重启回归通过。
修复后的程序与 TD 库已重新安装，真实 query-only 再次成功（553 毫秒）。

可复用的查询命令（日期与 daily 文件必须同步）：

```sh
SSE_ENABLE_LIVE_ORDER=NO \
LD_LIBRARY_PATH=/home/zane/td_query_runtime_20260824/lib \
timeout --signal=TERM --kill-after=5 30 taskset -c 240 \
  /home/zane/usagi-bin/t0_sse_journal_predict \
  --td-query-only /home/zane/usagi-runtime/20260909/td_query.json
```

配置只引用已有私密账户文件，不复制密码到仓库。账户由一个 OMS 独占；不要同时启动旧 main。

模拟闭环覆盖真实 ZStrategy → TD 插件接口 → 委托回报 → OMS，以及撤单、重复信号和交易开关。它证明代码通路，券商真实委托回报仍待验证。

## 启动入口与当前验收

使用 `tools/sse/run_journal_trading.py --live-config /home/zane/usagi-runtime/journal_trading.live.json`。长期连接和模型字段留在 live，日期、股票池、`global_params`、每股 `static_position` 从当天 daily 读取。启动时在内存中组织参数，通过匿名文件描述符传入同一个交易主程序；没有额外的配置生成操作。

09-09 daily 的 `global_params` 已从 09-04 补齐，`position_limit=1`。入口持有跨进程锁，并为每次连接持久递增 `td.epoch`。monitor、query-only 和正式交易使用各自的 OMS journal。

当天 daily 缺少 `global_params` 时，入口在内存中沿用最近合法历史 daily 的该字段并记录来源；当天显式参数优先，不继承历史股票静态信息，不改源配置。启动测试 9/9 通过。

真实 TD 登录和账户查询已通过。09-09 首次 monitor 的两个阻塞已修复：合法心跳被当作未知数据，以及快照/逐笔合流后误用全局 PHC 单调检查。现按 UDP 订阅分别分批、检查时间；同订阅倒序（含 idle 后倒序）仍拒绝。

修复后的主程序已于 09-09 15:51 安装至 `/home/zane/usagi-bin/t0_sse_journal_predict`，旧版本备份为同目录 `t0_sse_journal_predict.bak-20260909-155159`。处理器、策略、TD 禁单、journal 集成和配置门禁测试 5/5 通过。Python3 profile 测试未在实盘机运行（本机 Python2 缺少 `importlib.util`）。

收盘后用同一构建的 journal reader、解码、因子和模型读取今日原始记录：全 2316 股的限时回放到 09:31:48，产生 248,876 个逐笔预测和 51,900 个快照预测；当天 static_position>0 的 10 股另从头连续回放 2,038,208 条事件至 09:36，产生 5,386 个逐笔预测（09:35 后选用 880 个）、836 个快照预测，无处理错误。诊断源码和结果在 `/home/zane/usagi-experiments/journal-accept-20260909/`。它是离线模型验证；盘中追赶、持续运行和真实委托尚未验收。

capture 已按 15:10 定时正常停止，收盘检查的 journal_errors、journal_overflows、kernel_drops 均为 0。交易服务按日运行 monitor，真实下单和撤单仍关闭。当天 daily 必须存在且日期匹配，不能用昨天的股票静态数据冒充当天；09-10 daily 截至本次验收尚未到达。

live 入口要求 `execution=live`、`strategy_runtime.mode=live`，同时满足 `trading_enabled`、`production_approval` 与 `SSE_ENABLE_LIVE_ORDER=YES`。实际账户对账完成、行情追赶成功且信号距本地时间不超过 1 秒后才允许下单；这些条件不影响 capture 保存行情。
