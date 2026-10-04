# Test harness

Five things test this codebase, and which one a new test belongs in is
usually obvious from what it is asking:

| Runner | Asks | Lives in |
|---|---|---|
| `make test-unit` | Does the compiler frontend produce the right bytecode? | `tests/unit/` |
| `make test-rt` | Does a runtime subsystem hold its own invariants? | `tests/rt/` |
| `make test-chk` | Does the language behave the way the corpus says? | `tests/chk/` |
| `make test-probes` | Is it still small enough? | `tests/probes/` |
| `make test-bench` | Is it still fast enough? Run alone. | `tests/probes/` |

`make test` runs the first four plus the layering gate; `test-bench` is
separate, for the reason below. Every sanitizer variant
(`test-asan`, `test-ubsan`, `test-gc-stress`, `test-valgrind`,
`test-debug`, `test-switch`, `test-o2`) is the same aggregate rebuilt
with different flags, so a new test is picked up by all of them for free.

A behaviour question goes in the corpus, not in C. The two C runners are
for things a script cannot see: an emitted opcode sequence, a collector
invariant, a byte count.

## The frontend runner — `tests/unit/`

Header-only harness in `tests/unit/utest.h`: `UASSERT`, `UASSERT_EQ`,
`UASSERT_NE`, `UASSERT_STR_EQ`, and `utest_run` to name a case. One file
per subject, one `<basename>_suite(void)` per file, and
`tests/unit/runner.c` declares and calls them in file order.

```c
/* SPDX-License-Identifier: BSD-3-Clause */
#include "utest.h"
#include "lex/ulex.h"

#define UTEST(name) static void name(void)

UTEST(lexes_a_plus) {
    ULexer l;
    ulex_init(&l, "+", 1);
    UASSERT_EQ((int)ulex_next(&l).type, (int)TOK_PLUS);
}

void test_lex_plus_suite(void) {
    utest_run("lexes_a_plus", lexes_a_plus);
}
```

Adding a file means adding the `extern` and the call in `runner.c`; the
Makefile picks the source up by wildcard.

Suites that need a VM take one only because the emitter stores it for its
string table — identifiers and string literals intern into the same table
the runtime will later look them up in. `urbi_open` refuses a NULL
allocator, so `utest.h` provides `utest_alloc`, a plain realloc adaptor:

```c
UVM *vm = urbi_open(utest_alloc, NULL, NULL);
...
urbi_close(vm);
```

Nothing here starts a strand or collects a heap. A suite that wanted to
belongs in `tests/rt/` or the corpus.

**Frontend tests go through the public entry point.** `tests/unit/test_emit_bytecode.c`
pins the emitter's output two ways: the bytecode shape of a construct
(compiled through `urbi_compile`, loaded back with the real loader, and
disassembled with `uemit_disassemble`) and what a program evaluates to (run
through `urbi_run`). Every case compiles through the public API, never by
driving a `UEmitter` directly — `src/emit/uemit.h` is private to `src/emit/`,
and a test that called `uemit_new`/`uemit_statement` itself would be pinning
the driver's internals rather than what the frontend promises a caller. The
one exception is the diagnostic-buffer ordering case, which has no public
surface to observe through (nothing in `urbi.h` exposes warn/error diagnostic
order) and so drives `UEmitter` directly as a narrow, documented carve-out.

## The runtime runner — `tests/rt/`

Same shape, different harness (`tests/rt/rtest.h`: `RT_CHECK`, `RT_EQ`,
`rt_run`) and one suite per `src/rt/` subsystem: value, gc, str, obj,
list, strand, exec, sched, watch, unwind, api, realm, leaks.

These reach into the core's own headers, which is the point: they pin
invariants an embedder cannot observe — that a collection preserves
exactly the reachable set, that a parked strand is off the run queue,
that a hundred sleepers cost what they are supposed to.
`tests/rt/fakevm.h` provides a minimal VM for the suites below the exec
layer.

## The corpus — `tests/chk/`

The language specification, as executable fixtures. One directory per
topic, one `.chk` file per behaviour, and each file is a session:

```text
# Milestone: <release classifier>
# Covers: <what this pins, and why it is not obvious>
1 + 2
[00000000] 3
```

Lines that are not comments are input; lines beginning `[00000000]` are
expected output. `tests/integration/run_chk.sh` runs one file and
`chk_summary.sh` runs the corpus and reports per directory.

Three runner modes:

- the default drives the `urbi` REPL binary and diffs normalized output;
- `## mode: file` runs the body through `urbi -f`;
- `## mode: repl` hands the file to `repl-chk-driver`, for the NDJSON
  eval-service fixtures, whose expectations are substring sets rather
  than exact text and whose verdict is the driver's exit status.

A fixture carrying `## host:` directives needs more than one realm or a
clock it controls, and runs through `chk-host-driver` instead.

