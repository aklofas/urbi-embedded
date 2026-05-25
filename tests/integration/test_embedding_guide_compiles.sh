#!/usr/bin/env bash
# SPDX-License-Identifier: BSD-3-Clause
# test_embedding_guide_compiles.sh — compile every C code sample in
# docs/embedding-guide.md to catch API-drift early.
#
# Extraction convention
# ---------------------
# Two kinds of C fenced blocks appear in the guide:
#
#   /* STANDALONE EXAMPLE — ... */
#     A complete, self-contained C program (has its own main()).
#     Extracted as-is and compiled as a full translation unit.
#     Identified by: the first non-empty content line starts with
#     "/* STANDALONE EXAMPLE".
#
#   /* FRAGMENT — ... */
#     A partial snippet (declarations, function bodies, or expressions).
#     Extracted and wrapped in a void-returning compilation harness:
#
#       #include <stdio.h>
#       #include <stdlib.h>
#       #include <stdint.h>
#       #include <string.h>
#       #include "urbi/urbi.h"
#       #include "urbi/types.h"
#       #include "urbi/aux.h"
#       /* fragment source */
#       /* dummy references to avoid unused-declaration warnings */
#
#     Fragments are compiled with -fsyntax-only (type-check only; no link).
#     Standalone examples are compiled with a full link against liburbi.a.
#
# Any block that is neither labelled STANDALONE nor FRAGMENT is skipped
# (e.g., the bare typedefs shown for exposition; these are already covered
# by the FRAGMENT blocks that use them).
#
# Usage
# -----
#   ./tests/integration/test_embedding_guide_compiles.sh [BUILDDIR]
#
# BUILDDIR defaults to build/host.  Wired into make test-embedding-guide
# and RELEASETEST_PHASE1.

set -euo pipefail

BUILDDIR="${1:-build/host}"
GUIDE="docs/embedding-guide.md"
LIB="${BUILDDIR}/liburbi.a"
LIBURBI_AUX="${BUILDDIR}/liburbi_aux.a"
CC="${CC:-cc}"
CFLAGS_BASE="-std=c99 -Wall -Wextra -Wpedantic -Os -Iinclude"

# Reduce pedantic noise on fragment wrapping (unused vars, missing prototypes).
FRAGMENT_EXTRA="-Wno-unused-variable -Wno-unused-function -Wno-unused-parameter -Wno-missing-prototypes -Wno-missing-declarations"

if [ ! -f "$GUIDE" ]; then
    echo "FAIL: $GUIDE not found (run from repo root)" >&2
    exit 1
fi

if [ ! -f "$LIB" ]; then
    echo "FAIL: $LIB not built (run: make)" >&2
    exit 1
fi

TMPDIR_BASE=$(mktemp -d -t urbi_guide_compile_XXXXXX)
cleanup() { rm -rf "$TMPDIR_BASE"; }
trap cleanup EXIT

PASS=0
FAIL=0
SKIP=0

# ---------------------------------------------------------------------------
# Extract and compile each C block from the guide.
#
# Strategy: read the guide line by line; track open/close fences.
# When a ```c fence opens, collect lines until the closing ```.
# Classify the block and act accordingly.
# ---------------------------------------------------------------------------

block_lines=()
in_block=0
block_index=0

