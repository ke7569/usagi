# Shenzhen runtime simplification

The production runtime now has three deliberate boundaries:

- The daily business JSON is strict: `trading_day`,
  `static_data_source_date`, `static_data_hash`, and `ins_params` only.
- `libsze_md.so` does not contain the experimental direct sharded ring. That
  prototype is built only with `-DSZE_BUILD_DIRECT_SHARDED_RING=ON`.
- Recovery resource teardown is owned by one `MDEngineSZE` helper, used by
  initialization failures and shutdown.

## Migration

Convert an old all-in-one file offline, then validate it before deployment:

```bash
python3 migrate_legacy_daily.py old_config.json \
  config_sze_daily_20260817.json \
  --trading-day 20260817 --source-date 20260816
python3 prepare_sze_runtime.py validate \
  --system /home/zane/configs/general_config/sze_system.json \
  --daily config_sze_daily_20260817.json --day 20260817
```

The migration utility removes operational `cpu` and `last_position` fields and
recomputes `static_data_hash`. It does not fetch or alter static market data.

## Rollback

Keep the previous `libsze_md.so`, `libt0_strategy_sze.so`, fixed system JSON,
and daily JSON as a matched set. To roll back, stop the SZE services, restore
the matched library/config set, and run the same exact-date preflight before
starting recovery. Do not mix a pre-refactor MD library with a post-refactor
runtime configuration.
