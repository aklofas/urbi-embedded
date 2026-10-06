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
- Freshness: `test-stdlib-bytecode-fresh`
- Docs: `docs-check`
- Coverage: `coverage` (`--fail-under-line 80` hard gate in Phase 1; see Makefile `coverage` target. Lowered from 85% in Phase 0 of the core re-foundation — the deleted runtime-internals unit tests covered code that the refound/core branch replaces; re-baseline for the new core when it lands. GitHub Actions runs the same target with `continue-on-error: true` so a regression does not block CI on already-merged code, but pre-tag `make releasetest` hard-fails. Condition coverage is not measured — v1.x target.)
- Build hygiene: `test-bytecode-only`, `test-freestanding-host`, `test-bake-smoke`
- API surface: `test-api-manifest`, `test-aux-symbols`, `test-embedding-guide`, `test-external-embed-iinclude`
- Cross-compile: `cross-all` builds every preset (`arm-cortex-m0plus`,
  `arm-cortex-m4f`, `arm-cortex-m7`, `riscv32`) full and bytecode-only,
  running `test-freestanding`'s archive-self-containedness check on each
  bytecode-only shape as it builds; `test-cross-missing-toolchain`
  checks the failure mode for a preset whose compiler is absent. A
  preset's toolchain not being on `PATH` is a hard error, not a skip.
  `test-flagstamp-toolchain` checks that a different compiler behind the
  same name rebuilds the archive.

`make test-probes-32bit` (the qemu Cortex-M4 memory probes) is CI-only —
it needs `arm-none-eabi-gcc` and `qemu-system-arm`, neither of which
`releasetest` requires.

`make pico-repl-demo` (the Raspberry Pi Pico example firmware) is also
CI-only — it needs pico-sdk, which `releasetest` does not require.

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
| releasetest Phase 1 | 40 gates | ~90 s |
| releasetest Phase 2 | 2 gates | ~60 s |
| shiptest | releasetest + manual checklist | variable |

The 40-gate Phase 1 count includes the W5 gate (`test-stdlib-bytecode-fresh`)
added at v0.10.6-stabilization.
