# Language Conventions

This doc captures urbiscript-language-level conventions and the C/urbiscript API contract. It is the sibling to `STYLE.md`: STYLE.md governs the C side (naming, freestanding discipline, const-correctness, initialization, headers); this doc governs the language surface the C runtime projects — value types, literal representations, enum discipline across the ABI boundary, bytecode portability, reactive-runtime knobs, and the core/aux API split.

Scope: every decision here is load-bearing for at least one of the v1.0 milestones (emit, VM, reactive runtime, stdlib, C API). Decisions that affect only the C implementation go in STYLE.md. This doc is what sits across the C/urbiscript seam.

---

## Intent

Three priorities shape every convention in this doc:

1. **No ambiguity at the C/urbiscript boundary.** A value that crosses the ABI has exactly one shape, one interpretation, one failure mode. Magic strings ("queue", "drop") are banned; enums are the discipline that makes typos and refactors safe on both sides.
2. **Per-target flavors are explicit, not implicit.** Bytecode is not pretend-portable. A format descriptor declares the flavor in the header, and the loader refuses a mismatch with a diagnostic that names the field. No silent coercion.
3. **Discipline-aware from v1.** Conventions that keep long-term options open (enum discipline, strict core/aux split, typed numeric system) are adopted now even when the v1 cost is small. The alternative is a retrofit, which is always more expensive.

---

## 1. Numeric type system

urbiscript has two numeric types: **Integer** and **Float**. Both descend from a shared `Number` supertype prototype that carries common methods; type-specific methods live on the respective subtype.

### 1.1 Representation

| Type | Width | Target variance |
|---|---|---|
| Integer | i64, signed two's-complement | Identical on every target |
| Float | f32 on SP-FPU / no-FPU targets (Cortex-M4, ESP32-C3, ESP32-S3); f64 everywhere else (Linux, STM32H7) | Per-ABI flavor — see §4 |

Integer is fixed-width across every target on purpose. Counter semantics (ms-since-boot, list indices, coroutine IDs, tag generations) need exact range and no silent wrap on 32-bit embedded; i64 gives ±9.2e18 at the cost of one extra word on 32-bit MCUs and a small libgcc helper set (`__muldi3`, `__divdi3`, `__moddi3` — ~1–2 KB flash on no-FPU parts).

Float is per-ABI flavor because FPU capability is the axis that actually varies across targets and because making Float depend on the target is the only way to keep f32 code fast on Cortex-M4 without penalizing f64 code on Linux. Integer stays uniform, so counter-heavy code ports unchanged; only genuine-FP math sees the precision difference.

### 1.2 Literals

```urbi
// Integer
3
100
1_000_000          // underscores permitted as digit separators
0xFF               // hex, Integer
0b1010             // binary, Integer
0o17               // octal, Integer (new-style prefix)

// Float
3.0
3.14
1e3                // scientific notation is always Float (matches Lua 5.3)
1E-5
.5                 // leading-dot form, Float
```

Scientific notation is always Float, even when the mathematical value is integral. `1e3` is Float 1000.0, not Integer 1000. This matches Lua 5.3's rule and removes an ambiguity at the lexer level.

Time literals compile to Integer nanoseconds — see §2. Angle literals (`180deg`, `pi`) compile to Float in radians.

### 1.3 Arithmetic semantics

Following Lua 5.3's evolution model:

| Operation | Integer op Integer | Float op Float | Integer op Float |
|---|---|---|---|
| `+` `-` `*` | Integer (wrap on overflow) | Float | Float (Integer promoted) |
| `/` | **Float** (always) | Float | Float |
| `//` | Integer (floor-division) | Float | Float |
| `%` | Integer | Float | Float |

Key rules:

