#!/bin/sh
# check-trace-decode — decoder unit test. Skips cleanly if python3 is missing.
set -e
if ! command -v python3 >/dev/null 2>&1; then
    echo "check-trace-decode: python3 not found — SKIP"
    exit 0
fi
python3 tests/tools/test_trace_decode.py
