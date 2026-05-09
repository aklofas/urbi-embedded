# Design Decisions

## Overview

This document records design decisions whose rationale is worth preserving long
term but whose current behavior is described elsewhere. Each entry names the
decision, lists alternatives considered, explains why the chosen path won, and
notes what the decision constrains for future work.

The document is organized by decision topic, not by subsystem or milestone. The
intended reader is someone evaluating whether a decision should be changed, not
someone looking up how a feature currently works. For current behavior, follow
the reference links at the top of each entry.

---

## Decision format

Each entry follows this template:

```markdown
### <short decision title>

**Locked:** YYYY-MM-DD
**Status:** active | superseded | under review
**Reference docs:** links to where this is described descriptively

**Decision.** One-paragraph statement.

**Alternatives considered.**
- Alternative name — what it is, why rejected.

**Why this one.** 1–3 paragraphs of reasoning.

**Implications.** What this constrains for future work.
```

---

## Decisions

### Per-target bytecode flavor, not universal portability

**Locked:** 2026-04-21
**Status:** active
**Reference docs:**
[`internals/bytecode-format.md` — Header (24 bytes)](bytecode-format.md#header-24-bytes),
[`internals/bytecode-format.md` — v1.0 Supported Flavor Combinations](bytecode-format.md#v10-supported-flavor-combinations)

**Decision.** The `.urb` bytecode format carries an 8-byte flavor descriptor in
its 24-byte header. The descriptor records `int_width`, `float_type`,
`instr_width`, and `endianness`. The loader checks each field against the VM's
compile-time configuration and refuses any mismatch with a diagnostic that names
the offending field. No run-time coercion of a mismatched file is attempted.

**Alternatives considered.**

- *Universal portable bytecode with load-time coercion.* One `.urb` file runs on
  every target; the loader widens or narrows value types as needed to match the
  host. Rejected: see "Why this one" below.

- *Implicit flavor from context.* Omit the descriptor entirely and let the loader
  infer the format from build configuration. Rejected: silent mismatch is worse
  than loud refusal — an inferred-format file used on the wrong target produces
  garbage arithmetic, not an error.

**Why this one.**

The portability objection to per-target bytecode is that a single `.urb` file
could, in principle, describe a program runnable on any target. That picture
breaks down as soon as you look at what coercion would actually do. Converting
f64 constants to f32 is lossy: `3.141592653589793` becomes
`3.1415927` with ~6 significant digits. Converting in the other direction (f32
to f64) produces a value that is technically wider but carries only f32
precision. The coercion is silent — the user gets a different program than the
one they compiled, with no indication of where the values changed.

Load-time coercion also doesn't simplify the runtime. To handle both widths at
run time, the VM's register file must be able to hold either an f32 or an f64,
which means it carries tagged 8-byte slots regardless. If the target uses f32,
the top four bytes of every float slot are wasted, but they're still allocated.
A per-target flavor avoids that wasted space without sacrificing clarity:
the target's float width is pinned at compile time, the VM is compiled once for
that width, and the loader rejects files compiled for a different target.

Lua 5.5 makes a partial-portability compromise: its bytecode is not formally
portable but the format is not versioned per ABI either. This produces a
situation where some bytecode files happen to transfer between compatible hosts
and others silently corrupt. We choose not to replicate that ambiguity. A `.urb`
file is a per-target artifact. Shipping a program for multiple targets means
shipping multiple `.urb` files — the cost is proportional to flash for the
bytecode sections, not to any runtime overhead.

**Implications.** Build toolchains that produce `.urb` files must know their
target. Cross-compile recipes in a project's Makefile or build config always
name the target explicitly (e.g. `urbi-compile --target=cortex-m4`). The
default target when `--target` is omitted is `host-native`, making local
development friction-free. New targets are supported by adding a row to the
compiler's target table; no format-version bump is needed as long as the field
encoding fits within the declared byte layout.

---

### Register-based VM over stack-based

**Locked:** 2026-04-22
**Status:** active
**Reference docs:**
[`internals/opcodes.md`](opcodes.md),
[`LANG-CONVENTIONS.md` §1.3](../LANG-CONVENTIONS.md#13-arithmetic-semantics)

**Decision.** The VM and bytecode instruction set are register-based. Each
arithmetic instruction names its destination and source registers directly:
`ADD R0, R1, R2`. The emitter carries a register allocator that assigns and
reuses register slots as expressions are compiled.

**Alternatives considered.**

- *Stack-based VM.* Instructions implicitly pop operands from and push results
  onto a stack. Requires no register allocation in the emitter. Rejected: design
  reference and performance data both favor register-based at this scale.

- *Stack + register hybrid.* Use a stack for expression evaluation and named
  registers for locals. Rejected: provides neither the simplicity of a pure
  stack VM nor the density and performance of a pure register VM.

**Why this one.**

Lua's transition from a stack-based VM (Lua 5.0) to a register-based VM
(Lua 5.1) produced measured speedups of 20–50% on typical workloads. The
arithmetic is straightforward: one `ADD R0, R1, R2` instruction replaces the
four instructions a stack VM needs (`PUSH R1`, `PUSH R2`, `OP_ADD`,
`POP R0`). At the bytecode level this is a 4x reduction in instruction count
for binary operations, which is most of what a numeric expression evaluator
does.

The cost in `v0.1.0-skeleton` is a ~100-line stack-based register allocator in
the emitter. That cost is paid once. The benefit — lower instruction count,
simpler dispatch loop, Lua 5.1+ idioms carrying forward — accumulates across
every subsequent release.

**Implications.** Bytecode density is higher than a stack VM, which matters
for flash-constrained targets. The emitter must perform register allocation;
in the initial release this is a simple next-free-register stack with
destination reuse, adequate for expression trees. Later releases (locals,
upvalues, closures) will extend the allocator without changing the opcode
encoding. Lua 5.1+ literature and
tooling applies to the VM design with minimal adaptation.

---

### Byte-aligned 8/8/8/8 instruction encoding

**Locked:** 2026-04-22
**Status:** active
**Reference docs:** [`internals/opcodes.md` — Instruction encoding](opcodes.md#instruction-encoding)

**Decision.** Each `uint32_t` instruction is encoded as four 8-bit fields:
`op(8) | A(8) | B(8) | C(8)` in ABC form, or `op(8) | A(8) | Bx(16)` in ABx
form. Fields align on byte boundaries. Decoding any field is a byte read
followed by a mask or a shift, with no multi-bit boundary crossing.

**Alternatives considered.**

- *Lua 5.5 bit-packed encoding: `op(7) | A(8) | k(1) | B(8) | C(8)`.* The `k`
  flag distinguishes whether operand B is a register index or a constant-pool
  index. Saves one `OP_LOADK` per constant-operand instruction. Rejected: the
  byte-alignment benefit outweighs the density gain at the initial opcode count; the
  `k` flag also complicates the verifier (B means two different things depending
  on the flag).

- *Variable-length instructions.* Common opcodes encode in 2 bytes; rare ones
  use 4 or 6. Rejected: variable-length instructions defeat 4-byte-aligned
  dispatch, make the verifier stateful (you can't scan forward from an arbitrary
  PC without decoding from the start), and complicate any future JIT
  compilation.

**Why this one.**

The 8-bit opcode field is not a waste. Lua 5.5 ships 85 opcodes on a 7-bit
field; the v1.0 opcode budget is 8 in the initial release and realistically under 64 by v1.0
complete. The "lost bit" on the opcode is irrelevant. What byte-alignment buys
is concrete: the verifier's opcode-range check is one comparison, the
disassembler's decoder is three byte reads, and 8-bit-addressable targets
(Cortex-M4, RV32I) decode without sub-byte masking in the hot dispatch path.

Eliminating the `k` flag means one more `OP_LOADK` per constant-operand
instruction. When that density gap becomes measurable, the response is to add
`LOADI`-style immediate variants as distinct opcodes — clearer to disassemble,
simpler to verify, and equivalent in density — rather than retrofitting a flag
bit into the existing encoding. The encoding stays byte-aligned throughout.

**Implications.** The opcode field accommodates 256 distinct opcodes, far more
than v1.0 needs. Reserved opcodes 8–255 are available for later releases without a format
change. Instruction streams are 4-byte aligned in the file (the format includes
an explicit alignment pad before the instruction section), which satisfies the
alignment requirement for any direct-memory dispatch or future JIT code emission.

---

### `OP_LOADK` ABx form with 16-bit constant index

**Locked:** 2026-04-22
**Status:** active
**Reference docs:** [`internals/opcodes.md` — Opcode table](opcodes.md#opcode-table)

**Decision.** `OP_LOADK` uses the ABx instruction form. The destination register
is encoded in the 8-bit A field. The constant-pool index is encoded in the 16-bit
Bx field, supporting up to 65 536 constants per module.

**Alternatives considered.**

- *8-bit constant index with LOADKX overflow opcode.* Use an 8-bit Bx for indices
  0–255; when the index exceeds 255, emit a two-instruction sequence: `LOADKX`
  (which reads the next instruction word as a full constant index) followed by the
  actual `OP_LOADK`. Mirrors Lua 5.4's overflow mechanism. Rejected: 256 constants
  per module is too small for real programs; the two-instruction overhead on every
  out-of-range constant doubles the decode cost for a common case.

- *Varint-encoded constant index.* Pack the index using a variable-length encoding
  to keep small indices short. Rejected: breaks the invariant that every
  instruction is exactly 4 bytes. A variable-length field inside an otherwise
  fixed-width instruction format produces an instruction stream where scanning
  forward by PC requires decoding, not just offsetting.

**Why this one.**

The Bx form provides 65 536 index slots using the space that B and C occupy in
the ABC form. Real programs — even small ones — accumulate constants quickly:
every distinct integer literal, every string, every float literal is a pool
entry. Deduplication reduces the count, but a module that processes an enum with
hundreds of values or a lookup table with dozens of float keys can easily exceed
256 constants without any pathological structure.

The ABx form resolves this with no instruction overhead. One `OP_LOADK` per
constant load, one 16-bit index, the same 4-byte instruction width as every
other opcode. The decode cost is identical to the ABC form.

**Implications.** Constant pools are bounded at 65 536 entries per module. In
practice this ceiling is unlikely to be hit in early releases. If it ever becomes
binding, a `LOADKX` overflow opcode remains a v1.x option: the encoding
reserves the opcode byte space, and the ABx form at full 16-bit range already
handles every realistic case. The Bx field gives the verifier a trivial bounds
check: Bx must be strictly less than the pool's constant count.

---

### Delta-encoded synclines with `INT8_MIN` sentinel

**Locked:** 2026-04-22
**Status:** active
**Reference docs:** [`internals/bytecode-format.md` — Synclines: Delta Encoding](bytecode-format.md#synclines-delta-encoding)

**Decision.** Per-instruction source-line information is stored as a stream of
signed 8-bit delta values. Each byte encodes the difference between the current
instruction's source line and the previous one. The value `INT8_MIN` (`-128`,
`0x80`) is a sentinel: it means the line for this instruction is recorded as an
absolute-line checkpoint in a separate table rather than derivable from the delta
stream alone.

**Alternatives considered.**

- *Uncompressed parallel array.* One 4-byte `(line, col)` pair per instruction.
  Simple to read; direct random access by PC. Rejected: at a 10 000-instruction
  module, this costs 40 KB per module. For a format targeting embedded systems
  where the bytecode section lives in flash, 40 KB of debug overhead per module
  is not acceptable.

- *Uniform 16-bit delta.* Store a 16-bit signed delta per instruction. Handles
  any realistic source file without sentinels. Rejected: 2 bytes per instruction
  doubles the syncline section compared to 1 byte and still can't reach files
  longer than 32 767 lines — an unlikely ceiling, but not a real saving relative
  to the sentinel approach.

- *Run-length encoding.* Store a count alongside each line number, describing
  how many consecutive instructions share that line. Rejected: run-length
  encoding saves space only when many consecutive instructions share the same
  line, which is the common case but not guaranteed. Decoding requires more
  state than delta scanning (you track a remaining-count per run), and the
  density advantage over deltas on typical compiled code is marginal.

**Why this one.**

Most consecutive instructions in a compiled expression-statement are on the same
or adjacent lines. A signed delta of zero means "same line as the previous
instruction"; a delta of +1 or -1 covers line changes within a short source span.
Both fit in a signed byte with room to spare. The sentinel handles the uncommon
case — a long function spanning many source lines where a delta would overflow
an int8 — by writing a checkpoint record into the absolute-line table. The
checkpoint costs 8 bytes (a PC varint and a line varint) and resets the delta
accumulator. Small deltas remain cheap; large jumps are rare and their cost is
proportional to their infrequency.

The approach matches Lua 5.5's delta-with-checkpoint scheme closely. Lua uses
the same signed-byte delta width and an absolute-line table for seek
acceleration; the difference is Lua emits periodic checkpoints at fixed
PC intervals for binary-search lookups, while this implementation emits
checkpoints only when a delta would overflow an int8. The sentinel encoding
collapses the two concepts into a single mechanism: one byte means "use the
delta," one reserved byte value means "read the next checkpoint instead."

**Implications.** Recovering the source line for an arbitrary PC requires
scanning from the nearest absolute-line checkpoint before that PC. This is a
cold-path operation — it only runs when formatting a VM error message, never
during expression evaluation. Checkpoints are emitted whenever the line delta
would overflow an int8, which means deeply nested multi-line expressions or
files with large blank-section jumps produce checkpoints proportionally. Source
files with tightly sequential line numbers never trigger a checkpoint between
the bootstrap record at PC 0 and the end of the function.

---

### Pluggable allocator on `UModule` via `UModuleAllocFn`

**Locked:** 2026-04-22
**Status:** active
**Reference docs:**
[`internals/bytecode-format.md` — Sections](bytecode-format.md#sections),
`src/umodule.h`

**Decision.** The module deserializer (`umodule_deserialize`) accepts a
realloc-semantics allocator callback `UModuleAllocFn`. The host passes in a
function pointer matching `void *(*)(void *ptr, size_t nbytes, void *ud)`.
When `nbytes` is zero, the call is a free. When `ptr` is null and `nbytes` is
nonzero, the call is a malloc. Otherwise it is a realloc. The callback and a
user-data pointer (`ud`) are stored in the `UModule` struct and reused for the
module's lifetime, including `umodule_destroy`.

**Alternatives considered.**

- *Hard-wired `malloc` / `free`.* Call standard library allocation directly.
  Simple. Rejected: the runtime targets embedded systems where there is no
  standard library allocator. On FreeRTOS or bare-metal targets the host
  provides a pool allocator or a bump allocator from a statically declared
  buffer; hard-wiring `malloc` makes the loader unusable without a hosted libc.

- *Separate static-buffer and stdlib variants, like `uarena`.* Provide
  `umodule_deserialize_static(buf, size, ...)` and
  `umodule_deserialize_stdlib(...)` as two entry points. The arena module uses
  this shape to manage the `__STDC_HOSTED__` boundary. Rejected for modules: the
  arena is initialized once and then used in place; a module loaded at runtime
  on an embedded target is read from flash or a communication interface, and the
  number of modules loaded is not known at compile time. A static-buffer variant
  would require the host to size a single buffer for the largest module it will
  ever load — an impractical constraint for a runtime that may receive code over
  a remote REPL or load bytecode from a file system.

**Why this one.**

The callback approach puts the host in control of every allocation the module
loader performs without requiring the loader to know anything about the host's
memory subsystem. A FreeRTOS target wires in `pvPortMalloc` / `vPortFree`; a
pool-allocator target wires in its pool; a hosted target wires in
`realloc` / `free`. The loader code changes not at all.

The realloc-semantics callback is also how the upstream allocator hook
(`urbi_set_allocator`) at the VM level will work. Adopting the same shape in
the module loader means the two APIs compose naturally: a host that sets a
custom allocator at VM init can pass the same callback down through bytecode
loading without any adaptation.

The callback is stored on the `UModule` struct so that `umodule_destroy` can free
the module's sections through the same allocator that allocated them. This is
essential on embedded targets where the allocator is stateful — freeing through
a different allocator than the one used to allocate is a hard error.

**Implications.** Hosts that want to lock down their heap after startup — a
common embedded pattern where all dynamic allocation happens at boot and none
happens thereafter — can wire in an allocator that panics on any call after a
lock flag is set. Because every module allocation goes through the callback,
this pattern is enforced mechanically rather than by audit. The same property
holds for certified builds that must demonstrate no dynamic allocation during
normal operation: load all required modules at startup through the allocator,
lock the heap, run. The allocator callback records when locking was requested
and asserts if a post-lock allocation is attempted.

---

### Uniform 16-byte `UValue` value layout

**Locked:** 2026-04-23
**Status:** active
**Reference docs:**
[`internals/bytecode-format.md` — Header (24 bytes)](bytecode-format.md#header-24-bytes),
[`../LANG-CONVENTIONS.md` §1.1](../LANG-CONVENTIONS.md#11-numeric-types),
`src/umodule.h`

**Decision.** Every urbi-embedded target — 64-bit Linux, 32-bit Cortex-M,
RISC-V rv32imc — uses the same 16-byte `UValue` tagged struct for both
constants-pool entries and runtime register values. No per-host NaN-boxing;
no per-host split.

**Alternatives considered.**

- *NaN-boxing on 64-bit hosts, tagged struct on 32-bit.* Pack type tag and
  payload into a single `uint64_t` using the unused bits of a quiet NaN.
  Reduces register-file size from 16 bytes to 8 bytes on 64-bit hosts.
  Rejected: see "Why this one" below.

- *Pointer-tagging (low-bit tags on aligned pointers).* Use the low bits of
  a `void *` to carry a type tag for pointer-sized values. Rejected: does
  not accommodate i64 Integer values that exceed the pointer width on 32-bit
  targets; also non-portable across ABIs that do not guarantee pointer
  alignment beyond 1 byte.

**Why this one.**

The 2026-04-19 impl-design spec committed to NaN-boxing on 64-bit hosts (8-byte
values; type tag packed into unused bits of a quiet NaN; payload capped at
~48 bits). That commitment predates the 2026-04-21 language-and-runtime spec
which locked Integer = i64 across every target. A 64-bit integer does not fit
in a 48-bit NaN payload, so NaN-boxing would require heap-boxing any Integer
with |x| > ~1.4e14. That threshold is routinely crossed by monotonic ns
timestamps (a system running for less than two days crosses it), so programs doing time arithmetic
would hit the heap-boxed path in normal use, forcing allocation during
arithmetic and violating the "no emergency GC inside the allocator" commitment
from the language-and-runtime spec §2.2.

Lua 5.5 reached the same conclusion when Lua 5.3 added i64 Integer: Lua dropped
NaN-boxing and uses a uniform 16-byte `TValue` across all platforms. We follow
the same path.

**Cost.** Eight extra bytes per register on 64-bit hosts compared to a
hypothetical NaN-boxed layout. A typical function frame is <16 registers = 256
bytes, compact enough that active registers stay hot in L1 cache on any target. On the embedded targets that
actually constrain the RAM budget, the tagged struct would have been required
regardless — NaN-boxing savings never applied to 32-bit targets.

**Implications.** The register file is uniformly 16 bytes wide per slot. The
dispatch loop decodes tag and value with two field reads; no NaN-pattern
matching, no bit masking of payloads. Future GC write barriers operate on the
tag byte directly. Adding new value kinds (object pointer, bool, nil, symbol)
requires a new tag constant and a new union member in `UValue`, without
touching the bit-packing scheme.

**See also.**

- `2026-04-23-urbi-embedded-vm-design.md` §2.1 — full rationale with worked
  examples
- `../LANG-CONVENTIONS.md` §1.1 — Integer = i64 decision
- `src/umodule.h` — `UValue` struct definition

---

### No global mutable state

**Locked:** 2026-04-24
**Status:** active
**Reference docs:**
[`internals/architecture.md` — Multi-VM model](architecture.md#multi-vm-model),
`tools/audit-globals.sh`

**Decision.** No mutable datum may live at file scope in any `src/*.c` translation
unit. Every piece of state that changes at runtime must live on a `UVM` struct (or
on a struct owned by a `UVM`). Only compile-time constant tables — opcode name
arrays, version strings, static error-message literals — are permitted at file scope.

**Alternatives considered.**

- *Process-global singletons.* A single intern table or IC generation counter
  shared by all VM instances. Rejected: breaks multi-VM isolation; two independent
  VMs in the same process would corrupt each other's intern pools and IC state.

- *Thread-local storage (`__thread` / `_Thread_local`).* One datum per thread rather
  than one per process. Rejected: adds a threading-model assumption; freestanding
  targets and embedded RTOSes may not support TLS; still violates the design goal
  that a single thread can host multiple independent VMs.

**Why this one.**

The empirical baseline at v0.1.0-skeleton was zero non-const file-scope mutable
variables. The rule codifies that baseline as a forward constraint enforced
mechanically rather than by audit: the `cppcoreguidelines-avoid-non-const-global-variables`
clang-tidy check (enabled in `.clang-tidy`, gated under `make lint`) rejects any
new mutable file-scope definition at CI time. The `tools/audit-globals.sh` script
provides a secondary human-readable report. Both run in the CI `lint` job.

**Implications.** Every new mutable datum added after the initial release must land on `UVM`
or on a struct that `UVM` owns. Subsystem authors may not use `static` local
variables for mutable state that differs per-VM. This is a structural analogue of
Lua's `lua_State`-as-root design: Lua earns multi-VM embeddability precisely because
every mutable Lua datum lives on a `lua_State`.

---

### Tagged-pointer prototype chain

**Locked:** 2026-04-29
**Status:** active

**Decision.** Object prototype chains are stored as a tagged pointer with
three forms: zero-proto (a tagged null), single-proto (a tagged pointer
to one parent), and multi-proto (a tagged pointer to a heap-allocated
parent array). The tag bits encode which form, so single-proto lookup —
the dominant case — does not allocate.

**Alternatives considered.**

- *Always-array prototype chain.* Every object holds a `UObject **`
  parent array even when one parent is the common case. Rejected: an
  extra allocation per object, ~16 B overhead per single-proto object,
  no upside.
- *Linked-list prototype chain.* Each parent points to the next.
  Rejected: extra cache miss per traversal step, no benefit on
  modern hardware.

---

### Inline cache: 4 entries per call site

**Locked:** 2026-04-29
**Status:** active

**Decision.** Each slot-access call site carries 4 IC entries by
default; the embedded-footprint preset (`URBI_IC_ENTRIES_PER_SITE=2`)
narrows this to 2. Entries are organized as a small linear scan; on
miss, the slot lookup walks the prototype chain and writes a new
entry, evicting the least-recent if the table is full.

**Alternatives considered.**

- *Single-entry IC.* Smaller; thrashes badly under polymorphism.
- *Hash-table IC.* Larger; more memory traffic; doesn't pay for itself
  at the working-set size typical for embedded scripts.

---

### Scratch-frame primitive for AT_SYNC body execution

**Locked:** 2026-05-05
**Status:** active

**Decision.** The sync-execution sites in the reactive runtime (install
cond, eval cond, AT_SYNC body, eval/drain onleave, event-sync emit body)
all route through `urbi_run_closure_on_scratch` (and its
`_with_payload` variant). The primitive spins up an ephemeral `UStrand`,
runs the body closure with a fresh register window, and tears the strand
down on completion.

**Alternatives considered.**

- *Body-inlined emit at each site.* Original approach. Each AT_SYNC site
  emitted the body inline, which made register-allocation drift between
  sibling sites a recurrent bug class.
- *Strand-pool reuse.* Strands recycled across watcher fires.
  Rejected: complicates the GC walker contract; the per-fire allocation
  cost is small compared to the body's own work.

---

### Cooperative single-threaded VM as `v1.0` baseline

**Locked:** 2026-04-28
**Status:** active

**Decision.** The `URBI_SCHED_COOPERATIVE` scheduler is the only
implementation shipped at `v1.0`. Each `UVM` is driven by one thread at
a time; multi-threaded scheduling (`URBI_SCHED_PREEMPTIVE`) is a
post-v1.0 expansion. Several primitives (the `UModuleInstance`
walk-then-prepend, the deferred slot-change emit ring) assume
single-threaded VM and are flagged for revisit when multi-threaded
scheduling lands.

**Alternatives considered.**

- *Multi-threaded VM at v1.0.* Rejected as scope; the contract surface
  for cross-thread closure handoff and GC barriers is large enough to
  warrant a separate design pass.

---

### Bytecode wire format `v1.5` exact-match policy

**Locked:** 2026-05-07
**Status:** active

**Decision.** A bytecode module is loadable only by a runtime whose
wire-format version exactly matches the producer's. There is no
in-band migration. Live-system bytecode upgrade tooling — required for
embedded deploys that cannot rebuild from source — is a post-v1.0
roadmap item.

**Alternatives considered.**

- *Forward-compatible loader.* Older runtimes accept newer modules
  with backward-compatible features. Rejected: forces every wire-format
  decision to commit to a forward-compat envelope; no upside until live
  upgrade tooling lands.
- *Backward-compatible loader.* Newer runtimes accept older modules,
  rewriting opcodes on load. Rejected: same upside as live upgrade
  tooling but ships earlier; better to design the upgrade tooling
  separately and load it as an opt-in pass.

---

### Strict tooling as quality contract

**Locked:** 2026-05-09
**Status:** active

**Decision.** Four strict-tooling gates hard-fail in `make releasetest`:
cppcheck-strict (across all categories), tidy-strict (across all
categories), scan-build (clang static analyzer), docstring-coverage
(every header-exposed declaration in `include/urbi/*.h` carries a
contract docstring). A green `make releasetest` is the canonical
signal that the codebase is ready to ship.

**Alternatives considered.**

- *Advisory-only.* Tools run; warnings logged; no fail. Rejected:
  warnings accumulated; no enforcement.
- *Per-tool opt-in.* Each subsystem opts into each tool. Rejected:
  fragments enforcement across the tree.
