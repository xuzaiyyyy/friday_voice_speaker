#!/usr/bin/env bash
set -u

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
cd "${ROOT_DIR}" || exit 1

status=0

for ui in *.ui; do
    [ -f "${ui}" ] || continue
    printf '[ui] %s ... ' "${ui}"
    if ! python3 - "${ui}" <<'PY'
import pathlib
import sys
import xml.etree.ElementTree as ET

path = pathlib.Path(sys.argv[1])
data = path.read_bytes()
try:
    text = data.decode("utf-8-sig")
except UnicodeDecodeError as exc:
    print(f"BAD utf-8: {exc}")
    sys.exit(1)

try:
    ET.fromstring(text)
except ET.ParseError as exc:
    print(f"BAD xml: {exc}")
    sys.exit(1)

print("OK")
PY
    then
        status=1
        continue
    fi

    if command -v uic >/dev/null 2>&1; then
        tmp="/tmp/xiaoman_ui_check_$$.h"
        if ! uic "${ui}" -o "${tmp}" >/dev/null 2>&1; then
            echo "[uic] ${ui} failed"
            uic "${ui}" -o "${tmp}" >/dev/null
            status=1
        fi
        rm -f "${tmp}"
    fi
done

exit "${status}"
