# Test Harness

## Overview

`utest.h` is a header-only test harness with zero external dependencies, written
in pure C99. It works equally well at release, debug, and sanitizer build
configurations, and its output is plain text — no XML, no JSON, no special
runner protocol needed. Every subsystem under `src/` has a sibling
`tests/unit/test_<subsys>.c` file that groups that subsystem's test cases into a
suite function. `tests/unit/runner.c` provides the `main()` entrypoint and calls
each suite function in sequence. The full suite runs in under five seconds on
typical development hardware, making it practical to run before every commit.

## Anatomy of a test suite

Each test case is a small `static void` function. The `UTEST(case_name)` macro
(defined at the top of each test file) expands to exactly that — `static void
case_name(void)` — so there is no magic registration, no linker trick, and no
run-time reflection. You define cases like ordinary functions and then explicitly
pass each one to `utest_run` inside the suite function. Every assertion in the
body increments the global check counter; a failure increments the failure
counter and prints a `FAIL:` line with the file, line number, and expression
that did not hold.

Three assertion macros cover most needs:

- `UASSERT(cond)` — fails if `cond` is false.
- `UASSERT_EQ(a, b)` — casts both sides to `long long` and compares; on failure
  it prints both the expected and actual values.
- `UASSERT_STR_EQ(a, b)` — calls `strcmp` and prints the differing strings on
  failure.

Here is a real excerpt from `tests/unit/test_varint.c` that shows two cases and
the corresponding portion of the suite function:

```c
#define UTEST(name) static void name(void)

UTEST(destroy_empty_module_is_noop) {
    UModule c = {0};
    umodule_destroy(&c);
    UASSERT_EQ((void *)NULL, (void *)c.instructions);
    UASSERT_EQ((void *)NULL, (void *)c.constants);
    UASSERT_EQ((size_t)0, c.instr_count);
    UASSERT_EQ((size_t)0, c.const_count);
}

UTEST(decode_u_single_byte) {
    const uint8_t buf[] = {0x00};
    uint64_t v = 0;
    size_t consumed = 0;
    UASSERT_EQ(UVARINT_OK, uvarint_decode_u(buf, sizeof buf, &v, &consumed));
    UASSERT_EQ((uint64_t)0, v);
    UASSERT_EQ((size_t)1, consumed);

    const uint8_t buf2[] = {0x7F};   /* 127 — max single-byte */
    UASSERT_EQ(UVARINT_OK, uvarint_decode_u(buf2, sizeof buf2, &v, &consumed));
    UASSERT_EQ((uint64_t)127, v);
    UASSERT_EQ((size_t)1, consumed);
}

void test_varint_suite(void) {
    utest_run("varint decode u single byte",     decode_u_single_byte);
    /* ... more cases ... */
}
```

The string passed to `utest_run` is a human-readable description that appears in
the output line next to `PASS` or `FAIL`. Choose descriptions that read like
sentences so a failing run is readable without opening the source file. The
convention in this project is all-lowercase with spaces, matching the case
function name but substituting underscores for spaces.

`runner.c` declares every suite function `extern` at the top of the file and
calls each one from `main()`. The return value of `main()` is 0 if all cases
passed and 1 otherwise, which is what `make test` checks.

## Adding a new test file

The four-step recipe mirrors what `CONTRIBUTING.md` describes; these notes add
the concrete details.

**Step 1 — Create `tests/unit/test_<name>.c`.**

Include `utest.h` and define the `UTEST` convenience macro at the top of the
file (each file defines its own copy so the macro is always in scope without
polluting other translation units). Write each test case with `UTEST(case_name)
{ ... }`. At the bottom of the file, define the suite function:

