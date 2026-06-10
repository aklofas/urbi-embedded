#!/usr/bin/env bash
# check-version-sync.sh — fail if any of these drift from each other:
#   - ESP-IDF component manifest version vs latest release tag
#   - EVERY README.md "ABI X/Y/Z" mention vs include/urbi/version.h
#   - EVERY README.md "wire vX.Y"/"bytecode vX.Y" mention vs
#     URBI_BYTECODE_VERSION_MAJOR/MINOR in src/chunk/uchunk.h
#   - EVERY README.md urbi tag mention (vX.Y.Z[-name]) vs latest release tag
#     (mentions on lines containing "ESP-IDF" are toolchain pins, not urbi
#     tags, and are excluded)
#   - the URBI_VERSION literal in src/urbi.c vs latest release tag
#
# refactor-3 GATE-03: the previous version checked only the FIRST mention of
# each string (head -1) — the known README-ordering trap was structural.
#
# Pre-tag window escape: during the release ritual the manifests are bumped
# BEFORE the tag exists.  Export URBI_RELEASE_TAG_TO_BE=vX.Y.Z-name to check
# against the tag about to be created instead of `git tag` history.
#
# Run by `make check-version-sync` and by the version-sync GHA job.
set -eu

fail=0
fail_msg() { echo "ERROR: $*" >&2; fail=1; }

LATEST_TAG=$(git tag --sort=-v:refname | head -1)
if [ -n "${URBI_RELEASE_TAG_TO_BE:-}" ]; then
    LATEST_TAG="$URBI_RELEASE_TAG_TO_BE"
    echo "note: URBI_RELEASE_TAG_TO_BE=$LATEST_TAG (pre-tag window escape)"
fi
if [ -z "$LATEST_TAG" ]; then
    fail_msg "no git tag found and URBI_RELEASE_TAG_TO_BE unset"
    exit 1
fi

# === (1) ESP-IDF component manifest vs latest tag ===

COMPONENT_VERSION=$(grep '^version:' components/esp32-idf/idf_component.yml | \
                    sed 's/^version: *"\(.*\)"/\1/')
EXPECTED="${LATEST_TAG#v}"

if [ -z "$COMPONENT_VERSION" ]; then
    fail_msg "could not extract version: field from idf_component.yml"
elif [ "$COMPONENT_VERSION" != "$EXPECTED" ]; then
    fail_msg "component version drift"
    echo "    components/esp32-idf/idf_component.yml: $COMPONENT_VERSION" >&2
    echo "    latest git tag (stripped 'v'):           $EXPECTED" >&2
    echo "  Bump idf_component.yml version: field to match before tagging." >&2
fi

# === (2) README ABI vs include/urbi/version.h — ALL mentions ===

ABI_MAJOR=$(grep '^#define URBI_API_VERSION_MAJOR' include/urbi/version.h | \
            awk '{print $3}')
ABI_MINOR=$(grep '^#define URBI_API_VERSION_MINOR' include/urbi/version.h | \
            awk '{print $3}')
ABI_PATCH=$(grep '^#define URBI_API_VERSION_PATCH' include/urbi/version.h | \
            awk '{print $3}')
ABI_FULL="${ABI_MAJOR}/${ABI_MINOR}/${ABI_PATCH}"

ABI_MENTIONS=$(grep -oE 'ABI [0-9]+/[0-9]+/[0-9]+' README.md || true)
if [ -z "$ABI_MENTIONS" ]; then
    fail_msg "README.md contains no 'ABI X/Y/Z' string"
else
    while IFS= read -r mention; do
        if [ "$mention" != "ABI $ABI_FULL" ]; then
            fail_msg "README ABI drift: found '$mention'"
            echo "    include/urbi/version.h:  ABI $ABI_FULL" >&2
        fi
    done <<< "$ABI_MENTIONS"
fi

# === (3) README wire format vs uchunk.h — ALL mentions ===