- **`/` always returns Float.** `3 / 2 == 1.5`, never `1`. Integer division must use `//`.
- **`//` is floor-division**, not truncating division. `-3 // 2 == -2`. The result is Integer when both operands are Integer, Float otherwise.
- **Integer overflow wraps, two's-complement.** `INT64_MAX + 1 == INT64_MIN`. This is the Lua 5.3 default. Overflow detection is a v1.x deferred decision; v1 commits to wrap and lets programs cope.
- **Bitwise operations are methods on Integer, not symbolic operators** — see §1.4 below.  `|` and `&` in urbiscript are concurrency-composition operators at the statement level and are never overloaded for bit manipulation.

### 1.4 Bitwise operations

Bitwise ops live as methods on the `Integer` prototype, not as infix operators.  Rationale: `|` and `&` are already taken at the statement level (sequential-atomic and parallel-join composition respectively); overloading the same glyphs for integer-level bit manipulation conflates two different abstraction layers and forces a context-sensitive parser.  Using methods instead keeps the two layers cleanly separated.

```urbi
var flags = 0x0F
flags.bitand(0x03)        // 3
flags.bitor(0x10)         // 31
flags.bitxor(0xFF)        // 240
flags.bitnot()            // -16 (two's complement)
flags.bitshl(2)           // 60
flags.bitshr(1)           // 7
```

Method list (all on `Integer`, with matching C-level intrinsics):

| Method | Meaning | C equivalent |
|---|---|---|
| `bitand(Integer) → Integer` | Bitwise AND | `a & b` |
| `bitor(Integer) → Integer` | Bitwise OR | `a | b` |
| `bitxor(Integer) → Integer` | Bitwise XOR | `a ^ b` |
| `bitnot() → Integer` | Bitwise NOT | `~a` |
| `bitshl(Integer) → Integer` | Left shift | `a << b` |
| `bitshr(Integer) → Integer` | Right shift (arithmetic) | `a >> b` |

All six methods require the receiver to be Integer and (for binary forms) the argument to be Integer.  Applying to a Float raises `TypeError`.  There is no implicit truncation; if a program has a Float and wants to bit-manipulate, it coerces first: `x.toInt().bitand(mask)`.

Shift semantics: `bitshr` is arithmetic (sign-extending).  A logical-shift-right variant may be added in v1.x if a concrete use case surfaces; for now, if the user wants logical-shift-right they mask after shift: `x.bitshr(n).bitand((1_shl(64 - n)) - 1)` or similar.

Shift counts that are negative or >= 64 are defined behavior: the result is 0 for `bitshl`, and `-1` or `0` for `bitshr` depending on the sign of the receiver.  This mirrors Lua 5.3's documented rule and differs from C's undefined behavior on out-of-range shifts.

Boolean operators (`and`, `or`, `not`, plus their synonyms `&&`, `||`, `!`) are separate from bitwise.  They operate on truthiness, short-circuit, and return one of the operands (Python-style) rather than a Boolean.  Do not mix the two sets — if you want bitwise, call a method; if you want short-circuit, use the keyword.

### 1.5 Cross-type comparison

Comparison across types works by numerical value:

```urbi
3 == 3.0      // true
3 < 3.5       // true (Integer implicitly promoted)
type(3)       // "Integer"
type(3.0)     // "Float"
type(3) == type(3.0)   // false — the runtime tags differ even when values compare equal
```

Equality compares numerical value, not runtime tag. `isA(Number)` is true for both; `isA(Integer)` and `isA(Float)` distinguish them.

### 1.6 Prototype structure

```
Number (abstract supertype prototype)
├── Integer
│   └── methods: bitand, bitor, bitxor, shl, shr, bnot, toFloat, ...
└── Float
    └── methods: sqrt, sin, cos, toInt, isNaN, isInf, ...
```

`Number` carries `.round()`, `.abs()`, `.times()`, comparison, arithmetic — anything meaningful for both types. `Integer` carries bitwise ops and `.toFloat()`. `Float` carries transcendentals and FP-specific queries.

