#!/bin/sh
# SPDX-License-Identifier: BSD-3-Clause
# tests/scripts/check_wire_format_determinism.sh
# Run capture_wire_format_hashes.sh twice and assert identical output.
#
# Pre-v0.5.7.1, the capture script used `mktemp /tmp/urbi_chk_XXXXXX.u` per
# fixture; the random tmp path got embedded in the wire format's source_name
# field, producing non-deterministic per-fixture hashes across runs. The fix
# pipes source via stdin (`-f -`) so source_name is the stable string "-".
# This test guards against regressions.
set -eu
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
A=$(mktemp /tmp/urbi_wf_A_XXXXXX.txt)
B=$(mktemp /tmp/urbi_wf_B_XXXXXX.txt)
trap 'rm -f "$A" "$B"' EXIT

bash "$ROOT/tests/scripts/capture_wire_format_hashes.sh" "$A" > /dev/null
bash "$ROOT/tests/scripts/capture_wire_format_hashes.sh" "$B" > /dev/null

if ! diff -q "$A" "$B" > /dev/null; then
    echo "FAIL: capture_wire_format_hashes.sh produced different output across two runs" >&2
    diff "$A" "$B" | head -10 >&2
    exit 1
fi
echo "OK: capture_wire_format_hashes.sh deterministic"
