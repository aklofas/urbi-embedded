# C API stability policy

> Status: ABI pin at v0.10.14-prerc-infra (0/19/5) — PATCH bump from
> v0.10.13-hygiene (0/19/4).  22nd use of pre-v1.0 escape clause.
> First tag of the pre-v1.0-rc stabilization arc — STYLE.md doc
> correction, REPL output-backpressure liveness fix, and a `.chk`
> C host-driver; no new public C API symbols, no new opcodes.
> PATCH-only, not freeze-override
> under §3 (the ledger numbers every bump; only MINOR/MAJOR bumps
> require §3's freeze-override review).  The freeze pin is a forcing
> function (deliberate intent at every bump), not a hard cap.  Any
> further MINOR/MAJOR change after this tag must follow §3.

## 1. The freeze pin

`include/urbi/version.h` contains a `_Static_assert` pinning the public
ABI to `0/19/3`.  Any change to the public C API after v0.10.6-stabilization
either bumps the macros AND the assert in lockstep (deliberate intent), or
fails to compile.

The freeze does NOT mean "no more ABI changes ever before v1.0."  It means
each change must be deliberate and enumerated.

## 2. What counts as a breaking change

Per the leading comment in `version.h`:

- **MAJOR bump:** removed function, changed signature, removed/renumbered
  enum value, struct-layout change visible across the boundary, removed
  `URBI_ERR_*` slot.
- **MINOR bump:** additive — new function, new enum value appended at the
  next free index, new `URBI_ERR_*` slot, new build flag.
- **PATCH bump:** bug fix only, no header change.

Pre-v1.0 escape clause: while `URBI_API_VERSION_MAJOR == 0`, MINOR or PATCH
bumps MAY break ABI per standard semver convention.  Each bump enumerates
breakages in CHANGELOG.

## 3. Post-freeze breaking change policy

To break the freeze after v0.10.6-stabilization:

1. **CHANGELOG.md entry** — add a numbered "post-freeze break" entry under
   the new tag's section.  Cite this document and explain why the break
   is necessary.
2. **Static-assert bump** — update the expected values in version.h's
   freeze-pin assert.  The build will not compile until this is done.
3. **Macros bump** — update `URBI_API_VERSION_MAJOR/MINOR/PATCH` per §2.
4. **Code review** — at least one reviewer must explicitly OK the freeze
   break.

## 4. What v1.0 promises

The v1.0 ABI promise — once `URBI_API_VERSION_MAJOR` increments to 1 —
is full semantic versioning per §2.  MAJOR breaks require a deprecation
cycle.  MINOR adds may not remove or rename anything.  PATCH bumps must
be header-stable.

The pre-v1.0 escape clause expires at v1.0.0.

## 5. References

- `include/urbi/version.h` — the freeze pin + macros + history.
- `CHANGELOG.md` — per-tag enumeration of breaking changes.
- `docs/api-surface-tiers.md` — public/experimental/advanced tier
  classification of the API surface.

## 6. Post-freeze escape ledger

Summary of every ABI change since the v0.10.6-stabilization freeze pin.
Detail in `include/urbi/version.h` ledger comment and `CHANGELOG.md`.

### Escape #16 — v0.10.6-stabilization (MINOR — freeze-pin tag)

UReplConfig gains `rate_limit_per_second` field (struct-layout change,
additive at tail).  Symbolic freeze pin; declared "16th and FINAL" at
time of writing.  0/17/0 to 0/18/0.

### Escape #17 — v0.10.9-tag-state (MINOR — first post-freeze break)

4 new public C API symbols: `urbi_tag_block` / `_unblock` / `_freeze` /
`_unfreeze`.  UStrand gains `unblock_value` field (+16 B).  UTag gains
`UTAG_FLAG_BLOCKED` (0x04).  D1 SUSPENDED-machinery ratification.
Supersedes "16th and FINAL" framing.  0/18/2 to 0/19/0.

### Escape #18 — v0.10.10-job-introspection (PATCH-only)

D7 full-ship Cat. E ratification: Job proto, detach/disown, scopeTag,
Lobby.connectionTag.  All script-side; zero new public C API symbols.
0/19/0 to 0/19/1.  Recorded for ledger completeness; NOT a
freeze-override under §3.

### Escape #19 — v0.10.11-channel-and-isA (PATCH-only)

D6 + isA + D5 Cat. E ratification, plus bundled v0.10.10 carry-overs
(-MMD -MP auto-deps + test_repl_stop_path UAF fix).  Three new
urbiscript surface constructs (Channel proto, `<<` infix, isA method)
plus one policy change (Object atom proto unfrozen) — all script-side,
no new public C API symbols.  PATCH bump only: 0/19/1 to 0/19/2.
Recorded for ledger completeness; this is NOT a freeze-override
under §3.

### Escape #20 — v0.10.12-cat-e-activation (PATCH-only)

Final tag of the 4-tag Cat. E ratification arc.  Fixture-and-doc-
only tag: D2 cross-spec at→event chain activation (W1), at.sync
keyword normalization (W2), Cat. E close-out (W3).  No new public
C API symbols — only the ABI macro bump.  PATCH bump: 0/19/2 →
0/19/3.  Recorded for ledger completeness; this is NOT a freeze-
override under §3.

### Escape #21 — v0.10.13-hygiene (PATCH-only)

Post-Cat. E hygiene + one targeted runtime bug fix.  Two parallel
worktrees: W2 bundled markdownlint MD004 per-file override for
CHANGELOG, `make all` dep on the urbi CLI binary (closes the
v0.10.7-H stale-binary trap), and a `String.asString` stdlib overlay
(closes v0.10.11-A); W3 suppressed the slot-change emit on first
slot-install (closes v0.10.7-C — install is creation, not change).
The only runtime semantic change is W3's two-site suppression:
removed the direct emit in `urbi_object_set_local_slot` Case 2 (leaf-
shape-add path), and added a shape-snapshot gate in `vm_setslot_slow`
that suppresses emit when `recv->shape` changes (= new local slot
installed via the COW or miss-install path).  No new public C API
symbols.  Two new chk fixtures (`string_asstring`,
`slot_change_no_install_emit`), one replaced unit test.  PATCH bump:
0/19/3 → 0/19/4.  Recorded for ledger completeness; this is NOT a
freeze-override under §3.

### Escape #22 — v0.10.14-prerc-infra (PATCH-only)

First tag of the pre-v1.0-rc stabilization arc.  Three file-isolated
worktrees: W1 corrected the stale flat-layout claims in `docs/STYLE.md`;
W2 reworked the REPL reader-thread output flush from an EAGAIN
`nanosleep` spin to a per-session staging buffer + `POLLOUT`-driven
retry (a teardown-latency/liveness fix — the prior loop did not drop
bytes), with a new `test_repl_backpressure.c` regression suite; W3
added a C `.chk` host-driver (`tests/integration/chk_host_driver.c`)
with `## host: realm`/`run`/`step` directives over the existing public
embedding API, activating 5 previously-blocked scheduler/multi-realm
conformance fixtures.  No new public C API symbols — all changes are
internal (REPL), test-side (host-driver), or documentation.  PATCH
bump: 0/19/4 → 0/19/5.  Recorded for ledger completeness; this is NOT
a freeze-override under §3.
