# sse-live 收包与资源优化记录（2026-09-10）

本次接续旧会话完成代码修改、生产二进制部署和收盘后的真实服务试启动。09-11 开盘流量尚未发生，09-11 daily 配置当前未在研究端生成，不能将下一交易日预测启动标记为已验证。

## 核对到的实际问题

- 网卡 hqh-p1-k2 属于 NUMA 0，原普通 IRQ 分布在 NUMA 1 CPU 192–223；PTP IRQ 在 CPU 0，与旧 capture 最终日志所报收包核相同。
- 旧自动 CPU lease 仅排除部分 SSE 进程及 L3 租约，不排除 IRQ 或普通后台任务。
- megasas 存储 IRQ 的亲和性由驱动固定，修改 smp_affinity_list 和 smp_affinity 均返回 EIO，包括尝试在原范围内缩小。没有重载存储驱动或重启主机；它仍可与应用核争抢。
- 预测原配置为每 30 秒无限重启。真实试启动还发现快照心跳 0x8b 未被接受，会触发确定性的解码故障。
- GDB 线程栈确认 TD 动态依赖 liblog4cplus.so.0 创建约 512 个等待中的日志线程。当前库未发现受支持的线程数设置入口；未修改供应商二进制。其后台线程与 TD 一同固定在 CPU 40，预测主线程在 CPU 32。
- 旧记录中的供应商序号缺口、nodesc 累计计数不能证明缺口发生在哪一层；新的低流量测试也不能反推事发时状态。

## 已部署

| 资源 | 设置 |
|---|---|
| RX ring | 1024 → 4096，每次 capture 启动前核验，已生效时不重复设置 |
| 收包 / 分发 / journal | CPU 8 / 16 / 24，不同 L3，均 NUMA 0 |
| 预测 / TD | CPU 32 / 40，不同 L3，均 NUMA 0 |
| 行情 IRQ | CPU 64–95，PTP CPU 95，均 NUMA 0 |
| capture 控制线程、资源监控 | CPU 128；资源监控 Nice=10、idle I/O |
| UDP 批量 | 64 → 128；满批后免 poll 继续公平轮转各 socket |
| socket 缓冲 | 申请 64 MiB，ss 实测 rb=134217728，保持原申请值 |

capture 最终统计增加 full_receive_batches、max_receive_syscall_ns、max_full_batch_gap_ns。现有队列/时间戳统计保留。

逐笔通道缺口仍使处理失效、停止预测；日志包含前后 wire/provider 序号、stream sequence、包内偏移、硬件/内核/应用时间。journal_prediction_fault 还记录当前事件对应的 journal_file、journal_offset、event_id。定位只在故障路径扫描 journal。

缺口退出码为 65，systemd RestartPreventExitStatus=65 阻止反复回放。其他失败限制为 300 秒内最多启动 3 次。不得通过跳过缺口或仅重启绕过无效订单簿；需先补齐数据或从可靠状态重建。

快照心跳仅增加严格的 32 字节、两个相同 16 字节块、指定类型及保留字段检查，未泛化为跳过未知数据。

## 监控和明早验收

脚本：`/home/zane/usagi-sse-dev/tools/sse/sse_receive_ops.py`。

监控由 capture 的 Wants/After 自动启动，写入 `/home/zane/usagi-runtime/YYYYMMDD/receive-resources-HHMMSS.jsonl`。普通时段 5 秒采样，09:24–09:26 每秒采样；每 5 次采样记录进程线程亲和性及 schedstat。包含 NIC/UDP/softnet 基线与增量、socket 队列/丢包、CPU ticks、网卡和存储 IRQ、磁盘统计、capture 队列与健康。计数回退单独标记为 reset。

`port_rx_dp_di_dropped_packets` 在低流量时也增长。端口计数不等同于这两路组播行情的丢失；必须结合 socket、内部队列和业务序号判断。`loss_alert` 是计数异常提示，不是已确认的行情丢包归因。

- 00:05：现有 Windows `SSE-DailyConfig-Fetch`，延迟生成时重试到 07:30；本次提前拉取 09-11 返回“研究端尚未生成”。
- 08:50：新增 preflight，检查当日日配、模型路径和程序可用性。
- 08:55：capture 启动；应用 RX/IRQ 设置；绑定控制线程。
- 08:56：原 capture 开盘前检查。
- 08:57：原预测服务启动。
- 08:58：新增 check-startup，校验当日 capture、双通道、同一 generation 的预测 live 和 TD ready，记录线程现场。
- 15:09/15:10：原预测/capture 停止；15:15 停止资源监控。

新增验收结果写入 `/home/zane/usagi-runtime/ops.log`；失败退出非零，不会把未就绪报为通过。它不发送外部通知。

## 验证证据

- 现有 5 项测试通过：market_data_stream_test、sse_stream_processor_test、sse_journal_trading_launcher_test、sse_journal_launcher_test、sse_journal_integration_test。心跳修复后重复执行相关 2 项并通过。
- 新测试 `tests/sse/test_receive_resource_fault.py`：真实回环 UDP 双通道 1003 包，强制满批续收、快照 500 包完整到达；注入缺口后预测退出 65、journal 偏移与事件匹配，capture 继续接收。证据目录 `/tmp/sse-receive-fault-FuQiqD`。
- 本机 systemd 独立验证退出码 65：只启动一次，未自动重启；临时验证 unit 已移除，证据为 `restart-prevention.txt`。
- 生产路径真实试启动收到两路带硬件时间戳心跳，预测完成 journal 回放并进入 live，TD connected/ready，orders_enabled=false。
- 实测 CPU 亲和性与上述分配一致。低流量样本分发/journal/TD 核近满占用，收包和预测核低占用，队列为空；不能代表竞价高峰容量。
- 资源监控采样无错误，留存 socket drops=0、内部 queue_size=0、journal_errors/overflows=0 的低流量样本。

备份及启动报告：`/home/zane/usagi-runtime/resource-fix-backup-20260910/`。`startup-check-final.json` 为真实服务验收；`preflight-20260911.json` 如实报告未来日配缺失。

最终真实启动验收时间为 16:51:11，使用今日 2317 只股票的配置，包含所有关键线程/IRQ/ring 实际状态检查。最后一轮盘后 capture 收到 130 包、130 个硬件时间戳，kernel_drops、journal_errors、journal_overflows 均为 0，journal clean。验证进程已全部停止，active_capture 和今日 current 已恢复为原盘中 epoch；新参数与定时任务保留，明早生效。源码改动保留为未提交差异，已备份补丁和新增文件。

## 回退和边界

原二进制、systemd 服务、cron、日配模板和私有 live 配置在上述备份目录。原交易日 capture 位于 `/home/zane/usagi-runtime/20260910/capture-085501-ZI6zPk`，盘后验证另建 capture epoch，不改写原 journal。源码差异另存 source-changes.patch；新增文件保留在仓库。

回退需先停止预测和 capture，再恢复备份程序、配置及 cron、执行 daemon-reload。IRQ 原值及 ring 文本在 `/home/zane/usagi-runtime/receive-tuning/before-*.json`；最早一次记录为调整前状态。设置 ring=1024 可能短暂中断行情，仅在采集停止时操作。

尚待实盘后续证据：09-11 daily 到位后的 preflight，以及 08:58 实际启动、09:25 突发收包/调度/丢包数据。上游缺失无法靠本地容量或排序修复；本次未接入供应商补包接口。
