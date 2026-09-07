#!/usr/bin/env python3
"""Build a live-input, execution-disabled SZE paper shadow profile."""

import argparse
import json
import os
import sys


REPO_ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "../../.."))
sys.path.insert(0, os.path.join(REPO_ROOT, "tools", "config"))

from prepare_stream_processing import make_profile  # noqa: E402
from unified_config import (  # noqa: E402
    ConfigError,
    bind_environment,
    migrate_sze_system,
    validate,
    write_json,
)


def prepare(system, daily, day, generation, account_reference):
    config = migrate_sze_system(
        system, daily, account_reference, component="trade")
    if config["daily"]["trading_day"] != day:
        raise ConfigError("shadow trading day differs from daily configuration")

    recovery = config["market_data"].get("recovery")
    if not isinstance(recovery, dict) or recovery.get("enabled") is not True:
        raise ConfigError("shadow requires the recoverable SZE market-data input")
    recovery["trading_enabled"] = False
    recovery["expected_generation"] = generation
    recovery.pop("state_cpu", None)
    recovery.pop("strategy_cpu", None)

    # Preserve live-routing strategy behavior so intents traverse the in-process
    # OMS. The stream runtime itself is execution-disabled and uses PaperBackend.
    trading = config["trading"]
    trading["enabled"] = False
    trading["production_approval"] = False
    trading["runtime_mode"] = "staged"
    trading["oms"] = {
        "simulation_cash": 1000000000000.0,
        "fee_reserve_per_order": 0.0,
    }
    top_fields = set(config["migration"]["top_fields"])
    top_fields.update((
        "trading_enabled", "production_approval", "runtime_mode", "oms"))
    config["migration"]["top_fields"] = sorted(top_fields)

    config = bind_environment(config, "live")
    validate(config)
    profile = make_profile(
        config, strategy_intents=True, recovery_input="handoff")

    if profile["execution"] != "disabled" or \
            profile["environment"]["execution"] != "disabled" or \
            profile["strategy_runtime"]["mode"] != "paper-intents" or \
            profile["recovery"]["trading_enabled"] is not False:
        raise ConfigError("shadow execution safety contract was not preserved")
    return config, profile


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--system", required=True)
    parser.add_argument("--daily", required=True)
    parser.add_argument("--day", required=True, type=int)
    parser.add_argument("--generation", required=True, type=int)
    parser.add_argument("--account-reference", required=True)
    parser.add_argument("--unified-output", required=True)
    parser.add_argument("--profile-output", required=True)
    args = parser.parse_args(argv)
    if args.day < 20000101 or args.day > 99991231:
        raise ConfigError("invalid shadow trading day")
    if args.generation <= 0 or args.generation >= 1 << 64:
        raise ConfigError("invalid capture generation")

    config, profile = prepare(
        args.system, args.daily, args.day, args.generation,
        args.account_reference)
    write_json(args.unified_output, config)
    write_json(args.profile_output, profile)
    print(json.dumps({
        "account_reference": args.account_reference,
        "execution": profile["execution"],
        "generation": args.generation,
        "input_driver": profile["input_driver"],
        "instruments": len(profile["instruments"]),
        "market": profile["market"],
        "ok": True,
        "strategy_mode": profile["strategy_runtime"]["mode"],
        "trading_day": args.day,
    }, sort_keys=True))


if __name__ == "__main__":
    try:
        main()
    except (ConfigError, ValueError, TypeError, OSError) as error:
        raise SystemExit("prepare_sze_shadow_error: " + str(error))
