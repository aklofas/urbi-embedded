# C API stability policy

> Status: ABI pin at v0.10.10-job-introspection (0/19/1) — PATCH bump
> from v0.10.9-tag-state (0/19/0).  18th use of pre-v1.0 escape clause.
> D7 full-ship Cat. E ratification (Job proto, detach/disown,
> scopeTag, connectionTag) — all script-side; zero new public C API
> symbols.  PATCH-only, not freeze-override under §3 (the ledger
> numbers every bump; only MINOR/MAJOR bumps require §3's freeze-
> override review).  The freeze pin is a forcing function (deliberate
> intent at every bump), not a hard cap.  Any further MINOR/MAJOR
> change after this tag must follow §3.

## 1. The freeze pin

`include/urbi/version.h` contains a `_Static_assert` pinning the public
ABI to `0/19/1`.  Any change to the public C API after v0.10.6-stabilization
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
