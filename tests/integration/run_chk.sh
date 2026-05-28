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

# Parse tunables header. If present and doesn't match current build preset,
# skip (success). Pattern: '# tunables: <preset>' on its own line.
required_preset=$(grep -E '^[[:space:]]*#[[:space:]]*tunables:[[:space:]]*' "$CHK" | \
                   head -1 | \
                   sed -E 's/^[[:space:]]*#[[:space:]]*tunables:[[:space:]]*//; s/[[:space:]]*$//')
if [ -n "$required_preset" ]; then
    current_preset="${URBI_BUILD_PRESET:-default}"
    if [ "$required_preset" != "$current_preset" ]; then
        printf 'SKIP %s (requires tunables: %s; current: %s)\n' \
               "$CHK" "$required_preset" "$current_preset"
        exit 0
    fi
fi

TMPDIR_LOCAL=$(mktemp -d)
trap 'rm -rf "$TMPDIR_LOCAL"' EXIT

# Detect fixtures that carry `## host:` driver directives.  These need the
# C host-driver (multi-realm setup, urbi_step quiescence observation) which
# the single-pass `urbi -i` REPL path cannot express.  All other fixtures
# keep the existing REPL path completely unchanged.
#
# The driver binary lives alongside the urbi binary (same build dir), so each
# sanitizer variant picks up its own instrumented driver.
if grep -qE '^[[:space:]]*##[[:space:]]*host:' "$CHK"; then
    DRIVER="$(dirname "$URBI")/chk-host-driver"
    if [ ! -x "$DRIVER" ]; then
        printf 'error: chk-host-driver not found or not executable: %s\n' \
               "$DRIVER" >&2
        exit 2
    fi

    # Expected lines come from the same `[...]` frame the REPL path uses.
    grep -E '^\[' "$CHK" > "$TMPDIR_LOCAL/expected.raw" 2>/dev/null || true
    [ -f "$TMPDIR_LOCAL/expected.raw" ] || touch "$TMPDIR_LOCAL/expected.raw"
    sed -E 's/^\[[^]]*\] //' \
        < "$TMPDIR_LOCAL/expected.raw" \
        > "$TMPDIR_LOCAL/expected.norm"

    "$DRIVER" "$CHK" > "$TMPDIR_LOCAL/actual.raw" 2>&1
    sed -E 's/^\[[^]]*\] //' \
        < "$TMPDIR_LOCAL/actual.raw" \
        > "$TMPDIR_LOCAL/actual.norm"

    if diff -u "$TMPDIR_LOCAL/expected.norm" "$TMPDIR_LOCAL/actual.norm"; then
        printf 'PASS: %s (host-driver)\n' "$CHK"
        exit 0
    else
        printf 'FAIL: %s (host-driver)\n' "$CHK" >&2
        exit 1
    fi
fi

# 1. Partition fixture into inputs (to feed REPL) and expected (to diff).
awk '
    /^\[/              { print > expected; next }
    /^[[:space:]]*#/   { next }
    /^[[:space:]]*$/   { next }
                       { print > inputs }
' expected="$TMPDIR_LOCAL/expected.raw" \
  inputs="$TMPDIR_LOCAL/inputs.txt" \
  "$CHK"

# Guard against pathological fixtures (only comments, only expected,
# only inputs, or fully empty). touch creates the file if awk never
# fired the corresponding print > file branch.
[ -f "$TMPDIR_LOCAL/inputs.txt" ]  || touch "$TMPDIR_LOCAL/inputs.txt"
[ -f "$TMPDIR_LOCAL/expected.raw" ] || touch "$TMPDIR_LOCAL/expected.raw"

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
