# Migration: Block comments — nested to non-nesting

Legacy urbiscript supported nested block comments: an opening `/*` inside an
already-open block comment did not close the outer comment, so
`/* outer /* inner */ outer */` was valid and the entire span — including both
`outer */` strings — was treated as one comment.  urbi-embedded uses C-style
non-nesting block comments instead (see
[LANG-CONVENTIONS.md §7](../LANG-CONVENTIONS.md#7-block-comments--divergence-from-legacy)):
the first `*/` sequence closes the comment regardless of any `/*` sequences
that appear inside it.

**Migration recipe:** Audit legacy source for `/*` appearing inside an
existing block comment.  In practice this pattern is rare — production
urbiscript codebases rarely nested comments intentionally; the legacy feature
was mostly used to comment-out code that itself contained a `/* ... */` span.
For each nested instance, choose one of:

- Replace the inner block comment with a line comment (`// ...`) so the outer
  `/* ... */` survives unchanged.
- Restructure the outer comment so the inner span is not comment-delimited
  (e.g. describe the code in prose rather than quoting it verbatim).
- Split the outer comment into two non-overlapping block comments.

No automated tool is provided; the number of affected sites in real codebases
is expected to be zero or one.
