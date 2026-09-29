# Embedding guide

urbi-embedded is a C99 library. You give it an allocator, it gives you a
scripting runtime with coroutines, reactive watchers and cancellation
tags, and it never touches a thread, a socket or `malloc` on its own.

Everything below compiles: `make test-embedding-guide` extracts every
sample on this page and builds it against the archive.

- [The whole thing at once](#the-whole-thing-at-once)
- [Opening and closing](#opening-and-closing)
- [The allocator](#the-allocator)
- [Running script](#running-script)
- [The step loop](#the-step-loop)
- [Host functions](#host-functions)
- [Host hooks](#host-hooks)
- [Values](#values)
- [Errors](#errors)
- [Realms](#realms)
- [Events and watchers from C](#events-and-watchers-from-c)
- [Tags](#tags)
- [Precompiled bytecode](#precompiled-bytecode)
- [The eval service](#the-eval-service)

## The whole thing at once

One program that does everything an embedder normally needs: open a VM,
register a C function the script can call, run a script that installs a
reactive watcher, drive the scheduler until there is nothing left to do,
and close.

```c
/* STANDALONE EXAMPLE — open, register, run, step to quiescence, close. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "urbi/urbi.h"

/* --- 1. the allocator -------------------------------------------------
 * The only way the VM reaches memory.  realloc-shaped: nbytes == 0 frees.
 * A microcontroller would carve this out of a static array instead. */
static void *host_alloc(void *ptr, size_t nbytes, void *ud)
{
    (void)ud;
    if (nbytes == 0) { free(ptr); return NULL; }
    return realloc(ptr, nbytes);
}

/* --- 2. a host function ------------------------------------------------
 * Scripts call this as `read_sensor()`.  `out` is where the result goes;
 * the return value says whether the call succeeded or threw. */
static int c_read_sensor(UVM *vm, UValue self, UValue *args,
                         uint8_t nargs, UValue *out)
{
    (void)vm; (void)self; (void)args; (void)nargs;
    static int64_t reading = 0;
    reading += 7;                       /* stand-in for real hardware */
    *out = urbi_make_int(reading);
    return URBI_OK;
}

/* --- 3. where the script's output goes ---------------------------------
 * Without a writer, `echo` has nowhere to put anything.  What arrives is
 * the line as the language framed it -- `[00000000] *** threshold
 * crossed` for the echo below -- because a host that is driving a REPL
 * wants the session id and the marker.  Strip them if you are not. */
static void host_writer(void *ud, const char *chan, size_t chan_len,
                        const char *msg, size_t msg_len)
{
    (void)ud; (void)chan; (void)chan_len;
    printf("script: %.*s\n", (int)msg_len, msg);
}

/* --- 4. what the script says -------------------------------------------
 * `at` is a persistent watcher: it fires on the rising edge of its
 * condition, every time the condition goes from false to true.  `every`
 * runs its body on a timer.  Both keep running after the chunk that
 * installed them has returned, which is why the step loop below matters. */
static const char SCRIPT[] =
    "var Realm.level = 0;"
    "at (Realm.level > 20) echo(\"threshold crossed\");"
    "every (1ms) Realm.level = read_sensor();";

int main(void)
{
    UVM *vm = urbi_open(host_alloc, NULL, NULL);
    if (vm == NULL) { fprintf(stderr, "out of memory\n"); return 1; }

    URealm *realm = urbi_realm_main(vm);
    urbi_realm_set_writer(vm, realm, host_writer, NULL);

    /* Bind the C function to a name the script can see.  The two numbers
     * are the minimum and maximum argument counts the VM will accept. */
    if (urbi_register(vm, "read_sensor", c_read_sensor, 0, 0) != URBI_OK) {
        fprintf(stderr, "could not register read_sensor\n");
        urbi_close(vm);
        return 1;
    }

    /* Compile and run.  This returns when the chunk's own statements are
     * done -- the watcher and the timer it installed are still live. */
    UValue result = urbi_make_nil();
    char err[256] = { 0 };
    int rc = urbi_run(vm, realm, SCRIPT, strlen(SCRIPT), "sensor.u",
                      &result, err, sizeof err);
    if (rc != URBI_OK) {
        fprintf(stderr, "script failed: %s\n", err[0] ? err : "(no diagnostic)");
        urbi_close(vm);
        return 1;
    }

    /* The step loop.  urbi_step runs one slice and reports what it found:
     * it ran something, it is idle until a deadline, or it is quiescent
     * and nothing will ever make it runnable again on its own. */
    for (int slices = 0; slices < 10000; slices++) {
        uint64_t next_wake_us = 0;
        int step = urbi_step(vm, 0, &next_wake_us);

        if (step == URBI_STEP_QUIESCENT) break;
        if (step == URBI_STEP_IDLE_UNTIL) {
            /* Nothing to run until next_wake_us.  A real host sleeps here,
             * or waits on a select() with that deadline.  This one just
             * keeps going, because the VM's own clock advances per step
             * when no clock hook is installed. */
            continue;
        }
        /* URBI_STEP_RAN: there is more work; come straight back. */
    }

    urbi_close(vm);
    return 0;
}
```

`urbi_close` takes back everything: realms, strands, objects, loaded
chunks. Every pointer you obtained from the VM is invalid afterwards.

## Opening and closing

```c
/* FRAGMENT — the lifecycle pair.  Declarations only: the harness compiles this
 * against the real headers, so a signature that drifts from them
 * is a redeclaration conflict and fails the build. */

UVM *urbi_open(UVMAllocFn alloc, void *ud, const UVMConfig *config);

void urbi_close(UVM *vm);
```

`UVM` is opaque. `config` may be NULL for the defaults; the two knobs are
`step_budget` and `boot_stdlib` (set the latter to 0 for a VM with no
built-ins at all, which is only useful for measuring the bare core).

`step_budget` is what `urbi_step` spends when its caller passes 0. Leave
it unset and a zero budget keeps its plain meaning, run until nothing is
runnable; set it and every unbudgeted step becomes a bounded slice, which
is what a host on a fixed RTOS tick wants without repeating the number at
each call. An explicit budget always wins.

`urbi_open` returns NULL on out-of-memory **and on a NULL allocator**.
There is no built-in default: a library that calls `malloc` behind an
embedder's back is not embeddable.

A booted VM costs about 70 KB on a 64-bit host, measured on every build
by `tests/probes/boot_heap.c`.

## The allocator

One realloc-shaped callback:

```c
/* FRAGMENT — the three cases an allocator must handle. */
static void *fixed_heap_alloc(void *ptr, size_t nbytes, void *ud)
{
    /* ptr == NULL, nbytes  > 0  -> allocate
     * ptr != NULL, nbytes == 0  -> free, return NULL
     * ptr != NULL, nbytes  > 0  -> reallocate (may move) */
    (void)ud;
    if (nbytes == 0) { free(ptr); return NULL; }
    return realloc(ptr, nbytes);
}
```

On a target with no `malloc`, back it with a static array and a simple
allocator of your choosing. The VM does not care what is underneath, only
that the three cases behave.

## Running script

```c
/* FRAGMENT — compile and run.  Declarations only: the harness compiles this
 * against the real headers, so a signature that drifts from them
 * is a redeclaration conflict and fails the build. */

int  urbi_run(UVM *vm, URealm *realm, const char *src, size_t n, const char *name,
              UValue *out, char *err, size_t errcap);
```

Compiles and runs, and writes the value of the last statement through
`out`. `name` is what diagnostics call the source; NULL renders as
`<stdin>`, which is what the REPL uses.

Two failure shapes, and they are distinguishable:

- `URBI_ERR_COMPILE` — `err` holds a positioned diagnostic
  (`sensor.u:3:11: expected expression`). Nothing ran.
- `URBI_ERR_UNCAUGHT_THROW` — the program ran and threw something nothing
  caught. `urbi_last_error` holds the rendered value.

`urbi_run` returns when the chunk's own statements finish. Anything the
chunk installed — a watcher, a timer, a forked strand — is still live and
needs the step loop.

To call a function value you already hold:

```c
/* FRAGMENT — calling a closure you hold.  Declarations only: the harness compiles this
 * against the real headers, so a signature that drifts from them
 * is a redeclaration conflict and fails the build. */

int  urbi_call(UVM *vm, URealm *realm, UValue callee, UValue recv,
               const UValue *argv, uint8_t argc, UValue *out);
```

## The step loop

```c
/* FRAGMENT — the step loop.  Declarations only: the harness compiles this
 * against the real headers, so a signature that drifts from them
 * is a redeclaration conflict and fails the build. */

int urbi_step(UVM *vm, uint32_t budget, uint64_t *next_wake_us);

bool urbi_has_live_work(UVM *vm);
```

One call runs the run queue until its budget is spent or nothing is
runnable, and returns one of three answers:

| Return | Meaning | What the host does |
|---|---|---|
| `URBI_STEP_RAN` | a strand ran and there is more | call again immediately |
| `URBI_STEP_IDLE_UNTIL` | nothing runnable until `*next_wake_us` | sleep until then, or poll |
| `URBI_STEP_QUIESCENT` | nothing will become runnable on its own | stop, or wait for host input |

`budget` of 0 means `UVMConfig.step_budget`, and when that is 0 too, run
until nothing is runnable. Otherwise each strand the step dispatches gets
a slice of at most 256, and is charged the whole slice if it comes back
runnable, 1 if it parks or dies. Inside a slice only a backward jump
spends budget; calls do not.

A strand hands its slice back at a `;`, unless no other strand is ready
and nothing is pending against it: then it keeps going through up to 64
`;` in a row. So under a budget a lone busy strand advances up to 65
statements per slice where it used to advance one, in a loop with a `;`
in its body as well as in straight-line code; a loop without one is still
cut off by the backward-jump count. A timer that comes due meanwhile, and
a host write made between two steps, are noticed up to 64 statements
later. Strands that are ready together keep their relative order, but
strands that become ready at different times can interleave differently:
two strands whose timers are 1 ms apart used to log
`a1 a2 b1 a3 b2 a4 b3 a5 b4 b5` and now log
`a1 a2 a3 a4 a5 b1 b2 b3 b4 b5`. A pending interrupt injection ends the
run at the next `;`. An unbudgeted step runs until nothing is runnable
and gives the same results as before.

Quiescent does not mean finished. A VM waiting on an event the host has
not emitted yet is quiescent, and stays that way until the host emits it:

```c
/* FRAGMENT — host input, then step, in that order. */
void host_tick(UVM *vm, URealm *realm, urbi_event_id_t sensor_event)
{
    urbi_inject_event(vm, sensor_event, NULL, 0);
    /* Now there is work, so step until there is not. */
    uint64_t next_wake_us = 0;
    while (urbi_step(vm, 0, &next_wake_us) == URBI_STEP_RAN) { }
    (void)realm;
}
```

Write first, step second. Stepping a quiescent VM and then writing leaves
the write unprocessed until something else wakes it.

## Host functions

```c
/* FRAGMENT — binding a C function to a name.  Declarations only: the harness compiles this
 * against the real headers, so a signature that drifts from them
 * is a redeclaration conflict and fails the build. */

int urbi_register(UVM *vm, const char *path, urbi_native_fn fn, uint8_t min_args, uint8_t max_args);
```

`path` is a dotted name: `"read_sensor"` puts it in the global scope,
`"Motor.stop"` puts it on the `Motor` object. The VM checks the argument
count before the call, so the body does not have to.

```c
/* FRAGMENT — a native that takes arguments and can fail. */
static int c_set_speed(UVM *vm, UValue self, UValue *args,
                       uint8_t nargs, UValue *out)
{
    (void)self; (void)nargs;
    if (urbi_value_kind(args[0]) != URBI_VALUE_INT)
        return urbi_throw(vm, "TypeError", "set_speed: expected an Integer");

    int64_t rpm = urbi_value_as_int(args[0]);
    if (rpm < 0 || rpm > 3000)
        return urbi_throw(vm, "RangeError", "set_speed: out of range");

    /* ... drive the hardware ... */
    *out = urbi_make_nil();
    return URBI_OK;
}
```

`urbi_throw` raises a catchable script-level exception with the named
prototype. Return its result directly; do not also write `out`.

Two rules for a native body. It must not call `urbi_step` — the scheduler
is already inside one. And a `UValue` it holds across anything that might
allocate needs rooting: use `urbi_ref` before and `urbi_unref` after, or
restructure so nothing allocates in between.

## Host hooks

Four callbacks, all optional:

```c
/* FRAGMENT — the four host hooks.  Declarations only: the harness compiles this
 * against the real headers, so a signature that drifts from them
 * is a redeclaration conflict and fails the build. */

void urbi_set_clock(UVM *vm, uint64_t (*fn)(void *ud), void *ud);

void urbi_set_diag(UVM *vm, void (*fn)(UVM *vm, void *ud, int level, const char *msg, size_t len), void *ud);

void urbi_set_writer(UVM *vm, void (*fn)(void *ud, const char *chan, size_t chan_len, const char *msg, size_t msg_len), void *ud);

void urbi_set_wake(UVM *vm, void (*fn)(void *ud), void *ud);
```

- **clock** returns microseconds. Without it the VM advances its own
  counter by one microsecond per step, so timers still fire, just not in
  real time. Any program using `sleep` or `every` on real hardware needs
  this.
- **diag** receives runtime diagnostics, including the uncaught throw of
  a strand nobody is waiting on — there is no caller to hand that to, so
  this is the only place it surfaces.
- **writer** receives `echo` and channel output. Per-realm writers
  override it: `urbi_realm_set_writer`.
- **wake** is called when something becomes runnable. A host blocked in
  `select()` uses it to break out.

## Values

`UValue` is a 16-byte tagged value, and everything about it is inline in
`include/urbi/types.h` — no function calls:

```c
/* FRAGMENT — constructing, testing, reading. */
void value_basics(UVM *vm)
{
    UValue n = urbi_make_int(42);
    UValue f = urbi_make_float(1.5);
    UValue b = urbi_make_bool(true);
    UValue s = urbi_make_string(vm, "hello", 5);

    if (urbi_value_kind(n) == URBI_VALUE_INT) {
        int64_t got = urbi_value_as_int(n);
        (void)got;
    }
    (void)f; (void)b; (void)s;
}
```

`urbi_make_string` allocates, so it takes the VM; every other constructor
is pure. `urbi_value_as_*` does no kind check — ask `urbi_value_kind`
first.

For a printable form, `urbi_value_to_string(vm, v, buf, cap)` renders the
way the REPL does.

Reading and writing from C:

```c
/* FRAGMENT — reading and writing from C.  Declarations only: the harness compiles this
 * against the real headers, so a signature that drifts from them
 * is a redeclaration conflict and fails the build. */

int urbi_global_get(UVM *vm, URealm *realm, const char *name, UValue *out);

int urbi_global_set(UVM *vm, URealm *realm, const char *name, UValue v);

int urbi_slot_get(UVM *vm, UValue obj, const char *name, UValue *out);

int urbi_slot_set(UVM *vm, UValue obj, const char *name, UValue v);
```

## Errors

Every public function returns `int`: `URBI_OK` is 0, errors are negative.

```c
/* FRAGMENT — reading back what went wrong. */
void report_failure(UVM *vm)
{
    UErrorInfo info;
    if (urbi_last_error(vm, &info) == URBI_OK && info.message != NULL)
        fprintf(stderr, "urbi: %s\n", info.message);
    urbi_clear_error(vm);
}
```

`urbi_last_error` is cleared on a successful run, so a stale message
cannot be mistaken for a fresh one. One buffer holds it, which matters in
exactly one case: when a detached strand and an awaited strand both throw
in the same pump, the awaited one's message is what a caller reads, and
the detached one's went to the diagnostic hook when it died.

## Realms

A realm is one script world: its own globals, its own strands, its own
writer. Every realm's globals inherits from a single per-VM object that
holds the standard library, so a new realm costs one cell and one object
rather than a copy of everything.

```c
/* FRAGMENT — realms.  Declarations only: the harness compiles this
 * against the real headers, so a signature that drifts from them
 * is a redeclaration conflict and fails the build. */

URealm *urbi_realm_new(UVM *vm);

URealm *urbi_realm_main(UVM *vm);

void    urbi_realm_free(UVM *vm, URealm *realm);
```

Two realms cannot see each other's globals. That is what makes a
per-session REPL, or a per-behaviour sandbox, cheap.

## Events and watchers from C

```c
/* FRAGMENT — events from C.  Declarations only: the harness compiles this
 * against the real headers, so a signature that drifts from them
 * is a redeclaration conflict and fails the build. */

int urbi_event_register(UVM *vm, URealm *realm, const char *name, urbi_event_id_t *out_id);

int urbi_inject_event(UVM *vm, urbi_event_id_t id, const urbi_event_payload_t *payload, size_t n);
```

Register once, at start-up, and keep the id. `urbi_inject_event` is the
one entry point safe to call from outside the step loop — it appends to a
ring the scheduler drains — which makes it the right shape for an
interrupt handler.

```c
/* FRAGMENT — installing a watcher with a C body.  Declarations only: the harness compiles this
 * against the real headers, so a signature that drifts from them
 * is a redeclaration conflict and fails the build. */

int urbi_watch(UVM *vm, URealm *realm, const char *expr,
               int (*cb)(UVM *vm, void *ud, UValue value), void *ud);
```

installs a watcher whose body is a C callback rather than script. The
callback receives the condition's value, and its `int` return is IGNORED
— it is an int only so the typedef matches the rest of the host-callback
family.

```c
/* FRAGMENT — a watcher whose body is C. */
static int on_level_high(UVM *vm, void *ud, UValue value)
{
    (void)vm; (void)value;
    int *crossings = (int *)ud;
    (*crossings)++;
    return URBI_OK;            /* ignored; see above */
}

static int watch_the_level(UVM *vm, URealm *realm, int *crossings)
{
    return urbi_watch(vm, realm, "Realm.level > 20", on_level_high, crossings);
}
```

A watch lives until the realm's connection tag is stopped —
`urbi_tag_stop(vm, urbi_realm_tag(vm, realm))`, which works for the main
realm too.

## Tags

A tag is a cancellation handle:

```c
/* FRAGMENT — tags.  Declarations only: the harness compiles this
 * against the real headers, so a signature that drifts from them
 * is a redeclaration conflict and fails the build. */

int urbi_tag_new(UVM *vm, URealm *realm, const char *name, UValue *out);

int urbi_tag_stop(UVM *vm, UValue tag);

int urbi_tag_block(UVM *vm, UValue tag);

int urbi_tag_unblock(UVM *vm, UValue tag);

int urbi_tag_freeze(UVM *vm, UValue tag);

int urbi_tag_unfreeze(UVM *vm, UValue tag);
```

`stop` unwinds every strand in the tag's scope, running their `finally`
blocks and firing their `leave` handlers on the way out — it is not a
kill. `block` and `freeze` are independent gates: a blocked strand stays
off the run queue until unblocked.

Each realm has a root tag (`urbi_realm_tag`), so stopping that stops
everything in that realm.

## Precompiled bytecode

Compiling on the device costs the parser and the emitter. To avoid it,
compile on the host and ship the bytes:

```c
/* FRAGMENT — precompiled bytecode.  Declarations only: the harness compiles this
 * against the real headers, so a signature that drifts from them
 * is a redeclaration conflict and fails the build. */

int  urbi_compile(UVM *vm, const char *src, size_t n, const char *name,
                  uint8_t **out_bytes, size_t *out_len, char *err, size_t errcap);

int  urbi_load(UVM *vm, URealm *realm, const uint8_t *bytes, size_t n, UValue *out);

void urbi_chunk_free(UVM *vm, uint8_t *bytes, size_t n);
```

The wire format is version-pinned and flavour-pinned: a chunk from an
incompatible build is rejected with `URBI_ERR_BYTECODE_VERSION_MISMATCH`
rather than half-loaded. Build the library with `URBI_BYTECODE_ONLY=1` to
drop the frontend entirely when the device only ever loads bytes.

## The eval service

`include/urbi/repl.h` — not pulled in by `<urbi/urbi.h>`, so an embedder
that does not want a REPL does not get one.

```c
/* FRAGMENT — the eval service.  Declarations only: the harness compiles this
 * against the real headers, so a signature that drifts from them
 * is a redeclaration conflict and fails the build. */
#include <urbi/repl.h>

int  urbi_repl_serve_init(struct UVM *vm, const UReplConfig *cfg,
                          UReplServer **out_server);

int  urbi_repl_register_transport(UReplServer *server, const UTransport *transport);

int  urbi_repl_serve_step(UReplServer *server, uint64_t timeout_us);

void urbi_repl_serve_shutdown(UReplServer *server);
```

It is cooperative: `urbi_repl_serve_step` reads what a transport has,
evaluates it, writes the reply, and returns. No thread, no socket. Its
`timeout_us` is accepted and ignored — the sweep never waits, so pacing
an idle loop is the caller's business, and the caller already owns the
clock.

You supply the transport. A `UTransport` is four fields — a `ctx` and
`read` / `write` / `close` over it — and registering one adopts that
stream as a session with its own realm, its own globals and its own
output, so two clients cannot see each other's variables. The struct is
COPIED at registration; the `ctx` it points at stays yours until `close`
is called on it.

```c
/* FRAGMENT — standing the service up and driving it. */
#include <urbi/repl.h>

/* Whatever the device has behind it; the service never looks. */
int  uart_read(void *ctx, void *buf, size_t n);
int  uart_write(void *ctx, const void *buf, size_t n);
void uart_close(void *ctx);

static int serve_over_uart(UVM *vm, void *uart)
{
    /* The text a session compiles arrives from outside, so cap it. */
    UReplConfig cfg = { 0 };
    cfg.default_budget.max_source_bytes = 64 * 1024;

    UReplServer *repl = NULL;
    int rc = urbi_repl_serve_init(vm, &cfg, &repl);
    if (rc != URBI_OK) return rc;

    UTransport t = { uart, uart_read, uart_write, uart_close };
    rc = urbi_repl_register_transport(repl, &t);
    if (rc != URBI_OK) { urbi_repl_serve_shutdown(repl); return rc; }

    for (;;) {
        urbi_repl_serve_step(repl, 0);      /* never blocks */
        uint64_t next_wake_us = 0;
        if (urbi_step(vm, 0, &next_wake_us) == URBI_STEP_QUIESCENT) {
            /* Nothing to run and nothing pending: wait on the UART. */
        }
    }
}
```

The protocol is newline-delimited JSON, one request per line. See
[internals/repl-service.md](internals/repl-service.md).

The networked server that used to ship with this, and its socket and PTY
transports, are parked until Phase 5.
