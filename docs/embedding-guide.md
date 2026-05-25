# urbi-embedded Embedding Guide

urbi-embedded is a lightweight, embeddable runtime for urbiscript — a parallel, event-driven scripting language for robotics and reactive control systems. This guide covers how to embed the runtime in a C application, from the minimal viable setup through advanced topics such as ISR-safe event injection, host function registration, tag-based cancellation, and reference management.

All public API symbols are declared in `<urbi/urbi.h>`, `<urbi/types.h>`, `<urbi/aux.h>`, and `<urbi/version.h>`. Never include headers from `src/` — those are internal and carry no stability guarantee.

---

## Status of this guide

**This guide is accurate for the current API shape but contains deprecated
patterns that will be corrected in a future release.**

Today (pre-v1.0), embedders who want to stack-allocate `struct UVM` must
include `src/vm/uvm.h` and pass `-Isrc` at compile time. This is a known
limitation: the public headers only forward-declare `struct UVM`; they do
not expose its full definition. The proper opaque allocation API
(`urbi_vm_create()` / `urbi_vm_free()`) is planned for the v0.10.x
architectural refactor arc (Wave 4 W6). Until that lands:

- Snippets in this guide that include `vm/uvm.h` or `stdlib/stdlib_boot.h`
  from `src/` are **marked DEPRECATED**. They reflect today's only available
  pattern, not the intended public API.
- All such snippets pass `-Isrc` at compile time. That flag and those
  headers will become unnecessary once the opaque API ships.
- `urbi_stdlib_boot()` is now called automatically inside
  `urbi_realm_global()`. Embedder code that calls it explicitly is harmless
  today but redundant, and the symbol is not declared in any public header
  (it lives in `src/stdlib/stdlib_boot.h`). Remove it from new code.

The authoritative statement of what is and is not public API remains line 5
of this document: **never include headers from `src/` in production code**.
The deprecated snippets below violate that rule as a temporary workaround
only.

---

## Vocabulary

The runtime uses two nouns:

- **chunk** — a unit submitted to the runtime: a `.u` file, a REPL line,
  or the argument to `urbi_run_chunk`. Concretely a `UProto *` that
  happens to be a tree root.
- **proto** — the immutable bytecode unit per function definition.
  Lua-family precedent (Lua's `Proto`, mruby's `mrb_irep`).

A chunk is just the root proto of a tree. There is no separate `UChunk`
type; the noun is conceptual, surfaced in API names (`urbi_run_chunk`,
`urbi_chunk_from_bytes`).

"Module" is retired as a runtime noun in v0.9.2. Pre-v0.9.2 embedders
may see `UModule`-prefixed names in older docs or guides — these are
gone in v0.9.2; see the rename diff table below.

### v0.9.1 → v0.9.2 rename diff

| v0.9.1 | v0.9.2 |
|---|---|
| `UModule` (struct) | *deleted; UProto absorbs root fields* |
| `UModuleInstance` | `UChunkInstance` |
| `UModuleAllocFn` | `UChunkAllocFn` (`UAllocFn` was already taken by `uarena.h`) |
| `UModuleLoadError` | `UChunkLoadError` |
| `ULOAD_*` enum values | `UCHUNK_LOAD_*` |
| `urbi_load_module` | `urbi_load_chunk` |
| `urbi_module_from_bytes` | `urbi_chunk_from_bytes` |
| `urbi_module_free` | `urbi_chunk_free` |
| `urbi_module_instance_create` | `urbi_chunk_instance_create` |
| `urbi_module_instance_destroy` | `urbi_chunk_instance_destroy` |
| `urbi_get_or_create_module_instance` | `urbi_get_or_create_chunk_instance` |
| `urbi_load_translate_load_err` | `urbi_chunk_translate_load_err` |

Other public APIs (`urbi_run_chunk`, `urbi_run_script`, `urbi_unload`)
keep their names. Argument types: any `UModule *` parameter is now
`UProto *`. Wire format bumped v1.7 → v1.8 (semantic version bump; byte
layout unchanged). ABI 0/12/0 → 0/13/0.

---

## 1. Quick Start

The minimum viable embedding: allocate a VM, initialize it with a heap allocator, run a script, then destroy it.

> **DEPRECATED — internal header use.** The snippet below references
> `vm/uvm.h` from `src/`. This is not a supported public embedding
> pattern; it works today only because the public API does not yet expose
> opaque VM allocation. **Do not use in new code.** A supported opaque API
> (`urbi_vm_create()` / `urbi_vm_free()`) lands in Wave 4 of the v0.10.x
> architectural refactor arc; until then, embedders should treat this
> snippet as best-effort and consult the "Status of this guide" section
> above.

```c
/* STANDALONE EXAMPLE — compile with:
 *   cc -std=c99 -Iinclude -Isrc quick_start.c build/host/liburbi.a \
 *      build/host/liburbi_aux.a -lm -o quick_start
 *
 * DEPRECATED: `-Isrc` is required here because `vm/uvm.h` is an internal
 * header that exposes the full `struct UVM` layout for stack allocation.
 * The public headers in `include/urbi/` provide only an opaque
 * forward declaration. This pattern will be replaced by `urbi_vm_create()`
 * in a future release; do not use `-Isrc` in new production code.
 *
 * Requires: liburbi.a and liburbi_aux.a already built (run `make`). */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include "urbi/urbi.h"
#include "urbi/types.h"
#include "urbi/aux.h"
#include "vm/uvm.h"   /* DEPRECATED: internal header; needed today for stack allocation */

/* Simple allocator that wraps the system heap. */
static void *sys_alloc(void *ptr, size_t nbytes, void *ud)
{
    (void)ud;
    if (ptr == NULL && nbytes == 0) return NULL;
    if (nbytes == 0) { free(ptr); return NULL; }
    return ptr ? realloc(ptr, nbytes) : malloc(nbytes);
}

/* Writer: route urbiscript cout/cerr to stdout/stderr. */
static void writer(void *ud, const char *ch, size_t ch_len,
                   const char *msg, size_t msg_len, uint64_t ts_us)
{
    (void)ud; (void)ch_len; (void)ts_us;
    FILE *f = (ch[0] == 'c' && ch[1] == 'e') ? stderr : stdout;
    fprintf(f, "[%s] %.*s\n", ch, (int)msg_len, msg);
}

int main(void)
{
    /* Confirm compile-time header matches link-time library. */
    if (urbi_aux_check_version() != URBI_OK) {
        fprintf(stderr, "ABI version mismatch\n");
        return 1;
    }

    struct UVM vm;
    if (urbi_vm_init(&vm, sys_alloc, NULL) != URBI_OK) {
        fprintf(stderr, "urbi_vm_init: out of memory\n");
        return 1;
    }
    urbi_set_writer(&vm, writer, NULL);

    /* Compile a one-line script and run it. */
    const char *src = "cout << 42 + 1 << endl;";
    size_t src_len = 23;
    unsigned char *bc  = NULL;
    size_t         bc_len = 0;
    char           errbuf[256];
    if (urbi_compile_source(&vm, src, src_len, "<quick_start>",
                             &bc, &bc_len, errbuf, sizeof(errbuf)) != URBI_OK) {
        fprintf(stderr, "compile error: %s\n", errbuf);
        urbi_vm_destroy(&vm);
        return 1;
    }

    UValue result;
    int rc = urbi_aux_load_and_run(&vm, bc, bc_len, &result);
    free(bc);

    if (rc != URBI_OK) {
        fprintf(stderr, "run error: %d\n", rc);
        urbi_vm_destroy(&vm);
        return 1;
    }

    urbi_vm_destroy(&vm);
    return 0;
}
```