```c
/* SPDX-License-Identifier: BSD-3-Clause */

#include "utest.h"
#include "foo.h"         /* the subsystem under test */

#define UTEST(name) static void name(void)

UTEST(foo_returns_zero_on_empty_input) {
    UASSERT_EQ(0, foo_process(NULL, 0));
}

UTEST(foo_handles_single_byte) {
    const uint8_t in[] = {0x42};
    UASSERT_EQ(1, foo_process(in, sizeof in));
}

void test_foo_suite(void) {
    utest_run("foo returns zero on empty input",  foo_returns_zero_on_empty_input);
    utest_run("foo handles single byte",          foo_handles_single_byte);
}
```

**Step 2 — Declare the suite function in `runner.c`.**

Near the top of `tests/unit/runner.c`, alongside the other `extern` declarations,
add:

```c
extern void test_foo_suite(void);
```

**Step 3 — Call the suite from `main()`.**

Inside `main()` in `runner.c`, add a call next to the existing suite calls.
Order affects only the order of output lines; conventionally it matches the
order of the `extern` declarations at the top of the file:

```c
test_foo_suite();
/* Add new suites here as test files are added. */
```

**Step 4 — Verify.**

```sh
make test
```

The new cases should appear in the output with `PASS` lines. Zero should fail.
If the file does not compile, the error comes from the link step that builds
`build/host/tests/unit/runner`; the compiler message will identify the file and
line.

The Makefile discovers test sources automatically via `$(wildcard
tests/unit/test_*.c)`, so creating the file is enough to get it compiled. The
only manual edits required are the `extern` declaration and the `main()` call in
`runner.c`.

## Running tests

Each build variant uses its own subdirectory under `build/`, so they coexist
without requiring a `make clean` when switching between them.

| Target          | CFLAGS                          | Build dir            | When to run                                      |
| --------------- | ------------------------------- | -------------------- | ------------------------------------------------ |
| `make test`     | `-Os` release                   | `build/host/`        | Always; under 5 s; every commit                  |
| `make test-debug` | `-O0 -g`                      | `build/host-debug/`  | Routinely; prerequisite for gdb sessions         |
| `make test-asan` | AddressSanitizer (`-O1 -g`)   | `build/host-asan/`   | After any change to `src/*.c`                    |
| `make test-ubsan` | UndefinedBehaviorSanitizer (`-O1 -g`) | `build/host-ubsan/` | After any change to `src/*.c`          |

All four must pass before any commit lands. Running `make test` alone is
sufficient for documentation-only or tooling changes; after touching
implementation code, run all four.

Cross-compile variants build `liburbi.a` for Cortex-M7 (`make cross-arm`) and
RISC-V rv32imc (`make cross-riscv`) but do not execute tests — there is no
target execution environment on the build host. They verify that `src/*.c`
compiles cleanly with `-ffreestanding` for each architecture, which is enough to
catch host-only assumptions.

## Coverage expectations

The project targets at least 90% line coverage and at least 80% branch coverage
at v1.0 (see `../ROADMAP.md`, "Quality bars at v1.0"). Coverage is measured with
`gcov` against the debug build. There is no automated CI gate on coverage at the
current development stage, but the expectation is that every new `src/*.c` module
ships with a corresponding `tests/unit/test_<module>.c` that exercises it to at
least 90% line coverage before the module is merged. The subsystems shipped to
date — lexer, parser, arena, module, and emitter — all exceed 96% line coverage,
so the bar is achievable with normal TDD discipline: write a failing test, make
it pass, verify the uncovered lines, and add cases until the gap closes.

## Debugging a failing test

1. **Read the failure line.** `UASSERT` and `UASSERT_EQ` both print
   `FAIL: file:line: expression`. The expression text is stringified at compile
   time, so you can usually find the failing check without opening a debugger.

2. **Isolate by suite.** Temporarily comment out the other `test_*_suite()`
   calls in `runner.c`, rebuild with `make test`, and re-run. The output is much
   shorter and easier to read. Restore the calls before committing.
   (A per-case filter is not currently built into the harness; if you find
   yourself needing one repeatedly, it is a candidate for a small harness
   enhancement.)

3. **Open the runner in gdb.** Rebuild with `make test-debug`, then:

   ```sh
   gdb build/host-debug/tests/unit/runner
   (gdb) run
   (gdb) bt
   ```

   The debug build includes full DWARF info and no inlining, so backtraces map
   cleanly to source lines.

