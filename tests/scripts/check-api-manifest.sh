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

# Exported symbols from both libraries.  refactor-3 GATE-04: widened from
# ' T urbi_' to all [TDRB] (text, data, read-only data, BSS) symbols so
# exported DATA (urbi_stdlib_bytecode, URBI_DEFAULT_REPL_BUDGET, ...) is
# tracked too.  Review fix (v0.13.0): also [WVCu] — weak, weak-object,
# common, and unique symbols are exported and linkable just the same
# (urepl_dispatch_drain_if_active is exported W and bypassed the gate).
LIBS=("$LIB")
if [ -f "$LIBAUX" ]; then
    LIBS+=("$LIBAUX")
fi
ALL_EXPORTED=$(nm "${LIBS[@]}" \
    | grep -E ' [TDRBWVCu] ' \
    | awk '{print $NF}' \
    | sort -u \
    || true)
EXPORTED=$(echo "$ALL_EXPORTED" | grep -E '^(urbi|URBI)_' || true)
FOREIGN=$(echo "$ALL_EXPORTED" | grep -vE '^(urbi|URBI)_' || true)

if [ -z "$EXPORTED" ]; then
    echo "FAIL: nm returned no urbi_ symbols from ${LIBS[*]} — nm may have failed or the archive is empty" >&2
    exit 1
fi

# refactor-3 GATE-04: every global NOT in the urbi_/URBI_ namespace must be
# enumerated in the temporary allowlist.  The allowlist is a RATCHET: it
# freezes today's unprefixed-global set (the refactor-3 XC-01 inventory) so
# any NEW unprefixed global fails immediately.  v0.13.6 (namespace day)
# renames/statics these; v0.13.7 (API-23) deletes the allowlist.
ALLOWLIST="tests/scripts/api-manifest-symbol-allowlist.txt"
if [ ! -f "$ALLOWLIST" ]; then
    echo "FAIL: $ALLOWLIST not found" >&2
    exit 1
fi
ALLOWED=$(grep -vE '^[[:space:]]*(#|$)' "$ALLOWLIST" | sort -u)
NEW_FOREIGN=$(comm -23 <(echo "$FOREIGN") <(echo "$ALLOWED"))
if [ -n "$NEW_FOREIGN" ]; then
    echo "FAIL: NEW exported symbols outside the urbi_/URBI_ namespace (not in $ALLOWLIST):" >&2
    echo "$NEW_FOREIGN" | sed 's/^/  - /' >&2
    echo "" >&2
    echo "  Internal symbols must be static or carry a u*/urbi_ prefix (STYLE.md)." >&2
    echo "  Do NOT extend the allowlist — it is a shrinking ratchet scheduled for" >&2
    echo "  deletion at v0.13.7 (refactor-3 XC-01 / API-23)." >&2
    exit 1
fi

# Symbols listed in the manifest (backtick-quoted `urbi_*` / `URBI_*`
# patterns).  Pattern allows mixed-case and digits (e.g. urbi_encode_utf8,
# urbi_lobby_invoke_handleDisconnect, URBI_DEFAULT_REPL_BUDGET).
MANIFESTED=$(grep -oE '`(urbi|URBI)_[A-Za-z0-9_]+`' "$MANIFEST" \
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

# === Bidirectional check (v1.0 / B6a) ===
# The forward check above catches a symbol exported-but-undocumented.  The
# reverse direction matters once the public surface is FROZEN at v1.0: a
# documented Tier-1/Tier-2 symbol that silently STOPS being exported (deleted
# or accidentally hidden) must also fail.  Restricted to Tier 1 + Tier 2 (the
# stable/frozen tiers) and skips conditional subsections the build may legally
# omit — `### ...inline...` (never T symbols) and `### ...URBI_ENABLE_REPL...`
# / `### ...URBI_DEBUG...` (conditional builds).  Tier 3 (experimental) and
# Tier 4 (internal-leak, now -fvisibility=hidden) are intentionally excluded.
FROZEN=$(awk '
    /^## Tier 1/ { intier=1; skip=0; next }
    /^## Tier 2/ { intier=1; skip=0; next }
    /^## Tier 3/ { intier=0 }
    /^## Tier 4/ { intier=0 }
    /^### / {
        skip = ( $0 ~ /inline/ || $0 ~ /URBI_ENABLE_REPL/ || $0 ~ /URBI_DEBUG/ ) ? 1 : 0
        next
    }
    intier && !skip {
        while (match($0, /`urbi_[A-Za-z0-9_]+`/)) {
            s = substr($0, RSTART+1, RLENGTH-2)
            # Skip the documented URBI_DEBUG-only trio (declared in public
            # headers but compiled out of the default build — see the manifest
            # "Note:" paragraph).  These are not T symbols in any default build.
            if (s != "urbi_in_isr" && s != "urbi_get_determinism_checksum" \
                && s != "urbi_call_host_with_watchdog") {
                print s
            }
            $0 = substr($0, RSTART+RLENGTH)
        }
    }
' "$MANIFEST" | sort -u)

# Frozen symbols documented but not exported (the surface silently shrank).
MISSING=$(comm -23 <(echo "$FROZEN") <(echo "$EXPORTED"))
if [ -n "$MISSING" ]; then
    echo "FAIL: frozen Tier-1/Tier-2 symbols documented in $MANIFEST but NOT exported from the library:" >&2
    echo "$MISSING" | sed 's/^/  - /' >&2
    echo "" >&2
    echo "  The v1.0 public ABI surface must not shrink.  Either the symbol was" >&2
    echo "  removed (a MAJOR ABI break — revert or bump MAJOR) or it was moved out" >&2
    echo "  of a public include/urbi/*.h header (restore its declaration), or it" >&2
    echo "  is genuinely conditional and belongs under a skipped subsection." >&2
    exit 1
fi

FROZEN_COUNT=$(echo "$FROZEN" | wc -l)
echo "PASS: all $FROZEN_COUNT frozen Tier-1/Tier-2 symbols are still exported (surface did not shrink)"
