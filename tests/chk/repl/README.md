# REPL .chk NDJSON corpus

Golden fixtures driving the v0.9.1 REPL dispatcher end-to-end through
the in-process buffer transport. Run by
`tests/unit/test_repl_chk_corpus.c` (suite `repl_chk_corpus`); only
present when `URBI_ENABLE_REPL=1`.

## Format

```
# Comment lines start with '#'.
# Blank lines are ignored.

> {"id":1,"op":"eval","code":"1+2"}
< "kind":"result"
< "value":"3"
< "kind":"done"
```

Each `>` line is a single NDJSON request sent via the buffer transport.
Each `<` line lists substring tokens that MUST appear in the next
response line (in order — one `<` line consumes exactly one response
line). Multiple substrings can be packed into a single `<` line — they
must all appear, but order within the line is not significant.

To match wildcards (timestamps, lobby ids, addresses), simply omit
those fields from the `<` line and rely on substring matching of the
parts that ARE stable.

## Pragmas

```
@no-auto-session
```

If present on its own line before any `>` request, the runner does NOT
auto-create a default session — the fixture is expected to drive
session creation explicitly (currently unused; reserved for future
2-session fixtures).

## Fixtures

See `*.chk` files in this directory. Spec coverage:
- §6 NDJSON request/response envelopes
- §7 auth / readonly bit
- §9 lobby isolation + wall + handleDisconnect
- §10 introspection + Debug
- §3.4 compile-budget triple
- §14 exit criteria items 6, 8-11, 14, 15
