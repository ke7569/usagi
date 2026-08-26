#!/usr/bin/env python3
"""Dependency-free contract checks for the SSE TD adapter and test bundle."""

import json
import os


ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TD = os.path.join(ROOT, "modules", "deepwin_guoxin", "td")
DEPLOY = os.path.join(ROOT, "deploy", "sse_td_test_20260820")


def require(text, needle):
    if needle not in text:
        raise AssertionError("missing %r" % needle)


def main():
    entry = open(os.path.join(TD, "TDEngineGXSSE.cpp"), "rb").read().decode("ascii")
    require(entry, '#define T0_TD_SOURCE_ID 190')
    require(entry, '#define T0_TD_ENGINE_KEY "sse_td"')
    require(entry, '#define T0_TD_DEFAULT_MARKET_ID 101')
    require(entry, "#define gxbse_direct_cash_order gxsse_direct_cash_order")
    require(entry, '#include "TDEngineGXBSE.cpp"')

    cmake = open(os.path.join(ROOT, "CMakeLists.txt"), "rb").read().decode("ascii")
    require(cmake, "option(SSE_BUILD_TD")
    require(cmake, "${SZE_TD}/TDEngineGXSSE.cpp")
    require(cmake, "OUTPUT_NAME sse_td")

    config = json.loads(open(os.path.join(DEPLOY, "config_sse_td_test_600519.json"), "rb").read().decode("utf-8"))
    if config["market"] != "SH" or config["td_source_index"] != [190]:
        raise AssertionError("SSE TD source/market contract mismatch")
    if config["sse_test_order"]["enabled"] or config["production_approval"]:
        raise AssertionError("test bundle must be order-disabled by default")

    account = json.loads(open(os.path.join(DEPLOY, "deepwin_sse_td.example.json"), "rb").read().decode("utf-8"))
    unit = account["td"]["sse_td"]
    if unit["market_id"] != 101:
        raise AssertionError("unexpected SSE market id default")
    if "REPLACE_IN_ROOT_ONLY_SECRET" not in unit["trade_password"]:
        raise AssertionError("account example must not contain a credential")

    print("sse_td_contract_test: PASS")


if __name__ == "__main__":
    main()
