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

VIOLATIONS=$(grep -nE '^(static[[:space:]]+)?[a-zA-Z_][a-zA-Z0-9_*[:space:]]*[[:space:]][a-zA-Z_][a-zA-Z0-9_]*[[:space:]]*[=;]' src/*.c \
    | grep -vE 'const[[:space:]]' \
    | grep -vE '^\s*[a-zA-Z_][a-zA-Z0-9_]*[[:space:]]*\(' \
    || true)

if [ -n "$VIOLATIONS" ]; then
    printf 'audit-globals: non-const file-scope mutable(s) found:\n%s\n' "$VIOLATIONS" >&2
    printf '\nPer pre-M2 multi-VM audit: every mutable datum must live on UVM.\n' >&2
    printf 'See urbi-embedded/docs/internals/architecture.md "Multi-VM model".\n' >&2
    exit 1
fi

printf 'audit-globals: clean (no non-const file-scope mutables)\n'
exit 0
