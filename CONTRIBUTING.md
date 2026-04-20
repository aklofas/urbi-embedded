# Contributing to urbi-embedded

## Building

Requires a C99 compiler (GCC or Clang) and GNU Make. No other dependencies.

    make          # build liburbi.a
    make test     # run the unit test suite

## Test modes

    make test-debug    # -O0 -g, for debugging with gdb
    make test-asan     # AddressSanitizer instrumentation
    make test-ubsan    # UndefinedBehaviorSanitizer instrumentation

All four must pass before any commit is merged.

## Cross-compile sanity

If you have `arm-none-eabi-gcc` or `riscv64-unknown-elf-gcc` installed:

    make cross-arm     # build liburbi.a for ARM Cortex-M7
    make cross-riscv   # build liburbi.a for RISC-V rv32imc

These verify portability; they don't run tests (no target execution environment on the build host).

## Adding a new test file

1. Create `tests/unit/test_<name>.c` containing a suite function `test_<name>_suite(void)` that calls `utest_run("...", static_fn)` for each test case
2. Add `extern void test_<name>_suite(void);` near the top of `tests/unit/runner.c`
3. Add `test_<name>_suite();` inside `main()` in `runner.c`
4. `make test` — verify the new cases appear in the output

## Coding style

Terse and self-documenting. One-line purpose comment at the top of each file. SPDX license identifier on line one. Follow the style of existing `src/*.c` files.

## License

BSD-3-Clause.
