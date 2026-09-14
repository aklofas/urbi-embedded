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
# Implementation files whose layer is not their basename, as <path>=<layer>
# ("top" = the top-of-core rank a file with no layer name gets, e.g.
# uapi.c).  USched is a by-value member of UVM, so rt/usched.h must be
# complete before rt/uexec.h and therefore ranks below it -- but the
# scheduler DRIVES the exec core (usched_step calls uexec_run, utag_stop
# walks vm->realms), so its .c files are exec-rank.  usched_natives.c is
# the boot-table half of the same subsystem and reaches the stdlib glue,
# which puts it at the top with the other installers.  The rule the gate
# still enforces on all of them: rt/usched.h itself may reach no further
# than rt/ustrand.h, which the loop below checks as usual.
LAYER_OVERRIDES="src/rt/usched.c=uexec src/rt/utag.c=uexec src/rt/usched_natives.c=top"
# The src/repl files the Makefile actually compiles, with their headers.
# The parked networked server is left alone: it is held to the layering of
# the runtime it was written against, not this one.
REPL_CORE_FILES="src/repl/urepl.c src/repl/urepl.h \
src/repl/urepl_dispatch.c src/repl/urepl_dispatch.h \
src/repl/urepl_ndjson.c src/repl/urepl_ndjson.h \
src/repl/urepl_buffer_transport.c src/repl/urepl_buffer_transport.h"
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
# src/stdlib reaches the runtime through exactly one header.
STDLIB_ALLOWED="ustdlib_glue"
# src/repl sits ABOVE src/stdlib: the eval service drives the public API,
# and needs the realm it points a writer and a compile budget at, which is
# rt/urealm.h plus the rt/uexec.h that completes URealm.  Anything deeper
# — the collector, the scheduler, the object model — would make the
# service part of the runtime instead of a client of it.
REPL_ALLOWED="uexec urealm"
rc=0
for f in src/rt/*.c src/rt/*.h; do
    [ -f "$f" ] || continue
    base=$(basename "$f" | sed -E 's/\.(c|h)$//')
    self=$(layer_of "$base")
    if [ -z "$self" ]; then sr=$TOP; else sr=$(rank "$self"); fi
    for o in $LAYER_OVERRIDES; do
        case "$o" in
            "$f="*) ov=${o#*=}
                    if [ "$ov" = top ]; then sr=$TOP; else sr=$(rank "$ov"); fi ;;
        esac
    done
    for inc in $(grep -oE '#include "rt/u[a-z_]+\.h"' "$f" | sed -E 's/.*rt\/(u[a-z_]+)\.h"/\1/'); do
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
    for inc in $(grep -oE '#include "rt/u[a-z_]+\.h"' "$f" | sed -E 's/.*rt\/(u[a-z_]+)\.h"/\1/'); do
        ok=0; for a in $STDLIB_ALLOWED; do [ "$a" = "$inc" ] && ok=1; done
        [ $ok -eq 1 ] || { echo "LAYERING: $f includes rt/$inc.h (stdlib may include: $STDLIB_ALLOWED)"; rc=1; }
    done
done
for f in $REPL_CORE_FILES; do
    [ -f "$f" ] || continue
    for inc in $(grep -oE '#include "rt/u[a-z_]+\.h"' "$f" | sed -E 's/.*rt\/(u[a-z_]+)\.h"/\1/'); do
        ok=0; for a in $REPL_ALLOWED; do [ "$a" = "$inc" ] && ok=1; done
        [ $ok -eq 1 ] || { echo "LAYERING: $f includes rt/$inc.h (repl may include: $REPL_ALLOWED)"; rc=1; }
    done
    if grep -qE '#include "(vm|sched|object|gc|runtime|watcher|event|tag|realm|changed|value)/' "$f"; then
        echo "LAYERING: $f includes an old-runtime header"; rc=1
    fi
done
exit $rc
