#!/bin/sh
# Fails if any src/rt/ or src/stdlib/ file includes an rt header that sits
# later in the include order than the file's own header.
set -eu
cd "$(dirname "$0")/../.."
ORDER="uvalue ugc ustr uobj ulist ustrand usched uexec uwatch urealm uboot"
rank() { i=0; for n in $ORDER; do i=$((i+1)); [ "$n" = "$1" ] && { echo $i; return; }; done; echo 0; }
STDLIB_ALLOWED="uvalue ugc ustr uobj ulist uexec"
rc=0
for f in src/rt/*.c src/rt/*.h; do
    [ -f "$f" ] || continue
    self=$(basename "$f" | sed -E 's/\.(c|h)$//')
    sr=$(rank "$self")
    for inc in $(grep -oE '#include "rt/u[a-z]+\.h"' "$f" | sed -E 's/.*rt\/(u[a-z]+)\.h"/\1/'); do
        ir=$(rank "$inc")
        if [ "$ir" -gt "$sr" ]; then echo "LAYERING: $f includes rt/$inc.h (rank $ir > $sr)"; rc=1; fi
    done
    if grep -qE '#include "(vm|sched|object|gc|runtime|watcher|event|tag|realm|changed|value)/' "$f"; then
        echo "LAYERING: $f includes an old-runtime header"; rc=1
    fi
done
for f in src/stdlib/*.c src/stdlib/*.h; do
    for inc in $(grep -oE '#include "rt/u[a-z]+\.h"' "$f" | sed -E 's/.*rt\/(u[a-z]+)\.h"/\1/'); do
        ok=0; for a in $STDLIB_ALLOWED; do [ "$a" = "$inc" ] && ok=1; done
        [ $ok -eq 1 ] || { echo "LAYERING: $f includes rt/$inc.h (stdlib may include: $STDLIB_ALLOWED)"; rc=1; }
    done
done
exit $rc
