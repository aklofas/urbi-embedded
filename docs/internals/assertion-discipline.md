# Assertion Discipline

This document maps every assertion macro in urbi-embedded to its fire conditions,
use case, and the guidance for choosing the right one.

## Macro Reference

| Macro | Host debug | Host release | Freestanding debug | Freestanding release | Bytecode-only |
|---|---|---|---|---|---|
| `URBI_REQUIRE(cond, msg)` | **yes** | **yes** | **yes (via hook)** | **yes (via hook)** | **yes (spin/hook)** |
| `URBI_INTERNAL_ASSERT(cond)` | yes | no | no | no | no |
| `URBI_DISPATCH_ASSERT(cond)` | yes | no | no | no | no |
| `assert(cond)` (libc) | yes | no | banned | banned | banned |

**Key:** "yes" = the check fires and aborts/fails on violation.  "no" = expands
to `((void)0)`.  "via hook" = fires; abort behavior is embedder-supplied.

## Macro Definitions

### `URBI_REQUIRE(cond, msg)` — `include/urbi/require.h`

Unconditional invariant check.  Fires in every build mode.  When `cond` is
false:

1. If the embedder registered a hook via `urbi_set_require_fail_hook`, that
   hook is called with `(file, line, cond_str, msg)` and **must not return**.
2. On hosted builds with no hook: `fprintf(stderr, ...)` + `abort()`.
3. On freestanding builds with no hook: infinite spin.  This is an embedder
   defect — freestanding targets **should** always register a hook (typically
   one that triggers a watchdog reset or writes to a debug UART before halting).

Implementation lives in `src/runtime/urequire.c`.  The hook storage is a
file-static pointer; it is not thread-safe.  Register the hook once at
startup, before `urbi_vm_init`, and leave it set for the process lifetime.

### `URBI_INTERNAL_ASSERT(cond)` — `src/runtime/umacros.h`

Debug-only null-trap diagnostic.  Expands to `assert(cond)` when
`__STDC_HOSTED__` is true (which implies a hosted build where `<assert.h>` is
available), and to `((void)0)` everywhere else.

`assert` itself is controlled by `NDEBUG`: if the compiler defines `NDEBUG`
(e.g. `-DNDEBUG` in a release build), `assert` becomes a no-op.  The net
result: `URBI_INTERNAL_ASSERT` fires only on host debug builds.

**Do not** use `URBI_INTERNAL_ASSERT` for invariants whose violation would
cause data corruption or silent incorrect behavior in production — it will not
fire on embedded targets or release builds.

### `URBI_DISPATCH_ASSERT(cond)` — `src/vm/uvm.c` (local macro)

Hot-path debug check defined locally in the VM dispatch loop.  Defined as
`assert(cond)` under `URBI_DEBUG`, and `((void)0)` otherwise.

Per runtime-invariants audit F2: the three `URBI_DISPATCH_ASSERT` guards in
the `OP_CLOSURE` handler verify `omi != NULL`, `omi->proto_instances != NULL`,
and `ic_index` bounds before the `cl->proto_inst` assignment.  These asserts
**compile out in release**, leaving three sequential dereferences unguarded.
W5 of the v0.10.1-invariants arc replaces these with `URBI_REQUIRE`.

**Do not** introduce new `URBI_DISPATCH_ASSERT` sites.  Use `URBI_REQUIRE` for
any check that must survive release.

### `assert(cond)` (libc) — banned in new code

Standard C `assert` is unavailable in freestanding builds (`-ffreestanding`),
which is the production environment for every embedded port.  Additionally, it
is silently stripped in any build with `NDEBUG` defined.  Never add new
`assert(cond)` calls to urbi-embedded source; use one of the macros above.

## When to Use Which

```
Is the invariant load-bearing in production (freestanding / release)?
│
├─ YES → URBI_REQUIRE(cond, msg)
│        Fires in all modes.  Use for state-machine preconditions,
│        pointer-validity guards before dereferences, scheduler-contract
│        checks that must catch bugs on embedded targets.
│
└─ NO  → Is this a hot-path inner loop where the check measurably hurts
│         release performance?
│
         ├─ YES (hot path) → URBI_DISPATCH_ASSERT(cond)  [existing sites only]
         │                   Do NOT add new ones.  Document why the invariant
         │                   is provably correct without the assert in release.
         │
         └─ NO (not hot)  → URBI_INTERNAL_ASSERT(cond)
                            Appropriate for post-condition sanity checks,
                            refcount arithmetic guards, and alignment proofs
                            that are only exercised in debug runs.
```

## Freestanding Hook Registration

Freestanding embedders (RP2040, ESP32, STM32, RISC-V) should register a hook
at startup that matches the target's error-handling strategy:

```c
static void my_require_fail(const char *file, int line,
                             const char *cond, const char *msg)
{
    /* Write to UART debug port */
    uart_printf("URBI_REQUIRE failed: %s:%d: %s -- %s\n",
                file, line, cond, msg);
    /* Trigger watchdog reset or hard fault */
    watchdog_force_reset();
    for (;;) {}   /* never reached, but silence noreturn warnings */
}

/* Call before urbi_vm_init */
urbi_set_require_fail_hook(my_require_fail);
```

For hosts (Linux, macOS) the default behavior (stderr + abort) is sufficient
during development.  For production host daemons, register a hook that logs
to the application logger before calling `abort()`.

## Adoption Plan

`URBI_REQUIRE` was introduced in v0.10.1-invariants Wave 2 (W0).  Adoption at
specific invariant sites occurs across the remaining worktrees in that wave:

- **W4** — link-time guards: `_Static_assert` pairing with `URBI_REQUIRE` for
  run-time reachability checks.
- **W5** — `OP_CLOSURE` dispatch: replaces the three `URBI_DISPATCH_ASSERT`
  sites identified in runtime-invariants audit F2.
- **W7, W8, W9, W10** — scheduler, GC, and VM invariant sites identified in
  the scheduler audit F2.

The scheduler audit F2 ("`Cooperative-Only Invariants Encoded in Prose, Not
Asserts`") is the primary driver: several scheduler-contract preconditions are
documented in comments but not enforced in production builds.  `URBI_REQUIRE`
provides the mechanism; the wave worktrees supply the sites.

## References

- `include/urbi/require.h` — public header (macro + hook API)
- `src/runtime/urequire.c` — implementation
- `src/runtime/umacros.h` — `URBI_INTERNAL_ASSERT` + freestanding helpers
- `src/vm/uvm.c` (local) — `URBI_DISPATCH_ASSERT`
- `docs/refactor-1/urbi-embedded-scheduler-audit.md` §F2 — motivation
- `docs/refactor-1/urbi-embedded-runtime-invariants-audit.md` §F2 — OP_CLOSURE hazard
