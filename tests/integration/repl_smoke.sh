#!/bin/sh
# SPDX-License-Identifier: BSD-3-Clause
# urbi REPL integration tests.  POSIX sh.
# Argument: path to the urbi binary.

set -u

if [ "$#" -ne 1 ]; then
    printf 'usage: %s <path-to-urbi-binary>\n' "$0" >&2
    exit 2
fi

URBI="$1"

if [ ! -x "$URBI" ]; then
    printf 'error: urbi binary not found or not executable: %s\n' "$URBI" >&2
    exit 2
fi

FAIL=0
TOTAL=0

fail() {
    FAIL=$((FAIL + 1))
    printf '  FAIL: %s\n' "$1"
}

ok() {
    printf '  ok:   %s\n' "$1"
}

test_case() {
    TOTAL=$((TOTAL + 1))
}

# Trap cleanup of tempfiles.
TMPDIR_LOCAL=$(mktemp -d)
trap 'rm -rf "$TMPDIR_LOCAL"' EXIT

printf '=== urbi REPL integration tests ===\n'

# (cases go here; added in later tasks)

# --- --version ---
test_case
out=$("$URBI" --version 2>&1)
rc=$?
if [ "$rc" -eq 0 ] && printf '%s\n' "$out" | grep -qE '^urbi '; then
    ok '--version prints "urbi ..." and exits 0'
else
    fail "--version: rc=$rc, out='$out'"
fi

test_case
out=$("$URBI" -V 2>&1)
rc=$?
if [ "$rc" -eq 0 ] && printf '%s\n' "$out" | grep -qE '^urbi '; then
    ok '-V prints "urbi ..." and exits 0'
else
    fail "-V: rc=$rc, out='$out'"
fi

# --- --help ---
test_case
out=$("$URBI" --help 2>&1)
rc=$?
if [ "$rc" -eq 0 ] && printf '%s\n' "$out" | grep -q 'Usage:'; then
    ok '--help prints usage and exits 0'
else
    fail "--help: rc=$rc"
fi

test_case
out=$("$URBI" -h 2>&1)
rc=$?
if [ "$rc" -eq 0 ] && printf '%s\n' "$out" | grep -q 'Usage:'; then
    ok '-h prints usage and exits 0'
else
    fail "-h: rc=$rc"
fi

# --- unknown flag ---
test_case
out=$("$URBI" --nope 2>&1)
rc=$?
if [ "$rc" -eq 2 ]; then
    ok 'unknown flag exits 2'
else
    fail "unknown flag: rc=$rc, expected 2"
fi

printf '\n%d/%d tests passed\n' "$((TOTAL - FAIL))" "$TOTAL"
[ "$FAIL" -eq 0 ]
