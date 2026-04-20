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

All four must pass before any commit is merged. Each variant has its own build
tree, so they coexist and no `make clean` is required when switching between them.

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

Terse and self-documenting. One-line purpose comment at the top of each file. SPDX license identifier on line one. Follow the style of existing `src/*.c` files.

## License

BSD-3-Clause.