`.round()`, `.floor()`, `.ceil()` return Integer by v1.0 (recommended direction — §5 of the spec flags this as deferred, but Integer is the useful answer for array-index and counter use cases). Legacy urbi returned Float; this is a deliberate post-2.x evolution.

### 1.7 Consequence for the `.chk` corpus

Legacy urbi 2.x was Lua-5.0-shaped — one Float type. Tests in the legacy 2.x conformance corpus that assert `.isA(Float)` on integer literals (`3`, `100`) will fail against v1.0. Expected porting work is 1–2 days at M5: type-assertion sites get updated to `.isA(Integer)` or `.isA(Number)` as appropriate. Value-level assertions (`assert(1 + 2 == 3)`) pass unchanged — the number system changes what things *are*, not what they *equal*.

---

## 2. Time-literal precision and representation

Time literals in urbiscript compile to **Integer nanoseconds**. The lexer recognizes the suffix and produces a constant-pool entry of type Integer with value expressed in ns.

### 2.1 Conversion table

| Literal | Integer value | Notes |
|---|---|---|
| `1ns` | 1 | |
| `1us` | 1_000 | |
| `1ms` | 1_000_000 | |
| `100ms` | 100_000_000 | |
| `1s` | 1_000_000_000 | |
| `1min` | 60_000_000_000 | |
| `1h` | 3_600_000_000_000 | |
| `1day` | 86_400_000_000_000 | Integer fits |

Fractional time literals (`1.5s`) compile to Integer ns using round-half-to-even at the lexer — `1.5s` is `1_500_000_000` exactly. Programs wanting sub-ns precision should not be writing fractional time literals; sub-ns is not a domain urbi-embedded targets.

### 2.2 Exact range

At i64 precision, ns-encoded durations cover approximately ±292 years. The maximum representable positive duration is `INT64_MAX` ns ≈ 9.223e18 ns ≈ 292.47 years. This ceiling is far beyond any realistic duration an embedded urbi program will hold as a single literal; duration arithmetic (accumulators, countdowns) sits comfortably inside the range with room to spare.

### 2.3 Why nanoseconds

- **Finest common resolution.** Every supported target's timer subsystem can resolve microseconds; many can resolve hundreds of nanoseconds. Picking ns as the internal unit means the language representation is never the limiting factor — precision loss is at the timer hardware, not at urbi.
- **Exact arithmetic at i64.** Sum of 1000 `100ms` intervals is exactly `100_000_000_000` ns, no FP drift. An f64 encoding would already accumulate ~1 ULP of error on a seconds-to-years accumulator; Integer ns doesn't.
- **One representation for both literals and Duration results.** `Clock.now() - start` and `100ms` are the same type. No implicit conversion, no ambiguity about which unit a variable is in.

Sub-ns precision is out of scope. Domains that need it (e.g. RF signal processing) aren't the urbi target; if they ever become relevant, a separate `Duration` type (backlog) would carry its own representation choice.

### 2.4 Angles are different

Angle literals (`180deg`, `pi`, `pi/2`) compile to **Float in radians**, matching the legacy spec:

```urbi
180deg == pi          // true (Float comparison, within ULP)
pi/4                  // Float, 0.7853...
```

Angles are inherently FP (transcendental math), so Integer representation would be lossy before it even hit a `sin()` call. The asymmetry with time is deliberate — they're different physical quantities with different computational needs.

---

## 3. Enums-as-singletons rule

**Never cross an API boundary with a magic string.** The discipline has two sides — one on the C API, one inside urbiscript — and they have to be enforced together for the guarantee to hold.

### 3.1 Motivation

Three failure modes a magic-string API produces:

1. **Typos that compile.** `exhaust_policy = "quueue"` succeeds at write time, fails at first firing, and points at the assignment site — not the definition.
2. **Refactor erosion.** Renaming `"drop"` to `"drop_newest"` in the runtime leaves every call site holding the old value. Grep finds some; grep does not find them all.
3. **Ambient typo tolerance.** Runtime dispatches coded as `if (strcmp(s, "queue") == 0)` gain a tacit "we'll accept whatever sort of works" habit. That's incompatible with the discipline this doc enforces.

