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

# --- -e happy paths ---
test_case
out=$("$URBI" -e "1 + 2")
rc=$?
if [ "$rc" -eq 0 ] && [ "$out" = "3" ]; then
    ok '-e "1 + 2" prints "3"'
else
    fail "-e \"1 + 2\": rc=$rc, out='$out'"
fi

test_case
out=$("$URBI" -e "2 * 3")
rc=$?
if [ "$rc" -eq 0 ] && [ "$out" = "6" ]; then
    ok '-e "2 * 3" prints "6"'
else
    fail "-e \"2 * 3\": rc=$rc, out='$out'"
fi

test_case
out=$("$URBI" -e "5 / 2")
rc=$?
if [ "$rc" -eq 0 ] && [ "$out" = "2.5" ]; then
    ok '-e "5 / 2" prints "2.5" (DIV -> Float)'
else
    fail "-e \"5 / 2\": rc=$rc, out='$out'"
fi

test_case
out=$("$URBI" -e "1 + 5 / 2")
rc=$?
if [ "$rc" -eq 0 ] && [ "$out" = "3.5" ]; then
    ok '-e "1 + 5 / 2" prints "3.5" (Int+Float promotion)'
else
    fail "-e \"1 + 5 / 2\": rc=$rc, out='$out'"
fi

# --- -e error paths ---
test_case
out=$("$URBI" -e "0xG" 2>&1 >/dev/null)
rc=$?
if [ "$rc" -eq 1 ] && printf '%s\n' "$out" | grep -q '^urbi:'; then
    ok '-e "0xG" lex error -> rc=1, urbi: on stderr'
else
    fail "-e \"0xG\": rc=$rc, out='$out'"
fi

test_case
out=$("$URBI" -e "1 +" 2>&1 >/dev/null)
rc=$?
if [ "$rc" -eq 1 ] && printf '%s\n' "$out" | grep -q '^urbi:'; then
    ok '-e "1 +" parse error -> rc=1, urbi: on stderr'
else
    fail "-e \"1 +\": rc=$rc, out='$out'"
fi

# --- -e missing expression ---
test_case
out=$("$URBI" -e 2>&1 >/dev/null)
rc=$?
if [ "$rc" -eq 2 ]; then
    ok '-e with no expr -> rc=2'
else
    fail "-e (no expr): rc=$rc"
fi

printf '\n%d/%d tests passed\n' "$((TOTAL - FAIL))" "$TOTAL"
[ "$FAIL" -eq 0 ]
