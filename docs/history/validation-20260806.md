# Bundle validation

The bundle was built from its own `workspace` directory inside the validated
CentOS 7.9 container, rather than merely checking the repository build output.

Command:

```bash
./build_sze.sh
```

Results:

- `t0_strategy_sze`: built successfully.
- `sze_md`: built successfully.
- `sze_recovery_status`: built successfully.
- `sze_recovery_verify`: built successfully.
- `sze_recoverable_test`: PASS.
- `sze_protocol_test`: PASS.
- `sze_config_guard_test`: PASS.
- strategy ABI check: `kungfu::wingchun::IWCStrategy` present; global
  `IWCStrategy` typeinfo absent.
- `ldd` checks: no missing dependencies in the bundled build environment.

The bundle's prebuilt production artifact is separate from the portable
validation build. The validation CMake defaults to `-march=x86-64 -mtune=generic`;
set `SZE_MARCH_NATIVE=ON` only after comparing the production CPU flags.
