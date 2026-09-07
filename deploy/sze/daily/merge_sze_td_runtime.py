#!/usr/bin/env python3
"""Merge private TD settings into an ephemeral Deepwin config."""

import json
import os
import sys
import tempfile


def merge(base_path, credentials_path, system_path, output_path):
    with open(base_path, encoding="utf-8") as stream:
        output = json.load(stream)
    with open(credentials_path, encoding="utf-8") as stream:
        private = json.load(stream)
    with open(system_path, encoding="utf-8") as stream:
        system = json.load(stream)
    td = private.get("td")
    if not isinstance(td, dict) or not td:
        raise ValueError("TD credentials file must contain a non-empty td object")
    cpus = system["cpu_affinity"]
    receive_cpu = int(cpus["td_receive_cpu"])
    send_cpu = int(cpus["td_send_cpu"])
    for engine in td.values():
        if not isinstance(engine, dict):
            continue
        engine["receive_thread_cpu"] = receive_cpu
        engine["send_thread_cpu"] = send_cpu
        for account in engine.get("accounts", []):
            if isinstance(account, dict) and isinstance(account.get("info"), dict):
                account["info"]["receive_thread_cpu"] = receive_cpu
                account["info"]["send_thread_cpu"] = send_cpu
    output["td"] = td
    parent = os.path.dirname(os.path.abspath(output_path))
    fd, temporary = tempfile.mkstemp(prefix=".sze-td-", dir=parent)
    try:
        with os.fdopen(fd, "w", encoding="utf-8") as stream:
            json.dump(output, stream, separators=(",", ":"))
            stream.write("\n")
            stream.flush()
            os.fsync(stream.fileno())
        os.chmod(temporary, 0o600)
        os.replace(temporary, output_path)
    except Exception:
        try:
            os.unlink(temporary)
        except OSError:
            pass
        raise


if __name__ == "__main__":
    if len(sys.argv) != 5:
        raise SystemExit(
            "usage: merge_sze_td_runtime.py BASE CREDENTIALS SYSTEM OUTPUT")
    try:
        merge(sys.argv[1], sys.argv[2], sys.argv[3], sys.argv[4])
    except (KeyError, OSError, ValueError) as error:
        raise SystemExit("sze_td_config_error: {}".format(error))
