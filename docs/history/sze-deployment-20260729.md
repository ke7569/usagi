# Shenzhen All-Market Shadow Deployment: 20260729

The staged tree mirrors the target paths below `/home/zane`.  Copy the files
under `home/zane/` into the existing `/home/zane/` directory without deleting
the existing runtime libraries or historical data.  Copy the five unit/timer
files under `etc/systemd/system/` to `/etc/systemd/system/`.

Required pre-existing runtime paths on the host are `/home/zane/lib` or the
standard `/opt/deepwin` runtime.  The deployment does not bundle a second copy
of those shared libraries; the preflight checks their resolution using the
same `LD_LIBRARY_PATH` as the launch scripts.

Run before enabling any timer:

```bash
cd /home/zane/run_main
chmod +x *.sh
TRADING_DAY=20260729 ./sze_all_preflight_20260729.sh
sudo systemctl daemon-reload
sudo systemctl start sze-all-capture-20260729.service
/home/zane/bin/sze_recovery_status /dev/shm/sze_all_20260729.events
sudo systemctl start sze-all-start-20260729.service
```

Start the all-market service only after the capture service reports
`producer_alive=1` and the status tool shows the correct trading day.  It
starts all eight independent recovery consumers; each cannot send orders
because it has `capture_only=true`, `vtd=[]`, and `td_source_index=[]`.

Expected files for the day are:

* Journal: `/home/zane/data/sze_journal_20260729/`
* Shared ring: `/dev/shm/sze_all_20260729.events`
* Per-shard CSV: `/home/zane/run_main/log/sze_all_20260729/shard_XX/`
* Sequence/drop telemetry: `./sze_ingress_watch_20260729.sh`

Do not use this deployment for a production order-routing process.  It is
strictly an all-market shadow capture, orderbook, factor and prediction run.
