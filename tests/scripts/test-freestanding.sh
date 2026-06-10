#!/bin/sh
# T18 / Wave 1 — freestanding CI gate for URBI_BYTECODE_ONLY=1.
# Asserts the resulting liburbi.a has no unresolved libc symbols (printf,
# malloc, fopen, etc.). Reads liburbi.a built by cross-arm or cross-riscv
# under URBI_BYTECODE_ONLY=1.
#
# Closes REVIVAL §6 acceptance criterion #1 / #8 in CI: a freestanding
# liburbi.a must link cleanly against an embedded RTOS image without
# pulling in a hosted libc.  Any unresolved hosted-libc symbol on the
# strip target indicates a freestanding-discipline regression — the
# new dependency either belongs guarded behind URBI_BYTECODE_ONLY,
# routed through urbi_panic / vm->host_log_fn, or documented as an
# accepted exception in docs/freestanding-exceptions.md.
set -eu

ARCHIVE=${1:-build/cross-arm-bytecode-only/liburbi.a}
if [ ! -f "$ARCHIVE" ]; then
    echo "FAIL: $ARCHIVE not found. Run cross-arm-bytecode-only target first."
    exit 1
fi

# Which nm to use? Cross-arm needs arm-none-eabi-nm; cross-riscv uses
# riscv64-unknown-elf-nm (matches the Makefile's riscv64-unknown-elf-gcc
# CC for rv32imc); esp32s3 uses the unified ESP-IDF xtensa-esp-elf-nm;
# otherwise use plain nm.
case "$ARCHIVE" in
    *esp32s3*)     NM_CMD=xtensa-esp-elf-nm ;;
    *stm32f4*)     NM_CMD=arm-none-eabi-nm ;;
    *cross-arm*)   NM_CMD=arm-none-eabi-nm ;;
    *cross-riscv*) if command -v riscv64-unknown-elf-nm >/dev/null 2>&1; then
                       NM_CMD=riscv64-unknown-elf-nm
                   else
                       # xpack ships gcc/ar under both prefixes but binutils
                       # extras (nm) only as riscv-none-elf-*.
                       NM_CMD=riscv-none-elf-nm
                   fi ;;
    *)             NM_CMD=nm ;;
esac

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
            | sort -u)

if [ -n "$LIBC_SYMS" ]; then
    echo "FAIL: $ARCHIVE has unresolved libc symbols:"
    echo "$LIBC_SYMS" | sed 's/^/  /'
    echo ""
    echo "URBI_BYTECODE_ONLY=1 builds must be freestanding-clean."
    echo "Either remove the dep, guard it under #if !defined(URBI_BYTECODE_ONLY),"
    echo "or (last resort) document an exception in docs/freestanding-exceptions.md."
    exit 1
fi

echo "PASS: $ARCHIVE is freestanding-clean"
exit 0
