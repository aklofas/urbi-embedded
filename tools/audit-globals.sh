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
VIOLATIONS=$(find src -name '*.c' -print0 \
    | xargs -0 grep -nE '^(static[[:space:]]+)?[a-zA-Z_][a-zA-Z0-9_*[:space:]]*[[:space:]][a-zA-Z_][a-zA-Z0-9_]*[[:space:]]*[=;]' \
    | grep -vE 'const[[:space:]]' \
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

printf 'audit-globals: clean (no non-const file-scope mutables)\n'
exit 0