Enums solve all three at the language level.

### 3.2 C side: real `enum` types

Every policy, mode, state, or kind that crosses the `urbi.h` boundary is declared as a `typedef enum` with `URBI_<NOUN>_<VALUE>` members:

```c
/* src/urbi.h */
typedef enum {
    URBI_EXHAUST_QUEUE = 0,  /* enqueue firings while pool is saturated */
    URBI_EXHAUST_DROP  = 1,  /* drop firings while pool is saturated */
} urbi_exhaust_policy_t;

int urbi_watcher_set_exhaust_policy(urbi_state_t *vm,
                                    urbi_watcher_handle_t handle,
                                    const urbi_exhaust_policy_t policy);
```

Properties:

- Enum values are explicitly numbered. Stable ABI for bytecode + C API callers.
- `URBI_EXHAUST_QUEUE = 0` is the v1 default. Any new value added later goes at the end; existing values never shift.
- Compile-time typo detection: `urbi_watcher_set_exhaust_policy(vm, h, URBI_EXHAUST_QUUEUE)` fails to compile.
- Refactor safety: renaming the enumerator updates every call site via the compiler.

Internal-only enums (scheduler states, GC colors, bytecode dispatch kinds) follow the same pattern but live in the subsystem's `.c` as `static typedef enum`, not in `urbi.h`. The API-boundary distinction matters because audit surface is set by the public header.

### 3.3 urbiscript side: singleton objects on a well-known prototype

The urbiscript surface mirrors the C enum using a singleton prototype with constant slots:

```urbi
// Defined by the runtime, exposed globally.
ExhaustPolicy = Object.clone()
ExhaustPolicy.Queue = Object.clone()  // singleton instance
ExhaustPolicy.Drop  = Object.clone()  // singleton instance

// Slots are marked const at prototype definition time — any attempt to
// reassign raises a runtime error at the assignment.
ExhaustPolicy.setConstSlot("Queue")
ExhaustPolicy.setConstSlot("Drop")

// Usage:
mytag.exhaust_policy = ExhaustPolicy.Queue
mytag.exhaust_policy = ExhaustPolicy.Drop
```

Properties:

- `ExhaustPolicy.Queue` and `ExhaustPolicy.Drop` are the only legitimate values. Assigning anything else to a slot that accepts `ExhaustPolicy` raises `TypeError` at the slot setter.
- Typo on the value name: `mytag.exhaust_policy = ExhaustPolicy.Quueue` raises `SlotNotFound` at evaluation time, before the firing path runs. Far easier to diagnose than a runtime dispatch that silently falls through to a default.
- The slot setter on `mytag.exhaust_policy` checks `value.isA(ExhaustPolicy)` — a non-ExhaustPolicy value (including a random String `"queue"`) raises `TypeError` at the assignment, not at firing time.
- The C side marshals the value across the boundary via its matching enum: `ExhaustPolicy.Queue` → `URBI_EXHAUST_QUEUE`. The singleton carries a hidden integer slot holding the C enum value, visible to runtime dispatch but not re-assignable from urbiscript.

### 3.4 Generalization

This rule applies to every enum-shaped category crossing the API boundary, not just exhaust policy. As the runtime grows, expect:

- `BlendMode.Queue`, `BlendMode.Discard`, `BlendMode.Mix` — tag blending.
- `WatcherMode.Sync`, `WatcherMode.Async` — `at` vs `at sync`.
- `EventKind.Enter`, `EventKind.Leave`, `EventKind.Fire` — tag event lifecycle.
- `GCState.Idle`, `GCState.Marking`, `GCState.Sweeping` — GC state machine for inspection via `urbi_gc_state()`.

Each gets a typed `urbi_<noun>_t` enum on the C side and a matching singleton prototype on the urbiscript side. No new API boundary crosses a magic string. PRs that add one get bounced at review.

