#!/usr/bin/env bash
# Batch-path error surfacing: urbi -e / -f must exit non-zero on an
# uncaught throw and zero on success. The chk suite runs only the REPL
# path, so this script is the regression gate for the file/-e entry.
set -u
URBI="${URBI:-build/host/urbi}"
fail=0
chk() { # chk <expected-rc> <desc> <args...>
    local want="$1"; shift; local desc="$1"; shift
    "$URBI" "$@" >/dev/null 2>&1
    local got=$?
    if [ "$got" -ne "$want" ]; then
        echo "FAIL: $desc (rc=$got want=$want)"; fail=1
    fi
}
chk 0 "clean expr"            -e '1 + 1'
chk 0 "sleep(0) no-op"        -e 'sleep(0)'
chk 1 "uncaught scalar throw" -e 'throw 99'
# v0.13.4-B: a scalar throw must be REPORTED, not swallowed as "(vm error)".
#
# The wording changed at v0.14.0-refoundation.  v0.13.4 could only answer
# with the category, "uncaught throw", because the old core rendered a
# non-object throw as nil; the re-founded unwinder renders the value, so
# the batch path now says what was thrown.  That is what the corpus pins
# for the same throw under -i -- tests/chk/control_transfer/throw_uncaught.chk
# expects `!!! 42` -- and the two paths have to agree.  The category
# wording survives only as the fallback for a value the unwinder cannot
# spell at all, which is why it is not asserted here.
stderr=$("$URBI" -e 'throw 99' 2>&1 >/dev/null)
case "$stderr" in *"99"*) : ;; *) echo "FAIL: scalar throw stderr='$stderr' (want the thrown value, 99)"; fail=1 ;; esac
case "$stderr" in *"(vm error)"*) echo "FAIL: scalar throw stderr still says '(vm error)'"; fail=1 ;; *) : ;; esac
chk 1 "uncaught exception"    -e 'throw Exception.new("boom")'
chk 1 "fork strand throw after root ok" -e 'cout << "x", { throw 1 }'
tmp=$(mktemp); echo 'throw 42' > "$tmp"
chk 1 "uncaught throw in file" "$tmp"
rm -f "$tmp"

# LANG-S06: chunk-top & and , forks must work on the -e path (routes through
# the persistent loader strand rather than the run-to-completion transient).
out=$("$URBI" -e 'cout << "x", cout << "y"' 2>/dev/null)
case "$out" in *x*y*|*y*x*) : ;; *) echo "FAIL: chunk-top comma fork (out='$out')"; fail=1 ;; esac

out=$("$URBI" -e '{ cout << "a" } | { cout << "b" }; 1' 2>/dev/null)
case "$out" in *a*b*) : ;; *) echo "FAIL: sequential pipe (out='$out')"; fail=1 ;; esac

out=$("$URBI" -e '1 + 1' 2>/dev/null)
[ "$out" = "2" ] || { echo "FAIL: -e result print (out='$out')"; fail=1; }

chk 1 "throw still rc 1 after reroute" -e 'throw 99'

# sleep(1s) on the batch path must actually sleep ~1 second and exit 0
# (the loader strand parks at the sleep opcode and WAKE_AT drives the real wait).
t0=$(date +%s%N 2>/dev/null || echo 0)
"$URBI" -e 'sleep(1s)' >/dev/null 2>&1; rc_sleep=$?
t1=$(date +%s%N 2>/dev/null || echo 0)
if [ "$t0" != "0" ] && [ "$t1" != "0" ]; then
    elapsed_ms=$(( (t1 - t0) / 1000000 ))
    if [ "$rc_sleep" -ne 0 ]; then
        echo "FAIL: sleep(1s) exit rc=$rc_sleep (want 0)"; fail=1
    elif [ "$elapsed_ms" -lt 900 ]; then
        echo "FAIL: sleep(1s) elapsed ${elapsed_ms}ms (want >=900ms)"; fail=1
    fi
else
    chk 0 "sleep(1s) exit 0" -e 'sleep(1s)'
fi

[ "$fail" -eq 0 ] && echo "test-batch-errors: OK"
exit "$fail"