### Step loop for event-driven operation

In an event-driven system the host drives the VM in a loop rather than calling a single blocking `urbi_vm_run`. The step result controls what the host does next:

```c
/* FRAGMENT — step loop pattern */
void urbi_task(struct UVM *vm)
{
    for (;;) {
        uint64_t wake_us = 0;
        switch (urbi_step(vm, 10000 /* budget */, &wake_us)) {
            case URBI_STEP_RUNNING:
                /* Budget exhausted; more work pending — call again immediately. */
                break;
            case URBI_STEP_QUIESCENT:
                /* No live strands or pending events; host may sleep or yield. */
                return;
            case URBI_STEP_WAKE_AT:
                /* Sleep until wake_us (monotonic µs) or an injected event wakes us. */
                host_sleep_until(wake_us);
                break;
            case URBI_STEP_FATAL:
                /* A strand entered a fatal state — inspect or tear down. */
                handle_fatal(&vm);
                return;
        }
    }
}
```

### Teardown

`urbi_vm_destroy` releases all GC-managed memory and unregisters event handlers. It is safe to call even if `urbi_vm_init` returned an error (partial-init state is cleaned up on the destroy path).

---

## 2. Loading and Running

`urbi_run_chunk(vm, realm, module, &out)` compiles/loads bytecode into the
realm and runs it under a persistent scheduler-managed strand.  Unlike
pre-v0.8.0, the strand persists past the call return — if the chunk-top
hits a sleep, join-wait, or event-wait, `urbi_run_chunk` returns and the
host's main `urbi_step` loop continues driving the strand.

### Typical pattern

```c
urbi_vm_init(&vm, NULL, NULL);
URealm *realm = urbi_realm_create(&vm);

char errmsg[256];
UProto *root = urbi_chunk_from_bytes(buf, len, NULL, NULL,
                                     errmsg, sizeof(errmsg));
urbi_run_chunk(&vm, realm, root, NULL);  /* returns when chunk-top
                                          * parks or completes */

while (running) {
    UStepResult r = urbi_step(&vm, 1000, NULL);
    if (r == URBI_STEP_QUIESCENT) break;
    /* ... or sleep waiting for events ... */
}

urbi_vm_destroy(&vm);              /* kills all strands, drops proto refs */
urbi_chunk_free(root);             /* safe: refcount == 0 post-vm_destroy */
```

### Chunk-top parallel semantics

Chunk-top `&` (fork-join) and `,` (fork-detach) work — the persistent loader
strand has scheduler context.  Functions called from chunk-top that contain
fork inherit the loader strand's scheduler context.  This matches the legacy
urbiscript 2.0 spec; pre-v0.8.0 raised `URBI_ERR_STRAND_FATAL` for these.

Background work spawned at chunk-top (forked detach, at-handlers, every,
whenever) survives past `urbi_run_chunk`'s return; the host's `urbi_step`
loop drains it.

### See also

- `docs/internals/loader-strand.md` — full mechanics.
- `docs/internals/loader-strand.md` — persistent loader strand internals and language-level commitment.

---

## 3. Allocator Strategy

The VM uses a single allocator callback — `UVMAllocFn` — for every heap allocation (GC cells, intern table, scheduler queues, IC tables). The callback follows `realloc` semantics:

| `ptr` | `nbytes` | meaning |
|---|---|---|
| NULL | > 0 | allocate |
| non-NULL | 0 | free |
| non-NULL | > 0 | reallocate |

### Fixed-heap pattern (embedded targets)

On targets without a general-purpose heap (bare-metal FreeRTOS, no POSIX), allocate a static or PSRAM-backed byte pool and use a simple arena:

```c
/* FRAGMENT — fixed-heap allocator for embedded targets */
#include <string.h>
#include <stdint.h>
#include "urbi/types.h"

#define URBI_HEAP_SIZE (256 * 1024)   /* 256 KB — adjust to target */

/* Place in PSRAM on ESP32-S3: __attribute__((section(".ext_ram.bss"))) */
static uint8_t urbi_heap[URBI_HEAP_SIZE];

static void *arena_alloc(void *ptr, size_t nbytes, void *ud)
{
    /* Replace with a real arena allocator such as tlsf or heap_caps_realloc
     * (ESP-IDF).  This stub is illustrative only. */
    (void)ptr; (void)nbytes; (void)ud;
    return NULL;   /* placeholder */
}
```

### PSRAM vs internal SRAM

On dual-memory targets such as the ESP32-S3:

- **Internal SRAM** (192–512 KB depending on variant): use for VM structs and hot GC cells. Fastest access; tight capacity.
- **PSRAM** (external, 2–8 MB on S3): use for the bulk urbi heap (`heap_caps_malloc(size, MALLOC_CAP_SPIRAM)`). Latency ~10× higher than internal SRAM, but urbi's GC pauses are already bounded ≤2.1 µs at worst case, so PSRAM latency lands well within the 1 ms control-loop budget.

### Post-`urbi_lock_heap`

`urbi_lock_heap(vm)` is a one-way latch that prevents any new GC-cell allocation after it is called. Intended for hard-real-time phases (v2.0+) where post-init allocation is forbidden by policy. After calling it:

- Existing GC objects (watchers, events, strands, closures) continue to operate.
- GC mark/sweep still runs — collection still reclaims dead cells.
- Any API call that would allocate a new GC cell returns `URBI_ERR_OOM` or `URBI_WATCHER_HANDLE_INVALID` at the boundary.
- New `urbi_register_watcher` calls return `URBI_WATCHER_HANDLE_INVALID`.
- `urbi_event_unregister` returns `URBI_ERR_HEAP_LOCKED` (registry mutation requires allocation).

Do not call `urbi_lock_heap` in v1.0 use-cases unless you specifically need the hard-RT allocation-free guarantee — the embedder lifecycle works without it.

---

## 4. Event Flow

Events are the primary mechanism for moving data from C drivers (or ISR handlers) into the urbiscript reactive layer. The flow has three stages: register the event, inject from C (possibly from ISR), and consume in urbiscript via `at`.

### Registering a named event

```c
/* FRAGMENT — register a named event */
#include "urbi/urbi.h"
#include "urbi/types.h"

/* Destructure callback: convert raw ISR bytes into UValue arguments
 * for the at(sensor?(x, y, z)) watcher body.
 * Runs on MAIN thread (not in ISR). */
static int sensor_destructure(struct UVM *vm,
                               const urbi_event_payload_t *payload,
                               size_t payload_len,
                               UValue *out_args, int max_args, void *ud)
{
    (void)ud; (void)payload_len;
    if (max_args < 3) return -1;
    out_args[0] = urbi_make_float(payload->f32[0]);
    out_args[1] = urbi_make_float(payload->f32[1]);
    out_args[2] = urbi_make_float(payload->f32[2]);
    return 3;
}

/* Register: vm and realm must be initialized before this call. */
static urbi_event_id_t EV_SENSOR;

void register_events(struct UVM *vm, struct URealm *realm)
{
    EV_SENSOR = urbi_event_register(vm, realm, "sensor",
                                     sensor_destructure, NULL);
    if (EV_SENSOR == URBI_EVENT_ID_INVALID) {
        urbi_error_info_t info;
        urbi_last_error(vm, &info);
        /* handle error */
    }
}
```

