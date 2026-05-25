#!/usr/bin/env bash
# SPDX-License-Identifier: BSD-3-Clause
# check-api-manifest.sh — verify docs/api-surface-tiers.md covers all
# urbi_ symbols exported from liburbi.a and liburbi_aux.a.
#
# Usage: check-api-manifest.sh [BUILDDIR]
#   BUILDDIR defaults to build/host.
#
# Fails if any symbol exported from liburbi.a / liburbi_aux.a is not
# listed in docs/api-surface-tiers.md.
#
# Manifested symbols that are NOT exported (inline functions, URBI_DEBUG-only
# functions, REPL-conditional functions) are acceptable — the check is
# one-directional to avoid false failures on conditional builds.
#
# New public symbols require a PR-review-touch on docs/api-surface-tiers.md.

set -euo pipefail

BUILDDIR="${1:-build/host}"
LIB="${BUILDDIR}/liburbi.a"
LIBAUX="${BUILDDIR}/liburbi_aux.a"
MANIFEST="docs/api-surface-tiers.md"

if [ ! -f "$LIB" ]; then
    echo "FAIL: $LIB not found — run 'make' first" >&2
    exit 1
fi
if [ ! -f "$MANIFEST" ]; then
    echo "FAIL: $MANIFEST not found" >&2
    exit 1
fi

# Exported urbi_ symbols from both libraries (T symbols only).
LIBS=("$LIB")
if [ -f "$LIBAUX" ]; then
    LIBS+=("$LIBAUX")
fi
EXPORTED=$(nm "${LIBS[@]}" \
    | grep ' T urbi_' \
    | awk '{print $NF}' \
    | sort -u \
    || true)

if [ -z "$EXPORTED" ]; then
    echo "FAIL: nm returned no urbi_ symbols from ${LIBS[*]} — nm may have failed or the archive is empty" >&2
    exit 1
fi

# Symbols listed in the manifest (backtick-quoted `urbi_*` patterns).
# Pattern allows mixed-case and digits (e.g. urbi_encode_utf8,
# urbi_lobby_invoke_handleDisconnect).
MANIFESTED=$(grep -oE '`urbi_[A-Za-z0-9_]+`' "$MANIFEST" \
    | tr -d '`' \
    | sort -u)

# Symbols exported but not in the manifest.
UNDOCUMENTED=$(comm -23 \
    <(echo "$EXPORTED") \
    <(echo "$MANIFESTED"))

if [ -n "$UNDOCUMENTED" ]; then
    echo "FAIL: symbols exported from liburbi.a but missing from $MANIFEST:" >&2
    echo "$UNDOCUMENTED" | sed 's/^/  - /' >&2
    echo "" >&2
    echo "  Add each symbol to the appropriate tier section in $MANIFEST." >&2
    echo "  T4 (Internal-leak) is the correct tier for symbols not declared" >&2
    echo "  in any include/urbi/*.h header." >&2
    exit 1
fi

EXPORTED_COUNT=$(echo "$EXPORTED" | wc -l)
MANIFESTED_COUNT=$(echo "$MANIFESTED" | wc -l)
echo "PASS: all $EXPORTED_COUNT exported urbi_ symbols are documented in $MANIFEST ($MANIFESTED_COUNT total manifest entries)"
