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

printf '\n%d/%d tests passed\n' "$((TOTAL - FAIL))" "$TOTAL"
[ "$FAIL" -eq 0 ]
