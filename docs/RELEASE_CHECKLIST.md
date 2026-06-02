# Release Checklist

A repeatable gate for cutting a tagged urbi-embedded release. Run top-to-bottom;
do not tag until every box is checked. The v1.0.0 release used this list (see
`docs/release/release-notes-v1.0.0.md`).

## 1. Branch

- [ ] Work on a `release/vX.Y.Z` branch off `main` (never tag from a dirty tree).

## 2. Gate sweep (host)

- [ ] `make test` — unit + integration + `.chk` fixtures, 0 failures
- [ ] `make test-asan` — AddressSanitizer clean
- [ ] `make test-ubsan` — UBSan clean
- [ ] `make test-stdlib-bytecode-fresh` — baked stdlib blob is byte-fresh
- [ ] `make test-chk` — every `.chk` fixture runs + passes (no silent SKIP)
- [ ] `make test-api-manifest` — exported `urbi_*` documented + frozen surface intact
- [ ] `make test-abi-freeze` — `_Static_assert` ABI pin matches version.h
- [ ] `make test-wire-freeze` — wire-format pin matches `uchunk.h`
- [ ] `make docs-check` — markdownlint + public-doc scrub + link-check, 0 errors
- [ ] `make releasetest` — the full pre-release sweep is green (supersedes the above on a clean machine)

## 3. Cross builds

- [ ] `make cross-pico cross-esp32s3 cross-stm32f4` (and `cross-arm cross-riscv` if toolchains present)
- [ ] `make clone-build-demo-check` — every shipped-port example builds from a pristine tree

## 4. Hardware-in-the-loop (for any release touching the VM / GC / scheduler / stdlib / bytecode)

- [ ] Pico (RP2040) — re-flash `examples/pico/repl_demo`, confirm REPL + watcher LED
- [ ] ESP32-S3 — re-flash `examples/esp32/eye_demo`, confirm continuous run + button cycling
- [ ] STM32F4 — re-flash `examples/stm32f4/mandelbrot`, confirm render + tilt/zoom
- [ ] Append evidence to `docs/release/hardware-validation.md`; bump the "Last verified" rows in `docs/release/release-readiness.md`

## 5. Documentation current

- [ ] `docs/release/conformance-report.md` recomputed (fixture counts, coverage %)
- [ ] `docs/release/release-readiness.md` closure items all ✅ (or explicit accepted known-issues)
- [ ] `CHANGELOG.md` has the new version entry
- [ ] `README.md` status + supported-targets table reflect this release
- [ ] `docs/api-stability.md` escape ledger updated (or, post-1.0, a §3 deprecation entry)

## 6. Version transition

- [ ] `include/urbi/version.h` — `MAJOR/MINOR/PATCH` bumped + `_Static_assert` pin updated
- [ ] `tests/unit/test_api_version.c` — constants match version.h
- [ ] `components/esp32-idf/idf_component.yml` — `version:` matches the tag-to-be
- [ ] `README.md` — `ABI X/Y/Z`, `wire vN.N`, and tag reference all updated
- [ ] `make check-version-sync` — passes (run again after tagging; it compares idf to the latest tag)

## 7. Tag + push (irreversible — needs explicit go-ahead)

- [ ] `git checkout main && git merge --ff-only release/vX.Y.Z`
- [ ] `git tag -a vX.Y.Z -m "<summary>"`
- [ ] `make check-version-sync` (now passes — tag exists)
- [ ] `git push origin main && git push origin vX.Y.Z`

## 8. Post-release

- [ ] Write `docs/milestones/vX.Y.Z.md` retrospective
- [ ] Update `STATUS.md` (shipped + next pointer)
- [ ] Clean up the `release/vX.Y.Z` branch
