#!/usr/bin/env python3

import json
import os
import sys
import tempfile


if len(sys.argv) != 3:
    raise SystemExit("usage: extract_td_credentials.py PRIVATE_JSON OUTPUT_JSON")
source, output = sys.argv[1:]
data = json.load(open(source, encoding="utf-8"))
if not isinstance(data.get("td"), dict) or not data["td"]:
    raise SystemExit("private config has no td object")
parent = os.path.dirname(os.path.abspath(output))
fd, temporary = tempfile.mkstemp(prefix=".td-credentials-", dir=parent)
try:
    with os.fdopen(fd, "w", encoding="utf-8") as stream:
        json.dump({"td": data["td"]}, stream, indent=2)
        stream.write("\n")
        stream.flush()
        os.fsync(stream.fileno())
    os.chmod(temporary, 0o600)
    os.replace(temporary, output)
except Exception:
    try:
        os.unlink(temporary)
    except OSError:
        pass
    raise
