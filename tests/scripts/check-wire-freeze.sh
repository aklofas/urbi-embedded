#!/bin/sh
# tests/scripts/check-wire-freeze.sh
#
# Asserts that the wire-format _Static_assert in src/chunk/uchunk_io.c
# matches the URBI_BYTECODE_VERSION_* macros in src/chunk/uchunk.h.

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

# Also assert the doc still mentions v1.${minor} somewhere.
if ! grep -q "v1\.$minor" docs/internals/bytecode-format.md; then
    echo "wire freeze: docs/internals/bytecode-format.md does not mention v1.$minor"
    exit 1
fi

echo "wire format freeze pin in sync: v1.$minor / 0x1$minor"
