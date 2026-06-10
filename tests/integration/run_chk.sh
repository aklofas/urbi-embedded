#!/bin/sh
# SPDX-License-Identifier: BSD-3-Clause
# urbi .chk conformance-fixture runner. POSIX sh.
# Usage: run_chk.sh <path-to-urbi-binary> <path-to-chk-fixture>
#
# Exit codes (refactor-3 CHK-01/02/03/04 honesty contract; pinned by
# tests/integration/test_run_chk_runner.sh):
#   0  fixture passed
#   1  fixture failed (diff mismatch, unexpected exit status, or timeout)
#   2  runner error (bad args, unreadable file, missing binary/tool)
#   3  fixture skipped (tunables preset mismatch)
#   4  annotated placeholder (`# blocked:` / `# deferred:` / `# dropped:`
#      header per docs/release/chk-deferred-taxonomy.md — never executed)
#   5  VACUOUS (empty inputs + empty expected, no placeholder annotation)
#
# Directives (comment lines; same style as `# tunables:` / `## host:`):
#   # tunables: <preset>   — run only when URBI_BUILD_PRESET matches
#   ## host: <op>          — drive via chk-host-driver instead of `urbi -i`
#   ## exit: <n>           — expected process exit status (default 0)
#   ## timeout: <seconds>  — per-fixture timeout (default 30)

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

command -v timeout >/dev/null 2>&1 || {
    printf 'error: coreutils `timeout` not found in PATH (required by the chk runner since refactor-3 CHK-03)\n' >&2
    exit 2
}

# Parse tunables header. If present and doesn't match current build preset,
# skip. Pattern: '# tunables: <preset>' on its own line.  SKIP is exit 3
# (refactor-3 CHK-04) so aggregating targets can tally it distinctly; the
# preset-gated targets (test-chk-ros etc.) treat any nonzero rc as failure.
required_preset=$(grep -E '^[[:space:]]*#[[:space:]]*tunables:[[:space:]]*' "$CHK" | \
                   head -1 | \
                   sed -E 's/^[[:space:]]*#[[:space:]]*tunables:[[:space:]]*//; s/[[:space:]]*$//')
if [ -n "$required_preset" ]; then
    current_preset="${URBI_BUILD_PRESET:-default}"
    if [ "$required_preset" != "$current_preset" ]; then
        printf 'SKIP %s (requires tunables: %s; current: %s)\n' \
               "$CHK" "$required_preset" "$current_preset"
        exit 3
    fi
fi

# Placeholder annotation (docs/release/chk-deferred-taxonomy.md buckets).
# A placeholder fixture is a specification record, not a test — it never
# executes.  Exit 4 lets the suite tally placeholders separately from real
# passes (refactor-3 CHK-01).
placeholder=$(awk '/^#/ { print; next } { exit }' "$CHK" | \
              grep -E '^# (blocked|deferred|dropped):' | head -1)
if [ -n "$placeholder" ]; then
    printf 'PLACEHOLDER: %s (%s)\n' "$CHK" "$placeholder"
    exit 4
fi

# Expected exit status (refactor-3 CHK-02).  Default 0; a fixture that
# legitimately exits nonzero declares it via '## exit: <n>'.
expected_exit=$(grep -E '^[[:space:]]*##[[:space:]]*exit:[[:space:]]*' "$CHK" | \
                 head -1 | \
                 sed -E 's/^[[:space:]]*##[[:space:]]*exit:[[:space:]]*//; s/[[:space:]]*$//')
[ -n "$expected_exit" ] || expected_exit=0
case "$expected_exit" in
    ''|*[!0-9]*)
        echo "run_chk: invalid '## exit:' value in $CHK" >&2
        exit 2
        ;;
esac

# Per-fixture timeout (refactor-3 CHK-03).  coreutils `timeout` reports
# expiry as exit 124.
timeout_s=$(grep -E '^[[:space:]]*##[[:space:]]*timeout:[[:space:]]*' "$CHK" | \
             head -1 | \
             sed -E 's/^[[:space:]]*##[[:space:]]*timeout:[[:space:]]*//; s/[[:space:]]*$//')
[ -n "$timeout_s" ] || timeout_s=30
case "$timeout_s" in
    ''|*[!0-9]*)
        echo "run_chk: invalid '## timeout:' value in $CHK" >&2
        exit 2
        ;;
