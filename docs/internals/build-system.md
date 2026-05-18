# Build system

## Overview

The urbi-embedded build is a single top-level `Makefile` that produces
`build/<target>/liburbi.a` from `src/` plus optional auxiliaries
(`build/<target>/urbi` REPL binary, fuzzer harnesses, stress drivers,
test runners). Cross-compilation is selected via `make cross-arm` or
`make cross-riscv`, which invoke the same Makefile with a per-target
`TARGET=<dir>` and a swapped toolchain — every target gets its own
`build/$(TARGET)/` tree, so concurrent builds never race.

## Cross-compile toolchain prerequisites

The host-side `make test` does not require any cross toolchain. Cross-compile
targets (`make cross-arm`, `make cross-riscv`, `make cross-esp32s3-*`, `make cross-stm32f4*`)
require the corresponding toolchain on PATH.

### Ubuntu (24.04+) install commands

| Cross target | Toolchain | Package |
|---|---|---|
| `cross-arm` (Cortex-M7) | arm-none-eabi-gcc | `apt install gcc-arm-none-eabi` |
| `cross-stm32f4` (Cortex-M4F) | arm-none-eabi-gcc (same) | `apt install gcc-arm-none-eabi` |
| `cross-riscv` (rv32imc) | riscv64-unknown-elf-gcc | `apt install gcc-riscv64-unknown-elf` (or `gcc-riscv-none-elf`) |
| `cross-esp32s3-*` (Xtensa LX7) | xtensa-esp-elf-gcc (bundled with ESP-IDF) | source `$IDF_PATH/export.sh` |

### ESP-IDF environment

ESP32 cross-compile targets and the ESP-IDF managed-component build require
ESP-IDF v6.0.1 sourced into the shell:

```sh
. /opt/esp/idf/export.sh    # CI container layout (espressif/idf:v6.0.1)
. ~/Tools/esp-idf/export.sh # local-clone layout (alternative path)
```

This puts `xtensa-esp-elf-gcc`, `xtensa-esp-elf-ar`, `xtensa-esp-elf-nm`, and
`idf.py` on PATH. The Makefile's `cross-esp32s3-*` targets assume the env is
already sourced; they do not source it themselves.

### Fresh-clone verification

The cross-compile targets are designed to work from a fresh clone with no
prior build state. To verify after toolchain changes:

```sh
git clone <repo> urbi-embedded-test
cd urbi-embedded-test
# Source ESP-IDF env (if testing cross-esp32s3-*)
. ~/Tools/esp-idf/export.sh
make cross-arm
make cross-riscv
make cross-stm32f4
make cross-esp32s3-bytecode-only
make cross-esp32s3-full
```

All five should succeed without intermediate `make` runs. CI exercises this
via fresh containers per job.

## Stdlib bake (M6 Wave 2)

The standard library ships as a hybrid:

- C-native methods compiled into `liburbi.a` directly (`src/stdlib/*.c`,
  e.g. `object_root.c`, `atom_protos.c`).
- Pre-compiled urbiscript modules baked at build time as a single
  `.rodata` byte blob (`urbi_stdlib_bytecode[]`) and loaded at
  `urbi_vm_init` via `urbi_module_load`.

Per master spec §5.1.

### Tool

`tools/urbi-compile-stdlib` is a build-time C program that links
against the host `liburbi.a` and walks `src/stdlib/STDLIB_ORDER.txt`.
For each newline-separated `.u` filename, it compiles the source via
the public Urbi compile API and concatenates the resulting v1.5
wire-format buffers into a single blob.

Phase-3 baseline: walks the order file but does not actually compile —
empty `STDLIB_ORDER.txt` produces a 0-length blob. Phase 10 fills in
the `urbi_compile_source` loop once the public compile API and the
`.u` files are in place.

The blob is emitted as `src/stdlib/urbi_stdlib_bytecode.gen.c`:

```c
const unsigned char urbi_stdlib_bytecode[N] = { 0xXX, 0xYY, ... };
const size_t urbi_stdlib_bytecode_len = N;
```

The `.gen.c` is a TRACKED source file (committed to the repo, not
generated under `build/`) so the first build of `liburbi.a` does not
require the bake tool — closing the chicken-and-egg between the tool
and the library it links against.

