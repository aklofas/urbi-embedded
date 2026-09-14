#!/bin/sh
# SPDX-License-Identifier: BSD-3-Clause
# chk_summary.sh — run the .chk corpus, print a per-directory tally, and
# gate on the directories the current core is expected to satisfy.
#
# Usage: chk_summary.sh <urbi-binary>
#
# The whole corpus (minus tests/chk/repl, which is NDJSON rather than
# urbiscript) is always RUN and REPORTED, so the report shows how much of
# the language is back.  Only $CHK_GATE_DIRS decides pass/fail, because
# the runtime is being re-founded subsystem by subsystem: a directory
# joins the gate when its subsystem lands.
#
# tests/chk/bringup-exclusions.txt names individual fixtures inside a
# gated directory that a later subsystem still blocks.  It is a ratchet:
# an excluded fixture that starts passing FAILS the gate, so whoever
# unblocks it has to delete its line.
#
# Outcome columns follow run_chk.sh's exit-code contract:
#   PASS(0)  SKIP(3: preset-gated or no host driver)  PLACEHOLDER(4)
#   VACUOUS(5)  FAIL(anything else)

set -u
cd "$(dirname "$0")/../.."

URBI="${1:?usage: chk_summary.sh <urbi-binary>}"
CHK_GATE_DIRS="${CHK_GATE_DIRS:-arithmetic closure function control}"
EXCLUSIONS=tests/chk/bringup-exclusions.txt

excluded() {
    [ -f "$EXCLUSIONS" ] || return 1
    sed 's/#.*//' "$EXCLUSIONS" | tr -d ' \t' | grep -qx "$1"
}
gated() {
    for g in $CHK_GATE_DIRS; do [ "$g" = "$1" ] && return 0; done
    return 1
}

DIRS=$(find tests/chk -mindepth 1 -maxdepth 1 -type d ! -name repl \
       | sed 's|tests/chk/||' | sort)

gate_fail=""
ratchet_fail=""
tp=0; tf=0; tph=0; tsk=0; tv=0

for d in $DIRS; do
    p=0; f=0; ph=0; sk=0; v=0
    for x in tests/chk/"$d"/*.chk; do
        [ -f "$x" ] || continue
        URBI_BUILD_PRESET=default tests/integration/run_chk.sh "$URBI" "$x" >/dev/null 2>&1
        rc=$?
        case $rc in
            0) p=$((p + 1)) ;;
            3) sk=$((sk + 1)) ;;
            4) ph=$((ph + 1)) ;;
            5) v=$((v + 1)) ;;
            *) f=$((f + 1)) ;;
        esac
        gated "$d" || continue
        if excluded "$x"; then
            [ "$rc" -eq 0 ] && ratchet_fail="$ratchet_fail $x"
        elif [ "$rc" -ne 0 ] && [ "$rc" -ne 3 ] && [ "$rc" -ne 4 ]; then
            gate_fail="$gate_fail $x"
        fi
    done
    mark=" "
    gated "$d" && mark="*"
    printf '%s %-22s PASS=%-4d PLACEHOLDER=%-4d SKIP=%-4d VACUOUS=%-3d FAIL=%d\n' \
           "$mark" "$d" "$p" "$ph" "$sk" "$v" "$f"
    tp=$((tp + p)); tf=$((tf + f)); tph=$((tph + ph)); tsk=$((tsk + sk)); tv=$((tv + v))
done

printf '  %-22s PASS=%-4d PLACEHOLDER=%-4d SKIP=%-4d VACUOUS=%-3d FAIL=%d\n' \
       "TOTAL" "$tp" "$tph" "$tsk" "$tv" "$tf"
printf '  (* = gated: %s)\n' "$CHK_GATE_DIRS"

rc=0
if [ -n "$gate_fail" ]; then
    echo "chk: FAIL in a gated directory —$gate_fail"
    rc=1
fi
if [ -n "$ratchet_fail" ]; then
    echo "chk: these fixtures now pass and must be removed from $EXCLUSIONS —$ratchet_fail"
    rc=1
fi
[ "$rc" -eq 0 ] && echo "chk: gated directories green"
exit $rc
