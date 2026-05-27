#!/usr/bin/env bash
# tests/scripts/check_docstring_coverage.sh — fail if any function declaration
# in a public-API or subsystem-public header lacks an immediately preceding
# /* ... */ block comment (or // line comment).
#
# Scope: include/urbi/*.h (public API) plus src/*/u<subsys>.h (subsystem-public
# headers).  Skips _internal.h (intentionally private inter-TU API) and
# umacros.h (macro-only helper bag).
#
# A docstring "cascades" through a contiguous run of declarations: a comment
# above the first decl in a group covers later decls in the same group as
# long as no blank line, function definition, or non-decl content intervenes.
# Forward declarations (struct X; typedef ... X;) and callback typedefs
# (typedef R (*F)(...);) do not break the cascade — they typically sit
# between a docstring and the function decl that uses the type.
#
# See CONTRIBUTING.md for coverage policy.
set -eu

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
missing=0

HEADERS=$(ls "$ROOT"/include/urbi/*.h "$ROOT"/src/*/u*.h 2>/dev/null \
            | grep -vE '_internal\.h$' \
            | grep -vE '/umacros\.h$' \
            | LC_ALL=C sort)

for h in $HEADERS; do
  rel="${h#$ROOT/}"
  result=$(awk -v fname="$rel" '
    function reset_doc() { has_doc = 0 }
    function strip_comments(s,    out, i, c, in_block) {
      out = ""; in_block = 0
      for (i = 1; i <= length(s); i++) {
        c = substr(s, i, 1)
        if (in_block) {
          if (c == "*" && substr(s, i+1, 1) == "/") { in_block = 0; i++ }
        } else {
          if (c == "/" && substr(s, i+1, 1) == "*") { in_block = 1; i++ }
          else if (c == "/" && substr(s, i+1, 1) == "/") { return out }
          else out = out c
        }
      }
      return out
    }
    BEGIN { has_doc = 0; in_bc = 0; brace_depth = 0; in_macro = 0 }

    in_bc == 1 { if ($0 ~ /\*\//) { in_bc = 0; has_doc = 1 } next }
    in_macro == 1 {
      if ($0 ~ /\\[[:space:]]*$/) next
      else { in_macro = 0; next }
    }

    /^[[:space:]]*\/\*.*\*\/[[:space:]]*$/ { has_doc = 1; next }
    /^[[:space:]]*\/\*/ { if (brace_depth > 0) next; in_bc = 1; next }
    /^[[:space:]]*\/\// { if (brace_depth == 0) has_doc = 1; next }
    /^[[:space:]]*$/ { reset_doc(); next }
    /^[[:space:]]*#/ { if ($0 ~ /\\[[:space:]]*$/) in_macro = 1; next }

    # Forward declarations and typedef-aliases do not break the cascade.
    /^[[:space:]]*struct[[:space:]]+[a-zA-Z_][a-zA-Z0-9_]*[[:space:]]*;[[:space:]]*$/ { next }
    /^[[:space:]]*typedef[[:space:]]+[^()]*[[:space:]]+[a-zA-Z_][a-zA-Z0-9_]*[[:space:]]*;[[:space:]]*$/ { next }
    /^[[:space:]]*typedef[[:space:]]+[^;]*\(\*[[:space:]]*[a-zA-Z_][a-zA-Z0-9_]*[[:space:]]*\)[^;]*;[[:space:]]*$/ { next }

    {
      stripped = strip_comments($0)
      ob = gsub(/\{/, "{", stripped)
      cb = gsub(/\}/, "}", stripped)
    }

    brace_depth > 0 {
      brace_depth += ob - cb
      if (brace_depth < 0) brace_depth = 0
      reset_doc(); next
    }

    /\)[[:space:]]*;[[:space:]]*$/ {
      line = $0
      if (line ~ /\(\*[[:space:]]*[a-zA-Z_][a-zA-Z0-9_]*[[:space:]]*\)/) {
        brace_depth += ob - cb; reset_doc(); next
      }
      if (line ~ /^[[:space:]]*typedef/) {
        brace_depth += ob - cb; reset_doc(); next
      }
      if (line !~ /[a-zA-Z_][a-zA-Z0-9_]*[[:space:]]*\([^=]*\)[[:space:]]*;[[:space:]]*$/) {
        brace_depth += ob - cb; reset_doc(); next
      }
      if (line !~ /\(/) {
        brace_depth += ob - cb; reset_doc(); next
      }
      if (!has_doc) {
        print fname ":" NR ": " line
      }
      brace_depth += ob - cb
      next
    }
    /^[a-zA-Z_].*\)[[:space:]]*\{[[:space:]]*$/ {
      brace_depth += ob - cb; reset_doc(); next
    }
    /^[[:space:]]*\{[[:space:]]*$/ {
      brace_depth += ob - cb; reset_doc(); next
    }
    /[a-zA-Z_][a-zA-Z0-9_]*[[:space:]]*\(/ {
      brace_depth += ob - cb; next
    }
    {
      brace_depth += ob - cb
      if (brace_depth < 0) brace_depth = 0
      reset_doc()
    }
  ' "$h" 2>/dev/null || true)
  if [ -n "$result" ]; then
    echo "$result"
    cnt=$(echo "$result" | wc -l)
    missing=$((missing + cnt))
  fi
done

if [ "$missing" -gt 0 ]; then
  echo "test-docstring-coverage: $missing public/subsystem-public function declaration(s) missing docstrings" >&2
  exit 1
fi
echo "test-docstring-coverage: OK"
