#!/usr/bin/env python3
"""Add master-level Shenzhen universe exclusions to an AkShare static audit."""

import argparse
import json
import os
import re

import akshare as ak


SZ_CODE = re.compile(r"^(000|001|002|003|300|301)[0-9]{3}$")


def read_json(path):
    with open(path, encoding="utf-8") as stream:
        return json.load(stream)


def write_json(path, value):
    temporary = path + ".tmp"
    with open(temporary, "w", encoding="utf-8") as stream:
        json.dump(value, stream, ensure_ascii=False, indent=2)
        stream.write("\n")
    os.replace(temporary, path)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--audit", required=True)
    parser.add_argument("--rejected", required=True)
    args = parser.parse_args()

    audit = read_json(args.audit)
    rejected_doc = read_json(args.rejected)
    excluded = []
    for row in ak.stock_info_a_code_name().itertuples(index=False):
        code = str(row[0]).zfill(6)
        name = str(row[1])
        if not SZ_CODE.match(code):
            continue
        if "退" in name:
            reason = "delisting_or_delisting_board"
        elif name.startswith(("N", "C")):
            reason = "new_listing_excluded"
        elif not audit.get("include_st", False) and "ST" in name.upper():
            reason = "st_excluded"
        else:
            continue
        excluded.append({"code": code, "name": name, "reason": reason})

    rejected_doc["master_rule_excluded"] = excluded
    rejected_doc["master_rule_excluded_count"] = len(excluded)
    audit["master_rule_excluded_count"] = len(excluded)
    audit["rejected_count"] = len(rejected_doc.get("rejected", []))
    audit["accepted_count"] = len(audit.get("instruments", []))
    audit["candidate_count"] = audit["accepted_count"] + audit["rejected_count"]
    audit["universe_rule"] = {
        "include_prefixes": ["000", "001", "002", "003", "300", "301"],
        "exclude": ["ETF/LOF/bonds/funds/B-shares by prefix", "delisting", "new listing"],
        "st_included": bool(audit.get("include_st", False)),
        "suspended_included": bool(audit.get("include_suspended", False)),
    }
    write_json(args.rejected, rejected_doc)
    write_json(args.audit, audit)
    print("master_rule_excluded={}".format(len(excluded)))


if __name__ == "__main__":
    main()
