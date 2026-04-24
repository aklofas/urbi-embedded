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

# 1. Partition fixture into inputs (to feed REPL) and expected (to diff).
awk '
    /^\[/              { print > expected; next }
    /^[[:space:]]*#/   { next }
    /^[[:space:]]*$/   { next }
                       { print > inputs }
' expected="$TMPDIR_LOCAL/expected.raw" \
  inputs="$TMPDIR_LOCAL/inputs.txt" \
  "$CHK"

# 2. Normalize expected: strip the [...] frame prefix + single trailing space.
sed -E 's/^\[[^]]*\] //' \
    < "$TMPDIR_LOCAL/expected.raw" \
    > "$TMPDIR_LOCAL/expected.norm"

# 3. Drive the REPL once with all inputs; merge stderr to catch leakage.
"$URBI" -i < "$TMPDIR_LOCAL/inputs.txt" \
    > "$TMPDIR_LOCAL/actual.raw" 2>&1

# 4. Normalize actual using the same rule.
sed -E 's/^\[[^]]*\] //' \
    < "$TMPDIR_LOCAL/actual.raw" \
    > "$TMPDIR_LOCAL/actual.norm"

# 5. Diff normalized sides.
if diff -u "$TMPDIR_LOCAL/expected.norm" "$TMPDIR_LOCAL/actual.norm"; then
    printf 'PASS: %s\n' "$CHK"
    exit 0
else
    printf 'FAIL: %s\n' "$CHK" >&2
    exit 1
fi