### Injecting from ISR

`urbi_inject_event` is the only ISR-safe entry point. It deposits a payload into a lock-free SPSC ring; the VM drains the ring at the start of each `urbi_step` call on the main thread.

```c
/* FRAGMENT — inject from ISR */
void SENSOR_ISR(void)
{
    float x = read_sensor_x();
    float y = read_sensor_y();
    float z = read_sensor_z();

    urbi_event_payload_t p;
    p.f32[0] = x;
    p.f32[1] = y;
    p.f32[2] = z;
    p.f32[3] = 0.0f;   /* pad */

    urbi_inject_event(vm_ptr, EV_SENSOR, &p, sizeof(p));
    /* wake_fn fires here if installed (see urbi_set_wake_fn) */
}
```

### Consuming in urbiscript

```urbiscript
at (sensor?(x, y, z)) {
    cout << "sensor: " << x << " " << y << " " << z << endl
};
```

### `every (period) body`

Periodic-spawn construct (v0.9.4). `every (100) X` is sugar for spawning a strand that runs `X` every 100 milliseconds until tag-cancelled. The period is a millisecond float; the body is any statement, wrapped automatically by the parser into a zero-argument function literal.

```urbiscript
heartbeat: { every (500) led_toggle() };
// ...later...
heartbeat.stop();
```

The strand inherits the caller's ambient tag scope; `tag.stop()` cancels via the existing cleanup cascade. Body execution time delays subsequent fires (body+sleep model — the period is the minimum interval between fires, not a guaranteed cadence). The scheduler's sleep-queue drives re-arming directly from C; there is no urbiscript-side `sleep()`.

See `tests/chk/reactive/every/*.chk` for canonical behaviour.

**v0.9.4 limitations** (filed against v1.x):