WIRE_MAJOR=$(grep -E '^#define\s+URBI_BYTECODE_VERSION_MAJOR' src/chunk/uchunk.h | \
             awk '{print $3}' | tr -d 'Uu')
WIRE_MINOR=$(grep -E '^#define\s+URBI_BYTECODE_VERSION_MINOR' src/chunk/uchunk.h | \
             awk '{print $3}' | tr -d 'Uu')

if [ -z "$WIRE_MAJOR" ] || [ -z "$WIRE_MINOR" ]; then
    fail_msg "could not extract URBI_BYTECODE_VERSION_MAJOR/MINOR from src/chunk/uchunk.h"
else
    WIRE_STR="v${WIRE_MAJOR}.${WIRE_MINOR}"
    WIRE_MENTIONS=$(grep -oEi '(wire|bytecode) v[0-9]+\.[0-9]+' README.md || true)
    if [ -z "$WIRE_MENTIONS" ]; then
        fail_msg "README.md contains no 'wire vX.Y' or 'bytecode vX.Y' string"
    else
        while IFS= read -r mention; do
            mw=$(echo "$mention" | awk '{print $2}')
            if [ "$mw" != "$WIRE_STR" ]; then
                fail_msg "README wire format drift: found '$mention'"
                echo "    src/chunk/uchunk.h MAJOR/MINOR: $WIRE_STR" >&2
            fi
        done <<< "$WIRE_MENTIONS"
    fi
fi

# === (4) README tag references vs latest tag — ALL urbi-tag mentions ===
#
# Tag suffix can contain mixed case (e.g. v0.10.11-channel-and-isA).
# Lines mentioning ESP-IDF carry the toolchain pin (e.g. "ESP-IDF v6.0.1")
# — those are not urbi release tags and are excluded.

README_TAG_FOUND=0
while IFS=: read -r lineno tagmention; do
    [ -n "$tagmention" ] || continue
    line=$(sed -n "${lineno}p" README.md)
    case "$line" in
        *ESP-IDF*) continue ;;
    esac
    README_TAG_FOUND=1
    if [ "$tagmention" != "$LATEST_TAG" ]; then
        fail_msg "README tag drift at line $lineno: $tagmention"
        echo "    latest git tag:   $LATEST_TAG" >&2
    fi
done < <(grep -noE 'v[0-9]+\.[0-9]+\.[0-9]+(-[A-Za-z0-9-]+)?' README.md || true)

if [ "$README_TAG_FOUND" -eq 0 ]; then
    fail_msg "README.md contains no tag reference (vX.Y.Z[-name])"
fi

# === (5) urbi_version() literal in src/urbi.c vs latest tag ===
#
# refactor-3 GATE-03 (new check): the runtime self-report drifted to
# "0.5.7-fixes" for ~20 tags before refactor-3 API-04 caught it.  The
# unit test pins the literal; this gate pins it against the TAG.

URBI_VERSION_LIT=$(grep -E '^#define URBI_VERSION ' src/urbi.c | \
                   sed -E 's/^#define URBI_VERSION[[:space:]]+"([^"]*)".*/\1/')
if [ -z "$URBI_VERSION_LIT" ]; then
    fail_msg "could not extract '#define URBI_VERSION \"...\"' from src/urbi.c"
elif [ "$URBI_VERSION_LIT" != "$EXPECTED" ]; then
    fail_msg "urbi_version() literal drift"
    echo "    src/urbi.c URBI_VERSION:        \"$URBI_VERSION_LIT\"" >&2
    echo "    latest git tag (stripped 'v'):  \"$EXPECTED\"" >&2
fi

if [ "$fail" -eq 0 ]; then
    echo "OK: version sync clean"
    echo "  Component:     $COMPONENT_VERSION (matches $LATEST_TAG)"
    echo "  ABI:           $ABI_FULL"
    echo "  Wire:          $WIRE_STR"
    echo "  urbi_version:  $URBI_VERSION_LIT"
    exit 0
fi

exit 1