esac

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

    # refactor-3 CHK-01 (host path): a host-driver fixture with no expected
    # lines diffs against an empty file — it can never fail and proves
    # nothing.  Same vacuous contract as the script path below.
    if [ ! -s "$TMPDIR_LOCAL/expected.raw" ]; then
        printf 'VACUOUS: %s (host-driver; empty expected output, no placeholder annotation — annotate per docs/release/chk-deferred-taxonomy.md or activate; refactor-3 CHK-01)\n' \
               "$CHK" >&2
        exit 5
    fi

    sed -E 's/^\[[^]]*\] //' \
        < "$TMPDIR_LOCAL/expected.raw" \
        > "$TMPDIR_LOCAL/expected.norm"

    timeout "$timeout_s" "$DRIVER" "$CHK" > "$TMPDIR_LOCAL/actual.raw" 2>&1
    rc=$?
    if [ "$rc" -eq 124 ]; then
        printf 'TIMEOUT: %s (host-driver; no exit within %ss)\n' \
               "$CHK" "$timeout_s" >&2
        exit 1
    fi
    sed -E 's/^\[[^]]*\] //' \
        < "$TMPDIR_LOCAL/actual.raw" \
        > "$TMPDIR_LOCAL/actual.norm"

    if ! diff -u "$TMPDIR_LOCAL/expected.norm" "$TMPDIR_LOCAL/actual.norm"; then
        printf 'FAIL: %s (host-driver)\n' "$CHK" >&2
        exit 1
    fi
    if [ "$rc" -ne "$expected_exit" ]; then
        printf 'FAIL (exit %s): %s (host-driver; diff matched but expected exit %s)\n' \
               "$rc" "$CHK" "$expected_exit" >&2
        exit 1
    fi
    printf 'PASS: %s (host-driver)\n' "$CHK"
    exit 0
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

# refactor-3 CHK-01: a fixture with empty inputs AND empty expected output
# diffs two empty files — it can never fail and proves nothing.  Annotated
# placeholders were dispatched above (exit 4); reaching here unannotated is
# a corpus bug.
if [ ! -s "$TMPDIR_LOCAL/inputs.txt" ] && [ ! -s "$TMPDIR_LOCAL/expected.raw" ]; then
    printf 'VACUOUS: %s (empty inputs + empty expected, no placeholder annotation — annotate per docs/release/chk-deferred-taxonomy.md or activate; refactor-3 CHK-01)\n' \
           "$CHK" >&2
    exit 5
fi

# 2. Normalize expected: strip the [...] frame prefix + single trailing space.
sed -E 's/^\[[^]]*\] //' \
    < "$TMPDIR_LOCAL/expected.raw" \
    > "$TMPDIR_LOCAL/expected.norm"

# 3. Drive the REPL once with all inputs; merge stderr to catch leakage.
#    refactor-3 CHK-02/03: capture the exit status; bound the run.
timeout "$timeout_s" "$URBI" -i < "$TMPDIR_LOCAL/inputs.txt" \
    > "$TMPDIR_LOCAL/actual.raw" 2>&1
rc=$?
if [ "$rc" -eq 124 ]; then
    printf 'TIMEOUT: %s (no exit within %ss)\n' "$CHK" "$timeout_s" >&2
    exit 1
fi

# 4. Normalize actual using the same rule.
sed -E 's/^\[[^]]*\] //' \
    < "$TMPDIR_LOCAL/actual.raw" \
    > "$TMPDIR_LOCAL/actual.norm"

# 5. Diff normalized sides, then verify the exit status.  A crash AFTER
#    producing the right output is still a failure (refactor-3 CHK-02).
if ! diff -u "$TMPDIR_LOCAL/expected.norm" "$TMPDIR_LOCAL/actual.norm"; then
    printf 'FAIL: %s\n' "$CHK" >&2
    exit 1
fi
if [ "$rc" -ne "$expected_exit" ]; then
    printf 'FAIL (exit %s): %s (diff matched but expected exit %s)\n' \
           "$rc" "$CHK" "$expected_exit" >&2
    exit 1
fi
printf 'PASS: %s\n' "$CHK"
exit 0