### Two-pass build

1. `liburbi.a` builds with the placeholder `.gen.c` (the committed
   0-length blob).
2. `tools/urbi-compile-stdlib` links against that intermediate
   `liburbi.a`.
3. Subsequent builds regenerate `.gen.c` whenever
   `src/stdlib/STDLIB_ORDER.txt` or any `src/stdlib/*.u` changes,
   causing `liburbi.a` to re-link with the populated blob.

`Makefile` rules:

```makefile
tools/urbi-compile-stdlib: tools/urbi-compile-stdlib.c \
                           | build/host/liburbi.a
        $(CC) -std=c99 -Wall -Wextra -Wpedantic -Os \
            -Iinclude -o $@ $< build/host/liburbi.a

src/stdlib/urbi_stdlib_bytecode.gen.c: tools/urbi-compile-stdlib \
                                        src/stdlib/STDLIB_ORDER.txt \
                                        $(wildcard src/stdlib/*.u)
        ./tools/urbi-compile-stdlib \
            src/stdlib/STDLIB_ORDER.txt \
            src/stdlib \
            $@
```

A cycle exists in the dep graph:

```text
liburbi.a → .gen.o → .gen.c → bake-tool → liburbi.a
```

GNU make detects this and silently drops one edge with a one-line
`Circular ... dependency dropped` warning. This is intentional and
correctness-safe: `.gen.c` is a tracked source so the first build of
`liburbi.a` does not need the bake tool, and subsequent rebakes only
happen when `STDLIB_ORDER.txt` or a `.u` changes (then `liburbi.a`
re-links from the regenerated `.gen.o`). The order-only edge
(`| build/host/liburbi.a`) on the bake-tool rule communicates intent —
the tool needs `liburbi.a` to LINK against, but does not need to
relink whenever `liburbi.a`'s contents change.

### Determinism

The bake tool MUST be deterministic — same input `.u` files produce
byte-identical `.gen.c`. Asserted by `tests/scripts/bake_smoke.sh`
(3-run byte-identity check) wired into `make releasetest` as
`test-bake-smoke`. Any non-determinism here would cause spurious
wire-format-hash churn at every build, which would in turn invalidate
the wire-format-hash CI gate (`tests/golden/*-wire-format-hashes.txt`).

### Boot

Phase 4 wires `urbi_module_load(stdlib_blob, len)` into the
`urbi_vm_init` boot path after the C-native protos register. Single
ordered module load; no parser/emit involvement at boot.

### Cross-arch builds

`tools/urbi-compile-stdlib` is a HOST-ONLY tool — it is compiled
against the host `liburbi.a` and run on the host. Cross-arch builds
(`make cross-arm`, `make cross-riscv`) consume the resulting
`urbi_stdlib_bytecode.gen.c` source file and compile it for the
target like any other `src/stdlib/*.c`. There is no chicken-and-egg:
the bake tool runs once on the host, its output ships as portable
C source.

The Makefile rule for `tools/urbi-compile-stdlib` hard-pins the host
toolchain (`-std=c99 -Wall -Wextra -Wpedantic -Os`) instead of
inheriting the cross compiler's `CFLAGS`. The same `make cross-arm`
invocation that builds `liburbi.a` for Cortex-M7 still produces a
host-architecture bake-tool binary that links against the host
`liburbi.a` (which it produced earlier in the same invocation, or
which already exists from a prior `make`). The cross-target
`build/<target>/liburbi.a` is built without ever invoking the bake
tool — the tracked `.gen.c` is sufficient, and the cross compiler
just compiles it for the target.

Embedded targets that strip the parser/emitter (M7
`URBI_BYTECODE_ONLY`) still load the same blob via
`urbi_module_load`; the parser-free boot path is verified by
`tests/scripts/build-bytecode-only.sh` (Phase 13).

### Force-regenerate

`make bake-clean` re-runs the bake tool against the current
`STDLIB_ORDER.txt` and `.u` files, overwriting `.gen.c`. Used for
debugging when the committed `.gen.c` drifts from what the current
sources would produce (e.g. a `.u` was edited but `make` did not
notice because the timestamp regressed). Routine builds do not need
this — the dep graph picks up `.u` changes automatically.