- A label-prefix on `every()` must use the brace-block form (`tag: { every(P) X }`); bare-prefix `tag: every(P) X` is a parse error per `src/parse/uparse_react.c:87`.
- Closure-creation inside the body fails with `TypeError: CLOSURE: proto index out of range` (the body strand's `executing_proto.nested[]` does not include child protos). Pinned as a regression target in `tests/chk/reactive/every/nested.chk`.
- The label-bound tag identifier itself is `nil` because `Tag.new()` is v1.x-deferred; the `.stop()` half of the canonical legacy idiom needs that constructor to actually cancel via the tag.

### Watcher-body-done callback (telemetry)

Install a callback to observe every watcher-body completion — useful for latency profiling or watchdog integration:

```c
/* FRAGMENT — watcher-body-done telemetry */
static void on_body_done(struct UVM *vm, void *ud,
                          urbi_watcher_handle_t handle,
                          int completion_status)
{
    (void)vm; (void)ud;
    /* handle == 0: script-side at/whenever watcher.
     * handle != 0: host-side urbi_register_watcher watcher (Gap J). */
    record_watcher_latency(handle, completion_status);
}

void install_telemetry(struct UVM *vm)
{
    urbi_set_watcher_body_done_fn(vm, on_body_done, NULL);
}
```

### IMU multi-axis worked example

This example shows the canonical pattern for correlated sensor data: three axes of an IMU sensor that must be observed together as one coherent sample. Without atomicity, an `at(accel)` watcher body could fire before `at(gyro)` fires, seeing accel data from sample N and gyro data from sample N−1.

```c
/* FRAGMENT — IMU ISR with atomic event injection */
/* Forward declaration — sensor_destructure is defined in the event
 * registration section; see the Event Flow section for the full definition. */
static int sensor_destructure(struct UVM *vm,
                               const urbi_event_payload_t *payload,
                               size_t payload_len,
                               UValue *out_args, int max_args, void *ud);

static urbi_event_id_t EV_ACCEL;
static urbi_event_id_t EV_GYRO;
static urbi_event_id_t EV_MAG;

/* Register all three events during init (not in ISR). */
static void register_imu_events(struct UVM *vm, struct URealm *realm)
{
    EV_ACCEL = urbi_event_register(vm, realm, "accel", sensor_destructure, NULL);
    EV_GYRO  = urbi_event_register(vm, realm, "gyro",  sensor_destructure, NULL);
    EV_MAG   = urbi_event_register(vm, realm, "mag",   sensor_destructure, NULL);
}

/* ISR fires when I2C burst from IMU is complete. */
void IMU_DataReady_ISR(void)
{
    /* Read all three sub-samples in one burst (hardware guarantees coherence). */
    float ax, ay, az, gx, gy, gz, mx, my, mz;
    read_imu_burst(&ax, &ay, &az, &gx, &gy, &gz, &mx, &my, &mz);

    /* Begin atomic section: drain is suppressed until urbi_atomic_end. */
    urbi_atomic_begin(vm_ptr);

    urbi_event_payload_t pa = { .f32 = { ax, ay, az, 0.0f } };
    urbi_inject_event(vm_ptr, EV_ACCEL, &pa, sizeof(pa));

    urbi_event_payload_t pg = { .f32 = { gx, gy, gz, 0.0f } };
    urbi_inject_event(vm_ptr, EV_GYRO,  &pg, sizeof(pg));

    urbi_event_payload_t pm = { .f32 = { mx, my, mz, 0.0f } };
    urbi_inject_event(vm_ptr, EV_MAG,   &pm, sizeof(pm));

    /* End atomic section: all three events drain together in one pass.
     * The at(accel), at(gyro), and at(mag) watchers all fire on the same
     * scheduler tick — no partial observation is possible. */
    urbi_atomic_end(vm_ptr);
}
```

The urbiscript side sees all three as one dispatch:

```urbiscript
at (accel?(ax, ay, az)) { process_accel(ax, ay, az) };
at (gyro?(gx, gy, gz))  { process_gyro(gx, gy, gz) };
at (mag?(mx, my, mz))   { fuse_sensors(ax, ay, az, gx, gy, gz, mx, my, mz) };
```

`urbi_atomic_begin`/`urbi_atomic_end` are NOT ISR-safe on their own — they must be called from the MAIN thread or from an ISR only when the main thread is known to be blocked waiting for notification (the typical FreeRTOS pattern: ISR calls `urbi_atomic_begin`, does burst inject, calls `urbi_atomic_end`, then posts a task notification; the urbi task was blocking on `xTaskNotifyWait` and wakes to drain). Do not call them from a nested ISR or from a thread that contends with the main urbi thread.

---

## 5. Host Function Registration

`urbi_register` installs a C function as a script-visible global constant. The binding is const — re-registering the same name returns `URBI_ERR_CONST_SLOT_WRITE`.

### The `urbi_native_method_fn` signature

```c
/* FRAGMENT — host function signature */
typedef int (*urbi_native_method_fn)(struct UVM *vm,
                                     UValue self,
                                     UValue *args,
                                     uint8_t nargs,
                                     UValue *out);
```

Parameters:

- `vm` — the executing VM.
- `self` — the receiver (the object the slot was loaded from).
- `args` — argument array (NULL when `nargs == 0`).
- `nargs` — argument count.
- `out` — write the return value here; initialized to NIL before the call.

Return `UEXEC_OK` (0) on success, `UEXEC_THROW` (1) to signal an exception.

### Example: registering a sensor-read function

```c
/* FRAGMENT — register a host function */
#include "urbi/urbi.h"
#include "urbi/types.h"

static int fn_read_temperature(struct UVM *vm,
                                UValue self, UValue *args, uint8_t nargs,
                                UValue *out)
{
    /* Enforce that this function is not called from ISR context. */
    URBI_ASSERT_NOT_ISR(vm);
    (void)self; (void)args; (void)nargs;

    float temp_c = hardware_read_temperature();
    *out = urbi_make_float(temp_c);
    return UEXEC_OK;
}

static int fn_set_led(struct UVM *vm,
                       UValue self, UValue *args, uint8_t nargs,
                       UValue *out)
{
    URBI_ASSERT_NOT_ISR(vm);
    (void)self; (void)out;

    if (nargs < 1 || !urbi_value_is_bool(args[0])) {
        /* Report an error via the error ring; caller can inspect with
         * urbi_last_error. */
        urbi_set_error(vm, URBI_ERR_INVALID_ARG,
                        "set_led expects a boolean argument",
                        "<set_led>", 0, "fn_set_led");
        return UEXEC_THROW;
    }
    hardware_set_led(urbi_value_as_bool(args[0]));
    *out = urbi_make_nil();
    return UEXEC_OK;
}

void register_host_functions(struct UVM *vm, struct URealm *realm)
{
    urbi_register(vm, realm, "readTemperature", fn_read_temperature);
    urbi_register(vm, realm, "setLed",          fn_set_led);
}
```

After registration, urbiscript can call these directly:

```urbiscript
cout << readTemperature() << endl;
setLed(true);
```

### Batch registration

When registering many functions at once, the aux helper is more concise:

```c
/* FRAGMENT — batch function registration */
#include "urbi/aux.h"

/* Forward declarations of functions defined earlier in this driver file. */
static int fn_read_temperature(struct UVM *vm, UValue self,
                                UValue *args, uint8_t nargs, UValue *out);
static int fn_set_led(struct UVM *vm, UValue self,
                       UValue *args, uint8_t nargs, UValue *out);

static const urbi_aux_function_decl_t DRIVER_FNS[] = {
    { "readTemperature", fn_read_temperature },
    { "setLed",          fn_set_led          },
};

static void register_driver(struct UVM *vm, struct URealm *realm)
{
    int rc = urbi_aux_register_function_table(
        vm, realm, DRIVER_FNS,
        sizeof(DRIVER_FNS) / sizeof(DRIVER_FNS[0]));
    if (rc != URBI_OK) {
        /* First failure stops the table; entries before it are installed. */
    }
}
```

### Type-safe value dispatch with urbi_value_is_*

`<urbi/types.h>` provides 13 zero-overhead inline predicates — one per `UValKind` — so host functions can dispatch on argument type without comparing against internal constants:

```c
/* FRAGMENT — urbi_value_is_* predicate dispatch (W4/v0.10.3) */
#include "urbi/types.h"
#include "urbi/aux.h"    /* urbi_aux_value_to_* checked accessors */

static int fn_print_arg(struct UVM *vm,
                         UValue self, UValue *args, uint8_t nargs,
                         UValue *out)
{
    (void)self; (void)out;
    if (nargs < 1) return UEXEC_OK;
    UValue v = args[0];

    if      (urbi_value_is_int(v))   { printf("%lld\n", (long long)urbi_value_as_int(v)); }
    else if (urbi_value_is_float(v)) { printf("%g\n",   urbi_value_as_float(v)); }
    else if (urbi_value_is_bool(v))  { printf("%s\n",   urbi_value_as_bool(v) ? "true" : "false"); }
    else if (urbi_value_is_str(v))   { size_t len; const char *s = urbi_value_as_str(v, &len);
                                        printf("%.*s\n", (int)len, s); }
    else if (urbi_value_is_nil(v))   { printf("nil\n"); }
    else                             { printf("<kind=%d>\n", (int)urbi_value_kind(v)); }
    return UEXEC_OK;
}
```

For single-call safe extraction — check and extract in one step — use the checked-accessor variants from `<urbi/aux.h>`. They return `URBI_OK` on type match or `URBI_ERR_TYPE` (-26) on mismatch, leaving `*out` unmodified:

```c
/* FRAGMENT — urbi_aux_value_to_* checked accessors (W4/v0.10.3) */
static int fn_double_it(struct UVM *vm,
                         UValue self, UValue *args, uint8_t nargs,
                         UValue *out)
{
    (void)self;
    int64_t n = 0;
    if (nargs < 1 || urbi_aux_value_to_int(args[0], &n) != URBI_OK) {
        urbi_set_error(vm, URBI_ERR_INVALID_ARG, "expects an integer",
                        "<double_it>", 0, "fn_double_it");
        return UEXEC_THROW;
    }
    *out = urbi_make_int(n * 2);
    return UEXEC_OK;
}
```

Available predicates: `urbi_value_is_nil`, `_bool`, `_int`, `_float`, `_str`, `_void`, `_object`, `_event`, `_closure`, `_ptr`, `_tag`, `_strand`, `_host_fn`.

Available checked accessors: `urbi_aux_value_to_int`, `_float`, `_bool`, `_str`, `_ptr`, `_object`, `_event`, `_closure`, `_tag`.

### ISR safety

Any host function may be called from a script `at` body that fires on the main thread. Host functions must never call ISR-unsafe OS primitives without checking context first. Use `URBI_ASSERT_NOT_ISR(vm)` in debug builds to catch misuse at the call site. In release builds the macro is a no-op (zero overhead).

---

## 6. Tag Management

Tags are the cancellation primitive in urbiscript. A tag groups one or more strands; stopping the tag signals all of them to unwind cooperatively.

### Creating a tag from C

```c
/* FRAGMENT — tag creation and lifecycle */
#include "urbi/urbi.h"

static struct UTag *my_tag = NULL;

void setup_tag(struct UVM *vm, struct URealm *realm)
{
    my_tag = urbi_tag_create(vm, realm, "myTag", 5 /* strlen("myTag") */);
    if (my_tag == NULL) {
        /* OOM — handle error */
    }
    /* Keep the tag reachable: store it in a realm global or hold a
     * urbi_ref (see Section 6), otherwise the GC may collect it. */
}
```

### Stopping a tag

```c
/* FRAGMENT — stopping a tag */
void cancel_work(struct UVM *vm)
{
    if (my_tag != NULL) {
        int rc = urbi_tag_stop(vm, my_tag, urbi_make_nil());
        (void)rc;
        /* At the next urbi_step, all strands scoped to myTag unwind. */
    }
}
```

### Querying tag state

```c
/* FRAGMENT — tag state inspection */
void inspect_tag(void)
{
    urbi_tag_info_t info;
    if (urbi_tag_info(my_tag, &info) == URBI_OK) {
        switch (info.state) {
            case URBI_TAG_RUNNING: /* tag is active */                break;
            case URBI_TAG_STOPPED: /* urbi_tag_stop was called */     break;
            case URBI_TAG_FROZEN:  /* tag is frozen (stdlib, M8+) */  break;
            default: break;
        }
        /* info.member_count: number of strands currently scoped to this tag.
         * info.has_parent: true when urbi_tag_create set a parent. */
    }
}
```

### Tags in urbiscript

Tags created from C are directly usable in urbiscript once stored in a realm global:

```c
/* FRAGMENT — expose a C-created tag to urbiscript */
void expose_tag(struct UVM *vm, struct URealm *realm)
{
    /* Wrap the UTag pointer as a closure-wrapped constant, or simply hold
     * a ref and stop via C when the condition fires. */
    (void)vm; (void)realm;
}
```

---

## 7. Reference Management

GC-managed objects (closures, events, tags, objects) can be collected once no GC root keeps them alive. A `urbi_ref` pins a UValue as a GC root for the lifetime of the handle.

The design follows the precedent of Lua's `luaL_ref` / `luaL_unref`: an integer-keyed table of pinned values, with generation counters to detect use-after-unref.

### Pinning a value

```c
/* FRAGMENT — reference management */
#include "urbi/urbi.h"

/* Pin a UValue across GC cycles. */
urbi_ref_t pin_closure(struct UVM *vm, struct UClosure *cl)
{
    urbi_ref_t ref = urbi_ref(vm, urbi_make_closure(cl));
    if (ref == URBI_REF_INVALID) {
        /* OOM — table couldn't grow */
    }
    return ref;
}

/* Retrieve the pinned value. */
UValue get_pinned(struct UVM *vm, urbi_ref_t ref)
{
    UValue v = urbi_ref_get(vm, ref);
    /* If ref was urbi_unref'd earlier, urbi_ref_get returns NIL
     * (generation mismatch detected). */
    return v;
}

/* Release the pin. After this, the GC may collect the value. */
void release(struct UVM *vm, urbi_ref_t ref)
{
    urbi_unref(vm, ref);
    /* Subsequent urbi_ref_get(vm, ref) returns NIL — generation counter
     * was incremented on unref, making the old handle stale. */
}
```

### Generation-counter protection

`urbi_ref_t` encodes a 24-bit slot index and an 8-bit generation counter. After `urbi_unref`, the slot's generation increments. Any copy of the old handle that tries `urbi_ref_get` gets NIL instead of a dangling pointer. This is the same generation-counter pattern Lua uses to protect `luaL_unref` races.

### When to use refs

Use a ref whenever you need to hold a GC-managed object in C between API calls or across `urbi_step` boundaries:

- A closure returned by `urbi_make_native_closure` not yet installed via `urbi_register`.
- A `UTag` pointer returned by `urbi_tag_create` before it is stored in a realm global.
- A `UEvent` pointer you want to keep alive between event dispatches.

---

## 8. Lifecycle Contracts

This section documents exactly when script-side watchers and host-side watchers are unbound. Understanding these contracts prevents use-after-free on the C side and ensures cleanup callbacks fire in the expected order.

### Script-side `at` watcher unbind conditions

A script-side `at (cond) body` watcher unbinds when any of the following occurs:

| Condition | Behavior |
|---|---|
| Owning strand cancelled (`urbi_strand_cancel`) | Body never fires again; no notification to host |
| Owning tag stopped (`urbi_tag_stop`) | Tag's `leave` scope event fires first, then body unbinds |
| Owning realm destroyed | All realm watchers unbind silently (GC sweep) |
| VM destroyed (`urbi_vm_destroy`) | All watchers unbind silently |
| Heap locked (`urbi_lock_heap`) | Existing watchers continue running; new `at` returns OOM at allocation time |

### Host-side `urbi_register_watcher` unbind conditions

A host-side watcher installed via `urbi_register_watcher` unbinds when any of the following occurs:

| Condition | Behavior |
|---|---|
| Event unregistered (`urbi_event_unregister`) | Callback fires one final time with a "removed" sentinel (argc=0, event_id=`URBI_EVENT_ID_INVALID`), then unbinds |
| `urbi_unregister_watcher(handle)` called | No final callback; clean deferred unbind at end of current drain pass |
| Callback returns `URBI_ERR_WATCHER_UNREGISTER` | Auto-unbinds after the current callback returns |
| VM destroyed | Callback fires one final time with event_id=`URBI_EVENT_ID_INVALID`, then unbinds |
| Heap locked | Existing watchers continue; new `urbi_register_watcher` returns `URBI_WATCHER_HANDLE_INVALID` |

### `urbi_watcher_body_done_fn` fanout

The callback installed by `urbi_set_watcher_body_done_fn` fires after every watcher-body completion, for both script-side and host-side watchers:

- `handle == URBI_WATCHER_HANDLE_INVALID` (0): a script-side `at`/`whenever` watcher completed.
- `handle != 0`: a host-side `urbi_register_watcher` watcher completed; the handle matches what `urbi_register_watcher` returned.
- `completion_status`: mirrors the strand's fatal status for script-side (`UEXEC_OK`, `UEXEC_THROW`, `UEXEC_TAG_STOP`, `UEXEC_CANCEL`); for host-side it is `URBI_OK` or `URBI_ERR_WATCHER_UNREGISTER`.

**Important constraint:** The done callback runs at a deeply nested point inside the dispatch loop. Do not call urbi VM-mutating APIs from inside it. Safe operations include reading counters, posting to a host telemetry ring, or setting a volatile flag.

### Error inspection

When an API call fails, inspect the per-VM error ring for structured detail:

```c
/* FRAGMENT — error inspection */
void check_error(struct UVM *vm, int rc)
{
    if (rc != URBI_OK) {
        urbi_error_info_t info;
        urbi_last_error(vm, &info);
        fprintf(stderr, "[%s] error %d: %s (at %s:%d)\n",
                info.context, info.code, info.message,
                info.source_name[0] ? info.source_name : "<unknown>",
                info.source_line);
        urbi_clear_error(vm);
    }
}
```

The `const char*` fields in `urbi_error_info_t` point into VM-owned storage and are valid until the next API call that mutates error state. Copy them if you need them across subsequent calls.

### Unified error model (v0.10.3)

All public API functions return `int` with a consistent three-zone convention:

| Return value | Meaning |
|---|---|
| `URBI_OK` (0) | Success |
| Negative (`URBI_ERR_*`) | Failure — inspect the error ring |
| Positive (`UCallbackSignal`) | Callback-side signal — only valid as a return from host-watcher callbacks |

**`UCallbackSignal`** values for watcher/event callbacks:

| Constant | Value | Meaning |
|---|---|---|
| `URBI_CB_OK` | 0 | Stay registered, no side-effect |
| `URBI_CB_UNREGISTER` | 1 | Auto-unregister after this callback returns |
| `URBI_CB_THROW` | 2 | Raise an urbiscript exception in the calling strand |

The legacy name `URBI_ERR_WATCHER_UNREGISTER` is kept as an alias for `URBI_CB_UNREGISTER` for source compatibility. New code should use `URBI_CB_UNREGISTER`.

**Setter callbacks with `void *ud`:** all five setter functions accept a trailing `void *ud` opaque pointer that is forwarded to every callback invocation:

```c
/* FRAGMENT — ud forwarding pattern */
static void my_diag(struct UVM *vm, void *ud, int level, const char *fmt, ...)
{
    (void)vm;
    struct MyContext *ctx = (struct MyContext *)ud;
    /* use ctx for routing */
    (void)level; (void)fmt;
}

void setup_diag(struct UVM *vm, struct MyContext *ctx)
{
    urbi_set_diag_fn(vm, my_diag, ctx);
}
```

The same pattern applies to `urbi_set_time_us`, `urbi_set_watcher_body_done_fn`, `urbi_set_isr_check_fn`, and `urbi_register_event_drain`. Pass `NULL` when no context is needed.

---

## 9. Common Patterns

### Peripheral driver shape

The typical pattern for wiring a hardware peripheral into urbiscript:

1. **C layer**: driver functions poll or block on hardware; ISR deposits events via `urbi_inject_event`.
2. **Host-function layer**: `urbi_register` exposes control functions (e.g., `setMotorSpeed`, `readEncoder`) callable from script.
3. **Event layer**: `urbi_event_register` names events (e.g., `encoderTick`, `limitSwitch`) that ISRs inject.
4. **Urbiscript layer**: `at` and `whenever` react to events; host functions provide actuator control.

```c
/* FRAGMENT — peripheral driver wiring sketch */
static urbi_event_id_t EV_LIMIT;

static int fn_set_motor(struct UVM *vm, UValue self,
                         UValue *args, uint8_t nargs, UValue *out)
{
    URBI_ASSERT_NOT_ISR(vm);
    (void)self; (void)out;
    if (nargs < 1) return UEXEC_THROW;
    hardware_set_motor((int)urbi_value_as_int(args[0]));
    return UEXEC_OK;
}

void setup_motor_driver(struct UVM *vm, struct URealm *realm)
{
    EV_LIMIT = urbi_event_register(vm, realm, "limitSwitch", NULL, NULL);
    urbi_register(vm, realm, "setMotor", fn_set_motor);
}

void LIMIT_SWITCH_ISR(void)
{
    urbi_inject_event(vm_ptr, EV_LIMIT, NULL, 0);
}
```

Urbiscript side:

```urbiscript
at (limitSwitch) { setMotor(0) };
setMotor(100);
```

### Reactive object pattern

Expose mutable host state as a script-visible object with slot access:

```c
/* FRAGMENT — slot get/set from C */
void update_state_object(struct UVM *vm, UValue state_obj)
{
    /* Write a new value into the object's slot. */
    UValue v = urbi_make_float(3.14);
    urbi_slot_set(vm, state_obj, "value", 5, v);

    /* Read it back. */
    UValue result;
    if (urbi_slot_get(vm, state_obj, "value", 5, &result) == URBI_OK) {
        double f = urbi_value_as_float(result);
        (void)f;
    }
}
```

### Telemetry hookup

Combine `urbi_set_watcher_body_done_fn` with a lock-free ring buffer to record per-watcher latency without blocking the VM:

```c
/* FRAGMENT — lock-free telemetry */
static volatile uint32_t watcher_fire_count = 0;

static void telemetry_done(struct UVM *vm,
                            urbi_watcher_handle_t handle,
                            int status)
{
    (void)vm; (void)handle; (void)status;
    /* Atomic increment safe here: the done callback runs on MAIN thread. */
    watcher_fire_count++;
}
```

### Watchdog integration

Use a host-side watcher to detect stale event rates and feed a hardware watchdog:

```c
/* FRAGMENT — host-side watchdog watcher */
static int watchdog_watcher(struct UVM *vm, urbi_event_id_t id,
                             const UValue *args, int argc, void *ud)
{
    (void)vm; (void)id; (void)args; (void)argc; (void)ud;
    hardware_watchdog_kick();
    return URBI_OK;   /* stay registered */
}

void setup_watchdog(struct UVM *vm, struct URealm *realm,
                    urbi_event_id_t heartbeat_id)
{
    urbi_register_watcher(vm, realm, heartbeat_id, watchdog_watcher, NULL);
}
```

---

## 10. Anti-Patterns

### Heavy compute in an `at` body

Every opcode the VM executes inside an `at` body blocks all other strands. A per-pixel blob detector scanning a 320×240 frame would take roughly 1.5 seconds per invocation at ~50 ns/opcode — effectively freezing the reactive layer.

**Correct pattern:** do heavy computation in a separate OS task. The task posts a 12-byte result payload via `urbi_inject_event`; urbi receives the event and dispatches the reactive logic in ~100 µs.

### Holding sensor pointers across `at` body execution

Camera frame buffers and DMA buffers are typically returned to the driver immediately after processing. If an urbiscript `at` body holds a pointer to such a buffer (via a closure capture or a global variable set from an ISR), the driver blocks on the next `fb_get` waiting for the slot to free.

**Correct pattern:** copy data into an `urbi_event_payload_t` payload in the ISR/task and inject the copy. The payload bytes are owned by the ring entry — immutable and safe to inspect in the watcher body without holding any driver lock.

### Mixing allocators

The urbi heap is managed entirely through the `UVMAllocFn` you supply at `urbi_vm_init`. Passing a pointer allocated by `pvPortMalloc` to an urbi API call that will eventually free it (or vice versa) creates a cross-allocator mismatch that corrupts both heaps.

**Correct pattern:** keep allocator domains strictly separate. All urbi-internal memory flows through `UVMAllocFn`. All driver/host memory flows through the host allocator.

### Calling non-ISR-safe APIs from ISR context

The only urbi API call safe from ISR context is `urbi_inject_event` (and the wake-fn callback, which must be O(1) and non-allocating). Calling any other API — `urbi_tag_stop`, `urbi_register`, `urbi_event_register`, `urbi_step` — from ISR context is undefined behavior.

In debug builds, `URBI_ASSERT_NOT_ISR(vm)` placed at the top of a host function catches this class of bug at the call site. Register an ISR-check predicate via `urbi_set_isr_check_fn(vm, xPortInIsrContext, NULL)` (or the POSIX equivalent) so the assertion has real teeth.

### Sharing `UStrand` pointers across VMs

Each `UStrand` is bound to the VM and realm that created it. Passing a strand pointer to a different VM's step loop, or storing it in a way that another thread's VM can access it, is unsupported and will corrupt scheduler state.

### Reaching into internal `src/` headers

`src/vm/uvm.h`, `src/sched/ustrand.h`, and similar internal headers are not part of the public API and may change without notice. Any embedder code that `#include`s them will break across releases.

**Correct pattern:** use only `<urbi/urbi.h>`, `<urbi/types.h>`, `<urbi/aux.h>`, and `<urbi/version.h>`. If you need functionality that those headers do not expose, open an issue — don't reach through the internal layer.

**Known temporary exception:** the Quick Start and REPL snippets in this guide currently include `vm/uvm.h` to stack-allocate `struct UVM`. This is acknowledged as a gap (see "Status of this guide" above) and is explicitly marked DEPRECATED in those snippets. The opaque allocation API (`urbi_vm_create()` / `urbi_vm_free()`) that removes this requirement is planned for the v0.10.x architectural refactor arc.

---

## 11. Threading Model

urbi-embedded's threading model follows a clear progression from today's single-VM design toward future multi-VM and reactive-messaging architectures. The commitment below is stable: design decisions today do not foreclose the v1.x or v2.0+ paths.

| Tier | Today (v1.0) | Future (v1.x) | Possible (v2.0+) | Closed by design |
|---|---|---|---|---|
| Within one VM | Cooperative coroutines (strands) on one scheduler | Preemptive coroutines (`URBI_SCHED_PREEMPTIVE`) — still single OS thread | Erlang-style isolated realms with message passing | (n/a) |
| Cross-VM in one process | Not supported | Multi-VM-per-process: one OS thread per VM, no shared state | (covered above) | (n/a) |
| Inside one VM, multi-OS-thread | Not supported | Not planned | Erlang-style only | **Shared mutable state across OS threads — closed by design.** urbiscript's parallelism is cooperative-coroutine; no language construct requires shared mutable state across OS threads, so the runtime does not pay the synchronization cost. |

### Today's contract

- One OS thread drives `urbi_step` on a given VM. All MAIN-thread-annotated API calls must be made from that thread.
- ISR-safe calls (`urbi_inject_event`, wake-fn callback) may run concurrently with `urbi_step` from an ISR or a different thread — but only the listed ISR-safe entry points. Everything else requires the MAIN thread.
- Multiple VMs in one process are not supported at v1.0. Each VM owns its heap, scheduler, and event ring independently. Sharing GC-managed objects (UValue pointers) between VMs is undefined behavior.

### Future direction (v1.x)

Multi-VM-per-process will allow one OS thread per VM with no shared state between VMs. Inter-VM communication will use explicit message passing, not shared pointer access. The v1.0 API surface — including `urbi_atomic_begin`/`urbi_atomic_end` as single bool flags and the drain as single-consumer — is designed to be structurally compatible with this addition.

### Rejected path

A Java/CPython-style GIL or shared-mutable-state model is permanently off the table. urbiscript's language semantics do not require it, and the robotics/real-time audience specifically benefits from the absence of locking overhead.

### Footprint: root-only fields on UProto

Every UProto carries `source_name` + 6 other root-only-meaningful fields
(~40 B on 64-bit, ~20 B on 32-bit). On non-root protos these are
zero-initialized and waste space. Typical module worst case (50 nested
protos) ≈ 2 KB slack on 64-bit, 1 KB on 32-bit.

If your port reports footprint pressure, a side-struct optimization
(`URootMeta` hung off UProto via an 8-byte pointer) is documented in
`docs/urbi-embedded-design-risks.md` as a v1.x cleanup. Until then,
the simpler shape is shipped: every UProto looks the same.

---

## 12. REPL Service

Build with `URBI_ENABLE_REPL=1`. Adds `<urbi/repl.h>`, the `src/repl/` subsystem (NDJSON codec, MPSC eval queue, per-session output ringbuf, dispatcher, pluggable transports), and the `urbi-server` / `urbi-send` host binaries.

The service exposes a line-oriented NDJSON protocol over a `UTransport` vtable (TCP, Unix socket, UART, pty, in-process buffer). One TCP connection = one `URealm` (a "lobby") bound to that session for its entire lifetime. Output produced by strands hosted under that lobby's realm flows back to that lobby's client only.

For the on-the-wire shape, thread layout, and dispatcher internals, see `docs/internals/repl-service.md`.

### Minimal embedder

> **DEPRECATED — internal header use.** The snippet below includes
> `vm/uvm.h` and `stdlib/stdlib_boot.h` from `src/`, and calls
> `urbi_stdlib_boot()` which is not declared in any public header.
> Neither is a supported public embedding pattern. `urbi_stdlib_boot()`
> is now called automatically inside `urbi_realm_global()`; the explicit
> call here is redundant and will be removed once the opaque VM allocation
> API (`urbi_vm_create()` / `urbi_vm_free()`) lands in Wave 4 of the
> v0.10.x architectural refactor arc. **Do not use these patterns in new
> code.**

```c
/* FRAGMENT — minimal REPL embedder.
 * Requires URBI_ENABLE_REPL=1 build flags and link against liburbi.a + -lm.
 * Compile-only validated; linking deferred (URBI_ENABLE_REPL symbols not in
 * default build).
 *
 * DEPRECATED: vm/uvm.h and stdlib/stdlib_boot.h are internal src/ headers.
 * urbi_stdlib_boot() is internal-only and is now auto-called internally.
 * This pattern will be replaced by urbi_vm_create() in a future release. */

#include <signal.h>
#include <stdio.h>
#include "urbi/urbi.h"
#include "urbi/repl.h"
#include "vm/uvm.h"               /* DEPRECATED: internal header; needed today for stack allocation */
#include "stdlib/stdlib_boot.h"   /* DEPRECATED: internal header; urbi_stdlib_boot is not public */

static volatile sig_atomic_t running = 1;
static void on_sigint(int sig) { (void)sig; running = 0; }

int main(void)
{
    struct UVM vm;
    if (urbi_vm_init(&vm, NULL, NULL) != URBI_OK) return 1;
    urbi_stdlib_boot(&vm);  /* DEPRECATED: now auto-called by urbi_realm_global(); remove from new code */

    UReplConfig cfg = {
        .bind_addr          = "127.0.0.1",  /* loopback => no token needed */
        .tcp_port           = 54000,
        .max_clients        = 16,
        .output_ringbuf_cap = 64 * 1024,
    };

    int err = 0;
    UReplServer *server = urbi_repl_serve(&vm, &cfg, &err);
    if (!server) {
        fprintf(stderr, "urbi_repl_serve: err=%d\n", err);
        urbi_vm_destroy(&vm);
        return 1;
    }

    signal(SIGINT, on_sigint);

    /* Drive the VM. urbi_step drains the MPSC eval queue at every step
     * boundary; the listener pthread + per-client reader pthreads accept
     * and parse on their own. */
    while (running) {
        urbi_step(&vm, 1024, NULL);
    }

    urbi_repl_stop(server);
    urbi_vm_destroy(&vm);
    return 0;
}
```

Then from another shell:

```sh
urbi-send eval "1 + 2"
# → 3
```

### Configuration: `UReplConfig`

```c
typedef struct UReplConfig {
    const char    *bind_addr;          /* "127.0.0.1" | "0.0.0.0" | NULL = loopback */
    int            tcp_port;           /* -1 to disable TCP */
    const char    *unix_path;          /* NULL to disable Unix-socket listener */
    const char    *auth_token;         /* NULL = no token (loopback-only) */
    int            max_clients;        /* 0 → 16 default */
    size_t         output_ringbuf_cap; /* 0 → 64 KiB default */
    UCompileBudget default_budget;     /* 0-fields → URBI_DEFAULT_REPL_BUDGET */
} UReplConfig;
```

`bind_addr` starting with `/` is treated as a Unix-socket path and considered loopback for the default-secure check. `tcp_port == -1` disables the TCP listener (useful when only registering a UART or in-process transport).

### Auth posture: default-secure refuse-to-start

`urbi_repl_serve` refuses to start with `URBI_ERR_INSECURE_CONFIG` if `bind_addr` is non-loopback and `auth_token` is NULL. Loopback addresses (`127.0.0.1`, `::1`, NULL, `/...`) are exempt — local development "just works" with no token. To expose the server on a LAN address, pass an `auth_token`; the server's `hello` envelope advertises `auth_required: true` and rejects any op other than `auth` until the client presents a matching token.

Token comparison is constant-time. Per-source-IP rate-limiting is automatic: 5 failed `auth` attempts within 30 s from the same peer locks that IP out for 60 s (LRU table, 8 entries). Local Unix-socket peers are tracked by pid instead of IP.

### Per-realm writer

`urbi_realm_set_writer(vm, realm, fn, ud)` installs an output writer scoped to one realm. Strands hosted under that realm route `echo` / `Stream.write` / `cerr` output through the realm writer; if unset, the runtime falls back to the VM-wide writer installed via `urbi_set_writer`. The REPL service uses this internally to send each session's output to that session's ringbuf — embedders rarely call it directly, but it is public for hosts that want explicit per-tenant output isolation without the full REPL service.

`urbi_vm_write_in_realm(vm, realm, channel, ...)` is the explicit C-side dispatch entry; `urbi_vm_write` is a thin wrapper with `realm == NULL`.

### Per-realm compile-budget

`urbi_repl_eval` honors a per-realm `UCompileBudget`:

```c
typedef struct UCompileBudget {
    uint32_t max_parser_depth;  /* recursive-descent stack ceiling */
    uint32_t max_ast_nodes;     /* AST allocations per compile */
    uint32_t max_source_bytes;  /* source-length cap (checked at entry) */
} UCompileBudget;

extern const UCompileBudget URBI_DEFAULT_REPL_BUDGET;  /* 256 / 100000 / 1 MiB */

void urbi_realm_set_compile_budget(URealm *realm, const UCompileBudget *budget);
```

`urbi_realm_create_repl` automatically applies `URBI_DEFAULT_REPL_BUDGET` to the new realm. The default global realm (`urbi_realm_global`) has no budget by default — trusted host code is not rate-limited. Pass `NULL` to clear.

Budget exhaustion raises `URBI_ERR_COMPILE_BUDGET_DEPTH` / `_NODES` / `_SOURCE` from the parser; the dispatcher turns these into NDJSON `{kind:"error", code:"compile_budget_*"}` envelopes for the originating client.

### NDJSON wire protocol (summary)

One JSON document per line, terminated by `\n` (client may send `\r\n`; server always emits `\n`). Maximum line length defaults to 1 MiB (matches `max_source_bytes`).

Client ops: `auth`, `eval`, `cancel`, `introspect`, `lobby_new`, `lobby_close`.
Server response kinds: `hello`, `auth_ok`, `result`, `output`, `done`, `error`, `event`, `goodbye`.

```jsonc
// Client → server
{"id":2, "op":"eval", "code":"1 + 2"}
{"id":3, "op":"introspect", "what":"coros"}
{"id":4, "op":"cancel", "tag":"experiment_42"}

// Server → client
{"kind":"hello", "version":"v0.9.1", "lobby":"a3f2", "auth_required":false}
{"id":2, "kind":"result", "value":3, "ts":1234567}
{"id":2, "kind":"done"}
{"kind":"output", "lobby":"a3f2", "channel":"clog", "msg":"every-tick", "ts":1236600}
```

`id` is client-assigned and echoed on correlated responses. `output` originating from strands that outlive their `eval` carries `lobby` + `channel` but no `id`. Full schema lives in `docs/internals/repl-service.md` and inline in `<urbi/repl.h>`.

### Introspection: the nine commands

Available both as NDJSON `{op:"introspect", what:"..."}` ops and as `Debug.<op>()` urbiscript methods bound on each realm's `Global.Debug` slot:

| Op | C primitive | Returns |
|---|---|---|
| `coros` | `urbi_introspect_coros(vm, buf, cap, &n)` | All strands: id, state, wake deadline, source location, tag stack |
| `tags` | `urbi_introspect_tags(...)` | All active tags: name, state, member coro_ids |
| `watchers` | `urbi_introspect_watchers(...)` | All `at` / `whenever` watchers: predicate / body location, fire count |
| `events` | `urbi_introspect_events(...)` | All registered events: name, subscriber count |
| `stack` | `urbi_introspect_stack(vm, coro_id, ...)` | Backtrace frames (file:line:function) |
| `slots` | `urbi_introspect_slots(vm, realm, obj_path, ...)` | An object's slot dump |
| `profile` | `urbi_introspect_profile(...)` | Stubbed in v0.9.1 (locked wire shape; populated v1.x) |
| `gc` | `urbi_introspect_gc(...)` | Heap stats: alive cells / bytes, last_gc_us, total_gc_time_us |
| `lobbies` | `urbi_introspect_lobbies(...)` | Active sessions: lobby_id, peer_addr, connect_ts, eval_count |

Each primitive walks VM-internal linked lists on the MAIN thread and emits a single JSON object into a caller-provided buffer. Wire JSON shape is locked at v0.9.1 and frozen forward to v1.0.

### Step-driven mode (bare-metal)

Hosts without an OS thread for `urbi_repl_serve` to spawn its listener pthread can use the manual driver:

```c
UReplServer *server = NULL;
urbi_repl_serve_init(&vm, &cfg, &server);
urbi_repl_register_transport(server, &UREPL_TCP_TRANSPORT, tcp_state);
/* or &UREPL_PTY_TRANSPORT / &UREPL_PICO_UART_TRANSPORT / ... */

while (running) {
    urbi_repl_serve_step(server, /*timeout_us=*/1000);
    urbi_step(&vm, 1024, NULL);
}

urbi_repl_serve_shutdown(server);
```

One `serve_step` performs at most one accept + read + dispatch + write cycle across all registered transports, returns when no transport has progress to make or `timeout_us` elapses.

### USB CDC (Raspberry Pi Pico, TinyUSB)

Defined in `src/repl/urepl_transport_usb_cdc_pico.c` when `URBI_PICO_USB_CDC + PICO_BOARD + URBI_ENABLE_REPL` are all set. Single-host (CDC has one attached host); `pollable_fd_fn` returns `-1` so the embedder drives `urbi_repl_serve_step` from the main loop.

Embedder setup (extern declarations — the Pico transports follow the same header-less pattern as the v0.9.1 UART Pico stub):

```c
/* FRAGMENT — Pico USB CDC REPL transport */
#include "urbi/repl.h"

struct UUsbCdcPicoState;
extern struct UUsbCdcPicoState *urepl_usb_cdc_pico_state_create(void);
extern const UTransport UREPL_USB_CDC_PICO_TRANSPORT;

void register_usb_cdc(UReplServer *server)
{
    struct UUsbCdcPicoState *st = urepl_usb_cdc_pico_state_create();
    urbi_repl_register_transport(server, &UREPL_USB_CDC_PICO_TRANSPORT, st);
}
```

See `examples/pico/repl_demo/main/main.c` for a complete embedder.

### Build-flag contract: `URBI_REPL_COOPERATIVE_ONLY`

If you're embedding on a freestanding target (Pi Pico, bare-metal
STM32, FPU-less ARM) and have `URBI_ENABLE_REPL=1`, you almost
certainly want `URBI_REPL_COOPERATIVE_ONLY=1` too. The `cross-pico`
make recipe auto-pairs them; if you're building `liburbi.a` manually,
set both yourself.