4. **For memory bugs: `make test-asan`.** AddressSanitizer catches
   use-after-free, buffer overflows, stack overflows, and heap leaks at
   program exit. The sanitizer report identifies the exact allocation and
   access sites.

5. **For undefined behaviour: `make test-ubsan`.** UBSan catches signed
   integer overflow, misaligned pointer dereferences, invalid enum values,
   out-of-bounds array indexing, and shift-count violations. These are
   particularly important in the bytecode and varint code paths where
   bit manipulation is heavy.

6. **Add a regression test before claiming the fix.** Write a test case that
   reproduces the failure, verify it fails before the fix, apply the fix, and
   verify it passes. Then run the full battery before committing:

   ```sh
   make test && make test-debug && make test-asan && make test-ubsan
   ```

   A fix that is not covered by a regression test is likely to resurface.

## Correctness tooling beyond the unit suite

The unit suite is the primary feedback loop. Three further targets add
complementary correctness signal; each one lands under its own
`build/<target>/` directory and can coexist with any other build variant.

### Coverage — `make coverage`

Builds the test runner with GCC's `--coverage` instrumentation
(`-fprofile-arcs -ftest-coverage`) at `-O0 -g`, runs the full suite, and
then invokes `gcovr` to produce:

- A per-file summary on stdout (lines executed / total, percent).
- An HTML report tree at `build/host-coverage/report.html` plus
  per-source-file detail pages.

Report filter is `--filter 'src/'` — tests themselves are excluded from
coverage counts. Requires `gcovr` in `PATH`; install via
`sudo apt-get install -y gcovr` or `pip install --user gcovr`.

CI runs this as an **advisory** job — it won't fail the pipeline even on
tool crashes. Will promote to gating (with `--fail-under-line 90` line
floor) in a follow-up commit once 2–3 CI runs have established the noise
floor. The HTML report is uploaded as a CI artifact (`coverage-report`)
with a 14-day retention.

Repeated local runs: the target clears stale `.gcda` files at start so
successive runs produce clean counts.

### Valgrind memcheck — `make test-valgrind`

Builds the test runner at `-O0 -g` (no sanitizers) and runs it under
`valgrind --tool=memcheck` with `--error-exitcode=1`,
`--leak-check=full`, `--track-origins=yes`, `--show-leak-kinds=all`,
and `-q` (quiet — suppresses per-error output so the test runner's
own output stays readable). Any memcheck finding fails the build.

Complements — does not replace — the ASan and UBSan variants
(`make test-asan`, `make test-ubsan`). Memcheck's bit-precise uninit
tracking catches reads of uninitialized memory that ASan misses.
Memcheck is slower (~20–50× real-time overhead) but deterministic.

CI runs this as a **gating** job. Failure indicates a real bug; do not
suppress with `continue-on-error`.

### Fuzz testing — `make fuzz-lex`, `make fuzz-parse`

Clang + libFuzzer harnesses over the lexer and parser. Each harness
feeds raw bytes into the corresponding compiler-internal API
(`ulex_next`, `uparse_next_statement`) and asserts only "no crash".
Target property: both components produce a finite, non-crashing stream
of Tokens or AstNodes (including structured error values) for any byte
sequence.

Build + run:

```sh
make fuzz-build                    # builds both fuzzers
./build/host-fuzz/fuzz_lex   -runs=100000    # ~10s smoke budget
./build/host-fuzz/fuzz_parse -runs=100000
```

libFuzzer writes a finding file named `crash-<hash>`, `leak-<hash>`,
`timeout-<hash>`, or `oom-<hash>` in the current directory; reproduce with
`./build/host-fuzz/fuzz_lex crash-<hash>`. The `.gitignore` excludes
crash / leak / timeout / oom artifacts, but do commit them to a local
tracking branch if you want to preserve the reproducer while
investigating.

