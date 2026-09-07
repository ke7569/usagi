#!/usr/bin/env bash
set -euo pipefail

source /etc/os-release
[[ "${ID:-}" == centos && "${VERSION_ID:-}" == 7* ]] || {
  echo "expected CentOS 7.x" >&2
  exit 1
}
[[ "$(gcc -dumpversion)" == 4.8.5 ]] || {
  echo "expected gcc 4.8.5" >&2
  exit 1
}
[[ "$(g++ -dumpversion)" == 4.8.5 ]] || {
  echo "expected g++ 4.8.5" >&2
  exit 1
}
ldd --version 2>&1 | grep -F '2.17' >/dev/null
command -v cmake3 >/dev/null
RUNTIME_ROOT="${T0_DEEPWIN_RUNTIME_ROOT:-${RUNTIME_ROOT:-}}"
[[ -n "$RUNTIME_ROOT" ]] || {
  echo "set T0_DEEPWIN_RUNTIME_ROOT (or RUNTIME_ROOT) to the validated Deepwin runtime root" >&2
  exit 1
}
[[ -d "$RUNTIME_ROOT" ]] || {
  echo "Deepwin runtime root does not exist: $RUNTIME_ROOT" >&2
  exit 1
}
for path in \
  toolchain/deepwin_include/IWCStrategy.h \
  toolchain/boost_include/boost/version.hpp \
  toolchain/python_include/Python.h \
  runtime_so/deepwin_core/lib/wingchun/libwingchunstrategy.so \
  runtime_so/deepwin_core/lib/wingchun/libwingchunmd.so; do
  if [[ "$path" == runtime_so/* ]]; then
    candidate="$RUNTIME_ROOT/${path#runtime_so/}"
  else
    candidate="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/$path"
  fi
  [[ -e "$candidate" ]] || {
    echo "missing bundled dependency: $path" >&2
    exit 1
  }
done
echo "SZE build environment: PASS"
