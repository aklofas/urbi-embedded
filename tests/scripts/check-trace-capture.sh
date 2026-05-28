#!/bin/sh
# check-trace-capture — capture a tiny run with the URBI_TRACE=1 CLI, decode it,
# assert valid Chrome Trace JSON. Skips cleanly if python3 is missing.
# Assumes `make urbi-trace` has built build/host-trace/urbi (the make target
# test-trace-capture depends on urbi-trace).
set -e
if ! command -v python3 >/dev/null 2>&1; then
    echo "check-trace-capture: python3 not found — SKIP"
    exit 0
fi
BIN=build/host-trace/urbi
[ -x "$BIN" ] || { echo "check-trace-capture: $BIN not built (run: make urbi-trace)"; exit 1; }
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
"$BIN" --trace=sched:debug,gc:info --trace-out="$TMP/t.bin" \
    -e 'var i = 0; while (i < 3) { i = i + 1 }' >/dev/null
[ -s "$TMP/t.bin" ] || { echo "check-trace-capture: empty dump"; exit 1; }
python3 tools/urbi-trace-decode.py "$TMP/t.bin" --out "$TMP/t.json"
python3 - "$TMP/t.json" <<'PY'
import json, sys
doc = json.load(open(sys.argv[1]))
assert "traceEvents" in doc, "no traceEvents"
assert doc["metadata"]["urbt"]["record_bytes"] == 32, doc["metadata"]["urbt"]
n = doc["metadata"]["summary"]["parsed"]
assert n >= 1, "no records decoded"
print("check-trace-capture: decoded", n, "records — OK")
PY
