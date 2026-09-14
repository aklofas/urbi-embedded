# Assertion Discipline

This document maps every assertion macro in urbi-embedded to its fire conditions,
use case, and the guidance for choosing the right one.

## Macro Reference

| Macro | Host debug | Host release | Freestanding debug | Freestanding release | Bytecode-only |
|---|---|---|---|---|---|
| `URBI_REQUIRE(cond, msg)` | **yes** | **yes** | **yes (via hook)** | **yes (via hook)** | **yes (spin/hook)** |
| `URBI_INTERNAL_ASSERT(cond)` | yes | no | no | no | no |
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

Implementation lives in `src/util/urequire.c`.  The hook storage is a
file-static pointer; it is not thread-safe.  Register the hook once at
startup, before `urbi_open`, and leave it set for the process lifetime.

### `URBI_INTERNAL_ASSERT(cond)` — `src/util/umacros.h`

Debug-only null-trap diagnostic.  Expands to `assert(cond)` when
`__STDC_HOSTED__` is true (which implies a hosted build where `<assert.h>` is
available), and to `((void)0)` everywhere else.

`assert` itself is controlled by `NDEBUG`: if the compiler defines `NDEBUG`
(e.g. `-DNDEBUG` in a release build), `assert` becomes a no-op.  The net
result: `URBI_INTERNAL_ASSERT` fires only on host debug builds.

**Do not** use `URBI_INTERNAL_ASSERT` for invariants whose violation would
cause data corruption or silent incorrect behavior in production — it will not
fire on embedded targets or release builds.

### `assert(cond)` (libc) — banned in new code

Standard C `assert` is unavailable in freestanding builds (`-ffreestanding`),
which is the production environment for every embedded port.  Additionally, it
is silently stripped in any build with `NDEBUG` defined.  Never add new
`assert(cond)` calls to urbi-embedded source; use one of the macros above.

## When to Use Which

```text
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
         └─ Either way → URBI_INTERNAL_ASSERT(cond)
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

/* Call before urbi_open */
urbi_set_require_fail_hook(my_require_fail);
```

For hosts (Linux, macOS) the default behavior (stderr + abort) is sufficient
during development.  For production host daemons, register a hook that logs
to the application logger before calling `abort()`.

## Where they are used

`URBI_REQUIRE` guards the invariants that must catch a bug on a device
with no debugger attached: scheduler-contract preconditions, link-time
configuration agreement, pointer validity before a dereference the caller
cannot have checked. `URBI_INTERNAL_ASSERT` covers the post-condition
sanity checks that are worth running in a debug build and not worth the
bytes anywhere else.

The rule for new code is the flow chart above, and the reason the two
macros exist rather than one is that a freestanding release build has no
`assert` and no `abort` — what it has is the hook, and `URBI_REQUIRE` is
the only macro that reaches it.

## References

- `include/urbi/require.h` — public header (macro + hook API)
- `src/util/urequire.c` — implementation
- `src/util/umacros.h` — `URBI_INTERNAL_ASSERT` + freestanding helpers
- `docs/refactor-1/urbi-embedded-scheduler-audit.md` §F2 — motivation
- `docs/refactor-1/urbi-embedded-runtime-invariants-audit.md` §F2 — OP_CLOSURE hazard
