#!/bin/sh
# tests/scripts/check-wire-freeze.sh
#
# Asserts that the wire-format _Static_assert in src/chunk/uchunk_io.c
# matches the URBI_BYTECODE_VERSION_* macros in src/chunk/uchunk.h.
#
# v0.10.7 extension: also lints hardcoded literals in the unit test and
# the release-evidence document so that "bump macro + assert together but
# forget the test/docs" is caught here rather than silently.
#
# Checked locations:
#   src/chunk/uchunk.h                        — authoritative macros
#   src/chunk/uchunk_io.c                     — freeze pin _Static_assert
#   tests/unit/test_version.c                 — hardcoded VERSION_BYTE/MINOR literals
#   docs/internals/bytecode-format.md         — format history doc (must mention current)
#   docs/release/release-readiness.md         — release gate evidence row
#
# Design note: the codebase legitimately contains many references to old
# wire-format versions (v1.7, v1.8) in:
#   - version-history comments in uchunk.h and uchunk_io.c
#   - the bytecode-format.md version history table
#   - unit tests that verify old-version blobs are correctly rejected
#   - raw opcode bytes in the stdlib binary blob (urbi_stdlib_bytecode.gen.c)
# A broad regex scan over src/docs/tests generates too many false positives
# in this codebase.  Instead we lint the specific locations where drift
# actually causes problems: the unit-test literals and the evidence docs.

set -eu

UCHUNK_H="src/chunk/uchunk.h"
UCHUNK_IO="src/chunk/uchunk_io.c"

major=$(grep -E '^#define URBI_BYTECODE_VERSION_MAJOR' "$UCHUNK_H" | awk '{print $3}' | tr -d 'U')
minor=$(grep -E '^#define URBI_BYTECODE_VERSION_MINOR' "$UCHUNK_H" | awk '{print $3}' | tr -d 'U')

asserted=$(grep -E 'URBI_BYTECODE_VERSION_MAJOR == ' "$UCHUNK_IO" | head -1)
asserted_minor=$(grep -E 'URBI_BYTECODE_VERSION_MINOR == ' "$UCHUNK_IO" | head -1)

if ! echo "$asserted"       | grep -q "MAJOR == $major"; then
    echo "wire freeze: MAJOR macro=$major asserted=$asserted"
    exit 1
fi
if ! echo "$asserted_minor" | grep -q "MINOR == $minor"; then
    echo "wire freeze: MINOR macro=$minor asserted=$asserted_minor"
    exit 1
fi

# Also assert the bytecode-format doc still mentions the current version.
if ! grep -q "v1\.$minor" docs/internals/bytecode-format.md; then
    echo "wire freeze: docs/internals/bytecode-format.md does not mention v1.$minor"
    exit 1
fi

# v0.10.7: also lint hardcoded literals in unit tests + evidence docs.
# URBI_BYTECODE_VERSION_BYTE = (MAJOR << 4) | MINOR = 0x<major><minor>
byte_hex=$(printf '%x' "$(( (major << 4) | minor ))")

# --- test_version.c literal check ---
# The file has UASSERT_EQ((unsigned)URBI_BYTECODE_VERSION_BYTE, 0x<hex>U)
# Extract and compare.
test_byte_lit=$(grep 'UASSERT_EQ.*URBI_BYTECODE_VERSION_BYTE' tests/unit/test_version.c \
                | grep -oiE '0x[0-9a-f]+' | head -1 | tr '[:upper:]' '[:lower:]' | sed 's/0x//')
test_minor_lit=$(grep 'UASSERT_EQ.*URBI_BYTECODE_VERSION_MINOR' tests/unit/test_version.c \
                 | grep -oE ',\s*[0-9]+' | tr -d ', ' | head -1)
if [ "$test_byte_lit" != "$byte_hex" ]; then
    echo "FAIL: tests/unit/test_version.c VERSION_BYTE literal (0x${test_byte_lit}) != computed (0x${byte_hex})"
    exit 1
fi
if [ "$test_minor_lit" != "$minor" ]; then
    echo "FAIL: tests/unit/test_version.c MINOR literal ($test_minor_lit) != uchunk.h ($minor)"
    exit 1
fi

# --- evidence doc check ---
if ! grep -q "v1\.$minor" docs/release/release-readiness.md; then
    echo "FAIL: docs/release/release-readiness.md does not mention wire format v1.$minor"
    exit 1
fi

echo "wire format freeze pin in sync: v1.$minor / 0x${byte_hex}"