`CHK_GATE_DIRS` decides which directories are allowed to fail the build.
The whole corpus always runs and is always reported, so the tally shows
coverage rather than hiding it. A fixture that records a behaviour
nothing implements yet carries a PLACEHOLDER annotation naming what
blocks it; those are counted separately and never silently pass.

Normalization strips the session-id prefix and trailing whitespace, so a
fixture pins content, not the REPL's framing. Timing-dependent output is
the one thing the format cannot express: a fixture that needs a clock
uses the host driver.

## The probes — `tests/probes/`

Not tests. Each one measures a number the project committed to, prints
it, and exits non-zero when it moved the wrong way: the boot heap, the
cost of an idle strand, whether a ten-thousand-iteration loop gives its
memory back, whether a heap budget holds a VM inside its cap
(`small_heap`), and how the runtime compares to the one it replaced.
`tests/probes/probe.h` has the shared counting allocator and the
rationale; `tests/probes/baseline-timings.md` holds the old core's
numbers.

`make test-probes-32bit` rebuilds the probes for a Cortex-M4 preset and
runs them under qemu's MPS2-AN386 model, including `small_heap` at a 64
KB budget; it needs `arm-none-eabi-gcc` and `qemu-system-arm`, so it
runs in CI only, not in `make test` or `make releasetest`.

The timing probe is `make test-bench`, deliberately NOT part of `make
test`: `make test` is itself one gate of releasetest's parallel sweep,
and a wall-clock number taken while the other gates' compiles saturate
the machine is the machine's number, not the interpreter's — the same
probe read 4.99x under `-j32` and 1.46x solo. It is not part of `make
releasetest` either: the baseline is wall-clock seconds recorded on one
machine, so anywhere else (a CI runner) the ratio compares two machines.
Run it by hand, alone, on the machine that recorded the baseline. It
also runs only on the default host build: comparing an instrumented binary against
an uninstrumented baseline would measure the instrumentation.

## Running

| Target | Flags | Build dir | Use |
|---|---|---|---|
| `make test` | `-Os` | `build/host/` | the default loop |
| `make test-debug` | `-O0 -g -DURBI_DEBUG=1` | `build/host-debug/` | before reaching for a debugger |
| `make test-asan` | `-O1 -g -fsanitize=address` | `build/host-asan/` | use-after-free, leaks |
| `make test-ubsan` | `-O1 -g -fsanitize=undefined` | `build/host-ubsan/` | signed overflow, bad shifts, misaligned loads |
| `make test-gc-stress` | `-O1 -g -DURBI_GC_STRESS=1` | `build/host-gc-stress/` | rooting gaps — collects before every allocation |
| `make test-valgrind` | `-O1 -g` under memcheck | `build/host-valgrind/` | uninitialized reads ASan cannot see |
| `make test-switch` | `-Os -DURBI_VM_FORCE_SWITCH=1` | `build/host-switch/` | keeps the portable dispatch path honest |
| `make test-cache-verify` | `-O1 -g -DURBI_SLOT_CACHE_VERIFY=1` | `build/host-cache-verify/` | a slot-cache hit that disagrees with the uncached lookup |
| `make test-o2` | `-O2 -g` | `build/host-o2/` | the level desktop embedders actually use |
| `make test-bench` | `-Os` | `build/host/` | the timing probe, alone, on the baseline machine |
| `make releasetest` | all of the above except `test-bench`, plus `cross-all` and `test-cross-missing-toolchain`: 24 gates in parallel, then 2 alone | — | before a tag |

Build directories are disjoint, so the parallel sweep does not race.

## Debugging a failure

Rebuild at `-O0 -g` and run the one runner under a debugger:

```sh
make TARGET=host-debug CFLAGS="-std=c99 -Wall -Wextra -Wpedantic -O0 -g -DURBI_DEBUG=1" test-unit
gdb build/host-debug/tests/unit/runner
```

For a corpus failure, `tests/integration/run_chk.sh build/host/urbi
tests/chk/<dir>/<file>.chk` prints the diff for that one fixture.
`build/host/urbi --dump-bytecode` disassembles what the emitter produced,
which is usually the faster answer for anything shaped like "the wrong
thing ran".

`URBI_GC_STRESS=1` is worth reaching for early on any crash that moves
when unrelated code changes: it turns an intermittent rooting gap into a
deterministic one.

## Coverage and static analysis

`make coverage` instruments, runs the aggregate, and prints a gcovr
summary with an HTML report at `build/host-coverage/report.html`. The
floor is a hard gate; raise it, never lower it.

`make lint` runs clang-tidy (gating), cppcheck and GCC `-fanalyzer`
(advisory). `make test-cppcheck`, `make test-tidy-strict` and `make
test-scan-build` are the strict gating variants that `releasetest` runs.

`make fuzz-lex`, `fuzz-parse`, `fuzz-vm` and `fuzz-chunk` build libFuzzer
harnesses (clang only). `make test-fuzz-smoke` runs each for 20,000
bounded iterations as part of `releasetest`; the unbounded targets are
for a local session with a time budget.
