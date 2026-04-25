# Peer Languages

This document compares urbi-embedded against other embeddable scripting languages targeting microcontroller-class hardware. It exists to set honest expectations about where urbi-embedded is novel, where it overlaps with existing tools, and where another option may fit a project better.

The comparison is grouped by relevance to urbi-embedded's design. Languages with similar deployment targets and concurrency models are covered first, followed by languages sharing only some axes, and finally a wider list of options for context.

For the design rationale behind urbi-embedded's choices, see [internals/design-decisions.md](../internals/design-decisions.md). For language conventions, see [LANG-CONVENTIONS.md](../LANG-CONVENTIONS.md). For supported platforms and footprint targets, see [README.md](../README.md).

---

## At a glance

What urbi-embedded contributes that the surveyed alternatives do not, in combination:

- **Statement-separator concurrency.** Sequential vs. parallel composition is encoded in the choice of statement terminator (`;` `|` `,` `&`), not in library calls.
- **Language-level reactive watchers** over arbitrary expressions: `at (cond) body`, `whenever (cond) body`, `waituntil (cond)`. Watchers re-arm and persist as first-class language constructs, not callback registrations.
- **First-class tags** as scope-aware cancellation handles: `mytag: every(100ms) sense(); mytag.stop()` cancels every coroutine and watcher in the tag scope.
- **Time as language-level literal**: `100ms`, `1s`, `180deg`, `pi`.

What the surveyed alternatives have that urbi-embedded does not:

- **AtomVM**: preemptive scheduling, BEAM compatibility, distributed Erlang.
- **Toit**: fleet-management cloud platform, OTA updates, IDE integration tier.
- **MicroPython / CircuitPython**: vast Python ecosystem and educational reach.
- **Berry**: ten-year+ deployment in Tasmota smart-home firmware.
- **mruby/c, PicoRuby**: full Ruby semantics on MCU.

The intent is not to be the only option in this space — it is to be the right option when the language-level features above match the application.

---

## Feature matrix

The matrix groups languages by their primary niche. Each row is a verified fact as of April 2026; rows marked "—" are absent rather than unknown.

### MCU-deployed peers

