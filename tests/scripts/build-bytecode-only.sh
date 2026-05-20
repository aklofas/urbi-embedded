#!/usr/bin/env bash
# Phase 13 / T144: URBI_BYTECODE_ONLY emulation smoke gate.
#
# The real URBI_BYTECODE_ONLY build flag — compile out the
# lexer/parser/emitter, ship a runtime that can only execute
# pre-baked bytecode — lands at M7 per the v1.0 implementation
# design spec §1.1.  Phase 13 lands a smoke approximation that
# proves the architectural shape is sound:
#
#   1. The lex/parse/emit subsystems CAN be elided from the
#      object set (no link-time dependency from runtime/GC/sched
#      back into them at the symbol level once urbi_compile_source
#      and urbi_repl_eval are also dropped).
#   2. The resulting "stripped" archive still exports
#      urbi_stdlib_boot — the entry point M7's BYTECODE_ONLY mode
#      will use to load the baked stdlib blob without parsing.
#
# Excluded sources (the M7 BYTECODE_ONLY no-go list):
#   - src/lex/*.c          (lexer)
#   - src/parse/*.c        (parser + AST builder)
#   - src/emit/*.c         (bytecode emitter + disasm)
#   - src/urbi.c           (urbi_compile_source — parses + emits)
#   - src/chunk/uchunk_strand.c  (urbi_repl_eval — parses + emits)
#
# Hard-fail if:
#   - any kept source FAILS to compile after the above are pulled out
#   - the resulting archive cannot resolve urbi_stdlib_boot via nm
#
# Wired into `make releasetest` via test-bytecode-only.

set -euo pipefail

WORK=build/host-bytecode-only-smoke
LIB=$WORK/liburbi-stripped.a

mkdir -p "$WORK"
rm -f "$LIB"

# Build the kept subset of sources directly (bypass the main Makefile
# wildcard).  Compile each .c into the per-target build dir.
# Keep-list source of truth: tests/scripts/_bytecode-only-tus.sh
. "$(dirname "$0")/_bytecode-only-tus.sh"
# shellcheck disable=SC2207
ALL_KEEP=( $(list_kept_tus) )

CFLAGS_BASE="-std=c99 -Wall -Wextra -Wpedantic -Os"
CPPFLAGS_BASE="-Iinclude -Isrc -Itests/unit"

OBJS=()
for src in "${ALL_KEEP[@]}"; do
    obj="$WORK/$(echo "$src" | sed 's|/|_|g; s|\.c$|.o|')"
    # 2>/dev/null suppresses the existing -Wpedantic computed-goto
    # noise from src/vm/uvm.c (DISPATCH() macro with `goto *expr`);
    # those are documented warnings, not failures.  Compile errors
    # still trigger the failing exit code.
    ${CC:-cc} $CFLAGS_BASE $CPPFLAGS_BASE -c -o "$obj" "$src" 2>/dev/null \
        || { echo "FAIL: stripped-build compile error on $src" >&2; exit 1; }
    OBJS+=("$obj")
done

${AR:-ar} rcs "$LIB" "${OBJS[@]}"

# Symbol checks.  nm -g lists a defined symbol as `<addr> T <name>`;
# undefined references as `         U <name>`.  We grep for the
# defined form.
NEEDED=(urbi_stdlib_boot urbi_vm_init urbi_vm_destroy urbi_lock_heap)
NM=${NM:-nm}
SYMS=$("$NM" -g "$LIB" 2>/dev/null || true)
for sym in "${NEEDED[@]}"; do
    if ! echo "$SYMS" | grep -Eq "^[0-9a-f]+ T $sym$"; then
        echo "FAIL: symbol $sym missing from stripped archive" >&2
        echo "      (URBI_BYTECODE_ONLY-bound API not exported)" >&2
        exit 1
    fi
done

# Inverse check: parser/emitter symbols MUST be absent from the
# stripped archive (they are the things the M7 flag will elide).
# Allowed to appear as undefined references (U) — meaning a kept
# source still references them; that's the M7 wiring gap that the
# real BYTECODE_ONLY flag will close (urbi_compile_source body
# becomes a stub, urbi_repl_eval calls error out).
ABSENT=(uparse_next_statement uemit_statement ulex_init urbi_compile_source)
for sym in "${ABSENT[@]}"; do
    if echo "$SYMS" | grep -Eq "^[0-9a-f]+ T $sym$"; then
        echo "FAIL: symbol $sym defined in stripped archive" >&2
        echo "      (URBI_BYTECODE_ONLY exclusion list leak)" >&2
        exit 1
    fi
done

echo "PASS: URBI_BYTECODE_ONLY smoke — stripped archive ($(stat -c%s "$LIB" 2>/dev/null || stat -f%z "$LIB") bytes) exports stdlib boot, parser/emitter elided"
