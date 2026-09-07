# SZE new-framework shadow

This service runs the `sze-dev` stream runtime beside the existing production
recovery and trade processes. It reads the same journal and shared-memory ring
with an independent cursor. It cannot load a TD adapter: the stream profile,
runtime environment, and CLI all require `execution=disabled`, and strategy
intents terminate at the in-process `PaperBackend` OMS.

The timer starts at 09:10, after the production start timer. The service waits
for a healthy capture up to 20 times, reads the current capture generation,
generates an exact-day handoff profile, and binds its lifetime to the capture
service. It uses CPU 40 by default.

Runtime evidence is written under `/run/sze-shadow/YYYYMMDD/`:

- `prepare.json`: generated profile summary
- `capture.status`: capture epoch and health at startup
- `stream.stderr.log`: readiness or failure details
- `stream.stdout.log`: final processing and Paper OMS summary after shutdown

Install with:

```bash
install -m 0644 deploy/sze/shadow/sze-shadow.service /etc/systemd/system/
install -m 0644 deploy/sze/shadow/sze-shadow.timer /etc/systemd/system/
systemctl daemon-reload
systemctl enable --now sze-shadow.timer
```
