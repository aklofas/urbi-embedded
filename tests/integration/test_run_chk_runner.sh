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
# Stub: wrong output, clean exit — the plain diff-mismatch FAIL case.
cat > "$TMP/urbi-wrong" <<'EOF'
#!/bin/sh
cat > /dev/null
printf '[00000000] 4\n'
exit 0
EOF
chmod +x "$TMP/urbi-ok" "$TMP/urbi-crash" "$TMP/urbi-hang" "$TMP/urbi-wrong"

# --- host-driver path stubs (BLD-CI-7) -----------------------------------
# `## host:` fixtures dispatch to $(dirname urbi)/chk-host-driver instead of
# `urbi -i`.  Each behavior needs its own driver binary, so give each case its
# own dir holding a placeholder `urbi` (only its dirname is used to locate the
# driver) plus a `chk-host-driver` with the desired behavior.
HD_CRASH="$TMP/hd-crash"; HD_HANG="$TMP/hd-hang"; HD_VACUOUS="$TMP/hd-vacuous"
mkdir -p "$HD_CRASH" "$HD_HANG" "$HD_VACUOUS"
for d in "$HD_CRASH" "$HD_HANG" "$HD_VACUOUS"; do
    printf '#!/bin/sh\nexit 0\n' > "$d/urbi"   # placeholder; host path never runs it
    chmod +x "$d/urbi"
done
# Driver: correct output then crash — host-path crash-after-output FAIL case.
cat > "$HD_CRASH/chk-host-driver" <<'EOF'
#!/bin/sh
printf '[00000000] 3\n'
exit 139
EOF
# Driver: correct output then hangs — host-path TIMEOUT-kill case.
cat > "$HD_HANG/chk-host-driver" <<'EOF'
#!/bin/sh
printf '[00000000] 3\n'
sleep 1000
EOF
# Driver: never reached — the vacuous case is caught before the driver runs;
# a well-behaved stub is enough to prove the driver would be available.
cat > "$HD_VACUOUS/chk-host-driver" <<'EOF'
#!/bin/sh
printf '[00000000] 3\n'
exit 0
EOF
chmod +x "$HD_CRASH/chk-host-driver" "$HD_HANG/chk-host-driver" "$HD_VACUOUS/chk-host-driver"

# host-driver fixtures: `## host:` selects the driver path.
cat > "$TMP/host-basic.chk" <<'EOF'
## host: quiescence
[00000000] 3
EOF
cat > "$TMP/host-timeout.chk" <<'EOF'
## host: quiescence
## timeout: 2
[00000000] 3
EOF
cat > "$TMP/host-vacuous.chk" <<'EOF'
## host: quiescence
EOF

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

cat > "$TMP/bad-exit-directive.chk" <<'EOF'
## exit: banana
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
"$RUNNER" "$TMP/urbi-wrong" "$TMP/basic.chk" >/dev/null 2>&1
expect_rc "wrong output FAILs (diff mismatch)"   1 $?
"$RUNNER" "$TMP/no-such-urbi" "$TMP/basic.chk" >/dev/null 2>&1
expect_rc "missing binary is a runner error"     2 $?
"$RUNNER" "$TMP/urbi-ok" "$TMP/bad-exit-directive.chk" >/dev/null 2>&1
expect_rc "non-integer '## exit:' is a runner error" 2 $?

# host-driver path contract (BLD-CI-7).  DRIVER = $(dirname urbi)/chk-host-driver.
"$RUNNER" "$HD_CRASH/urbi" "$TMP/host-basic.chk" >/dev/null 2>&1
expect_rc "host-driver crash-after-output FAILs (CHK-02 host path)"    1 $?
"$RUNNER" "$HD_HANG/urbi" "$TMP/host-timeout.chk" >/dev/null 2>&1
expect_rc "host-driver hang TIMEOUTs with '## timeout: 2' (CHK-03 host)" 1 $?
"$RUNNER" "$HD_VACUOUS/urbi" "$TMP/host-vacuous.chk" >/dev/null 2>&1
expect_rc "host-driver empty-expected is VACUOUS (CHK-01 host path)"   5 $?

if [ "$fails" -gt 0 ]; then
    echo "test_run_chk_runner: $fails contract check(s) FAILED"
    exit 1
fi
echo "test_run_chk_runner: all 13 runner-contract checks PASS"
