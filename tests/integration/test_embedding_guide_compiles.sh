#!/usr/bin/env bash
# SPDX-License-Identifier: BSD-3-Clause
# test_embedding_guide_compiles.sh — compile every C code sample in
# docs/embedding-guide.md, and RUN the standalone ones, so the page cannot
# drift from the headers it documents.
#
# EVERY block must carry a marker.  A block that carries neither is
# counted as a skip, and a skip is what let five wrong public-API
# prototypes sit on that page unnoticed: the unmarked blocks were exactly
# the transcribed signatures, which is the highest drift risk in the
# document.  Prototype listings belong in a FRAGMENT -- they are
# declarations, so -fsyntax-only against the real headers turns any drift
# into a redeclaration conflict.
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
#       #include <stdbool.h>
#       #include "urbi/urbi.h"
#       #include "urbi/types.h"
#       /* fragment source */
#
#     A fragment needing <urbi/repl.h> includes it itself.
#     Fragments are compiled with -fsyntax-only (type-check only; no link).
#     Standalone examples are compiled with a full link against liburbi.a,
#     then EXECUTED; $EXPECT_<block index> names a substring the output
#     must contain.
#
# Any block that is neither labelled STANDALONE nor FRAGMENT is counted as
# a skip.  The gate prints the skip count; it should be 0.
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

# What each standalone example must print when it runs, by block index.
# The flagship registers a native, installs an `at` watcher and a timer,
# and steps until the watcher fires; if it ever stops firing the guide is
# telling embedders something that is no longer true.
EXPECT_1="threshold crossed"

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
        printf '\n'
        # The guide's fragments are self-contained: each declares whatever
        # it uses.  The only thing the harness still supplies is an event id,
        # because a fragment that shows host input should not have to invent
        # a registration call to demonstrate the step ordering.
        printf '/* Compilation harness stubs */\n'
        printf 'static urbi_event_id_t harness_event_id;\n'
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
    # Standalone examples include a full main(); link the shipped archive.
    if ! errbuf=$($CC $CFLAGS_BASE "$src" "$LIB" -lm -o "$exe" 2>&1); then
        FAIL=$((FAIL + 1))
        echo "  FAIL standalone $idx:" >&2
        echo "$errbuf" >&2
        echo "  Source: $src" >&2
        return
    fi

    # And RUN it.  A guide example that compiles but does not do what the
    # surrounding prose says it does is still wrong, and this one is the
    # page's flagship: it registers a native, installs a watcher, drives
    # the scheduler and expects the watcher to fire.  $EXPECT_<idx> names
    # a substring its output must contain.
    local expect_var="EXPECT_${idx}"
    local expect="${!expect_var:-}"
    local out
    if ! out=$(timeout 60 "$exe" 2>&1); then
        FAIL=$((FAIL + 1))
        echo "  FAIL standalone $idx: exited non-zero when run" >&2
        echo "$out" >&2
        return
    fi
    if [ -n "$expect" ] && [[ "$out" != *"$expect"* ]]; then
        FAIL=$((FAIL + 1))
        echo "  FAIL standalone $idx: output did not contain \"$expect\"" >&2
        echo "  got: $out" >&2
        return
    fi
    PASS=$((PASS + 1))
    if [ -n "$expect" ]; then
        echo "  PASS standalone $idx (ran; output contains \"$expect\")"
    else
        echo "  PASS standalone $idx (ran)"
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
