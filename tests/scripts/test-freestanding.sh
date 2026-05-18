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
    *cross-riscv*) NM_CMD=riscv64-unknown-elf-nm ;;
    *)             NM_CMD=nm ;;
esac

LIBC_SYMS=$($NM_CMD "$ARCHIVE" 2>/dev/null \
            | awk '$1 == "U" && $2 ~ /^(printf|fprintf|sprintf|snprintf|malloc|calloc|realloc|free|fopen|fclose|fread|fwrite|strtod|strtol|strtoul|abort|exit)$/ {print $2}' \
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
