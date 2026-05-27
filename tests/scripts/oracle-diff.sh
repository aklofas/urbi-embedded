#!/usr/bin/env bash
# oracle-diff.sh — diff our urbi binary's stdout against urbiforge oracle.
#
# v0.6.2 Wave 3 third-party sanity check on Gaps #1 (closure capture)
# and #4 (operator overload) where `.chk` corpus alone doesn't fully
# specify corner cases.  NOT a CI gate — run opt-in.
#
# urbiforge invocation pattern (discovered Phase 0 Task 4):
#   urbi-launch -s -- <script-path>
# wraps `liburbi.so` plugin-mode script engine.  URBI_ROOT must point at
# the install prefix; LD_LIBRARY_PATH must include $URBI_ROOT/lib.
#
# Output normalization for diff:
#   - Strip preamble ("new server ...", "URBI IS FINALLY READY ...",
#     and the "[NNN] *** Urbi version ..." banner).
#   - Replace timestamp prefixes "[NNNNNNNN]" with "[T]" so per-run
#     drift doesn't dominate the diff.
#
# Env:
#   URBI_ORACLE_ROOT  install prefix (default /tmp/urbi-oracle)
#   LEGACY_TESTS      path to aldebaran-urbi tests/2.x directory (required;
#                     falls back to ./legacy/repos/aldebaran-urbi/tests/2.x)
#
# Args (optional): one or more fixture paths relative to LEGACY_TESTS.
# When omitted, runs the default Wave 3 fixture set.
#
# Output: per-fixture `[ok]` / `[diff]` / `[skip]` lines on stdout;
# diff captures land at /tmp/oracle-diff-<basename>.log.
#
# Exit: 0 always (advisory, not a gate).
set -euo pipefail

URBI_ORACLE_ROOT="${URBI_ORACLE_ROOT:-/tmp/urbi-oracle}"
LEGACY_TESTS="${LEGACY_TESTS:-./legacy/repos/aldebaran-urbi/tests/2.x}"
OURS="./build/host/urbi"

ORACLE_BIN="$URBI_ORACLE_ROOT/bin/urbi-launch"

if [ ! -x "$ORACLE_BIN" ]; then
    echo "oracle-diff: $ORACLE_BIN not built; see Phase 0 Task 4 of v0.6.2 plan" >&2
    exit 1
fi
if [ ! -x "$OURS" ]; then
    echo "oracle-diff: $OURS not built; run 'make' first" >&2
    exit 1
fi
if [ ! -d "$LEGACY_TESTS" ]; then
    echo "oracle-diff: $LEGACY_TESTS not found; set LEGACY_TESTS env" >&2
    exit 1
fi

# Wrapper to run the oracle with the env it needs.
run_oracle() {
    URBI_ROOT="$URBI_ORACLE_ROOT" \
    LD_LIBRARY_PATH="$URBI_ORACLE_ROOT/lib" \
        timeout 30 "$ORACLE_BIN" -s -- "$@" 2>&1 || true
}

# Normalize for diff:
#   - drop "new server ..." line
#   - drop "URBI IS FINALLY READY ..." line
#   - drop the "[NNN] *** Urbi version ..." banner
#   - replace "[NNNNNNNN]" timestamps with "[T]"
normalize() {
    sed -E \
        -e '/^new server /d' \
        -e '/^URBI IS FINALLY READY/d' \
        -e '/^\[[0-9]+(:[a-z]+)?\] \*\*\* Urbi version/d' \
        -e 's/^\[[0-9]+(:[a-z]+)?\]/[T\1]/'
}

if [ "$#" -gt 0 ]; then
    FIXTURES=("$@")
else
    # Default Wave 3 fixture set — gaps #1 + #3 + #4
    FIXTURES=(
        "operators.chk" "operator-parens.chk" "edit-container.chk"
        "this.chk" "self.chk" "self-versus-locals.chk"
        "closure/closure.chk" "closure/scopes.chk" "closure/read.chk"
    )
fi

ok=0
diff_count=0
skip=0

for fix in "${FIXTURES[@]}"; do
    fix_path="$LEGACY_TESTS/$fix"
    if [ ! -f "$fix_path" ]; then
        echo "[skip] $fix (no such fixture)"
        skip=$((skip + 1))
        continue
    fi
    log="/tmp/oracle-diff-$(basename "$fix" .chk).log"
    oracle_out=$(run_oracle "$fix_path" | normalize)
    ours_out=$("$OURS" --batch "$fix_path" 2>&1 | normalize || true)
    if diff <(printf '%s\n' "$oracle_out") <(printf '%s\n' "$ours_out") > "$log"; then
        echo "[ok]   $fix"
        ok=$((ok + 1))
        rm -f "$log"
    else
        echo "[diff] $fix — see $log"
        diff_count=$((diff_count + 1))
    fi
done

echo "---"
echo "ok=$ok diff=$diff_count skip=$skip"
