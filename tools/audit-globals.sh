#!/bin/sh
# SPDX-License-Identifier: BSD-3-Clause
# Reject non-const file-scope mutables in src/*.c. Per pre-M2 multi-VM
# audit decision: every mutable datum must live on UVM, not in
# file-scope storage.
#
# Allowed file-scope: `static const ...`, `extern const ...`, function
# definitions, type definitions, preprocessor macros.
#
# Exit 0 = clean. Exit 1 = violations found.

set -u

cd "$(dirname "$0")/.."

# Match `static <type> <ident>` or `<type> <ident>` at column zero in
# *.c files where the type does NOT contain "const" and the line does
# not look like a function definition.
#
# Accepted patterns:  static const, extern const, typedef, function defs.
# Rejected:           static int counter; static UVM *current; etc.

# refactor-3 GATE-05: scan src/**/*.c recursively (was top-level src/*.c only
# — the gated REPL/ROS/urobotics subdirectories were never checked).
# `typedef` lines are structurally global-free; `audit-globals-allow:` is the
# blessed-exception marker (each carries a rationale + fixing-wave pointer).
#
# Review fixes (v0.13.0):
#   - The main scan also matches anonymous-aggregate globals
#     (`static struct { ... } name[N];`) — the old simple-declaration
#     pattern could not cross the brace body, so g_protos slipped through.
#   - The const exclusion is anchored to the declaration start: only a
#     declaration that is ITSELF const-qualified is skipped.  A "const"
#     anywhere on the line (e.g. inside a struct body or a comment) no
#     longer launders a mutable global.
#   - `grep -H` everywhere: a single-file xargs batch must not drop the
#     `file:` prefix the exclusion regexes anchor on.
VIOLATIONS=$(find src -name '*.c' -print0 \
    | xargs -0 grep -HnE '^(static[[:space:]]+)?([a-zA-Z_][a-zA-Z0-9_*[:space:]]*[[:space:]][a-zA-Z_][a-zA-Z0-9_]*[[:space:]]*[=;]|(struct|union)[[:space:]]*\{.*\}[[:space:]]*[a-zA-Z_][a-zA-Z0-9_]*([[:space:]]*\[[^][]*\])*[[:space:]]*[=;])' \
    | grep -vE '^[^:]*:[0-9]+:[[:space:]]*(static[[:space:]]+)?(extern[[:space:]]+)?const[[:space:]]' \
    | grep -vE '^[^:]*:[0-9]+:(static[[:space:]]+)?typedef[[:space:]]' \
    | grep -vE 'audit-globals-allow:' \
    | grep -vE '^[^:]*:[0-9]+:[[:space:]]*[a-zA-Z_][a-zA-Z0-9_]*[[:space:]]*\(' \
    || true)

if [ -n "$VIOLATIONS" ]; then
    printf 'audit-globals: non-const file-scope mutable(s) found:\n%s\n' "$VIOLATIONS" >&2
    printf '\nPer pre-M2 multi-VM audit: every mutable datum must live on UVM.\n' >&2
    printf 'See urbi-embedded/docs/internals/architecture.md "Multi-VM model".\n' >&2
    exit 1
fi

# Pin the blessed-exception count.  Every `audit-globals-allow:` marker is a
# deliberately accepted process-global; a new one must not appear silently
# (e.g. pasted from an example) and a removed one means a global was either
# fixed (good — shrink the pin) or un-marked while still present (bad).
EXPECTED_ALLOW_MARKERS=8
ACTUAL_ALLOW_MARKERS=$(find src \( -name '*.c' -o -name '*.h' \) -print0 \
    | xargs -0 grep -Hn 'audit-globals-allow:' \
    | wc -l)
if [ "$ACTUAL_ALLOW_MARKERS" -ne "$EXPECTED_ALLOW_MARKERS" ]; then
    printf 'audit-globals: audit-globals-allow marker count drift: found %s, pinned %s.\n' \
        "$ACTUAL_ALLOW_MARKERS" "$EXPECTED_ALLOW_MARKERS" >&2
    printf 'Blessing a new global (or fixing one) is fine — but do it deliberately:\n' >&2
    printf 'update EXPECTED_ALLOW_MARKERS in tools/audit-globals.sh in the same commit\n' >&2
    printf 'and make sure the marker carries a rationale + fixing-wave pointer.\n' >&2
    exit 1
fi

printf 'audit-globals: clean (no non-const file-scope mutables; %s blessed audit-globals-allow markers)\n' \
    "$ACTUAL_ALLOW_MARKERS"
exit 0