The flag swaps `urbi_mutex_t` / `urbi_cond_t` / `urbi_thread_t`
typedefs in internal REPL struct fields from pthread types (~40 bytes
each on Linux x86-64) to 1-byte empty stubs. Embedders linking
against the library MUST set the same flag, or struct layouts diverge
silently — same trap class as `URBI_FLOAT_TYPE`.

Calling `urbi_repl_serve()` (the threaded entry point) on a
cooperative-only library is **not supported** — the auth and transport
TUs that back it are filtered out at build time. Use
`urbi_repl_serve_init` + `urbi_repl_serve_step` instead, driving
the cooperative loop from your embedder's main loop.

On cooperative builds, `dispatch_auth` auto-approves clients (the
auth TU is filtered out). USB CDC / UART transports on freestanding
targets have no network threat model. If you bring up a custom
cooperative transport with network exposure, treat the auth path as
no-op until the v1.x `cooperative_auth_token` opt-in lands.

### See also

- `docs/internals/repl-service.md` — thread model, queue + ringbuf contracts, session lifecycle, transport adapter pattern, full NDJSON schema.
- `<urbi/repl.h>` — public API surface; inline doc comments on `UReplConfig` / `UTransport`.
- `tools/urbi-server.c`, `tools/urbi-send.c` — reference embedders.
