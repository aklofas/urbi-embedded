#!/bin/sh
# tests/scripts/check-abi-freeze.sh
#
# Asserts that the _Static_assert in include/urbi/version.h matches the
# URBI_API_VERSION_* macros immediately above it.  Detects accidental
# drift between the macros and the freeze pin (i.e., someone bumped the
# macros without explicitly overriding the freeze).
#
# This is BELT-AND-BRACES — the static_assert already catches mismatch at
# compile time.  This script makes the intent visible in a CI log line.

set -eu

VERSION_H="include/urbi/version.h"

major=$(grep -E '^#define URBI_API_VERSION_MAJOR' "$VERSION_H" | awk '{print $3}')
minor=$(grep -E '^#define URBI_API_VERSION_MINOR' "$VERSION_H" | awk '{print $3}')
patch=$(grep -E '^#define URBI_API_VERSION_PATCH' "$VERSION_H" | awk '{print $3}')

asserted=$(grep -E 'URBI_API_VERSION_MAJOR == ' "$VERSION_H" | head -1)
asserted_minor=$(grep -E 'URBI_API_VERSION_MINOR == ' "$VERSION_H" | head -1)
asserted_patch=$(grep -E 'URBI_API_VERSION_PATCH == ' "$VERSION_H" | head -1)

if ! echo "$asserted"       | grep -q "MAJOR == $major"; then
    echo "ABI freeze: MAJOR macro=$major asserted=$asserted"
    exit 1
fi
if ! echo "$asserted_minor" | grep -q "MINOR == $minor"; then
    echo "ABI freeze: MINOR macro=$minor asserted=$asserted_minor"
    exit 1
fi
if ! echo "$asserted_patch" | grep -q "PATCH == $patch"; then
    echo "ABI freeze: PATCH macro=$patch asserted=$asserted_patch"
    exit 1
fi

echo "ABI freeze pin in sync: $major/$minor/$patch"