### 3.5 What this buys

- **Type discipline.** Safety-focused C conventions treat categorical values as distinct types; enums are the canonical expression. Magic strings collapse the category into `char *`, which is not a category — the type system loses information the programmer intended to preserve.
- **Typo safety.** Caught by the C compiler on one side, by the slot-not-found path on the other. Neither side permits "this might be a typo that compiles and runs with silent fallback."
- **Refactor safety.** Renaming an enumerator updates every call site mechanically on the C side and forces a slot-rename commit on the urbiscript side. The number of overlooked sites is zero.

---

## 4. Bytecode format descriptor

urbi-embedded bytecode is per-ABI-flavor, not portable. The header carries an explicit descriptor; the loader refuses a mismatch with a diagnostic. There is no run-time coercion.

### 4.1 Header layout

Every `.urb` bytecode file starts with a magic number, a version byte, and the 8-byte format descriptor:

```
offset 0     magic          4 bytes    "URBI"  (0x55 0x52 0x42 0x49)
offset 4     version        1 byte     bytecode format version (v1: 0x01)
offset 5     (reserved)     3 bytes    padding to 8-byte alignment (zero)

offset 8     format descriptor (8 bytes):
  byte 0:    int_width      1 byte     v1: always 8 (i64)
  byte 1:    float_type     1 byte     v1: 4 (f32) or 8 (f64)
  byte 2:    instr_width    1 byte     v1: always 4 (uint32)
  byte 3:    endianness     1 byte     v1: 0 (little) or 1 (big); diagnostic only
  byte 4-7:  reserved       4 bytes    future flags (Integer subtype, alt VM modes, etc.)
```

After the descriptor: varint-encoded sections (constant pool, function table, instructions, synclines, symbol table). All size and integer fields in the body are varint — Lua 5.5's `ldump.c` approach. No endianness issue in the body because varints are byte-level. The only fixed-width fields in the format are Integer and Float constants in the constant pool; those are endian-correct for the declared flavor.

### 4.2 v1 supported combinations

| `int_width` | `float_type` | `instr_width` | Target triples |
|---|---|---|---|
| 8 | 8 | 4 | `linux-x86_64`, `stm32h7` |
| 8 | 4 | 4 | `cortex-m4`, `esp32-c3`, `esp32-s3` |

Every other combination (e.g. `int_width=4`, `float_type=16`) is refused at load time in v1. The reserved bytes leave room for future extensions without a header-version bump.

### 4.3 Loader behavior on mismatch

The loader reads the header, compares each field against the VM's compile-time constants (`URBI_INT_WIDTH`, `URBI_FLOAT_TYPE`, `URBI_INSTR_WIDTH`), and if any field disagrees:

- returns `URBI_ERR_BYTECODE_FLAVOR_MISMATCH`,
- sets the VM's last-error buffer to a diagnostic of the form
  `"bytecode flavor mismatch: expected float_type=8 (f64), got float_type=4 (f32)"`,
- does not attempt partial load or coercion.

The diagnostic names the specific field that disagrees so the user can fix their toolchain invocation without reading a hex dump. Endianness mismatch is diagnostic-only — it produces the same refusal, but v1 ships only little-endian targets, so this field is there for future-proofing, not current use.

### 4.4 Compiler tool: `urbi-compile --target`

The host bytecode compiler is target-aware:

```sh
urbi-compile --target=cortex-m4   file.u -o file.urb   # int_width=8 float_type=4
urbi-compile --target=stm32h7     file.u -o file.urb   # int_width=8 float_type=8
urbi-compile --target=linux-x86_64 file.u -o file.urb  # int_width=8 float_type=8
urbi-compile file.u -o file.urb                         # host-native (default)
```

The default target when `--target` is unspecified is `host-native` — the flavor matching the machine running `urbi-compile`. This makes local development friction-free and forces embedded builds to be explicit, which is the right direction. Cross-build recipes in the user's Makefile or buildroot config always name the target explicitly.