| Language | Concurrency | Reactive watchers | Object model | Min RAM | Min flash | License | Implementation |
| --- | --- | --- | --- | --- | --- | --- | --- |
| **urbi-embedded** | Statement-level parallel-by-default; cooperative scheduler | `at`, `whenever`, `waituntil` over arbitrary expressions | Prototype-based, multi-proto inheritance | TBD (target: tens of KB at min config) | TBD | BSD-3-Clause | C99, freestanding |
| [AtomVM](https://www.atomvm.net/) | Preemptive (Erlang BEAM) | `receive` (message-pattern await) | Functional / actor | 128 KB | 512 KB (1 MB recommended) | Apache-2.0 | C, Erlang, Elixir |
| [Toit](https://toit.io/) | Cooperative tasks; monitor library (Latch / Channel / Semaphore / Mutex / Mailbox) | `pin.wait-for` (single-pin library function) | Class-based, single inheritance | ~500 KB (ESP32-class) | — | LGPL-2.1 (VM), MIT (stdlib) | C++ |
| [MicroPython](https://micropython.org/) | `_thread` (cooperative); `uasyncio` (event loop) | `Pin.irq` callbacks | Python classes, MRO | 16 KB | 256 KB | MIT | C99 |
| [CircuitPython](https://circuitpython.org/) | `asyncio` only (single-threaded by design) | `Pin` events (library) | Python classes | 16 KB | 256 KB | MIT | C99 (MicroPython fork) |
| [mruby/c](https://github.com/mrubyc/mrubyc) | Priority-based scheduler ("rrt0"); cooperative + preemptive paths; first-class `Task` class | — | Ruby classes | <40 KB | <64 KB | BSD-3-Clause | C99 |
| [PicoRuby](https://github.com/picoruby/picoruby) | Inherits mruby VM (PicoRuby variant) or mruby/c VM (FemtoRuby variant) | — | Ruby classes | 128 KB (FemtoRuby) / 512 KB (PicoRuby) | varies | MIT | C, Ruby |
| [Berry](https://github.com/berry-lang/berry) | — | — | Single-inheritance classes | <4 KB heap | <40 KB | MIT | C |
| [Espruino](https://www.espruino.com/) | JavaScript event loop (single-threaded) | `setWatch` (single-pin library function) | JavaScript prototypes | 8 KB | 128 KB | MPL-2.0 | C99 |
| [PikaPython](https://github.com/pikasTech/PikaPython) | — | — | Python class subset | 4 KB | 64 KB | MIT | C |
| [Microvium](https://github.com/coder-mike/microvium) | — | — | JavaScript subset | 64 B idle | <16 KB | MIT | C, build-time TypeScript |
| [Elk](https://github.com/cesanta/elk) | — | — | JavaScript subset (ES6) | ~100 B core | ~20 KB | AGPLv3 / commercial | C |
| Mecrisp Forth | Cooperative `MULTITASK` / `PAUSE` primitives | — | None (stack) | <2 KB | ~12 KB | GPL | Assembly + Forth |

### Embeddable, primarily desktop / server class

| Language | Concurrency | Reactive | Object model | License | Implementation |
| --- | --- | --- | --- | --- | --- |
| [Lua](https://www.lua.org/) | Coroutines | — | Tables + metatables | MIT | C |
| [Luau](https://luau.org/) | Coroutines | — | Tables + metatables | MIT | C++ (C++11 runtime) |
| [Janet](https://janet-lang.org/) | Cooperative fibers; `ev/spawn`, `ev/go`, supervisor channels, structured concurrency via `ev/gather` | — | Tables + prototypes | MIT | C99 |
| [Wren](https://wren.io/) | Language-level fibers (`Fiber.yield`, `Fiber.transfer`) | — | Class-based | MIT | C99 |
| [Gravity](https://github.com/marcobambini/gravity) | Fibers (Wren-inspired) | — | Class-based, Swift-like | MIT | C |
| [Squirrel](http://squirrel-lang.org/) | Generators / coroutines | — | Class-based, table-based | MIT | C++ |
| [QuickJS](https://bellard.org/quickjs/) / QuickJS-NG | Promises (host-driven loop) | — | JavaScript | MIT | C99 |
| [Duktape](https://duktape.org/) | — | — | JavaScript | MIT | C99 |
| [JerryScript](https://jerryscript.net/) | — | — | JavaScript | Apache-2.0 | C |
| [Cyber](https://cyberscript.dev/) | Fibers (`coinit`, `coresume`) | — | Dynamic | MIT | Zig |

### Synchronous reactive — language family kin

These languages predate or run parallel to urbi-embedded's reactive design. None are embeddable interpreters in the same sense; they are mostly compilers targeting C.

| Language | Reactive primitives | Implementation form | Notes |
| --- | --- | --- | --- |
| [Esterel](https://en.wikipedia.org/wiki/Esterel) | `await`, `every`, `present`, `\|\|` | Compile-to-C | Foundational synchronous language (1980s) |
| [Lustre](https://en.wikipedia.org/wiki/Lustre_(programming_language)) | dataflow equations | Compile-to-C | Industrial deployment via SCADE Suite |
| [SCADE](https://www.ansys.com/products/embedded-software/ansys-scade-suite) | dataflow + state machines | Industrial codegen tool | DO-178C qualified for avionics |
| [Céu](https://ceu-lang.org/chico/) | `await`, `emit`, `par/and`, `par/or`, `every`, `spawn` | Compile-to-C | Closest semantic peer; designed for embedded soft-real-time |

---

## Detailed comparison — closest peers

### AtomVM

**Repository:** [github.com/atomvm/AtomVM](https://github.com/atomvm/AtomVM)
**Latest stable:** v0.6.6 (June 2025); v0.7.0-alpha includes distributed Erlang and JIT (March 2026)
**License:** Apache-2.0

AtomVM is a lightweight implementation of the Erlang BEAM virtual machine targeting microcontrollers. It runs unmodified Erlang, Elixir, and Gleam BEAM bytecode on ESP32 (520 KB RAM / 4 MB flash typical), STM32 (verified on F411CE with 128 KB RAM running its sudoku benchmark), and Raspberry Pi Pico / Pico 2.

The concurrency model is the BEAM model unchanged: per-process heaps, message passing as the only synchronization primitive, supervisors as first-class restart strategies, and **preemptive scheduling**. Each process is independently garbage collected, so a long-running process cannot pause unrelated processes.

**Where it overlaps with urbi-embedded.** Both run a managed-language VM on Cortex-M / RP2040 / ESP32 hardware. Both expose process-like concurrency at language level. AtomVM's `receive` with pattern matching plays a role similar to urbi-embedded's `at` watchers — both pause execution awaiting a condition.

**Where it differs.** AtomVM inherits Erlang's syntax, type system, and libraries; urbi-embedded uses a prototype-based object model and statement-separator concurrency. AtomVM's reactive primitive is message-pattern awaiting; urbi-embedded's reactive primitives are condition-expression watchers that re-arm. AtomVM is preemptive; urbi-embedded is cooperative with statement-boundary preemption points. AtomVM's restart-strategy supervisors are explicit configuration; urbi-embedded's tag scopes are implicit lexical containers.

**When to pick AtomVM over urbi-embedded.** When the application benefits from BEAM ecosystem reuse (existing OTP libraries, Phoenix LiveView via Popcorn, distributed Erlang clustering across devices), or when preemptive scheduling and per-process GC isolation are firm requirements.

### Toit

**Repository:** [github.com/toitlang/toit](https://github.com/toitlang/toit) · **Site:** [toit.io](https://toit.io/)
**Latest:** v2.0.0-alpha.192 (April 2026)
**License:** LGPL-2.1 (compiler/VM), MIT (standard libraries)

Toit is an object-oriented language designed by Toitware specifically for ESP32 deployment, with cloud-managed OTA updates and live-reload tooling as part of the product. It compiles to a custom bytecode and runs on a battery-optimized VM with cooperative-only multitasking.

Concurrency is exposed through a `task` primitive plus a `monitor` library providing **Latch / Channel / Semaphore / Mutex / Mailbox** as the canonical synchronization menu. Tasks yield only at `sleep` calls and I/O operations like `pin.wait-for`; CPU-bound code that does not yield will starve other tasks. The design explicitly trades preemptive safety for cooperative simplicity, and the documentation calls out the trade-off.

**Where it overlaps with urbi-embedded.** Both expose first-class concurrent tasks at language level. Both target ESP32-class hardware with VMs sized for ~500 KB RAM. Both pair the VM with cooperative-only scheduling and statement-boundary safe points.

**Where it differs.** Toit has no reactive watchers — `pin.wait-for` is a single-pin library function, not a general-expression watcher. Toit's statement separators encode sequence, not concurrency; parallelism happens by spawning tasks. Toit ships as a hosted product with cloud orchestration; urbi-embedded is a self-hosted runtime library.

**When to pick Toit over urbi-embedded.** When fleet management, OTA deployment, and a polished cloud workflow matter as much as the language. Toit's `monitor` library is the canonical reference for synchronization primitives in this space and is worth studying regardless of which runtime is chosen.

### MicroPython and CircuitPython

**Repositories:** [github.com/micropython/micropython](https://github.com/micropython/micropython), [github.com/adafruit/circuitpython](https://github.com/adafruit/circuitpython)
**License:** MIT

MicroPython is the most-deployed managed-language runtime on microcontrollers, with ports to STM32, ESP32, RP2040, nRF52, Teensy, Pyboard, and dozens of others. CircuitPython is Adafruit's fork, prioritizing single-threaded simplicity and educational use.

Both expose Python's full class system with multiple inheritance, dynamic typing, and reference-counted garbage collection. Concurrency is library-level: `_thread` for cooperative threading on most ports, `uasyncio` for an async/await event loop. Hardware events arrive via `Pin.irq()` callbacks.

**Where it overlaps with urbi-embedded.** Both target Cortex-M / ESP32 / RP2040. Both run a managed-language VM with garbage collection and a cooperative scheduler.

**Where it differs.** Python's reference-counted GC and dict-everywhere object model are well-understood but heavier than urbi-embedded's prototype-with-shapes model. MicroPython's `uasyncio` requires `async def` annotations and explicit `await` keywords; urbi-embedded encodes parallel composition in statement separators. Python has no reactive watchers as language constructs.

**When to pick MicroPython.** When Python ecosystem reuse, educational reach, or the broadest hardware port matrix matter. The MicroPython community is the deepest in this space and the documentation is excellent.

### mruby/c and PicoRuby

**Repositories:** [github.com/mrubyc/mrubyc](https://github.com/mrubyc/mrubyc), [github.com/picoruby/picoruby](https://github.com/picoruby/picoruby)
**Latest:** mruby/c v3.4.1 (March 2026), PicoRuby v3.4.2 (March 2026)
**License:** BSD-3-Clause (mruby/c), MIT (PicoRuby)

mruby/c is a smaller-footprint reimplementation of the mruby virtual machine, originally developed at the Shimane IT Open-Innovation Center for industrial use on PIC32 and Cortex-M. PicoRuby is a parallel project that ships in two variants — **PicoRuby** (uses the mruby VM, ≤512 KB RAM, supports networking) and **FemtoRuby** (uses the mruby/c VM, ≤128 KB RAM). Both are actively maintained and ship in real products, including [PRK Firmware](https://github.com/picoruby/prk_firmware), a Pi Pico keyboard firmware written in Ruby.

Verified scheduler details from `src/rrt0.h`: the scheduler is named "rrt0" (realtime multitask monitor) and uses **priority-based scheduling** (default priority 128, separate `priority_preemption` field for effective priority) with a **timeslice counter**. The state enum is bitmapped — `DORMANT` (0x00), `READY` (0x02), `RUNNING` (0x03), `WAITING` (0x04, with reason sub-codes for sleep / mutex / join), `SUSPENDED` (0x08). The `flag_preemption` field on the VM struct indicates that some ports support genuine preemption rather than pure cooperation. The first-class `Task` class supports `pass` (yield), `suspend`, `resume`, `terminate`, `join`, `raise(exception)` (cross-task exception injection), and `value` (retrieve termination result); tasks are named (≤15 characters) and retrievable via `Task.get(name)`. Synchronization is via `mrbc_mutex`.

**Where it overlaps with urbi-embedded.** Both target Cortex-M / ESP32 with sub-100 KB RAM minimum-config goals. Both run a managed-language VM with a priority-aware cooperative scheduler. mruby/c's Task API is conceptually similar in some respects to urbi-embedded's tag-scope cancellation, particularly the `raise` cross-task exception injection.

**Where it differs.** Ruby class model rather than prototypes. No reactive watchers. Concurrency is API-level (`Task.create`, `Task#pass`) rather than syntax-level (statement separators). Tags-as-scopes versus named-tasks-as-handles is a different shape for cancellation.

**When to pick mruby/c or PicoRuby.** When Ruby semantics matter, when integrating with the existing mruby ecosystem is valuable, or when the priority-based real-time scheduler is a firmer fit than urbi-embedded's tag-aware cooperative scheduler.

### Berry

**Repository:** [github.com/berry-lang/berry](https://github.com/berry-lang/berry)
**Latest formal release:** v1.1.0 (August 2022); active development on master through April 2026
**License:** MIT

Berry is a class-based scripting language designed for low-resource devices. It ships as the scripting layer in [Tasmota](https://tasmota.github.io/docs/Berry/), the popular ESP8266/ESP32 smart-home firmware, with millions of deployments. Reported footprint is **<40 KB flash and <4 KB heap on ARM Cortex-M4**, the most aggressive verified footprint for a class-based scripting VM in this survey.

Berry is single-threaded with no concurrency primitives in the language. Event handling in Tasmota is done via host-side callback registration.

**When to pick Berry over urbi-embedded.** When the application is event-driven (a configuration script reacting to host events) and does not need language-level concurrency or reactive watchers. Berry's footprint claim is the bar to clear for "credibly small" embedded scripting.

### Espruino

**Repository:** [github.com/espruino/Espruino](https://github.com/espruino/Espruino) · **Site:** [espruino.com](https://www.espruino.com/)
**License:** MPL-2.0

Espruino is a JavaScript runtime designed for microcontrollers, with shipping consumer products including the Pixl.js and the Bangle.js smartwatch. It uses a custom JavaScript interpreter (not V8 or QuickJS) and runs on nRF52, STM32, and ESP32 with a footprint floor of ~128 KB flash and 8 KB RAM.

Espruino's [`setWatch`](https://www.espruino.com/Reference#l__global_setWatch) function is the closest existing-product analogue to urbi-embedded's `at` — it registers a callback that fires on pin edge events. urbi-embedded generalizes this pattern to arbitrary expressions and persists watchers as language constructs rather than callback registrations.

**Where it overlaps with urbi-embedded.** Prototype-based object model. MCU deployment. Single-pin reactive callback (in Espruino's case via library; in urbi-embedded's case via `at`).

**Where it differs.** JavaScript syntax and semantics. Single-threaded event loop, no language-level concurrency. `setWatch` is single-pin and library-level rather than expression-general and language-level.

### Janet

**Repository:** [github.com/janet-lang/janet](https://github.com/janet-lang/janet) · **Site:** [janet-lang.org](https://janet-lang.org/)
**Latest:** v1.41.2 (February 2026)
**License:** MIT

Janet is a dynamically typed Lisp-flavored language designed for embedding in C and C++ programs. It is the closest peer to urbi-embedded by *language design* in the embeddable-scripting space — both share a prototype-flavored object model, both expose cooperative concurrency at the language level, and both prize structured concurrency.

Janet's [event module](https://janet-lang.org/api/ev.html) provides `ev/spawn` and `ev/go` for creating fibers, `ev/chan` for channels with `ev/give` / `ev/take`, `ev/gather` for structured "wait for all of these" composition, and **supervisor channels** — when a fiber created with `ev/go` terminates, errors, or signals, an event is pushed to a supervisor channel for orchestration. Cancellation is via `ev/cancel` and integrates with `try` / `protect`.

Janet runs on desktop and embedded Linux (~300 KB binary class). It is not deployed on bare-metal MCUs at the time of writing.

**Where it overlaps with urbi-embedded.** Prototype-flavored object model. Cooperative fibers with structured concurrency. Embeddable C99 implementation.

**Where it differs.** Lisp s-expression syntax. No statement separators. Channel-based supervisors rather than scope-membership tags. No reactive watchers in the language. No bare-metal MCU heritage.

### Céu

**Repository:** [github.com/fsantanna/ceu](https://github.com/fsantanna/ceu) · **Site:** [ceu-lang.org](https://ceu-lang.org/chico/)
**License:** Free software (GPL-family)

Céu is a synchronous reactive language by Francisco Sant'Anna designed for embedded soft-real-time systems. It is the closest peer to urbi-embedded by *reactive semantics* — `await`, `emit`, `par/and`, `par/or`, `every`, and `spawn` cover essentially the same niche as urbi-embedded's `at`, `whenever`, `waituntil`, `every`, and the parallel statement separators.

Céu compiles to C; there is no embeddable Céu interpreter. Code is written in Céu, run through the compiler, and the generated C is linked into firmware. This is a different deployment model from urbi-embedded's runtime-VM approach, and it implies different trade-offs: Céu can do compile-time deterministic-concurrency analysis (detecting conflicting parallel writes) that an interpreted runtime cannot.

Sant'Anna's published work on Céu is the most relevant academic literature for the design space urbi-embedded operates in. The TECS 2017 paper "[The Design and Implementation of the Synchronous Language CÉU](https://dl.acm.org/doi/abs/10.1145/3035544)" and the LCTES 2018 paper on bounded-memory deterministic semantics are the canonical references.

**Where it overlaps with urbi-embedded.** Same family of reactive constructs (`await` / `every` / parallel composition). Embedded-soft-real-time target.

**Where it differs.** Compile-to-C tool versus runtime VM. Synchronous semantics (single time-step reaction model) versus urbi-embedded's coroutine-with-statement-boundary-yield model. Custom syntax versus urbiscript syntax.

---

## Detailed comparison — adjacent niches

### Lua and Luau

**Repositories:** [www.lua.org](https://www.lua.org/), [luau.org](https://luau.org/)

Lua is the most-deployed embeddable scripting language in history, the engineering benchmark every embeddable runtime measures itself against. Luau is Roblox's heavily-modified Lua 5.1 fork with gradual typing, a custom optimizing compiler, and optional native code generation for x64 and arm64.

Both use tables-with-metatables as the universal data structure. Inheritance is convention via `__index`. Concurrency is coroutines plus a host-driven scheduler — neither language has language-level concurrency primitives or reactive constructs.

**Where it overlaps with urbi-embedded.** Both expose a managed-language VM via a stack-based C API. Lua's clean separation of compile (`luaL_loadstring`) and load is the prior-art reference for urbi-embedded's `urbi_compile` / `urbi_load` split.

**Where it differs.** Tables-with-metatables versus prototypes-with-shapes; see the object-model trade-off discussion below. Lua and Luau both leave concurrency and reactive constructs to the host. Lua's `__gc` finalizer is similar in spirit to urbi-embedded's `enter` / `leave` tag-scope events but operates at GC time rather than scope-exit time.

### JavaScript engines for embedding

A short survey of the JavaScript options:

- **[QuickJS](https://bellard.org/quickjs/) / [QuickJS-NG](https://github.com/quickjs-ng/quickjs)** — full ES2020+; ~210 KB; runs on ESP32 with ~256 KB RAM.
- **[Duktape](https://duktape.org/)** — ES5/E5.1; ~200 KB / 70 KB RAM; Cortex-M production deployments.
- **[JerryScript](https://jerryscript.net/)** — ES5.1+; ~64 KB possible; originally Samsung; pace appears slowed.
- **[Microvium](https://github.com/coder-mike/microvium)** — JS subset; **<16 KB ROM, 64 B idle RAM** via build-time partial evaluation and snapshot.
- **[Elk](https://github.com/cesanta/elk)** — ES6 subset; **20 KB flash, ~100 bytes RAM core VM**; runs on 8-bit Arduino Nano (2 KB RAM); AGPLv3 / commercial dual license.
- **[MQuickJS](https://github.com/conoro/mquickjs)** — ESP32 fork of MicroQuickJS; 100 KB ROM / 10 KB RAM; fork has limited commits.

None of these expose language-level concurrency beyond promises (which require a host-side event loop) or reactive constructs.

### Smaller class-based / object-oriented options

- **[Wren](https://wren.io/)** — class-based, fibers (`Fiber.yield`, `Fiber.transfer`); v0.4.0 released April 2025 after a two-year gap.
- **[Gravity](https://github.com/marcobambini/gravity)** — class-based with Swift-like syntax, fibers (Wren-inspired); active (v0.9.7 in April 2026); originally developed for the Creo iOS/Android creative tool.
- **[Squirrel](http://squirrel-lang.org/)** — class-based and table-based; ran in production on STM32F205 in the Electric Imp platform. Latest release Feb 2022.

### Stack and concatenative languages

**Forth** dialects ([Mecrisp-Stellaris](https://mecrisp.sourceforge.net/), [FlashForth](https://flashforth.com/)) ship at extreme footprints (Mecrisp at ~12 KB on Cortex-M) and many include language-level cooperative multitasking via `MULTITASK` and `PAUSE` primitives. Forth's stack-everything model is fundamentally different from urbi-embedded's expression-oriented prototype OO, but the engineering discipline of Forth implementations is exemplary.

---

## Object model — tables versus prototypes

The most consequential design choice separating urbi-embedded from the Lua / Luau / Janet family is the object model. Lua and similar languages use **tables with metatables** — a single hash-map data structure plus convention-based inheritance via the `__index` metamethod. urbi-embedded uses **prototypes with shapes** — first-class objects with named slots, multi-prototype inheritance lists, and per-call-site inline caches keyed on hidden classes.

For embedded scripting in general, tables-with-metatables is the better default. The data structure is simpler, the implementation is smaller (~1000 to 1500 lines of C), and the mental model is easier to teach. Lua's success on billions of MCU deployments validates this design choice.

For urbiscript specifically, prototypes-with-shapes is the only design that preserves the language's semantics. Three properties make tables a poor fit:

- **First-class slot properties.** Reactive watchers like `at (motor.position.changed?) body` require the runtime to observe slot reads and writes per-slot. With prototypes plus slot-property `oget` / `oset`, this falls out naturally; with tables, the only options are universal-proxy metatables that destroy performance, external watch dictionaries that break composition, or userdata that loses the table affordance.
- **Multi-prototype inheritance.** Robotics composition often wants a class to inherit from several roles at once (a `Camera` is a `Sensor` and a `NetworkDevice`). Prototypes handle this natively via a list of protos with depth-first MRO; tables require ad-hoc `__index = function(t, k) ... end` handlers without inline caching.
- **Atom uniformity.** urbiscript treats numbers and strings as objects (`1.clone()` returns a new Integer-kind object), enabling uniform message-send dispatch. Lua treats numbers as values and only provides per-type metatables for strings.

For details on the prototype implementation — shape transitions, inline caches, copy-on-write, the topology generation counter — see [internals/architecture.md](../internals/architecture.md). For the runtime `UObject` / `UShape` / `USlot` layout, see the architecture document and the source under `../src/`.

---

## When to pick which

A short decision guide for a developer evaluating their options:

- **Pick urbi-embedded** when language-level reactive watchers (`at`, `whenever`, `waituntil`), statement-separator parallel-by-default concurrency, and tag-scope cancellation match the application's natural shape — typical fits include robotics, control loops, sensor fusion, and event-heavy embedded control.
- **Pick AtomVM** when BEAM ecosystem reuse, preemptive scheduling, or fault-tolerant supervisor restart strategies are firm requirements.
- **Pick Toit** when fleet management, OTA deployment, and a cloud-orchestrated workflow are part of the project requirements.
- **Pick MicroPython or CircuitPython** when Python ecosystem reuse, educational deployment, or the broadest hardware port matrix are decisive.
- **Pick mruby/c or PicoRuby** when Ruby semantics matter or the priority-based real-time scheduler is a firmer fit than urbi-embedded's tag-aware cooperative scheduler.
- **Pick Berry** when the application is event-driven configuration scripting that does not need language-level concurrency.
- **Pick Espruino** when JavaScript familiarity and existing nRF52 / Bangle.js prototyping infrastructure already exist.
- **Pick Lua or Luau** when extreme simplicity, vast ecosystem, and table-as-everything are the right shape — especially for game scripting or host-configuration use cases.
- **Pick Céu** when the application is purely synchronous reactive and a compile-to-C deployment model is acceptable.
- **Pick Forth** when extreme footprint constraints (sub-16 KB) and direct hardware control dominate everything else.

---

## Notes on this document

This document is maintained as facts change. Each fact and footprint number has been verified from a primary source as of April 2026. Where a number is approximate or platform-dependent, that is noted inline.

For peer projects, the canonical curated list is [dbohdan/embedded-scripting-languages](https://github.com/dbohdan/embedded-scripting-languages), which catalogues 175+ embeddable scripting language implementations across all categories.

Suggestions, corrections, and additional peer projects are welcome via issues on the urbi-embedded repository.