Local-only: CI has no scheduled fuzz job yet. Requires clang and
`libclang-rt-18-dev` (for the libFuzzer + ASan runtime archives);
install with
`sudo apt-get install -y clang libclang-rt-18-dev`.

Seed corpus: the `tests/fuzz/corpus/` directory is gitignored and
optional. You can supply existing `.chk` fragments, existing test
inputs, or any other representative bytes as seeds — libFuzzer handles
coverage-guided exploration from there. Corpus seeding can cut
time-to-first-finding significantly on a cold start; it's not required.

Not covered: `fuzz-emit`. The emitter takes structured AST input, not
byte streams; a typed fuzzer (random-AST generator → emit → deserialize
round-trip) is a separate design.

## Conformance fixtures — `.chk` files and `make test-chk`

`.chk` fixtures are on-disk transcripts of REPL dialogs. Each fixture
records a sequence of urbiscript inputs paired with the exact REPL
output each one should produce. The `tests/integration/run_chk.sh`
runner pipes the inputs through `urbi -i`, normalizes away the
varying `[########]` timestamp prefix on both sides, and diffs. Pass
means byte-equal after normalization.

The fixture corpus is how we prove language-level behavior remains
stable as the implementation grows. Unit tests exercise compiler and
VM internals in isolation; `.chk` fixtures exercise the whole
pipeline from source text through to REPL framing, from the outside.

### Fixture format

Three line classes per `.chk` file:

- **Input line** — any line not starting with `[` and not a fixture
  comment (§below). Fed verbatim as one line into `urbi -i` stdin.
- **Expected-output line** — a line matching `^\[[^]]*\]` followed by
  a space, appearing immediately after its paired input line. The
  bracketed prefix is author-convention only; the runner strips it
  before diffing, so any content inside the brackets is acceptable.
  Legacy convention is an 8-digit counter (`[00000001]`, `[00000002]`, …).
- **Fixture comment** — a line whose first non-space character is `#`,
  or a blank line. Never forwarded to the REPL.

`#` is the fixture-comment marker rather than `//` because `//` is
valid urbiscript — the runner must not couple to the urbiscript lexer
to decide whether a `//` line is intended as REPL input (it is) or
as fixture-level commentary (it is not). `#` is unambiguous at every
milestone of urbiscript.

### Normalization rule

Exactly one rule: strip the `^\[[^]]*\]` prefix (including the
following space) from every line on both sides before diffing. File
paths, line and column numbers, trailing whitespace, and everything
else is compared literally. This is deliberately strict — `sed
's/ *$//'` would mask regressions.

At M2+, when legacy fixtures are ported, the rule may extend to
cover span-notation normalization (`input.u:@.L-C:` → `<stdin>:L:C:`
or similar). That extension lands when a concrete port forces it,
not preemptively.

### Running

One fixture at a time:

```sh
tests/integration/run_chk.sh build/host/urbi tests/chk/arithmetic.chk
```

All fixtures via Make:

```sh
make test-chk
```

`make test-chk` is a dependency of `make test`, so every sanitizer
variant (`test`, `test-asan`, `test-ubsan`, `test-debug`,
`test-switch`) runs the fixture corpus automatically. `make
test-valgrind` does NOT run the fixtures — valgrind-wrapping a shell
pipeline produces false-positives from dash internals rather than
signal about the runtime. ASan coverage under `make test-asan`
provides the memcheck story for the integration path.

### Authoring a new fixture

1. Run the expressions you care about against `urbi -i` interactively
   and capture the exact framed output.
2. Interleave the inputs and captured outputs in a new file under
   `tests/chk/`, one pair per REPL exchange.
3. Add `# --- <section name>` comment markers for readability.
4. Run `tests/integration/run_chk.sh build/host/urbi
   tests/chk/<name>.chk` and confirm `PASS:`. If it fails, diff the
   expected output against what the binary actually produces — the
   fixture records what the implementation does, not what you wish
   it did.
5. Commit with the `chk:` subsystem prefix.
