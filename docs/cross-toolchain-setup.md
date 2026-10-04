# Cross-toolchain setup

The cross presets (see
[internals/build-system.md](internals/build-system.md#cross-presets))
need two bare-metal toolchains on PATH. A preset whose compiler is
missing fails the build with the compiler's name; nothing is skipped.

## The toolchains

| Toolchain | Version | Presets | xpack package |
|---|---|---|---|
| `arm-none-eabi-gcc` | 14.2.1-1.1 | `arm-cortex-m0plus`, `arm-cortex-m4f`, `arm-cortex-m7` | [`xpack-dev-tools/arm-none-eabi-gcc-xpack`](https://github.com/xpack-dev-tools/arm-none-eabi-gcc-xpack) |
| `riscv-none-elf-gcc` | 15.2.0-1 | `riscv32` | [`xpack-dev-tools/riscv-none-elf-gcc-xpack`](https://github.com/xpack-dev-tools/riscv-none-elf-gcc-xpack) |

GitHub Actions CI installs the same two xpack releases, in the `cross`
matrix and in the `releasetest` job, so the dev box and CI see the same
compiler and the same undefined-symbol sets.

## Why xpack

Distro packages ship the compiler without a matching bare-metal sysroot,
and their versions drift from what CI uses. xpack distributions bundle
the compiler with a matching newlib build and the libgcc helpers that
freestanding builds pull in.

## Install (Linux x86_64)

```sh
mkdir -p $HOME/.local/xpack

curl -L -o /tmp/arm.tar.gz \
    https://github.com/xpack-dev-tools/arm-none-eabi-gcc-xpack/releases/download/v14.2.1-1.1/xpack-arm-none-eabi-gcc-14.2.1-1.1-linux-x64.tar.gz
tar -xzf /tmp/arm.tar.gz -C $HOME/.local/xpack
export PATH=$HOME/.local/xpack/xpack-arm-none-eabi-gcc-14.2.1-1.1/bin:$PATH

curl -L -o /tmp/riscv.tar.gz \
    https://github.com/xpack-dev-tools/riscv-none-elf-gcc-xpack/releases/download/v15.2.0-1/xpack-riscv-none-elf-gcc-15.2.0-1-linux-x64.tar.gz
tar -xzf /tmp/riscv.tar.gz -C $HOME/.local/xpack
export PATH=$HOME/.local/xpack/xpack-riscv-none-elf-gcc-15.2.0-1/bin:$PATH
```

Persist the `PATH` additions in your shell profile. Then verify:

```sh
cd urbi-embedded
arm-none-eabi-gcc --version
riscv-none-elf-gcc --version
make cross-all           # eight archives, four freestanding gates
```

## When toolchains drift

A cross-only CI failure that does not reproduce locally usually means
the two machines run different compilers. Check the versions above
first; a newer libgcc can pull in helper symbols the older one did not.