v1 ships five supported triples as above. New triples become supported by adding a row to the compiler's target table and teaching the loader to accept the new flavor combination. No format-version bump required as long as the field widths stay within the declared byte layout.

### 4.5 Why per-ABI rather than universally portable

The alternative — a single portable bytecode with per-target coercion at load time — has been rejected. Reasons:

- **Coercion is either lossy or expensive.** f64→f32 loses precision. i64→i32 loses range. Doing the conversion at load time hides a silent behavior change behind a nominal compatibility guarantee.
- **Load-time coercion still requires runtime tagging that distinguishes the original.** Which means the VM has to carry both widths anyway.
- **The user's toolchain knows the target.** Baking flavor into the artifact is an honest declaration, not a lost flexibility. If someone wants to ship one `.urb` to run on both, they ship two `.urb` files — the cost is 2x the flash for bytecode, not 2x the runtime.

Lua 5.5's partial bytecode portability is a design compromise we're declining on purpose.

---

## 5. Per-watcher exhaust policy

When an `at` or `whenever` watcher's condition fires faster than its body coroutine pool can absorb, the runtime dispatches based on the watcher's exhaust policy. Policy is configurable per-watcher, with a VM-wide default and static compile-time pool sizing.

### 5.1 Model recap

- `at` / `whenever` register a **watcher record** (~120 B).
- Each firing that does not run inline (i.e. not `at sync`) spawns a **body coroutine** (~1 KB) drawn from a bounded pool.
- When the pool is saturated, the exhaust policy on the watcher decides what happens to the incoming firing.

### 5.2 v1 policy values

| Enum | urbiscript surface | Behavior when pool saturated |
|---|---|---|
| `URBI_EXHAUST_QUEUE` | `ExhaustPolicy.Queue` | Enqueue the firing in the per-watcher pending-firing queue (depth = `URBI_COROUTINE_QUEUE_DEPTH`). Run when a pool slot frees. If the queue itself is full, behavior falls through to drop (with a debug-build warning). |
| `URBI_EXHAUST_DROP` | `ExhaustPolicy.Drop` | Silently drop the firing. Bump a per-watcher counter visible via `mytag.dropped_count`. |

`URBI_EXHAUST_QUEUE` is the v1 default. Additional policies (`URBI_EXHAUST_RESTART`, `URBI_EXHAUST_COALESCE`) are backlog for v1.x.

### 5.3 urbiscript API

The watcher's owning tag carries the exhaust policy slot. Assignment changes the policy for every watcher under the tag:

```urbi
sensor_loop: every(10ms) read_sensor(),
sensor_loop.exhaust_policy = ExhaustPolicy.Drop

// One-off at:
at (battery.level < 0.1) sound_alarm(),
// (anonymous watcher — uses VM default unless explicitly tagged)

// Grouped tag, shared policy:
critical: {
    at (temp > limit) shutdown(),
    whenever (fault_bit) log_fault(),
}
critical.exhaust_policy = ExhaustPolicy.Queue
```

Reading the slot returns the current policy singleton. Assigning a non-ExhaustPolicy value raises `TypeError` at the assignment (§3).

### 5.4 C API

```c
/* Set the VM-wide default applied to watchers that don't override. */
int urbi_set_coroutine_exhaust_default(urbi_state_t *vm,
                                       const urbi_exhaust_policy_t policy);

/* Per-watcher override. handle is returned by urbi_watcher_register. */
int urbi_watcher_set_exhaust_policy(urbi_state_t *vm,
                                    urbi_watcher_handle_t handle,
                                    const urbi_exhaust_policy_t policy);

/* Query. */
urbi_exhaust_policy_t urbi_watcher_get_exhaust_policy(const urbi_state_t *vm,
                                                     urbi_watcher_handle_t handle);
```