compile_fragment() {
    local idx="$1"; shift
    local -a lines=("$@")
    local src="${TMPDIR_BASE}/frag_${idx}.c"

    {
        printf '#include <stdio.h>\n'
        printf '#include <stdlib.h>\n'
        printf '#include <stdint.h>\n'
        printf '#include <string.h>\n'
        printf '#include <stdbool.h>\n'
        printf '#include "urbi/urbi.h"\n'
        printf '#include "urbi/types.h"\n'
        printf '#include "urbi/aux.h"\n'
        printf '\n'
        # Provide stub types/functions for hardware abstractions used in examples.
        # Declared as weak/static so that fragment redefinitions don't collide.
        printf '/* Compilation harness stubs */\n'
        printf 'static struct UVM *vm_ptr;\n'
        printf 'static struct UTag *my_tag;\n'
        printf 'static urbi_event_id_t EV_SENSOR;\n'
        # Note: EV_ACCEL/EV_GYRO/EV_MAG are defined within the IMU fragment itself.
        printf 'static void host_sleep_until(uint64_t t) { (void)t; }\n'
        printf 'static void handle_fatal(struct UVM **v) { (void)v; }\n'
        printf 'static void record_watcher_latency(urbi_watcher_handle_t h, int s) { (void)h; (void)s; }\n'
        printf 'static float read_sensor_x(void) { return 0.0f; }\n'
        printf 'static float read_sensor_y(void) { return 0.0f; }\n'
        printf 'static float read_sensor_z(void) { return 0.0f; }\n'
        printf 'static void hardware_set_motor(int v) { (void)v; }\n'
        printf 'static float hardware_read_temperature(void) { return 0.0f; }\n'
        printf 'static void hardware_set_led(bool v) { (void)v; }\n'
        printf 'static void hardware_watchdog_kick(void) { }\n'
        printf 'static void read_imu_burst(float *ax, float *ay, float *az, float *gx, float *gy, float *gz, float *mx, float *my, float *mz) { *ax=*ay=*az=*gx=*gy=*gz=*mx=*my=*mz=0.0f; }\n'
        # Note: fn_read_temperature, fn_set_led, fn_set_motor, sensor_destructure
        # are NOT pre-defined here — some fragments define them themselves.
        # The forward declarations in those fragments (added to the guide) handle
        # self-contained compilation.  sensor_destructure is defined in the
        # event-registration fragment; the IMU fragment uses a separate stub.
        printf 'static int harness_sensor_destructure(struct UVM *vm, const urbi_event_payload_t *p, size_t plen, UValue *out, int max, void *ud) { (void)vm; (void)p; (void)plen; (void)out; (void)max; (void)ud; return 0; }\n'
        printf '\n'
        for line in "${lines[@]}"; do
            printf '%s\n' "$line"
        done
    } > "$src"

    local errbuf
    if errbuf=$($CC $CFLAGS_BASE $FRAGMENT_EXTRA -fsyntax-only "$src" 2>&1); then
        PASS=$((PASS + 1))
        echo "  PASS fragment $idx"
    else
        FAIL=$((FAIL + 1))
        echo "  FAIL fragment $idx:" >&2
        echo "$errbuf" >&2
        echo "  Source: $src" >&2
    fi
}

compile_standalone() {
    local idx="$1"; shift
    local -a lines=("$@")
    local src="${TMPDIR_BASE}/standalone_${idx}.c"
    local exe="${TMPDIR_BASE}/standalone_${idx}"

    {
        for line in "${lines[@]}"; do
            printf '%s\n' "$line"
        done
    } > "$src"

    local errbuf
    # Standalone examples include full main(); link against both archives.
    # liburbi_aux.a references symbols in liburbi.a, so pass aux first, then
    # core; the linker resolves aux's undefined refs from the core archive.
    if errbuf=$($CC $CFLAGS_BASE "$src" "$LIBURBI_AUX" "$LIB" -lm -o "$exe" 2>&1); then
        PASS=$((PASS + 1))
        echo "  PASS standalone $idx"
    else
        FAIL=$((FAIL + 1))
        echo "  FAIL standalone $idx:" >&2
        echo "$errbuf" >&2
        echo "  Source: $src" >&2
    fi
}

echo "=== test_embedding_guide_compiles: extracting from $GUIDE ==="

# Parse the guide and extract C blocks.
while IFS= read -r line; do
    if [ $in_block -eq 0 ]; then
        if [[ "$line" == '```c' ]]; then
            in_block=1
            block_lines=()
        fi
    else
        if [[ "$line" == '```' ]]; then
            # Block closed. Classify and compile.
            in_block=0
            block_index=$((block_index + 1))

            if [ ${#block_lines[@]} -eq 0 ]; then
                SKIP=$((SKIP + 1))
                continue
            fi

            # Find first non-empty line for classification.
            first_content=""
            for bl in "${block_lines[@]}"; do
                if [[ -n "$bl" ]]; then
                    first_content="$bl"
                    break
                fi
            done

            if [[ "$first_content" == *"STANDALONE EXAMPLE"* ]]; then
                compile_standalone "$block_index" "${block_lines[@]}"
            elif [[ "$first_content" == *"FRAGMENT"* ]]; then
                compile_fragment "$block_index" "${block_lines[@]}"
            else
                SKIP=$((SKIP + 1))
                echo "  SKIP block $block_index (no STANDALONE/FRAGMENT marker)"
            fi
        else
            block_lines+=("$line")
        fi
    fi
done < "$GUIDE"

echo "=== Results: $PASS passed, $FAIL failed, $SKIP skipped ==="

if [ $FAIL -gt 0 ]; then
    echo "FAIL: $FAIL code sample(s) did not compile" >&2
    exit 1
fi

if [ $PASS -eq 0 ]; then
    echo "FAIL: no code samples found (guide may be empty or malformed)" >&2
    exit 1
fi

echo "PASS: test_embedding_guide_compiles.sh ($PASS samples)"
