# Historical SSE memory pipeline experiment

This directory preserves the independent Shanghai event-pipeline prototype
from the `usagi-experiment` `a06531a` line.  It is intentionally disabled by
default and has no production entry point.  Enable it with
`-DUSAGI_BUILD_SSE_EXPERIMENTS=ON` when investigating the old capture,
sharding, journal, model, or sampling behavior.

The experiment owns its old event envelope, SPSC queues, journal adapter,
model consumers, benchmark inputs, tests, and UDP observer.  The UDP runtime
is ABI-isolated from `common/stream`.  CPU affinity and shard planning are
provided by the main `sse_runtime_support` target; their old implementations
are deliberately not copied here.

`legacy_journal/` contains the old `SZERecoverable` implementation solely for
the historical `.szej` format.  Those files are not the main `sze` module.
Old `.szej` journals are incompatible with the main `.t0md` journal format
and must not be replayed by the production runtime.

The old sampling and latency measurements describe this prototype's queue,
timer, and model path.  They are historical diagnostics, not measurements or
contracts for the main SSE runtime.

Targets and tests use the `sse_experimental_` prefix.  No model weights,
market-data captures, deployment binaries, TD adapters, or `get_obj` sources
are part of this experiment.

For a local historical run after configuring with the option enabled in
`build/sse-dev-experiments`:

```sh
build/sse-dev-experiments/sse_experimental_event_pipeline \
  sse/experimental/memory_pipeline/config/sse_event_pipeline.example.json
build/sse-dev-experiments/sse_experimental_event_pipeline \
  sse/experimental/memory_pipeline/config/sse_event_pipeline.example.json --replay
build/sse-dev-experiments/sse_experimental_event_pipeline \
  sse/experimental/memory_pipeline/config/benchmark_sse_20260904_tick.json --benchmark
build/sse-dev-experiments/sse_experimental_udp_observer /tmp/sse-experimental.jsonl \
  feed_a 127.0.0.1 12020 --interface-ip 127.0.0.1 --duration-ms 1000
```

The benchmark JSON files retain the source machine's absolute model, journal,
and capture paths; replace those paths for the machine running the experiment.
These commands exercise only the historical experiment and are not capture or
deployment instructions for the main runtime.

To register the real-weight consumer test, set all three CMake cache inputs:
`SSE_EXPERIMENT_TEST_TICK_MODEL`, `SSE_EXPERIMENT_TEST_STATIC_JSON`, and
`SSE_EXPERIMENT_TEST_TRADING_DAY`. Without them, the consumer test executable
is built but its data-dependent CTest case is not registered. Python integration
checks support Python 2.7 or Python 3 through `SSE_EXPERIMENT_PYTHON`.
