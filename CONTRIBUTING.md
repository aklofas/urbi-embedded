# Contributing to urbi-embedded

## Building

Requires a C99 compiler (GCC or Clang) and GNU Make. No other dependencies.

    make          # build build/host/liburbi.a
    make test     # run the unit test suite

All build artifacts land under `build/<TARGET>/`. The default `TARGET` is `host`.
Override with `make TARGET=<name>` if you need a custom output tree.

## Test modes

    make test-debug    # -O0 -g, for debugging with gdb       → build/host-debug/
    make test-asan     # AddressSanitizer instrumentation     → build/host-asan/
    make test-ubsan    # UndefinedBehaviorSanitizer           → build/host-ubsan/
    make test-valgrind # valgrind memcheck (CI-gating)        → build/host-valgrind/

All four fast variants (`test`, `test-debug`, `test-asan`, `test-ubsan`) must
pass before any commit is merged; each variant has its own build tree, so
they coexist and no `make clean` is required when switching between them.
`test-valgrind` is enforced in CI but runs ~20–50× slower than a plain build
— run it periodically and always before a milestone tag, not on every commit.

## Cross-compile sanity

If you have `arm-none-eabi-gcc` or `riscv64-unknown-elf-gcc` installed:

    make cross-arm     # → build/arm-cortex-m7/liburbi.a
    make cross-riscv   # → build/riscv-rv32imc/liburbi.a

These verify portability; they don't run tests (no target execution environment on the build host).

## Indexing database for LSP editors

Generate a `compile_commands.json` for clangd, CLion, VS Code, or any LSP-based editor:

    make compile_commands.json

The file is gitignored — regenerate it after changing `CFLAGS`/`CPPFLAGS` or adding/removing source files.

## Adding a new test file

1. Create `tests/unit/test_<name>.c` containing a suite function `test_<name>_suite(void)` that calls `utest_run("...", static_fn)` for each test case
2. Add `extern void test_<name>_suite(void);` near the top of `tests/unit/runner.c`
3. Add `test_<name>_suite();` inside `main()` in `runner.c`
4. `make test` — verify the new cases appear in the output

## Coding style

See `docs/STYLE.md` for the full style guide — naming, memory model, const-correctness, initialization, error handling, headers, tests, and comment conventions. Mechanical rules are enforced by `.editorconfig`, `.clang-tidy`, and the Makefile's warning flags.

## Per-file LOC-cap exceptions

The default soft cap is 1000 LOC per `.c` source file (enforced by
`make test-loc-cap`).  Files listed below are exempted with rationale.

- `loc-cap-exception:src/vm/uvm.c` — opcode dispatch loop. The body of
  `dispatch_loop_until_yield` (computed-goto dispatch + ~47 inline opcode
  handlers + safepoint / exit-strand / halt-error labels) is intentionally
  inlined in a single TU for cache locality of the dispatch table and to
  let the compiler keep the entire instruction-stream state machine in
  registers across opcodes.  Decomposing into per-opcode helpers would
  defeat the threading optimization that gives the VM ~10x dispatch-loop
  throughput on hosted builds and ~3x on Cortex-M7.  This exception is
  permitted by `docs/superpowers/specs/2026-05-05-v0.5.x-cleanup-design.md`
  §3.3 ("generated dispatch tables, opcode trampolines").

## License

BSD-3-Clause.
