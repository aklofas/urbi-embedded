# Getting Started

## What is urbiscript?

urbiscript is a prototype-based, parallel-by-default, event-driven scripting
language designed for robotics. Its defining characteristic is that statement
separators carry concurrency meaning: `;` sequences with a yield point, `|`
sequences without one, `,` fires the right-hand side in a new parallel strand,
and `&` launches a parallel strand and joins before continuing. Beyond
concurrency, the language has first-class reactive primitives — `at (cond)
body` installs a persistent watcher that fires whenever the condition becomes
true, `whenever` re-fires on every true interval, `every(100ms)` fires on a
timer, and `waituntil` blocks the current strand until a condition holds. Tags
provide structured cancellation: `mytag: every(100ms) sense.read() |` starts a
background sensing loop; `mytag.stop()` cancels it cleanly. Time and angle
literals (`100ms`, `1s`, `180deg`) are part of the lexical grammar, not library
calls. The combination is essentially unique among embedded-systems languages.

urbi-embedded is a modern C99 implementation of urbiscript targeting embedded
hardware — Cortex-M7 (STM32H7) and RISC-V 32-bit (ESP32-C3) — as well as
Linux, with Lua-grade embeddability as the design goal: a single static library,
no external dependencies, pluggable allocator and time source, and a public C
API of under 80 functions. See [../ROADMAP.md](../ROADMAP.md) for the full
feature sequence to `v1.0.0`.

---

## Prerequisites

- A C99 compiler: GCC or Clang.
- GNU make.
- Optionally, `arm-none-eabi-gcc` for the ARM Cortex-M7 cross-compile target.
- Optionally, `riscv64-unknown-elf-gcc` for the RISC-V rv32imc cross-compile
  target.

No other dependencies. No CMake, autotools, Python, Ruby, or build bootstrap.

---

## Clone and build

```sh
git clone https://github.com/aklofas/urbi-embedded.git
cd urbi-embedded
make
```

This produces `build/host/liburbi.a`, the static library, compiled for the host
architecture.

---

## Run the test suite

```sh
make test
```

Expected output: 222 cases, 732 checks, 0 failed. The suite completes in under
five seconds on typical development hardware.

Three additional build modes run the same suite with extra instrumentation:

- `make test-asan` — AddressSanitizer: catches heap overflows, use-after-free,
  and invalid frees.
- `make test-ubsan` — UndefinedBehaviorSanitizer: catches signed overflow,
  misaligned access, and related issues.
- `make test-debug` — debug build without sanitizers: enables assertions and
  keeps symbols for GDB.

All three modes must pass before a change lands on `main`. See
[../internals/test-harness.md](../internals/test-harness.md) for the full
harness documentation, including how to add tests and read coverage reports.

---

## What is in the walking skeleton today

The lexer, parser, bump-allocator arena, bytecode emitter, and `.urb` module
loader are complete and tested. The lexer tokenizes the full urbiscript token
set including time and angle literals, multi-radix integers, and identifiers.
The parser builds an AST using the arena allocator, with Pratt precedence
climbing for arithmetic and panic-mode recovery at statement boundaries. The
emitter compiles AST nodes down to an 8-opcode bytecode and serializes them to
the `.urb` on-disk format with a header, constant pool, instructions, and debug
synclines. The module loader deserializes and verifies `.urb` files.

The VM — which executes the bytecode — and the interactive `urbi` REPL are the
final pieces of the `v0.1.0-skeleton` release and arrive together. Until that
tag cuts, the test suite is the executable proof of the pipeline. See
[../internals/architecture.md](../internals/architecture.md) for the full
pipeline shape, [../internals/opcodes.md](../internals/opcodes.md) for the
instruction set, and
[../internals/bytecode-format.md](../internals/bytecode-format.md) for the
`.urb` on-disk format.

---

## Your first urbiscript program

The walking-skeleton goal is a REPL that accepts `1 + 2 |` and prints `3`. Once
`v0.1.0-skeleton` is tagged, that works. Until then, the test suite exercises
the same arithmetic path end-to-end — the emitter tests compile `1 + 2` and
verify the bytecode; what is missing is the final execution step.

The following shows what the REPL session is targeted to look like at
`v0.1.0-skeleton`. This is the target experience, not the current one:

```text
$ urbi -i
[00000000] 1 + 2 |
[00000001] 3
[00000002] ^D
$
```

What each element means:

- `[NNNNNNNN]` is a sequential prompt counter. Input prompts and output lines
  share the same counter so the session history is unambiguous.
- `|` is the statement terminator for the sequential-atomic separator. The other
  separators — `;`, `,`, `&` — encode different concurrency semantics and are
  covered in the language tour, which arrives at `v1.0.0`.
- The REPL prints the value of the last non-nil expression in the statement.

At the current tip of `main`, running `urbi -i` is not yet possible. See
[../ROADMAP.md](../ROADMAP.md) for when the `v0.1.0-skeleton` tag cuts and what
the `v0.2.0-expressions` release adds on top.

---

## Cross-compiling for embedded targets

```sh
make cross-arm    # produces build/arm-cortex-m7/liburbi.a   (Cortex-M7)
make cross-riscv  # produces build/riscv-rv32imc/liburbi.a   (ESP32-C3)
```

The host has no execution environment for those architectures, so there is no
test runner at the cross targets — the cross-compile steps verify that the
sources are free of host-specific assumptions and that the library links cleanly.
Integrating the library into a real embedded project requires host code that
instantiates the runtime and wires up the allocator and time-source hooks; that
is covered in `embedding/guide.md`, which is planned for `v0.6.0-embedded`. For
now, the cross targets confirm portability of `src/`.

---

## Next steps

- [../internals/architecture.md](../internals/architecture.md) — how the lexer,
  parser, arena, emitter, module loader, and VM fit together.
- [../internals/test-harness.md](../internals/test-harness.md) — how to run
  tests, add new test cases, and read coverage reports.
- [../ROADMAP.md](../ROADMAP.md) — what each release delivers and the quality
  bars it must clear.
- [../../CONTRIBUTING.md](../../CONTRIBUTING.md) — how to contribute tests, bug
  reports, and code.
- `language/reference.md` — planned for `v0.2.0-expressions`.
- `language/tour.md` — planned for `v1.0.0`.