Host apps that want a project-wide policy call `urbi_set_coroutine_exhaust_default` once at init. Per-watcher overrides happen from urbiscript (§5.3) or from C when the host is managing watchers externally.

### 5.5 Compile-time knobs

The pool and queue are statically sized; no dynamic growth, no surprise allocation under load. Host sets these via `-D` flags at build time or in a project header:

| Macro | Meaning | 16 KB M4 | 64 KB ESP32-C3 | Linux |
|---|---|---|---|---|
| `URBI_COROUTINE_POOL_SIZE` | concurrent body-coroutine ceiling | 4 | 20 | 512 |
| `URBI_COROUTINE_QUEUE_DEPTH` | pending firings queued per watcher before fall-through | 8 | 32 | unbounded |

"Unbounded" on Linux is modeled as a deque with `size_t` capacity, growing via the host allocator; embedded builds get the fixed ring buffer.

### 5.6 Per-watcher byte in the record

The watcher record allocates one byte for the exhaust policy enum (leaving room for up to 255 future policies without widening). It sits adjacent to the mode-flags byte (sync/async, edge/level), keeping the record packed. The `dropped_count` field is `uint32_t`; overflow wraps but the odds of 4e9 drops in a running session without the user noticing are nil.

### 5.7 Tag grouping interaction

Assigning `tag.exhaust_policy = p` walks the tag's watcher chain and updates every watcher's record in place. O(n) in the chain length, typically <10. Subsequent watcher registrations under the same tag inherit the tag's policy at registration time — they don't pointer-chase back to the tag at every firing.

Tags that contain sub-tags propagate exhaust policy on assignment but not on registration; a sub-tag established before the parent's policy was set keeps its own policy unless explicitly reassigned. This mirrors the `blend` mode inheritance pattern from legacy 2.x spec §20.

---

## 6. Core/aux C API contract

The public C API is split across two headers with a strict rule: **aux must be strictly implementable via core public functions only**. Not "mostly", not "except where performance demands otherwise". Strictly.

### 6.1 Shape

```
src/urbi.h       — core runtime API       (target: < 80 functions at v1)
src/urbi.c       — core implementation     (one TU)
src/urbi_aux.h   — convenience layer       (target: ~40–60 functions by v1.x)
src/urbi_aux.c   — convenience implementation (separate TU)
```

The two TUs compile independently. A target with tight flash can link `urbi.c` only and omit aux entirely; certification builds do the same to shrink the audit surface. Hosted / REPL / development builds link both.

### 6.2 What `urbi.h` contains

Core functionality — functions where the runtime uniquely provides the capability, where aux wrapping wouldn't reduce the work:

- **State lifecycle.** `urbi_state_new`, `urbi_state_free`, `urbi_set_allocator`, `urbi_set_error_sink`.
- **Bytecode loading.** `urbi_load_bytecode`, `urbi_bytecode_header_read`.
- **Evaluation.** `urbi_eval`, `urbi_call`, `urbi_resume`.
- **Tag control.** `urbi_tag_new`, `urbi_tag_stop`, `urbi_tag_freeze`, `urbi_tag_set_blend`.
- **Memory hooks.** `urbi_alloc`, `urbi_free`, `urbi_gc_step`, `urbi_gc_collect`, `urbi_gc_state`.
- **Timer callbacks.** `urbi_set_time_source`, `urbi_tick`.
- **Scheduler hooks.** `urbi_scheduler_step`, `urbi_scheduler_yield_cb`.
- **Reactive runtime access.** `urbi_watcher_register`, `urbi_watcher_remove`, `urbi_watcher_set_exhaust_policy`, `urbi_set_coroutine_exhaust_default`.
- **Error inspection.** `urbi_last_error_code`, `urbi_last_error_message`.

Each of these is new C code that the runtime has to carry regardless of whether aux exists. Adding one to the core budget costs an API-surface slot and requires proportional justification.

### 6.3 What `urbi_aux.h` contains

