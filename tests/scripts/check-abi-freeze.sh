#!/bin/sh
# tests/scripts/check-abi-freeze.sh
#
# Asserts that the _Static_assert in include/urbi/version.h matches the
# URBI_API_VERSION_* macros immediately above it.  Detects accidental
# drift between the macros and the freeze pin (i.e., someone bumped the
# macros without explicitly overriding the freeze).
#
# v0.10.7 extension: also lints hardcoded literals in the unit test and
# the two release-evidence documents so that "bump macro + assert together
# but forget the test/docs" is caught here rather than silently.
#
# Checked locations:
#   include/urbi/version.h                    — authoritative macros + freeze pin
#   tests/unit/test_api_version.c             — hardcoded MINOR/PATCH literals
#   docs/api-stability.md                     — freeze pin evidence doc
#   docs/release/release-readiness.md         — release gate evidence row
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

# v0.10.7: also lint hardcoded literals in unit tests + evidence docs.
triple="${major}/${minor}/${patch}"

# --- test_api_version.c literal check ---
# The file has UASSERT_EQ(URBI_API_VERSION_MINOR, <literal>) — extract and compare.
test_major_lit=$(grep 'UASSERT_EQ(URBI_API_VERSION_MAJOR' tests/unit/test_api_version.c \
                 | grep -oE ',\s*[0-9]+' | tr -d ', ' | head -1)
test_minor_lit=$(grep 'UASSERT_EQ(URBI_API_VERSION_MINOR' tests/unit/test_api_version.c \
                 | grep -oE ',\s*[0-9]+' | tr -d ', ' | head -1)
test_patch_lit=$(grep 'UASSERT_EQ(URBI_API_VERSION_PATCH' tests/unit/test_api_version.c \
                 | grep -oE ',\s*[0-9]+' | tr -d ', ' | head -1)
if [ "$test_major_lit" != "$major" ]; then
    echo "FAIL: tests/unit/test_api_version.c MAJOR literal ($test_major_lit) != version.h ($major)"
    exit 1
fi
if [ "$test_minor_lit" != "$minor" ]; then
    echo "FAIL: tests/unit/test_api_version.c MINOR literal ($test_minor_lit) != version.h ($minor)"
    exit 1
fi
if [ "$test_patch_lit" != "$patch" ]; then
    echo "FAIL: tests/unit/test_api_version.c PATCH literal ($test_patch_lit) != version.h ($patch)"
    exit 1
fi

# --- evidence doc checks ---
for doc in docs/api-stability.md docs/release/release-readiness.md; do
    if ! grep -q "$triple" "$doc"; then
        echo "FAIL: $doc does not mention current ABI triple $triple"
        exit 1
    fi
done

echo "ABI freeze pin in sync: $major/$minor/$patch"
