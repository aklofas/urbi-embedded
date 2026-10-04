# C API stability policy

There is no API or ABI compatibility promise before 1.0.0. The runtime core is
being re-founded; the 1.0.0 tag will be the first freeze. The sections below
describe what that freeze will promise.

## 2. What counts as a breaking change

Per the leading comment in `version.h`:

- **MAJOR bump:** removed function, changed signature, removed/renumbered
  enum value, struct-layout change visible across the boundary, removed
  `URBI_ERR_*` slot.
- **MINOR bump:** additive — new function, new enum value appended at the
  next free index, new `URBI_ERR_*` slot, new build flag, or a new
  trailing field appended to a config or stats struct (`v0.16.0-shell`
  added `heap_budget` to both `UVMConfig` and `UGcStats` this way, with
  no symbol change).
- **PATCH bump:** bug fix only, no header change.

Pre-v1.0 escape clause: while `URBI_API_VERSION_MAJOR == 0`, MINOR or PATCH
bumps MAY break ABI per standard semver convention.  Each bump enumerates
breakages in CHANGELOG.

## 4. What v1.0 promises

The v1.0 ABI promise — once `URBI_API_VERSION_MAJOR` increments to 1 —
is full semantic versioning per §2.  MAJOR breaks require a deprecation
cycle.  MINOR adds may not remove or rename anything.  PATCH bumps must
be header-stable.

The pre-v1.0 escape clause expires at v1.0.0.

## 5. The native-function protocol

There is one callback type for host code that script can call,
`urbi_native_fn` in `include/urbi/urbi.h`:

```c
typedef int (*urbi_native_fn)(UVM *vm, UValue self, UValue *args,
                              uint8_t nargs, UValue *out);
```

The argument count is `uint8_t nargs` (range 0–255), matching the
bytecode CALL opcode, which packs the count into a single byte.  A native
returns `UEXEC_OK` with its result in `*out`, or raises with `urbi_throw`
and returns what it returned (`UEXEC_THROW`).

A native becomes a script-visible value by being installed under a path
with `urbi_register`; the host reads it back as a first-class callable
with `urbi_global_get` / `urbi_slot_get` and may pass it to `urbi_call`.
There is no separate "wrap a C function as a closure" constructor.

## 6. References

- `include/urbi/version.h` — the version macros.
- `CHANGELOG.md` — per-tag enumeration of breaking changes.
- `docs/api-surface-tiers.md` — public/experimental/advanced tier
  classification of the API surface.
