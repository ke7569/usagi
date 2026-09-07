# SZE Prediction Log

The online prediction path supports two output formats:

- `csv`: legacy buffered CSV output for compatibility and small diagnostics.
- `sze_log`: asynchronous framed binary output for live full-market collection.

For `sze_log`, every sample is written as one record. Instruments listed in
`detail_instruments` receive full ten-level book and all model factors. Other
enabled instruments receive only the compact sample fields: timestamps,
sequence/window identifiers, trigger flags, prices, turnover, volume, source,
continuity, prediction, and model latency.

Order, trade, and market-resolution records are written only for detailed
instruments. This avoids formatting and event-level timing reads for the rest
of the universe.

Example:

```json
"sze_prediction_capture": {
  "enabled": true,
  "directory": "/home/zane/run_main/log/sze_all_202608xx",
  "prefix": "sze_all_202608xx",
  "instruments": ["000001.SZ", "000002.SZ"],
  "output_format": "sze_log",
  "detail_instruments": ["000001.SZ"],
  "events": true,
  "samples": true,
  "capture_only": true,
  "flush_interval_ms": 1000,
  "log_batch_bytes": 1048576,
  "log_queue_bytes": 268435456
}
```

The live file is `<prefix>_predictions.szelog`. It has a fixed file header,
framed records, record sequence numbers, and CRC checksums. A crash leaves an
incomplete tail that the parser ignores after validating all complete records.

After the session:

```bash
python3 /home/zane/bin/parse_sze_prediction_log.py \
  /home/zane/run_main/log/sze_all_202608xx/sze_all_202608xx_predictions.szelog \
  --output-dir /home/zane/run_main/log/sze_all_202608xx/parsed
```

The parser produces compact samples, full samples, detailed events, market
resolutions, and `summary.json`. CSV is therefore a post-session export rather
than the online write format.
