# linenoise vendoring ledger

## Upstream

- URL: https://github.com/antirez/linenoise
- Commit: a15597057991fc748b3759cc66e157c9ea8bdfff
- Tag: none (master HEAD at sync time)
- Date: 2026-04-21
- License: BSD-2-Clause (preserved verbatim in linenoise.c and linenoise.h)

## Local patches

None.

## Last reviewed

2026-04-23 — initial vendoring.

## Refresh procedure

1. Fetch the desired upstream commit of `linenoise.h` and `linenoise.c`
   into `tools/`.
2. Re-apply any local patches from the "Local patches" section above.
3. Update this file: commit SHA, tag (if any), date, patches ledger,
   review date.
4. Run `make all urbi-bin && make test` to verify no regressions.
5. Commit with prefix `tools:` and the upstream SHA in the body.
