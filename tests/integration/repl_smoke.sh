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

# --- -i basic happy paths (stdin piped, no tty) ---
test_case
out=$(printf '1 + 2\n' | "$URBI" -i)
rc=$?
if [ "$rc" -eq 0 ] && printf '%s\n' "$out" | grep -qE '^\[[0-9]{8}\] 3$'; then
    ok '-i "1 + 2" prints [TTTTTTTT] 3'
else
    fail "-i 1+2: rc=$rc, out='$out'"
fi

test_case
out=$(printf '5 / 2\n' | "$URBI" -i)
rc=$?
if [ "$rc" -eq 0 ] && printf '%s\n' "$out" | grep -qE '^\[[0-9]{8}\] 2\.5$'; then
    ok '-i "5 / 2" prints [TTTTTTTT] 2.5'
else
    fail "-i 5/2: rc=$rc, out='$out'"
fi

test_case
out=$(printf '1 + 2\n3 * 4\n' | "$URBI" -i)
rc=$?
lines=$(printf '%s\n' "$out" | grep -cE '^\[[0-9]{8}\] ')
if [ "$rc" -eq 0 ] && [ "$lines" -eq 2 ]; then
    ok '-i two lines -> two frames'
else
    fail "-i multi-line: rc=$rc, lines=$lines, out='$out'"
fi

# --- -i error recovery: error on line 1, value on line 2 ---
test_case
out=$(printf '1 +\n1 + 2\n' | "$URBI" -i)
rc=$?
err_lines=$(printf '%s\n' "$out" | grep -cE '^\[[0-9]{8}\] !!!')
val_lines=$(printf '%s\n' "$out" | grep -cE '^\[[0-9]{8}\] 3$')
if [ "$rc" -eq 0 ] && [ "$err_lines" -eq 1 ] && [ "$val_lines" -eq 1 ]; then
    ok '-i recovers from error line, evaluates next line'
else
    fail "-i recovery: rc=$rc, err=$err_lines, val=$val_lines"
fi

# --- -i EOF on empty stdin ---
test_case
out=$("$URBI" -i < /dev/null)
rc=$?
if [ "$rc" -eq 0 ] && [ -z "$out" ]; then
    ok '-i with /dev/null stdin exits cleanly'
else
    fail "-i /dev/null: rc=$rc, out='$out'"
fi

# --- -i with lex error ---
test_case
out=$(printf '0xG\n' | "$URBI" -i)
rc=$?
if [ "$rc" -eq 0 ] && printf '%s\n' "$out" | grep -qE '^\[[0-9]{8}\] !!!'; then
    ok '-i lex error -> framed !!! on stdout'
else
    fail "-i lex error: rc=$rc, out='$out'"
fi

# --- REPL realm: cross-line shared-proto access ---
# v0.9.1 breaking change: Object (and the other 14 builtin atom protos)
# became read-only.  The mutable cross-session shared proto is now Global
# per spec §4.1.  This test was the canonical v0.9.0 demonstration of the
# Object.x = ... idiom; migrated to Global.x = ... at v0.9.1.
test_case
out=$(printf 'Global.foo = 42\nGlobal.foo\n' | "$URBI" -i)
rc=$?
if [ "$rc" -eq 0 ] && printf '%s\n' "$out" | grep -qE '^\[[0-9]{8}\] 42$'; then
    ok 'Global.foo shared across REPL lines (REPL realm end-to-end)'
else
    fail "Global.foo not visible across REPL lines: rc=$rc, out='$out'"
fi

# --- REPL realm: Object is mutable post-D5 (v0.10.11 Cat. E ratify) ---
# D5 removed Object from the readonly cohort; `Object.foo = 42` must
# now succeed and the value round-trip.  Lobby stays frozen (separate
# gate not included here — covered by tests/chk/repl/lobby_readonly.chk).
test_case
out=$(printf 'Object.foo = 42; Object.foo\n' | "$URBI" -i)
rc=$?
if [ "$rc" -eq 0 ] && printf '%s\n' "$out" | grep -q '42'; then
    ok 'Object.x = ... succeeds (unfrozen at v0.10.11 D5)'
else
    fail "Object.foo mutation failed post-D5: rc=$rc, out='$out'"
fi

printf '\n%d/%d tests passed\n' "$((TOTAL - FAIL))" "$TOTAL"
[ "$FAIL" -eq 0 ]
