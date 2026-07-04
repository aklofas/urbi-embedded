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
chk 1 "uncaught scalar throw" -e 'throw 99'
chk 1 "uncaught exception"    -e 'throw Exception.new("boom")'
tmp=$(mktemp); echo 'throw 42' > "$tmp"
chk 1 "uncaught throw in file" "$tmp"
rm -f "$tmp"
[ "$fail" -eq 0 ] && echo "test-batch-errors: OK"
exit "$fail"
