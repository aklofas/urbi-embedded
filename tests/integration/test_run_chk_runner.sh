#!/usr/bin/env bash
# SPDX-License-Identifier: BSD-3-Clause
# test_run_chk_runner.sh — meta-test pinning run_chk.sh's outcome contract
# (refactor-3 CHK-01/02/03/04).  Uses stub "urbi" binaries so the runner's
# exit codes are tested independently of the real VM:
#   0 PASS   1 FAIL/TIMEOUT   2 runner error   3 SKIP (preset)
#   4 annotated placeholder   5 VACUOUS (unannotated)
set -u
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
RUNNER="$ROOT/tests/integration/run_chk.sh"
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
fails=0
expect_rc() { # <desc> <expected-rc> <actual-rc>
    if [ "$2" -ne "$3" ]; then
        echo "FAIL: $1 — expected rc=$2 got rc=$3"
        fails=$((fails + 1))
    else
        echo "ok: $1 (rc=$3)"
    fi
}

# Stub: echoes the canonical frame for input `1 + 2;` then exits 0.
cat > "$TMP/urbi-ok" <<'EOF'
#!/bin/sh
cat > /dev/null
printf '[00000000] 3\n'
exit 0
EOF
# Stub: correct output then crash — the CHK-02 crash-after-output case.
cat > "$TMP/urbi-crash" <<'EOF'
#!/bin/sh
cat > /dev/null
printf '[00000000] 3\n'
exit 139
EOF
# Stub: correct output then hangs — the CHK-03 case.
cat > "$TMP/urbi-hang" <<'EOF'
#!/bin/sh
cat > /dev/null
printf '[00000000] 3\n'
sleep 1000
EOF
chmod +x "$TMP/urbi-ok" "$TMP/urbi-crash" "$TMP/urbi-hang"

cat > "$TMP/basic.chk" <<'EOF'
1 + 2;
[00000000] 3
EOF

cat > "$TMP/exit139.chk" <<'EOF'
## exit: 139
1 + 2;
[00000000] 3
EOF

cat > "$TMP/quick-timeout.chk" <<'EOF'
## timeout: 2
1 + 2;
[00000000] 3
EOF

cat > "$TMP/vacuous.chk" <<'EOF'
# Covers: nothing — deliberately vacuous (runner meta-test)
EOF

cat > "$TMP/placeholder.chk" <<'EOF'
# blocked: <v1.0-rc> — runner meta-test placeholder
EOF

cat > "$TMP/gated.chk" <<'EOF'
# tunables: ros
1 + 2;
[00000000] 3
EOF

"$RUNNER" "$TMP/urbi-ok" "$TMP/basic.chk" >/dev/null 2>&1
expect_rc "matching output + exit 0 PASSes"      0 $?
"$RUNNER" "$TMP/urbi-crash" "$TMP/basic.chk" >/dev/null 2>&1
expect_rc "crash-after-output FAILs (CHK-02)"    1 $?
"$RUNNER" "$TMP/urbi-crash" "$TMP/exit139.chk" >/dev/null 2>&1
expect_rc "'## exit: 139' makes the crash expected" 0 $?
"$RUNNER" "$TMP/urbi-hang" "$TMP/quick-timeout.chk" >/dev/null 2>&1
expect_rc "'## timeout: 2' TIMEOUTs the hang (CHK-03)" 1 $?
"$RUNNER" "$TMP/urbi-ok" "$TMP/vacuous.chk" >/dev/null 2>&1
expect_rc "unannotated vacuous fixture is VACUOUS (CHK-01)" 5 $?
"$RUNNER" "$TMP/urbi-ok" "$TMP/placeholder.chk" >/dev/null 2>&1
expect_rc "annotated placeholder exits 4 (CHK-01)" 4 $?
URBI_BUILD_PRESET=default "$RUNNER" "$TMP/urbi-ok" "$TMP/gated.chk" >/dev/null 2>&1
expect_rc "preset mismatch SKIPs with rc=3 (CHK-04)" 3 $?

if [ "$fails" -gt 0 ]; then
    echo "test_run_chk_runner: $fails contract check(s) FAILED"
    exit 1
fi
echo "test_run_chk_runner: all 7 runner-contract checks PASS"
