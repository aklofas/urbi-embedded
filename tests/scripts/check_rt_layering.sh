#!/bin/sh
# Fails if any src/rt/ or src/stdlib/ file includes an rt header that sits
# later in the include order than the file's own header.
#
# A file's layer is the LONGEST name in $ORDER that prefixes its basename:
# uexec_ops.c belongs to uexec, ustrand.c to ustrand (not ustr).  A file
# whose basename matches no layer name (uproto_bind.c, uapi.c) is a
# top-of-core helper and may include anything in src/rt — it still may not
# reach into the old runtime directories, which the second check enforces.
set -eu
cd "$(dirname "$0")/../.."
ORDER="uvalue ugc ustr uobj ulist ustrand usched uexec uwatch urealm uboot"
TOP=99
rank() { i=0; for n in $ORDER; do i=$((i+1)); [ "$n" = "$1" ] && { echo $i; return; }; done; echo 0; }
layer_of() {
    best=""
    for n in $ORDER; do
        case "$1" in
            "$n"*) if [ ${#n} -gt ${#best} ]; then best="$n"; fi ;;
        esac
    done
    echo "$best"
}
STDLIB_ALLOWED="uvalue ugc ustr uobj ulist uexec"
rc=0
for f in src/rt/*.c src/rt/*.h; do
    [ -f "$f" ] || continue
    base=$(basename "$f" | sed -E 's/\.(c|h)$//')
    self=$(layer_of "$base")
    if [ -z "$self" ]; then sr=$TOP; else sr=$(rank "$self"); fi
    for inc in $(grep -oE '#include "rt/u[a-z]+\.h"' "$f" | sed -E 's/.*rt\/(u[a-z]+)\.h"/\1/'); do
        ir=$(rank "$inc")
        if [ "$ir" -gt "$sr" ]; then echo "LAYERING: $f includes rt/$inc.h (rank $ir > $sr)"; rc=1; fi
    done
    if grep -qE '#include "(vm|sched|object|gc|runtime|watcher|event|tag|realm|changed|value)/' "$f"; then
        echo "LAYERING: $f includes an old-runtime header"; rc=1
    fi
    # Freestanding rule (spec section 3): src/rt/ uses no libc beyond
    # these five headers.  Anything hosted -- snprintf, malloc, assert --
    # belongs in src/host/ or the frontend, not in the core that has to
    # build for a microcontroller.  test-freestanding-host is parked with
    # the old core, so without this check nothing catches a regression
    # until a port task tries to compile.
    for sys in $(grep -oE '#include[[:space:]]*<[a-z./]+>' "$f" |
                 sed -E 's/.*<([a-z./]+)>/\1/'); do
        case "$sys" in
            stdint.h|stddef.h|stdbool.h|string.h|math.h) ;;
            *) echo "LAYERING: $f includes <$sys> (src/rt may use only stdint.h stddef.h stdbool.h string.h math.h)"; rc=1 ;;
        esac
    done
done
for f in src/stdlib/*.c src/stdlib/*.h; do
    [ -f "$f" ] || continue
    for inc in $(grep -oE '#include "rt/u[a-z]+\.h"' "$f" | sed -E 's/.*rt\/(u[a-z]+)\.h"/\1/'); do
        ok=0; for a in $STDLIB_ALLOWED; do [ "$a" = "$inc" ] && ok=1; done
        [ $ok -eq 1 ] || { echo "LAYERING: $f includes rt/$inc.h (stdlib may include: $STDLIB_ALLOWED)"; rc=1; }
    done
done
exit $rc
