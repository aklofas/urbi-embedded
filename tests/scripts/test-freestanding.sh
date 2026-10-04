#!/bin/sh
# Freestanding archive gate for URBI_BYTECODE_ONLY=1.
# Asserts a bytecode-only liburbi.a has no unresolved libc symbols
# (printf, malloc, fopen, etc.).  Usage:
#
#   test-freestanding.sh <archive> [<preset>]
#
# `make cross-<preset>-bytecode-only` runs it on the archive it builds.
#
# A freestanding liburbi.a must link cleanly against an embedded RTOS
# image without pulling in a hosted libc.  Any unresolved hosted-libc
# symbol on the strip target indicates a freestanding-discipline
# regression: the new dependency either belongs guarded behind
# URBI_BYTECODE_ONLY, routed through urbi_panic / vm->host_log_fn, or
# documented as an accepted exception in docs/freestanding-exceptions.md.
set -eu

ARCHIVE=${1:-build/arm-cortex-m4f-bytecode-only/liburbi.a}
if [ ! -f "$ARCHIVE" ]; then
    echo "FAIL: $ARCHIVE not found. Run a cross-<preset>-bytecode-only target first."
    exit 1
fi

# The second argument names the preset; its nm is the one that reads the
# archive.  Without a preset the host nm is used.
PRESET=${2:-}
if [ -n "$PRESET" ]; then
    if [ ! -f "presets/$PRESET.mk" ]; then
        echo "FAIL: preset $PRESET has no presets/$PRESET.mk" >&2
        exit 1
    fi
    NM_CMD=$(sed -n 's/^CROSS_NM *:= *//p' "presets/$PRESET.mk")
else
    NM_CMD=nm
fi

# A missing cross-nm must fail loudly, not pass vacuously (the nm stderr
# redirect below would otherwise swallow command-not-found into an empty
# symbol list — same trap as the strict-tidy gate, refactor-3 GATE-01).
command -v "$NM_CMD" >/dev/null 2>&1 || {
    echo "FAIL: $NM_CMD not found in PATH — the freestanding gate cannot run." >&2
    echo "      install the matching cross toolchain (or fix PATH) and re-run." >&2
    exit 1
}

. "$(dirname "$0")/_freestanding-forbidden.sh"
LIBC_SYMS=$($NM_CMD "$ARCHIVE" 2>/dev/null \
            | awk -v re="$FORBIDDEN_LIBC_REGEX" '$1 == "U" && $2 ~ re {print $2}' \
            | LC_ALL=C sort -u)

if [ -n "$LIBC_SYMS" ]; then
    echo "FAIL: $ARCHIVE has unresolved libc symbols:"
    echo "$LIBC_SYMS" | sed 's/^/  /'
    echo ""
    echo "URBI_BYTECODE_ONLY=1 builds must be freestanding-clean."
    echo "Either remove the dep, guard it under #if !defined(URBI_BYTECODE_ONLY),"
    echo "or (last resort) document an exception in docs/freestanding-exceptions.md."
    exit 1
fi

# Self-containedness: every project symbol the archive references must
# be defined inside it.  A libc denylist cannot see a call into a
# directory the build left out; an unresolved project-prefixed name is
# exactly that.  libgcc helpers (__aeabi_*, __udivdi3) and the embedder's
# mem*/str* carry no project prefix and are not matched.
DEFINED=$(mktemp)
trap 'rm -f "$DEFINED"' EXIT
$NM_CMD --defined-only "$ARCHIVE" 2>/dev/null \
    | awk 'NF == 3 {print $3}' | LC_ALL=C sort -u > "$DEFINED"
MISSING=$($NM_CMD "$ARCHIVE" 2>/dev/null \
          | awk '$1 == "U" && $2 ~ /^(urbi_|u[a-z]+_)/ {print $2}' \
          | LC_ALL=C sort -u | LC_ALL=C comm -23 - "$DEFINED")

if [ -n "$MISSING" ]; then
    echo "FAIL: $ARCHIVE references project symbols it does not define:"
    echo "$MISSING" | sed 's/^/  /'
    echo ""
    echo "A bytecode-only archive must be self-contained: guard the caller"
    echo "under URBI_BYTECODE_ONLY or route it through a hook the host sets."
    exit 1
fi

echo "PASS: $ARCHIVE is freestanding-clean"
exit 0
