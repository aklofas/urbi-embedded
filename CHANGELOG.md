# Changelog

## Unreleased

### Foundation (M0 + M1 Phase 1)

- Oracle verification complete — see REVIVAL.md §1 for result
- Header-only test harness `utest.h` (zero dependencies)
- Make targets: `test`, `test-asan`, `test-ubsan`, `test-debug`, `cross-arm`, `cross-riscv`
- GitHub Actions CI covering host (debug/release/ASan/UBSan) plus ARM Cortex-M7 and RISC-V rv32imc cross-compiles
- Initial placeholder API: `urbi_version()`
