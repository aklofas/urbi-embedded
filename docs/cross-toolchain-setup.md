# Cross-toolchain setup for local CI parity

`make releasetest` runs every host-side CI gate locally. When any of
the bare-metal cross toolchains are also installed, releasetest detects
them at startup and runs the corresponding cross-build + freestanding
symbol check in a sequential Phase 0 before the parallel Phase 1
sweep. Without these toolchains, releasetest is still useful — it just
defers cross verification to GitHub Actions CI.

This doc documents the recommended install path for the three cross
toolchains the project uses.

## The three toolchains

| Toolchain | Used by | xpack package |
|---|---|---|
| `arm-none-eabi-gcc` | `cross-arm` (Cortex-M7) + `cross-stm32f4` (STM32F429 Cortex-M4F) | [`xpack-dev-tools/arm-none-eabi-gcc-xpack`](https://github.com/xpack-dev-tools/arm-none-eabi-gcc-xpack) |
| `riscv-none-elf-gcc` | `cross-riscv` (rv32imc) | [`xpack-dev-tools/riscv-none-elf-gcc-xpack v15.2.0-1`](https://github.com/xpack-dev-tools/riscv-none-elf-gcc-xpack) |
| `xtensa-esp-elf-gcc` | `cross-esp32s3-bytecode-only` + `test-cross-esp32s3-freestanding-golden` | ESP-IDF v6.0.1 (bundles the Xtensa toolchain), or [`xpack-dev-tools/xtensa-esp-elf-gcc-xpack`](https://github.com/xpack-dev-tools/xtensa-esp-elf-gcc-xpack) |

## Why xpack

Distro packages (apt's `gcc-arm-none-eabi`, `gcc-riscv64-unknown-elf`)
ship the compiler binary without a working bare-metal sysroot — the
very first `#include <string.h>` in any TU fails because no newlib
headers are present. xpack distributions bundle the compiler with a
matching newlib build and the libgcc soft-float helpers needed for
freestanding builds. GitHub Actions CI uses xpack for RISC-V as of
commit `a57b1cf` (post-v0.9.2 hotfix).

## Install (Linux x86_64 example)

```sh
# arm-none-eabi
curl -L -o /tmp/arm.tar.gz \
    https://github.com/xpack-dev-tools/arm-none-eabi-gcc-xpack/releases/download/v14.2.1-1.1/xpack-arm-none-eabi-gcc-14.2.1-1.1-linux-x64.tar.gz
mkdir -p $HOME/.local/xpack && tar -xzf /tmp/arm.tar.gz -C $HOME/.local/xpack
export PATH=$HOME/.local/xpack/xpack-arm-none-eabi-gcc-14.2.1-1.1/bin:$PATH

# riscv-none-elf (xpack ships this name; Makefile uses riscv64-unknown-elf —
# symlink to bridge)
curl -L -o /tmp/riscv.tar.gz \
    https://github.com/xpack-dev-tools/riscv-none-elf-gcc-xpack/releases/download/v15.2.0-1/xpack-riscv-none-elf-gcc-15.2.0-1-linux-x64.tar.gz
tar -xzf /tmp/riscv.tar.gz -C $HOME/.local/xpack
export PATH=$HOME/.local/xpack/xpack-riscv-none-elf-gcc-15.2.0-1/bin:$PATH
for f in $HOME/.local/xpack/xpack-riscv-none-elf-gcc-15.2.0-1/bin/riscv-none-elf-*; do
    ln -sf "$f" "$HOME/.local/bin/riscv64-unknown-elf-$(basename "$f" | sed 's/^riscv-none-elf-//')"
done

# xtensa-esp-elf — either install ESP-IDF (recommended; brings in idf.py and
# the QEMU emulator alongside the toolchain) or grab the standalone xpack:
# https://github.com/espressif/esp-idf  (run install.sh, source export.sh)
```

Persist the `PATH` additions in your shell profile. Then verify:

```sh
cd urbi-embedded
arm-none-eabi-gcc --version
riscv-none-elf-gcc --version
xtensa-esp-elf-gcc --version
make test-freestanding   # all three archives built + checked end-to-end
make releasetest         # detection banner shows all three "present"; Phase 0 runs
```

## When toolchains drift

If a future GHA CI failure is cross-only (passes locally without
toolchains, fails in CI on a cross gate), the most common causes are
new libgcc soft-float helper symbols pulled in by recent runtime code
(the `cross-esp32s3-freestanding-golden` gate catches these
explicitly). Re-baseline the golden by running the cross build
locally with toolchains installed and inspecting the diff.