Convenience wrappers: patterns that repeat at call sites, worth factoring once, but trivially expressible via core alone. Examples (projected for v1.x):

- `urbi_aux_push_args(vm, fmt, ...)` — varargs wrapper over repeated `urbi_push_int` / `urbi_push_float` / `urbi_push_string` calls.
- `urbi_aux_build_list(vm, n, items)` — iterate and call `urbi_list_append` n times.
- `urbi_aux_repl_bootstrap(vm)` — set up stdin/stdout NDJSON handlers using core's `urbi_set_io_sink`.
- `urbi_aux_format_error(vm, buf, size)` — pull `urbi_last_error_code` and `urbi_last_error_message`, format into the caller's buffer.
- `urbi_aux_load_file(vm, path)` — `fopen`+`fread`+`urbi_load_bytecode` — hosted-only, guarded by `__STDC_HOSTED__`.

Naming: `urbi_aux_*` prefix for every function in this header. No exceptions.

### 6.4 The strict-implementability rule

Every aux function must satisfy: *the entire body is expressible as a sequence of calls to functions declared in `urbi.h`*. Private headers are not available (aux includes only `urbi.h`, not `urbi_internal.h` or subsystem headers). No tricks via shared global state, no "aux-only" internal API, no reaching-past.

Concretely, aux.c looks like:

```c
/* src/urbi_aux.c */
#include "urbi.h"    /* and only urbi.h */
#include "urbi_aux.h"

int urbi_aux_build_list(urbi_state_t *vm, size_t n, const urbi_value_t *items) {
    const int handle = urbi_list_new(vm);
    if (handle < 0) return handle;
    for (size_t i = 0; i < n; i++) {
        const int rc = urbi_list_append(vm, handle, items[i]);
        if (rc < 0) return rc;
    }
    return handle;
}
```

If a proposed aux function cannot be written this way, two paths:

1. **Refactor to make it expressible.** Most failures are lack of imagination; a slightly more general core function unlocks the aux.
2. **Propose a core addition.** Pay the <80 budget. Accept that the new function is a stable ABI commitment.

There is no third path. "Just let this one aux function see the internals" is how every similar split has rotted, every time, in every project that has tried it.

### 6.5 Review-time governance

Every PR adding or changing a function in `urbi_aux.h` gets the question at review:

> Is this function strictly implementable via `urbi.h` public functions only? If you were told `urbi_aux.c` must include only `urbi.h`, would this function compile?

If no, the PR is not merged as-is. The reviewer and author pick path (1) or path (2) from §6.4. The governance check happens even when the author believes they're shaving a microsecond — the rule isn't "aux must be slow", it's "aux must be derivable". Those are orthogonal.

### 6.6 Certification consequence

If v2.0 or later pursues a formal audit path, the audit scope is the core. Aux can be audited separately with its own narrower rule set, or excluded from the certified build entirely. This is the real structural reason for the split — not Lua-parity cosmetics. If aux cheats on the rule, it becomes audit-poisonous, and the split stops buying us anything.

The discipline is a long-term commitment. The v1 payoff is small (a slightly smaller `urbi.h`); the v2 payoff is being able to certify at all.

### 6.7 When aux grows

Don't write aux functions speculatively. Each aux function waits for a real call site in a real consumer (the REPL, the micro-ROS bridge, the stdlib) to motivate it. The target of "~40–60 by v1.x" is a projection of what the observed call-site pressure looks like; it is not a budget to fill.

First aux functions land during M6 when the C API milestone forces them. Empty `urbi_aux.h` at M1 is correct.

---

## Revisions

These conventions are not immutable. When a rule in this doc turns out to be wrong:

1. Raise it in an issue or a commit body first. Don't edit the doc silently.
2. If the change is adopted, update this doc in the same PR / commit series that exercises the change in code. Don't leave the guide stale while the code diverges.
3. Decisions that shaped a past debate stay here with their rationale, even when superseded — so future-me and future-collaborators don't re-derive them from scratch.
