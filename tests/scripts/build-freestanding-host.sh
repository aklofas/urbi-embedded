#!/bin/sh
# Host-side freestanding gate (v0.9.3-ci-hardening).
#
# For each TU in the URBI_BYTECODE_ONLY keep-list, compile under
# host cc with -ffreestanding -DURBI_BYTECODE_ONLY=1 and nm-grep
# the resulting .o against the forbidden-libc symbol regex.
#
# Catches the leak class that masked v0.9.1 + v0.9.2 from CI:
# an unguarded snprintf / printf / malloc in a TU that compiles
# everywhere but breaks the freestanding-clean contract.
#
# Does NOT depend on any cross toolchain — host cc is sufficient
# to surface the symbol reference.  Cross builds catch a different
# class (libgcc helper drift); see test-freestanding.sh + the
# per-cross-target freestanding-golden gates for that.
#
# Wired into RELEASETEST_PHASE1 via the test-freestanding-host
# Makefile target.
set -eu

HERE="$(dirname "$0")"
. "$HERE/_bytecode-only-tus.sh"
. "$HERE/_freestanding-forbidden.sh"

WORK=build/host-freestanding-host
mkdir -p "$WORK"
rm -f "$WORK"/*.o

CFLAGS_BASE="-std=c99 -Wall -Wextra -Wpedantic -Os"
CFLAGS_FREESTANDING="-ffreestanding -DURBI_BYTECODE_ONLY=1"
CPPFLAGS_BASE="-Iinclude -Isrc -Itests/unit"

CC=${CC:-cc}
NM=${NM:-nm}

fail_count=0
fail_report=""

for src in $(list_kept_tus); do
    obj="$WORK/$(echo "$src" | sed 's|/|_|g; s|\.c$|.o|')"
    # 2>/dev/null suppresses computed-goto -Wpedantic noise from
    # src/vm/uvm.c's DISPATCH() macro — documented warnings, not
    # failures.  Compile errors still fail the script via set -e.
    if ! $CC $CFLAGS_BASE $CFLAGS_FREESTANDING $CPPFLAGS_BASE \
            -c -o "$obj" "$src" 2>/dev/null; then
        echo "FAIL: freestanding compile error on $src" >&2
        exit 1
    fi

    leaks=$($NM -u "$obj" 2>/dev/null \
            | awk -v re="$FORBIDDEN_LIBC_REGEX" '$1 == "U" && $2 ~ re {print $2}' \
            | sort -u)
    if [ -n "$leaks" ]; then
        fail_count=$((fail_count + 1))
        fail_report="${fail_report}
$src leaks:
$(echo "$leaks" | sed 's/^/  /')"
    fi
done

if [ "$fail_count" -gt 0 ]; then
    echo "FAIL: $fail_count TU(s) leak forbidden libc symbols under" >&2
    echo "      -ffreestanding -DURBI_BYTECODE_ONLY=1:" >&2
    echo "$fail_report" >&2
    echo "" >&2
    echo "Either remove the dep, guard the offending code under" >&2
    echo "#if !defined(URBI_BYTECODE_ONLY), or (last resort) document" >&2
    echo "an exception in docs/freestanding-exceptions.md." >&2
    exit 1
fi

tu_count=$(list_kept_tus | wc -l | tr -d ' ')
echo "PASS: host-side freestanding gate — $tu_count TUs clean"
exit 0
