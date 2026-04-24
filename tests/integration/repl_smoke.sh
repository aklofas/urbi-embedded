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

# --- -f / positional file ---
test_case
f="$TMPDIR_LOCAL/prog1.urb"
printf '1 + 2 |\n' > "$f"
out=$("$URBI" "$f")
rc=$?
if [ "$rc" -eq 0 ] && [ -z "$out" ]; then
    ok 'positional file with "1 + 2 |" runs, prints nothing, exit 0'
else
    fail "positional file: rc=$rc, out='$out'"
fi

test_case
out=$("$URBI" -f "$f")
rc=$?
if [ "$rc" -eq 0 ] && [ -z "$out" ]; then
    ok '-f file with "1 + 2 |" runs, prints nothing, exit 0'
else
    fail "-f file: rc=$rc, out='$out'"
fi

# --- multi-statement file: runs all, prints nothing ---
test_case
f2="$TMPDIR_LOCAL/prog2.urb"
printf '1 + 2 |\n3 * 4 |\n' > "$f2"
out=$("$URBI" "$f2")
rc=$?
if [ "$rc" -eq 0 ] && [ -z "$out" ]; then
    ok 'multi-statement file runs, prints nothing, exit 0'
else
    fail "multi-statement file: rc=$rc, out='$out'"
fi

# --- file not found ---
test_case
out=$("$URBI" "$TMPDIR_LOCAL/does-not-exist.urb" 2>&1 >/dev/null)
rc=$?
if [ "$rc" -eq 2 ] && printf '%s\n' "$out" | grep -q '^urbi:'; then
    ok 'file-not-found -> rc=2, urbi: on stderr'
else
    fail "file-not-found: rc=$rc, out='$out'"
fi

# --- -f missing argument ---
test_case
out=$("$URBI" -f 2>&1 >/dev/null)
rc=$?
if [ "$rc" -eq 2 ]; then
    ok '-f with no path -> rc=2'
else
    fail "-f (no path): rc=$rc"
fi

# --- piped stdin as script ---
test_case
out=$(printf '1 + 2 |\n' | "$URBI")
rc=$?
if [ "$rc" -eq 0 ] && [ -z "$out" ]; then
    ok 'piped stdin (non-tty) runs as script, prints nothing'
else
    fail "piped stdin: rc=$rc, out='$out'"
fi

# --- -f with a malformed file ---
test_case
f3="$TMPDIR_LOCAL/bad.urb"
printf '0xG |\n' > "$f3"
out=$("$URBI" -f "$f3" 2>&1 >/dev/null)
rc=$?
if [ "$rc" -eq 1 ] && printf '%s\n' "$out" | grep -q '^urbi:'; then
    ok '-f with lex error -> rc=1'
else
    fail "-f malformed: rc=$rc, out='$out'"
fi

# --- --dump-bytecode with -e ---
test_case
out=$("$URBI" --dump-bytecode -e "1 + 2")
rc=$?
if [ "$rc" -eq 0 ] \
    && printf '%s\n' "$out" | grep -q 'LOADK' \
    && printf '%s\n' "$out" | grep -q 'ADD' \
    && printf '%s\n' "$out" | grep -q 'RET'; then
    ok '--dump-bytecode -e "1 + 2" prints LOADK + ADD + RET'
else
    fail "--dump-bytecode -e: rc=$rc, out='$out'"
fi

# --- --dump-bytecode with positional file ---
test_case
f="$TMPDIR_LOCAL/dump.urb"
printf '1 + 2 |\n' > "$f"
out=$("$URBI" --dump-bytecode "$f")
rc=$?
if [ "$rc" -eq 0 ] && printf '%s\n' "$out" | grep -q 'LOADK'; then
    ok '--dump-bytecode <file> prints bytecode'
else
    fail "--dump-bytecode <file>: rc=$rc, out='$out'"
fi

# --- --dump-bytecode with -i is a usage error ---
test_case
out=$("$URBI" --dump-bytecode -i 2>&1)
rc=$?
if [ "$rc" -eq 2 ] && printf '%s\n' "$out" | grep -q 'requires'; then
    ok '--dump-bytecode -i -> rc=2, "requires" in stderr'
else
    fail "--dump-bytecode -i: rc=$rc, out='$out'"
fi

# --- --dump-bytecode with compile error ---
test_case
out=$("$URBI" --dump-bytecode -e "0xG" 2>&1 >/dev/null)
rc=$?
if [ "$rc" -eq 1 ] && printf '%s\n' "$out" | grep -q '^urbi:'; then
    ok '--dump-bytecode + lex error -> rc=1'
else
    fail "--dump-bytecode error: rc=$rc, out='$out'"
fi

# --- --dump-bytecode with no -e or file ---
test_case
out=$("$URBI" --dump-bytecode 2>&1 >/dev/null)
rc=$?
if [ "$rc" -eq 2 ]; then
    ok '--dump-bytecode alone -> rc=2'
else
    fail "--dump-bytecode alone: rc=$rc"
fi

printf '\n%d/%d tests passed\n' "$((TOTAL - FAIL))" "$TOTAL"
[ "$FAIL" -eq 0 ]
