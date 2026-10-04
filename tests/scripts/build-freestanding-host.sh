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

# BLD-CI-5: fail if a new src/ dir was added without a keep-vs-exclude
# decision in the bytecode-only keep-list (drift guard).
check_all_src_dirs_classified

WORK=build/host-freestanding-host
mkdir -p "$WORK"
rm -f "$WORK"/*.o

CFLAGS_BASE="-std=c99 -Wall -Wextra -Wpedantic -Os"
CFLAGS_FREESTANDING="-ffreestanding -DURBI_BYTECODE_ONLY=1"
CPPFLAGS_BASE="-Iinclude -Isrc -Itests/unit"

CC=${CC:-cc}
NM=${NM:-nm}

# A missing nm must fail loudly, not pass vacuously (the nm stderr redirect
# below would otherwise swallow command-not-found into an empty leak list —
# same trap as the strict-tidy gate, refactor-3 GATE-01).
command -v "$NM" >/dev/null 2>&1 || {
    echo "FAIL: $NM not found in PATH — the host freestanding gate cannot run." >&2
    exit 1
}

# Self-check: a fortifying host cc (-D_FORTIFY_SOURCE default at -Os/-O2)
# rewrites snprintf to __snprintf_chk, which an un-widened regex would
# miss — prove the detector still works on THIS toolchain, against a
# scratch TU outside the tree, before trusting any clean run below.
cat > "$WORK/_selfcheck.c" <<'EOF'
#include <stdio.h>
int urbi_freestanding_gate_selfcheck(int n)
{
    /* A fixed-size local, not a pointer parameter: fortify only
     * emits the __snprintf_chk rewrite when __builtin_object_size
     * can see a compile-time bound on the destination. */
    char buf[64];
    return snprintf(buf, sizeof buf, "%d", n);
}
EOF
if ! $CC $CFLAGS_BASE $CFLAGS_FREESTANDING $CPPFLAGS_BASE \
        -c -o "$WORK/_selfcheck.o" "$WORK/_selfcheck.c" 2>/dev/null; then
    echo "FAIL: the gate self-check TU failed to compile — cannot trust the gate." >&2
    exit 1
fi
selfcheck_leaks=$($NM -u "$WORK/_selfcheck.o" 2>/dev/null \
        | awk -v re="$FORBIDDEN_LIBC_REGEX" '$1 == "U" && $2 ~ re {print $2}')
if [ -z "$selfcheck_leaks" ]; then
    echo "FAIL: gate self-check did not detect a known snprintf leak —" >&2
    echo "      FORBIDDEN_LIBC_REGEX cannot catch this toolchain's symbol" >&2
    echo "      naming (e.g. a fortify-wrapped __snprintf_chk).  Fix the" >&2
    echo "      regex in tests/scripts/_freestanding-forbidden.sh before" >&2
    echo "      trusting any PASS below." >&2
    exit 1
fi

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
