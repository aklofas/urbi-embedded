# Release Checklist

A repeatable gate for cutting a tagged urbi-embedded release. Run
top-to-bottom; do not tag until every box is checked.

## 1. Branch

- [ ] Work on a `release/vX.Y.Z` branch off `main` (never tag from a dirty tree).

## 2. Gate sweep (host)

- [ ] `make clean && make test` — both runners, the layering gate, the corpus and the probes, 0 failures
- [ ] `make test-asan` / `make test-ubsan` / `make test-gc-stress` / `make test-valgrind` — clean
- [ ] `make test-probes` — the memory numbers printed; nothing above its cap
- [ ] `make test-bench` — the timing probe, on an otherwise idle machine
- [ ] `make test-chk` — `0 vacuous, 0 failed`; SKIPs are parked-component fixtures only
- [ ] `make test-api-manifest` — every exported `urbi_*` documented, frozen surface intact, no new unprefixed global
- [ ] `make test-embedding-guide` — every C sample compiles, 0 skipped, and the complete program runs
- [ ] `make docs-check` — markdownlint + link-check, 0 errors
- [ ] `make releasetest` — the full parallel sweep is green (supersedes the above on a clean machine)

## 3. Cross builds and hardware

PARKED. Every cross target and every hardware port is out of the build
until Phase 5 re-attaches them to the re-founded runtime. When they come
back, so do these boxes: the cross archive builds, a re-flash of each
board's demo, and the evidence appended to
`docs/release/hardware-validation.md`.

The boot-heap probe holds a 64-bit host figure for the same reason. The
32-bit number the spec targets gets measured on the first cross build.

## 4. Documentation current

- [ ] `CHANGELOG.md` has the new version entry, with the probe numbers in it
- [ ] `README.md` status line and targets table reflect this release
- [ ] `docs/internals/` says what the code does — no document describes a subsystem that is gone

## 5. Version transition

- [ ] `include/urbi/version.h` — `MAJOR/MINOR/PATCH` bumped
- [ ] `tests/unit/test_api_version.c` — constants match version.h
- [ ] `components/esp32-idf/idf_component.yml` — `version:` matches the tag-to-be
- [ ] `README.md` — `ABI X/Y/Z`, `wire vN.N`, and tag reference all updated
- [ ] `URBI_RELEASE_TAG_TO_BE=vX.Y.Z make check-version-sync` — passes in the pre-tag window
- [ ] `include/urbi/version.h` — `URBI_RELEASE_STRING` matches the tag with its leading `v` stripped

## 6. Tag + push (irreversible — needs explicit go-ahead)

- [ ] `git checkout main && git merge --ff-only release/vX.Y.Z`
- [ ] `git tag -a vX.Y.Z -m "<summary>"`
- [ ] `make check-version-sync` (now passes — tag exists)
- [ ] `git push origin main && git push origin vX.Y.Z`

## 7. Post-release

- [ ] Write `docs/milestones/vX.Y.Z.md` retrospective
- [ ] Update `STATUS.md` (shipped + next pointer)
- [ ] Clean up the `release/vX.Y.Z` branch
