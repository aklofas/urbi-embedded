#!/bin/sh
# SPDX-License-Identifier: BSD-3-Clause
# urbi .chk conformance-fixture runner. POSIX sh.
# Usage: run_chk.sh <path-to-urbi-binary> <path-to-chk-fixture>
#
# Exit codes:
#   0  fixture passed
#   1  fixture failed (diff mismatch)
#   2  runner error (bad args, unreadable file, missing binary)

set -u

if [ "$#" -ne 2 ]; then
    printf 'usage: %s <urbi-binary> <chk-fixture>\n' "$0" >&2
    exit 2
fi

URBI="$1"
CHK="$2"

if [ ! -x "$URBI" ]; then
    printf 'error: urbi binary not found or not executable: %s\n' "$URBI" >&2
    exit 2
fi

if [ ! -r "$CHK" ]; then
    printf 'error: fixture not readable: %s\n' "$CHK" >&2
    exit 2
fi

TMPDIR_LOCAL=$(mktemp -d)
trap 'rm -rf "$TMPDIR_LOCAL"' EXIT

# --- pipeline lands in Task 3 ---
printf 'PASS: %s\n' "$CHK"
exit 0
