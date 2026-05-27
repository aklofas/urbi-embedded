#!/usr/bin/env bash
# check-version-sync.sh — fail if any of these drift from each other:
#   - ESP-IDF component manifest version vs latest release tag
#   - README.md ABI version string vs include/urbi/version.h
#   - README.md wire format string vs URBI_BYTECODE_VERSION_MAJOR/MINOR in
#     src/chunk/uchunk.h (the byte is a computed macro; we derive it from
#     the two literal constants)
#   - README.md tag reference vs latest release tag
#
# Run by `make check-version-sync` and by the version-sync GHA job.
# Run on every PR + merge.
set -eu

fail=0
fail_msg() { echo "ERROR: $*" >&2; fail=1; }

# === (1) ESP-IDF component manifest vs latest tag ===

COMPONENT_VERSION=$(grep '^version:' components/esp32-idf/idf_component.yml | \
                    sed 's/^version: *"\(.*\)"/\1/')
LATEST_TAG=$(git tag --sort=-v:refname | head -1)

if [ -z "$COMPONENT_VERSION" ]; then
    fail_msg "could not extract version: field from idf_component.yml"
elif [ -z "$LATEST_TAG" ]; then
    fail_msg "no git tag found; cannot verify component version sync"
else
    EXPECTED="${LATEST_TAG#v}"
    if [ "$COMPONENT_VERSION" != "$EXPECTED" ]; then
        fail_msg "component version drift"
        echo "    components/esp32-idf/idf_component.yml: $COMPONENT_VERSION" >&2
        echo "    latest git tag (stripped 'v'):           $EXPECTED" >&2
        echo "  Bump idf_component.yml version: field to match before tagging." >&2
    fi
fi

# === (2) README ABI vs include/urbi/version.h ===

ABI_MAJOR=$(grep '^#define URBI_API_VERSION_MAJOR' include/urbi/version.h | \
            awk '{print $3}')
ABI_MINOR=$(grep '^#define URBI_API_VERSION_MINOR' include/urbi/version.h | \
            awk '{print $3}')
ABI_PATCH=$(grep '^#define URBI_API_VERSION_PATCH' include/urbi/version.h | \
            awk '{print $3}')
ABI_FULL="${ABI_MAJOR}/${ABI_MINOR}/${ABI_PATCH}"

# README cites ABI as "ABI X/Y/Z" — extract every such mention
README_ABI=$(grep -oE 'ABI [0-9]+/[0-9]+/[0-9]+' README.md | head -1 | awk '{print $2}')

if [ -z "$README_ABI" ]; then
    fail_msg "README.md contains no 'ABI X/Y/Z' string"
elif [ "$README_ABI" != "$ABI_FULL" ]; then
    fail_msg "README ABI drift"
    echo "    README.md:               ABI $README_ABI" >&2
    echo "    include/urbi/version.h:  ABI $ABI_FULL" >&2
fi

# === (3) README wire format vs URBI_BYTECODE_VERSION_MAJOR/MINOR in
#         src/chunk/uchunk.h ===
#
# URBI_BYTECODE_VERSION_BYTE is a computed macro:
#   #define URBI_BYTECODE_VERSION_BYTE ((MAJOR << 4U) | MINOR)
# We derive the version string directly from the two literal constants.

WIRE_MAJOR=$(grep -E '^#define\s+URBI_BYTECODE_VERSION_MAJOR' src/chunk/uchunk.h | \
             awk '{print $3}' | tr -d 'Uu')
WIRE_MINOR=$(grep -E '^#define\s+URBI_BYTECODE_VERSION_MINOR' src/chunk/uchunk.h | \
             awk '{print $3}' | tr -d 'Uu')

if [ -z "$WIRE_MAJOR" ] || [ -z "$WIRE_MINOR" ]; then
    fail_msg "could not extract URBI_BYTECODE_VERSION_MAJOR/MINOR from src/chunk/uchunk.h"
else
    WIRE_STR="v${WIRE_MAJOR}.${WIRE_MINOR}"

    # README cites wire as "wire vX.Y" or "bytecode vX.Y" or similar
    # (case-insensitive — README sometimes capitalizes at sentence start).
    README_WIRE=$(grep -oEi '(wire|bytecode) v[0-9]+\.[0-9]+' README.md | \
                  head -1 | awk '{print $2}')

    if [ -z "$README_WIRE" ]; then
        fail_msg "README.md contains no 'wire vX.Y' or 'bytecode vX.Y' string"
    elif [ "$README_WIRE" != "$WIRE_STR" ]; then
        fail_msg "README wire format drift"
        echo "    README.md:                              $README_WIRE" >&2
        echo "    src/chunk/uchunk.h MAJOR/MINOR:         $WIRE_STR" >&2
    fi
fi

# === (4) README tag reference vs latest tag ===

# README "Tagged" claim — find and verify.  Tag suffix can contain
# mixed case (e.g. v0.10.11-channel-and-isA), so [A-Za-z0-9-]+ not [a-z0-9-]+.
README_TAG=$(grep -oE 'v[0-9]+\.[0-9]+\.[0-9]+(-[A-Za-z0-9-]+)?' README.md | head -1)

if [ -z "$README_TAG" ]; then
    fail_msg "README.md contains no tag reference (vX.Y.Z[-name])"
elif [ "$README_TAG" != "$LATEST_TAG" ]; then
    fail_msg "README tag reference drift"
    echo "    README.md:        $README_TAG" >&2
    echo "    latest git tag:   $LATEST_TAG" >&2
fi

if [ "$fail" -eq 0 ]; then
    echo "OK: version sync clean"
    echo "  Component:     $COMPONENT_VERSION (matches $LATEST_TAG)"
    echo "  ABI:           $ABI_FULL"
    echo "  Wire:          $WIRE_STR"
    echo "  Tag in README: $README_TAG"
    exit 0
fi

exit 1
