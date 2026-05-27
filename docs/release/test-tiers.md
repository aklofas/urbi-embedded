# Test tiers

> Defines what runs at each gate level.
> devtest = local iteration loop; releasetest = pre-tag gate; shiptest = pre-publish gate.

## devtest

`make test` — host unit tests + integration tests + .chk corpus fixtures.
Runs in ~30 s on typical development hardware.
Used for iteration: no sanitizers, no cross-compile gates, no coverage.

```sh
make test
```

## releasetest

`make releasetest` — full pre-tag sweep over two phases.

**Phase 1** (parallel, ~90 s):

- Host build variants: `test`, `test-asan`, `test-ubsan`, `test-debug`, `test-switch`
- Static analysis: `lint`, `test-cppcheck`, `test-tidy-strict`, `test-scan-build`
- Freeze gates: `test-abi-freeze`, `test-wire-freeze`
- REPL security: `test-repl-security`
- Freshness: `test-stdlib-bytecode-fresh`
- Dependency pins: `test-dependency-pins`
- Docs: `docs-check`, `test-docstring-coverage`
- Coverage: `coverage` (`--fail-under-line 85` hard gate in Phase 1; see Makefile `coverage` target. GitHub Actions runs the same target with `continue-on-error: true` so a regression does not block CI on already-merged code, but pre-tag `make releasetest` hard-fails. Branch and condition coverage are measured but not gated — v1.x target.)
- Build hygiene: `test-bytecode-only`, `test-freestanding-host`, `test-bake-smoke`
- API surface: `test-gc-roots-coverage`, `test-api-manifest`, `test-aux-symbols`, `test-embedding-guide`, `test-external-embed-iinclude`
- Cross-compile (when toolchains present): pico, esp32s3, stm32f4, arm, riscv

**Phase 2** (sequential, after Phase 1 completes):

- `test-valgrind` — Valgrind memcheck under the full unit + .chk suite
- `test-corpus-sanitize` — corpus sanitizer sweep

Required before any annotated release tag.

```sh
make clean && make releasetest
```

## shiptest

releasetest + manual procedures from [manual-procedures.md](manual-procedures.md):

- ESP32-S3 hardware bring-up smoke (per [hardware-validation.md](hardware-validation.md)).
- Raspberry Pi Pico hardware bring-up smoke.
- STM32F4 hardware bring-up smoke.
- README + CHANGELOG accuracy review against the tag content.
- Tag artifact dry-run (`git tag -a vX.Y.Z -m "..." --dry-run`).
- Release notes review against [release-notes-template.md](release-notes-template.md).

Required only for the v0.10.x → v1.0 publishing milestone and subsequent
stable releases. Not required for v0.10.x interstitial tags.

## Gate count summary (as of v0.10.6-stabilization)

| Tier | Count | Wall-clock |
|---|---|---|
| devtest | ~1970 unit cases + 269 .chk fixtures | ~30 s |
| releasetest Phase 1 | 27 gates | ~90 s |
| releasetest Phase 2 | 2 gates | ~60 s |
| shiptest | releasetest + manual checklist | variable |

The 27-gate Phase 1 count includes the 2 W5 gates (`test-stdlib-bytecode-fresh`,
`test-dependency-pins`) added at v0.10.6-stabilization.
